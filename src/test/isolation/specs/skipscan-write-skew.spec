# Loose (skip) scan under SERIALIZABLE: a skipped row moved to a new value
# must conflict with the reader, even though the reader never visited that
# row's heap page.

setup
{
  CREATE TABLE ssi_t (a text);
  INSERT INTO ssi_t
    SELECT (array['a','c','e'])[(g % 3) + 1] FROM generate_series(1, 30000) g;
  CREATE INDEX ssi_t_a_idx ON ssi_t (a);
  ANALYZE ssi_t;
  CREATE TABLE ssi_log (n int);
}

teardown
{
  DROP TABLE ssi_t;
  DROP TABLE ssi_log;
}

session s1
step s1_begin  { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_read   { SELECT count(DISTINCT a) FROM ssi_t; }
step s1_write  { INSERT INTO ssi_log VALUES (1); }
step s1_commit { COMMIT; }

session s2
step s2_begin  { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read   { SELECT count(*) FROM ssi_log; }
step s2_update { UPDATE ssi_t SET a = 'b'
                 WHERE ctid = (SELECT ctid FROM ssi_t WHERE a = 'a' LIMIT 1); }
step s2_commit { COMMIT; }

permutation s1_begin s1_read s1_write s2_begin s2_read s2_update s1_commit s2_commit
