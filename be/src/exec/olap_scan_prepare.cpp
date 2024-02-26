// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/olap_scan_prepare.h"

#include <variant>

#include "column/type_traits.h"
#include "exprs/dictmapping_expr.h"
#include "exprs/expr_context.h"
#include "exprs/in_const_predicate.hpp"
#include "gutil/map_util.h"
#include "runtime/descriptors.h"
#include "storage/chunk_predicate.h"
#include "storage/column_predicate.h"
#include "storage/olap_runtime_range_pruner.h"
#include "storage/olap_runtime_range_pruner.hpp"
#include "storage/predicate_parser.h"
#include "types/date_value.hpp"
#include "types/logical_type.h"
#include "types/logical_type_infra.h"

namespace starrocks {

// ------------------------------------------------------------------------------------
// Util methods.
// ------------------------------------------------------------------------------------

static bool ignore_cast(const SlotDescriptor& slot, const Expr& expr) {
    if (slot.type().is_date_type() && expr.type().is_date_type()) {
        return true;
    }
    return slot.type().is_string_type() && expr.type().is_string_type();
}

static Expr* get_root_expr(ExprContext* ctx) {
    if (dynamic_cast<DictMappingExpr*>(ctx->root())) {
        return ctx->root()->get_child(1);
    }
    return ctx->root();
}

template <typename ValueType>
static bool check_decimal_overflow(int precision, const ValueType& value) {
    if constexpr (is_decimal<ValueType>) {
        return -get_scale_factor<ValueType>(precision) < value && value < get_scale_factor<ValueType>(precision);
    } else {
        return false;
    }
}

template <typename ValueType>
static bool get_predicate_value(ObjectPool* obj_pool, const SlotDescriptor& slot, const Expr* expr, ExprContext* ctx,
                                ValueType* value, SQLFilterOp* op, Status* status) {
    if (expr->get_num_children() != 2) {
        return false;
    }

    Expr* l = expr->get_child(0);
    Expr* r = expr->get_child(1);

    // 1. ensure |l| points to a slot ref and |r| points to a const expression.
    bool reverse_op = false;
    if (!r->is_constant()) {
        reverse_op = true;
        std::swap(l, r);
    }

    // TODO(zhuming): DATE column may be casted to double.
    if (l->type().type != slot.type().type && !ignore_cast(slot, *l)) {
        return false;
    }

    // when query on a `DATE` column with predicate, both the `DATE`
    // column and the operand of predicate will be casted to timestamp.
    if constexpr (std::is_same_v<ValueType, DateValue>) {
        if (l->op() == TExprOpcode::CAST) {
            l = l->get_child(0);
        }
    }

    if (!l->is_slotref() || !r->is_constant()) {
        return false;
    }

    std::vector<SlotId> slot_ids;
    if (l->get_slot_ids(&slot_ids) != 1 || slot_ids[0] != slot.id()) {
        return false;
    }

    // 3. extract the const value from |r|.
    ColumnPtr column_ptr = EVALUATE_NULL_IF_ERROR(ctx, r, nullptr);
    if (column_ptr == nullptr) {
        return false;
    }

    DCHECK_EQ(1u, column_ptr->size());
    if (column_ptr->only_null() || column_ptr->is_null(0)) {
        return false;
    }

    // check column type, as not all exprs return a const column.
    ColumnPtr data = column_ptr;
    if (column_ptr->is_nullable()) {
        data = down_cast<NullableColumn*>(column_ptr.get())->data_column();
    } else if (column_ptr->is_constant()) {
        data = down_cast<ConstColumn*>(column_ptr.get())->data_column();
    } else { // defensive check.
        DCHECK(false) << "unreachable path: unknown column type of expr evaluate result";
        return false;
    }

    if (expr->op() == TExprOpcode::EQ || expr->op() == TExprOpcode::NE) {
        *op = to_olap_filter_type(expr->op(), false);
    } else {
        *op = to_olap_filter_type(expr->op(), reverse_op);
    }

    if constexpr (std::is_same_v<ValueType, DateValue>) {
        if (data->is_timestamp()) {
            TimestampValue ts = down_cast<TimestampColumn*>(data.get())->get(0).get_timestamp();
            *value = implicit_cast<DateValue>(ts);
            if (implicit_cast<TimestampValue>(*value) != ts) {
                // |ts| has nonzero time, rewrite predicate.
                switch (*op) {
                case FILTER_LARGER_OR_EQUAL:
                    // rewrite (c >= '2020-01-01 01:00:00') to (c > '2020-01-01').
                    *op = FILTER_LARGER;
                    break;
                case FILTER_LESS:
                    // rewrite (c < '2020-01-01 01:00:00') to (c <= '2020-01-01').
                    *op = FILTER_LESS_OR_EQUAL;
                    break;
                case FILTER_LARGER:
                    [[fallthrough]];
                case FILTER_LESS_OR_EQUAL:
                    // Just ignore the time value.
                    break;
                case FILTER_IN:
                    *status = Status::EndOfFile("predicate for date always false");
                    return false;
                case FILTER_NOT_IN:
                    // TODO(zhuming): Should be rewrote to `NOT NULL`.
                    return false;
                }
            }
        } else {
            DCHECK(data->is_date());
            *value = down_cast<DateColumn*>(data.get())->get(0).get_date();
        }
    } else if constexpr (std::is_same_v<ValueType, Slice>) {
        // |column_ptr| will be released after this method return, have to ensure that
        // the corresponding external storage will not be deallocated while the slice
        // still been used.
        const auto* slice = reinterpret_cast<const Slice*>(data->raw_data());
        std::string* str = obj_pool->add(new std::string(slice->data, slice->size));
        *value = *str;
    } else {
        *value = *reinterpret_cast<const ValueType*>(data->raw_data());
        if (r->type().is_decimalv3_type()) {
            return check_decimal_overflow<ValueType>(r->type().precision, *value);
        }
    }
    return true;
}

static std::vector<ExprContextContainer> build_expr_context_containers(const std::vector<ExprContext*>& expr_contexts) {
    std::vector<ExprContextContainer> containers;
    for (auto* expr_ctx : expr_contexts) {
        containers.emplace_back(expr_ctx);
    }
    return containers;
}

static std::vector<RawExprContainer> build_raw_expr_containers(const std::vector<Expr*>& exprs) {
    std::vector<RawExprContainer> containers;
    for (auto* expr : exprs) {
        containers.emplace_back(expr);
    }
    return containers;
}

// ------------------------------------------------------------------------------------
// ChunkPredicateBuilder
// ------------------------------------------------------------------------------------

RawExprContainer::RawExprContainer(Expr* root_expr) : root_expr(root_expr) {}
Expr* RawExprContainer::root() const {
    return root_expr;
}
StatusOr<ExprContext*> RawExprContainer::expr_context(ObjectPool* obj_pool, RuntimeState* state)  const {
    ExprContext* expr_ctx = obj_pool->add(new ExprContext(root_expr));
    RETURN_IF_ERROR(expr_ctx->prepare(state));
    RETURN_IF_ERROR(expr_ctx->open(state));
    return expr_ctx;
}

ExprContextContainer::ExprContextContainer(ExprContext* expr_ctx) : expr_ctx(expr_ctx) {}
Expr* ExprContextContainer::root() const {
    return get_root_expr(expr_ctx);
}
StatusOr<ExprContext*> ExprContextContainer::expr_context(ObjectPool* obj_pool, RuntimeState* state) const {
    return expr_ctx;
}

template <ExprContainer E>
ChunkPredicateBuilder<E>::ChunkPredicateBuilder(const OlapScanConjunctsManagerOptions& opts, Type type,
                                                std::vector<E> exprs, bool allow_partial_normalized)
        : _type(type),
          _opts(opts),
          _exprs(std::move(exprs)),
          _allow_partial_normalized(allow_partial_normalized),
          _normalized_exprs(_exprs.size()) {}

template <ExprContainer E>
StatusOr<bool> ChunkPredicateBuilder<E>::parse_conjuncts() {
    if (_allow_partial_normalized) {
        RETURN_IF_ERROR(normalize_expressions());
        RETURN_IF_ERROR(build_olap_filters());
        RETURN_IF_ERROR(build_scan_keys(_opts.scan_keys_unlimited, _opts.max_scan_key_num));
    }

    if (_opts.enable_column_expr_predicate) {
        VLOG_FILE << "OlapScanConjunctsManager: enable_column_expr_predicate = true. push down column expr predicates";
        build_column_expr_predicates();
    }

    return _normalize_and_or_predicates();
}

template <ExprContainer E>
StatusOr<bool> ChunkPredicateBuilder<E>::_normalize_and_or_predicate(const Expr* root_expr) {
    if (TExprOpcode::COMPOUND_OR == root_expr->op()) {
        ChunkPredicateBuilder child_builder(_opts, Type::OR, build_raw_expr_containers(root_expr->children()), false);
        ASSIGN_OR_RETURN(const bool normalized, child_builder.parse_conjuncts());
        if (normalized) {
            _child_builders.emplace_back(std::move(child_builder));
        }
        return normalized;
    }

    if (TExprOpcode::COMPOUND_AND == root_expr->op()) {
        ChunkPredicateBuilder child_builder(_opts, Type::AND, build_raw_expr_containers(root_expr->children()), false);
        ASSIGN_OR_RETURN(const bool normalized, child_builder.parse_conjuncts());
        if (normalized) {
            _child_builders.emplace_back(std::move(child_builder));
        }
        return normalized;
    }

    return false;
}

template <ExprContainer E>
StatusOr<bool> ChunkPredicateBuilder<E>::_normalize_and_or_predicates() {
    const size_t num_preds = _exprs.size();
    for (size_t i = 0; i < num_preds; i++) {
        if (_normalized_exprs[i]) {
            continue;
        }

        ASSIGN_OR_RETURN(const bool normalized, _normalize_and_or_predicate(_exprs[i]));
        if (!normalized && !_allow_partial_normalized) {
            return false;
        }
        _normalized_exprs[i] = normalized;
    }

    return true;
}

template <ExprContainer E>
StatusOr<ChunkPredicatePtr> ChunkPredicateBuilder<E>::get_chunk_predicate(PredicateParser* parser) {
    auto chunk_pred = _type == Type::AND ? CompoundChunkPredicate::create_and() : CompoundChunkPredicate::create_or();

    ASSIGN_OR_RETURN(auto col_preds, _get_column_predicates(parser));
    for (auto& col_pred : col_preds) {
        chunk_pred->add_child_predicate(std::make_unique<ColumnChunkPredicate>(std::move(col_pred)));
    }

    for (auto& child_builder : _child_builders) {
        ASSIGN_OR_RETURN(auto child_pred, child_builder.get_chunk_predicate(parser));
        chunk_pred->add_child_predicate(std::move(child_pred));
    }

    return chunk_pred;
}

template <ExprContainer E>
template <LogicalType SlotType, typename RangeValueType>
void ChunkPredicateBuilder<E>::normalize_in_or_equal_predicate(const SlotDescriptor& slot,
                                                               ColumnValueRange<RangeValueType>* range) {
    Status status;

    for (size_t i = 0; i < _exprs.size(); i++) {
        if (_normalized_exprs[i]) {
            continue;
        }

        const Expr* root_expr = _exprs[i].root();

        // 1. Normalize in conjuncts like 'where col in (v1, v2, v3)'
        if (TExprOpcode::FILTER_IN == root_expr->op()) {
            const Expr* l = root_expr->get_child(0);

            if (!l->is_slotref() || (l->type().type != slot.type().type && !ignore_cast(slot, *l))) {
                continue;
            }
            std::vector<SlotId> slot_ids;
            if (1 == l->get_slot_ids(&slot_ids) && slot_ids[0] == slot.id()) {
                const auto* pred = down_cast<const VectorizedInConstPredicate<SlotType>*>(root_expr);
                // join in runtime filter  will handle by `_normalize_join_runtime_filter`
                if (pred->is_join_runtime_filter()) {
                    continue;
                }

                if (pred->is_not_in() || pred->null_in_set() ||
                    pred->hash_set().size() > config::max_pushdown_conditions_per_column) {
                    continue;
                }

                std::set<RangeValueType> values;
                for (const auto& value : pred->hash_set()) {
                    values.insert(value);
                }
                if (range->add_fixed_values(FILTER_IN, values).ok()) {
                    _normalized_exprs[i] = true;
                }
            }
        }

        // 2. Normalize eq conjuncts like 'where col = value'
        if (TExprNodeType::BINARY_PRED == root_expr->node_type() &&
            FILTER_IN == to_olap_filter_type(root_expr->op(), false)) {
            using ValueType = typename RunTimeTypeTraits<SlotType>::CppType;
            SQLFilterOp op;
            ValueType value;
            ExprContext* expr_context = _exprs[i].expr_context(_opts.obj_pool, _opts.runtime_state);
            bool ok = get_predicate_value(_opts.obj_pool, slot, root_expr, expr_context, &value, &op, &status);
            if (ok && range->add_fixed_values(FILTER_IN, std::set<RangeValueType>{value}).ok()) {
                _normalized_exprs[i] = true;
            }
        }
    }
}

// explicit specialization for DATE.
template <ExprContainer E>
template <>
void ChunkPredicateBuilder<E>::normalize_in_or_equal_predicate<starrocks::TYPE_DATE, DateValue>(
        const SlotDescriptor& slot, ColumnValueRange<DateValue>* range) {
    Status status;

    for (size_t i = 0; i < _exprs.size(); i++) {
        if (_normalized_exprs[i]) {
            continue;
        }

        const Expr* root_expr = _exprs[i].root();

        // 1. Normalize in conjuncts like 'where col in (v1, v2, v3)'
        if (TExprOpcode::FILTER_IN == root_expr->op()) {
            const Expr* l = root_expr->get_child(0);
            // TODO(zhuming): DATE column may be casted to double.
            if (l->type().type != starrocks::TYPE_DATE && l->type().type != starrocks::TYPE_DATETIME) {
                continue;
            }

            LogicalType pred_type = l->type().type;
            // ignore the cast on DATE.
            if (l->op() == TExprOpcode::CAST) {
                l = l->get_child(0);
            }
            if (!l->is_slotref()) {
                continue;
            }
            std::vector<SlotId> slot_ids;
            if (1 == l->get_slot_ids(&slot_ids) && slot_ids[0] == slot.id()) {
                std::set<DateValue> values;

                if (pred_type == starrocks::TYPE_DATETIME) {
                    const auto* pred =
                            down_cast<const VectorizedInConstPredicate<starrocks::TYPE_DATETIME>*>(root_expr);
                    // join in runtime filter  will handle by `_normalize_join_runtime_filter`
                    if (pred->is_join_runtime_filter()) {
                        continue;
                    }

                    if (pred->is_not_in() || pred->null_in_set() ||
                        pred->hash_set().size() > config::max_pushdown_conditions_per_column) {
                        continue;
                    }

                    for (const TimestampValue& ts : pred->hash_set()) {
                        auto date = implicit_cast<DateValue>(ts);
                        if (implicit_cast<TimestampValue>(date) == ts) {
                            values.insert(date);
                        }
                    }
                } else if (pred_type == starrocks::TYPE_DATE) {
                    const auto* pred = down_cast<const VectorizedInConstPredicate<starrocks::TYPE_DATE>*>(root_expr);
                    if (pred->is_not_in() || pred->null_in_set() ||
                        pred->hash_set().size() > config::max_pushdown_conditions_per_column) {
                        continue;
                    }
                    for (const DateValue& date : pred->hash_set()) {
                        values.insert(date);
                    }
                }
                if (values.empty()) {
                    status = Status::EndOfFile("const false predicate result");
                    continue;
                }
                if (range->add_fixed_values(FILTER_IN, values).ok()) {
                    _normalized_exprs[i] = true;
                }
            }
        }

        // 2. Normalize eq conjuncts like 'where col = value'
        if (TExprNodeType::BINARY_PRED == root_expr->node_type() &&
            FILTER_IN == to_olap_filter_type(root_expr->op(), false)) {
            SQLFilterOp op;
            DateValue value{0};
            ExprContext* expr_context = _exprs[i].expr_context(_opts.obj_pool, _opts.runtime_state);
            bool ok = get_predicate_value(_opts.obj_pool, slot, root_expr, expr_context, &value, &op, &status);
            if (ok && range->add_fixed_values(FILTER_IN, std::set<DateValue>{value}).ok()) {
                _normalized_exprs[i] = true;
            }
        }
    }
}

template <ExprContainer E>
template <LogicalType SlotType, typename RangeValueType>
void ChunkPredicateBuilder<E>::normalize_binary_predicate(const SlotDescriptor& slot,
                                                          ColumnValueRange<RangeValueType>* range) {
    Status status;
    DCHECK((SlotType == slot.type().type) || (SlotType == TYPE_VARCHAR && slot.type().type == TYPE_CHAR));

    for (size_t i = 0; i < _exprs.size(); i++) {
        if (_normalized_exprs[i]) {
            continue;
        }

        const Expr* root_expr = _exprs[i].root();

        if (TExprNodeType::BINARY_PRED != root_expr->node_type()) {
            continue;
        }

        using ValueType = typename RunTimeTypeTraits<SlotType>::CppType;

        SQLFilterOp op;
        ValueType value;
        ExprContext* expr_context = _exprs[i].expr_context(_opts.obj_pool, _opts.runtime_state);
        bool ok = get_predicate_value(_opts.obj_pool, slot, root_expr, expr_context, &value, &op, &status);
        if (ok && range->add_range(op, static_cast<RangeValueType>(value)).ok()) {
            _normalized_exprs[i] = true;
        }
    }
}

template <ExprContainer E>
template <LogicalType SlotType, typename RangeValueType>
void ChunkPredicateBuilder<E>::normalize_join_runtime_filter(const SlotDescriptor& slot,
                                                             ColumnValueRange<RangeValueType>* range) {
    // in runtime filter

    for (size_t i = 0; i < _exprs.size(); i++) {
        if (_normalized_exprs[i]) {
            continue;
        }

        const Expr* root_expr = _exprs[i].root();
        if (TExprOpcode::FILTER_IN == root_expr->op()) {
            const Expr* l = root_expr->get_child(0);
            if (!l->is_slotref() || (l->type().type != slot.type().type && !ignore_cast(slot, *l))) {
                continue;
            }
            std::vector<SlotId> slot_ids;
            if (1 == l->get_slot_ids(&slot_ids) && slot_ids[0] == slot.id()) {
                const auto* pred = down_cast<const VectorizedInConstPredicate<SlotType>*>(root_expr);

                if (!pred->is_join_runtime_filter()) {
                    continue;
                }

                // Ensure we don't compute this conjuncts again in olap scanner
                _normalized_exprs[i] = true;

                if (pred->is_not_in() || pred->null_in_set() ||
                    pred->hash_set().size() > config::max_pushdown_conditions_per_column) {
                    continue;
                }

                std::set<RangeValueType> values;
                for (const auto& value : pred->hash_set()) {
                    values.insert(value);
                }
                (void)range->add_fixed_values(FILTER_IN, values);
            }
        }
    }

    // bloom runtime filter
    for (const auto& it : _opts.runtime_filters->descriptors()) {
        const RuntimeFilterProbeDescriptor* desc = it.second;
        const JoinRuntimeFilter* rf = desc->runtime_filter();
        using RangeType = ColumnValueRange<RangeValueType>;
        using ValueType = typename RunTimeTypeTraits<SlotType>::CppType;
        SlotId slot_id;

        // probe expr is slot ref and slot id matches.
        if (!desc->is_probe_slot_ref(&slot_id) || slot_id != slot.id()) continue;

        // runtime filter existed and does not have null.
        if (rf == nullptr) {
            rt_ranger_params.add_unarrived_rf(desc, &slot);
            continue;
        }

        if (rf->has_null()) continue;

        // If this column doesn't have other filter, we use join runtime filter
        // to fast comput row range in storage engine
        if (range->is_init_state()) {
            range->set_index_filter_only(true);
        }

        // if we have multi-scanners
        // If a scanner has finished building a runtime filter,
        // the rest of the runtime filters will be normalized here

        auto& global_dicts = _opts.runtime_state->get_query_global_dict_map();
        if constexpr (SlotType == TYPE_VARCHAR) {
            if (auto iter = global_dicts.find(slot_id); iter != global_dicts.end()) {
                detail::RuntimeColumnPredicateBuilder::build_minmax_range<
                        RangeType, ValueType, LowCardDictType,
                        detail::RuntimeColumnPredicateBuilder::GlobalDictCodeDecoder>(*range, rf, &iter->second.first);
            } else {
                detail::RuntimeColumnPredicateBuilder::build_minmax_range<
                        RangeType, ValueType, SlotType, detail::RuntimeColumnPredicateBuilder::DummyDecoder>(*range, rf,
                                                                                                             nullptr);
            }
        } else {
            detail::RuntimeColumnPredicateBuilder::build_minmax_range<
                    RangeType, ValueType, SlotType, detail::RuntimeColumnPredicateBuilder::DummyDecoder>(*range, rf,
                                                                                                         nullptr);
        }
    }
}

template <ExprContainer E>
template <LogicalType SlotType, typename RangeValueType>
void ChunkPredicateBuilder<E>::normalize_not_in_or_not_equal_predicate(const SlotDescriptor& slot,
                                                                       ColumnValueRange<RangeValueType>* range) {
    Status status;
    DCHECK((SlotType == slot.type().type) || (SlotType == TYPE_VARCHAR && slot.type().type == TYPE_CHAR));

    using ValueType = typename RunTimeTypeTraits<SlotType>::CppType;
    // handle not equal.
    for (size_t i = 0; i < _exprs.size(); i++) {
        if (_normalized_exprs[i]) {
            continue;
        }
        const Expr* root_expr = _exprs[i].root();
        // handle not equal
        if (root_expr->node_type() == TExprNodeType::BINARY_PRED && root_expr->op() == TExprOpcode::NE) {
            SQLFilterOp op;
            ValueType value;
            ExprContext* expr_context = _exprs[i].expr_context(_opts.obj_pool, _opts.runtime_state);
            bool ok = get_predicate_value(_opts.obj_pool, slot, root_expr, expr_context, &value, &op, &status);
            if (ok && range->add_fixed_values(FILTER_NOT_IN, std::set<RangeValueType>{value}).ok()) {
                _normalized_exprs[i] = true;
            }
        }

        // handle not in
        if (root_expr->node_type() == TExprNodeType::IN_PRED && root_expr->op() == TExprOpcode::FILTER_NOT_IN) {
            const Expr* l = root_expr->get_child(0);
            if (!l->is_slotref() || (l->type().type != slot.type().type && !ignore_cast(slot, *l))) {
                continue;
            }
            std::vector<SlotId> slot_ids;

            if (1 == l->get_slot_ids(&slot_ids) && slot_ids[0] == slot.id()) {
                const auto* pred = down_cast<const VectorizedInConstPredicate<SlotType>*>(root_expr);
                // RTF won't generate not in predicate
                DCHECK(!pred->is_join_runtime_filter());

                if (!pred->is_not_in() || pred->null_in_set() ||
                    pred->hash_set().size() > config::max_pushdown_conditions_per_column) {
                    continue;
                }

                std::set<RangeValueType> values;
                for (const auto& value : pred->hash_set()) {
                    values.insert(value);
                }
                if (range->add_fixed_values(FILTER_NOT_IN, values).ok()) {
                    _normalized_exprs[i] = true;
                }
            }
        }
    }
}

template <ExprContainer E>
void ChunkPredicateBuilder<E>::normalize_is_null_predicate(const SlotDescriptor& slot) {
    for (size_t i = 0; i < _exprs.size(); i++) {
        if (_normalized_exprs[i]) {
            continue;
        }
        const Expr* root_expr = _exprs[i].root();
        if (TExprNodeType::FUNCTION_CALL == root_expr->node_type()) {
            std::string is_null_str;
            if (root_expr->is_null_scalar_function(is_null_str)) {
                Expr* e = root_expr->get_child(0);
                if (!e->is_slotref()) {
                    continue;
                }
                std::vector<SlotId> slot_ids;
                if (1 != e->get_slot_ids(&slot_ids) || slot_ids[0] != slot.id()) {
                    continue;
                }
                TCondition is_null;
                is_null.column_name = slot.col_name();
                is_null.condition_op = "is";
                is_null.condition_values.push_back(is_null_str);
                is_null_vector.push_back(is_null);
                _normalized_exprs[i] = true;
            }
        }
    }
}

template <ExprContainer E>
template <LogicalType SlotType, typename RangeValueType>
void ChunkPredicateBuilder<E>::normalize_predicate(const SlotDescriptor& slot,
                                                   ColumnValueRange<RangeValueType>* range) {
    normalize_in_or_equal_predicate<SlotType, RangeValueType>(slot, range);
    normalize_binary_predicate<SlotType, RangeValueType>(slot, range);
    normalize_not_in_or_not_equal_predicate<SlotType, RangeValueType>(slot, range);
    normalize_is_null_predicate(slot);
    // Must handle join runtime filter last
    normalize_join_runtime_filter<SlotType, RangeValueType>(slot, range);
}

struct ColumnRangeBuilder {
    template <LogicalType ltype>
    std::nullptr_t operator()(ChunkPredicateBuilder<ExprContextContainer>* parent, const SlotDescriptor* slot,
                              std::map<std::string, ColumnValueRangeType>* column_value_ranges) {
        if constexpr (ltype == TYPE_TIME || ltype == TYPE_NULL || ltype == TYPE_JSON || lt_is_float<ltype> ||
                      lt_is_binary<ltype>) {
            return nullptr;
        }

        // Treat tinyint and boolean as int
        constexpr LogicalType limit_type = ltype == TYPE_TINYINT || ltype == TYPE_BOOLEAN ? TYPE_INT : ltype;
        // Map TYPE_CHAR to TYPE_VARCHAR
        constexpr LogicalType mapping_type = ltype == TYPE_CHAR ? TYPE_VARCHAR : ltype;
        using value_type = typename RunTimeTypeLimits<limit_type>::value_type;
        using RangeType = ColumnValueRange<value_type>;

        const std::string& col_name = slot->col_name();
        RangeType full_range(col_name, ltype, RunTimeTypeLimits<ltype>::min_value(),
                             RunTimeTypeLimits<ltype>::max_value());
        if constexpr (lt_is_decimal<limit_type>) {
            full_range.set_precision(slot->type().precision);
            full_range.set_scale(slot->type().scale);
        }
        ColumnValueRangeType& v = LookupOrInsert(column_value_ranges, col_name, full_range);
        auto& range = std::get<ColumnValueRange<value_type>>(v);
        if constexpr (lt_is_decimal<limit_type>) {
            range.set_precision(slot->type().precision);
            range.set_scale(slot->type().scale);
        }
        parent->normalize_predicate<mapping_type, value_type>(*slot, &range);
        return nullptr;
    }
};

template <ExprContainer E>
Status ChunkPredicateBuilder<E>::normalize_expressions() {
    // Note: _normalized_exprs size must be equal to _conjunct_ctxs size,
    // but HashJoinNode will push down predicate to OlapScanNode's _conjunct_ctxs,
    // So _conjunct_ctxs will change after OlapScanNode prepare,
    // So we couldn't resize _normalized_exprs when OlapScanNode init or prepare

    // TODO(zhuming): if any of the normalized column range is empty, we can know that
    // no row will be selected anymore and can return EOF directly.
    for (auto& slot : _opts.tuple_desc->decoded_slots()) {
        type_dispatch_predicate<std::nullptr_t>(slot->type().type, false, ColumnRangeBuilder(), this, slot,
                                                &column_value_ranges);
    }
    return Status::OK();
}

template <ExprContainer E>
Status ChunkPredicateBuilder<E>::build_olap_filters() {
    olap_filters.clear();

    for (auto iter : column_value_ranges) {
        std::vector<TCondition> filters;
        std::visit([&](auto&& range) { range.to_olap_filter(filters); }, iter.second);
        bool empty_range = std::visit([](auto&& range) { return range.is_empty_value_range(); }, iter.second);
        if (empty_range) {
            return Status::EndOfFile("EOF, Filter by always false condition");
        }

        for (auto& filter : filters) {
            olap_filters.emplace_back(std::move(filter));
        }
    }

    return Status::OK();
}

// Try to convert the ranges predicates applied on key columns to in predicates to increase
// the scan concurrency, i.e, the number of OlapScanners.
// For example, if the original query is `select * from t where c0 between 1 and 3 and c1 between 12 and 13`,
// where c0 is the first key column and c1 is the second key column, this routine will convert the predicates
// to `where c0 in (1,2,3) and c1 in (12,13)`, which is equivalent to the following disjunctive predicates:
// `where (c0=1 and c1=12)
//     OR (c0=1 and c1=13)
//     OR (c0=2 and c1=12)
//     OR (c0=2 and c1=13)
//     OR (c0=3 and c1=12)
//     OR (c0=3 and c1=13)
// `.
// By doing so, we can then create six instances of OlapScanner and assign each one with one of the disjunctive
// predicates and run the OlapScanners concurrently.

class ExtendScanKeyVisitor : public boost::static_visitor<Status> {
public:
    ExtendScanKeyVisitor(OlapScanKeys* scan_keys, int32_t max_scan_key_num)
            : _scan_keys(scan_keys), _max_scan_key_num(max_scan_key_num) {}

    template <class T>
    Status operator()(T& v) {
        return _scan_keys->extend_scan_key(v, _max_scan_key_num);
    }

private:
    OlapScanKeys* _scan_keys;
    int32_t _max_scan_key_num;
};

template <ExprContainer E>
Status ChunkPredicateBuilder<E>::build_scan_keys(bool unlimited, int32_t max_scan_key_num) {
    int conditional_key_columns = 0;
    scan_keys.set_is_convertible(unlimited);
    const std::vector<std::string>& ref_key_column_names = *_opts.key_column_names;

    for (const auto& key_column_name : ref_key_column_names) {
        if (column_value_ranges.count(key_column_name) == 0) {
            break;
        }
        conditional_key_columns++;
    }
    if (config::enable_short_key_for_one_column_filter || conditional_key_columns > 1) {
        for (int i = 0; i < conditional_key_columns && !scan_keys.has_range_value(); ++i) {
            ExtendScanKeyVisitor visitor(&scan_keys, max_scan_key_num);
            if (!std::visit(visitor, column_value_ranges[ref_key_column_names[i]]).ok()) {
                break;
            }
        }
    }
    return Status::OK();
}

template <ExprContainer E>
StatusOr<std::vector<ColumnPredicatePtr>> ChunkPredicateBuilder<E>::_get_column_predicates(PredicateParser* parser) {
    std::vector<ColumnPredicatePtr> preds;
    for (auto& f : olap_filters) {
        std::unique_ptr<ColumnPredicate> p(parser->parse_thrift_cond(f));
        RETURN_IF(!p, Status::RuntimeError("invalid filter"));
        p->set_index_filter_only(f.is_index_filter_only);
        preds.emplace_back(std::move(p));
    }
    for (auto& f : is_null_vector) {
        std::unique_ptr<ColumnPredicate> p(parser->parse_thrift_cond(f));
        RETURN_IF(!p, Status::RuntimeError("invalid filter"));
        preds.emplace_back(std::move(p));
    }

    const auto& slots = _opts.tuple_desc->decoded_slots();
    for (auto& iter : slot_index_to_expr_ctxs) {
        int slot_index = iter.first;
        auto& expr_ctxs = iter.second;
        const SlotDescriptor* slot_desc = slots[slot_index];
        for (ExprContext* ctx : expr_ctxs) {
            ASSIGN_OR_RETURN(auto tmp, parser->parse_expr_ctx(*slot_desc, _opts.runtime_state, ctx));
            std::unique_ptr<ColumnPredicate> p(std::move(tmp));
            if (p == nullptr) {
                std::stringstream ss;
                ss << "invalid filter, slot=" << slot_desc->debug_string();
                if (ctx != nullptr) {
                    ss << ", expr=" << ctx->root()->debug_string();
                }
                LOG(WARNING) << ss.str();
                return Status::RuntimeError("invalid filter");
            } else {
                preds.emplace_back(std::move(p));
            }
        }
    }
    return preds;
}

template <ExprContainer E>
Status ChunkPredicateBuilder<E>::get_key_ranges(std::vector<std::unique_ptr<OlapScanRange>>* key_ranges) {
    RETURN_IF_ERROR(scan_keys.get_key_range(key_ranges));
    if (key_ranges->empty()) {
        key_ranges->emplace_back(std::make_unique<OlapScanRange>());
    }
    return Status::OK();
}

template <ExprContainer E>
bool ChunkPredicateBuilder<E>::is_pred_normalized(size_t index) const {
    return index < _normalized_exprs.size() && _normalized_exprs[index];
}

template <ExprContainer E>
void ChunkPredicateBuilder<E>::build_column_expr_predicates() {
    std::map<SlotId, int> slot_id_to_index;
    const auto& slots = _opts.tuple_desc->decoded_slots();
    for (int i = 0; i < slots.size(); i++) {
        const SlotDescriptor* slot_desc = slots[i];
        SlotId slot_id = slot_desc->id();
        slot_id_to_index.insert(std::make_pair(slot_id, i));
    }

    for (size_t i = 0; i < _exprs.size(); i++) {
        if (_normalized_exprs[i]) continue;

        const Expr* root_expr = _exprs[i].root();
        std::vector<SlotId> slot_ids;
        root_expr->get_slot_ids(&slot_ids);
        if (slot_ids.size() != 1) continue;
        int index = -1;
        {
            auto iter = slot_id_to_index.find(slot_ids[0]);
            if (iter == slot_id_to_index.end()) continue;
            index = iter->second;
        }
        // note(yan): we only handles scalar type now to avoid complex type mismatch.
        // otherwise we don't need this limitation.
        const SlotDescriptor* slot_desc = slots[index];
        LogicalType ltype = slot_desc->type().type;
        if (!support_column_expr_predicate(ltype)) {
            continue;
        }

        auto iter = slot_index_to_expr_ctxs.find(index);
        if (iter == slot_index_to_expr_ctxs.end()) {
            slot_index_to_expr_ctxs.insert(make_pair(index, std::vector<ExprContext*>{}));
            iter = slot_index_to_expr_ctxs.find(index);
        }
        ASSIGN_OR_RETURN(auto* expr_ctx, _exprs[i].expr_context(_opts.obj_pool, _opts.runtime_state));
        iter->second.emplace_back(expr_ctx);
        _normalized_exprs[i] = true;
    }
}

// ------------------------------------------------------------------------------------
// OlapScanConjunctsManager
// ------------------------------------------------------------------------------------

OlapScanConjunctsManager::OlapScanConjunctsManager(OlapScanConjunctsManagerOptions&& opts)
        : _opts(opts),
          _root_builder(_opts, ChunkPredicateBuilder<ExprContextContainer>::Type::AND,
                        build_expr_context_containers(*_opts.conjunct_ctxs_ptr), true) {}

Status OlapScanConjunctsManager::parse_conjuncts() {
    return _root_builder.parse_conjuncts().status();
}

Status OlapScanConjunctsManager::eval_const_conjuncts(const std::vector<ExprContext*>& conjunct_ctxs, Status* status) {
    *status = Status::OK();
    for (const auto& ctx : conjunct_ctxs) {
        // if conjunct is constant, compute direct and set eos = true
        if (ctx->root()->is_constant()) {
            ASSIGN_OR_RETURN(ColumnPtr value, ctx->root()->evaluate_const(ctx));

            if (value == nullptr || value->only_null() || value->is_null(0)) {
                *status = Status::EndOfFile("conjuncts evaluated to null");
                break;
            } else if (value->is_constant() && !ColumnHelper::get_const_value<TYPE_BOOLEAN>(value)) {
                *status = Status::EndOfFile("conjuncts evaluated to false");
                break;
            }
        }
    }
    return Status::OK();
}

StatusOr<ChunkPredicatePtr> OlapScanConjunctsManager::get_chunk_predicate(PredicateParser* parser) {
    return _root_builder.get_chunk_predicate(parser);
}

Status OlapScanConjunctsManager::get_key_ranges(std::vector<std::unique_ptr<OlapScanRange>>* key_ranges) {
    return _root_builder.get_key_ranges(key_ranges);
}

void OlapScanConjunctsManager::get_not_push_down_conjuncts(std::vector<ExprContext*>* predicates) {
    // DCHECK_EQ(_opts.conjunct_ctxs_ptr->size(), _normalized_exprs.size());
    const size_t num_preds = _opts.conjunct_ctxs_ptr->size();
    for (size_t i = 0; i < num_preds; i++) {
        if (!_root_builder.is_pred_normalized(i)) {
            predicates->push_back(_opts.conjunct_ctxs_ptr->at(i));
        }
    }
}

const UnarrivedRuntimeFilterList& OlapScanConjunctsManager::unarrived_runtime_filters() {
    return _root_builder.unarrived_runtime_filters();
}

} // namespace starrocks
