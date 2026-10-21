-- Five late rows into five sealed hours, each with its marker, as aminsert
-- would write. Their bytes are above every other row's, so a missed marker
-- shows in max().
INSERT INTO {{T}}
  SELECT {{START}} + k * interval '100 hours' + interval '30 minutes', 7777777, 0
  FROM generate_series(1, 5) k;
INSERT INTO {{T}}_dirty
  SELECT date_bin('1 hour', {{START}} + k * interval '100 hours' + interval '30 minutes', {{O}})
  FROM generate_series(1, 5) k;
ANALYZE {{T}}, {{T}}_dirty;
