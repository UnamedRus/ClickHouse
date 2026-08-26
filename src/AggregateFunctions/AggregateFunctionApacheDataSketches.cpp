#include "config.h"

#if USE_DATASKETCHES

#include <AggregateFunctions/AggregateFunctionApacheDataSketches.h>
#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/HllSketchData.h>

namespace DB
{

namespace
{

void registerUniqApacheHLL(AggregateFunctionFactory & factory)
{
    FunctionDocumentation::Description description = R"(
Calculates the approximate number of different argument values using an [Apache DataSketches](https://datasketches.apache.org/docs/HLL/HllSketches.html) HyperLogLog sketch.

The serialized state produced by the `-State` combinator is byte-compatible with the Apache DataSketches HLL format, so sketches can be exchanged with external services (Java, Python, C++) using the standard `-State`/`-Merge` combinators. For example, a sketch built by an upstream service can be merged with `uniqApacheHLLMerge`, and a sketch built in ClickHouse can be exported with `uniqApacheHLLState`.

Integers are hashed as their 8-byte representation (matching DataSketches `update(long)`), floating-point values as an IEEE-754 double, and strings as their raw bytes.

An estimate obtained by merging sketches is not the same number as one computed in a single pass over the same values, even though both are derived from identical registers: DataSketches reports the HIP estimator for a sketch that has only been updated and the composite estimator for one produced by a union. The result therefore depends on how the aggregation was partitioned across threads, parts and shards, and is slightly less accurate once any merge has taken place.

The resolution of a merged sketch is the smallest `lg_k` among its inputs, not the `lg_k` named by the type. Merging a sketch that was built with a lower `lg_k` - for example one produced by another service - permanently lowers the resolution of both the estimate and the state written back.
    )";
    FunctionDocumentation::Syntax syntax = "uniqApacheHLL([lg_k, [type]])(x)";
    FunctionDocumentation::Arguments arguments = {
        {"x", "Column to compute the number of distinct values of.", {"(U)Int*", "Float*", "Date", "DateTime", "String"}},
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

    factory.registerFunction(
        HllSketchPolicy::getName(),
        {createAggregateFunctionApacheDataSketches<HllSketchPolicy>, documentation, properties});
}

}

void registerAggregateFunctionsApacheDataSketches(AggregateFunctionFactory & factory);
void registerAggregateFunctionsApacheDataSketches(AggregateFunctionFactory & factory)
{
    registerUniqApacheHLL(factory);
    /// Additional DataSketches-backed functions (e.g. CPC, quantiles) are registered here as they
    /// are added, each reusing `createAggregateFunctionApacheDataSketches` with its own policy.
}

}

#endif
