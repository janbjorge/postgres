-- grain's read path over three levels, top down, in one statement (one
-- snapshot): a day row where the output is daily or coarser, the day is
-- sealed and has no marker; else its hour rows where sealed and clean; else
-- its minute rows where sealed and clean; else the rows themselves (the tail
-- from W on, and every marked minute). Then roll up to the output bucket.
--
-- START: the window's start. BUCKET: the output bucket of b.
-- DAYS_OK: whether day rows may be used (not for hourly output).
SELECT {{BUCKET}} AS bucket, sum(n)::int8 AS n, sum(s)::int8 AS s, min(mn) AS mn, max(mx) AS mx
FROM (
  WITH
  -- markers in the window
  mk AS (
    SELECT bucket FROM {{T}}_dirty WHERE bucket >= {{START}} AND bucket < {{W}}
  ),
  -- days in the window; ok: read the day row
  dd AS (
    SELECT d, ({{DAYS_OK}} AND d + interval '1 day' <= {{W}}
               AND NOT EXISTS (SELECT 1 FROM mk WHERE mk.bucket >= d AND mk.bucket < d + interval '1 day')) AS ok
    FROM generate_series({{START}}, {{T_END}} - interval '1 microsecond', interval '1 day') d
  ),
  -- hours of the days not read; ok: read the hour row
  hh AS (
    SELECT h, (h + interval '1 hour' <= {{W}}
               AND NOT EXISTS (SELECT 1 FROM mk WHERE mk.bucket >= h AND mk.bucket < h + interval '1 hour')) AS ok
    FROM dd, generate_series(dd.d, dd.d + interval '23 hours', interval '1 hour') h
    WHERE NOT dd.ok AND h < {{T_END}}
  ),
  -- sealed, clean minutes of the hours not read
  mm AS (
    SELECT m
    FROM hh, generate_series(hh.h, hh.h + interval '59 minutes', interval '1 minute') m
    WHERE NOT hh.ok AND m + interval '1 minute' <= {{W}}
      AND NOT EXISTS (SELECT 1 FROM mk WHERE mk.bucket = m)
  )
  SELECT g.bucket AS b, g.n, g.s, g.mn, g.mx FROM {{T}}_day g JOIN dd ON g.bucket = dd.d WHERE dd.ok
  UNION ALL
  SELECT g.bucket, g.n, g.s, g.mn, g.mx FROM {{T}}_hour g JOIN hh ON g.bucket = hh.h WHERE hh.ok
  UNION ALL
  SELECT g.bucket, g.n, g.s, g.mn, g.mx FROM {{T}}_min g JOIN mm ON g.bucket = mm.m
  UNION ALL
  -- the tail, from W on
  SELECT date_bin('1 minute', done_at, {{O}}), count(*), sum(bytes), min(bytes), max(bytes)
  FROM {{T}}
  WHERE done_at >= {{W}} AND done_at < {{T_END}}
  GROUP BY 1
  UNION ALL
  -- marked minutes
  SELECT k.bucket, count(*), sum(r.bytes), min(r.bytes), max(r.bytes)
  FROM (SELECT DISTINCT bucket FROM mk) k
  JOIN {{T}} r ON r.done_at >= k.bucket AND r.done_at < k.bucket + interval '1 minute'
  GROUP BY 1
) x
GROUP BY 1
HAVING sum(n) > 0
ORDER BY 1;
