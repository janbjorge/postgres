-- Today: a plain GROUP BY, hourly, from START to T_END.
SELECT date_bin('1 hour', done_at, {{O}}) AS bucket,
       count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx
FROM {{T}}
WHERE done_at >= {{START}} AND done_at < {{T_END}}
GROUP BY 1
ORDER BY 1;
