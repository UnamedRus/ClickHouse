-- A predicate above a step that renames its columns has to be composed through it, or the join-order
-- estimator either drops it or cannot resolve it against the statistics - and builds the wrong side.

SET allow_experimental_statistics = 1;
SET allow_statistics = 1;
SET use_statistics = 1;
SET materialize_statistics_on_insert = 1;

-- The build side comes from the estimated row counts, so pin everything else that could decide it:
-- the reorder must run, unrandomized, allowed to swap, and scored from statistics, not a cached
-- hash-table size.
SET query_plan_join_swap_table = 'auto';
SET query_plan_optimize_join_order_limit = 10;
SET query_plan_optimize_join_order_randomize = 0;
SET use_hash_table_stats_for_join_reordering = 0;

DROP TABLE IF EXISTS t_filter_est_big;
DROP TABLE IF EXISTS t_filter_est_mid;

CREATE TABLE t_filter_est_big
(
    k UInt64 STATISTICS(tdigest, uniq),
    v UInt64 STATISTICS(tdigest, uniq)
) ENGINE = MergeTree ORDER BY tuple();

CREATE TABLE t_filter_est_mid
(
    k UInt64 STATISTICS(tdigest, uniq)
) ENGINE = MergeTree ORDER BY tuple();

INSERT INTO t_filter_est_big SELECT number, number FROM numbers(1000000);
INSERT INTO t_filter_est_mid SELECT number FROM numbers(500000);

OPTIMIZE TABLE t_filter_est_big FINAL;
OPTIMIZE TABLE t_filter_est_mid FINAL;

-- Relations print in plan order, so the second one is the build side.

-- Control: one `FilterStep` above the read. Nothing to compose, so the 100-row side is built.
SELECT 'single filter';
SELECT extract(explain, 'ReadFromMergeTree \\(\\w+\\.(\\w+)\\)') AS relation
FROM viewExplain('EXPLAIN', '', (
SELECT count()
FROM (SELECT * FROM t_filter_est_big WHERE k < 100) AS b
JOIN t_filter_est_mid AS s ON b.k = s.k
))
WHERE explain LIKE '%ReadFromMergeTree%';

-- Two filters separated by steps they cannot be merged across. The outer predicate speaks the inner
-- step's output names, so it reaches the table only once composed. That side is 100 rows.
SELECT 'stacked filters';
SELECT extract(explain, 'ReadFromMergeTree \\(\\w+\\.(\\w+)\\)') AS relation
FROM viewExplain('EXPLAIN', '', (
SELECT count()
FROM (
    SELECT * FROM (
        SELECT * FROM (
            SELECT * FROM (SELECT * FROM t_filter_est_big LIMIT 10000000) WHERE v < 10000000
        ) LIMIT 9999999
    ) WHERE k < 100
) AS b
JOIN t_filter_est_mid AS s ON b.k = s.k
))
WHERE explain LIKE '%ReadFromMergeTree%';

-- The composed predicate must carry real selectivity, not merely reach the estimator: combining the
-- two without re-expressing the outer one falls back to a default selectivity, which lands below the
-- 500k side. An outer predicate selecting nearly every row must leave 1M too big to build on.
SELECT 'stacked filters, non-selective outer predicate';
SELECT extract(explain, 'ReadFromMergeTree \\(\\w+\\.(\\w+)\\)') AS relation
FROM viewExplain('EXPLAIN', '', (
SELECT count()
FROM (
    SELECT * FROM (
        SELECT * FROM (
            SELECT * FROM (SELECT * FROM t_filter_est_big LIMIT 10000000) WHERE v < 10000000
        ) LIMIT 9999999
    ) WHERE k < 999999
) AS b
JOIN t_filter_est_mid AS s ON b.k = s.k
))
WHERE explain LIKE '%ReadFromMergeTree%';

DROP TABLE t_filter_est_big;
DROP TABLE t_filter_est_mid;
