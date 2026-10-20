# grain: writers, summarization and readers at the same time
#
# Summarization moves the watermark W, waits for every transaction that held a
# write lock on the table before W moved, then summarizes under a snapshot
# taken after the wait.  A writer that locked the table later reads the new
# W and marks the bucket it changed.  Either way readers must see exactly
# what a plain GROUP BY sees under their snapshot.

setup
{
	SET timezone = 'UTC';
	CREATE TABLE grain_t (t timestamptz, v int4);
	CREATE INDEX grain_t_bt ON grain_t (t);
	INSERT INTO grain_t
		SELECT date_trunc('minute', now()) - interval '3 hours' + i * interval '10 seconds', i % 100
		FROM generate_series(0, 3 * 360 - 1) i;
	CREATE INDEX grain_t_g ON grain_t USING grain (t) INCLUDE (v)
		WITH (buckets = '1 minute, 1 hour', delay = '2 hours');
	SELECT grain_summarize('grain_t_g');
	ALTER INDEX grain_t_g SET (delay = '1 minute');

	CREATE FUNCTION grain_t_differ() RETURNS bigint LANGUAGE sql STABLE AS $$
		WITH g AS (
			SELECT bucket, n, sum[1] AS s, min[1] AS mn, max[1] AS mx
			FROM grain_buckets('grain_t_g', '1 minute',
								  now() - interval '1 day', now() + interval '1 day')),
		p AS (
			SELECT date_bin('1 minute', t, '2000-01-01 00:00+00') AS bucket,
				   count(*) AS n, sum(v)::numeric AS s, min(v)::int8 AS mn, max(v)::int8 AS mx
			FROM grain_t
			WHERE t >= now() - interval '1 day' AND t < now() + interval '1 day'
			GROUP BY 1)
		SELECT count(*) FROM ((TABLE g EXCEPT ALL TABLE p)
							  UNION ALL (TABLE p EXCEPT ALL TABLE g)) x
	$$;
}

teardown
{
	DROP FUNCTION grain_t_differ();
	DROP TABLE grain_t;
}

# A writer into buckets between the old and the new W
session writer
step w_begin	{ BEGIN; }
step w_insert	{ INSERT INTO grain_t VALUES (now() - interval '30 minutes', 1000); }
step w_delete	{ DELETE FROM grain_t WHERE t < now() - interval '150 minutes'; }
step w_update	{ UPDATE grain_t SET v = v + 1 WHERE t >= now() - interval '90 minutes'
				  AND t < now() - interval '80 minutes'; }
step w_commit	{ COMMIT; }
step w_abort	{ ROLLBACK; }

session summarizer
step summarize	{ SELECT grain_summarize('grain_t_g') >= 0 AS summarized; }

# A reader whose snapshot is older than the summarization
session old
step o_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1 AS snapshot; }
step o_check	{ SELECT grain_t_differ() AS old_differ; }
step o_commit	{ COMMIT; }

session reader
step check		{ SELECT grain_t_differ() AS differ; }

# Summarization waits for a writer that held its lock when W moved
permutation w_begin w_insert summarize w_commit check
permutation w_begin w_delete w_update summarize w_commit check
# ... and an aborted one leaves nothing behind
permutation w_begin w_insert w_update summarize w_abort check
# A writer that locks the table after W moved marks its bucket
permutation summarize w_begin w_insert w_delete w_update w_commit check summarize check
# A reader older than the summarization
permutation o_begin w_begin w_insert w_commit summarize o_check o_commit check
