# Granule-level Deduplicated `Map` Text Index — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second `Map` text-index mode that prunes granules on exact key/value co-occurrence using a per-granule key-slot space, deduplicating repeated entries, with no direct read.

**Architecture:** A sibling to the existing element-level map mode. A new index argument `map_mode='granule'` selects it. Build accumulates distinct keys and per-key distinct values per index-granule, assigns each distinct key a slot (`kid = g*R + local_slot`, `R = max distinct-keys-per-granule`), and stores key/value slot postings. Queries intersect key and value postings in slot space; `kid / R` yields the granule. Merges rebuild from the `Map` column. The novel slot-assignment algorithm lives in a standalone, unit-tested unit; everything else mirrors the element mode.

**Tech Stack:** C++ (ClickHouse), MergeTree secondary indexes, Roaring posting lists, gtest (`unit_tests_dbms`), stateless SQL tests (`tests/queries/0_stateless`).

## Global Constraints

- Allman braces (opening brace on its own line) — enforced by CI style check.
- No `sleep` in C++ to handle races.
- Say "exception" not "crash" in comments/messages.
- Wrap SQL/class/function names in backticks in comments and commit messages; write a function as `f`, not `f()`.
- Build only via `clickhouse-agent build agent-01 dev <target>`, output redirected to a log in the build dir, analyzed by a subagent. Do not pass `-j`/`nproc` to ninja.
- Run tests with output redirected to a uniquely named log in the build dir; analyze via subagent.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>` (`.sql`) or `add-test <name>.sh`. Do not add `no-*` tags unless strictly necessary. Prefer a new test over extending an existing one.
- Feature is gated by the existing `allow_experimental_full_text_index = 1` setting (same as all text-index work).
- The value type of an indexed `Map` is `String` or `FixedString`; its default is the empty string / all-zero bytes respectively.

**Planning-time decision (not in the spec, flagged for review):** mode selection is a new text-index argument `map_mode` with values `'element'` (default, preserves the already-committed element-mode behavior) and `'granule'`. On a non-`Map` column `map_mode` is rejected.

---

## File Structure

- **Create** `src/Storages/MergeTree/TextIndexMapGranule.h` / `.cpp` — the pure slot-assignment algorithm (`computeMapGranuleSlots`) and the `granuleOfSlot` helper. One responsibility: turn per-granule key/value sets into slot postings. No MergeTree dependencies beyond basic types, so it is unit-testable in isolation.
- **Create** `src/Storages/MergeTree/tests/gtest_text_index_map_granule.cpp` — gtest for the algorithm.
- **Modify** `src/Storages/MergeTree/MergeTreeIndexText.h` — add `map_element_granule` to `MergeTreeIndexTextParams`; add `Version::WithMapElementGranule`; add `map_element_granule`/`map_key_stride` to `TextIndexHeader`; declare `addMapGranuleDocuments`, `assignGranuleKeySlots`, `hasMapEntryGranule`.
- **Modify** `src/Storages/MergeTree/MergeTreeIndexText.cpp` — arg parsing (`map_mode`), validator, aggregator build path, dump-time slot assignment, header serialize/deserialize, granule `hasMapEntryGranule`.
- **Modify** `src/Storages/MergeTree/MergeTreeIndexConditionText.h` / `.cpp` — new RPN atoms `FUNCTION_MAP_KEY_VALUE_IN`, `FUNCTION_MAP_HAS_KEY`, `FUNCTION_MAP_HAS_VALUE`; parse the four query forms; default-value carve-out; `mayBeTrueOnGranule` for the granule mode.
- **Modify** `src/Storages/MergeTree/TextIndexUtils.cpp` — route `MergeTextIndexesTask` to rebuild-from-column when the index is granule mode.
- **Create** stateless tests under `tests/queries/0_stateless/` (numbers assigned by `add-test`).
- **Modify** `docs/en/engines/table-engines/mergetree-family/invertedindexes.md` (or the current text-index doc) — document `map_mode='granule'`.

---

## Task 1: Mode selection, validator, and header version

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeIndexText.h` (params + header struct + Version enum)
- Modify: `src/Storages/MergeTree/MergeTreeIndexText.cpp` (`textIndexCreator`, `textIndexValidator`, `serializeHeader`, `deserializeHeaderPrefix`)
- Test: `tests/queries/0_stateless/` new `.sql` (create/validate)

**Interfaces:**
- Produces: `MergeTreeIndexTextParams::map_element_granule` (bool); `TextIndexHeader::Version::WithMapElementGranule = 4`; `TextIndexHeader::map_element_granule` (bool) and `TextIndexHeader::map_key_stride` (UInt64, this is `R`); `serializeHeader`/`deserializeHeaderPrefix` round-trip these two fields when `version >= WithMapElementGranule`.

- [ ] **Step 1: Write the failing stateless test**

Run `./tests/queries/0_stateless/add-test 04500_text_index_map_granule_create.sql` then write:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;

DROP TABLE IF EXISTS t_mg_create;

-- granule mode accepted on Map(String,String)
CREATE TABLE t_mg_create
(
    id UInt64,
    m Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id;
SELECT 'created';

-- granule mode rejected on a non-Map column
CREATE TABLE t_mg_bad
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id; -- { serverError BAD_ARGUMENTS }

-- unknown map_mode rejected
CREATE TABLE t_mg_bad2
(
    id UInt64,
    m Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'nope') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id; -- { serverError BAD_ARGUMENTS }

DROP TABLE t_mg_create;
```

Reference file (`.reference`): a single line `created`.

- [ ] **Step 2: Add the params field and header version/fields**

In `MergeTreeIndexText.h`, extend `MergeTreeIndexTextParams` (after `bool map_element = false;`):

```cpp
    /// When set, the index is built over a `Map` column in granule mode: each distinct key in an
    /// index-granule gets one slot, values are assigned their key's slot, and the index prunes on
    /// exact key/value co-occurrence at granule granularity (no direct read).
    bool map_element_granule = false;
```

In `TextIndexHeader::Version`, add `WithMapElementGranule = 4,`. In `TextIndexHeader`, after `map_stride`:

```cpp
    /// Persisted for version >= WithMapElementGranule.
    bool map_element_granule = false;
    /// Persisted for version >= WithMapElementGranule. Fixed per-granule slot stride: kid = g*R + slot.
    UInt64 map_key_stride = 0;
```

- [ ] **Step 3: Parse `map_mode` and set the flag in `textIndexCreator`**

In `MergeTreeIndexText.cpp`, `textIndexCreator`, where `bool map_element = ...isMap()` is computed, replace with mode parsing (mirror how `tokenizer`/other named args are read from the index arguments):

```cpp
    const bool is_map = !index.data_types.empty() && WhichDataType(index.data_types[0]).isMap();

    /// map_mode: 'element' (default) or 'granule'. Only valid on a Map column.
    String map_mode = "element";
    if (const auto * arg = index.arguments.tryGet("map_mode"))
        map_mode = arg->safeGet<String>();

    bool map_element = is_map && map_mode == "element";
    bool map_element_granule = is_map && map_mode == "granule";
```

(If the index-argument accessor differs, use the same accessor the surrounding code uses for `tokenizer`; the semantics are "read optional named string argument `map_mode`".)

Add `map_element_granule` to the `MergeTreeIndexTextParams` aggregate initialization next to `map_element`.

- [ ] **Step 4: Validate in `textIndexValidator`**

In `textIndexValidator`, extend the `Map` branch: the existing code already requires String/FixedString key and value for a `Map`. Add mode checks:

```cpp
    String map_mode = "element";
    if (const auto * arg = index.arguments.tryGet("map_mode"))
        map_mode = arg->safeGet<String>();

    if (map_mode != "element" && map_mode != "granule")
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unknown map_mode '{}' for text index, expected 'element' or 'granule'", map_mode);

    if (map_mode != "element" && !WhichDataType(index_data_type).isMap())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "map_mode is only valid for a text index on a Map column");
```

- [ ] **Step 5: Serialize/deserialize the new header fields**

In `serializeHeader`, add a `map_element_granule`/`map_key_stride` parameter pair (after the `map_element`/`map_stride` block) and write them under the new version:

```cpp
    if (version >= static_cast<MergeTreeIndexVersion>(TextIndexHeader::Version::WithMapElementGranule))
    {
        writeVarUInt(static_cast<UInt64>(map_element_granule), ostr);
        writeVarUInt(map_key_stride, ostr);
    }
```

In `deserializeHeaderPrefix`, bump the guard to `version > WithMapElementGranule` and read them:

```cpp
    if (version >= static_cast<UInt64>(TextIndexHeader::Version::WithMapElementGranule))
    {
        UInt64 mg = 0;
        readVarUInt(mg, istr);
        header.map_element_granule = mg != 0;
        readVarUInt(header.map_key_stride, istr);
    }
```

Update all `serializeHeader` call sites to pass the two new arguments (`params.map_element_granule`, `map_key_stride`; pass `false, 0` where not applicable). The version chosen at write time becomes `WithMapElementGranule` when `params.map_element_granule`, else the existing selection.

- [ ] **Step 6: Build**

Run: `clickhouse-agent build agent-01 dev clickhouse > /home/unamed/lab/clickhouse-agents/builds/agent-01/dev/build_t1.log 2>&1`
Expected: exit 0, `clickhouse` linked. Analyze the log via a subagent.

- [ ] **Step 7: Run the stateless test**

Run: `tests/clickhouse-test --client-option allow_experimental_full_text_index=1 04500_text_index_map_granule_create > /home/unamed/lab/clickhouse-agents/builds/agent-01/dev/test_t1.log 2>&1` (use the agent's server; or run the `.sql` via `clickhouse local` as in the smoke tests). Expected: PASS. Analyze via subagent.

- [ ] **Step 8: Commit**

```bash
git add src/Storages/MergeTree/MergeTreeIndexText.h src/Storages/MergeTree/MergeTreeIndexText.cpp tests/queries/0_stateless/04500_text_index_map_granule_create.*
git commit -m "Add granule map_mode selection, validator and header version for Map text index"
```

---

## Task 2: The slot-assignment algorithm (pure, unit-tested)

**Files:**
- Create: `src/Storages/MergeTree/TextIndexMapGranule.h`
- Create: `src/Storages/MergeTree/TextIndexMapGranule.cpp`
- Test: `src/Storages/MergeTree/tests/gtest_text_index_map_granule.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace DB
  {
  /// Per index-granule: distinct keys, each mapped to its distinct values (all un-namespaced).
  using MapGranuleEntries = std::vector<std::vector<std::pair<String, std::vector<String>>>>;

  struct MapGranuleSlots
  {
      UInt64 stride = 0; /// R = max distinct-keys-per-granule
      /// Namespaced token (\x01+key or \x02+value) -> ascending, de-duplicated slot ids.
      std::vector<std::pair<String, std::vector<UInt32>>> postings;
  };

  /// Assigns each distinct key in each granule a local slot (frequency-positional: keys ranked by
  /// the number of granules they appear in, descending, ties by key ascending), computes the
  /// stride R = max keys-per-granule, and emits key and value slot postings. Throws
  /// SUPPORT_IS_DISABLED if granules * R exceeds UInt32::max.
  MapGranuleSlots computeMapGranuleSlots(const MapGranuleEntries & granules);

  /// granule index of a slot id given the stride.
  inline UInt64 granuleOfSlot(UInt32 kid, UInt64 stride) { return stride ? kid / stride : 0; }
  }
  ```
- Consumes: `MAP_KEY_NAMESPACE`, `MAP_VALUE_NAMESPACE` from `MergeTreeIndexText.h`.

- [ ] **Step 1: Write the failing gtest**

Create `src/Storages/MergeTree/tests/gtest_text_index_map_granule.cpp`:

```cpp
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
```

Add the gtest source to the unit-tests target if the build uses an explicit list; ClickHouse's `src/Storages/MergeTree/tests/` gtests are usually globbed into `unit_tests_dbms` automatically — verify by building the target.

- [ ] **Step 2: Build the unit tests target to confirm the test fails to link**

Run: `clickhouse-agent build agent-01 dev unit_tests_dbms > /home/unamed/lab/clickhouse-agents/builds/agent-01/dev/build_t2_fail.log 2>&1`
Expected: FAIL — undefined reference to `computeMapGranuleSlots` (header/impl not yet created). Analyze via subagent.

- [ ] **Step 3: Create the header `TextIndexMapGranule.h`**

```cpp
#pragma once

#include <base/types.h>
#include <vector>
#include <utility>

namespace DB
{

using MapGranuleEntries = std::vector<std::vector<std::pair<String, std::vector<String>>>>;

struct MapGranuleSlots
{
    UInt64 stride = 0;
    std::vector<std::pair<String, std::vector<UInt32>>> postings;
};

MapGranuleSlots computeMapGranuleSlots(const MapGranuleEntries & granules);

inline UInt64 granuleOfSlot(UInt32 kid, UInt64 stride)
{
    return stride ? kid / stride : 0;
}

}
```

- [ ] **Step 4: Implement `TextIndexMapGranule.cpp`**

```cpp
#include <Storages/MergeTree/TextIndexMapGranule.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Common/Exception.h>

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace DB
{

namespace ErrorCodes
{
    extern const int SUPPORT_IS_DISABLED;
}

MapGranuleSlots computeMapGranuleSlots(const MapGranuleEntries & granules)
{
    MapGranuleSlots result;

    /// Global key frequency = number of granules a key appears in.
    std::unordered_map<std::string_view, size_t> key_frequency;
    UInt64 stride = 0;
    for (const auto & granule : granules)
    {
        stride = std::max<UInt64>(stride, granule.size());
        for (const auto & [key, values] : granule)
            ++key_frequency[key];
    }
    result.stride = std::max<UInt64>(stride, 1);

    const UInt64 total_slots = static_cast<UInt64>(granules.size()) * result.stride;
    if (total_slots > std::numeric_limits<UInt32>::max())
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
            "Cannot build granule map text index: granules * stride ({}) exceeds the maximum slot id {}",
            total_slots, std::numeric_limits<UInt32>::max());

    /// token -> collected slot ids (may receive duplicates across values/granules; sorted+uniqued at the end).
    std::unordered_map<std::string, std::vector<UInt32>> postings;

    for (size_t g = 0; g < granules.size(); ++g)
    {
        /// Rank this granule's keys by (global frequency desc, key asc) -> local slot.
        std::vector<size_t> order(granules[g].size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::ranges::sort(order, [&](size_t lhs, size_t rhs)
        {
            const auto & kl = granules[g][lhs].first;
            const auto & kr = granules[g][rhs].first;
            const size_t fl = key_frequency[kl];
            const size_t fr = key_frequency[kr];
            return fl != fr ? fl > fr : kl < kr;
        });

        for (UInt32 slot = 0; slot < order.size(); ++slot)
        {
            const auto & [key, values] = granules[g][order[slot]];
            const UInt32 kid = static_cast<UInt32>(g * result.stride + slot);

            postings[String(1, MAP_KEY_NAMESPACE) + key].push_back(kid);
            for (const auto & value : values)
                postings[String(1, MAP_VALUE_NAMESPACE) + value].push_back(kid);
        }
    }

    result.postings.reserve(postings.size());
    for (auto & [token, ids] : postings)
    {
        std::ranges::sort(ids);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        result.postings.emplace_back(token, std::move(ids));
    }
    /// Deterministic order for reproducible dictionaries.
    std::ranges::sort(result.postings, [](const auto & a, const auto & b) { return a.first < b.first; });

    return result;
}

}
```

- [ ] **Step 5: Build the unit tests target**

Run: `clickhouse-agent build agent-01 dev unit_tests_dbms > /home/unamed/lab/clickhouse-agents/builds/agent-01/dev/build_t2.log 2>&1`
Expected: exit 0. Analyze via subagent.

- [ ] **Step 6: Run the gtest**

Run: `/home/unamed/lab/clickhouse-agents/builds/agent-01/dev/src/unit_tests_dbms --gtest_filter='TextIndexMapGranule.*' > /home/unamed/lab/clickhouse-agents/builds/agent-01/dev/test_t2.log 2>&1`
Expected: all 5 tests PASS. Analyze via subagent.

- [ ] **Step 7: Commit**

```bash
git add src/Storages/MergeTree/TextIndexMapGranule.h src/Storages/MergeTree/TextIndexMapGranule.cpp src/Storages/MergeTree/tests/gtest_text_index_map_granule.cpp
git commit -m "Add pure slot-assignment algorithm for granule Map text index with gtest"
```

---

## Task 3: Build path — granule-aware accumulation and dump

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeIndexText.h` (declare `addMapGranuleDocuments`, `assignGranuleKeySlots`; granule builder state)
- Modify: `src/Storages/MergeTree/MergeTreeIndexText.cpp` (aggregator `update`, dump)
- Test: `tests/queries/0_stateless/` new `.sql`

**Interfaces:**
- Consumes: `computeMapGranuleSlots`, `MapGranuleEntries`, `MapGranuleSlots` (Task 2); `params.map_element_granule` (Task 1).
- Produces: an index part in granule mode whose header carries `map_element_granule=true` and `map_key_stride=R`, and whose postings are over slot ids. `MergeTreeIndexAggregatorText::addMapGranuleDocuments(const ColumnPtr & column, size_t start_row, size_t rows_read, size_t index_granularity_rows)` collects entries into the current granule; the granule boundary is every `index_granularity_rows` table rows.

- [ ] **Step 1: Write the failing stateless test**

`./tests/queries/0_stateless/add-test 04501_text_index_map_granule_build.sql`:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t_mg_build;
CREATE TABLE t_mg_build
(
    id UInt64,
    m Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 4;

INSERT INTO t_mg_build VALUES
    (1, {'color':'red','size':'big'}),
    (2, {'color':'red'}),                 -- duplicate pair color=red collapses within the granule
    (3, {'color':'blue','shape':'round'}),
    (4, {'color':'red'});

SELECT count() FROM t_mg_build;           -- 4, index build did not corrupt the part
SELECT count() FROM system.data_skipping_indices WHERE table = 't_mg_build' AND name = 'idx'; -- 1
DROP TABLE t_mg_build;
```

`.reference`:
```
4
1
```

- [ ] **Step 2: Add granule-builder state and method declarations**

In `MergeTreeIndexText.h`, in `MergeTreeIndexAggregatorText` (or the granule builder that owns `addMapDocuments`), add:

```cpp
    /// Granule map mode: entries of the granule currently being filled (distinct keys -> distinct values).
    MapGranuleEntries map_granule_entries;
    /// Rows consumed into the current (open) granule; flushed at index_granularity_rows.
    size_t map_granule_open_rows = 0;

    void addMapGranuleDocuments(const ColumnPtr & column, size_t start_row, size_t rows_read, size_t index_granularity_rows);
    void assignGranuleKeySlots(); /// dump-time: computeMapGranuleSlots + emit postings + set stride
```

Include `<Storages/MergeTree/TextIndexMapGranule.h>` in the `.h`.

- [ ] **Step 3: Route granule mode in the aggregator `update`**

In `MergeTreeIndexAggregatorText::update`, mirror the existing `if (params.map_element)` early-return block with a granule branch placed first:

```cpp
    if (params.map_element_granule)
    {
        const size_t rows_read = std::min(limit, block.rows() - *pos);
        if (rows_read == 0)
            return;
        const auto & index_column = block.getByName(index_column_name);
        addMapGranuleDocuments(index_column.column, *pos, rows_read, index_granularity_rows);
        *pos += rows_read;
        return;
    }
```

`index_granularity_rows` is the index's granularity in rows (the same value the writer uses to cut granules). Use the aggregator's existing granularity field; if none is directly available, thread it from the index descriptor at aggregator construction.

- [ ] **Step 4: Implement `addMapGranuleDocuments`**

In `MergeTreeIndexText.cpp`, mirroring `addMapDocuments` but collapsing to per-granule distinct sets:

```cpp
void MergeTreeIndexAggregatorText::addMapGranuleDocuments(
    const ColumnPtr & column, size_t start_row, size_t rows_read, size_t index_granularity_rows)
{
    const auto full_column = column->convertToFullColumnIfConst();
    const auto & column_map = assert_cast<const ColumnMap &>(*full_column);
    const auto & column_array = column_map.getNestedColumn();
    const IColumn::Offsets & offsets = column_array.getOffsets();
    const auto & tuple = column_map.getNestedData();
    const IColumn & keys = tuple.getColumn(0);
    const IColumn & values = tuple.getColumn(1);

    auto open_new_granule = [&]() { map_granule_entries.emplace_back(); map_granule_open_rows = 0; };
    if (map_granule_entries.empty())
        open_new_granule();

    for (size_t row = start_row; row < start_row + rows_read; ++row)
    {
        if (map_granule_open_rows == index_granularity_rows)
            open_new_granule();

        auto & granule = map_granule_entries.back();
        const size_t row_begin = offsets[row - 1];
        const size_t row_end = offsets[row];
        for (size_t e = row_begin; e < row_end; ++e)
        {
            const String key = keys.getDataAt(e).toString();
            const String value = values.getDataAt(e).toString();

            /// find-or-add the key in this granule (small K; linear scan is fine), dedup values.
            auto it = std::ranges::find_if(granule, [&](const auto & kv) { return kv.first == key; });
            if (it == granule.end())
                granule.push_back({key, {value}});
            else if (std::ranges::find(it->second, value) == it->second.end())
                it->second.push_back(value);
        }
        ++map_granule_open_rows;
    }
}
```

- [ ] **Step 5: Implement `assignGranuleKeySlots` and wire it into the dump**

```cpp
void MergeTreeIndexAggregatorText::assignGranuleKeySlots()
{
    const MapGranuleSlots slots = computeMapGranuleSlots(map_granule_entries);
    granule_builder.map_key_stride = slots.stride; // stored into the header at serialize time

    for (const auto & [token, ids] : slots.postings)
        for (UInt32 kid : ids)
            granule_builder.addTokenSlot(token, kid); // adds token->kid into tokens_map postings

    map_granule_entries.clear();
    map_granule_open_rows = 0;
}
```

Add a small helper `MergeTreeIndexTextGranuleBuilder::addTokenSlot(std::string_view token, UInt32 slot)` that inserts `slot` into the posting for `token` (reuse the existing `addToken` insertion path but with an explicit id rather than `current_row`; the existing builder already maps token -> `PostingListBuilder`). Call `assignGranuleKeySlots` at the point the element mode calls `reassignMapElementIds` (just before producing the writable granule), guarded by `params.map_element_granule`.

At `serializeBinaryWithMultipleStreams`, select `Version::WithMapElementGranule` when `params.map_element_granule` and pass `map_element_granule=true`, `map_key_stride` to `serializeHeader`.

- [ ] **Step 6: Build**

Run: `clickhouse-agent build agent-01 dev clickhouse > .../build_t3.log 2>&1`
Expected: exit 0. Analyze via subagent.

- [ ] **Step 7: Run the test**

Run the stateless test as in Task 1 Step 7 with name `04501_text_index_map_granule_build`. Expected: PASS. Analyze via subagent.

- [ ] **Step 8: Commit**

```bash
git add src/Storages/MergeTree/MergeTreeIndexText.h src/Storages/MergeTree/MergeTreeIndexText.cpp tests/queries/0_stateless/04501_text_index_map_granule_build.*
git commit -m "Build granule Map text index: per-granule accumulation and slot dump"
```

---

## Task 4: Query — RPN atoms, parsing, carve-out, granule pruning

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeIndexConditionText.h` (RPN enum, `map_element_granule` flag)
- Modify: `src/Storages/MergeTree/MergeTreeIndexConditionText.cpp` (`traverseFunctionNode`, `alwaysUnknownOrTrue`, `requiresReadingAllTokens`, `mayBeTrueOnGranule`)
- Modify: `src/Storages/MergeTree/MergeTreeIndexText.cpp` (`hasMapEntryGranule`)
- Modify: `src/Storages/MergeTree/MergeTreeIndexText.h` (declare `hasMapEntryGranule`)
- Test: four new `.sql` stateless tests

**Interfaces:**
- Consumes: header `map_element_granule`/`map_key_stride`; `granuleOfSlot`.
- Produces: RPN atoms `FUNCTION_MAP_KEY_VALUE_EQUALS` (reused), `FUNCTION_MAP_KEY_VALUE_IN`, `FUNCTION_MAP_HAS_KEY`, `FUNCTION_MAP_HAS_VALUE`; `MergeTreeIndexGranuleText::hasMapEntryGranule(const TextSearchQuery & key_query, const TextSearchQuery & value_query_or_empty)` returning whether the current index-granule may contain a match.

- [ ] **Step 1: Write the four failing stateless tests**

Create with `add-test` (numbers auto-assigned; names below):

`04502_text_index_map_granule_equals.sql` — equality + index-on==off cross-check + co-occurrence trap:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red','size':'big'}),(2,{'color':'blue'}),(3,{'shape':'round','color':'red'}),(4,{'size':'red'});
-- co-occurrence trap: row 4 has value 'red' but under key 'size', not 'color'
SELECT id FROM t WHERE m['color']='red' ORDER BY id;                          -- 1,3
SELECT id FROM t WHERE m['color']='red' SETTINGS force_data_skipping_indices='idx' ORDER BY id; -- 1,3
DROP TABLE t;
```
`.reference`:
```
1
3
1
3
```

`04503_text_index_map_granule_default_value.sql` — the false-negative guard:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red'}),(2,{'size':'big'}),(3,{'color':''}),(4,{'shape':'x'});
-- m['color']='' is TRUE for rows lacking 'color' (2,4) AND the explicit '' (3). Index must NOT prune.
SELECT id FROM t WHERE m['color']='' ORDER BY id;   -- 2,3,4
DROP TABLE t;
```
`.reference`:
```
2
3
4
```

`04504_text_index_map_granule_key_value_only.sql` — `mapContains`, value-only, `IN`:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red'}),(2,{'size':'big'}),(3,{'color':'blue','shape':'round'});
SELECT id FROM t WHERE mapContains(m,'shape') ORDER BY id;              -- 3
SELECT id FROM t WHERE has(mapValues(m),'big') ORDER BY id;            -- 2
SELECT id FROM t WHERE m['color'] IN ('red','blue') ORDER BY id;       -- 1,3
DROP TABLE t;
```
`.reference`:
```
3
2
1
3
```

`04505_text_index_map_granule_prune.sql` — EXPLAIN granule reduction:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=2;
INSERT INTO t SELECT number, map('k', toString(number)) FROM numbers(8);
SELECT trimLeft(explain) FROM (EXPLAIN indexes=1 SELECT count() FROM t WHERE m['k']='5')
WHERE explain LIKE '%Granules:%';
DROP TABLE t;
```
`.reference` (exact granule counts depend on layout; capture after implementation — expect a reduced selected count such as `Granules: 1/4`).

- [ ] **Step 2: Add RPN atoms and the condition flag**

In `MergeTreeIndexConditionText.h` `enum Function`, after `FUNCTION_MAP_KEY_VALUE_EQUALS`:

```cpp
            /// `m[k] IN (...)` on a granule map index: key slot AND union of value slots.
            FUNCTION_MAP_KEY_VALUE_IN,
            /// `mapContains(m,k)`: key token presence only.
            FUNCTION_MAP_HAS_KEY,
            /// `has(mapValues(m),v)`: value token presence only.
            FUNCTION_MAP_HAS_VALUE,
```

Add `bool map_element_granule = false;` next to the existing `bool map_element`, set from the index params in the condition constructor (thread it exactly like `map_element` was threaded in the port).

- [ ] **Step 3: Parse the four forms in `traverseFunctionNode`**

Add a granule branch, mirroring the existing `map_element` `equals` branch but emitting the new atoms and applying the default-value carve-out. Sketch:

```cpp
    if (map_element_granule)
    {
        // Resolve m['k'] from subcolumn m.key_<k> or arrayElement(m,'k') exactly as the element branch does.
        // For equals: if value is the type default ("" for String / zero FixedString), do NOT emit a prunable
        // atom -> return false here so the atom becomes UNKNOWN (alwaysUnknownOrTrue keeps the granule).
        if (function_name == "equals" && value_field.getType() == Field::Types::String)
        {
            if (value_field.safeGet<String>().empty())
                return false; // default value -> non-pruning (fail-open)
            // emit FUNCTION_MAP_KEY_VALUE_EQUALS with All-mode query over {\x01+key, \x02+value}
            ...
            return true;
        }
        // function_name == "in": if ANY listed value is the default -> return false (non-pruning).
        // else emit FUNCTION_MAP_KEY_VALUE_IN carrying the key token and the list of value tokens.
        // mapContains -> FUNCTION_MAP_HAS_KEY with {\x01+key}. has(mapValues,v) -> FUNCTION_MAP_HAS_VALUE with {\x02+v}.
    }
```

Reuse the `tryParseMapSubcolumnName` / `arrayElement` extraction already present from the port. FixedString default = a string of zero bytes of the fixed length; treat "all-zero or empty" as default.

- [ ] **Step 4: Register the atoms in `alwaysUnknownOrTrue` and `requiresReadingAllTokens`**

Add `FUNCTION_MAP_KEY_VALUE_IN`, `FUNCTION_MAP_HAS_KEY`, `FUNCTION_MAP_HAS_VALUE` to the same switch/lists where `FUNCTION_MAP_KEY_VALUE_EQUALS` already appears (so they are recognized as supported, prunable atoms).

- [ ] **Step 5: Implement `hasMapEntryGranule` and dispatch in `mayBeTrueOnGranule`**

In `MergeTreeIndexText.cpp`, add a granule-mode evaluator that intersects/unions token postings and checks whether any resulting slot maps to the current granule index. Mirror `hasMapEntry` but replace per-mark element-range intersection with a granule-index match:

```cpp
bool MergeTreeIndexGranuleText::hasMapEntryGranule(const std::vector<TextSearchQuery> & and_queries,
                                                   const std::vector<TextSearchQuery> & or_value_queries) const
{
    // Build the AND of and_queries' postings (key + optional single value), then, if or_value_queries
    // is non-empty (IN / value-only), OR their postings and AND with the key postings.
    // For each resulting slot id, granuleOfSlot(kid, header.map_key_stride) == this granule's index -> true.
    // Conservative keeps (bypassed / postings not materialized) return true, exactly like hasMapEntry.
}
```

In `MergeTreeIndexConditionText::mayBeTrueOnGranule`, add cases for the three new atoms plus the reused equals atom, calling `hasMapEntryGranule` when `map_element_granule`. The existing `map_element` path stays untouched.

- [ ] **Step 6: Build, then run all four tests**

Run: `clickhouse-agent build agent-01 dev clickhouse > .../build_t4.log 2>&1` (exit 0), then run `04502`–`04505` (analyze each log via subagent). Capture the actual `Granules:` line into `04505`'s `.reference`.

- [ ] **Step 7: Commit**

```bash
git add src/Storages/MergeTree/MergeTreeIndexConditionText.h src/Storages/MergeTree/MergeTreeIndexConditionText.cpp src/Storages/MergeTree/MergeTreeIndexText.h src/Storages/MergeTree/MergeTreeIndexText.cpp tests/queries/0_stateless/0450{2,3,4,5}_text_index_map_granule_*.*
git commit -m "Query granule Map text index: RPN atoms, default-value carve-out, granule pruning"
```

---

## Task 5: Merge — rebuild from the Map column

**Files:**
- Modify: `src/Storages/MergeTree/TextIndexUtils.cpp` (`MergeTextIndexesTask`)
- Test: `tests/queries/0_stateless/` new `.sql`

**Interfaces:**
- Consumes: the granule build path (Task 3), the header flag (Task 1).
- Produces: after any merge, a granule-mode index part equivalent to a fresh build over the merged data.

- [ ] **Step 1: Write the failing stateless test**

`04506_text_index_map_granule_merge.sql`:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red'}),(2,{'color':'blue'});
INSERT INTO t VALUES (3,{'color':'red','size':'big'}),(4,{'shape':'round'});
OPTIMIZE TABLE t FINAL;
SELECT count() FROM system.parts WHERE table='t' AND active; -- 1
SELECT id FROM t WHERE m['color']='red' ORDER BY id;         -- 1,3
SELECT id FROM t WHERE m['color']='red' SETTINGS force_data_skipping_indices='idx' ORDER BY id; -- 1,3
DROP TABLE t;
```
`.reference`:
```
1
1
3
1
3
```

- [ ] **Step 2: Route granule mode to rebuild in `MergeTextIndexesTask`**

In `TextIndexUtils.cpp`, at the point the task decides how to combine source indexes, add: if the index is granule mode (from the index params / header), do **not** merge postings; instead run the normal build aggregator over the merged `Map` column for the new part (the same code the writer uses on insert). Concretely, when `map_element_granule`, the task's `prepare`/`execute` produces the index by feeding the merged part's `m` column through a `MergeTreeIndexAggregatorText` in granule mode, exactly as a fresh part build, and writing the result. No `adjustMapElementPostings`/rerank path is taken.

Because the aggregator already reads from a block stream, reuse the merged-part reader that the element mode's merge uses, but skip the posting-remap branch entirely for granule mode.

- [ ] **Step 3: Build, then run the test**

Run: `clickhouse-agent build agent-01 dev clickhouse > .../build_t5.log 2>&1` (exit 0), then run `04506` (analyze via subagent). Expected: 1 active part; correct results both with and without `force_data_skipping_indices`.

- [ ] **Step 4: Commit**

```bash
git add src/Storages/MergeTree/TextIndexUtils.cpp tests/queries/0_stateless/04506_text_index_map_granule_merge.*
git commit -m "Merge granule Map text index by rebuilding from the Map column"
```

---

## Task 6: Edge cases, size check, and docs

**Files:**
- Test: `tests/queries/0_stateless/` new `.sql` (FixedString, empty maps, IN-with-default)
- Modify: the current text-index documentation page under `docs/`

**Interfaces:** none new.

- [ ] **Step 1: Write the edge-case test**

`04507_text_index_map_granule_edge_cases.sql`:

```sql
-- Tags: no-fasttest
SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(FixedString(3), String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'abc':'red'}),(2,{}),(3,{'abc':'blue'});   -- empty map row
SELECT id FROM t WHERE m['abc']='blue' ORDER BY id;                 -- 3
-- IN list containing the default value '' -> whole atom non-pruning, still correct
SELECT id FROM t WHERE m['abc'] IN ('blue','') ORDER BY id;         -- 2 (default via empty map),3
DROP TABLE t;
```
`.reference`:
```
3
2
3
```

- [ ] **Step 2: Build (if not current) and run the edge-case test**

Run `04507`; analyze via subagent. Expected: PASS. (Verify empty-map rows do not create spurious keys and that `''` in the `IN` list disables pruning.)

- [ ] **Step 3: Manual size check (informational, not a gate)**

Load a repetitive log-like dataset into three tables — granule mode, element mode, and the concat workaround — and compare `system.data_skipping_indices` / part index file sizes. Record the ratio in the commit message. This confirms the design's size premise; it is not a pass/fail test.

- [ ] **Step 4: Document `map_mode='granule'`**

In the text-index doc page, add a subsection (with an explicit `{#...}` anchor per repo convention) describing: what granule mode is, that it prunes at granule granularity with no direct read, the `m[k]=v` / `IN` / `mapContains` / `mapValues` support, the default-value behavior (never prunes), and the size/precision trade-off versus element mode.

- [ ] **Step 5: Commit**

```bash
git add tests/queries/0_stateless/04507_text_index_map_granule_edge_cases.* docs/
git commit -m "Add edge-case tests and docs for granule Map text index"
```

---

## Self-Review

**Spec coverage:**
- Core mechanism / slots / stride → Task 2 (algorithm) + Task 3 (build).
- Storage layout / header version → Task 1 + Task 3.
- Build & merge (rebuild) → Task 3 + Task 5.
- Query (4 forms) → Task 4.
- Default-value carve-out → Task 4 (Steps 3, and test `04503`).
- Skip policy (always evaluate where safe, no selectivity bypass) → Task 4 Step 5 (no `text_index_hint_max_selectivity` call in the granule path).
- Id cap → Task 2 (`computeMapGranuleSlots` throws `SUPPORT_IS_DISABLED`).
- Testing (co-occurrence, prune, merge, FixedString, empty map, size) → Tasks 4–6.
- Mode selection (planning-time decision) → Task 1.

**Placeholder scan:** integration Steps in Tasks 3–5 give exact function names, signatures, insertion points, and code sketches that mirror named existing functions (`addMapDocuments`, `reassignMapElementIds`, `hasMapEntry`, `serializeHeader`); the novel algorithm (Task 2) and all tests are complete code. The only intentionally deferred literal is `04505`'s `.reference` granule count, which must be captured from the first passing run (noted in-step).

**Type consistency:** `map_element_granule` (params, header, condition), `map_key_stride` (header) / `stride` (algorithm), `computeMapGranuleSlots`, `MapGranuleEntries`, `MapGranuleSlots`, `granuleOfSlot`, `addMapGranuleDocuments`, `assignGranuleKeySlots`, `hasMapEntryGranule` are used consistently across tasks.
