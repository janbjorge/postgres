\set QUIET on
CREATE EXTENSION IF NOT EXISTS pageinspect;
CREATE EXTENSION IF NOT EXISTS pg_visibility;
DROP TABLE IF EXISTS t;
CREATE TABLE t (ts timestamptz, n int) WITH (fillfactor = 90, autovacuum_enabled = off);
INSERT INTO t SELECT '2026-01-01'::timestamptz + i * interval '1 s', i FROM generate_series(1, 2000) i;
VACUUM (FREEZE false) t;
CREATE TEMP TABLE obs (step text, vm_page0_bits text, vm_lsn pg_lsn, block0_sum bigint);
CREATE FUNCTION snap(s text) RETURNS void LANGUAGE sql AS $$
  INSERT INTO obs
  SELECT s,
         (SELECT all_visible::text || '/' || all_frozen::text FROM pg_visibility_map('t', 0)),
         (SELECT lsn FROM page_header(get_raw_page('t', 'vm', 0))),
         (SELECT sum(n) FROM t WHERE ctid >= '(0,0)' AND ctid < '(1,0)');
$$;
SELECT snap('1 after VACUUM: "summary" computed here');
UPDATE t SET n = n + 1000000 WHERE ctid = '(0,1)';
SELECT snap('2 after HOT UPDATE on block 0');
SELECT count(*) FROM t \g /dev/null
SELECT snap('3 after read-only SELECT (no VACUUM)');
SELECT snap('4 again, no change expected');
\set QUIET off
SELECT * FROM obs;
SELECT n_tup_hot_upd FROM pg_stat_user_tables WHERE relname = 't';
