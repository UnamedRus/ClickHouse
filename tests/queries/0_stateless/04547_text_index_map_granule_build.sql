SET allow_experimental_full_text_index = 1;
DROP TABLE IF EXISTS t_mg_build;
CREATE TABLE t_mg_build
(
    id UInt64,
    m Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 4;

INSERT INTO t_mg_build VALUES
    (1, {'color':'red','size':'big'}),
    (2, {'color':'red'}),                 -- duplicate pair color=red collapses within the granule
    (3, {'color':'blue','shape':'round'}),
    (4, {'color':'red'});

SELECT count() FROM t_mg_build;           -- 4, index build did not corrupt the part
SELECT count() FROM system.data_skipping_indices WHERE table = 't_mg_build' AND name = 'idx'; -- 1
DROP TABLE t_mg_build;
