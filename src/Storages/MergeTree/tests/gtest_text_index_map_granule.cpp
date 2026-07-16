#include <gtest/gtest.h>
#include <Storages/MergeTree/TextIndexMapGranule.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>

using namespace DB;

namespace
{
String key(const String & k) { return String(1, MAP_KEY_NAMESPACE) + k; }
String val(const String & v) { return String(1, MAP_VALUE_NAMESPACE) + v; }

const std::vector<UInt32> * find(const MapGranuleSlots & s, const String & token)
{
    for (const auto & [t, ids] : s.postings)
        if (t == token)
            return &ids;
    return nullptr;
}
}

TEST(TextIndexMapGranule, StrideIsMaxKeysPerGranule)
{
    MapGranuleEntries g = {
        {{"a", {"1"}}, {"b", {"2"}}},          // granule 0: 2 keys
        {{"a", {"3"}}, {"b", {"4"}}, {"c", {"5"}}}, // granule 1: 3 keys
    };
    auto s = computeMapGranuleSlots(g);
    EXPECT_EQ(s.stride, 3u);
}

TEST(TextIndexMapGranule, KeyAndItsValueShareTheSameSlot)
{
    MapGranuleEntries g = {
        {{"color", {"red"}}, {"size", {"big"}}},
    };
    auto s = computeMapGranuleSlots(g);
    const auto * kslots = find(s, key("color"));
    const auto * vslots = find(s, val("red"));
    ASSERT_NE(kslots, nullptr);
    ASSERT_NE(vslots, nullptr);
    ASSERT_EQ(kslots->size(), 1u);
    ASSERT_EQ(vslots->size(), 1u);
    EXPECT_EQ((*kslots)[0], (*vslots)[0]);
    // color's value red maps to color's slot, not size's slot.
    const auto * size_slots = find(s, key("size"));
    ASSERT_NE(size_slots, nullptr);
    EXPECT_NE((*vslots)[0], (*size_slots)[0]);
}

TEST(TextIndexMapGranule, SlotEncodesGranuleViaStride)
{
    MapGranuleEntries g = {
        {{"a", {"x"}}, {"b", {"y"}}},
        {{"a", {"z"}}},
    };
    auto s = computeMapGranuleSlots(g);
    const auto * a = find(s, key("a"));
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->size(), 2u); // 'a' appears in both granules
    EXPECT_EQ(granuleOfSlot((*a)[0], s.stride), 0u);
    EXPECT_EQ(granuleOfSlot((*a)[1], s.stride), 1u);
}

TEST(TextIndexMapGranule, ValueUnderTwoKeysGetsBothSlots)
{
    MapGranuleEntries g = {
        {{"a", {"shared"}}, {"b", {"shared"}}},
    };
    auto s = computeMapGranuleSlots(g);
    const auto * v = find(s, val("shared"));
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->size(), 2u); // shared is a value of both a and b
}

TEST(TextIndexMapGranule, FrequentKeyGetsStableLowSlot)
{
    // 'a' appears in all 3 granules (most frequent) -> rank 0 everywhere.
    MapGranuleEntries g = {
        {{"a", {"1"}}, {"z", {"2"}}},
        {{"a", {"3"}}, {"y", {"4"}}},
        {{"a", {"5"}}},
    };
    auto s = computeMapGranuleSlots(g);
    const auto * a = find(s, key("a"));
    ASSERT_NE(a, nullptr);
    for (UInt32 kid : *a)
        EXPECT_EQ(kid % s.stride, 0u); // 'a' always local slot 0
}
