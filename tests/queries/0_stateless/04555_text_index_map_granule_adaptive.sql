-- Regression test: granule-mode Map text index must not drop rows under adaptive granularity.
-- When index_granularity_bytes is small and rows are wide, marks hold fewer rows than
-- index_granularity (the setting). Before the fix, chunk boundaries were computed from the
-- setting-based row count at BUILD time but from getMarkStartingRow(1) at READ time, causing
-- misattribution and silent row drops for matches in later marks.

SET allow_experimental_full_text_index = 1;

DROP TABLE IF EXISTS t_adaptive_granule;

-- Use index_granularity=8192 (the default), but index_granularity_bytes=1024 so that rows
-- with a long 'pad' column force the byte cap to bind. At ~2100 bytes per row, marks
-- will hold roughly 1 row each — far fewer than 8192 — making the adaptive effect strong.
-- We insert 12 rows so the index covers multiple marks, with the matching value ('needle')
-- intentionally placed in the last rows (later marks) to expose the bug.
CREATE TABLE t_adaptive_granule (
    id   UInt64,
    pad  String,
    m    Map(String, String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 8192, index_granularity_bytes = 1024;

-- Each row is ~2100 bytes (pad fills ~2048 bytes) so the byte cap fires after roughly 1 row per mark.
-- Rows 10 and 11 have the matching entry m['color']='needle'; they fall in later marks.
INSERT INTO t_adaptive_granule SELECT
    number AS id,
    repeat('x', 2048) AS pad,
    if(number >= 10, map('color', 'needle', 'size', 'large'), map('color', 'other', 'size', 'small')) AS m
FROM numbers(12);

-- Without index: should return 10 and 11.
SELECT id FROM t_adaptive_granule WHERE m['color'] = 'needle' ORDER BY id;

-- With index forced: must return the same 10 and 11 (no silent row drops).
SELECT id FROM t_adaptive_granule WHERE m['color'] = 'needle' ORDER BY id
SETTINGS force_data_skipping_indices = 'idx';

DROP TABLE t_adaptive_granule;
