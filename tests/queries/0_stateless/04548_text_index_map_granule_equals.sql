SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red','size':'big'}),(2,{'color':'blue'}),(3,{'shape':'round','color':'red'}),(4,{'size':'red'});
-- co-occurrence trap: row 4 has value 'red' but under key 'size', not 'color'
SELECT id FROM t WHERE m['color']='red' ORDER BY id;                          -- 1,3
SELECT id FROM t WHERE m['color']='red' ORDER BY id SETTINGS force_data_skipping_indices='idx'; -- 1,3
DROP TABLE t;
