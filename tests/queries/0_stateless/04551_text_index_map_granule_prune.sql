SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=2;
INSERT INTO t SELECT number, map('k', toString(number)) FROM numbers(8);
SELECT trimLeft(explain) FROM (EXPLAIN indexes=1 SELECT count() FROM t WHERE m['k']='5')
WHERE explain LIKE '%Granules:%' AND explain LIKE '%/%'
ORDER BY explain;
DROP TABLE t;
