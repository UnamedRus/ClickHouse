-- Tags: long

-- The states below hold more values than fit into the buffer for copying (32 MiB), so the radix selection is used.

-- A large state of almost identical extreme values plus a few outliers of the opposite extreme:
-- the minimum and the maximum must be exact.
SELECT quantileExact(1)(x), quantileExact(0)(x), quantileExact(0.5)(x)
FROM (SELECT if(number IN (1, 7), 9223372036854775807, -9223372036854775808) AS x FROM numbers(5000000));

SELECT quantileExact(1)(x), quantileExact(0)(x), quantileExact(0.5)(x)
FROM (SELECT if(number IN (1, 7), 18446744073709551615, 0)::UInt64 AS x FROM numbers_mt(5000000));

SELECT quantilesExactLow(0, 1)(x), quantilesExactHigh(0, 1)(x)
FROM (SELECT if(number IN (1, 7), 9223372036854775807, -9223372036854775808) AS x FROM numbers_mt(5000000));

-- The result must not depend on the order of the values: sorted, reversed and shuffled permutations of 0..4999999.
SELECT 'sorted', quantilesExact(0, 0.5, 1)(number) FROM numbers_mt(5000000);
SELECT 'reversed', quantilesExact(0, 0.5, 1)(4999999 - number) FROM numbers_mt(5000000);
SELECT 'shuffled', quantilesExact(0, 0.5, 1)((number * 2654435761) % 5000000) FROM numbers_mt(5000000);
SELECT 'shuffled Float64', quantilesExact(0, 0.5, 1)(toFloat64((number * 2654435761) % 5000000)) FROM numbers_mt(5000000);

-- 4-byte values need more rows to reach the radix selection: a permutation of 0..8999999.
SELECT 'shuffled Int32', quantilesExact(0, 0.5, 1)(toInt32((number * 2654435761) % 9000000)) FROM numbers_mt(9000000);
