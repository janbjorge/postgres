-- grain's read path, in one statement (one snapshot): sealed hours with no
-- marker from their summary rows; the open hours (from W on) and the marked
-- hours from the table.
SELECT bucket, n, s, mn, mx
FROM (
  -- sealed, clean hours
  SELECT s.bucket, s.n, s.s, s.mn, s.mx
  FROM {{T}}_summary s
  WHERE s.bucket >= {{START}} AND s.bucket < {{W}} AND s.n > 0
    AND NOT EXISTS (SELECT 1 FROM {{T}}_dirty d WHERE d.bucket = s.bucket)
  UNION ALL
  -- open hours
  SELECT date_bin('1 hour', done_at, {{O}}), count(*), sum(bytes), min(bytes), max(bytes)
  FROM {{T}}
  WHERE done_at >= {{W}} AND done_at < {{T_END}}
  GROUP BY 1
  UNION ALL
  -- marked hours
  SELECT d.bucket, count(*), sum(t.bytes), min(t.bytes), max(t.bytes)
  FROM (SELECT DISTINCT bucket FROM {{T}}_dirty WHERE bucket >= {{START}} AND bucket < {{W}}) d
  JOIN {{T}} t ON t.done_at >= d.bucket AND t.done_at < d.bucket + interval '1 hour'
  GROUP BY 1
) x
ORDER BY 1;
