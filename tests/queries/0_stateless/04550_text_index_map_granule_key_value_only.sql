SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String,String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id SETTINGS index_granularity=4;
INSERT INTO t VALUES (1,{'color':'red'}),(2,{'size':'big'}),(3,{'color':'blue','shape':'round'});
SELECT id FROM t WHERE mapContains(m,'shape') ORDER BY id;              -- 3
SELECT id FROM t WHERE has(mapValues(m),'big') ORDER BY id;            -- 2
SELECT id FROM t WHERE m['color'] IN ('red','blue') ORDER BY id;       -- 1,3
DROP TABLE t;
