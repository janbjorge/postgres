--
-- grain_agg: GROUP BY date_bin(...) computed from a grain index
--
SET timezone = 'UTC';

CREATE TABLE grain_agg_orders (
    created_at timestamptz,
    amount_cents int8,
    qty int4,
    small int2,
    status text
);

-- Three days of paid and refunded orders, ten per minute, in 2020
INSERT INTO grain_agg_orders
SELECT '2020-01-01 00:00+00'::timestamptz + i * interval '6 seconds',
       (i * 37) % 1000, i % 7, (i % 5)::int2,
       CASE WHEN i % 10 = 0 THEN 'refunded' ELSE 'paid' END
FROM generate_series(0, 3 * 24 * 600 - 1) i;
CREATE INDEX grain_agg_orders_created_at ON grain_agg_orders (created_at);
CREATE INDEX grain_agg_orders_paid ON grain_agg_orders USING grain (created_at)
    INCLUDE (amount_cents, qty, small)
    WITH (buckets = '1 minute, 1 hour, 1 day')
    WHERE status = 'paid';
SELECT grain_summarize('grain_agg_orders_paid') > 0 AS summarized;
ANALYZE grain_agg_orders;

-- Days of a three-day window: the index reads three summary rows
EXPLAIN (COSTS OFF)
SELECT date_bin('1 day', created_at, '2000-01-01'), count(*), sum(amount_cents)
FROM grain_agg_orders
WHERE status = 'paid'
  AND created_at >= '2020-01-01' AND created_at < '2020-01-04'
GROUP BY 1 ORDER BY 1;
SELECT date_bin('1 day', created_at, '2000-01-01'), count(*), sum(amount_cents)
FROM grain_agg_orders
WHERE status = 'paid'
  AND created_at >= '2020-01-01' AND created_at < '2020-01-04'
GROUP BY 1 ORDER BY 1;

-- Whether a query is planned with the index.  Hashed and sorted grouping
-- are off, so that this shows which queries the index can answer rather
-- than which it answers more cheaply.
CREATE FUNCTION grain_agg_used(q text) RETURNS bool
LANGUAGE plpgsql AS $$
DECLARE
    line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        IF line LIKE '%Grain Aggregate%' THEN
            RETURN true;
        END IF;
    END LOOP;
    RETURN false;
END
$$;

-- The query run with and without the index: rows that differ
CREATE FUNCTION grain_agg_differ(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    d bigint;
BEGIN
    PERFORM set_config('enable_grainagg', 'on', true);
    EXECUTE 'CREATE TEMP TABLE grain_agg_g AS SELECT s::text AS r FROM (' || q || ') s';
    PERFORM set_config('enable_grainagg', 'off', true);
    EXECUTE 'CREATE TEMP TABLE grain_agg_p AS SELECT s::text AS r FROM (' || q || ') s';
    PERFORM set_config('enable_grainagg', 'on', true);
    SELECT count(*) INTO d
    FROM ((TABLE grain_agg_g EXCEPT ALL TABLE grain_agg_p)
          UNION ALL (TABLE grain_agg_p EXCEPT ALL TABLE grain_agg_g)) x;
    DROP TABLE grain_agg_g, grain_agg_p;
    RETURN d;
END
$$;

SET enable_hashagg = off;
SET enable_sort = off;

CREATE TABLE grain_agg_queries (what text, q text);
INSERT INTO grain_agg_queries VALUES
('every aggregate, hours',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*),
          count(amount_cents), sum(amount_cents), min(amount_cents),
          max(amount_cents), sum(qty), min(qty), max(small), sum(small)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01 10:17:30+00'
     AND created_at < '2020-01-02 03:59:59+00'
   GROUP BY 1$$),
('days, bounds > and <=',
 $$SELECT date_bin('1 day', created_at, '2000-01-01'), count(*), sum(qty)
   FROM grain_agg_orders
   WHERE created_at > '2019-12-31 23:00+00' AND status = 'paid'
     AND created_at <= '2020-01-03 00:00+00'
   GROUP BY 1$$),
('minutes, commuted bounds, HAVING',
 $$SELECT date_bin('1 minute', created_at, '2000-01-01'), count(*) * 2,
          sum(qty) + 1
   FROM grain_agg_orders
   WHERE '2020-01-02 00:00+00' <= created_at AND status = 'paid'
     AND '2020-01-02 01:00+00' > created_at
   GROUP BY 1
   HAVING max(amount_cents) > 900$$),
('another origin on the buckets',
 $$SELECT date_bin('1 day', created_at, '2020-01-01 00:00+00'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01 12:00+00'
     AND created_at < '2020-01-03 12:00+00'
   GROUP BY 1$$),
('bounds from now()',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= now() - interval '10 years'
     AND created_at < now()
   GROUP BY 1$$),
('an empty window',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-02'
     AND created_at < '2020-01-01'
   GROUP BY 1$$),
('rescanned under a lateral join',
 $$SELECT d, x.*
   FROM (VALUES ('2020-01-01 00:00+00'::timestamptz),
                ('2020-01-02 06:00+00')) v(d),
        LATERAL (SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
                 FROM grain_agg_orders
                 WHERE status = 'paid' AND created_at >= v.d
                   AND created_at < v.d + interval '5 hours'
                 GROUP BY 1) x$$);

SELECT what, grain_agg_used(q) AS used, grain_agg_differ(q) AS differ
FROM grain_agg_queries ORDER BY what;

-- Marked buckets, in a transaction, are read from their rows
BEGIN;
INSERT INTO grain_agg_orders
VALUES ('2020-01-02 12:00:01+00', 999999, 3, 2, 'paid');
DELETE FROM grain_agg_orders WHERE created_at = '2020-01-01 06:00:06+00';
UPDATE grain_agg_orders SET status = 'refunded'
WHERE created_at = '2020-01-02 09:09:00+00';
SELECT what, grain_agg_differ(q) AS differ
FROM grain_agg_queries ORDER BY what;
ROLLBACK;

-- Rescanned with unchanged bounds, the buckets read once are returned again
SET enable_material = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
EXPLAIN (COSTS OFF)
SELECT v.x, g.*
FROM (VALUES (1), (2)) v(x) LEFT JOIN
     (SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
      FROM grain_agg_orders
      WHERE status = 'paid' AND created_at >= '2020-01-01'
        AND created_at < '2020-01-01 03:00' GROUP BY 1) g ON true;
SELECT grain_agg_differ($$
SELECT v.x, g.*
FROM (VALUES (1), (2)) v(x) LEFT JOIN
     (SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
      FROM grain_agg_orders
      WHERE status = 'paid' AND created_at >= '2020-01-01'
        AND created_at < '2020-01-01 03:00' GROUP BY 1) g ON true$$) AS differ;
RESET enable_material;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- What the index can't answer
SELECT what, grain_agg_used(q) AS used FROM (VALUES
('another qual',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND qty > 2 AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('not the predicate',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE created_at >= '2020-01-01' AND created_at < '2020-01-02'
   GROUP BY 1$$),
('a looser predicate',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status IN ('paid', 'refunded') AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('no upper bound',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01' GROUP BY 1$$),
('a width not a level',
 $$SELECT date_bin('2 hours', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('an origin off the buckets',
 $$SELECT date_bin('1 minute', created_at, '2000-01-01 00:00:30'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('date_bin() on timestamp',
 $$SELECT date_bin('1 hour', created_at::timestamp, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('a second group key',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), qty, count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1, 2$$),
('DISTINCT',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(DISTINCT qty)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('FILTER',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'),
          count(*) FILTER (WHERE qty > 2)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('avg()',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), avg(qty)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('an expression aggregated',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), sum(qty * 2)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('the time key aggregated',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), max(created_at)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < '2020-01-02' GROUP BY 1$$),
('a join',
 $$SELECT date_bin('1 hour', o.created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders o JOIN grain_agg_queries q ON true
   WHERE o.status = 'paid' AND o.created_at >= '2020-01-01'
     AND o.created_at < '2020-01-02' GROUP BY 1$$),
('a volatile bound',
 $$SELECT date_bin('1 hour', created_at, '2000-01-01'), count(*)
   FROM grain_agg_orders
   WHERE status = 'paid' AND created_at >= '2020-01-01'
     AND created_at < clock_timestamp() GROUP BY 1$$)
) v(what, q) ORDER BY what;

SET enable_grainagg = off;
SELECT grain_agg_used(q) AS used FROM grain_agg_queries ORDER BY what LIMIT 1;
RESET enable_grainagg;

-- A generic plan reads the bounds at execution
SET plan_cache_mode = force_generic_plan;
PREPARE grain_agg_days(timestamptz, timestamptz) AS
SELECT date_bin('1 day', created_at, '2000-01-01'), count(*), sum(qty)
FROM grain_agg_orders
WHERE status = 'paid' AND created_at >= $1 AND created_at < $2
GROUP BY 1;
EXPLAIN (COSTS OFF) EXECUTE grain_agg_days('2020-01-01', '2020-01-04');
EXECUTE grain_agg_days('2020-01-01', '2020-01-04');
EXECUTE grain_agg_days('2020-01-02 12:00', '2020-01-03 12:00');
EXECUTE grain_agg_days(NULL, '2020-01-04');
-- grain has no bucket for infinite time keys
EXECUTE grain_agg_days('-infinity', '2020-01-04');
DEALLOCATE grain_agg_days;
RESET plan_cache_mode;

RESET enable_hashagg;
RESET enable_sort;
DROP TABLE grain_agg_orders, grain_agg_queries;
DROP FUNCTION grain_agg_used(text), grain_agg_differ(text);
