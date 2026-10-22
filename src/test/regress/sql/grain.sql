--
-- grain: exact aggregates per time bucket
--
SET timezone = 'UTC';

CREATE TABLE grain_orders (
    id serial,
    created_at timestamptz,
    amount_cents int8,
    qty int4,
    status text
);
CREATE INDEX grain_orders_created_at ON grain_orders (created_at);

-- Three days of paid and refunded orders, ten per minute, in 2020: before
-- any watermark.  A few orders far in the future: after every watermark.
INSERT INTO grain_orders (created_at, amount_cents, qty, status)
SELECT '2020-01-01 00:00+00'::timestamptz + i * interval '6 seconds',
       (i * 37) % 1000, i % 7,
       CASE WHEN i % 10 = 0 THEN 'refunded' ELSE 'paid' END
FROM generate_series(0, 3 * 24 * 600 - 1) i;
INSERT INTO grain_orders (created_at, amount_cents, qty, status) VALUES
    ('2100-01-01 00:00:30+00', 5, 1, 'paid'),
    (NULL, 7, 1, 'paid'),
    ('infinity', 9, 1, 'paid');

-- What is refused
CREATE INDEX ON grain_orders USING grain (created_at);  -- no buckets
CREATE INDEX ON grain_orders USING grain (created_at) WITH (buckets = '1 month');
CREATE INDEX ON grain_orders USING grain (created_at) WITH (buckets = '1 hour, 90 minutes');
CREATE INDEX ON grain_orders USING grain (created_at) WITH (buckets = '1 hour, 1 minute');
CREATE INDEX ON grain_orders USING grain (created_at) WITH (buckets = '0 seconds');
CREATE INDEX ON grain_orders USING grain (created_at) WITH (buckets = '1 hour', delay = '-1 hour');
CREATE INDEX ON grain_orders USING grain (created_at) WITH (buckets = '1 hour', origin = 'infinity');
CREATE INDEX ON grain_orders USING grain (created_at, id) WITH (buckets = '1 hour');
CREATE INDEX ON grain_orders USING grain ((to_timestamp(id))) WITH (buckets = '1 hour');
CREATE INDEX ON grain_orders USING grain (created_at) INCLUDE (status) WITH (buckets = '1 hour');
CREATE UNIQUE INDEX ON grain_orders USING grain (created_at) WITH (buckets = '1 hour');
CREATE INDEX ON grain_orders USING grain (id) WITH (buckets = '1 hour');
CREATE TEMP TABLE grain_temp (t timestamptz);
CREATE INDEX ON grain_temp USING grain (t) WITH (buckets = '1 hour');
CREATE UNLOGGED TABLE grain_unlogged (t timestamptz);
CREATE INDEX ON grain_unlogged USING grain (t) WITH (buckets = '1 hour');
DROP TABLE grain_temp, grain_unlogged;

-- Minute, hour and day levels, partial on paid orders
CREATE INDEX grain_orders_paid ON grain_orders USING grain (created_at)
    INCLUDE (amount_cents, qty)
    WITH (buckets = '1 minute, 1 hour, 1 day')
    WHERE status = 'paid';

-- Its summary and marker tables belong to it
SELECT c.relname ~ '^pg_grain_\d+_(summary|marker|summary_index)$' AS named,
       n.nspname, c.relkind, d.deptype
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
LEFT JOIN pg_depend d ON d.classid = 'pg_class'::regclass AND d.objid = c.oid
     AND d.refobjid = 'grain_orders_paid'::regclass
WHERE c.relname LIKE 'pg_grain_%'
ORDER BY c.relkind, d.deptype, c.relname ~ 'marker';

-- The planner never uses it
EXPLAIN (COSTS OFF)
SELECT count(*) FROM grain_orders
WHERE created_at >= '2020-01-02' AND status = 'paid';

-- grain against a plain GROUP BY, under one snapshot: rows that differ
CREATE FUNCTION grain_differ(width interval, lo timestamptz, hi timestamptz)
RETURNS bigint LANGUAGE sql STABLE AS $$
    WITH g AS (
        SELECT bucket, n, nn, sum, min, max
        FROM grain_buckets('grain_orders_paid', width, lo, hi)),
    p AS (
        SELECT date_bin(width, created_at, '2000-01-01 00:00+00') AS bucket,
               count(*) AS n,
               ARRAY[count(amount_cents), count(qty)] AS nn,
               ARRAY[sum(amount_cents)::numeric, sum(qty)::numeric] AS sum,
               ARRAY[min(amount_cents), min(qty)::int8] AS min,
               ARRAY[max(amount_cents), max(qty)::int8] AS max
        FROM grain_orders
        WHERE status = 'paid' AND created_at >= lo AND created_at < hi
        GROUP BY 1)
    SELECT count(*) FROM ((TABLE g EXCEPT ALL TABLE p)
                          UNION ALL (TABLE p EXCEPT ALL TABLE g)) x
$$;

CREATE FUNCTION grain_differ_all() RETURNS TABLE (width interval, lo timestamptz, hi timestamptz, differ bigint)
LANGUAGE sql STABLE AS $$
    SELECT w, lo, hi, grain_differ(w, lo, hi)
    FROM (VALUES (interval '1 minute'), (interval '1 hour'), (interval '1 day')) AS ws(w),
         (VALUES ('2019-12-31 23:00+00'::timestamptz, '2020-01-05 00:00+00'::timestamptz),
                 ('2020-01-01 10:17:30+00', '2020-01-02 03:59:59+00'),
                 ('2020-01-02 00:00+00', '2020-01-03 00:00+00'),
                 ('2099-12-31 00:00+00', '2100-01-02 00:00+00')) AS win(lo, hi)
    WHERE grain_differ(w, lo, hi) <> 0
$$;

-- Before any summarization every bucket is scanned
SELECT * FROM grain_differ_all();
SELECT bucket, n, nn, sum, min, max
FROM grain_buckets('grain_orders_paid', '1 day', '2020-01-01', '2020-01-05');

-- The first summarization
SELECT grain_summarize('grain_orders_paid') > 0 AS summarized;
SELECT relname AS summary FROM pg_class WHERE relname ~ '^pg_grain_\d+_summary$' \gset
SELECT level, count(*), sum(n) FROM pg_toast.:summary
WHERE bucket < '2021-01-01' GROUP BY 1 ORDER BY 1;
SELECT * FROM grain_differ_all();

-- Reads after a summarization use the summaries and give the same answer
SELECT bucket, n, nn, sum, min, max
FROM grain_buckets('grain_orders_paid', '1 day', '2020-01-01', '2020-01-05');

-- A late insert, a delete and an update into summarized buckets, in a
-- transaction: markers make the reader scan those buckets
BEGIN;
INSERT INTO grain_orders (created_at, amount_cents, qty, status)
VALUES ('2020-01-02 12:00:01+00', 999999, 3, 'paid');
DELETE FROM grain_orders WHERE created_at = '2020-01-01 06:00:06+00';
UPDATE grain_orders SET amount_cents = amount_cents + 1
WHERE created_at = '2020-01-03 18:30:12+00';
SELECT relname AS marker FROM pg_class WHERE relname ~ '^pg_grain_\d+_marker$' \gset
SELECT bucket FROM pg_toast.:marker ORDER BY 1;
SELECT * FROM grain_differ_all();
ROLLBACK;
SELECT count(*) FROM pg_toast.:marker;
SELECT * FROM grain_differ_all();

-- The same, committed, then summarized
INSERT INTO grain_orders (created_at, amount_cents, qty, status)
VALUES ('2020-01-02 12:00:01+00', 999999, 3, 'paid');
DELETE FROM grain_orders WHERE created_at = '2020-01-01 06:00:06+00';
UPDATE grain_orders SET amount_cents = amount_cents + 1
WHERE created_at = '2020-01-03 18:30:12+00';
-- an update moving a row between buckets
UPDATE grain_orders SET created_at = created_at + interval '1 day'
WHERE created_at = '2020-01-01 08:00:00+00';
-- a refund: the row leaves the partial index
UPDATE grain_orders SET status = 'refunded'
WHERE created_at = '2020-01-02 09:09:00+00';
-- an update of no indexed column touches no bucket
UPDATE grain_orders SET id = id WHERE created_at = '2020-01-02 10:10:00+00';
-- NULL and infinite time keys are in no bucket
UPDATE grain_orders SET amount_cents = 8 WHERE created_at IS NULL OR created_at = 'infinity';
SELECT bucket FROM pg_toast.:marker ORDER BY 1;
SELECT * FROM grain_differ_all();
SELECT grain_summarize('grain_orders_paid') > 0 AS summarized;
SELECT count(*) FROM pg_toast.:marker;
SELECT * FROM grain_differ_all();
SELECT bucket, n, nn, sum, min, max
FROM grain_buckets('grain_orders_paid', '1 day', '2020-01-01', '2020-01-05');

-- A backfill before the first summarized day
INSERT INTO grain_orders (created_at, amount_cents, qty, status)
SELECT '2019-12-31 22:00+00'::timestamptz + i * interval '1 minute', 1, 1, 'paid'
FROM generate_series(0, 89) i;
SELECT count(*) FROM pg_toast.:marker;
SELECT grain_summarize('grain_orders_paid') > 0 AS summarized;
SELECT * FROM grain_differ_all();

-- Summarizing again with nothing new changes nothing
SELECT level, count(*), sum(n) FROM pg_toast.:summary
WHERE bucket < '2021-01-01' GROUP BY 1 ORDER BY 1;

-- The width must be one of the levels; the window must be finite
SELECT * FROM grain_buckets('grain_orders_paid', '2 hours', '2020-01-01', '2020-01-02');
SELECT * FROM grain_buckets('grain_orders_paid', '1 hour', '-infinity', '2020-01-02');
SELECT * FROM grain_buckets('grain_orders_created_at', '1 hour', '2020-01-01', '2020-01-02');

-- A partitioned grain index has no storage of its own; its partitions'
-- indexes are the grain indexes
CREATE TABLE grain_parted (t timestamptz) PARTITION BY RANGE (t);
CREATE TABLE grain_parted_2020 PARTITION OF grain_parted
    FOR VALUES FROM ('2020-01-01') TO ('2021-01-01');
CREATE INDEX grain_parted_t ON grain_parted USING grain (t) WITH (buckets = '1 hour');
SELECT grain_summarize('grain_parted_t');
SELECT * FROM grain_buckets('grain_parted_t', '1 hour', '2020-01-01', '2020-01-02');
SELECT grain_summarize('grain_parted_2020_t_idx') >= 0 AS summarized;
DROP TABLE grain_parted;

-- Summarization needs READ COMMITTED
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT grain_summarize('grain_orders_paid');
ROLLBACK;

-- delay can change at any time
ALTER INDEX grain_orders_paid SET (delay = '1 hour');
SELECT grain_summarize('grain_orders_paid') >= 0 AS summarized;
SELECT * FROM grain_differ_all();

-- The levels are fixed when the index is built, as BRIN's pages_per_range
-- is: changing them takes effect at the next REINDEX, and until then the
-- summaries are read with the levels they were built with
ALTER INDEX grain_orders_paid SET (buckets = '1 day', origin = '2000-01-01 00:30+00');
SELECT * FROM grain_differ_all();
ALTER INDEX grain_orders_paid SET (buckets = '1 minute, 1 hour, 1 day');
ALTER INDEX grain_orders_paid RESET (origin);
CREATE INDEX grain_orders_levels ON grain_orders USING grain (created_at)
    WITH (buckets = '1 hour');
SELECT grain_summarize('grain_orders_levels') > 0 AS summarized;
ALTER INDEX grain_orders_levels SET (buckets = '1 hour, 1 day');
SELECT count(*) FROM grain_buckets('grain_orders_levels', '1 day', '2020-01-01', '2020-01-05');
REINDEX INDEX grain_orders_levels;
SELECT count(*) > 0 AS rows FROM grain_buckets('grain_orders_levels', '1 day', '2020-01-01', '2020-01-05');
DROP INDEX grain_orders_levels;

-- Rebuilds empty the summaries; the answers stay the same
REINDEX INDEX grain_orders_paid;
SELECT count(*) FROM pg_toast.:summary;
SELECT * FROM grain_differ_all();
SELECT grain_summarize('grain_orders_paid') > 0 AS summarized;
VACUUM FULL grain_orders;
SELECT count(*) FROM pg_toast.:summary;
SELECT grain_summarize('grain_orders_paid') > 0 AS summarized;
SELECT * FROM grain_differ_all();
BEGIN;
TRUNCATE grain_orders;
SELECT count(*) FROM pg_toast.:summary;
ROLLBACK;
SELECT count(*) > 0 AS kept FROM pg_toast.:summary;
SELECT * FROM grain_differ_all();

-- Not supported yet
REINDEX INDEX CONCURRENTLY grain_orders_paid;
ALTER INDEX grain_orders_paid SET TABLESPACE pg_default;

-- REINDEX (TABLESPACE) moves the summary and marker tables with the index
SET allow_in_place_tablespaces = true;
CREATE TABLESPACE regress_grain_tblspc LOCATION '';
REINDEX (TABLESPACE regress_grain_tblspc) INDEX grain_orders_paid;
SELECT count(*) AS moved FROM pg_class c JOIN pg_tablespace s ON s.oid = c.reltablespace
WHERE s.spcname = 'regress_grain_tblspc';
SELECT grain_summarize('grain_orders_paid') > 0 AS summarized;
SELECT * FROM grain_differ_all();
REINDEX (TABLESPACE pg_default) INDEX grain_orders_paid;
DROP TABLESPACE regress_grain_tblspc;
RESET allow_in_place_tablespaces;

-- Only the owner summarizes; readers need SELECT on the table; the table's
-- owner can't write the summary and marker tables
CREATE ROLE regress_grain_owner;
CREATE ROLE regress_grain_reader;
ALTER TABLE grain_orders OWNER TO regress_grain_owner;
SET ROLE regress_grain_owner;
SELECT grain_summarize('grain_orders_paid') >= 0 AS summarized;
RESET ROLE;
SELECT has_table_privilege('regress_grain_owner', 'pg_toast.' || :'marker', 'INSERT') AS marker_insert,
       has_table_privilege('regress_grain_owner', 'pg_toast.' || :'summary', 'DELETE') AS summary_delete;
SET ROLE regress_grain_reader;
SELECT grain_summarize('grain_orders_paid');
SELECT count(*) FROM grain_buckets('grain_orders_paid', '1 day', '2020-01-01', '2020-01-05');
RESET ROLE;
GRANT SELECT ON grain_orders TO regress_grain_reader;
SET ROLE regress_grain_reader;
SELECT count(*) FROM grain_buckets('grain_orders_paid', '1 day', '2020-01-01', '2020-01-05');
RESET ROLE;
ALTER TABLE grain_orders ENABLE ROW LEVEL SECURITY;
SET ROLE regress_grain_reader;
SELECT count(*) FROM grain_buckets('grain_orders_paid', '1 day', '2020-01-01', '2020-01-05');
RESET ROLE;
ALTER TABLE grain_orders DISABLE ROW LEVEL SECURITY;
REVOKE SELECT ON grain_orders FROM regress_grain_reader;

-- The summary and marker tables drop with the index
DROP INDEX grain_orders_paid;
SELECT count(*) FROM pg_class WHERE relname LIKE 'pg_grain_%';

DROP FUNCTION grain_differ_all();
DROP FUNCTION grain_differ(interval, timestamptz, timestamptz);
DROP TABLE grain_orders;
DROP ROLE regress_grain_owner;
DROP ROLE regress_grain_reader;
