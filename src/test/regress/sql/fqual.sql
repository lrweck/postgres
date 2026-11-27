--
-- Functional qual pushdown
--
SET enable_seqscan = off;

CREATE TABLE fq (i int, n numeric, f float8, s text, t timestamp);
INSERT INTO fq
  SELECT g, (g % 100) + 0.5, (g % 100) + 0.5, 's' || (g % 100),
         timestamp '2012-01-01' + g * interval '5 minutes'
  FROM generate_series(1, 100000) g;
CREATE INDEX fq_i ON fq (i);
CREATE INDEX fq_n ON fq (n);
CREATE INDEX fq_f ON fq (f);
CREATE INDEX fq_s ON fq (s);
CREATE INDEX fq_t ON fq (t);
VACUUM ANALYZE fq;

-- CASE: the branch whose value is c is the only one that can produce it
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE CASE WHEN i = 42 THEN 1 ELSE 0 END = 1;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq
   WHERE CASE WHEN i = 42 THEN 1 WHEN i = 43 THEN 2 ELSE 0 END = 2;
-- ELSE matching is the complement, not an indexable predicate
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE CASE WHEN i = 42 THEN 1 ELSE 2 END = 2;

-- COALESCE / NULLIF
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE COALESCE(s, 'x') = 's42';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE COALESCE(s, 's42') = 's42';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE NULLIF(s, 'zzz') = 's42';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE NULLIF(s, 's42') = 's42';

-- date/time
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE date(t) = '2012-01-02';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE date_trunc('day', t) = '2012-01-02';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE date_trunc('month', t) = '2012-01-01';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE extract(year FROM t) = 2012;

-- abs / sign / floor / ceil / trunc
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE abs(i) = 42;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE sign(n) = 1;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE floor(n) = 3;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE ceil(n) = 3;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE trunc(n) = 3;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE floor(f) = 3;

-- arithmetic
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE i + 5 = 42;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE 5 - i = 42;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE i * 3 = 42;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE -i = -42;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE n + 0.5 = 3.0;

-- not derivable: opaque functions
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE lower(s) = 's42';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE upper(s) = 'S42';
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE length(s) = 3;
EXPLAIN (COSTS OFF)
  SELECT count(*) FROM fq WHERE md5(s) = md5('s42');

-- results: same with the derivation off
SET enable_seqscan = on;

SELECT 'case' AS q, count(*) FROM fq
 WHERE CASE WHEN i = 42 THEN 1 ELSE 0 END = 1
UNION ALL
SELECT 'coalesce', count(*) FROM fq WHERE COALESCE(s, 's42') = 's42'
UNION ALL
SELECT 'nullif', count(*) FROM fq WHERE NULLIF(s, 's42') = 's42'
UNION ALL
SELECT 'date', count(*) FROM fq WHERE date(t) = '2012-01-02'
UNION ALL
SELECT 'date_trunc', count(*) FROM fq WHERE date_trunc('month', t) = '2012-01-01'
UNION ALL
SELECT 'extract', count(*) FROM fq WHERE extract(year FROM t) = 2012
UNION ALL
SELECT 'abs', count(*) FROM fq WHERE abs(i) = 42
UNION ALL
SELECT 'floor', count(*) FROM fq WHERE floor(n) = 3
UNION ALL
SELECT 'arith_add', count(*) FROM fq WHERE i + 5 = 42
UNION ALL
SELECT 'arith_mul', count(*) FROM fq WHERE i * 3 = 42
UNION ALL
SELECT 'arith_neg', count(*) FROM fq WHERE -i = -42
ORDER BY 1;

SET enable_functional_quals = off;

SELECT 'case' AS q, count(*) FROM fq
 WHERE CASE WHEN i = 42 THEN 1 ELSE 0 END = 1
UNION ALL
SELECT 'coalesce', count(*) FROM fq WHERE COALESCE(s, 's42') = 's42'
UNION ALL
SELECT 'nullif', count(*) FROM fq WHERE NULLIF(s, 's42') = 's42'
UNION ALL
SELECT 'date', count(*) FROM fq WHERE date(t) = '2012-01-02'
UNION ALL
SELECT 'date_trunc', count(*) FROM fq WHERE date_trunc('month', t) = '2012-01-01'
UNION ALL
SELECT 'extract', count(*) FROM fq WHERE extract(year FROM t) = 2012
UNION ALL
SELECT 'abs', count(*) FROM fq WHERE abs(i) = 42
UNION ALL
SELECT 'floor', count(*) FROM fq WHERE floor(n) = 3
UNION ALL
SELECT 'arith_add', count(*) FROM fq WHERE i + 5 = 42
UNION ALL
SELECT 'arith_mul', count(*) FROM fq WHERE i * 3 = 42
UNION ALL
SELECT 'arith_neg', count(*) FROM fq WHERE -i = -42
ORDER BY 1;


-- a column with no index gets no derived predicate (no extra filter)
CREATE TABLE fq_noidx (i int);
INSERT INTO fq_noidx SELECT g FROM generate_series(1, 1000) g;
VACUUM ANALYZE fq_noidx;
EXPLAIN (COSTS OFF) SELECT count(*) FROM fq_noidx WHERE abs(i) = 1;
DROP TABLE fq_noidx;

RESET enable_functional_quals;
RESET enable_seqscan;

DROP TABLE fq;
