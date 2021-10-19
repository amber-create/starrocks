// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exprs/vectorized/math_functions.h"

#include <math.h>

#include "column/column_helper.h"
#include "exprs/expr.h"
#include "util/time.h"

namespace starrocks {
namespace vectorized {

// ==== basic check rules =========
DEFINE_UNARY_FN_WITH_IMPL(NegativeCheck, value) {
    return value < 0;
}

DEFINE_UNARY_FN_WITH_IMPL(NonPositiveCheck, value) {
    return value <= 0;
}

DEFINE_UNARY_FN_WITH_IMPL(NanCheck, value) {
    return std::isnan(value);
}

DEFINE_UNARY_FN_WITH_IMPL(ZeroCheck, value) {
    return value == 0;
}

// ====== evaluation + check rules ========

#define DEFINE_MATH_UNARY_FN(NAME, TYPE, RESULT_TYPE)                                                          \
    ColumnPtr MathFunctions::NAME(FunctionContext* context, const starrocks::vectorized::Columns& columns) {   \
        using VectorizedUnaryFunction = VectorizedStrictUnaryFunction<NAME##Impl>;                             \
        if constexpr (pt_is_decimal<TYPE>) {                                                                   \
            const auto& type = context->get_return_type();                                                     \
            return VectorizedUnaryFunction::evaluate<TYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0), type.precision, \
                                                                        type.scale);                           \
        } else {                                                                                               \
            return VectorizedUnaryFunction::evaluate<TYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0));                \
        }                                                                                                      \
    }

#define DEFINE_MATH_UNARY_WITH_ZERO_CHECK_FN(NAME, TYPE, RESULT_TYPE)                                        \
    ColumnPtr MathFunctions::NAME(FunctionContext* context, const starrocks::vectorized::Columns& columns) { \
        using VectorizedUnaryFunction = VectorizedInputCheckUnaryFunction<NAME##Impl, ZeroCheck>;            \
        return VectorizedUnaryFunction::evaluate<TYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0));                  \
    }

#define DEFINE_MATH_UNARY_WITH_NEGATIVE_CHECK_FN(NAME, TYPE, RESULT_TYPE)                                    \
    ColumnPtr MathFunctions::NAME(FunctionContext* context, const starrocks::vectorized::Columns& columns) { \
        using VectorizedUnaryFunction = VectorizedInputCheckUnaryFunction<NAME##Impl, NegativeCheck>;        \
        return VectorizedUnaryFunction::evaluate<TYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0));                  \
    }

#define DEFINE_MATH_UNARY_WITH_NON_POSITIVE_CHECK_FN(NAME, TYPE, RESULT_TYPE)                                \
    ColumnPtr MathFunctions::NAME(FunctionContext* context, const starrocks::vectorized::Columns& columns) { \
        using VectorizedUnaryFunction = VectorizedInputCheckUnaryFunction<NAME##Impl, NonPositiveCheck>;     \
        return VectorizedUnaryFunction::evaluate<TYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0));                  \
    }

#define DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN(NAME, TYPE, RESULT_TYPE)                                  \
    ColumnPtr MathFunctions::NAME(FunctionContext* context, const starrocks::vectorized::Columns& columns) { \
        using VectorizedUnaryFunction = VectorizedOutputCheckUnaryFunction<NAME##Impl, NanCheck>;            \
        return VectorizedUnaryFunction::evaluate<TYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0));                  \
    }

#define DEFINE_MATH_BINARY_WITH_OUTPUT_NAN_CHECK_FN(NAME, LTYPE, RTYPE, RESULT_TYPE)                         \
    ColumnPtr MathFunctions::NAME(FunctionContext* context, const starrocks::vectorized::Columns& columns) { \
        using VectorizedBinaryFunction = VectorizedOuputCheckBinaryFunction<NAME##Impl, NanCheck>;           \
        return VectorizedBinaryFunction::evaluate<LTYPE, RTYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0),          \
                                                                             VECTORIZED_FN_ARGS(1));         \
    }

// ============ math function macro ==========

#define DEFINE_MATH_UNARY_FN_WITH_IMPL(NAME, TYPE, RESULT_TYPE, FN) \
    DEFINE_UNARY_FN(NAME##Impl, FN);                                \
    DEFINE_MATH_UNARY_FN(NAME, TYPE, RESULT_TYPE);

#define DEFINE_MATH_UNARY_FN_CAST_WITH_IMPL(NAME, TYPE, RESULT_TYPE, FN) \
    DEFINE_UNARY_FN_CAST(NAME##Impl, FN);                                \
    DEFINE_MATH_UNARY_FN(NAME, TYPE, RESULT_TYPE);

#define DEFINE_MATH_BINARY_FN(NAME, LTYPE, RTYPE, RESULT_TYPE)                                                         \
    ColumnPtr MathFunctions::NAME(FunctionContext* context, const starrocks::vectorized::Columns& columns) {           \
        return VectorizedStrictBinaryFunction<NAME##Impl>::evaluate<LTYPE, RTYPE, RESULT_TYPE>(VECTORIZED_FN_ARGS(0),  \
                                                                                               VECTORIZED_FN_ARGS(1)); \
    }

#define DEFINE_MATH_BINARY_FN_WITH_IMPL(NAME, LTYPE, RTYPE, RESULT_TYPE, FN) \
    DEFINE_BINARY_FUNCTION(NAME##Impl, FN);                                  \
    DEFINE_MATH_BINARY_FN(NAME, LTYPE, RTYPE, RESULT_TYPE);

#define DEFINE_MATH_UNARY_WITH_NEGATIVE_CHECK_FN_WITH_IMPL(NAME, TYPE, RESULT_TYPE, FN) \
    DEFINE_UNARY_FN(NAME##Impl, FN);                                                    \
    DEFINE_MATH_UNARY_WITH_NEGATIVE_CHECK_FN(NAME, TYPE, RESULT_TYPE);

#define DEFINE_MATH_UNARY_WITH_NON_POSITIVE_CHECK_FN_WITH_IMPL(NAME, TYPE, RESULT_TYPE, FN) \
    DEFINE_UNARY_FN(NAME##Impl, FN);                                                        \
    DEFINE_MATH_UNARY_WITH_NON_POSITIVE_CHECK_FN(NAME, TYPE, RESULT_TYPE);

#define DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(NAME, TYPE, RESULT_TYPE, FN) \
    DEFINE_UNARY_FN(NAME##Impl, FN);                                                      \
    DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN(NAME, TYPE, RESULT_TYPE);

#define DEFINE_MATH_BINARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(NAME, LTYPE, RTYPE, RESULT_TYPE, FN) \
    DEFINE_BINARY_FUNCTION(NAME##Impl, FN);                                                        \
    DEFINE_MATH_BINARY_WITH_OUTPUT_NAN_CHECK_FN(NAME, LTYPE, RTYPE, RESULT_TYPE);

// ============ math function impl ==========
ColumnPtr MathFunctions::pi(FunctionContext* context, const starrocks::vectorized::Columns& columns) {
    return ColumnHelper::create_const_column<TYPE_DOUBLE>(M_PI, 1);
}

ColumnPtr MathFunctions::e(FunctionContext* context, const starrocks::vectorized::Columns& columns) {
    return ColumnHelper::create_const_column<TYPE_DOUBLE>(M_E, 1);
}

// sign
DEFINE_UNARY_FN_WITH_IMPL(signImpl, v) {
    return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f);
}

DEFINE_MATH_UNARY_FN(sign, TYPE_DOUBLE, TYPE_FLOAT);

// round
DEFINE_UNARY_FN_WITH_IMPL(roundImpl, v) {
    return static_cast<int64_t>(v + ((v < 0) ? -0.5 : 0.5));
}

DEFINE_MATH_UNARY_FN(round, TYPE_DOUBLE, TYPE_BIGINT);

// log
DEFINE_BINARY_FUNCTION_WITH_IMPL(logProduceNullImpl, base, v) {
    return std::isnan(v) || base <= 0 || std::fabs(base - 1.0) < MathFunctions::EPSILON || v <= 0.0;
}

DEFINE_BINARY_FUNCTION_WITH_IMPL(logImpl, base, v) {
    return (double)(std::log(v) / std::log(base));
}

ColumnPtr MathFunctions::log(FunctionContext* context, const Columns& columns) {
    auto l = VECTORIZED_FN_ARGS(0);
    auto r = VECTORIZED_FN_ARGS(1);
    return VectorizedUnstrictBinaryFunction<logProduceNullImpl, logImpl>::evaluate<TYPE_DOUBLE>(l, r);
}

// log2
DEFINE_UNARY_FN_WITH_IMPL(log2Impl, v) {
    return (double)(std::log(v) / std::log(2.0));
}

DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN(log2, TYPE_DOUBLE, TYPE_DOUBLE);

// radians
DEFINE_UNARY_FN_WITH_IMPL(radiansImpl, v) {
    return (double)(v * M_PI / 180.0);
}

DEFINE_MATH_UNARY_FN(radians, TYPE_DOUBLE, TYPE_DOUBLE);

// degrees
DEFINE_UNARY_FN_WITH_IMPL(degreesImpl, v) {
    return (double)(v * 180.0 / M_PI);
}

DEFINE_MATH_UNARY_FN(degrees, TYPE_DOUBLE, TYPE_DOUBLE);

// bin
DEFINE_STRING_UNARY_FN_WITH_IMPL(binImpl, v) {
    uint64_t n = static_cast<uint64_t>(v);
    const size_t max_bits = sizeof(uint64_t) * 8;
    char result[max_bits];
    uint32_t index = max_bits;
    do {
        result[--index] = '0' + (n & 1);
    } while (n >>= 1);
    return std::string(result + index, max_bits - index);
}

ColumnPtr MathFunctions::bin(FunctionContext* context, const Columns& columns) {
    return VectorizedStringStrictUnaryFunction<binImpl>::evaluate<TYPE_BIGINT, TYPE_VARCHAR>(columns[0]);
}

// unary math
// float double abs
DEFINE_MATH_UNARY_FN_WITH_IMPL(abs_double, TYPE_DOUBLE, TYPE_DOUBLE, std::fabs);
DEFINE_MATH_UNARY_FN_WITH_IMPL(abs_float, TYPE_FLOAT, TYPE_FLOAT, std::fabs);

// integer abs
// std::abs(TYPE_MIN) is still TYPE_MIN, so integers except largeint need to cast to ResultType
// before std::abs.
DEFINE_MATH_UNARY_FN_WITH_IMPL(abs_largeint, TYPE_LARGEINT, TYPE_LARGEINT, std::abs);
DEFINE_MATH_UNARY_FN_CAST_WITH_IMPL(abs_bigint, TYPE_BIGINT, TYPE_LARGEINT, std::abs);
DEFINE_MATH_UNARY_FN_CAST_WITH_IMPL(abs_int, TYPE_INT, TYPE_BIGINT, std::abs);
DEFINE_MATH_UNARY_FN_CAST_WITH_IMPL(abs_smallint, TYPE_SMALLINT, TYPE_INT, std::abs);
DEFINE_MATH_UNARY_FN_CAST_WITH_IMPL(abs_tinyint, TYPE_TINYINT, TYPE_SMALLINT, std::abs);

// decimal abs
DEFINE_MATH_UNARY_FN_WITH_IMPL(abs_decimal32, TYPE_DECIMAL32, TYPE_DECIMAL32, std::abs);
DEFINE_MATH_UNARY_FN_WITH_IMPL(abs_decimal64, TYPE_DECIMAL64, TYPE_DECIMAL64, std::abs);
DEFINE_MATH_UNARY_FN_WITH_IMPL(abs_decimal128, TYPE_DECIMAL128, TYPE_DECIMAL128, std::abs);

// degrees
DEFINE_UNARY_FN_WITH_IMPL(abs_decimalv2valImpl, v) {
    DecimalV2Value value = v;
    value.to_abs_value();
    return value;
}

DEFINE_MATH_UNARY_FN(abs_decimalv2val, TYPE_DECIMALV2, TYPE_DECIMALV2);

DEFINE_UNARY_FN_WITH_IMPL(cotImpl, v) {
    return 1.0 / std::tan(v);
}

DEFINE_MATH_UNARY_WITH_ZERO_CHECK_FN(cot, TYPE_DOUBLE, TYPE_DOUBLE);

DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(sin, TYPE_DOUBLE, TYPE_DOUBLE, std::sin);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(asin, TYPE_DOUBLE, TYPE_DOUBLE, std::asin);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(cos, TYPE_DOUBLE, TYPE_DOUBLE, std::cos);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(acos, TYPE_DOUBLE, TYPE_DOUBLE, std::acos);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(tan, TYPE_DOUBLE, TYPE_DOUBLE, std::tan);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(atan, TYPE_DOUBLE, TYPE_DOUBLE, std::atan);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(ceil, TYPE_DOUBLE, TYPE_BIGINT, std::ceil);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(floor, TYPE_DOUBLE, TYPE_BIGINT, std::floor);
DEFINE_MATH_UNARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(exp, TYPE_DOUBLE, TYPE_DOUBLE, std::exp);

DEFINE_MATH_UNARY_WITH_NON_POSITIVE_CHECK_FN_WITH_IMPL(ln, TYPE_DOUBLE, TYPE_DOUBLE, std::log);
DEFINE_MATH_UNARY_WITH_NON_POSITIVE_CHECK_FN_WITH_IMPL(log10, TYPE_DOUBLE, TYPE_DOUBLE, std::log10);
DEFINE_MATH_UNARY_WITH_NEGATIVE_CHECK_FN_WITH_IMPL(sqrt, TYPE_DOUBLE, TYPE_DOUBLE, std::sqrt);

DEFINE_BINARY_FUNCTION_WITH_IMPL(truncateImpl, l, r) {
    return MathFunctions::double_round(l, r, false, true);
}

DEFINE_BINARY_FUNCTION_WITH_IMPL(round_up_toImpl, l, r) {
    return MathFunctions::double_round(l, r, false, false);
}

// binary math
DEFINE_MATH_BINARY_FN(truncate, TYPE_DOUBLE, TYPE_INT, TYPE_DOUBLE);
DEFINE_MATH_BINARY_FN(round_up_to, TYPE_DOUBLE, TYPE_INT, TYPE_DOUBLE);
DEFINE_MATH_BINARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(pow, TYPE_DOUBLE, TYPE_DOUBLE, TYPE_DOUBLE, std::pow);
DEFINE_MATH_BINARY_WITH_OUTPUT_NAN_CHECK_FN_WITH_IMPL(atan2, TYPE_DOUBLE, TYPE_DOUBLE, TYPE_DOUBLE, std::atan2);

#undef DEFINE_MATH_UNARY_FN
#undef DEFINE_MATH_UNARY_FN_WITH_IMPL
#undef DEFINE_MATH_BINARY_FN
#undef DEFINE_MATH_BINARY_FN_WITH_IMPL

const double log_10[] = {
        1e000, 1e001, 1e002, 1e003, 1e004, 1e005, 1e006, 1e007, 1e008, 1e009, 1e010, 1e011, 1e012, 1e013, 1e014, 1e015,
        1e016, 1e017, 1e018, 1e019, 1e020, 1e021, 1e022, 1e023, 1e024, 1e025, 1e026, 1e027, 1e028, 1e029, 1e030, 1e031,
        1e032, 1e033, 1e034, 1e035, 1e036, 1e037, 1e038, 1e039, 1e040, 1e041, 1e042, 1e043, 1e044, 1e045, 1e046, 1e047,
        1e048, 1e049, 1e050, 1e051, 1e052, 1e053, 1e054, 1e055, 1e056, 1e057, 1e058, 1e059, 1e060, 1e061, 1e062, 1e063,
        1e064, 1e065, 1e066, 1e067, 1e068, 1e069, 1e070, 1e071, 1e072, 1e073, 1e074, 1e075, 1e076, 1e077, 1e078, 1e079,
        1e080, 1e081, 1e082, 1e083, 1e084, 1e085, 1e086, 1e087, 1e088, 1e089, 1e090, 1e091, 1e092, 1e093, 1e094, 1e095,
        1e096, 1e097, 1e098, 1e099, 1e100, 1e101, 1e102, 1e103, 1e104, 1e105, 1e106, 1e107, 1e108, 1e109, 1e110, 1e111,
        1e112, 1e113, 1e114, 1e115, 1e116, 1e117, 1e118, 1e119, 1e120, 1e121, 1e122, 1e123, 1e124, 1e125, 1e126, 1e127,
        1e128, 1e129, 1e130, 1e131, 1e132, 1e133, 1e134, 1e135, 1e136, 1e137, 1e138, 1e139, 1e140, 1e141, 1e142, 1e143,
        1e144, 1e145, 1e146, 1e147, 1e148, 1e149, 1e150, 1e151, 1e152, 1e153, 1e154, 1e155, 1e156, 1e157, 1e158, 1e159,
        1e160, 1e161, 1e162, 1e163, 1e164, 1e165, 1e166, 1e167, 1e168, 1e169, 1e170, 1e171, 1e172, 1e173, 1e174, 1e175,
        1e176, 1e177, 1e178, 1e179, 1e180, 1e181, 1e182, 1e183, 1e184, 1e185, 1e186, 1e187, 1e188, 1e189, 1e190, 1e191,
        1e192, 1e193, 1e194, 1e195, 1e196, 1e197, 1e198, 1e199, 1e200, 1e201, 1e202, 1e203, 1e204, 1e205, 1e206, 1e207,
        1e208, 1e209, 1e210, 1e211, 1e212, 1e213, 1e214, 1e215, 1e216, 1e217, 1e218, 1e219, 1e220, 1e221, 1e222, 1e223,
        1e224, 1e225, 1e226, 1e227, 1e228, 1e229, 1e230, 1e231, 1e232, 1e233, 1e234, 1e235, 1e236, 1e237, 1e238, 1e239,
        1e240, 1e241, 1e242, 1e243, 1e244, 1e245, 1e246, 1e247, 1e248, 1e249, 1e250, 1e251, 1e252, 1e253, 1e254, 1e255,
        1e256, 1e257, 1e258, 1e259, 1e260, 1e261, 1e262, 1e263, 1e264, 1e265, 1e266, 1e267, 1e268, 1e269, 1e270, 1e271,
        1e272, 1e273, 1e274, 1e275, 1e276, 1e277, 1e278, 1e279, 1e280, 1e281, 1e282, 1e283, 1e284, 1e285, 1e286, 1e287,
        1e288, 1e289, 1e290, 1e291, 1e292, 1e293, 1e294, 1e295, 1e296, 1e297, 1e298, 1e299, 1e300, 1e301, 1e302, 1e303,
        1e304, 1e305, 1e306, 1e307, 1e308};

#define ARRAY_ELEMENTS_NUM(A) ((uint64_t)(sizeof(A) / sizeof(A[0])))

double MathFunctions::double_round(double value, int64_t dec, bool dec_unsigned, bool truncate) {
    bool dec_negative = (dec < 0) && !dec_unsigned;
    uint64_t abs_dec = dec_negative ? -dec : dec;
    /*
       tmp2 is here to avoid return the value with 80 bit precision
       This will fix that the test round(0.1,1) = round(0.1,1) is true
       Tagging with volatile is no guarantee, it may still be optimized away...
       */
    volatile double tmp2 = 0.0;

    double tmp = (abs_dec < ARRAY_ELEMENTS_NUM(log_10) ? log_10[abs_dec] : std::pow(10.0, (double)abs_dec));

    // Pre-compute these, to avoid optimizing away e.g. 'floor(v/tmp) * tmp'.
    volatile double value_div_tmp = value / tmp;
    volatile double value_mul_tmp = value * tmp;

    if (dec_negative && std::isinf(tmp)) {
        tmp2 = 0.0;
    } else if (!dec_negative && std::isinf(value_mul_tmp)) {
        tmp2 = value;
    } else if (truncate) {
        if (value >= 0.0) {
            tmp2 = dec < 0 ? std::floor(value_div_tmp) * tmp : std::floor(value_mul_tmp) / tmp;
        } else {
            tmp2 = dec < 0 ? std::ceil(value_div_tmp) * tmp : std::ceil(value_mul_tmp) / tmp;
        }
    } else {
        tmp2 = dec < 0 ? std::rint(value_div_tmp) * tmp : std::rint(value_mul_tmp) / tmp;
    }

    return tmp2;
}

bool MathFunctions::decimal_in_base_to_decimal(int64_t src_num, int8_t src_base, int64_t* result) {
    uint64_t temp_num = std::abs(src_num);
    int32_t place = 1;
    *result = 0;
    do {
        int32_t digit = temp_num % 10;
        // Reset result if digit is not representable in src_base.
        if (digit >= src_base) {
            *result = 0;
            place = 1;
        } else {
            *result += digit * place;
            place *= src_base;
            // Overflow.
            if (UNLIKELY(*result < digit)) {
                return false;
            }
        }
        temp_num /= 10;
    } while (temp_num > 0);
    *result = (src_num < 0) ? -(*result) : *result;
    return true;
}

bool MathFunctions::handle_parse_result(int8_t dest_base, int64_t* num, StringParser::ParseResult parse_res) {
    // On overflow set special value depending on dest_base.
    // This is consistent with Hive and MySQL's behavior.
    if (parse_res == StringParser::PARSE_OVERFLOW) {
        if (dest_base < 0) {
            *num = -1;
        } else {
            *num = std::numeric_limits<uint64_t>::max();
        }
    } else if (parse_res == StringParser::PARSE_FAILURE) {
        // Some other error condition.
        return false;
    }
    return true;
}

const char* MathFunctions::_s_alphanumeric_chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
std::string MathFunctions::decimal_to_base(int64_t src_num, int8_t dest_base) {
    // Max number of digits of any base (base 2 gives max digits), plus sign.
    const size_t max_digits = sizeof(uint64_t) * 8 + 1;
    char buf[max_digits];
    int32_t result_len = 0;
    int32_t buf_index = max_digits - 1;
    uint64_t temp_num;
    if (dest_base < 0) {
        // Dest base is negative, treat src_num as signed.
        temp_num = std::abs(src_num);
    } else {
        // Dest base is positive. We must interpret src_num in 2's complement.
        // Convert to an unsigned int to properly deal with 2's complement conversion.
        temp_num = static_cast<uint64_t>(src_num);
    }
    int abs_base = std::abs(dest_base);
    do {
        buf[buf_index] = _s_alphanumeric_chars[temp_num % abs_base];
        temp_num /= abs_base;
        --buf_index;
        ++result_len;
    } while (temp_num > 0);
    // Add optional sign.
    if (src_num < 0 && dest_base < 0) {
        buf[buf_index] = '-';
        ++result_len;
    }
    return std::string(buf + max_digits - result_len, result_len);
}

ColumnPtr MathFunctions::conv_int(FunctionContext* context, const starrocks::vectorized::Columns& columns) {
    auto bigint = ColumnViewer<TYPE_BIGINT>(columns[0]);
    auto src_base = ColumnViewer<TYPE_TINYINT>(columns[1]);
    auto dest_base = ColumnViewer<TYPE_TINYINT>(columns[2]);

    ColumnBuilder<TYPE_VARCHAR> result;
    auto size = columns[0]->size();
    for (int row = 0; row < size; ++row) {
        if (bigint.is_null(row) || src_base.is_null(row) || dest_base.is_null(row)) {
            result.append_null();
            continue;
        }

        int64_t binint_value = bigint.value(row);
        int8_t src_base_value = src_base.value(row);
        int8_t dest_base_value = dest_base.value(row);
        if (std::abs(src_base_value) < MIN_BASE || std::abs(src_base_value) > MAX_BASE ||
            std::abs(dest_base_value) < MIN_BASE || std::abs(dest_base_value) > MAX_BASE) {
            result.append_null();
            continue;
        }

        if (src_base_value < 0 && binint_value >= 0) {
            result.append_null();
            continue;
        }

        int64_t decimal_num = binint_value;
        if (src_base_value != 10) {
            if (!decimal_in_base_to_decimal(binint_value, src_base_value, &decimal_num)) {
                handle_parse_result(dest_base_value, &decimal_num, StringParser::PARSE_OVERFLOW);
            }
        }

        result.append(Slice(decimal_to_base(decimal_num, dest_base_value)));
    }

    return result.build(ColumnHelper::is_all_const(columns));
}

ColumnPtr MathFunctions::conv_string(FunctionContext* context, const starrocks::vectorized::Columns& columns) {
    auto string_viewer = ColumnViewer<TYPE_VARCHAR>(columns[0]);
    auto src_base = ColumnViewer<TYPE_TINYINT>(columns[1]);
    auto dest_base = ColumnViewer<TYPE_TINYINT>(columns[2]);

    ColumnBuilder<TYPE_VARCHAR> result;
    auto size = columns[0]->size();
    for (int row = 0; row < size; ++row) {
        if (string_viewer.is_null(row) || src_base.is_null(row) || dest_base.is_null(row)) {
            result.append_null();
            continue;
        }

        auto string_value = string_viewer.value(row);
        int8_t src_base_value = src_base.value(row);
        int8_t dest_base_value = dest_base.value(row);
        if (std::abs(src_base_value) < MIN_BASE || std::abs(src_base_value) > MAX_BASE ||
            std::abs(dest_base_value) < MIN_BASE || std::abs(dest_base_value) > MAX_BASE) {
            result.append_null();
            continue;
        }

        StringParser::ParseResult parse_res;
        int64_t decimal_num = StringParser::string_to_int<int64_t>(reinterpret_cast<char*>(string_value.data),
                                                                   string_value.size, src_base_value, &parse_res);

        if (src_base_value < 0 && decimal_num >= 0) {
            result.append_null();
            continue;
        }

        if (!handle_parse_result(dest_base_value, &decimal_num, parse_res)) {
            result.append(Slice("0", 1));
            continue;
        }

        result.append(Slice(decimal_to_base(decimal_num, dest_base_value)));
    }

    return result.build(ColumnHelper::is_all_const(columns));
}

Status MathFunctions::rand_prepare(starrocks_udf::FunctionContext* context,
                                   starrocks_udf::FunctionContext::FunctionStateScope scope) {
    if (scope == FunctionContext::THREAD_LOCAL) {
        auto* seed = reinterpret_cast<uint32_t*>(context->allocate(sizeof(uint32_t)));
        context->set_function_state(scope, seed);
        if (context->get_num_args() == 1) {
            // This is a call to RandSeed, initialize the seed
            // TODO: should we support non-constant seed?
            if (!context->is_constant_column(0)) {
                std::stringstream error;
                error << "Seed argument to rand() must be constant";
                context->set_error(error.str().c_str());
                return Status::InvalidArgument(error.str());
            }

            auto seed_column = context->get_constant_column(0);
            if (seed_column->only_null() || seed_column->is_null(0)) {
                return Status::OK();
            }

            int64_t seed_value = ColumnHelper::get_const_value<TYPE_BIGINT>(seed_column);
            *seed = seed_value;
        } else {
            *seed = GetCurrentTimeNanos();
        }
    }
    return Status::OK();
}

Status MathFunctions::rand_close(starrocks_udf::FunctionContext* context,
                                 starrocks_udf::FunctionContext::FunctionStateScope scope) {
    if (scope == FunctionContext::THREAD_LOCAL) {
        auto* seed = reinterpret_cast<uint8_t*>(context->get_function_state(FunctionContext::THREAD_LOCAL));
        context->free(seed);
    }
    return Status::OK();
}

ColumnPtr MathFunctions::rand(FunctionContext* context, const Columns& columns) {
    int32_t num_rows = ColumnHelper::get_const_value<TYPE_INT>(columns[columns.size() - 1]);
    auto* seed = reinterpret_cast<uint32_t*>(context->get_function_state(FunctionContext::THREAD_LOCAL));
    DCHECK(seed != nullptr);

    ColumnBuilder<TYPE_DOUBLE> result;
    generate_randoms(&result, num_rows, seed);

    return result.build(false);
}

ColumnPtr MathFunctions::rand_seed(FunctionContext* context, const Columns& columns) {
    DCHECK_EQ(columns.size(), 2);

    if (columns[0]->only_null()) {
        return ColumnHelper::create_const_null_column(columns[0]->size());
    }

    return rand(context, columns);
}

} // namespace vectorized
} // namespace starrocks
