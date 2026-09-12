-- A predicate sitting above a step that renames its columns has to be composed through that step
-- before it can describe the relation below it. Without that, the join-order estimator either drops
-- it (over-estimating the relation) or fails to resolve it against the table statistics (sizing the
-- relation from a blanket default). Either way the wrong side of the join ends up in the hash table.

SET allow_experimental_statistics = 1;
SET allow_statistics = 1;
SET use_statistics = 1;
SET materialize_statistics_on_insert = 1;

-- The build side is chosen from the estimated row counts, so everything that could decide it some
-- other way is pinned: the reorder must run (`limit`), must not be randomized, must be allowed to
-- swap the sides, and must score the relations from their statistics rather than from a hash-table
-- size cached by an earlier query.
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

-- The relations are printed in plan order, so the second one is the build side of the join.

-- Control: a single `FilterStep` directly above the read. The predicate already describes the
-- relation below it, so the estimator resolves it and keeps the 100-row side as the build side.
SELECT 'single filter';
SELECT extract(explain, 'ReadFromMergeTree \\(\\w+\\.(\\w+)\\)') AS relation
FROM viewExplain('EXPLAIN', '', (
SELECT count()
FROM (SELECT * FROM t_filter_est_big WHERE k < 100) AS b
JOIN t_filter_est_mid AS s ON b.k = s.k
))
WHERE explain LIKE '%ReadFromMergeTree%';

-- Two filters separated by steps that cannot be merged across. The outer predicate speaks the inner
-- step's output names, so it only reaches the table once it is composed through that step. The
-- filtered side is 100 rows and must still be the build side.
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

-- The composed predicate must carry the real selectivity, not merely reach the estimator. Combining
-- the two predicates without re-expressing the outer one leaves it unresolvable against the table's
-- statistics, and the relation falls back to a fixed default selectivity - which lands below the
-- 500k side and picks the 1M-row relation as the build side. An outer predicate selecting nearly
-- every row must leave that relation too big to build on.
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
