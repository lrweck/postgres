--
-- Measure the per-comparison cost of a sort, so that the numbers in the
-- discussion of this patch can be reproduced on another machine.
--
-- Usage:
--
--	psql -d some_scratch_database -f src/tools/measure_sort_cost.sql
--
-- The database must use UTF8: the locale matrices sort accented Latin, Turkish,
-- Cyrillic and CJK data, and chr() rejects those code points in SQL_ASCII.
--
-- Creates five tables and drops them at the end: two of 1,000,000 rows shaped
-- like the ones used for the type matrix (326 bytes per row) and the collation
-- matrices (435 bytes per row), one of 1,000,000 rows for the width curve, one
-- of 1,000,000 rows for the multi-column check, and one each of 100,000,
-- 1,000,000 and 4,000,000 rows for the size scaling.  Needs work_mem of 2GB to
-- keep the sorts in memory, about 2 GB of disk, and several minutes.
--
-- Every number is the minimum of three runs of the total Execution Time of
--
--	SELECT count(*) FROM (SELECT * FROM <table> ORDER BY <key>) x
--
-- which is what the numbers in the description used; the Sort stores only the
-- key, but the row width still decides how much of each tuple the executor
-- carries, so the table shape matters and is fixed here.
--
-- The run-count estimate is not measured by this script: it needs trace_sort
-- and access to the server log.  With trace_sort enabled,
--
--	SET trace_sort = on;
--	SET work_mem = '1MB';
--	EXPLAIN (ANALYZE) SELECT * FROM sort_cost_types ORDER BY tx100;
--
-- count the "finished writing run" lines in the server log and compare with
-- (input_bytes + ntuples * sizeof(SortTuple)) / work_mem.
--
\set ON_ERROR_STOP on

SET client_min_messages = warning;
SET work_mem = '2GB';
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = 0;

DROP TABLE IF EXISTS sort_cost_types;
CREATE TABLE sort_cost_types AS
SELECT g AS i4ref,
       g::bigint AS i8,
       g::float8 + 0.5 AS f8,
       date '2000-01-01' + g AS dt,
       timestamp '2000-01-01' + (g || ' seconds')::interval AS ts,
       timestamptz '2000-01-01 00:00:00+00' + (g || ' seconds')::interval AS tstz,
       (time '00:00' + (g || ' microseconds')::interval)::time AS tm,
       (g || ' seconds')::interval AS iv,
       g::numeric AS nm,
       md5(g::text)::uuid AS uu,
       ('10.0.0.' || (g % 250))::inet AS ine,
       ('v' || lpad(g::text, 9, '0'))::char(10) AS bp10,
       ('v' || lpad(g::text, 9, '0'))::varchar(10) AS vc10,
       ('v' || lpad(g::text, 9, '0'))::name AS nm_name,
       'v' || lpad(g::text, 10, '0') || repeat('x', 88) AS tx100,
       repeat('p', 21) AS pad
FROM generate_series(1, 1000000) g;
ANALYZE sort_cost_types;

DROP TABLE IF EXISTS sort_cost_text;
CREATE TABLE sort_cost_text AS
SELECT g AS i4,
       substr(md5(g::text), 1, 10) AS t10,
       substr(repeat(md5(g::text), 3), 1, 40) AS t40,
       substr(repeat(md5(g::text), 4), 1, 100) AS t100,
       substr(md5((g % 10000)::text), 1, 10) AS t10d,
       substr(repeat(md5((g % 10000)::text), 4), 1, 100) AS t100d,
       substr(md5(g::text), 1, 16) AS ascii,
       chr(192 + (g % 60)) || substr(md5(g::text), 1, 15) AS latin,
       substr('iI' || chr(305) || chr(304), (g % 4) + 1, 1) || substr(md5(g::text), 1, 15) AS turk,
       chr(1040 + (g % 64)) || substr(md5(g::text), 1, 15) AS cyr,
       chr(19968 + (g % 1000)) || substr(md5(g::text), 1, 15) AS cjk,
       repeat('p', 50) AS pad
FROM generate_series(1, 1000000) g;
ANALYZE sort_cost_text;

DROP FUNCTION IF EXISTS sort_cost_ms(text);
DROP FUNCTION IF EXISTS sort_cost_ms(text, text);

--
-- Minimum Execution Time, in milliseconds, of ordering the given table by the
-- given key expression.
--
CREATE FUNCTION sort_cost_ms(tbl text, key text) RETURNS float8
LANGUAGE plpgsql AS
$$
DECLARE
    best float8 := 'Infinity';
    ln text;
BEGIN
    FOR i IN 1..3 LOOP
        FOR ln IN EXECUTE format('EXPLAIN (ANALYZE, TIMING ON) '
                                 'SELECT count(*) FROM (SELECT * FROM %I ORDER BY %s) x',
                                 tbl, key) LOOP
            IF ln LIKE 'Execution Time: %' THEN
                best := least(best, split_part(ln, ' ', 3)::float8);
            END IF;
        END LOOP;
    END LOOP;
    RETURN best;
END
$$;

SELECT sort_cost_ms('sort_cost_types', 'i4ref') AS int4_ms \gset

\echo
\echo '== comparison cost by type, ratio to int4 =='
SELECT t.type,
       CASE WHEN t.key = 'i4ref' THEN round(:int4_ms::numeric, 0)
            ELSE round(s.ms::numeric, 0) END AS ms,
       CASE WHEN t.key = 'i4ref' THEN 1.00
            ELSE round((s.ms / :int4_ms)::numeric, 2) END AS vs_int4
  FROM (VALUES ('date', 'dt'), ('int4', 'i4ref'), ('float8', 'f8'), ('int8', 'i8'),
               ('timestamp', 'ts'), ('timestamptz', 'tstz'), ('numeric', 'nm'),
               ('interval', 'iv'), ('bpchar(10)', 'bp10'), ('varchar(10)', 'vc10'),
               ('time', 'tm'), ('uuid', 'uu'), ('text(100)', 'tx100'), ('name', 'nm_name'))
       AS t(type, key),
       LATERAL (SELECT CASE WHEN t.key = 'i4ref' THEN :int4_ms
                            ELSE sort_cost_ms('sort_cost_types', t.key) END AS ms) s;

\echo
\echo '== comparison cost by collation provider, ratio to int4 =='
SELECT t.cell, round(s.ms::numeric, 0) AS ms, round((s.ms / :int4_ms)::numeric, 2) AS vs_int4
  FROM (VALUES ('text(10), C', 't10 COLLATE "C"'),
               ('text(10), builtin', 't10 COLLATE ucs_basic'),
               ('text(40), C', 't40 COLLATE "C"'),
               ('text(100), C', 't100 COLLATE "C"'),
               ('text(10) 10k distinct, C', 't10d COLLATE "C"'),
               ('text(100) 10k distinct, C', 't100d COLLATE "C"'))
       AS t(cell, key),
       LATERAL (SELECT sort_cost_ms('sort_cost_text', t.key) AS ms) s;

SELECT EXISTS (SELECT 1 FROM pg_collation
               WHERE collname = 'en-x-icu' AND collprovider = 'i') AS have_icu
\gset

\if :have_icu
\echo
\echo '== ICU against the C collation, same data =='
SELECT t.data, round(s.c_ms::numeric, 0) AS c_ms, round(s.icu_ms::numeric, 0) AS icu_ms,
       round((s.icu_ms / s.c_ms)::numeric, 2) AS icu_over_c
  FROM (VALUES ('ASCII, en-US', 'ascii', 'en-x-icu'),
               ('accented Latin, en-US', 'latin', 'en-x-icu'),
               ('accented Latin, pt-BR', 'latin', 'pt-BR-x-icu'),
               ('Turkish, tr-TR', 'turk', 'tr-TR-x-icu'),
               ('Cyrillic, ru-RU', 'cyr', 'ru-RU-x-icu'),
               ('CJK, zh-Hans', 'cjk', 'zh-Hans-x-icu'))
       AS t(data, key, locale),
       LATERAL (SELECT sort_cost_ms('sort_cost_text', t.key || ' COLLATE "C"') AS c_ms,
                       sort_cost_ms('sort_cost_text', t.key || ' COLLATE "' || t.locale || '"') AS icu_ms) s;

\echo
\echo '== ICU against the C collation, by key width and cardinality =='
SELECT t.cell, round(s.c_ms::numeric, 0) AS c_ms, round(s.icu_ms::numeric, 0) AS icu_ms,
       round((s.icu_ms / s.c_ms)::numeric, 2) AS icu_over_c
  FROM (VALUES ('text(10)', 't10'), ('text(100)', 't100'),
               ('text(10) 10k distinct', 't10d'),
               ('text(100) 10k distinct', 't100d'))
       AS t(cell, key),
       LATERAL (SELECT sort_cost_ms('sort_cost_text', t.key || ' COLLATE "C"') AS c_ms,
                       sort_cost_ms('sort_cost_text', t.key || ' COLLATE "en-x-icu"') AS icu_ms) s;
\endif

--
-- The three effects the model does not express.  These need their own tables
-- because the number depends on the row shape and on the data, not only on the
-- type.
--
SET jit = off;

DROP TABLE IF EXISTS sort_cost_width;
CREATE TABLE sort_cost_width AS
SELECT g AS i4,
       substr(m, 1, 8)   AS t8,
       substr(m, 1, 16)  AS t16,
       substr(m, 1, 32)  AS t32,
       substr(m, 1, 64)  AS t64,
       substr(m, 1, 128) AS t128,
       substr(m, 1, 256) AS t256,
       'PREFIX1234' || substr(m, 1, 22)  AS p32,
       'PREFIX1234' || substr(m, 1, 118) AS p128,
       repeat('p', 40) AS pad
FROM (SELECT g,
             md5(g::text) || md5((g + 7)::text) || md5((g + 13)::text) ||
             md5((g + 19)::text) || md5((g + 23)::text) || md5((g + 29)::text) ||
             md5((g + 31)::text) || md5((g + 37)::text) AS m
      FROM generate_series(1, 1000000) g) s;
ANALYZE sort_cost_width;

SELECT sort_cost_ms('sort_cost_width', 'i4') AS width_i4_ms \gset

\echo
\echo '== width, COLLATE C, random data and with a shared 10-char prefix =='
SELECT t.cell, round(s.ms::numeric, 0) AS ms,
       round((s.ms / :width_i4_ms)::numeric, 2) AS vs_int4
  FROM (VALUES ('8 B', 't8 COLLATE "C"'),
               ('16 B', 't16 COLLATE "C"'),
               ('32 B', 't32 COLLATE "C"'),
               ('64 B', 't64 COLLATE "C"'),
               ('128 B', 't128 COLLATE "C"'),
               ('256 B', 't256 COLLATE "C"'),
               ('32 B, 10-char shared prefix', 'p32 COLLATE "C"'),
               ('128 B, 10-char shared prefix', 'p128 COLLATE "C"'))
       AS t(cell, key),
       LATERAL (SELECT sort_cost_ms('sort_cost_width', t.key) AS ms) s;

DROP TABLE IF EXISTS sort_cost_multi;
CREATE TABLE sort_cost_multi AS
SELECT g AS i4,
       md5(g::text)::uuid AS u1,
       md5((g + 11)::text)::uuid AS u2,
       md5((g + 22)::text)::uuid AS u3,
       md5((g + 33)::text)::uuid AS u4,
       '00000000-0000-0000-0000-000000000001'::uuid AS c1,
       '00000000-0000-0000-0000-000000000002'::uuid AS c2,
       repeat('p', 40) AS pad
FROM generate_series(1, 1000000) g;
ANALYZE sort_cost_multi;

SELECT sort_cost_ms('sort_cost_multi', 'i4') AS multi_i4_ms \gset

\echo
\echo '== multi-column uuid, one key against four =='
SELECT t.cell, round(s.ms::numeric, 0) AS ms,
       round((s.ms / :multi_i4_ms)::numeric, 2) AS vs_int4
  FROM (VALUES ('1 key', 'u1'), ('2 keys', 'u1, u2'),
               ('3 keys', 'u1, u2, u3'), ('4 keys', 'u1, u2, u3, u4'),
               ('constant + 1 key', 'c1, u1'))
       AS t(cell, key),
       LATERAL (SELECT sort_cost_ms('sort_cost_multi', t.key) AS ms) s;

--
-- Size scaling: one table per size so that the ratio can be read across row
-- counts.  The row shape is the same for all three.
--
DROP TABLE IF EXISTS sort_cost_100k, sort_cost_1m, sort_cost_4m;
CREATE TABLE sort_cost_100k AS
    SELECT g AS i4, md5(g::text)::uuid AS uu, g::numeric AS nm, repeat('p', 60) AS pad
      FROM generate_series(1, 100000) g;
CREATE TABLE sort_cost_1m AS
    SELECT g AS i4, md5(g::text)::uuid AS uu, g::numeric AS nm, repeat('p', 60) AS pad
      FROM generate_series(1, 1000000) g;
CREATE TABLE sort_cost_4m AS
    SELECT g AS i4, md5(g::text)::uuid AS uu, g::numeric AS nm, repeat('p', 60) AS pad
      FROM generate_series(1, 4000000) g;
ANALYZE sort_cost_100k;
ANALYZE sort_cost_1m;
ANALYZE sort_cost_4m;

\echo
\echo '== size scaling, ratio to int4 at the same row count =='
SELECT t.tbl,
       round((sort_cost_ms(t.tbl, 'uu') / sort_cost_ms(t.tbl, 'i4'))::numeric, 2) AS uuid_vs_int4,
       round((sort_cost_ms(t.tbl, 'nm') / sort_cost_ms(t.tbl, 'i4'))::numeric, 2) AS numeric_vs_int4
  FROM (VALUES ('sort_cost_100k'), ('sort_cost_1m'), ('sort_cost_4m')) t(tbl);

DROP TABLE sort_cost_width;
DROP TABLE sort_cost_multi;
DROP TABLE sort_cost_100k;
DROP TABLE sort_cost_1m;
DROP TABLE sort_cost_4m;

DROP TABLE sort_cost_types;
DROP TABLE sort_cost_text;
DROP FUNCTION sort_cost_ms(text, text);
