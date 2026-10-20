-- grain's state, as the sealer leaves it: a row for every sealed minute
-- (empty minutes with n = 0), hours folded from minutes, days from hours;
-- no markers.
CREATE TABLE {{T}}_min (bucket timestamptz PRIMARY KEY, n int8, s int8, mn int4, mx int4);
CREATE TABLE {{T}}_hour (LIKE {{T}}_min INCLUDING ALL);
CREATE TABLE {{T}}_day (LIKE {{T}}_min INCLUDING ALL);
CREATE TABLE {{T}}_dirty (bucket timestamptz NOT NULL);
CREATE INDEX ON {{T}}_dirty (bucket);

INSERT INTO {{T}}_min
  SELECT m, coalesce(a.n, 0), coalesce(a.s, 0), a.mn, a.mx
  FROM generate_series(date_bin('1 minute', {{T_END}} - interval '400 days', {{O}}),
                       {{W}} - interval '1 minute', interval '1 minute') m
  LEFT JOIN (SELECT date_bin('1 minute', done_at, {{O}}) AS b,
                    count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx
             FROM {{T}}
             WHERE done_at < {{W}}
             GROUP BY 1) a ON a.b = m;
INSERT INTO {{T}}_hour
  SELECT date_bin('1 hour', bucket, {{O}}), sum(n), sum(s), min(mn), max(mx)
  FROM {{T}}_min
  GROUP BY 1
  HAVING date_bin('1 hour', bucket, {{O}}) + interval '1 hour' <= {{W}};
INSERT INTO {{T}}_day
  SELECT date_bin('1 day', bucket, {{O}}), sum(n), sum(s), min(mn), max(mx)
  FROM {{T}}_hour
  GROUP BY 1
  HAVING date_bin('1 day', bucket, {{O}}) + interval '1 day' <= {{W}};

VACUUM (ANALYZE) {{T}}, {{T}}_min, {{T}}_hour, {{T}}_day, {{T}}_dirty;
