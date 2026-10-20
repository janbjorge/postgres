-- E: randomized check of grain's write, seal and read rules, in plain SQL.
--
-- Question: do the marker rule, the sealer and the top-down reader give the
-- same answer as a plain GROUP BY for any window, width and mix of writes:
-- late inserts, updates that move rows between buckets or in and out of the
-- partial index, deletes, NULL values, windows not aligned to buckets?
-- Decides: whether the read logic is right before it is written in C.
--
-- Not tested here: concurrency (one session), the real executor callbacks.
--
-- Usage: psql -v seed=0.42 -v rows=20000 -v ops=20000 -v reads=600 -f fuzz_read.sql

\set ON_ERROR_STOP on
SET client_min_messages = warning;
DROP SCHEMA IF EXISTS gf CASCADE;
CREATE SCHEMA gf;
SET search_path = gf;

-- The table, and the index's declaration: time key ts, INCLUDE (v),
-- buckets '1 minute, 1 hour, 1 day', WHERE status = 'paid'.
CREATE TABLE t (id serial PRIMARY KEY, ts timestamptz NOT NULL, v int4, status text NOT NULL);
CREATE INDEX ON t (ts);

-- The index's state: W, summary rows per level (0 minute, 1 hour, 2 day),
-- dirty markers at the finest level.
CREATE TABLE meta (w timestamptz NOT NULL, now timestamptz NOT NULL);
CREATE TABLE summ (level int, bucket timestamptz, n int8, nn int8, s int8, mn int4, mx int4,
                   PRIMARY KEY (level, bucket));
CREATE TABLE mark (bucket timestamptz NOT NULL);
CREATE INDEX ON mark (bucket);

CREATE FUNCTION o() RETURNS timestamptz IMMUTABLE LANGUAGE sql
  AS $$ SELECT timestamptz '2000-01-01 00:00+00' $$;
CREATE FUNCTION width(level int) RETURNS interval IMMUTABLE LANGUAGE sql
  AS $$ SELECT (ARRAY[interval '1 minute', interval '1 hour', interval '1 day'])[level + 1] $$;

-- Writer rule: a row version in the index whose time key is before W marks
-- its finest bucket, in the writer's transaction. Called for the new
-- version (aminsert) and the old one (amnotifydelete).
CREATE FUNCTION touch(p_ts timestamptz, p_status text) RETURNS void LANGUAGE sql AS $$
  INSERT INTO mark
  SELECT date_bin('1 minute', p_ts, o()) FROM meta WHERE p_status = 'paid' AND p_ts < meta.w
$$;

CREATE FUNCTION op_insert(p_ts timestamptz, p_v int4, p_status text) RETURNS void LANGUAGE sql AS $$
  INSERT INTO t (ts, v, status) VALUES (p_ts, p_v, p_status);
  SELECT touch(p_ts, p_status);
$$;

CREATE FUNCTION op_update(p_id int, p_ts timestamptz, p_v int4, p_status text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE r t;
BEGIN
  SELECT * INTO r FROM t WHERE id = p_id;
  IF NOT FOUND THEN RETURN; END IF;
  UPDATE t SET ts = p_ts, v = p_v, status = p_status WHERE id = p_id;
  PERFORM touch(r.ts, r.status);
  PERFORM touch(p_ts, p_status);
END $$;

CREATE FUNCTION op_delete(p_id int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r t;
BEGIN
  DELETE FROM t WHERE id = p_id RETURNING * INTO r;
  IF FOUND THEN PERFORM touch(r.ts, r.status); END IF;
END $$;

-- Aggregates of the index's rows in [lo, hi), one row per finest bucket.
CREATE FUNCTION scan_minutes(lo timestamptz, hi timestamptz)
RETURNS TABLE (bucket timestamptz, n int8, nn int8, s int8, mn int4, mx int4) LANGUAGE sql AS $$
  SELECT date_bin('1 minute', ts, o()), count(*), count(v), sum(v), min(v), max(v)
  FROM t WHERE status = 'paid' AND ts >= lo AND ts < hi GROUP BY 1
$$;

-- The sealer: move W forward; recompute the finest buckets it passed and
-- every marked one (empty ones get n = 0); fold each coarser bucket that is
-- complete and touched from the finer rows; delete the markers.
CREATE FUNCTION seal(new_w timestamptz) RETURNS void LANGUAGE plpgsql AS $$
DECLARE old_w timestamptz;
BEGIN
  SELECT w INTO old_w FROM meta;
  IF new_w <= old_w THEN RETURN; END IF;
  UPDATE meta SET w = new_w;

  CREATE TEMP TABLE redo ON COMMIT DROP AS
    SELECT generate_series(old_w, new_w - interval '1 minute', interval '1 minute') AS b
    UNION SELECT bucket FROM mark;

  DELETE FROM summ WHERE level = 0 AND bucket IN (SELECT b FROM redo);
  INSERT INTO summ
    SELECT 0, r.b, coalesce(a.n, 0), coalesce(a.nn, 0), a.s, a.mn, a.mx
    FROM redo r
    LEFT JOIN LATERAL scan_minutes(r.b, r.b + interval '1 minute') a ON true;

  FOR lvl IN 1..2 LOOP
    CREATE TEMP TABLE up ON COMMIT DROP AS
      SELECT DISTINCT date_bin(width(lvl), b, o()) AS b FROM redo;
    DELETE FROM summ WHERE level = lvl AND bucket IN (SELECT b FROM up);
    INSERT INTO summ
      SELECT lvl, u.b, sum(f.n), sum(f.nn), sum(f.s), min(f.mn), max(f.mx)
      FROM up u JOIN summ f ON f.level = lvl - 1
        AND f.bucket >= u.b AND f.bucket < u.b + width(lvl)
      WHERE u.b + width(lvl) <= new_w
      GROUP BY u.b
      HAVING count(*) = (ARRAY[60, 24])[lvl];
    DROP TABLE redo;
    ALTER TABLE up RENAME TO redo;
  END LOOP;
  DROP TABLE redo;

  DELETE FROM mark;
END $$;

-- The reader. Top down: a bucket's summary row is used when the bucket lies
-- inside the window, is no wider than the output width, has a row and no
-- marker inside it; otherwise its finer buckets are tried; a finest bucket
-- without a usable row is scanned, clipped to the window. wd NULL means one
-- total, reported at f.
CREATE FUNCTION g_read(f timestamptz, tt timestamptz, wd interval)
RETURNS TABLE (bucket timestamptz, n int8, nn int8, s int8, mn int4, mx int4)
LANGUAGE plpgsql AS $$
DECLARE
  rows_used summ[] := '{}';
  scans tstzmultirange := '{}';
  vb timestamptz;
  vl int;
  r summ;
BEGIN
  -- a stack of (level, bucket), coarsest first
  CREATE TEMP TABLE IF NOT EXISTS todo (level int, b timestamptz);
  TRUNCATE todo;
  INSERT INTO todo SELECT 2, d FROM generate_series(date_bin('1 day', f, o()), tt - interval '1 microsecond', interval '1 day') d;
  LOOP
    SELECT todo.level, todo.b INTO vl, vb FROM todo ORDER BY todo.level DESC, todo.b LIMIT 1;
    EXIT WHEN NOT FOUND;
    DELETE FROM todo WHERE todo.level = vl AND todo.b = vb;
    CONTINUE WHEN vb + width(vl) <= f OR vb >= tt;
    r := NULL;
    IF vb >= f AND vb + width(vl) <= tt AND (wd IS NULL OR width(vl) <= wd)
       AND NOT EXISTS (SELECT 1 FROM mark WHERE mark.bucket >= vb AND mark.bucket < vb + width(vl)) THEN
      SELECT * INTO r FROM summ WHERE summ.level = vl AND summ.bucket = vb;
    END IF;
    IF r.level IS NOT NULL THEN
      rows_used := rows_used || r;
    ELSIF vl = 0 THEN
      scans := scans + tstzmultirange(tstzrange(greatest(vb, f), least(vb + interval '1 minute', tt)));
    ELSE
      INSERT INTO todo SELECT vl - 1, x FROM generate_series(vb, vb + width(vl) - width(vl - 1), width(vl - 1)) x;
    END IF;
  END LOOP;

  PERFORM set_config('gf.used', cardinality(rows_used)::text, true);
  RETURN QUERY
  SELECT p.bk, sum(p.n)::int8, sum(p.nn)::int8, sum(p.s)::int8, min(p.mn), max(p.mx)
  FROM (SELECT CASE WHEN wd IS NULL THEN f ELSE date_bin(wd, u.bucket, o()) END AS bk, u.n, u.nn, u.s, u.mn, u.mx
        FROM unnest(rows_used) u
        UNION ALL
        SELECT CASE WHEN wd IS NULL THEN f ELSE date_bin(wd, x.ts, o()) END, 1, (x.v IS NOT NULL)::int, x.v, x.v, x.v
        FROM t x WHERE x.status = 'paid' AND x.ts <@ scans) p
  GROUP BY p.bk HAVING sum(p.n) > 0;
END $$;

-- The truth.
CREATE FUNCTION truth(f timestamptz, tt timestamptz, wd interval)
RETURNS TABLE (bucket timestamptz, n int8, nn int8, s int8, mn int4, mx int4) LANGUAGE sql AS $$
  SELECT CASE WHEN wd IS NULL THEN f ELSE date_bin(wd, ts, o()) END, count(*), count(v), sum(v), min(v), max(v)
  FROM t WHERE status = 'paid' AND ts >= f AND ts < tt GROUP BY 1
$$;

CREATE TABLE log (k int, f timestamptz, tt timestamptz, wd interval, buckets int, differ int,
                  marks int, w timestamptz, used int);

CREATE FUNCTION check_read(k int, f timestamptz, tt timestamptz, wd interval) RETURNS int LANGUAGE plpgsql AS $$
DECLARE d int; nb int;
BEGIN
  SELECT count(*) INTO d FROM (
    (SELECT * FROM g_read(f, tt, wd) EXCEPT ALL SELECT * FROM truth(f, tt, wd))
    UNION ALL
    (SELECT * FROM truth(f, tt, wd) EXCEPT ALL SELECT * FROM g_read(f, tt, wd))) x;
  SELECT count(*) INTO nb FROM truth(f, tt, wd);
  INSERT INTO log SELECT k, f, tt, wd, nb, d, (SELECT count(*) FROM mark), w,
                         current_setting('gf.used')::int FROM meta;
  IF d <> 0 THEN
    RAISE WARNING 'MISMATCH read % window [%, %) width %: % rows differ', k, f, tt, wd, d;
  END IF;
  RETURN d;
END $$;

-- The run. Rows start 3 days before "now" and arrive as time passes; the
-- first seal starts W on a day boundary before the oldest row.
SELECT setseed(:seed);
SET gf.ops = :ops;
SET gf.reads = :reads;
INSERT INTO meta VALUES (timestamptz '2026-10-14 00:00+00', timestamptz '2026-10-17 00:00+00');
INSERT INTO t (ts, v, status)
  SELECT timestamptz '2026-10-14 00:00+00' + random() * interval '3 days',
         CASE WHEN random() < 0.05 THEN NULL ELSE (random() * 1000)::int4 END,
         CASE WHEN random() < 0.8 THEN 'paid' ELSE 'pending' END
  FROM generate_series(1, :rows);
SELECT seal(date_bin('1 minute', now - interval '1 minute', o())) FROM meta;

DO $$
DECLARE
  nops int := current_setting('gf.ops')::int;
  nreads int := current_setting('gf.reads')::int;
  k int := 0;
  x float8;
  maxid int;
  f timestamptz;
  tt timestamptz;
  wds interval[] := ARRAY[interval '1 minute', interval '1 hour', interval '1 day', NULL];
  wd interval;
  nw timestamptz;
BEGIN
  FOR i IN 1..nops LOOP
    UPDATE meta SET now = now + random() * interval '4 seconds' RETURNING now INTO nw;
    SELECT max(id) INTO maxid FROM t;
    x := random();
    IF x < 0.55 THEN        -- a new row, stamped about now
      PERFORM op_insert(nw - random() * interval '20 seconds',
                        (random() * 1000)::int4, CASE WHEN random() < 0.8 THEN 'paid' ELSE 'pending' END);
    ELSIF x < 0.65 THEN     -- a late row, anywhere in the past; one in 50 bigger than any other
      PERFORM op_insert(nw - random() * interval '3 days',
                        CASE WHEN random() < 0.02 THEN 7777777 WHEN random() < 0.05 THEN NULL ELSE (random() * 1000)::int4 END,
                        'paid');
    ELSIF x < 0.75 THEN     -- refund or un-refund: in or out of the partial index
      PERFORM op_update(id, ts, v, CASE WHEN status = 'paid' THEN 'refunded' ELSE 'paid' END)
      FROM t WHERE id = 1 + (random() * (maxid - 1))::int;
    ELSIF x < 0.82 THEN     -- change the value
      PERFORM op_update(id, ts, CASE WHEN random() < 0.1 THEN NULL ELSE (random() * 1000)::int4 END, status)
      FROM t WHERE id = 1 + (random() * (maxid - 1))::int;
    ELSIF x < 0.88 THEN     -- move the row in time, across buckets
      PERFORM op_update(id, ts + (random() - 0.5) * interval '6 hours', v, status)
      FROM t WHERE id = 1 + (random() * (maxid - 1))::int;
    ELSIF x < 0.96 THEN     -- delete
      PERFORM op_delete(1 + (random() * (maxid - 1))::int);
    ELSE                    -- seal, with lag 1 minute
      PERFORM seal(date_bin('1 minute', nw - interval '1 minute', o()));
    END IF;

    -- reads spread over the run
    IF random() < nreads::float8 / nops THEN
      k := k + 1;
      wd := wds[1 + floor(random() * 4)::int];
      IF random() < 0.3 THEN   -- aligned to whole days or hours
        f := date_bin(CASE WHEN random() < 0.5 THEN interval '1 day' ELSE interval '1 hour' END,
                      nw - random() * interval '3 days', o());
      ELSE
        f := nw - random() * interval '3 days 6 hours';
      END IF;
      tt := least(f + random() * interval '3 days', nw + interval '1 minute');
      IF random() < 0.3 THEN tt := nw + interval '1 minute'; END IF;
      CONTINUE WHEN tt <= f;
      IF wd = interval '1 minute' THEN tt := least(tt, f + random() * interval '6 hours'); END IF;
      PERFORM check_read(k, f, tt, wd);
    END IF;
  END LOOP;
END $$;

SELECT count(*) AS reads, sum(differ) AS rows_differ, count(*) FILTER (WHERE differ > 0) AS bad_reads,
       sum(buckets) AS buckets_checked, count(*) FILTER (WHERE marks > 0) AS reads_with_markers,
       max(marks) AS max_markers, sum(used) AS summary_rows_used,
       count(*) FILTER (WHERE used = 0) AS reads_all_scanned
FROM log;
SELECT coalesce(wd::text, 'total') AS width, count(*) AS reads, sum(differ) AS differ FROM log GROUP BY 1 ORDER BY 1;
SELECT count(*) AS rows_now, (SELECT count(*) FROM summ) AS summary_rows FROM t;
