-- Tags: no-fasttest, no-cpu-s390x
-- ^ DataSketches is not built in fast-test builds.

-- The union of sketches that are all small replays their values into the union, and the HIP estimate of such a union depends
-- on the order of the merges. The third parameter, `estimator`, chooses the estimate of a merged state: `DEFAULT` is the one of
-- the library, `COMPOSITE` is computed from the merged sketches only.

SET max_threads = 1;

-- 8 sketches of 100 values each, neighbours overlap by half, 450 distinct values in total, merged in three orders

SELECT 'the default estimate of a merged state depends on the order of the merges';
SELECT count() = 3 AND uniqExact(r) > 1 FROM
(
    SELECT uniqApacheHLLMerge(11, 'HLL_8')(s) AS r FROM (SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(0) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(1) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(2) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(3) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(4) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(5) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(6) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(7) * 50 + number) AS s FROM numbers(100)) UNION ALL
    SELECT uniqApacheHLLMerge(11, 'HLL_8')(s) AS r FROM (SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(7) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(6) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(5) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(4) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(3) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(2) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(1) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(0) * 50 + number) AS s FROM numbers(100)) UNION ALL
    SELECT uniqApacheHLLMerge(11, 'HLL_8')(s) AS r FROM (SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(3) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(6) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(0) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(5) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(1) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(7) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(2) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(4) * 50 + number) AS s FROM numbers(100))
);

SELECT 'the composite estimate does not';
SELECT count() = 3 AND uniqExact(r) = 1 AND any(r) BETWEEN 430 AND 470 FROM
(
    SELECT uniqApacheHLLMerge(11, 'HLL_8', 'COMPOSITE')(s) AS r FROM (SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(0) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(1) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(2) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(3) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(4) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(5) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(6) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(7) * 50 + number) AS s FROM numbers(100)) UNION ALL
    SELECT uniqApacheHLLMerge(11, 'HLL_8', 'COMPOSITE')(s) AS r FROM (SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(7) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(6) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(5) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(4) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(3) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(2) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(1) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(0) * 50 + number) AS s FROM numbers(100)) UNION ALL
    SELECT uniqApacheHLLMerge(11, 'HLL_8', 'COMPOSITE')(s) AS r FROM (SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(3) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(6) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(0) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(5) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(1) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(7) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(2) * 50 + number) AS s FROM numbers(100) UNION ALL SELECT uniqApacheHLLState(11, 'HLL_8')(toUInt64(4) * 50 + number) AS s FROM numbers(100))
);

SELECT 'the estimator does not change the state, and states of any estimator have one type where a common type is needed';
SELECT hex(toString(uniqApacheHLLState(11, 'HLL_8', 'COMPOSITE')(number))) = hex(toString(uniqApacheHLLState(11, 'HLL_8', 'DEFAULT')(number))) FROM numbers(1000);
SELECT length([uniqApacheHLLState(8, 'HLL_6', 'COMPOSITE')(toUInt64(1)), uniqApacheHLLState(toUInt64(2))]);

SELECT 'a sketch that is not merged keeps its estimate';
SELECT uniqApacheHLL(11, 'HLL_8', 'COMPOSITE')(number), uniqApacheHLL(11, 'HLL_8', 'DEFAULT')(number) FROM numbers(100);

SELECT 'wrong parameters';
SELECT uniqApacheHLL(11, 'HLL_8', 'HIP')(number) FROM numbers(1); -- { serverError BAD_ARGUMENTS }
SELECT uniqApacheHLL(11, 'HLL_8', 1)(number) FROM numbers(1); -- { serverError BAD_ARGUMENTS }
SELECT uniqApacheHLL(11, 'HLL_8', 'COMPOSITE', 1)(number) FROM numbers(1); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
