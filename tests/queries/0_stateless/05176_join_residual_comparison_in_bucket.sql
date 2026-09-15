-- A JOIN ON condition that is not all equalities leaves a residual comparison, which the probe checks
-- once per candidate row of the bucket the hash keys leave. Every query below is paired with a control
-- whose left argument is written as `v + 0`: an expression rather than a plain column, which the probe
-- cannot test while walking the bucket and so must evaluate the general way. The two must agree.

SET join_algorithm = 'parallel_hash';
SET query_plan_join_swap_table = 'false';

DROP TABLE IF EXISTS t_residual_probe;
DROP TABLE IF EXISTS t_residual_build;

CREATE TABLE t_residual_probe (k UInt64, v UInt64) ENGINE = MergeTree ORDER BY k;
CREATE TABLE t_residual_build (k UInt64, v UInt64) ENGINE = MergeTree ORDER BY k;

-- Eight rows per key, so every probe row walks a bucket rather than a single entry, and the surviving
-- candidates sit at varying positions inside it.
INSERT INTO t_residual_probe SELECT number % 500 AS k, number % 8 AS v FROM numbers(4000);
INSERT INTO t_residual_build SELECT number % 500 AS k, number % 8 AS v FROM numbers(4000);

SELECT 'inner all, equality residual';
SELECT (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND l.v = r.v) AS walked,
       (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND l.v + 0 = r.v) AS evaluated,
       walked = evaluated AS agree;

SELECT 'inner all, ordering residual';
SELECT (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND l.v < r.v) AS walked,
       (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND l.v + 0 < r.v) AS evaluated,
       walked = evaluated AS agree;

-- The right column as the first argument reverses the direction the walk must apply.
SELECT 'inner all, ordering residual reversed';
SELECT (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND r.v > l.v) AS walked,
       (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND r.v > l.v + 0) AS evaluated,
       walked = evaluated AS agree;

-- ANY keeps the first surviving candidate, so the walk may stop at it.
SELECT 'left any';
SELECT (SELECT count() FROM t_residual_probe AS l ANY LEFT JOIN t_residual_build AS r ON l.k = r.k AND l.v < r.v) AS walked,
       (SELECT count() FROM t_residual_probe AS l ANY LEFT JOIN t_residual_build AS r ON l.k = r.k AND l.v + 0 < r.v) AS evaluated,
       walked = evaluated AS agree;

SELECT 'left semi';
SELECT (SELECT count() FROM t_residual_probe AS l SEMI LEFT JOIN t_residual_build AS r ON l.k = r.k AND l.v < r.v) AS walked,
       (SELECT count() FROM t_residual_probe AS l SEMI LEFT JOIN t_residual_build AS r ON l.k = r.k AND l.v + 0 < r.v) AS evaluated,
       walked = evaluated AS agree;

-- ANTI reports the absence of a match, so a surviving candidate must still be seen when the walk stops.
SELECT 'left anti';
SELECT (SELECT count() FROM t_residual_probe AS l ANTI LEFT JOIN t_residual_build AS r ON l.k = r.k AND l.v < r.v) AS walked,
       (SELECT count() FROM t_residual_probe AS l ANTI LEFT JOIN t_residual_build AS r ON l.k = r.k AND l.v + 0 < r.v) AS evaluated,
       walked = evaluated AS agree;

-- A residual no candidate satisfies must leave the bucket with nothing.
SELECT 'no candidate survives';
SELECT (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND l.v > r.v + 100) AS walked,
       (SELECT count() FROM t_residual_probe AS l JOIN t_residual_build AS r ON l.k = r.k AND l.v + 0 > r.v + 100) AS evaluated,
       walked = evaluated AS agree;

-- A NULL on the right must not count as a surviving candidate.
SELECT 'nullable right column';
SELECT (SELECT count() FROM t_residual_probe AS l JOIN (SELECT k, if(v % 3 = 0, NULL, v) AS v FROM t_residual_build) AS r
            ON l.k = r.k AND l.v < r.v) AS walked,
       (SELECT count() FROM t_residual_probe AS l JOIN (SELECT k, if(v % 3 = 0, NULL, v) AS v FROM t_residual_build) AS r
            ON l.k = r.k AND l.v + 0 < r.v) AS evaluated,
       walked = evaluated AS agree;

DROP TABLE t_residual_probe;
DROP TABLE t_residual_build;
