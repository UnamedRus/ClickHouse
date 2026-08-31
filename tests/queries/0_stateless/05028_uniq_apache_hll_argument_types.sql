-- Tags: no-fasttest
-- ^ DataSketches is not built in fast-test builds.

-- `uniqApacheHLL` accepts what `uniq` accepts. Types the sketch can hash directly are fed to it as
-- an 8-byte integer, an IEEE-754 double or their raw bytes; anything else, and any call with more
-- than one argument, is hashed to a single value by ClickHouse first, exactly as `uniq` does.
-- All counts below are small enough for the sketch to stay in coupon mode, where it is exact.

SELECT 'types the sketch hashes directly';
SELECT uniqApacheHLL(toUInt64(number)) FROM numbers(20);
SELECT uniqApacheHLL(toInt32(number)) FROM numbers(20);
SELECT uniqApacheHLL(toFloat32(number)) FROM numbers(20);
SELECT uniqApacheHLL(toFloat64(number)) FROM numbers(20);
SELECT uniqApacheHLL(toString(number)) FROM numbers(20);
SELECT uniqApacheHLL(toFixedString(toString(number), 8)) FROM numbers(20);
SELECT uniqApacheHLL(toDate('2020-01-01') + number) FROM numbers(20);
SELECT uniqApacheHLL(toDate32('2020-01-01') + number) FROM numbers(20);
SELECT uniqApacheHLL(toDateTime('2020-01-01 00:00:00') + number) FROM numbers(20);
SELECT uniqApacheHLL(reinterpretAsUUID(toUInt128(number))) FROM numbers(20);
SELECT uniqApacheHLL(toIPv4('1.2.3.0') + number) FROM numbers(20);
SELECT uniqApacheHLL(toIPv6(concat('2001:db8::', hex(number + 1)))) FROM numbers(20);
SELECT uniqApacheHLL(toInt128(number)) FROM numbers(20);
SELECT uniqApacheHLL(toUInt256(number)) FROM numbers(20);
SELECT uniqApacheHLL(CAST(number % 3, 'Enum8(\'a\' = 0, \'b\' = 1, \'c\' = 2)')) FROM numbers(20);

SELECT 'types hashed by ClickHouse first';
SELECT uniqApacheHLL(toDecimal64(number, 2)) FROM numbers(20);
SELECT uniqApacheHLL(toDecimal128(number, 4)) FROM numbers(20);
SELECT uniqApacheHLL(toDateTime64(number, 3)) FROM numbers(20);
SELECT uniqApacheHLL(materialize([number])) FROM numbers(20);
SELECT uniqApacheHLL(toNullable(number)) FROM numbers(20);

SELECT 'several arguments';
-- A tuple and the same columns passed separately are hashed the same way, so they agree.
SELECT uniqApacheHLL(number, number + 1) FROM numbers(20);
SELECT uniqApacheHLL((number, number + 1)) FROM numbers(20);
SELECT uniqApacheHLL(number, number, number) FROM numbers(20);
-- Arguments that are not contiguous in memory take the exact 128-bit hash instead.
SELECT uniqApacheHLL(toString(number), number) FROM numbers(20);

SELECT 'the parameters still apply';
SELECT uniqApacheHLL(8)(reinterpretAsUUID(toUInt128(number)))  FROM numbers(20);
SELECT toTypeName(uniqApacheHLLState(8, 'HLL_6')(number, number + 1)) FROM numbers(1);

SELECT 'no arguments is still an error';
SELECT uniqApacheHLL() FROM numbers(1); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
