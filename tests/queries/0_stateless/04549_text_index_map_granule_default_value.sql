SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red'}),(2,{'size':'big'}),(3,{'color':''}),(4,{'shape':'x'});
-- m['color']='' is TRUE for rows lacking 'color' (2,4) AND the explicit '' (3). Index must NOT prune.
SELECT id FROM t WHERE m['color']='' ORDER BY id;   -- 2,3,4
DROP TABLE t;
