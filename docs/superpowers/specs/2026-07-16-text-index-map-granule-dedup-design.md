---
description: 'Design for a granule-level, deduplicated text index on Map columns that prunes on exact key/value co-occurrence without direct read.'
sidebar_label: 'Map granule-dedup text index'
sidebar_position: 1
slug: /development/specs/text-index-map-granule-dedup
title: 'Granule-level deduplicated Map text index'
doc_type: 'guide'
---

# Granule-level deduplicated `Map` text index {#granule-dedup-map-text-index}

Status: **design approved**, not yet implemented. Sibling to the existing element-level exact
map mode (see `tmp/text-index-map-element-design.md`); this is the opposite trade-off.

Related: https://github.com/ClickHouse/ClickHouse/issues/110454

## Motivation {#motivation}

The current element-level map text index resolves `WHERE m['k'] = 'v'` exactly (row-precise,
direct read) but stores two postings per map **entry** (`≈ rows × arity`), making it
~1.5–3× the size of the concat workaround and requiring complex merge machinery (element-id
stride, dump-time reassign, per-merge arithmetic remap, `FINAL` frequency-rerank).

This design goes the other way: give up direct read and row-precision, accept **granule-level
accuracy**, and **deduplicate** repeated map entries down to distinct pairs *per granule*. For
repetitive maps (logs: small key set, values repeating across rows within a granule) this is
expected to be smaller than **both** the element-level design and the concat workaround, with
far less code. Goals, in priority order: shrink index size, enable key-only / value-only /
co-occurrence queries, and simplify the implementation.

## Core mechanism: per-granule key slots {#core-mechanism}

The index is a **skip index only** (`direct_read_mode = None`): it prunes granules and the
original predicate always re-filters survivors. It can never return a wrong row — only read a
superfluous granule.

Precision comes from a per-granule **slot** space that ties a value back to the specific key
it belongs to (which `mapKeys` + `mapValues` cannot do):

- Per index-granule, each **distinct key gets one slot**. Let `K_g` be the distinct-key count
  of granule `g`, and `R = max_g K_g` the stride over the part.
- Global slot id: `kid = g * R + local_slot`, so `g = kid / R` is pure integer arithmetic —
  no per-granule offset table, and no overflow because `R` is the observed maximum
  (`local_slot < K_g ≤ R`).
- Each **value is assigned the slot of the key it appears under** in that granule.

For `m['foo'] = 'bar'` the token query is `post(\x01foo) AND post(\x02bar)`. Within a granule
the key↔slot map is a bijection, so if a slot is owned by `foo` and also carries value `bar`,
that granule provably contains an entry `foo = bar`. A non-empty AND therefore means the
granule has the pair; an empty AND means it provably does not. This is **granule-exact**: no
false positives beyond granule granularity, and the single false-negative case (a default-value
needle) is handled by the carve-out in [Correctness](#default-value-carveout) below.

### Deduplication and size {#dedup-and-size}

Build accumulates distinct keys and per-key distinct values **per granule** with set
semantics, so row repetition and duplicate pairs collapse. Posting entries per token equal the
number of granules the token touches, not the number of rows. Repetitive maps shrink
dramatically; the structural cost is one extra key posting per distinct key per granule versus
the concat workaround's single `key=value` token.

## Storage layout {#storage-layout}

Reuses the existing text-index on-disk structure (dictionary, posting lists, sparse index,
header). Differences from the element-level map mode:

- **Dictionary** (`.dct`): one namespaced token per distinct key (`\x01` + key) and per
  distinct value (`\x02` + value). Front-coding compresses shared prefixes; key-only and
  value-only search reuse the same dictionary. `\x01` / `\x02` are the existing
  `MAP_KEY_NAMESPACE` / `MAP_VALUE_NAMESPACE` bytes.
- **Posting lists** (`.pst`): over the **slot-id space**, not rows/elements.
  - `\x01key` → the key's own slots (one per granule containing the key).
  - `\x02value` → the key-slots that value is assigned to.
- **Header**: new version `TextIndexHeader::Version::WithMapElementGranule` persisting `R` and
  the mode flag. `g = kid / R` needs nothing else. Older readers reject via the existing
  `version > max` guard.

Strictly less persisted state than the element mode: no per-row element-count table, no
row-level `map_stride`, no positions, no direct-read state.

## Build and merge {#build-and-merge}

One code path serves initial write and merge.

- `addMapDocuments` walks the `Map` column by row, tracking the current index-granule boundary
  (every `GRANULARITY` table-granules), maintaining the granule's distinct keys and each key's
  distinct values. Set semantics here are where deduplication happens. Whole key/value tokens
  only — no tokenizer or preprocessor.
- `assignGranuleKeySlots` (replaces the element mode's `reassignMapElementIds`) runs at
  dump time when the whole part is in RAM:
  1. compute `K_g` per granule and `R = max_g K_g`;
  2. local slot per key = rank of the key among *this granule's* keys by **global key
     frequency** (posting size), ties broken by token, so frequent keys land on stable low
     slots across granules and key postings compress well;
  3. emit `\x01key → g*R + local_slot` and, for each distinct `(key, value)` in the granule,
     `\x02value → g*R + local_slot(key)`.
- **Merge**: re-read the merged `Map` column and run the same path on the new granule layout.
  No posting remap, no rerank, no cross-part slot alignment. `MergeTextIndexesTask` for this
  mode collapses to "rebuild from column". Mutations / `DELETE` / TTL already rebuild.

Deleted relative to the element mode: `map_stride` over rows, per-row element counts,
`adjustMapElementPostings`, `executeRerankStep`, `rerankMapElement`, and all direct-read state.

### Element-id cap {#id-cap}

`granules * R ≤ UInt32::max`. Past it the build throws `SUPPORT_IS_DISABLED`, the same failure
model as today, but the ceiling (`granules × keys`) has vastly more headroom than the element
mode's `rows × arity`.

## Query {#query}

Predicate parsing in `MergeTreeIndexConditionText.cpp` maps `m['k']` from either the map
subcolumn `m.key_<k>` or `arrayElement(m, 'k')`, as today. All atoms use
`direct_read_mode = None`.

| Query | RPN atom | Token query |
|---|---|---|
| `m['k'] = 'v'` | `FUNCTION_MAP_KEY_VALUE_EQUALS` | `post(\x01k) AND post(\x02v)` |
| `m['k'] IN (v1, v2, …)` | `FUNCTION_MAP_KEY_VALUE_IN` | `post(\x01k) AND (post(\x02v1) ∪ …)` |
| `mapContains(m, 'k')` | `FUNCTION_MAP_HAS_KEY` | `post(\x01k)` |
| `has(mapValues(m), 'v')` | `FUNCTION_MAP_HAS_VALUE` | `post(\x02v)` |

Granule evaluation `hasMapEntryGranule` (replaces the per-mark `hasMapEntry`): compute the
token AND / OR / union into a slot-id posting set, map each `kid` to its granule via `kid / R`,
and the set of distinct granule indices is exactly the set of surviving granules. The pruning
unit *is* the index-granule — no per-mark range arithmetic. A token absent from the dictionary
yields an empty posting and prunes the granule for AND atoms, via the existing
`rows_range` / `is_failed` analyzer states.

## Correctness: the default-value carve-out {#default-value-carveout}

`arrayElement(m, 'k')` and the `m.key_k` subcolumn both return the value type's **default**
(`''` for `String`, zero bytes for `FixedString`) when the key is **absent**. Therefore
`m['k'] = <default>` is semantically true for rows that do **not** contain key `k`. The index
only records keys that are present, so pruning on a default-value needle would drop granules
that legitimately match — a **false negative**. This is the single unsafe case (the branch
already carries `0524eb01d44 Do not use text index for equality with an empty needle`).

Rule:

- `FUNCTION_MAP_KEY_VALUE_EQUALS` / `FUNCTION_MAP_KEY_VALUE_IN` where the value needle (or **any**
  value in the `IN` list) equals the type default → the atom is `alwaysUnknownOrTrue`: the
  granule is always kept and the real predicate decides. Fail-open, so a prune is emitted only
  when provably safe.
- `mapContains` (key-only) is exact presence — safe, no default trap.
- `has(mapValues(m), 'v')` (value-only) is safe even for `v = ''`, because `mapValues`
  contains only actually-stored values (absent keys inject nothing), so `''` means a real
  empty-valued entry, which the index recorded.

## Skip policy {#skip-policy}

The index is a skip index (`None`); the codebase's `TextIndexDirectReadMode::Hint` is a
direct-read feature and does not apply here. `mayBeTrueOnGranule`:

- default-value equality / `IN` atom → **true** (keep granule);
- otherwise → the slot-AND / presence verdict, which may skip the granule.

Chosen policy: **always evaluate the verdict where safe, no selectivity short-circuit** — the
`text_index_hint_max_selectivity` bypass is not applied in this mode, keeping the logic simple
at the cost of reading postings for common tokens.

## Testing {#testing}

- Correctness with index-on == index-off cross-check across all four query forms.
- Default-value trap: `m['absent'] = ''` returns the rows with the key absent (index must not
  prune) — the key regression this design must not break; `IN` with a default value in the list
  makes the whole atom non-pruning.
- Co-occurrence exactness: a granule with `foo = X` and `bar = Y` but no `foo = Y` prunes
  correctly for `m['foo'] = 'Y'` — the win over `mapKeys` + `mapValues`.
- Granule pruning: `EXPLAIN indexes = 1` shows reduced `Granules: k/N` for a selective value;
  `force_data_skipping_indices` recognizes the index.
- Merge → rebuild: background merge and `OPTIMIZE FINAL` produce identical results with slots
  recomputed for the new granules.
- Size benchmark on a repetitive-map (log-like) dataset comparing this index versus the
  element-level mode versus the concat workaround; expectation is this mode is smallest.
- `FixedString` keys/values, empty-map rows.

## Non-goals / follow-ups {#non-goals}

- No direct read, no row-level accuracy — always re-filter.
- Whole-token equality only; `m[k] contains word` or prefix on values is out of scope.
- A DDL knob to override `R`, and 64-bit slot ids, are possible later extensions, mirroring the
  element mode's follow-ups.
