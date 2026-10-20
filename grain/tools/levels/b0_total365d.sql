-- Today: one total over a year.
SELECT {{START_YEAR}} AS bucket,
       count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx
FROM {{T}}
WHERE done_at >= {{START_YEAR}} AND done_at < {{T_END}};
