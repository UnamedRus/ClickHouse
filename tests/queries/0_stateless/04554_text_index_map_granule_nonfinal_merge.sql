-- Test: after a non-FINAL merge the granule Map text index is rebuilt correctly.
-- Note: 'OPTIMIZE TABLE t' (without FINAL) merges parts but does not guarantee a single
-- output part. We use min_bytes_for_wide_part=0 so both inserts produce wide parts,
-- making a background merge more likely. The correctness check is the important part.
SET allow_experimental_full_text_index = 1;

DROP TABLE IF EXISTS t;
CREATE TABLE t (id UInt64, m Map(String, String),
    INDEX idx m TYPE text(tokenizer='splitByNonAlpha', map_mode='granule') GRANULARITY 1)
ENGINE=MergeTree ORDER BY id
SETTINGS index_granularity=4, min_bytes_for_wide_part=0;

INSERT INTO t VALUES (1,{'color':'red'}),(2,{'color':'blue'});
INSERT INTO t VALUES (3,{'color':'red','size':'big'}),(4,{'shape':'round'});

-- Two active parts before the merge.
SELECT count() FROM system.parts WHERE table='t' AND active AND database=currentDatabase();

-- Non-FINAL optimize: merges parts but may leave more than one part depending on the
-- merge selector. Either way the index must remain correct for surviving parts.
OPTIMIZE TABLE t;

-- Results must be correct regardless of how many parts remain after the merge.
SELECT id FROM t WHERE m['color']='red' ORDER BY id;

-- Repeat with the index forced to confirm the index does not suppress valid granules.
SELECT id FROM t WHERE m['color']='red' ORDER BY id SETTINGS force_data_skipping_indices='idx';

DROP TABLE t;
