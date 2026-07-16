SET allow_experimental_full_text_index = 1;

DROP TABLE IF EXISTS t_mg_create;

-- granule mode accepted on Map(String,String)
CREATE TABLE t_mg_create
(
    id UInt64,
    m Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id;
SELECT 'created';

-- granule mode rejected on a non-Map column
CREATE TABLE t_mg_bad
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id; -- { serverError BAD_ARGUMENTS }

-- unknown map_mode rejected
CREATE TABLE t_mg_bad2
(
    id UInt64,
    m Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'nope') GRANULARITY 1
) ENGINE = MergeTree ORDER BY id; -- { serverError BAD_ARGUMENTS }

DROP TABLE t_mg_create;
