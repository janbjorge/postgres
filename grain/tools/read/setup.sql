-- The table: N rows spread evenly over the 90 days before T_END.
DROP TABLE IF EXISTS {{T}}, {{T}}_summary, {{T}}_dirty;
CREATE TABLE {{T}} (done_at timestamptz NOT NULL, bytes int4 NOT NULL, status int2 NOT NULL);
SELECT setseed(0.42);
INSERT INTO {{T}}
  SELECT {{T_END}} - interval '90 days' + (i - 1) * (interval '90 days' / {{N}}),
         (random() * 1000000)::int4,
         (i % 5)::int2
  FROM generate_series(1, {{N}}) i;
VACUUM (ANALYZE) {{T}};
