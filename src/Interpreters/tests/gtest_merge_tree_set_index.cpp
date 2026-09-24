#include <Interpreters/Set.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <Storages/MergeTree/KeyCondition.h>

#include <gtest/gtest.h>

using namespace DB;

TEST(MergeTreeSetIndex, checkInRangeOne)
{
    DataTypes types = {std::make_shared<const DataTypeInt64>()};

    auto mut = types[0]->createColumn();
    mut->insert(1);
    mut->insert(5);
    mut->insert(7);

    Columns columns = {std::move(mut)};

    std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> mapping = {{0, 0, {}}};
    auto set = std::make_unique<MergeTreeSetIndex>(columns, std::move(mapping));

    // Left and right bounded
    Ranges ranges = {Range(1, true, 4, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "(1, 4)";

    ranges = {Range(2, true, 4, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "(2, 4)";

    ranges = {Range(-1, true, 0, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "(-1, 0)";

    ranges = {Range(-1, true, 10, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "(-1, 10)";

    // Left bounded
    ranges = {Range::createLeftBounded(1, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "(1, +inf)";

    ranges = {Range::createLeftBounded(-1, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "(-1, +inf)";

    ranges = {Range::createLeftBounded(10, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "(10, +inf)";

    // Right bounded
    ranges = {Range::createRightBounded(1, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "(-inf, 1)";

    ranges = {Range::createRightBounded(-1, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "(-inf, -1)";

    ranges = {Range::createRightBounded(10, true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "(-inf, 10)";
}

TEST(MergeTreeSetIndex, checkInRangeTuple)
{
    DataTypes types = {std::make_shared<const DataTypeUInt64>(), std::make_shared<const DataTypeString>()};

    Columns columns;
    {
        auto values = {1, 1, 3, 3, 3, 10};
        auto mut = types[0]->createColumn();
        for (const auto & val : values)
            mut->insert(val);
        columns.push_back(std::move(mut));
    }

    {
        auto values = {"a", "b", "a", "a", "b", "c"};
        auto mut = types[1]->createColumn();
        for (const auto & val : values)
            mut->insert(val);
        columns.push_back(std::move(mut));
    }

    std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> mapping = {{0, 0, {}}, {1, 1, {}}};
    auto set = std::make_unique<MergeTreeSetIndex>(columns, std::move(mapping));

    Ranges ranges = {Range(1), Range("a", true, "c", true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "Range(1), Range('a', true, 'c', true)";

    ranges = {Range(1, false, 3, false), Range::createWholeUniverseWithoutNull()};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "Range(1, false, 3, false), Range::createWholeUniverseWithoutNull()";

    ranges = {Range(2, false, 5, false), Range::createWholeUniverseWithoutNull()};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "Range(2, false, 5, false), Range::createWholeUniverseWithoutNull()";

    ranges = {Range(3), Range::createLeftBounded("a", true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "Range(3), Range::createLeftBounded('a', true)";

    ranges = {Range(3), Range::createLeftBounded("f", true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "Range(3), Range::createLeftBounded('f', true)";

    ranges = {Range(3), Range::createRightBounded("a", true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "Range(3), Range::createRightBounded('a', true)";

    ranges = {Range(3), Range::createRightBounded("b", true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "Range(3), Range::createRightBounded('b', true)";

    ranges = {Range(1), Range("b")};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "Range(1), Range('b')";

    ranges = {Range(1), Range("c")};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "Range(1), Range('c')";

    ranges = {Range(2, true, 3, true), Range::createWholeUniverseWithoutNull()};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, true) << "Range(2, true, 3, true), Range('x', true, 'z', true)";

    ranges = {Range(2), Range("a", true, "z", true)};
    ASSERT_EQ(set->checkInRange(ranges, types).can_be_true, false) << "Range(2, true, 3, true), Range('c', true, 'z', true)";
}

/// The sparse overload takes a map from key column to position in the given ranges, where -1 (or an
/// index past the end of the map) means that column is not tracked and must be treated as
/// unconstrained. It had no coverage; these pin that behaviour.
TEST(MergeTreeSetIndex, checkInRangeSparseTuple)
{
    DataTypes types = {std::make_shared<const DataTypeUInt64>(), std::make_shared<const DataTypeString>()};

    Columns columns;
    {
        auto values = {1, 1, 3, 3, 3, 10};
        auto mut = types[0]->createColumn();
        for (const auto & val : values)
            mut->insert(val);
        columns.push_back(std::move(mut));
    }
    {
        auto values = {"a", "b", "a", "a", "b", "c"};
        auto mut = types[1]->createColumn();
        for (const auto & val : values)
            mut->insert(val);
        columns.push_back(std::move(mut));
    }

    std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> mapping = {{0, 0, {}}, {1, 1, {}}};
    auto set = std::make_unique<MergeTreeSetIndex>(columns, std::move(mapping));

    /// Both columns tracked: must agree with the dense overload.
    {
        std::vector<int> pos = {0, 1};
        Ranges ranges = {Range(1), Range("a", true, "c", true)};
        ASSERT_EQ(set->checkInRange(pos, ranges, types).can_be_true, true) << "tracked: (1), ('a','c')";

        ranges = {Range(2), Range("a", true, "c", true)};
        ASSERT_EQ(set->checkInRange(pos, ranges, types).can_be_true, false) << "tracked: (2), ('a','c')";
    }

    /// Second column untracked: unconstrained, so only the first column discriminates.
    {
        std::vector<int> pos = {0, -1};
        DataTypes sparse_types = {types[0]};

        Ranges ranges = {Range(1)};
        ASSERT_EQ(set->checkInRange(pos, ranges, sparse_types).can_be_true, true) << "untracked: (1), *";

        ranges = {Range(2)};
        ASSERT_EQ(set->checkInRange(pos, ranges, sparse_types).can_be_true, false) << "untracked: (2), *";

        ranges = {Range(3)};
        ASSERT_EQ(set->checkInRange(pos, ranges, sparse_types).can_be_true, true) << "untracked: (3), *";

        ranges = {Range(11)};
        ASSERT_EQ(set->checkInRange(pos, ranges, sparse_types).can_be_true, false) << "untracked: (11), *";
    }

    /// A map shorter than the key: any column past its end is untracked too.
    {
        std::vector<int> pos = {0};
        DataTypes sparse_types = {types[0]};

        Ranges ranges = {Range(3)};
        ASSERT_EQ(set->checkInRange(pos, ranges, sparse_types).can_be_true, true) << "short map: (3)";

        ranges = {Range(2)};
        ASSERT_EQ(set->checkInRange(pos, ranges, sparse_types).can_be_true, false) << "short map: (2)";
    }
}

TEST(MergeTreeSetIndex, checkInRangeSparseOne)
{
    DataTypes types = {std::make_shared<const DataTypeInt64>()};

    auto mut = types[0]->createColumn();
    mut->insert(1);
    mut->insert(5);
    mut->insert(7);
    Columns columns = {std::move(mut)};

    std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> mapping = {{0, 0, {}}};
    auto set = std::make_unique<MergeTreeSetIndex>(columns, std::move(mapping));

    std::vector<int> pos = {0};
    Ranges ranges = {Range(1, true, 4, true)};
    ASSERT_EQ(set->checkInRange(pos, ranges, types).can_be_true, true) << "(1, 4)";

    ranges = {Range(2, true, 4, true)};
    ASSERT_EQ(set->checkInRange(pos, ranges, types).can_be_true, false) << "(2, 4)";

    /// The only key column untracked: nothing constrains the set at all.
    std::vector<int> none = {-1};
    ranges = {Range(2, true, 4, true)};
    ASSERT_EQ(set->checkInRange(none, ranges, types).can_be_true, true) << "untracked key column";
}

namespace
{

/// Builds a key range set from closed intervals given per tuple position.
MergeTreeKeyRangeSet makeRangeSet(const DataTypes & types, const std::vector<std::vector<Field>> & lowers,
                                  const std::vector<std::vector<Field>> & uppers)
{
    Columns lower;
    Columns upper;
    for (size_t i = 0; i < types.size(); ++i)
    {
        auto lo = types[i]->createColumn();
        auto hi = types[i]->createColumn();
        for (size_t j = 0; j < lowers[i].size(); ++j)
        {
            lo->insert(lowers[i][j]);
            hi->insert(uppers[i][j]);
        }
        lower.push_back(std::move(lo));
        upper.push_back(std::move(hi));
    }

    std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> mapping;
    for (size_t i = 0; i < types.size(); ++i)
        mapping.push_back({i, i, {}});
    return MergeTreeKeyRangeSet(std::move(lower), std::move(upper), std::move(mapping));
}

}

TEST(MergeTreeKeyRangeSet, singleColumn)
{
    DataTypes types = {std::make_shared<const DataTypeInt64>()};
    /// Two disjoint windows: [1, 3] and [7, 9].
    auto set = makeRangeSet(types, {{Field(1), Field(7)}}, {{Field(3), Field(9)}});

    Ranges ranges = {Range(2, true, 2, true)};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "inside the first window";

    ranges = {Range(5, true, 6, true)};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, false) << "in the gap between windows";

    ranges = {Range(-5, true, 0, true)};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, false) << "below every window";

    ranges = {Range(10, true, 20, true)};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, false) << "above every window";

    ranges = {Range(3, true, 7, true)};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "touching both windows";

    ranges = {Range(1, true, 1, true)};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "on a window's lower edge";

    ranges = {Range(9, true, 9, true)};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "on a window's upper edge";

    ranges = {Range::createWholeUniverseWithoutNull()};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "unbounded";

    /// An entry can never prove a granule matches entirely.
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_false, true) << "can_be_false is always true";
}

/// The shape this exists for: equality on the leading key column, a range on the next. Each entry
/// is then an interval of key tuples, so the answer is exact rather than an over-approximation.
TEST(MergeTreeKeyRangeSet, prefixEqualityWithRange)
{
    DataTypes types = {std::make_shared<const DataTypeUInt64>(), std::make_shared<const DataTypeString>()};

    /// (k1 = 1 AND k2 BETWEEN 'a' AND 'c') OR (k1 = 3 AND k2 BETWEEN 'x' AND 'z')
    auto set = makeRangeSet(
        types,
        {{Field(UInt64(1)), Field(UInt64(3))}, {Field("a"), Field("x")}},
        {{Field(UInt64(1)), Field(UInt64(3))}, {Field("c"), Field("z")}});

    Ranges ranges = {Range(UInt64(1)), Range("b")};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "(1, 'b') is in the first window";

    ranges = {Range(UInt64(1)), Range("z")};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, false) << "(1, 'z') is past the first window";

    ranges = {Range(UInt64(3)), Range("y")};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "(3, 'y') is in the second window";

    ranges = {Range(UInt64(3)), Range("b")};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, false) << "(3, 'b') is before the second window";

    ranges = {Range(UInt64(2)), Range::createWholeUniverseWithoutNull()};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, false) << "k1 = 2 lies between the windows";

    /// A granule spanning both windows.
    ranges = {Range(UInt64(1), true, UInt64(3), true), Range::createWholeUniverseWithoutNull()};
    ASSERT_EQ(set.checkInRange(ranges, types).can_be_true, true) << "granule covering both windows";
}

/// The constructor rejects an inverted entry, and entries whose corner arrays are not
/// non-decreasing, because the binary searches would then return wrong answers rather than slow
/// ones. That check raises LOGICAL_ERROR, which `Exception.cpp` turns into an assertion failure in
/// debug and sanitizer builds and into a thrown exception elsewhere - so it aborts in exactly the
/// configurations this test runs in, and there is no portable way to assert on it from here.

namespace
{

Columns makeColumns(const DataTypes & types, const std::vector<std::vector<Field>> & values)
{
    Columns columns;
    for (size_t i = 0; i < types.size(); ++i)
    {
        auto column = types[i]->createColumn();
        for (const auto & value : values[i])
            column->insert(value);
        columns.push_back(std::move(column));
    }
    return columns;
}

/// Assembles corner columns for entries given as (prefix equalities..., lower, upper) per row,
/// which is the shape the restricted case produces. `n_prefix` columns are shared by both corners.
std::pair<Columns, Columns> makeCorners(const DataTypes & types, const std::vector<std::vector<Field>> & values, size_t n_prefix)
{
    auto columns = makeColumns(types, values);
    Columns lower(columns.begin(), columns.begin() + n_prefix);
    lower.push_back(columns[n_prefix]);
    Columns upper(columns.begin(), columns.begin() + n_prefix);
    upper.push_back(columns[n_prefix + 1]);
    return {lower, upper};
}

std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> identityMapping(size_t tuple_size)
{
    std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> mapping;
    for (size_t i = 0; i < tuple_size; ++i)
        mapping.push_back({i, i, {}});
    return mapping;
}

}

/// `buildKeyRangeSet` has to establish what `MergeTreeKeyRangeSet` requires and does not check for
/// itself: ordered, non-overlapping entries.
TEST(BuildKeyRangeSet, coalescing)
{
    DataTypes bound_types = {std::make_shared<const DataTypeInt64>(), std::make_shared<const DataTypeInt64>()};

    /// Overlapping ranges become one.
    {
        auto [lower, upper] = makeCorners(bound_types, {{Field(1), Field(3)}, {Field(5), Field(8)}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.outcome, KeyRangeSetOutcome::Built);
        ASSERT_EQ(result.set->size(), 1u) << "[1,5] and [3,8] overlap";
    }

    /// Touching ranges become one.
    {
        auto [lower, upper] = makeCorners(bound_types, {{Field(1), Field(5)}, {Field(5), Field(9)}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.set->size(), 1u) << "[1,5] and [5,9] touch";
    }

    /// A contained range is absorbed.
    {
        auto [lower, upper] = makeCorners(bound_types, {{Field(1), Field(3)}, {Field(10), Field(4)}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.set->size(), 1u) << "[3,4] lies inside [1,10]";
        DataTypes key_types = {bound_types[0]};
        Ranges ranges = {Range(7, true, 7, true)};
        ASSERT_EQ(result.set->checkInRange(ranges, key_types).can_be_true, true) << "the merged entry still covers 7";
    }

    /// Disjoint ranges stay apart, whatever order they arrive in.
    {
        auto [lower, upper] = makeCorners(bound_types, {{Field(7), Field(1)}, {Field(9), Field(3)}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.set->size(), 2u) << "[7,9] and [1,3] are disjoint and given out of order";

        DataTypes key_types = {bound_types[0]};
        Ranges ranges = {Range(5, true, 5, true)};
        ASSERT_EQ(result.set->checkInRange(ranges, key_types).can_be_true, false) << "5 is in the gap";
    }

    /// A row whose bounds cross matches nothing and is dropped rather than rejected.
    {
        auto [lower, upper] = makeCorners(bound_types, {{Field(1), Field(9)}, {Field(3), Field(2)}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.set->size(), 1u) << "the row with lower above upper is dropped";
    }

    /// Nothing survives. This says the key is constrained to nothing, which is the opposite of
    /// there being no constraint - a caller must be able to tell the two apart.
    {
        auto [lower, upper] = makeCorners(bound_types, {{Field(9)}, {Field(1)}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.outcome, KeyRangeSetOutcome::MatchesNothing);
        ASSERT_EQ(result.set, nullptr);
    }
}

TEST(BuildKeyRangeSet, prefixIsRespected)
{
    DataTypes types = {
        std::make_shared<const DataTypeUInt64>(),
        std::make_shared<const DataTypeString>(),
        std::make_shared<const DataTypeString>()};

    /// (1, [a,c]), (1, [b,d]) overlap and merge; (2, [a,c]) is a different prefix and does not.
    const std::vector<std::vector<Field>> elements_values = {
        {Field(UInt64(1)), Field(UInt64(1)), Field(UInt64(2))},
        {Field("a"), Field("b"), Field("a")},
        {Field("c"), Field("d"), Field("c")}};

    auto [lower, upper] = makeCorners(types, elements_values, 1);
    auto result = buildKeyRangeSet(lower, upper, identityMapping(2));
    ASSERT_EQ(result.outcome, KeyRangeSetOutcome::Built);
    ASSERT_EQ(result.set->size(), 2u) << "the two entries under prefix 1 merge, prefix 2 stays separate";

    DataTypes key_types = {types[0], types[1]};

    Ranges ranges = {Range(UInt64(1)), Range("d")};
    ASSERT_EQ(result.set->checkInRange(ranges, key_types).can_be_true, true) << "(1, 'd') is inside the merged entry";

    ranges = {Range(UInt64(2)), Range("d")};
    ASSERT_EQ(result.set->checkInRange(ranges, key_types).can_be_true, false) << "(2, 'd') is past the prefix-2 entry";
}

/// A NULL bound makes the comparison NULL, not true, so the row satisfies nothing.
TEST(BuildKeyRangeSet, nullBoundsAreDropped)
{
    DataTypes nullable_types = {
        std::make_shared<const DataTypeNullable>(std::make_shared<const DataTypeInt64>()),
        std::make_shared<const DataTypeNullable>(std::make_shared<const DataTypeInt64>())};

    {
        auto [lower, upper] = makeCorners(nullable_types, {{Field(1), Field()}, {Field(3), Field(9)}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.outcome, KeyRangeSetOutcome::Built);
        ASSERT_EQ(result.set->size(), 1u) << "the row with a NULL lower bound is dropped";
    }

    {
        auto [lower, upper] = makeCorners(nullable_types, {{Field(1)}, {Field()}}, 0);
        auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
        ASSERT_EQ(result.outcome, KeyRangeSetOutcome::MatchesNothing) << "only row had a NULL upper bound";
    }
}

/// `contains` answers the row-level question exactly, where `checkInRange` answers the
/// granule-level one conservatively.
TEST(MergeTreeKeyRangeSet, containsRow)
{
    DataTypes bound_types = {std::make_shared<const DataTypeInt64>(), std::make_shared<const DataTypeInt64>()};
    /// [1, 3] and [7, 9]
    auto [lower, upper] = makeCorners(bound_types, {{Field(1), Field(7)}, {Field(3), Field(9)}}, 0);
    auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
    ASSERT_EQ(result.outcome, KeyRangeSetOutcome::Built);

    DataTypes key_types = {bound_types[0]};
    auto probe = makeColumns(key_types, {{Field(0), Field(1), Field(2), Field(3), Field(5), Field(7), Field(9), Field(10)}});
    const std::vector<bool> expected = {false, true, true, true, false, true, true, false};

    for (size_t row = 0; row < expected.size(); ++row)
        ASSERT_EQ(result.set->contains(probe, row), expected[row]) << "row " << row;
}

TEST(MergeTreeKeyRangeSet, containsRowWithPrefix)
{
    DataTypes types = {
        std::make_shared<const DataTypeUInt64>(),
        std::make_shared<const DataTypeString>(),
        std::make_shared<const DataTypeString>()};

    /// (1, [a,c]) and (3, [x,z])
    auto [lower, upper] = makeCorners(types, {
        {Field(UInt64(1)), Field(UInt64(3))},
        {Field("a"), Field("x")},
        {Field("c"), Field("z")}}, 1);
    auto result = buildKeyRangeSet(lower, upper, identityMapping(2));
    ASSERT_EQ(result.outcome, KeyRangeSetOutcome::Built);

    DataTypes key_types = {types[0], types[1]};
    auto probe = makeColumns(key_types, {
        {Field(UInt64(1)), Field(UInt64(1)), Field(UInt64(2)), Field(UInt64(3)), Field(UInt64(3))},
        {Field("b"),       Field("z"),       Field("b"),       Field("y"),       Field("a")}});
    const std::vector<bool> expected = {true, false, false, true, false};

    for (size_t row = 0; row < expected.size(); ++row)
        ASSERT_EQ(result.set->contains(probe, row), expected[row]) << "row " << row;
}

/// Two ranged columns. Entries like these overlap as lexicographic spans, which the earlier
/// restricted form rejected outright; the running maximum over upper corners is what admits them.
TEST(MergeTreeKeyRangeSet, multipleRangedColumns)
{
    DataTypes key_types = {std::make_shared<const DataTypeInt64>(), std::make_shared<const DataTypeInt64>()};

    /// A: k1 in [1,10], k2 in [5,6]      B: k1 in [2,3], k2 in [1,2]
    Columns lower = makeColumns(key_types, {{Field(1), Field(2)}, {Field(5), Field(1)}});
    Columns upper = makeColumns(key_types, {{Field(10), Field(3)}, {Field(6), Field(2)}});

    auto result = buildKeyRangeSet(lower, upper, identityMapping(2));
    ASSERT_EQ(result.outcome, KeyRangeSetOutcome::Built);
    ASSERT_EQ(result.set->size(), 2u) << "boxes are not merged - their union is not a box";
    ASSERT_FALSE(result.set->isDisjoint()) << "A and B overlap as lexicographic spans";

    auto mayMatch = [&](Int64 k1, Int64 k2)
    {
        Ranges ranges = {Range(k1), Range(k2)};
        return result.set->checkInRange(ranges, key_types).can_be_true;
    };

    ASSERT_TRUE(mayMatch(2, 1)) << "(2,1) is in B";
    ASSERT_TRUE(mayMatch(2, 5)) << "(2,5) is in A";
    ASSERT_FALSE(mayMatch(0, 0)) << "(0,0) is below every entry";
    ASSERT_FALSE(mayMatch(11, 0)) << "(11,0) is above every entry";

    /// Documented over-approximation: an entry is tested through its lexicographic span, which
    /// contains the box, so a point in the span but outside every box answers "may match".
    ASSERT_TRUE(mayMatch(2, 4)) << "(2,4) is in neither box but lies in A's span";
}

/// Equality on the leading column keeps entries disjoint, which is what `contains` needs.
TEST(MergeTreeKeyRangeSet, disjointnessIsRecorded)
{
    DataTypes bound_types = {std::make_shared<const DataTypeInt64>(), std::make_shared<const DataTypeInt64>()};

    auto [lower, upper] = makeCorners(bound_types, {{Field(1), Field(7)}, {Field(3), Field(9)}}, 0);
    auto result = buildKeyRangeSet(lower, upper, identityMapping(1));
    ASSERT_TRUE(result.set->isDisjoint()) << "[1,3] and [7,9] do not overlap";

    /// Overlapping single-column entries are merged, so the result is disjoint again.
    auto [lo2, hi2] = makeCorners(bound_types, {{Field(1), Field(2)}, {Field(5), Field(9)}}, 0);
    auto merged = buildKeyRangeSet(lo2, hi2, identityMapping(1));
    ASSERT_EQ(merged.set->size(), 1u);
    ASSERT_TRUE(merged.set->isDisjoint());
}
