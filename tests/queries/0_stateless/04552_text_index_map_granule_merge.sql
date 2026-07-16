-- Test: after merging parts, the granule Map text index is rebuilt correctly.
SET allow_experimental_full_text_index = 1;

DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String, String),
    INDEX idx m TYPE text(tokenizer = 'splitByNonAlpha', map_mode = 'granule') GRANULARITY 1)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 4;

INSERT INTO t VALUES (1, {'color': 'red'}), (2, {'color': 'blue'});
INSERT INTO t VALUES (3, {'color': 'red', 'size': 'big'}), (4, {'shape': 'round'});

OPTIMIZE TABLE t FINAL;

-- After FINAL merge there must be exactly one active part.
SELECT count() FROM system.parts WHERE table = 't' AND active AND database = currentDatabase();

-- Correct results without index forcing.
SELECT id FROM t WHERE m['color'] = 'red' ORDER BY id;

-- Correct results with index forced (index must not drop valid granules).
SELECT id FROM t WHERE m['color'] = 'red' ORDER BY id SETTINGS force_data_skipping_indices = 'idx';

DROP TABLE t;
