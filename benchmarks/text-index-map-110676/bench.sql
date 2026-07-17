-- ============================================================
-- Map text-index size benchmark: concat vs element vs granule
-- Reproducing and extending ClickHouse issue #110676
-- https://github.com/ClickHouse/ClickHouse/issues/110676
--
-- Run with:
--   clickhouse local --path /path/to/data --multiquery < bench.sql
-- or via bench.sh (recommended, captures logs).
--
-- Three strategies per workload, SAME data:
--   concat  : kv Array(String) MATERIALIZED + INDEX TYPE text(tokenizer='array')
--   element : INDEX TYPE text(tokenizer='splitByNonAlpha', map_mode='element')
--   granule : INDEX TYPE text(tokenizer='splitByNonAlpha', map_mode='granule')
--
-- Five workloads matching issue #110676:
--   WL1: low-card values (~1M rows, 4 fixed keys, 5-value enum)
--   WL2: high-card values (unique trace_id per row, ~1M rows)
--   WL3: 400k distinct key=value pairs, ~10 reps each (~4M rows)
--   WL4: mixed realistic (8 enum keys + 2 unique keys, ~1M rows)
--   WL5: high-card keys (keys nearly unique per row, ~1M rows)
-- ============================================================

SET allow_experimental_full_text_index = 1;

-- ============================================================
-- WL1: Low-cardinality values
-- ~1M rows, 4 keys from fixed set, values from 5-value enum
-- Issue concat baseline: 3.06 MiB
-- ============================================================

DROP TABLE IF EXISTS wl1_concat;
DROP TABLE IF EXISTS wl1_element;
DROP TABLE IF EXISTS wl1_granule;

CREATE TABLE wl1_concat
(
    id UInt64,
    m  Map(String, String),
    kv Array(String) MATERIALIZED arrayMap((k, v) -> concat(k, '=', v), mapKeys(m), mapValues(m)),
    INDEX idx_kv kv TYPE text(tokenizer = 'array') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl1_element
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'element') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl1_granule
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

-- 1M rows: 4 keys (level, service, env, region), each value from 5-value enum
INSERT INTO wl1_concat
SELECT
    number AS id,
    map(
        'level',   arrayElement(['info', 'warn', 'error', 'debug', 'trace'], (number % 5) + 1),
        'service', arrayElement(['svcA', 'svcB', 'svcC', 'svcD', 'svcE', 'svcF', 'svcG', 'svcH'], (number % 8) + 1),
        'env',     arrayElement(['prod', 'staging', 'dev'], (number % 3) + 1),
        'region',  arrayElement(['us-east', 'us-west', 'eu-central', 'ap-south'], (number % 4) + 1)
    ) AS m
FROM numbers(1000000);

INSERT INTO wl1_element SELECT id, m FROM wl1_concat;
INSERT INTO wl1_granule SELECT id, m FROM wl1_concat;

OPTIMIZE TABLE wl1_concat  FINAL;
OPTIMIZE TABLE wl1_element FINAL;
OPTIMIZE TABLE wl1_granule FINAL;

SELECT 'WL1 index sizes (low-card values):';
SELECT
    table,
    name,
    data_compressed_bytes,
    data_uncompressed_bytes,
    round(data_compressed_bytes   / 1048576.0, 3) AS compressed_MiB,
    round(data_uncompressed_bytes / 1048576.0, 3) AS uncompressed_MiB
FROM system.data_skipping_indices
WHERE table IN ('wl1_concat', 'wl1_element', 'wl1_granule')
ORDER BY table;

-- ============================================================
-- WL2: High-cardinality values (unique trace_id per row)
-- ~1M rows, keys: level (3 vals), service (5 vals), trace_id (unique)
-- Issue concat baseline: 5.69 MiB
-- ============================================================

DROP TABLE IF EXISTS wl2_concat;
DROP TABLE IF EXISTS wl2_element;
DROP TABLE IF EXISTS wl2_granule;

CREATE TABLE wl2_concat
(
    id UInt64,
    m  Map(String, String),
    kv Array(String) MATERIALIZED arrayMap((k, v) -> concat(k, '=', v), mapKeys(m), mapValues(m)),
    INDEX idx_kv kv TYPE text(tokenizer = 'array') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl2_element
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'element') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl2_granule
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

-- 1M rows: level (3 vals), service (5 vals), trace_id (unique per row)
INSERT INTO wl2_concat
SELECT
    number AS id,
    map(
        'level',    arrayElement(['info', 'warn', 'error'], (number % 3) + 1),
        'service',  concat('svc', toString(number % 5)),
        'trace_id', concat('trace', toString(number))
    ) AS m
FROM numbers(1000000);

INSERT INTO wl2_element SELECT id, m FROM wl2_concat;
INSERT INTO wl2_granule SELECT id, m FROM wl2_concat;

OPTIMIZE TABLE wl2_concat  FINAL;
OPTIMIZE TABLE wl2_element FINAL;
OPTIMIZE TABLE wl2_granule FINAL;

SELECT 'WL2 index sizes (high-card values / unique trace_id):';
SELECT
    table,
    name,
    data_compressed_bytes,
    data_uncompressed_bytes,
    round(data_compressed_bytes   / 1048576.0, 3) AS compressed_MiB,
    round(data_uncompressed_bytes / 1048576.0, 3) AS uncompressed_MiB
FROM system.data_skipping_indices
WHERE table IN ('wl2_concat', 'wl2_element', 'wl2_granule')
ORDER BY table;

-- ============================================================
-- WL3: 400k distinct pairs, ~10 reps each
-- ~4M rows, pair id = number % 400000
-- key = 'k' || (pair_id / 100), value = 'v' || (pair_id % 100)
-- Issue concat baseline: 19.34 MiB
-- ============================================================

DROP TABLE IF EXISTS wl3_concat;
DROP TABLE IF EXISTS wl3_element;
DROP TABLE IF EXISTS wl3_granule;

CREATE TABLE wl3_concat
(
    id UInt64,
    m  Map(String, String),
    kv Array(String) MATERIALIZED arrayMap((k, v) -> concat(k, '=', v), mapKeys(m), mapValues(m)),
    INDEX idx_kv kv TYPE text(tokenizer = 'array') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl3_element
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'element') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl3_granule
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

-- 4M rows; pair_id = number % 400000, so 400k distinct pairs each ~10 reps
-- Use 2 entries per row (2 distinct pairs per row) drawn from the same pool
INSERT INTO wl3_concat
SELECT
    number AS id,
    map(
        concat('key', toString(number % 400000 / 100)),
        concat('val', toString(number % 400000 % 100)),
        concat('key', toString((number + 200000) % 400000 / 100)),
        concat('val', toString((number + 200000) % 400000 % 100))
    ) AS m
FROM numbers(4000000);

INSERT INTO wl3_element SELECT id, m FROM wl3_concat;
INSERT INTO wl3_granule SELECT id, m FROM wl3_concat;

OPTIMIZE TABLE wl3_concat  FINAL;
OPTIMIZE TABLE wl3_element FINAL;
OPTIMIZE TABLE wl3_granule FINAL;

SELECT 'WL3 index sizes (400k pairs, ~10 reps each):';
SELECT
    table,
    name,
    data_compressed_bytes,
    data_uncompressed_bytes,
    round(data_compressed_bytes   / 1048576.0, 3) AS compressed_MiB,
    round(data_uncompressed_bytes / 1048576.0, 3) AS uncompressed_MiB
FROM system.data_skipping_indices
WHERE table IN ('wl3_concat', 'wl3_element', 'wl3_granule')
ORDER BY table;

-- ============================================================
-- WL4: Mixed realistic (8 enum keys + 2 unique keys)
-- ~1M rows, 8 low-card keys + user_id + request_id (unique)
-- Issue concat baseline: 11.80 MiB
-- ============================================================

DROP TABLE IF EXISTS wl4_concat;
DROP TABLE IF EXISTS wl4_element;
DROP TABLE IF EXISTS wl4_granule;

CREATE TABLE wl4_concat
(
    id UInt64,
    m  Map(String, String),
    kv Array(String) MATERIALIZED arrayMap((k, v) -> concat(k, '=', v), mapKeys(m), mapValues(m)),
    INDEX idx_kv kv TYPE text(tokenizer = 'array') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl4_element
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'element') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl4_granule
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

-- 1M rows: 8 enum-valued keys + user_id (unique) + request_id (unique)
INSERT INTO wl4_concat
SELECT
    number AS id,
    map(
        'level',      arrayElement(['info', 'warn', 'error', 'debug', 'trace'], (number % 5) + 1),
        'service',    arrayElement(['svcA', 'svcB', 'svcC', 'svcD', 'svcE'], (number % 5) + 1),
        'env',        arrayElement(['prod', 'staging', 'dev'], (number % 3) + 1),
        'region',     arrayElement(['us-east', 'us-west', 'eu-central', 'ap-south'], (number % 4) + 1),
        'datacenter', arrayElement(['dc1', 'dc2', 'dc3'], (number % 3) + 1),
        'tier',       arrayElement(['frontend', 'backend', 'db', 'cache'], (number % 4) + 1),
        'team',       arrayElement(['alpha', 'beta', 'gamma', 'delta', 'epsilon'], (number % 5) + 1),
        'priority',   arrayElement(['low', 'medium', 'high', 'critical'], (number % 4) + 1),
        'user_id',    concat('user', toString(number)),
        'request_id', concat('req', toString(number))
    ) AS m
FROM numbers(1000000);

INSERT INTO wl4_element SELECT id, m FROM wl4_concat;
INSERT INTO wl4_granule SELECT id, m FROM wl4_concat;

OPTIMIZE TABLE wl4_concat  FINAL;
OPTIMIZE TABLE wl4_element FINAL;
OPTIMIZE TABLE wl4_granule FINAL;

SELECT 'WL4 index sizes (mixed realistic: 8 enum + 2 unique keys):';
SELECT
    table,
    name,
    data_compressed_bytes,
    data_uncompressed_bytes,
    round(data_compressed_bytes   / 1048576.0, 3) AS compressed_MiB,
    round(data_uncompressed_bytes / 1048576.0, 3) AS uncompressed_MiB
FROM system.data_skipping_indices
WHERE table IN ('wl4_concat', 'wl4_element', 'wl4_granule')
ORDER BY table;

-- ============================================================
-- WL5: High-cardinality keys (keys nearly unique per row)
-- ~1M rows, key = 'k' || (number % 500000) (500k distinct keys)
-- Issue concat baseline: 10.74 MiB
-- ============================================================

DROP TABLE IF EXISTS wl5_concat;
DROP TABLE IF EXISTS wl5_element;
DROP TABLE IF EXISTS wl5_granule;

CREATE TABLE wl5_concat
(
    id UInt64,
    m  Map(String, String),
    kv Array(String) MATERIALIZED arrayMap((k, v) -> concat(k, '=', v), mapKeys(m), mapValues(m)),
    INDEX idx_kv kv TYPE text(tokenizer = 'array') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl5_element
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'element') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

CREATE TABLE wl5_granule
(
    id UInt64,
    m  Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id;

-- 1M rows: keys are nearly unique (500k distinct keys, each appears ~2x)
-- values are from a small enum (5 values) to isolate key-cardinality effect
INSERT INTO wl5_concat
SELECT
    number AS id,
    map(
        concat('k', toString(number % 500000)),
        arrayElement(['alpha', 'beta', 'gamma', 'delta', 'epsilon'], (number % 5) + 1)
    ) AS m
FROM numbers(1000000);

INSERT INTO wl5_element SELECT id, m FROM wl5_concat;
INSERT INTO wl5_granule SELECT id, m FROM wl5_concat;

OPTIMIZE TABLE wl5_concat  FINAL;
OPTIMIZE TABLE wl5_element FINAL;
OPTIMIZE TABLE wl5_granule FINAL;

SELECT 'WL5 index sizes (high-card keys, ~500k distinct keys):';
SELECT
    table,
    name,
    data_compressed_bytes,
    data_uncompressed_bytes,
    round(data_compressed_bytes   / 1048576.0, 3) AS compressed_MiB,
    round(data_uncompressed_bytes / 1048576.0, 3) AS uncompressed_MiB
FROM system.data_skipping_indices
WHERE table IN ('wl5_concat', 'wl5_element', 'wl5_granule')
ORDER BY table;

-- ============================================================
-- PRUNING PARITY: EXPLAIN + row-count parity per workload
-- Using a selective query that should prune well
-- Note: EXPLAIN must be a top-level statement in --multiquery mode;
-- wrap results in a subquery only when running via single --query call.
-- ============================================================

SELECT '=== PRUNING PARITY CHECKS ===';

-- WL1: rare value query (level=error appears in ~1/5 rows)
SELECT 'WL1: total marks';
SELECT table, sum(marks) AS total_marks FROM system.parts WHERE table IN ('wl1_concat','wl1_element','wl1_granule') AND active GROUP BY table ORDER BY table;

SELECT 'WL1 EXPLAIN concat has(kv, level=error) -- look for Granules: N/123:';
EXPLAIN indexes=1 SELECT count() FROM wl1_concat WHERE has(kv, 'level=error');

SELECT 'WL1 EXPLAIN element m[level]=error:';
EXPLAIN indexes=1 SELECT count() FROM wl1_element WHERE m['level']='error';

SELECT 'WL1 EXPLAIN granule m[level]=error:';
EXPLAIN indexes=1 SELECT count() FROM wl1_granule WHERE m['level']='error';

SELECT 'WL1 row count concat (all three must match):';
SELECT count() FROM wl1_concat  WHERE has(kv, 'level=error');
SELECT 'WL1 row count element:';
SELECT count() FROM wl1_element WHERE m['level']='error';
SELECT 'WL1 row count granule:';
SELECT count() FROM wl1_granule WHERE m['level']='error';

-- WL2: selective trace_id query
SELECT 'WL2: total marks';
SELECT table, sum(marks) AS total_marks FROM system.parts WHERE table IN ('wl2_concat','wl2_element','wl2_granule') AND active GROUP BY table ORDER BY table;

SELECT 'WL2 EXPLAIN concat has(kv, trace_id=trace42000) -- expect 1/123:';
EXPLAIN indexes=1 SELECT count() FROM wl2_concat WHERE has(kv, 'trace_id=trace42000');

SELECT 'WL2 EXPLAIN element m[trace_id]=trace42000:';
EXPLAIN indexes=1 SELECT count() FROM wl2_element WHERE m['trace_id']='trace42000';

SELECT 'WL2 EXPLAIN granule m[trace_id]=trace42000:';
EXPLAIN indexes=1 SELECT count() FROM wl2_granule WHERE m['trace_id']='trace42000';

SELECT 'WL2 row count concat (all three must match):';
SELECT count() FROM wl2_concat  WHERE has(kv, 'trace_id=trace42000');
SELECT 'WL2 row count element:';
SELECT count() FROM wl2_element WHERE m['trace_id']='trace42000';
SELECT 'WL2 row count granule:';
SELECT count() FROM wl2_granule WHERE m['trace_id']='trace42000';

-- WL4: selective query on enum key
SELECT 'WL4: total marks';
SELECT table, sum(marks) AS total_marks FROM system.parts WHERE table IN ('wl4_concat','wl4_element','wl4_granule') AND active GROUP BY table ORDER BY table;

SELECT 'WL4 EXPLAIN concat has(kv, tier=cache) -- tier=cache in 1/4 rows:';
EXPLAIN indexes=1 SELECT count() FROM wl4_concat WHERE has(kv, 'tier=cache');

SELECT 'WL4 EXPLAIN element m[tier]=cache:';
EXPLAIN indexes=1 SELECT count() FROM wl4_element WHERE m['tier']='cache';

SELECT 'WL4 EXPLAIN granule m[tier]=cache:';
EXPLAIN indexes=1 SELECT count() FROM wl4_granule WHERE m['tier']='cache';

SELECT 'WL4 row count concat (all three must match):';
SELECT count() FROM wl4_concat  WHERE has(kv, 'tier=cache');
SELECT 'WL4 row count element:';
SELECT count() FROM wl4_element WHERE m['tier']='cache';
SELECT 'WL4 row count granule:';
SELECT count() FROM wl4_granule WHERE m['tier']='cache';

-- ============================================================
-- SUMMARY: combined index sizes across all workloads
-- ============================================================
SELECT '=== SUMMARY: all index sizes ===';
SELECT
    table,
    name,
    data_compressed_bytes,
    data_uncompressed_bytes,
    round(data_compressed_bytes   / 1048576.0, 3) AS compressed_MiB,
    round(data_uncompressed_bytes / 1048576.0, 3) AS uncompressed_MiB
FROM system.data_skipping_indices
WHERE table IN (
    'wl1_concat', 'wl1_element', 'wl1_granule',
    'wl2_concat', 'wl2_element', 'wl2_granule',
    'wl3_concat', 'wl3_element', 'wl3_granule',
    'wl4_concat', 'wl4_element', 'wl4_granule',
    'wl5_concat', 'wl5_element', 'wl5_granule'
)
ORDER BY table, name;

-- ============================================================
-- CLEANUP: remove benchmark tables
-- Comment these out if you want to inspect the data afterward
-- ============================================================
DROP TABLE IF EXISTS wl1_concat;
DROP TABLE IF EXISTS wl1_element;
DROP TABLE IF EXISTS wl1_granule;
DROP TABLE IF EXISTS wl2_concat;
DROP TABLE IF EXISTS wl2_element;
DROP TABLE IF EXISTS wl2_granule;
DROP TABLE IF EXISTS wl3_concat;
DROP TABLE IF EXISTS wl3_element;
DROP TABLE IF EXISTS wl3_granule;
DROP TABLE IF EXISTS wl4_concat;
DROP TABLE IF EXISTS wl4_element;
DROP TABLE IF EXISTS wl4_granule;
DROP TABLE IF EXISTS wl5_concat;
DROP TABLE IF EXISTS wl5_element;
DROP TABLE IF EXISTS wl5_granule;
