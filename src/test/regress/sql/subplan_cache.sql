--
-- Caching of correlated subplan results
--
CREATE TABLE spc_t (a int, b int);
INSERT INTO spc_t SELECT g % 10, g FROM generate_series(1, 100) g;
ANALYZE spc_t;

-- The cache must not change the result: compare it with the cache off.
SET enable_subplan_cache = on;
CREATE TEMP TABLE spc_on AS
  SELECT a, (SELECT sum(b) FROM spc_t t2 WHERE t2.a = t1.a) AS s
    FROM spc_t t1 ORDER BY a;
SET enable_subplan_cache = off;
CREATE TEMP TABLE spc_off AS
  SELECT a, (SELECT sum(b) FROM spc_t t2 WHERE t2.a = t1.a) AS s
    FROM spc_t t1 ORDER BY a;
SELECT count(*) AS diffs
  FROM spc_on o FULL JOIN spc_off f USING (a, s)
 WHERE o.a IS NULL OR f.a IS NULL;

-- EXISTS subplans are cached as well; the result must match.
SET enable_subplan_cache = on;
SELECT count(*) FROM spc_t t1
 WHERE EXISTS (SELECT 1 FROM spc_t t2 WHERE t2.a = t1.a AND t2.b > 50);
SET enable_subplan_cache = off;
SELECT count(*) FROM spc_t t1
 WHERE EXISTS (SELECT 1 FROM spc_t t2 WHERE t2.a = t1.a AND t2.b > 50);

-- A volatile subplan must not be cached: nextval() advances on every row.
SET enable_subplan_cache = on;
CREATE SEQUENCE spc_seq;
SELECT count(DISTINCT (SELECT nextval('spc_seq') * 1000 + t1.a)) AS distinct_vals
  FROM spc_t t1;
DROP SEQUENCE spc_seq;

RESET enable_subplan_cache;
DROP TABLE spc_t;
