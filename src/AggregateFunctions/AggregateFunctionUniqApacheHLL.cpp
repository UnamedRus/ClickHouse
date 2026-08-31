#include "config.h"

#if USE_DATASKETCHES

#include <AggregateFunctions/AggregateFunctionUniqApacheHLL.h>
#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <Common/FieldVisitorConvertToNumber.h>

namespace DB
{

AggregateFunctionPtr createAggregateFunctionUniqApacheHLL(
    const std::string & name, const DataTypes & argument_types, const Array & params, const Settings *)
{
    uint8_t lg_config_k = 12;
    datasketches::target_hll_type target_type = datasketches::HLL_4;

    if (params.size() > 2)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Aggregate function {} accepts at most two parameters (lg_k, type).", name);

    if (!params.empty())
    {
        const UInt64 lg_k_param = applyVisitor(FieldVisitorConvertToNumber<UInt64>(), params[0]);
        if (lg_k_param < 4 || lg_k_param > 21)
            throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND,
                "Parameter lg_k for aggregate function {} is out of range: [4, 21].", name);
        lg_config_k = static_cast<uint8_t>(lg_k_param);
    }

    if (params.size() == 2)
    {
        if (params[1].getType() != Field::Types::String)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Parameter type for aggregate function {} must be a string.", name);

        const String type_param = params[1].safeGet<String>();
        if (type_param == "HLL_4")
            target_type = datasketches::HLL_4;
        else if (type_param == "HLL_6")
            target_type = datasketches::HLL_6;
        else if (type_param == "HLL_8")
            target_type = datasketches::HLL_8;
        else
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Parameter type for aggregate function {} must be one of 'HLL_4', 'HLL_6', 'HLL_8'.", name);
    }

    if (argument_types.empty())
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Incorrect number of arguments for aggregate function {}", name);

    /// Only the exact hash can read arguments that are not contiguous in memory.
    const bool use_exact_hash_function = !isAllArgumentsContiguousInMemory(argument_types);

    if (argument_types.size() == 1)
    {
        const IDataType & argument_type = *argument_types[0];

        AggregateFunctionPtr res(createWithNumericType<AggregateFunctionUniqApacheHLL>(
            argument_type, lg_config_k, target_type, argument_types, params));
        if (res)
            return res;

        WhichDataType which(argument_type);
        if (which.isDate())
            return std::make_shared<AggregateFunctionUniqApacheHLL<DataTypeDate::FieldType>>(lg_config_k, target_type, argument_types, params);
        if (which.isDate32())
            return std::make_shared<AggregateFunctionUniqApacheHLL<DataTypeDate32::FieldType>>(lg_config_k, target_type, argument_types, params);
        if (which.isDateTime())
            return std::make_shared<AggregateFunctionUniqApacheHLL<DataTypeDateTime::FieldType>>(lg_config_k, target_type, argument_types, params);
        if (which.isStringOrFixedString())
            return std::make_shared<AggregateFunctionUniqApacheHLL<String>>(lg_config_k, target_type, argument_types, params);
        if (which.isUUID())
            return std::make_shared<AggregateFunctionUniqApacheHLL<DataTypeUUID::FieldType>>(lg_config_k, target_type, argument_types, params);
        if (which.isIPv4())
            return std::make_shared<AggregateFunctionUniqApacheHLL<DataTypeIPv4::FieldType>>(lg_config_k, target_type, argument_types, params);
        if (which.isIPv6())
            return std::make_shared<AggregateFunctionUniqApacheHLL<DataTypeIPv6::FieldType>>(lg_config_k, target_type, argument_types, params);
        if (which.isTuple())
        {
            if (use_exact_hash_function)
                return std::make_shared<AggregateFunctionUniqApacheHLLVariadic<true, true>>(lg_config_k, target_type, argument_types, params);
            return std::make_shared<AggregateFunctionUniqApacheHLLVariadic<false, true>>(lg_config_k, target_type, argument_types, params);
        }
    }

    /// The variadic method is also the generic fallback for a single argument of any other type.
    if (use_exact_hash_function)
        return std::make_shared<AggregateFunctionUniqApacheHLLVariadic<true, false>>(lg_config_k, target_type, argument_types, params);
    return std::make_shared<AggregateFunctionUniqApacheHLLVariadic<false, false>>(lg_config_k, target_type, argument_types, params);
}

void registerAggregateFunctionUniqApacheHLL(AggregateFunctionFactory & factory);
void registerAggregateFunctionUniqApacheHLL(AggregateFunctionFactory & factory)
{
    FunctionDocumentation::Description description = R"(
Calculates the approximate number of different argument values using an [Apache DataSketches](https://datasketches.apache.org/docs/HLL/HllSketches.html) HyperLogLog sketch.

The serialized state produced by the `-State` combinator carries the sketch in the Apache DataSketches HLL format, framed with a varint length prefix, so sketches can be exchanged with external services (Java, Python, C++) using the standard `-State`/`-Merge` combinators. For example, a sketch built by an upstream service can be merged with `uniqApacheHLLMerge`, and a sketch built in ClickHouse can be exported with `uniqApacheHLLState`.

Integers are hashed as their 8-byte representation (matching DataSketches `update(long)`), floating-point values as an IEEE-754 double, strings as their raw bytes, `UUID`s as their canonical 16 bytes and `IPv6` addresses in network order. Sketches over those types can be reproduced by a producer outside ClickHouse that hashes the same bytes.

Sketches over any other type cannot. The 128 and 256 bit integers are hashed in ClickHouse's own byte order, for which no convention is shared between implementations. Every remaining type - a decimal, `DateTime64`, an array - and every call with more than one argument is first hashed to a single value by ClickHouse, exactly as `uniq` does, and only that hash reaches the sketch.

An estimate obtained by merging sketches is not the same number as one computed in a single pass over the same values, even though both are derived from identical registers: DataSketches reports the HIP estimator for a sketch that has only been updated and the composite estimator for one produced by a union. The result therefore depends on how the aggregation was partitioned across threads, parts and shards, and is slightly less accurate once any merge has taken place.

The resolution of a merged sketch is the smallest `lg_k` among its inputs, not the `lg_k` named by the type. Merging a sketch that was built with a lower `lg_k` - for example one produced by another service - permanently lowers the resolution of both the estimate and the state written back.
    )";
    FunctionDocumentation::Syntax syntax = "uniqApacheHLL([lg_k, [type]])(x[, y, ...])";
    FunctionDocumentation::Arguments arguments = {
        {"x[, y, ...]", "Columns to compute the number of distinct combinations of.", {"Any"}},
    };
    FunctionDocumentation::Parameters parameters = {
        {"lg_k", "Optional. Log-base-2 of the number of buckets, in range [4, 21]. Higher means better accuracy and more memory. Default: 12.", {"UInt8"}},
        {"type", "Optional. Storage format of the sketch: 'HLL_4', 'HLL_6', or 'HLL_8'. Default: 'HLL_4'.", {"String"}},
    };
    FunctionDocumentation::ReturnedValue returned_value = {"Returns the approximate number of distinct values.", {"UInt64"}};
    FunctionDocumentation::Examples examples = {
        {"Basic usage", "SELECT uniqApacheHLL(number) FROM numbers(1000)", "1000"},
        {"With parameters", "SELECT uniqApacheHLL(14, 'HLL_8')(number) FROM numbers(1000)", "1000"},
    };
    FunctionDocumentation::IntroducedIn introduced_in = {26, 6};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::AggregateFunction;
    FunctionDocumentation documentation = {description, syntax, arguments, parameters, returned_value, examples, introduced_in, category};

    AggregateFunctionProperties properties = { .returns_default_when_only_null = true, .is_order_dependent = false };

    factory.registerFunction("uniqApacheHLL", {createAggregateFunctionUniqApacheHLL, documentation, properties});
}

}

#endif
