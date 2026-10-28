-- Tags: long

-- `quantileExact` family switches from a single array to chunked storage with radix selection for large states.
-- Every check compares the results with the reference computed by sorting all the values with `arraySort`.

SET max_threads = 4;
SET max_block_size = 8192;

DROP TABLE IF EXISTS t_quantile_exact_chunked;

CREATE TABLE t_quantile_exact_chunked
(
    n UInt64,
    f64 Float64,
    f32 Float32,
    i64 Int64,
    u64 UInt64,
    i32 Int32,
    u8 UInt8,
    i16 Int16,
    dt DateTime,
    d Date,
    d64 Decimal64(3),
    d32 Decimal32(2),
    d128 Decimal128(5),
    dt64 DateTime64(3, 'UTC'),
    i128 Int128,
    i256 Int256,
    u256 UInt256
)
ENGINE = MergeTree ORDER BY n SETTINGS index_granularity = 8192;

-- 300000 rows: more than one chunk for every type except the 1-byte ones, many parts and blocks for parallel aggregation.
INSERT INTO t_quantile_exact_chunked
SELECT
    number AS n,
    (toInt64(sipHash64(number) % 2000001) - 1000000) / 7 AS f64,
    toFloat32(f64) AS f32,
    toInt64(sipHash64(number, 1)) AS i64,
    sipHash64(number, 2) AS u64,
    toInt32(sipHash64(number, 3) % 1000) - 500 AS i32,
    toUInt8(sipHash64(number, 4) % 256) AS u8,
    toInt16(toInt64(sipHash64(number, 5) % 65536) - 32768) AS i16,
    toDateTime(1600000000 + sipHash64(number, 6) % 100000000, 'UTC') AS dt,
    toDate(10000 + sipHash64(number, 7) % 20000) AS d,
    toDecimal64((toInt64(sipHash64(number, 8) % 2000000001) - 1000000000) / 1000, 3) AS d64,
    toDecimal32((toInt64(sipHash64(number, 9) % 2000001) - 1000000) / 100, 2) AS d32,
    toDecimal128((toInt64(sipHash64(number, 10) % 2000000001) - 1000000000) / 100000, 5) AS d128,
    toDateTime64(1600000000 + (sipHash64(number, 11) % 100000000000) / 1000, 3, 'UTC') AS dt64,
    toInt128(toInt64(sipHash64(number, 12))) * 1000000007 AS i128,
    toInt256(toInt64(sipHash64(number, 13))) * toInt256(toInt64(sipHash64(number, 14))) AS i256,
    toUInt256(sipHash64(number, 15)) * 1000000000000 AS u256
FROM numbers(300000);


-- All rows: a single large state, merged from several threads.

SELECT 'f64', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'f32', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f32) AS qs_high,
        quantileExact(0.3)(f32) AS q,
        medianExact(f32) AS med,
        medianExactLow(f32) AS low,
        medianExactHigh(f32) AS high,
        arraySort(groupArray(f32)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'i64', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_high,
        quantileExact(0.3)(i64) AS q,
        medianExact(i64) AS med,
        medianExactLow(i64) AS low,
        medianExactHigh(i64) AS high,
        arraySort(groupArray(i64)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'u64', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u64) AS qs_high,
        quantileExact(0.3)(u64) AS q,
        medianExact(u64) AS med,
        medianExactLow(u64) AS low,
        medianExactHigh(u64) AS high,
        arraySort(groupArray(u64)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'i32', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'u8', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u8) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u8) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u8) AS qs_high,
        quantileExact(0.3)(u8) AS q,
        medianExact(u8) AS med,
        medianExactLow(u8) AS low,
        medianExactHigh(u8) AS high,
        arraySort(groupArray(u8)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'i16', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i16) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i16) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i16) AS qs_high,
        quantileExact(0.3)(i16) AS q,
        medianExact(i16) AS med,
        medianExactLow(i16) AS low,
        medianExactHigh(i16) AS high,
        arraySort(groupArray(i16)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'dt', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(dt) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(dt) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(dt) AS qs_high,
        quantileExact(0.3)(dt) AS q,
        medianExact(dt) AS med,
        medianExactLow(dt) AS low,
        medianExactHigh(dt) AS high,
        arraySort(groupArray(dt)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'd', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d) AS qs_high,
        quantileExact(0.3)(d) AS q,
        medianExact(d) AS med,
        medianExactLow(d) AS low,
        medianExactHigh(d) AS high,
        arraySort(groupArray(d)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'd64', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs_high,
        quantileExact(0.3)(d64) AS q,
        medianExact(d64) AS med,
        medianExactLow(d64) AS low,
        medianExactHigh(d64) AS high,
        arraySort(groupArray(d64)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'd32', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d32) AS qs_high,
        quantileExact(0.3)(d32) AS q,
        medianExact(d32) AS med,
        medianExactLow(d32) AS low,
        medianExactHigh(d32) AS high,
        arraySort(groupArray(d32)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'd128', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d128) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d128) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d128) AS qs_high,
        quantileExact(0.3)(d128) AS q,
        medianExact(d128) AS med,
        medianExactLow(d128) AS low,
        medianExactHigh(d128) AS high,
        arraySort(groupArray(d128)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'dt64', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(dt64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(dt64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(dt64) AS qs_high,
        quantileExact(0.3)(dt64) AS q,
        medianExact(dt64) AS med,
        medianExactLow(dt64) AS low,
        medianExactHigh(dt64) AS high,
        arraySort(groupArray(dt64)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'i128', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i128) AS qs,
        quantileExact(0.3)(i128) AS q,
        medianExact(i128) AS med,
        arraySort(groupArray(i128)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'i256', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
);

SELECT 'u256', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u256) AS qs,
        quantileExact(0.3)(u256) AS q,
        medianExact(u256) AS med,
        arraySort(groupArray(u256)) AS s
    FROM t_quantile_exact_chunked
);


-- Sizes around the threshold of switching to chunks (32768 values of 8 bytes, 8192 values of 32 bytes).

SELECT 'f64 n < 8191', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8191
);

SELECT 'i32 n < 8191', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8191
);

SELECT 'i256 n < 8191', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8191
);

SELECT 'f64 n < 8192', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8192
);

SELECT 'i32 n < 8192', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8192
);

SELECT 'i256 n < 8192', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8192
);

SELECT 'f64 n < 8193', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8193
);

SELECT 'i32 n < 8193', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8193
);

SELECT 'i256 n < 8193', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 8193
);

SELECT 'f64 n < 32767', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32767
);

SELECT 'i32 n < 32767', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32767
);

SELECT 'i256 n < 32767', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32767
);

SELECT 'f64 n < 32768', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32768
);

SELECT 'i32 n < 32768', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32768
);

SELECT 'i256 n < 32768', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32768
);

SELECT 'f64 n < 32769', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32769
);

SELECT 'i32 n < 32769', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32769
);

SELECT 'i256 n < 32769', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 32769
);

SELECT 'f64 n < 65537', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 65537
);

SELECT 'i32 n < 65537', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_high,
        quantileExact(0.3)(i32) AS q,
        medianExact(i32) AS med,
        medianExactLow(i32) AS low,
        medianExactHigh(i32) AS high,
        arraySort(groupArray(i32)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 65537
);

SELECT 'i256 n < 65537', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    WHERE n < 65537
);


-- Interpolated variants (only for basic numeric types).

SELECT 'f64 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(f64) AS qs_exc,
            quantileExactInclusive(0.5)(f64) AS q_inc,
            quantileExactExclusive(0.5)(f64) AS q_exc,
            arraySort(groupArray(f64)) AS s
        FROM t_quantile_exact_chunked
    )
);

SELECT 'f32 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f32) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(f32) AS qs_exc,
            quantileExactInclusive(0.5)(f32) AS q_inc,
            quantileExactExclusive(0.5)(f32) AS q_exc,
            arraySort(groupArray(f32)) AS s
        FROM t_quantile_exact_chunked
    )
);

SELECT 'i64 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(i64) AS qs_exc,
            quantileExactInclusive(0.5)(i64) AS q_inc,
            quantileExactExclusive(0.5)(i64) AS q_exc,
            arraySort(groupArray(i64)) AS s
        FROM t_quantile_exact_chunked
    )
);

SELECT 'i32 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i32) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(i32) AS qs_exc,
            quantileExactInclusive(0.5)(i32) AS q_inc,
            quantileExactExclusive(0.5)(i32) AS q_exc,
            arraySort(groupArray(i32)) AS s
        FROM t_quantile_exact_chunked
    )
);

SELECT 'u8 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(u8) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(u8) AS qs_exc,
            quantileExactInclusive(0.5)(u8) AS q_inc,
            quantileExactExclusive(0.5)(u8) AS q_exc,
            arraySort(groupArray(u8)) AS s
        FROM t_quantile_exact_chunked
    )
);

SELECT 'i16 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i16) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(i16) AS qs_exc,
            quantileExactInclusive(0.5)(i16) AS q_inc,
            quantileExactExclusive(0.5)(i16) AS q_exc,
            arraySort(groupArray(i16)) AS s
        FROM t_quantile_exact_chunked
    )
);

SELECT 'f64 n < 32769 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(f64) AS qs_exc,
            quantileExactInclusive(0.5)(f64) AS q_inc,
            quantileExactExclusive(0.5)(f64) AS q_exc,
            arraySort(groupArray(f64)) AS s
        FROM t_quantile_exact_chunked
        WHERE n < 32769
    )
);


-- Many small groups and several large groups.

SELECT 'f64 group by n % 10000', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 10000
);

SELECT 'f64 group by n % 3', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 3
);

SELECT 'i64 group by n % 10000', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_high,
        quantileExact(0.3)(i64) AS q,
        medianExact(i64) AS med,
        medianExactLow(i64) AS low,
        medianExactHigh(i64) AS high,
        arraySort(groupArray(i64)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 10000
);

SELECT 'i64 group by n % 3', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_high,
        quantileExact(0.3)(i64) AS q,
        medianExact(i64) AS med,
        medianExactLow(i64) AS low,
        medianExactHigh(i64) AS high,
        arraySort(groupArray(i64)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 3
);

SELECT 'd64 group by n % 10000', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs_high,
        quantileExact(0.3)(d64) AS q,
        medianExact(d64) AS med,
        medianExactLow(d64) AS low,
        medianExactHigh(d64) AS high,
        arraySort(groupArray(d64)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 10000
);

SELECT 'd64 group by n % 3', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(d64) AS qs_high,
        quantileExact(0.3)(d64) AS q,
        medianExact(d64) AS med,
        medianExactLow(d64) AS low,
        medianExactHigh(d64) AS high,
        arraySort(groupArray(d64)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 3
);

SELECT 'i256 group by n % 10000', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 10000
);

SELECT 'i256 group by n % 3', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 3
);


-- Distributed query: the states are serialized and merged on the initiator.

SELECT 'remote f64', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM remote('127.0.0.{1,2}', currentDatabase(), t_quantile_exact_chunked)
);

SELECT 'remote i256', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i256) AS qs,
        quantileExact(0.3)(i256) AS q,
        medianExact(i256) AS med,
        arraySort(groupArray(i256)) AS s
    FROM remote('127.0.0.{1,2}', currentDatabase(), t_quantile_exact_chunked)
);

SELECT 'remote i64 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(i64) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(i64) AS qs_exc,
            quantileExactInclusive(0.5)(i64) AS q_inc,
            quantileExactExclusive(0.5)(i64) AS q_exc,
            arraySort(groupArray(i64)) AS s
        FROM remote('127.0.0.{1,2}', currentDatabase(), t_quantile_exact_chunked)
    )
);


-- Two-level aggregation and external merges.

SELECT 'f64 group by n % 50000 two-level', count(), countIf(NOT (length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]))
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(f64) AS qs_high,
        quantileExact(0.3)(f64) AS q,
        medianExact(f64) AS med,
        medianExactLow(f64) AS low,
        medianExactHigh(f64) AS high,
        arraySort(groupArray(f64)) AS s
    FROM t_quantile_exact_chunked
    GROUP BY n % 50000
)
SETTINGS group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1;


-- -State and -Merge, the states are serialized between the stages of distributed query.
SELECT 'state merge',
    (SELECT quantilesExactMerge(0.1, 0.5, 0.9)(st) FROM (SELECT quantilesExactState(0.1, 0.5, 0.9)(f64) AS st FROM t_quantile_exact_chunked GROUP BY n % 7))
    = (SELECT quantilesExact(0.1, 0.5, 0.9)(f64) FROM t_quantile_exact_chunked);

SELECT 'state merge of chunked and small',
    (SELECT medianExactMerge(st) FROM (SELECT medianExactState(i64) AS st FROM t_quantile_exact_chunked GROUP BY n < 100))
    = (SELECT medianExact(i64) FROM t_quantile_exact_chunked);


-- The serialized state is the number of values and the values in the order of insertion, the same as `groupArray`.
SELECT 'state format', hex(quantileExactState(f64)) = hex(groupArrayState(f64)), length(hex(quantileExactState(f64)))
FROM (SELECT f64 FROM t_quantile_exact_chunked WHERE n < 100000 ORDER BY n) SETTINGS max_threads = 1;

SELECT 'state format small', hex(quantileExactState(toUInt8(number))) FROM numbers(3);

-- States written by the previous versions.
SELECT 'old state', finalizeAggregation(CAST(unhex('03000102'), 'AggregateFunction(quantileExact, UInt8)'));

SELECT 'old large state', finalizeAggregation(CAST(unhex(hex(groupArrayState(f64))), 'AggregateFunction(quantileExact(0.3), Float64)')) = quantileExact(0.3)(f64)
FROM t_quantile_exact_chunked;

-- -State of the merged state: the merged state shares chunks with the source states.
SELECT 'state of merged', finalizeAggregation(st2) = (SELECT medianExact(u64) FROM t_quantile_exact_chunked)
FROM (SELECT medianExactMergeState(st) AS st2 FROM (SELECT medianExactState(u64) AS st FROM t_quantile_exact_chunked GROUP BY n % 5));

-- runningAccumulate merges each state into the accumulator, the source states are destroyed after that.
SELECT 'running accumulate', groupArray(m) = [
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n % 4 <= 0),
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n % 4 <= 1),
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n % 4 <= 2),
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n % 4 <= 3)]
FROM
(
    SELECT runningAccumulate(st) AS m
    FROM (SELECT n % 4 AS k, medianExactState(f64) AS st FROM t_quantile_exact_chunked GROUP BY k ORDER BY k)
)
SETTINGS allow_deprecated_error_prone_window_functions = 1, max_bytes_before_external_group_by = 0, max_bytes_ratio_before_external_group_by = 0;


-- Window function: the same state is finalized many times while the values are added.
SELECT 'window f64', arrayMap(x -> x.2, arraySort(groupArray((n, m)))) = [
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n <= 0),
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n <= 32766),
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n <= 32767),
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n <= 32768),
        (SELECT medianExact(f64) FROM t_quantile_exact_chunked WHERE n <= 33999)]
FROM
(
    SELECT n, medianExact(f64) OVER (ORDER BY n ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS m
    FROM t_quantile_exact_chunked
    WHERE n < 34000
)
WHERE n IN (0, 32766, 32767, 32768, 33999);

SELECT 'window i256', arrayMap(x -> x.2, arraySort(groupArray((n, m)))) = [
        (SELECT quantileExact(0.5)(i256) FROM t_quantile_exact_chunked WHERE n <= 0),
        (SELECT quantileExact(0.5)(i256) FROM t_quantile_exact_chunked WHERE n <= 8190),
        (SELECT quantileExact(0.5)(i256) FROM t_quantile_exact_chunked WHERE n <= 8191),
        (SELECT quantileExact(0.5)(i256) FROM t_quantile_exact_chunked WHERE n <= 8192),
        (SELECT quantileExact(0.5)(i256) FROM t_quantile_exact_chunked WHERE n <= 8999)]
FROM
(
    SELECT n, quantileExact(0.5)(i256) OVER (ORDER BY n ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS m
    FROM t_quantile_exact_chunked
    WHERE n < 9000
)
WHERE n IN (0, 8190, 8191, 8192, 8999);

-- arrayReduce, -ForEach, -Resample, -If.
SELECT 'arrayReduce', arrayReduce('medianExact', groupArray(f64)) = medianExact(f64) FROM t_quantile_exact_chunked;
SELECT 'ForEach', medianExactForEach([f64, -f64]) = [medianExact(f64), medianExact(-f64)] FROM t_quantile_exact_chunked;
SELECT 'Resample', quantileExactResample(0.5, 0, 3, 1)(f64, n % 3) = [medianExactIf(f64, n % 3 = 0), medianExactIf(f64, n % 3 = 1), medianExactIf(f64, n % 3 = 2)]
FROM t_quantile_exact_chunked;


-- Special floating point values: NaNs are skipped; -0.0, 0.0, infinities, all-equal values.

SELECT 'special floats', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_high,
        quantileExact(0.3)(x) AS q,
        medianExact(x) AS med,
        medianExactLow(x) AS low,
        medianExactHigh(x) AS high,
        arraySort(arrayFilter(v -> NOT isNaN(v), groupArray(x))) AS s
    FROM (SELECT if(number % 3 = 0, arrayElement([nan, -nan, inf, -inf, -0., 0., 1e-310, -1e-310, 1., -1.], intDiv(number, 3) % 10 + 1), toFloat64(number) - 100000) AS x FROM numbers(100000))
);

SELECT 'special floats only', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_high,
        quantileExact(0.3)(x) AS q,
        medianExact(x) AS med,
        medianExactLow(x) AS low,
        medianExactHigh(x) AS high,
        arraySort(arrayFilter(v -> NOT isNaN(v), groupArray(x))) AS s
    FROM (SELECT arrayElement([nan, -0., 0., inf, -inf], number % 5 + 1) AS x FROM numbers(200000))
);

SELECT 'zeros', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_high,
        quantileExact(0.3)(x) AS q,
        medianExact(x) AS med,
        medianExactLow(x) AS low,
        medianExactHigh(x) AS high,
        arraySort(groupArray(x)) AS s
    FROM (SELECT if(number % 2, -0., 0.) AS x FROM numbers(100000))
);

SELECT 'nans interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(x) AS qs_exc,
            quantileExactInclusive(0.5)(x) AS q_inc,
            quantileExactExclusive(0.5)(x) AS q_exc,
            arraySort(arrayFilter(v -> NOT isNaN(v), groupArray(x))) AS s
        FROM (SELECT if(number % 2, nan, toFloat64(number % 1000)) AS x FROM numbers(200000))
    )
);

SELECT 'all equal', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_high,
        quantileExact(0.3)(x) AS q,
        medianExact(x) AS med,
        medianExactLow(x) AS low,
        medianExactHigh(x) AS high,
        arraySort(groupArray(x)) AS s
    FROM (SELECT toFloat64(42) AS x FROM numbers(5000000))
);

SELECT 'three values', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_high,
        quantileExact(0.3)(x) AS q,
        medianExact(x) AS med,
        medianExactLow(x) AS low,
        medianExactHigh(x) AS high,
        arraySort(groupArray(x)) AS s
    FROM (SELECT toInt64(number % 3) - 1 AS x FROM numbers_mt(5000000))
);

SELECT 'all nans', medianExact(nan), quantilesExact(0.1, 0.9)(x), quantileExactInclusive(0.5)(x) FROM (SELECT nan AS x FROM numbers(100000));


-- More than 32 MiB of values: radix selection over several passes.

SELECT 'large f64', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_high,
        quantileExact(0.3)(x) AS q,
        medianExact(x) AS med,
        medianExactLow(x) AS low,
        medianExactHigh(x) AS high,
        arraySort(groupArray(x)) AS s
    FROM (SELECT (toInt64(sipHash64(number) % 2000001) - 1000000) / 7 AS x FROM numbers_mt(5000000))
);

SELECT 'large i32', length(s) > 0
    AND arrayAll((x, l) -> x = s[toUInt64(if(l < 1, floor(l * length(s)), length(s) - 1)) + 1], qs, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_low, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND arrayAll((x, l) -> x = s[toUInt64(if(l = 0.5, intDiv(length(s), 2), if(l < 1, floor(l * length(s)), length(s) - 1))) + 1], qs_high, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1])
    AND q = s[toUInt64(floor(0.3 * length(s))) + 1]
    AND med = s[intDiv(length(s), 2) + 1]
    AND low = s[if(length(s) % 2 = 1, intDiv(length(s), 2), intDiv(length(s), 2) - 1) + 1]
    AND high = s[intDiv(length(s), 2) + 1]
FROM
(
    SELECT
        quantilesExact(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs,
        quantilesExactLow(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_low,
        quantilesExactHigh(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_high,
        quantileExact(0.3)(x) AS q,
        medianExact(x) AS med,
        medianExactLow(x) AS low,
        medianExactHigh(x) AS high,
        arraySort(groupArray(x)) AS s
    FROM (SELECT toInt32(sipHash64(number)) AS x FROM numbers_mt(9000000))
);

SELECT 'large f64 interpolated',
    arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_inc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_inc, arrayMap(h -> toUInt64(floor(h)), hs_inc)))
    AND arrayAll((x, e) -> abs(x - e) <= 1e-12 * greatest(abs(e), 1), qs_exc, arrayMap((h, k) -> if(k >= length(s), toFloat64(s[length(s)]), if(k < 1, toFloat64(s[1]), toFloat64(s[k]) + (h - k) * (toFloat64(s[k + 1]) - toFloat64(s[k])))),
        hs_exc, arrayMap(h -> toUInt64(floor(h)), hs_exc)))
    AND abs(q_inc - qs_inc[6]) <= 1e-12 * greatest(abs(q_inc), 1)
    AND abs(q_exc - qs_exc[4]) <= 1e-12 * greatest(abs(q_exc), 1)
FROM
(
    SELECT *, arrayMap(l -> l * (length(s) - 1) + 1, [0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1]) AS hs_inc, arrayMap(l -> l * (length(s) + 1), [0.001, 0.1, 0.25, 0.5, 0.75, 0.999]) AS hs_exc
    FROM
    (
        SELECT
            quantilesExactInclusive(0, 0.001, 0.1, 0.25, 0.3, 0.5, 0.75, 0.9, 0.999, 1)(x) AS qs_inc,
            quantilesExactExclusive(0.001, 0.1, 0.25, 0.5, 0.75, 0.999)(x) AS qs_exc,
            quantileExactInclusive(0.5)(x) AS q_inc,
            quantileExactExclusive(0.5)(x) AS q_exc,
            arraySort(groupArray(x)) AS s
        FROM (SELECT sipHash64(number) / 3 AS x FROM numbers_mt(5000000))
    )
);


DROP TABLE t_quantile_exact_chunked;
