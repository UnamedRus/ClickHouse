-- Tags: no-parallel-replicas

-- Regression test for a bug where a skip index defined on a CAST(col, 'Type')
-- expression was silently dropped for a matching indexHint() condition under the
-- new analyzer (enable_analyzer = 1), while it worked under enable_analyzer = 0.
--
-- Root cause: cloneDAGWithInversionPushDown() normalizes constant column names to
-- their AST form (stripping the analyzer-only `_Type` suffix, e.g. `'Array(String)'_String`
-- -> `'Array(String)'`) so that they match the index sample block. For indexHint the
-- arguments live inside the FunctionIndexHint's own internal ActionsDAG (which is what
-- RPNBuilder reads for index analysis), and that internal DAG was reused as-is instead of
-- being rewritten -- so its CAST type-argument kept the `_String` suffix and no longer
-- matched the index column name, dropping the index.

DROP TABLE IF EXISTS t_index_hint_cast;

CREATE TABLE t_index_hint_cast
(
    id  UInt32,
    arr Array(Dynamic),
    INDEX idx (CAST(arr, 'Array(String)')) TYPE ngrambf_v1(3, 512, 2, 0) GRANULARITY 1
) ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 4;

INSERT INTO t_index_hint_cast
SELECT number, [toString(number), concat('host', toString(number))]::Array(Dynamic)
FROM numbers(20);
OPTIMIZE TABLE t_index_hint_cast FINAL;

-- force_data_skipping_indices throws INDEX_NOT_USED if `idx` is dropped during analysis.
-- Before the fix this succeeded under enable_analyzer = 0 but threw under enable_analyzer = 1.

SELECT 'analyzer=0 absent-needle', count()
FROM t_index_hint_cast
WHERE indexHint(hasAll(arr::Array(String), ['zzz']))
  AND arrayExists(x -> position(toString(x), 'zzz') > 0, arr)
SETTINGS enable_analyzer = 0, force_data_skipping_indices = 'idx';

SELECT 'analyzer=1 absent-needle', count()
FROM t_index_hint_cast
WHERE indexHint(hasAll(arr::Array(String), ['zzz']))
  AND arrayExists(x -> position(toString(x), 'zzz') > 0, arr)
SETTINGS enable_analyzer = 1, force_data_skipping_indices = 'idx';

-- The index must also stay usable at data-read time.
SELECT 'analyzer=1 absent-needle data-read', count()
FROM t_index_hint_cast
WHERE indexHint(hasAll(arr::Array(String), ['zzz']))
  AND arrayExists(x -> position(toString(x), 'zzz') > 0, arr)
SETTINGS enable_analyzer = 1, force_data_skipping_indices = 'idx', use_skip_indexes_on_data_read = 1;

-- Correctness: a present needle returns the matching row under both analyzers.
SELECT 'analyzer=0 present-needle', id
FROM t_index_hint_cast
WHERE indexHint(hasAll(arr::Array(String), ['host7']))
  AND arrayExists(x -> position(toString(x), 'host7') > 0, arr)
ORDER BY id
SETTINGS enable_analyzer = 0, force_data_skipping_indices = 'idx';

SELECT 'analyzer=1 present-needle', id
FROM t_index_hint_cast
WHERE indexHint(hasAll(arr::Array(String), ['host7']))
  AND arrayExists(x -> position(toString(x), 'host7') > 0, arr)
ORDER BY id
SETTINGS enable_analyzer = 1, force_data_skipping_indices = 'idx';

DROP TABLE t_index_hint_cast;
