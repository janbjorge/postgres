-- grain's state, as the sealer leaves it: a summary row for every hour
-- below W (empty hours with n = 0), and no markers.
CREATE TABLE {{T}}_summary (bucket timestamptz PRIMARY KEY, n int8, s int8, mn int4, mx int4);
CREATE TABLE {{T}}_dirty (bucket timestamptz NOT NULL);
INSERT INTO {{T}}_summary
  SELECT h, count(t.done_at), coalesce(sum(t.bytes), 0), min(t.bytes), max(t.bytes)
  FROM generate_series(date_bin('1 hour', {{T_END}} - interval '90 days', {{O}}),
                       {{W}} - interval '1 hour', interval '1 hour') h
  LEFT JOIN {{T}} t ON t.done_at >= h AND t.done_at < h + interval '1 hour'
  GROUP BY h;
VACUUM (ANALYZE) {{T}}, {{T}}_summary, {{T}}_dirty;
