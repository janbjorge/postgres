-- Five late rows into five sealed minutes, each with its marker, as
-- aminsert would write; the hours and days rolled up from them are now
-- stale. Their bytes are above every other row's, so a missed marker shows
-- in max().
INSERT INTO {{T}}
  SELECT {{START_HOUR}} + k * interval '100 hours' + interval '30 minutes 17 seconds', 7777777, 0
  FROM generate_series(1, 5) k;
INSERT INTO {{T}}_dirty
  SELECT date_bin('1 minute', {{START_HOUR}} + k * interval '100 hours' + interval '30 minutes 17 seconds', {{O}})
  FROM generate_series(1, 5) k;
ANALYZE {{T}}, {{T}}_dirty;
