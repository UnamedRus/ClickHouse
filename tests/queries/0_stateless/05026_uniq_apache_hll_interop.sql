-- Tags: no-fasttest
-- ^ DataSketches is not built in fast-test builds.

-- The state of `uniqApacheHLL` carries an Apache DataSketches HLL sketch in its native serialized
-- form, framed with the varint length prefix that `writeVectorBinary` adds. Every blob below was
-- produced by the Apache DataSketches C++ library directly rather than by ClickHouse, so these
-- queries pin the wire contract with external producers in both directions. A change that breaks
-- interoperability has to change one of these literals.

SELECT 'import sketches built outside ClickHouse';

-- lg_k = 12, HLL_4, `hll_sketch::update(uint64_t)` over 0..4. Coupon list mode.
SELECT finalizeAggregation(CAST(unhex('1C0201070C03080500CBD7C2042BF2FB06862FF90D7581660781BC5D06'), 'AggregateFunction(uniqApacheHLL, UInt64)'));

-- The same sketch reached through `-Merge`.
SELECT uniqApacheHLLMerge(s) FROM (SELECT CAST(unhex('1C0201070C03080500CBD7C2042BF2FB06862FF90D7581660781BC5D06'), 'AggregateFunction(uniqApacheHLL, UInt64)') AS s);

-- Two external sketches over 0..4 and 3..7 union to 8 distinct values.
SELECT uniqApacheHLLMerge(s) FROM
(
    SELECT CAST(unhex('1C0201070C03080500CBD7C2042BF2FB06862FF90D7581660781BC5D06'), 'AggregateFunction(uniqApacheHLL, UInt64)') AS s
    UNION ALL
    SELECT CAST(unhex('1C0201070C030805007581660781BC5D067B65E608FC2D420AC1E91705'), 'AggregateFunction(uniqApacheHLL, UInt64)') AS s
);

-- An external sketch merged with one built by ClickHouse: the two hash identically, so 0..4 and
-- 3..7 overlap on 3 and 4 and the union is 8.
SELECT uniqApacheHLLMerge(s) FROM
(
    SELECT CAST(unhex('1C0201070C03080500CBD7C2042BF2FB06862FF90D7581660781BC5D06'), 'AggregateFunction(uniqApacheHLL, UInt64)') AS s
    UNION ALL
    SELECT uniqApacheHLLState(number) AS s FROM numbers(3, 5)
);

-- lg_k = 12, HLL_4, raw bytes of 'alpha', 'beta', 'gamma'.
SELECT finalizeAggregation(CAST(unhex('140201070C03080300BD3A090A8E5A62115168C90A'), 'AggregateFunction(uniqApacheHLL, UInt64)'));

-- lg_k = 4, HLL_4, 100 distinct values. Dense HLL mode rather than a coupon list.
SELECT finalizeAggregation(CAST(unhex('300A0107040008020215EB1DC787F15440000000000000FB3F000000000000000003000000000000000251214121031025'), 'AggregateFunction(uniqApacheHLL(4), UInt64)'));

-- lg_k = 14, HLL_8: the non-default parameters are carried by the state type.
SELECT finalizeAggregation(CAST(unhex('1C0201070E03080508CBD7C2042BF2FB06862FF90D7581660781BC5D06'), 'AggregateFunction(uniqApacheHLL(14, \'HLL_8\'), UInt64)'));

-- An externally produced empty sketch.
SELECT finalizeAggregation(CAST(unhex('080201070C030C0000'), 'AggregateFunction(uniqApacheHLL, UInt64)'));

SELECT 'export sketches for consumption outside ClickHouse';

-- `max_threads` is pinned because a state that was merged from several partial states may lay its
-- coupons out in a different order than a state built by a single thread.
SELECT hex(toString(uniqApacheHLLState(number))) = '1C0201070C03080500CBD7C2042BF2FB06862FF90D7581660781BC5D06' FROM numbers(5) SETTINGS max_threads = 1;
SELECT hex(toString(uniqApacheHLLState(14, 'HLL_8')(number))) = '1C0201070E03080508CBD7C2042BF2FB06862FF90D7581660781BC5D06' FROM numbers(5) SETTINGS max_threads = 1;

-- Importing an external sketch and exporting it again must reproduce it byte for byte.
SELECT hex(toString(uniqApacheHLLMergeState(s))) = '1C0201070C03080500CBD7C2042BF2FB06862FF90D7581660781BC5D06'
FROM (SELECT CAST(unhex('1C0201070C03080500CBD7C2042BF2FB06862FF90D7581660781BC5D06'), 'AggregateFunction(uniqApacheHLL, UInt64)') AS s)
SETTINGS max_threads = 1;

SELECT 'states survive a write/read cycle through storage';

DROP TABLE IF EXISTS hll_interop_states;
CREATE TABLE hll_interop_states (k UInt8, s AggregateFunction(uniqApacheHLL, UInt64)) ENGINE = AggregatingMergeTree ORDER BY k;
INSERT INTO hll_interop_states SELECT number % 4 AS k, uniqApacheHLLState(number) FROM numbers(20) GROUP BY k;
OPTIMIZE TABLE hll_interop_states FINAL;
-- Small enough to stay in coupon mode, so the union is exact regardless of how it was partitioned.
SELECT uniqApacheHLLMerge(s) FROM hll_interop_states;
-- A state read back from disk is still a valid sketch for an external consumer.
SELECT countDistinct(hex(toString(s))) = 4 FROM hll_interop_states;
DROP TABLE hll_interop_states;

SELECT 'malformed states are rejected';

-- A well-formed length prefix followed by a payload that is not a sketch. `datasketches` reports
-- this as `std::invalid_argument`, which must surface as `CORRUPTED_DATA` rather than escaping as
-- a logical error.
SELECT finalizeAggregation(CAST(unhex('08FFFFFFFFFFFFFFFF'), 'AggregateFunction(uniqApacheHLL, UInt64)')); -- { serverError CORRUPTED_DATA }
SELECT finalizeAggregation(CAST(unhex('0801020304050607FF'), 'AggregateFunction(uniqApacheHLL, UInt64)')); -- { serverError CORRUPTED_DATA }
