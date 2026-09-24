#include <Columns/ColumnConst.h>
#include <Columns/ColumnSet.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeSet.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Interpreters/Set.h>
#include <Interpreters/castColumn.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int LOGICAL_ERROR;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{

/** `__keyRangesIn(key, ranges)` - whether the key lies in any of a set of key constraints.
  *
  * `key` is a single column or a tuple of them. `ranges` is a set holding one column per key
  * position: a position constrained by a range carries a `Tuple(lower, upper)`, one constrained by
  * equality carries the value itself. So
  *
  *     __keyRangesIn((k1, k2), (SELECT c, (lo, hi) FROM t))
  *
  * is true for a row when some row of `t` has `k1 = c` and `lo <= k2 <= hi`.
  *
  * This is the compact form of a disjunction of key constraints. `IN` expresses the same thing when
  * every position is an equality; this adds ranges, which an `IN` list cannot express without
  * enumerating every value. `KeyCondition` recognises it and prunes on it, so the index does most of
  * the work and this evaluates only what survives.
  *
  * Internal: the name is not meant to be written by hand, and nothing guarantees its stability.
  */
class FunctionKeyRangesIn final : public IFunction
{
public:
    static constexpr auto name = "__keyRangesIn";

    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionKeyRangesIn>(); }

    String getName() const override { return name; }

    size_t getNumberOfArguments() const override { return 2; }

    /// The set argument is not a value to be materialised per row.
    bool useDefaultImplementationForConstants() const override { return false; }
    bool useDefaultImplementationForNulls() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo &) const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() != 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Function '{}' expects 2 arguments, got {}", getName(), arguments.size());

        if (!typeid_cast<const DataTypeSet *>(arguments[1].get()))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Second argument of function '{}' must be a set, got {}", getName(), arguments[1]->getName());

        return std::make_shared<DataTypeUInt8>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        const ColumnSet * column_set = tryGetColumnSet(arguments[1].column);
        if (!column_set)
            throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                "Second argument of function '{}' must be a set, got {}", getName(), arguments[1].column->getName());

        auto future_set = column_set->getData();
        auto set = future_set ? future_set->get() : nullptr;
        if (!set)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Set passed to function '{}' is not ready", getName());

        if (!set->hasExplicitSetElements())
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Set passed to function '{}' does not keep its elements", getName());

        auto key_columns = flattenKey(arguments[0], input_rows_count);

        auto range_set = makeRangeSet(*set, key_columns);
        if (!range_set)
            return ColumnUInt8::create(input_rows_count, 0);

        auto result = ColumnUInt8::create(input_rows_count, 0);
        auto & data = result->getData();
        for (size_t row = 0; row < input_rows_count; ++row)
            data[row] = range_set->contains(key_columns, row) ? 1 : 0;

        return result;
    }

private:
    /// A set argument reaches a function either bare or wrapped in a constant.
    static const ColumnSet * tryGetColumnSet(const ColumnPtr & column)
    {
        if (const auto * column_set = checkAndGetColumnConstData<const ColumnSet>(column.get()))
            return column_set;
        return checkAndGetColumn<const ColumnSet>(column.get());
    }

    /// The key argument is either a tuple of key columns or a single one; both become one column
    /// per position. A constant is expanded, since the set is searched per row either way.
    static Columns flattenKey(const ColumnWithTypeAndName & argument, size_t input_rows_count)
    {
        ColumnPtr column = argument.column;
        if (const auto * constant = typeid_cast<const ColumnConst *>(column.get()))
            column = constant->convertToFullColumnIfConst();

        Columns key_columns;
        if (const auto * tuple = typeid_cast<const ColumnTuple *>(column.get()))
        {
            for (size_t i = 0; i < tuple->tupleSize(); ++i)
                key_columns.push_back(tuple->getColumnPtr(i));
        }
        else
        {
            key_columns.push_back(column);
        }

        for (auto & key_column : key_columns)
        {
            if (key_column->size() < input_rows_count)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "Key column of function '{}' has {} rows, expected {}", name, key_column->size(), input_rows_count);
        }

        return key_columns;
    }

    /// Null when the set constrains the key to nothing, which the caller answers false for.
    static MergeTreeKeyRangeSetPtr makeRangeSet(const Set & set, const Columns & key_columns)
    {
        auto elements = set.getSetElements();
        auto types = set.getElementsTypes();

        Columns lower;
        Columns upper;
        DataTypes element_types;
        if (!splitKeyRangeCorners(elements, types, lower, upper, element_types))
            throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                "Set passed to function '{}' must hold one column per key position, each a value or a "
                "Tuple(lower, upper) of one type", name);

        if (lower.size() != key_columns.size())
            throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                "Set passed to function '{}' constrains {} positions but the key has {}",
                name, lower.size(), key_columns.size());

        /// The container type is fixed by `MergeTreeSetIndex`'s existing interface, which both this
        /// and `buildKeyRangeSet` take.
        std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> indexes_mapping; // STYLE_CHECK_ALLOW_STD_CONTAINERS
        indexes_mapping.reserve(lower.size());
        for (size_t i = 0; i < lower.size(); ++i)
            indexes_mapping.push_back({i, i, {}});

        auto built = buildKeyRangeSet(lower, upper, std::move(indexes_mapping));
        return built.outcome == KeyRangeSetOutcome::Built ? built.set : nullptr;
    }
};

}

REGISTER_FUNCTION(KeyRangesIn)
{
    FunctionDocumentation::Description description = R"(
Internal function. Returns whether a key lies in any of a set of key constraints.

The second argument holds one column per key position: a position constrained by a range carries a
`Tuple(lower, upper)`, and a position constrained by equality carries the value itself. It is the
compact form of a disjunction of such constraints, which `IN` cannot express without enumerating
every value of the ranged position. `KeyCondition` recognises it and uses it to prune granules.

Not intended to be written by hand; its name and behaviour may change.
    )";
    FunctionDocumentation::Syntax syntax = "__keyRangesIn(key, ranges)";
    FunctionDocumentation::Arguments arguments = {
        {"key", "A key column, or a tuple of them.", {"Any"}},
        {"ranges", "A set holding one column per key position.", {"Set"}}
    };
    FunctionDocumentation::ReturnedValue returned_value
        = {"Returns `1` when the key lies in some constraint of the set, otherwise `0`.", {"UInt8"}};
    FunctionDocumentation::Examples examples = {
    {
        "Usage example",
        "SELECT __keyRangesIn((3, 5), (SELECT 3, (1, 10)))",
        R"(
┌─res─┐
│   1 │
└─────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in = {26, 9};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Other;
    FunctionDocumentation documentation
        = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionKeyRangesIn>(documentation);
}

}
