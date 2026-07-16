-- Edge cases for granule Map text index:
--   (1) FixedString keys with an empty-map row,
--   (2) IN list containing the default value ('' for String values).
SET allow_experimental_full_text_index = 1;

DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(FixedString(3), String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;

-- Row 2 has an empty map: the key 'abc' is absent and m['abc'] returns the default ''.
INSERT INTO t VALUES (1,{'abc':'red'}),(2,{}),(3,{'abc':'blue'});

-- Only row 3 has the key 'abc' with value 'blue'.
SELECT id FROM t WHERE m['abc']='blue' ORDER BY id;

-- IN list contains the default value '' so the index must not prune:
-- row 2 returns '' (key absent), row 3 returns 'blue' — both satisfy m['abc'] IN ('blue','').
SELECT id FROM t WHERE m['abc'] IN ('blue','') ORDER BY id;

DROP TABLE t;
