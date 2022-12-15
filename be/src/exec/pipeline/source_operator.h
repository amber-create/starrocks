// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <utility>

#include "exec/pipeline/operator.h"
#include "exec/pipeline/scan/chunk_source.h"
#include "exec/workgroup/work_group_fwd.h"

namespace starrocks {

class PriorityThreadPool;

namespace pipeline {

class SourceOperator;
using SourceOperatorPtr = std::shared_ptr<SourceOperator>;

class SourceOperatorFactory : public OperatorFactory {
public:
    SourceOperatorFactory(int32_t id, const std::string& name, int32_t plan_node_id)
            : OperatorFactory(id, name, plan_node_id) {}
    bool is_source() const override { return true; }
    // with_morsels returning true means that the SourceOperator needs attach to MorselQueue, only
    // OlapScanOperator needs to do so.
    virtual bool with_morsels() const { return false; }
    // Set the DOP(degree of parallelism) of the SourceOperator, SourceOperator's DOP determine the Pipeline's DOP.
    virtual void set_degree_of_parallelism(size_t degree_of_parallelism) {
        this->_degree_of_parallelism = degree_of_parallelism;
    }
    virtual size_t degree_of_parallelism() const { return _degree_of_parallelism; }

    // When the pipeline of this source operator wants to insert a local shuffle for some complex operators,
    // such as hash join and aggregate, use this method to decide whether really need to insert a local shuffle.
    virtual bool could_local_shuffle() const { return _could_local_shuffle; }
    void set_could_local_shuffle(bool could_local_shuffle) { _could_local_shuffle = could_local_shuffle; }
    virtual TPartitionType::type partition_type() const { return TPartitionType::type::HASH_PARTITIONED; }

protected:
    size_t _degree_of_parallelism = 1;
    bool _could_local_shuffle = true;
};

class SourceOperator : public Operator {
public:
    SourceOperator(OperatorFactory* factory, int32_t id, const std::string& name, int32_t plan_node_id,
                   int32_t driver_sequence)
            : Operator(factory, id, name, plan_node_id, driver_sequence) {}
    ~SourceOperator() override = default;

    bool need_input() const override { return false; }

    Status push_chunk(RuntimeState* state, const vectorized::ChunkPtr& chunk) override {
        return Status::InternalError("Shouldn't push chunk to source operator");
    }

    virtual void add_morsel_queue(MorselQueue* morsel_queue) { _morsel_queue = morsel_queue; };

    const MorselQueue* morsel_queue() const { return _morsel_queue; }

    size_t degree_of_parallelism() const {
        auto* source_op_factory = down_cast<SourceOperatorFactory*>(_factory);
        return source_op_factory->degree_of_parallelism();
    }

protected:
    MorselQueue* _morsel_queue = nullptr;
};

} // namespace pipeline
} // namespace starrocks
