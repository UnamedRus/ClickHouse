-- Regression test: granule-mode Map text index must not drop rows when a single mark
-- is filled by MULTIPLE insert blocks (max_insert_block_size < index_granularity).
-- Before the fix, the `update` call ordinal was used as the chunk number at build time
-- while the data mark number was used at read time, causing chunk drift and silent row drops.

SET allow_experimental_full_text_index = 1;

DROP TABLE IF EXISTS t_multiblock;

-- index_granularity=4: each mark covers 4 rows.
-- max_insert_block_size=2 and min_insert_block_size_rows=1: each INSERT block has 2 rows,
-- so TWO update() calls contribute to every mark. Before the fix, the second update() call
-- would open a new chunk (chunk=1) inside the aggregator while the reader checked chunk=0
-- for mark 0, causing rows contributed by the second block to be silently dropped.
CREATE TABLE t_multiblock (
    id  UInt64,
    m   Map(String, String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 4, index_granularity_bytes = 0;

-- Insert 12 rows with block size 2; each mark has 4 rows filled by 2 blocks.
-- 'needle' is in row 6 (mark 1, second block of that mark).
INSERT INTO t_multiblock SELECT
    number AS id,
    if(number = 6, map('k', 'needle'), map('k', toString(number))) AS m
FROM numbers(12)
SETTINGS max_insert_block_size = 2, min_insert_block_size_rows = 1;

-- Without index: must return row 6.
SELECT id FROM t_multiblock WHERE m['k'] = 'needle' ORDER BY id;

-- With index forced: must also return row 6 (no silent drop from multi-block chunk drift).
SELECT id FROM t_multiblock WHERE m['k'] = 'needle' ORDER BY id
SETTINGS force_data_skipping_indices = 'idx';

DROP TABLE t_multiblock;
