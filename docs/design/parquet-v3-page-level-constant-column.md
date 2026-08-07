# Parquet v3: per-subgroup (page-level) constant-column detection

## Motivation

The current constant-column optimization (`detectConstantColumn`) fires only when a whole
Parquet **column chunk** is single-valued (footer `min == max`, no nulls). Data that is sorted or
clustered on a column is often constant over long **runs of pages** without the whole row group
being constant, so chunk-level detection misses it.

Parquet's **Column Index** stores per-page `min_values` / `max_values` / `null_pages` /
`null_counts`. The v3 reader already loads and parses it (`applyColumnIndex`) for predicate
push-down, so per-page "is this page constant?" is available at zero extra I/O. This lets us mark a
column constant for the row range of an individual **row subgroup** (the unit that becomes one
output `Chunk`) and materialize it as a `ColumnConst`, skipping the covered pages' reads and decode.

## Tiers (both kept)

| Tier | Source | Always present? | Exact flag? | Granularity |
|------|--------|-----------------|-------------|-------------|
| 1 (existing) | footer `ColumnMetaData.statistics` | yes | yes (`is_min_value_exact`) | whole chunk |
| 2 (this doc) | Column Index (per page) | no (optional) | no | per subgroup |

Tier 1 stays the always-on baseline and the **only** safe detector for `BYTE_ARRAY` /
`FIXED_LEN_BYTE_ARRAY` (Column Index has no per-page exactness flag; 16-byte truncation can make two
distinct strings compare equal). Tier 2 is opportunistic: only for fixed-width numeric/date/time,
only when the Column Index is already loaded, only for chunks tier 1 did not already mark constant.

## Key decision: do NOT change subgroup sizing

Aligning subgroups to page boundaries would fragment the block stream into many tiny chunks
(pages are far smaller than a row group), and a subgroup carries all columns so its size is bounded
by the non-constant columns anyway. Instead, keep subgroup boundaries exactly as today and do
**per-subgroup, per-column** detection: a column is constant for a subgroup iff every Column-Index
page overlapping the subgroup's row range is constant with the *same* value (or every such page is
`null_pages`). Chunk count is unchanged; we just catch subgroups that sit inside a constant run.

## Phases

- **Phase 0** — retain per-page constant info. `applyColumnIndex` currently discards the parsed
  `parq::ColumnIndex`. Keep a compact per-page summary on `ColumnChunk` (value + `is_const` +
  `all_null`), plus the page→`first_row_index` map already in the Offset Index. Populate only for
  eligible types.
- **Phase 1** — `detectConstantSubchunk(column, column_info, [start_row, end_row))`: scan the pages
  overlapping the range; return constant + value when all are `is_const` and share one value;
  all-null when all are `null_pages`.
- **Phase 2** — call it in `intersectColumnIndexResultsAndInitSubgroups` for each subgroup /
  primitive column that tier 1 didn't already mark constant; set `subchunk.is_constant` /
  `is_all_null` / `constant_value` (the same fields `decodePrimitiveColumn` propagates).
  `formOutputColumn` needs no change (already emits `ColumnConst`).
- **Phase 3** — skip work for constant subchunks: `decodePrimitiveColumn` skips decode on
  `subchunk.is_constant`; `determinePagesToPrefetch` skips fetching a page only when it is constant
  in **every** subgroup that overlaps it.
- **Phase 4** — stateless tests (page-run constant, all-null-per-page, byte-array negative/truncation
  guard, `GROUP BY` correctness), with a new `ParquetConstantColumnSubchunks` ProfileEvent to prove
  tier 2 fired.
- **Phase 5** — ProfileEvents comparison on clustered data: expect further `S3GetObject` /
  `ParquetFetchWaitTimeMicroseconds` drops with chunk count unchanged.

## Guardrails

- Fixed-width numeric/date/time only (truncation).
- Reuse the Column Index only when already loaded; never force-fetch it just for this.
- Gate on the existing `input_format_parquet_use_constant_column_optimization` setting.
- Partial-page subgroup boundaries are fine: a partial overlap of a constant page still yields that
  value, as long as every overlapping page is constant with the shared value.

## Out of scope

In-subgroup partial fill for constant runs *shorter* than a subgroup (skip only those pages' bytes,
yield a full column) — a later follow-up.
