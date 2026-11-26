--
-- Loose (skip) index scans
--
SET max_parallel_workers_per_gather = 0;

CREATE TABLE skipscan_t (a int, b int, c text);
INSERT INTO skipscan_t
  SELECT g % 100, (g / 100) % 10, 'x' FROM generate_series(1, 200000) g;
CREATE INDEX skipscan_t_a_idx ON skipscan_t (a);
CREATE INDEX skipscan_t_ab_idx ON skipscan_t (a, b);
CREATE INDEX skipscan_t_ab_desc_idx ON skipscan_t (a, b DESC NULLS LAST);
VACUUM ANALYZE skipscan_t;

-- baseline plan
SET enable_skipscan = off;
EXPLAIN (COSTS OFF) SELECT DISTINCT a FROM skipscan_t;

-- loose plans
SET enable_skipscan = on;
EXPLAIN (COSTS OFF) SELECT DISTINCT a FROM skipscan_t;
EXPLAIN (COSTS OFF) SELECT a FROM skipscan_t GROUP BY a;
EXPLAIN (COSTS OFF) SELECT DISTINCT a, b FROM skipscan_t;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (a) a, b FROM skipscan_t ORDER BY a, b;
SELECT count(*) FROM (SELECT DISTINCT b FROM skipscan_t WHERE a = 1) s;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (a) a, b FROM skipscan_t
  ORDER BY a DESC, b DESC;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (a) a, b, c FROM skipscan_t
  ORDER BY a, b;
EXPLAIN (COSTS OFF) SELECT a, min(b) FROM skipscan_t GROUP BY a;
EXPLAIN (COSTS OFF) SELECT a, max(b) FROM skipscan_t GROUP BY a;
EXPLAIN (COSTS OFF) SELECT a, count(*) FROM skipscan_t GROUP BY a;

-- results
SELECT count(*) FROM (SELECT DISTINCT a FROM skipscan_t) s;
SELECT count(*) FROM (SELECT a, b FROM skipscan_t GROUP BY a, b) s;
SELECT count(*) FROM (SELECT DISTINCT ON (a) a, b FROM skipscan_t
                      ORDER BY a, b) s;
SELECT count(*) FROM (SELECT DISTINCT ON (a) a, b FROM skipscan_t
                      ORDER BY a DESC, b DESC) s;
SELECT count(*) FROM (SELECT a, count(*) FROM skipscan_t GROUP BY a) s;
SELECT count(*) FROM (SELECT a, min(b) FROM skipscan_t GROUP BY a) s;
SELECT count(*) FROM (SELECT a, max(b) FROM skipscan_t GROUP BY a) s;
SELECT count(*) FROM (SELECT a, min(b), max(b) FROM skipscan_t GROUP BY a) s;

-- on/off comparison
SET enable_skipscan = off;
CREATE TEMP TABLE skipscan_ref AS SELECT DISTINCT a FROM skipscan_t;
CREATE TEMP TABLE skipscan_ref_bw AS
  SELECT DISTINCT ON (a) a, b FROM skipscan_t ORDER BY a DESC, b DESC;
SET enable_skipscan = on;

SELECT count(*) FROM
  ((SELECT DISTINCT a FROM skipscan_t EXCEPT SELECT * FROM skipscan_ref)
   UNION ALL
   (SELECT * FROM skipscan_ref EXCEPT SELECT DISTINCT a FROM skipscan_t)) s;

SELECT count(*) FROM
  ((SELECT DISTINCT ON (a) a, b FROM skipscan_t ORDER BY a DESC, b DESC
    EXCEPT SELECT * FROM skipscan_ref_bw)
   UNION ALL
   (SELECT * FROM skipscan_ref_bw
    EXCEPT SELECT DISTINCT ON (a) a, b FROM skipscan_t
           ORDER BY a DESC, b DESC)) s;

-- a qpqual must not lose rows (forced onto the index path)
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM (SELECT DISTINCT a FROM skipscan_t WHERE b % 2 = 0) s;
RESET enable_seqscan;
RESET enable_bitmapscan;

DROP TABLE skipscan_t;
