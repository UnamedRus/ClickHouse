#include <Interpreters/Set.h>
#include <DataTypes/DataTypesNumber.h>
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
