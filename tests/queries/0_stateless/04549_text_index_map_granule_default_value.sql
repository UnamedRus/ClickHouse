SET allow_experimental_full_text_index = 1;

-- Test 1: Map(String, String) — default is empty string "".
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red'}),(2,{'size':'big'}),(3,{'color':''}),(4,{'shape':'x'});
-- m['color']='' is TRUE for rows lacking 'color' (2,4) AND the explicit '' (3). Index must NOT prune.
SELECT id FROM t WHERE m['color']='' ORDER BY id;   -- 2,3,4
DROP TABLE t;

-- Test 2: Map(String, FixedString(3)) — default is 3 zero bytes '\0\0\0'.
-- Rows: (1) key present, non-zero value; (2) key absent; (3) key present, explicit zero value.
-- Querying m['k'] = '\0\0\0' must return rows 2 and 3 (must NOT prune row 2 which lacks the key).
DROP TABLE IF EXISTS t2;
CREATE TABLE t2 (id UInt64, m Map(String, FixedString(3)),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t2 VALUES (1, {'k': 'abc'}), (2, {}), (3, {'k': '\0\0\0'});
-- m['k'] for row 2 (absent key) returns '\0\0\0' — must NOT be pruned.
SELECT id FROM t2 WHERE m['k'] = toFixedString('\0\0\0', 3) ORDER BY id;  -- 2,3
-- IN variant: same carve-out must fire.
SELECT id FROM t2 WHERE m['k'] IN (toFixedString('\0\0\0', 3)) ORDER BY id;  -- 2,3
DROP TABLE t2;
