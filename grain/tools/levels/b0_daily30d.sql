-- Today: a plain GROUP BY, daily over 30 days.
SELECT date_bin('1 day', done_at, {{O}}) AS bucket,
       count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx
FROM {{T}}
WHERE done_at >= {{START_DAY}} AND done_at < {{T_END}}
GROUP BY 1
ORDER BY 1;
