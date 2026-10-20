/*-------------------------------------------------------------------------
 *
 * grain_summarize.c
 *	  Summarization, grain_summarize(), and the reader, grain_buckets().
 *
 * Summarization moves the watermark W forward, waits for every transaction
 * that held a write lock on the table before W moved, then under one
 * snapshot recomputes the new finest buckets and every marked one, folds
 * them into the coarser levels, replaces their summary rows and deletes the
 * markers it used.  A writer that locks the table after W moved reads the
 * new W, so a change to a summarized bucket that the snapshot missed always
 * leaves a marker visible together with the change.
 *
 * The reader uses a bucket's summary row when its snapshot sees the row,
 * sees no marker in the bucket, and the bucket lies wholly in the window.
 * Otherwise it descends a level, and at the finest level scans the rows.
 *
 * Both aggregate table rows through SPI, so the planner can use the user's
 * own btree or BRIN index on the time key.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/grain/grain_summarize.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/grain_private.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "common/int.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

/*
 * The most buckets grain_buckets() returns for one call.  Its output array
 * is allocated whole, so this bounds that allocation.
 */
#define GRAIN_MAX_OUTPUT_BUCKETS	10000000

/* Everything summarization and the reader share for one index */
typedef struct GrainCtx
{
	Relation	heap;
	Relation	index;
	GrainCache *cache;
	Relation	summaryrel;
	Relation	summaryidx;
	IndexInfo  *summaryidxinfo; /* summarization only */
	Snapshot	snapshot;
	SPIPlanPtr	rangeplan;		/* aggregates per bucket of a range, prepared
								 * on first use */
	SPIPlanPtr	minplan;		/* min(time key), for the first summarization,
								 * prepared on first use */
	MemoryContext aggcxt;		/* where aggregates live */
} GrainCtx;

/* A summary row as summarization holds it while it works on one chunk */
typedef struct GrainRow
{
	TimestampTz bucket;			/* hash key */
	GrainAgg	agg;
	ItemPointerData stored;		/* the visible stored row, if any */
	bool		changed;
} GrainRow;

/*
 * " FROM table WHERE key >= $lo AND key < $hi", and the index predicate of a
 * partial index.  lo and hi are parameter numbers.
 */
static char *
grain_from_where(GrainCtx *ctx, const char *key, int lo, int hi)
{
	Relation	heap = ctx->heap;
	Oid			heapoid = RelationGetRelid(heap);
	List	   *pred = RelationGetIndexPredicate(ctx->index);
	char	   *nspname = get_namespace_name(RelationGetNamespace(heap));
	char	   *relname = RelationGetRelationName(heap);
	StringInfoData sql;

	initStringInfo(&sql);
	appendStringInfo(&sql, " FROM ONLY %s WHERE %s >= $%d AND %s < $%d",
					 quote_qualified_identifier(nspname, relname),
					 key, lo, key, hi);
	if (pred != NIL)
	{
		Node	   *expr = (Node *) make_ands_explicit(pred);
		List	   *dpcontext = deparse_context_for(relname, heapoid);

		appendStringInfo(&sql, " AND (%s)",
						 deparse_expression(expr, dpcontext, false, false));
	}
	return sql.data;
}

static const char *
grain_key_name(GrainCtx *ctx)
{
	return quote_identifier(get_attname(RelationGetRelid(ctx->heap),
										ctx->index->rd_index->indkey.values[0],
										false));
}

/* min(time key) of the rows in [$1, $2) */
static char *
grain_min_sql(GrainCtx *ctx)
{
	const char *key = grain_key_name(ctx);

	return psprintf("SELECT pg_catalog.min(%s)%s",
					key, grain_from_where(ctx, key, 1, 2));
}

/*
 * The table's rows of [$3, $4) per bucket, date_bin($1, key, $2):
 * count(*), then count, sum, min and max of each INCLUDE column.
 */
static char *
grain_range_sql(GrainCtx *ctx)
{
	Relation	index = ctx->index;
	Oid			heapoid = RelationGetRelid(ctx->heap);
	int			natts = IndexRelationGetNumberOfAttributes(index);
	const char *key = grain_key_name(ctx);
	StringInfoData sql;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT pg_catalog.date_bin($1, %s, $2)"
					 ", pg_catalog.count(*)", key);
	for (int i = 1; i < natts; i++)
	{
		AttrNumber	attnum = index->rd_index->indkey.values[i];
		const char *v = quote_identifier(get_attname(heapoid, attnum, false));

		appendStringInfo(&sql,
						 ", pg_catalog.count(%s)"
						 ", pg_catalog.sum(%s)::pg_catalog.numeric"
						 ", pg_catalog.min(%s)::pg_catalog.int8"
						 ", pg_catalog.max(%s)::pg_catalog.int8",
						 v, v, v, v);
	}
	appendStringInfoString(&sql, grain_from_where(ctx, key, 3, 4));
	appendStringInfoString(&sql, " GROUP BY 1");
	return sql.data;
}

/*
 * Open what summarization and the reader need.  The caller has done
 * SPI_connect().
 */
static void
grain_ctx_open(GrainCtx *ctx, Relation heap, Relation index, Snapshot snapshot,
			   LOCKMODE lockmode, MemoryContext aggcxt)
{
	memset(ctx, 0, sizeof(GrainCtx));
	ctx->heap = heap;
	ctx->index = index;
	ctx->cache = grain_get_cache(index);
	ctx->snapshot = snapshot;
	ctx->aggcxt = aggcxt;
	ctx->summaryrel = table_open(ctx->cache->summaryrelid, lockmode);
	ctx->summaryidx = index_open(ctx->cache->summaryidxid, lockmode);
}

static SPIPlanPtr
grain_prepare(const char *sql, int nargs, Oid *argtypes)
{
	SPIPlanPtr	plan = SPI_prepare(sql, nargs, argtypes);

	if (plan == NULL)
		elog(ERROR, "SPI_prepare failed: %s",
			 SPI_result_code_string(SPI_result));
	return plan;
}

static void
grain_ctx_close(GrainCtx *ctx)
{
	index_close(ctx->summaryidx, NoLock);
	table_close(ctx->summaryrel, NoLock);
}

/*
 * Aggregate the table's rows of [lo, hi) per bucket of the given width, one
 * of the index's levels, calling found() for each bucket that has rows.
 */
static void
grain_scan_range(GrainCtx *ctx, int64 width, TimestampTz lo, TimestampTz hi,
				 void (*found) (void *arg, const GrainAgg *agg), void *arg)
{
	GrainSpec  *spec = &ctx->cache->spec;
	Interval	itv = {.time = width};
	Datum		values[4];
	int			ret;

	if (ctx->rangeplan == NULL)
	{
		Oid			argtypes[4] = {INTERVALOID, TIMESTAMPTZOID,
		TIMESTAMPTZOID, TIMESTAMPTZOID};

		ctx->rangeplan = grain_prepare(grain_range_sql(ctx), 4, argtypes);
	}
	values[0] = IntervalPGetDatum(&itv);
	values[1] = TimestampTzGetDatum(spec->origin);
	values[2] = TimestampTzGetDatum(lo);
	values[3] = TimestampTzGetDatum(hi);

	ret = SPI_execute_snapshot(ctx->rangeplan, values, NULL, ctx->snapshot,
							   InvalidSnapshot, true, false, 0);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_snapshot failed: %s",
			 SPI_result_code_string(ret));

	for (uint64 r = 0; r < SPI_processed; r++)
	{
		Datum		cols[2 + 4 * GRAIN_MAX_VALUES];
		bool		nulls[2 + 4 * GRAIN_MAX_VALUES];
		GrainAgg	agg;
		MemoryContext oldcxt;

		/* bucket, count(*), then count, sum, min, max per value column */
		heap_deform_tuple(SPI_tuptable->vals[r], SPI_tuptable->tupdesc,
						  cols, nulls);
		grain_agg_init(&agg, DatumGetTimestampTz(cols[0]));
		agg.n = DatumGetInt64(cols[1]);
		oldcxt = MemoryContextSwitchTo(ctx->aggcxt);
		for (int i = 0; i < spec->nvalues; i++)
		{
			Datum	   *v = &cols[2 + 4 * i];

			agg.nn[i] = DatumGetInt64(v[0]);
			if (agg.nn[i] == 0)
				continue;
			agg.sum[i] = datumCopy(v[1], false, -1);
			agg.min[i] = DatumGetInt64(v[2]);
			agg.max[i] = DatumGetInt64(v[3]);
		}
		MemoryContextSwitchTo(oldcxt);
		found(arg, &agg);
	}
	SPI_freetuptable(SPI_tuptable);
}

/*
 * Visit the summary rows of one level in [lo, hi), in bucket order.
 */
static void
grain_scan_summary(GrainCtx *ctx, int16 level, TimestampTz lo, TimestampTz hi,
				   void (*found) (void *arg, int16 level, const GrainAgg *agg,
								  ItemPointer tid),
				   void *arg)
{
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	tupdesc = RelationGetDescr(ctx->summaryrel);
	MemoryContext oldcxt;

	ScanKeyInit(&keys[0], 1, BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(level));
	ScanKeyInit(&keys[1], 2, BTGreaterEqualStrategyNumber, F_TIMESTAMPTZ_GE,
				TimestampTzGetDatum(lo));
	ScanKeyInit(&keys[2], 2, BTLessStrategyNumber, F_TIMESTAMPTZ_LT,
				TimestampTzGetDatum(hi));

	scan = systable_beginscan_ordered(ctx->summaryrel, ctx->summaryidx,
									  ctx->snapshot, 3, keys);
	while ((tuple = systable_getnext_ordered(scan,
											 ForwardScanDirection)) != NULL)
	{
		GrainAgg	agg;

		oldcxt = MemoryContextSwitchTo(ctx->aggcxt);
		grain_agg_from_summary(tuple, tupdesc, ctx->cache->spec.nvalues, &agg);
		MemoryContextSwitchTo(oldcxt);
		found(arg, level, &agg, &tuple->t_self);
	}
	systable_endscan_ordered(scan);
}

static int
grain_ts_cmp(const void *a, const void *b)
{
	return pg_cmp_s64(*(const TimestampTz *) a, *(const TimestampTz *) b);
}

/*
 * The visible markers, as a sorted array of distinct buckets, with the TIDs
 * of all of them when tids is not NULL.
 */
static TimestampTz *
grain_read_markers(GrainCtx *ctx, int *nbuckets, List **tids)
{
	Relation	markerrel = table_open(ctx->cache->markerrelid,
									   AccessShareLock);
	TableScanDesc scan = table_beginscan(markerrel, ctx->snapshot, 0, NULL, 0);
	HeapTuple	tuple;
	int			n = 0;
	int			cap = 64;
	TimestampTz *buckets = palloc_array(TimestampTz, cap);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		isnull;
		Datum		d = heap_getattr(tuple, Anum_grain_marker_bucket,
									 RelationGetDescr(markerrel), &isnull);

		if (n == cap)
		{
			cap *= 2;
			buckets = repalloc_array(buckets, TimestampTz, cap);
		}
		buckets[n++] = DatumGetTimestampTz(d);
		if (tids)
		{
			ItemPointer tid = palloc_object(ItemPointerData);

			ItemPointerCopy(&tuple->t_self, tid);
			*tids = lappend(*tids, tid);
		}
	}
	table_endscan(scan);
	table_close(markerrel, AccessShareLock);

	qsort(buckets, n, sizeof(TimestampTz), grain_ts_cmp);
	*nbuckets = qunique(buckets, n, sizeof(TimestampTz), grain_ts_cmp);
	return buckets;
}

/* Whether the sorted marker array has a bucket in [lo, hi) */
static bool
grain_marked(const TimestampTz *markers, int nmarkers, TimestampTz lo,
			 TimestampTz hi)
{
	int			l = 0;
	int			h = nmarkers;

	/* first marker >= lo */
	while (l < h)
	{
		int			m = (l + h) / 2;

		if (markers[m] < lo)
			l = m + 1;
		else
			h = m;
	}
	return l < nmarkers && markers[l] < hi;
}

/* ---------------------------------------------------------------------
 * Summarization
 * ---------------------------------------------------------------------
 */

typedef struct GrainSummarizeChunk
{
	HTAB	   *rows[GRAIN_MAX_LEVELS];
	List	   *stale;			/* TIDs of stored rows to delete */
	int64		n;				/* rows the scan found */
} GrainSummarizeChunk;

static GrainRow *
summarize_row(GrainSummarizeChunk *chunk, int level, TimestampTz bucket,
			  bool *found)
{
	return (GrainRow *) hash_search(chunk->rows[level], &bucket, HASH_ENTER,
									found);
}

static void
summarize_found_stored(void *arg, int16 level, const GrainAgg *agg,
					   ItemPointer tid)
{
	GrainSummarizeChunk *chunk = (GrainSummarizeChunk *) arg;
	bool		found;
	GrainRow   *row = summarize_row(chunk, level, agg->bucket, &found);

	row->agg = *agg;
	row->stored = *tid;
	row->changed = false;
}

static void
summarize_found_stale(void *arg, int16 level, const GrainAgg *agg,
					  ItemPointer tid)
{
	GrainSummarizeChunk *chunk = (GrainSummarizeChunk *) arg;
	ItemPointer copy = palloc_object(ItemPointerData);

	*copy = *tid;
	chunk->stale = lappend(chunk->stale, copy);
}

static void
summarize_found_rows(void *arg, const GrainAgg *agg)
{
	GrainSummarizeChunk *chunk = (GrainSummarizeChunk *) arg;
	bool		found;
	GrainRow   *row = summarize_row(chunk, 0, agg->bucket, &found);

	/* The finest buckets of the range were reset to empty before the scan */
	Assert(found && row->changed);
	row->agg = *agg;
	chunk->n += agg->n;
}

/*
 * Summarize [lo, hi) of the coarsest bucket that starts at top, hi being at
 * most the watermark.
 *
 * With recompute, lo is top: every stored row of the bucket is replaced,
 * because a marker says some of it changed.  Otherwise [lo, hi) is new and
 * the stored rows before lo stay.
 *
 * A coarsest bucket summarized whole that has no rows gets only its own row,
 * with n = 0; its finer buckets get none.  Readers that descend into it
 * find no finer rows and scan, which finds nothing.
 */
static int64
summarize_chunk(GrainCtx *ctx, TimestampTz top, TimestampTz lo, TimestampTz hi,
				bool recompute, TimestampTz watermark)
{
	GrainSpec  *spec = &ctx->cache->spec;
	int			nlevels = spec->nlevels;
	int			toplevel = nlevels - 1;
	int64		topwidth = spec->width[toplevel];
	GrainSummarizeChunk chunk = {0};
	TupleDesc	tupdesc = RelationGetDescr(ctx->summaryrel);
	HASHCTL		hctl;
	ListCell   *lc;
	int64		nbuckets = 0;
	bool		emptytop;

	Assert(!recompute || lo == top);
	hctl.keysize = sizeof(TimestampTz);
	hctl.entrysize = sizeof(GrainRow);
	hctl.hcxt = ctx->aggcxt;
	for (int l = 0; l < nlevels; l++)
	{
		chunk.rows[l] = hash_create("grain summarize rows", 256, &hctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		if (recompute)
			grain_scan_summary(ctx, l, top, top + topwidth,
							   summarize_found_stale, &chunk);
		else
			grain_scan_summary(ctx, l, top, lo, summarize_found_stored,
							   &chunk);
	}

	/* Reset the finest buckets of [lo, hi) to empty, then scan their rows */
	for (TimestampTz b = lo; b < hi; b += spec->width[0])
	{
		bool		found;
		GrainRow   *row = summarize_row(&chunk, 0, b, &found);

		ItemPointerSetInvalid(&row->stored);
		grain_agg_init(&row->agg, b);
		row->changed = true;
		nbuckets++;
	}
	if (lo < hi)
		grain_scan_range(ctx, spec->width[0], lo, hi, summarize_found_rows,
						 &chunk);

	/* An empty coarsest bucket gets only its own row; see above */
	emptytop = lo == top && hi == top + topwidth && chunk.n == 0;
	if (emptytop)
	{
		bool		found;
		GrainRow   *row;

		row = summarize_row(&chunk, toplevel, top, &found);
		ItemPointerSetInvalid(&row->stored);
		grain_agg_init(&row->agg, top);
		row->changed = true;
	}
	else
	{
		/*
		 * Fold up.  A coarser bucket is built when one of its finer buckets
		 * changed, it is complete before the watermark, and every finer
		 * bucket in it has a row.
		 */
		for (int l = 1; l < nlevels; l++)
		{
			HASH_SEQ_STATUS seq;
			GrainRow   *fine;
			int64		ratio = spec->width[l] / spec->width[l - 1];

			hash_seq_init(&seq, chunk.rows[l - 1]);
			while ((fine = hash_seq_search(&seq)) != NULL)
			{
				TimestampTz b;
				GrainRow   *coarse;
				GrainAgg	agg;
				bool		found;
				bool		complete = true;

				if (!fine->changed)
					continue;
				b = grain_bin(fine->bucket, spec->width[l], spec->origin);
				if (b + spec->width[l] > watermark)
					continue;
				coarse = hash_search(chunk.rows[l], &b, HASH_FIND, &found);
				if (found && coarse->changed)
					continue;	/* already built */

				grain_agg_init(&agg, b);
				for (int64 k = 0; k < ratio; k++)
				{
					TimestampTz sub = b + k * spec->width[l - 1];
					GrainRow   *subrow = hash_search(chunk.rows[l - 1], &sub,
													 HASH_FIND, &found);

					if (!found)
					{
						complete = false;
						break;
					}
					grain_agg_add(&agg, &subrow->agg, spec->nvalues);
				}
				if (!complete)
					continue;

				coarse = summarize_row(&chunk, l, b, &found);
				if (!found)
					ItemPointerSetInvalid(&coarse->stored);
				coarse->agg = agg;
				coarse->changed = true;
			}
		}
	}

	foreach(lc, chunk.stale)
		simple_heap_delete(ctx->summaryrel, (ItemPointer) lfirst(lc));
	for (int l = 0; l < nlevels; l++)
	{
		HASH_SEQ_STATUS seq;
		GrainRow   *row;

		hash_seq_init(&seq, chunk.rows[l]);
		while ((row = hash_seq_search(&seq)) != NULL)
		{
			HeapTuple	tuple;
			Datum		keyvals[2];
			bool		keynulls[2] = {false, false};

			if (!row->changed || (emptytop && l < toplevel))
				continue;
			if (ItemPointerIsValid(&row->stored))
				simple_heap_delete(ctx->summaryrel, &row->stored);
			tuple = grain_agg_to_summary(tupdesc, l, &row->agg, spec->nvalues);
			simple_heap_insert(ctx->summaryrel, tuple);
			keyvals[0] = Int16GetDatum(l);
			keyvals[1] = TimestampTzGetDatum(row->bucket);
			index_insert(ctx->summaryidx, keyvals, keynulls, &tuple->t_self,
						 ctx->summaryrel, UNIQUE_CHECK_NO, false,
						 ctx->summaryidxinfo);
			heap_freetuple(tuple);
		}
		hash_destroy(chunk.rows[l]);
	}
	return nbuckets;
}

/*
 * Where the last summarization ended: the latest end of a summary row at any
 * level, or DT_NOBEGIN when there are none.
 */
static TimestampTz
summarize_next_bucket(GrainCtx *ctx)
{
	GrainSpec  *spec = &ctx->cache->spec;
	TimestampTz next = DT_NOBEGIN;

	for (int l = 0; l < spec->nlevels; l++)
	{
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	tuple;

		ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(l));
		scan = systable_beginscan_ordered(ctx->summaryrel, ctx->summaryidx,
										  ctx->snapshot, 1, &key);
		tuple = systable_getnext_ordered(scan, BackwardScanDirection);
		if (tuple != NULL)
		{
			bool		isnull;
			Datum		d = heap_getattr(tuple, Anum_grain_summary_bucket,
										 RelationGetDescr(ctx->summaryrel),
										 &isnull);

			next = Max(next, DatumGetTimestampTz(d) + spec->width[l]);
		}
		systable_endscan_ordered(scan);
	}
	return next;
}

/* min(time key) of the rows in the index, finite ones only, or DT_NOEND */
static TimestampTz
summarize_min_key(GrainCtx *ctx)
{
	Datum		values[2];
	int			ret;
	bool		isnull;
	Datum		d;
	TimestampTz result = DT_NOEND;

	if (ctx->minplan == NULL)
	{
		Oid			argtypes[2] = {TIMESTAMPTZOID, TIMESTAMPTZOID};

		ctx->minplan = grain_prepare(grain_min_sql(ctx), 2, argtypes);
	}
	values[0] = TimestampTzGetDatum(MIN_TIMESTAMP);
	values[1] = TimestampTzGetDatum(END_TIMESTAMP);
	ret = SPI_execute_snapshot(ctx->minplan, values, NULL, ctx->snapshot,
							   InvalidSnapshot, true, false, 0);
	if (ret != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "SPI_execute_snapshot failed: %s",
			 SPI_result_code_string(ret));
	d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1,
					  &isnull);
	if (!isnull)
		result = DatumGetTimestampTz(d);
	SPI_freetuptable(SPI_tuptable);
	return result;
}

/*
 * Check that index is a grain index and heap, opened before it, is its
 * table.  heap is NULL when IndexGetRelation() found no table, which the
 * first check then explains.  Shaped like brin_summarize_range().
 */
static void
grain_check_index(Relation index, Relation heap, Oid heapoid)
{
	if (index->rd_rel->relkind != RELKIND_INDEX ||
		index->rd_rel->relam != GRAIN_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a grain index",
						RelationGetRelationName(index))));

	/*
	 * Since the caller did the IndexGetRelation call without any lock, it's
	 * barely possible that a race against an index drop/recreation could have
	 * netted us the wrong table.  Recheck.
	 */
	if (heap == NULL ||
		heapoid != IndexGetRelation(RelationGetRelid(index), false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index \"%s\"",
						RelationGetRelationName(index))));
}

/*
 * Summarize the finest buckets up to now() - delay, and recompute every
 * coarsest bucket that has a marker.  Returns the number of finest buckets
 * computed.  The caller holds ShareUpdateExclusiveLock on the table and the
 * index; that lock conflicts with itself, so only one summarization runs per
 * index.
 */
static int64
grain_summarize_index(Relation heap, Relation index)
{
	GrainCtx	ctx;
	GrainSpec  *spec;
	TimestampTz oldmark;
	TimestampTz watermark;
	TimestampTz start;
	TimestampTz *markers;
	int			nmarkers;
	List	   *markertids = NIL;
	LOCKTAG		heaplocktag;
	int64		nsummarized = 0;
	int64		topwidth;
	int			mi;
	Relation	markerrel;
	ListCell   *lc;

	spec = &grain_get_cache(index)->spec;
	topwidth = spec->width[spec->nlevels - 1];

	/* 1. Move W forward, so that writers from now on mark what they change */
	oldmark = grain_read_watermark(index);
	watermark = grain_bin(GetCurrentTimestamp() - spec->delay, spec->width[0],
						  spec->origin);
	if (watermark > oldmark)
		grain_write_watermark(index, watermark);
	else
		watermark = oldmark;

	/*
	 * 2. Wait for every transaction that held a write lock on the table
	 * before W moved, as CREATE INDEX CONCURRENTLY does.  Writers that lock
	 * the table from now on read the new W.
	 */
	SET_LOCKTAG_RELATION(heaplocktag, MyDatabaseId, RelationGetRelid(heap));
	WaitForLockers(heaplocktag, ShareLock, false);

	/* 3. Take the snapshot after the wait, so it sees those writers' rows */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	grain_ctx_open(&ctx, heap, index, RegisterSnapshot(GetLatestSnapshot()),
				   RowExclusiveLock,
				   AllocSetContextCreate(CurrentMemoryContext,
										 "grain summarize",
										 ALLOCSET_DEFAULT_SIZES));
	ctx.summaryidxinfo = BuildIndexInfo(ctx.summaryidx);

	markers = grain_read_markers(&ctx, &nmarkers, &markertids);
	/* Writers mark only buckets before the W they read, never past ours */
	if (nmarkers > 0 && markers[nmarkers - 1] >= watermark)
		elog(ERROR, "grain index \"%s\" has a marker past its watermark",
			 RelationGetRelationName(index));

	start = summarize_next_bucket(&ctx);
	if (start == DT_NOBEGIN)
	{
		/* First summarization: start at the oldest row's coarsest bucket */
		start = summarize_min_key(&ctx);
		if (start != DT_NOEND)
			start = grain_bin(start, topwidth, spec->origin);
	}

	/*
	 * Work one coarsest bucket at a time, in order: the marked ones before
	 * the new range, then the new range [start, W), recomputing whole any
	 * coarsest bucket with a marker.
	 */
	mi = 0;
	while (mi < nmarkers || start < watermark)
	{
		TimestampTz top;
		TimestampTz next;
		bool		marked;

		if (mi < nmarkers &&
			(start >= watermark ||
			 markers[mi] < grain_bin(start, topwidth, spec->origin)))
			top = grain_bin(markers[mi], topwidth, spec->origin);
		else
			top = grain_bin(start, topwidth, spec->origin);

		marked = mi < nmarkers && markers[mi] < top + topwidth;
		while (mi < nmarkers && markers[mi] < top + topwidth)
			mi++;

		/* A marked bucket is recomputed whole; else only its new part */
		nsummarized += summarize_chunk(&ctx, top,
									   marked ? top : Max(top, start),
									   Min(top + topwidth, watermark),
									   marked, watermark);

		next = top + topwidth;
		if (start < next)
			start = Min(next, watermark);
		MemoryContextReset(ctx.aggcxt);
		CHECK_FOR_INTERRUPTS();
	}

	/* The markers this snapshot saw are now folded in */
	markerrel = table_open(ctx.cache->markerrelid, RowExclusiveLock);
	foreach(lc, markertids)
		simple_heap_delete(markerrel, (ItemPointer) lfirst(lc));
	table_close(markerrel, NoLock);

	UnregisterSnapshot(ctx.snapshot);
	grain_ctx_close(&ctx);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return nsummarized;
}

/*
 * grain_summarize(index regclass) returns bigint
 *
 * Summarize what the delay allows; see grain_summarize_index.  Shaped like
 * brin_summarize_range().
 */
Datum
grain_summarize(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	heap;
	Relation	index;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	int64		nsummarized = 0;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Control functions of grain indexes cannot be executed during recovery.")));
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain_summarize() must run at READ COMMITTED"),
				 errdetail("It needs a snapshot taken after it waits for writers.")));

	/*
	 * We must lock table before index to avoid deadlocks.  However, if the
	 * passed indexoid isn't an index then IndexGetRelation() will fail.
	 * Rather than emitting a not-very-helpful error message, postpone
	 * complaining, expecting that the is-it-an-index test below will fail.
	 */
	heapoid = IndexGetRelation(indexoid, true);
	if (OidIsValid(heapoid))
	{
		heap = table_open(heapoid, ShareUpdateExclusiveLock);

		/*
		 * Run the table's index predicate and the summary queries as the
		 * table owner, with security-restricted operations locked down and
		 * GUC changes kept local, as brin_summarize_range() does.
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heap->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
		RestrictSearchPath();
	}
	else
	{
		heap = NULL;
		/* Set these just to suppress "uninitialized variable" warnings */
		save_userid = InvalidOid;
		save_sec_context = -1;
		save_nestlevel = -1;
	}

	index = index_open(indexoid, ShareUpdateExclusiveLock);

	grain_check_index(index, heap, heapoid);

	/* User must own the index (comparable to privileges needed for VACUUM) */
	if (!object_ownercheck(RelationRelationId, indexoid, save_userid))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX,
					   RelationGetRelationName(index));

	/* see gin_clean_pending_list() */
	if (index->rd_index->indisvalid)
		nsummarized = grain_summarize_index(heap, index);
	else
		ereport(DEBUG1,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(index))));

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	index_close(index, ShareUpdateExclusiveLock);
	table_close(heap, ShareUpdateExclusiveLock);

	PG_RETURN_INT64(nsummarized);
}

/* ---------------------------------------------------------------------
 * The reader
 * ---------------------------------------------------------------------
 */

typedef struct GrainRange
{
	TimestampTz lo;
	TimestampTz hi;
} GrainRange;

typedef struct GrainReadState
{
	GrainCtx   *ctx;
	TimestampTz lo;				/* the window */
	TimestampTz hi;
	int			outlevel;
	TimestampTz first;			/* first output bucket */
	int			nout;
	GrainAgg   *out;
	TimestampTz *markers;
	int			nmarkers;
	TimestampTz watermark;		/* no summary rows from here on */
	/* ranges of rows to scan, merged as they come */
	GrainRange *scan;
	int			nscan;
	int			scancap;
} GrainReadState;

typedef struct GrainLevelRows
{
	GrainAgg   *rows;
	int			n;
	int			cap;
} GrainLevelRows;

static void
read_found_stored(void *arg, int16 level, const GrainAgg *agg, ItemPointer tid)
{
	GrainLevelRows *lr = (GrainLevelRows *) arg;

	if (lr->n == lr->cap)
	{
		lr->cap = Max(16, lr->cap * 2);
		lr->rows = lr->rows ? repalloc_array(lr->rows, GrainAgg, lr->cap)
			: palloc_array(GrainAgg, lr->cap);
	}
	lr->rows[lr->n++] = *agg;
}

static int
read_bucket_cmp(const void *key, const void *row)
{
	return pg_cmp_s64(*(const TimestampTz *) key,
					  ((const GrainAgg *) row)->bucket);
}

static GrainAgg *
read_lookup(GrainLevelRows *lr, TimestampTz bucket)
{
	if (lr->n == 0)
		return NULL;
	return bsearch(&bucket, lr->rows, lr->n, sizeof(GrainAgg),
				   read_bucket_cmp);
}

static void
read_add_scan(GrainReadState *rs, TimestampTz lo, TimestampTz hi)
{
	if (rs->nscan > 0 && rs->scan[rs->nscan - 1].hi == lo)
	{
		rs->scan[rs->nscan - 1].hi = hi;
		return;
	}
	if (rs->nscan == rs->scancap)
	{
		rs->scancap = Max(16, rs->scancap * 2);
		rs->scan = rs->scan ? repalloc_array(rs->scan, GrainRange, rs->scancap)
			: palloc_array(GrainRange, rs->scancap);
	}
	rs->scan[rs->nscan].lo = lo;
	rs->scan[rs->nscan].hi = hi;
	rs->nscan++;
}

/*
 * Add bucket b of a level to agg, from its summary row if it can be used,
 * else from finer levels, else by scanning its rows.  rows holds the
 * level's visible summary rows around b.
 */
static void
read_bucket(GrainReadState *rs, int level, TimestampTz b, GrainLevelRows *rows,
			GrainAgg *agg)
{
	GrainSpec  *spec = &rs->ctx->cache->spec;
	TimestampTz end = b + spec->width[level];
	TimestampTz lo = Max(b, rs->lo);
	TimestampTz hi = Min(end, rs->hi);
	GrainAgg   *row;
	GrainLevelRows finer = {0};

	if (lo >= hi)
		return;

	/* Nothing at or after the watermark has a summary row at any level */
	if (b >= rs->watermark)
	{
		read_add_scan(rs, lo, hi);
		return;
	}

	if (lo == b && hi == end &&
		!grain_marked(rs->markers, rs->nmarkers, b, end) &&
		(row = read_lookup(rows, b)) != NULL)
	{
		grain_agg_add(agg, row, spec->nvalues);
		return;
	}

	if (level == 0)
	{
		read_add_scan(rs, lo, hi);
		return;
	}

	grain_scan_summary(rs->ctx, level - 1, b, end, read_found_stored, &finer);
	for (TimestampTz sub = b; sub < end; sub += spec->width[level - 1])
		read_bucket(rs, level - 1, sub, &finer, agg);
	if (finer.rows)
		pfree(finer.rows);
}

static void
read_found_rows(void *arg, const GrainAgg *agg)
{
	GrainReadState *rs = (GrainReadState *) arg;
	GrainSpec  *spec = &rs->ctx->cache->spec;
	TimestampTz b = grain_bin(agg->bucket, spec->width[rs->outlevel],
							  spec->origin);
	int64		i = (b - rs->first) / spec->width[rs->outlevel];

	Assert(i >= 0 && i < rs->nout);
	grain_agg_add(&rs->out[i], agg, spec->nvalues);
}

/*
 * grain_buckets(index regclass, width interval, lower timestamptz,
 *					upper timestamptz)
 *
 * count(*), and count, sum, min and max of each INCLUDE column, per bucket
 * of the given width for the rows with lower <= time key < upper, as the
 * plain GROUP BY date_bin(width, key, origin) would give them.  Buckets with
 * no rows are left out.  The width must be one of the index's levels.
 */
Datum
grain_buckets(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Interval   *width = PG_GETARG_INTERVAL_P(1);
	TimestampTz lo = PG_GETARG_TIMESTAMPTZ(2);
	TimestampTz hi = PG_GETARG_TIMESTAMPTZ(3);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			heapoid;
	Relation	heap;
	Relation	index;
	GrainCtx	ctx;
	GrainSpec  *spec;
	GrainReadState rs = {0};
	GrainLevelRows top = {0};
	int64		usecs;
	int			save_nestlevel;
	AclResult	aclresult;

	InitMaterializedSRF(fcinfo, 0);

	/* Lock the table before the index; see grain_summarize() */
	heapoid = IndexGetRelation(indexoid, true);
	heap = OidIsValid(heapoid) ? table_open(heapoid, AccessShareLock) : NULL;
	index = index_open(indexoid, AccessShareLock);
	grain_check_index(index, heap, heapoid);

	/* Summaries bypass row-level security, so it must be off */
	aclresult = pg_class_aclcheck(heapoid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(heap->rd_rel->relkind),
					   RelationGetRelationName(heap));
	if (check_enable_rls(heapoid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain_buckets() is not supported on tables with row-level security")));
	if (!index->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(index))));

	if (TIMESTAMP_NOT_FINITE(lo) || TIMESTAMP_NOT_FINITE(hi))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the window of grain_buckets() must be finite")));

	spec = &grain_get_cache(index)->spec;
	usecs = grain_interval_usecs(width, "width");
	rs.outlevel = -1;
	for (int l = 0; l < spec->nlevels; l++)
		if (spec->width[l] == usecs)
			rs.outlevel = l;
	if (rs.outlevel < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("width must be one of the bucket widths of index \"%s\"",
						RelationGetRelationName(index))));

	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	grain_ctx_open(&ctx, heap, index, GetActiveSnapshot(), AccessShareLock,
				   CurrentMemoryContext);

	rs.ctx = &ctx;
	rs.lo = lo;
	rs.hi = hi;
	if (lo < hi)
	{
		int64		nout;

		rs.first = grain_bin(lo, usecs, spec->origin);
		nout = (hi - rs.first + usecs - 1) / usecs;
		if (nout > GRAIN_MAX_OUTPUT_BUCKETS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("window of grain_buckets() has too many buckets"),
					 errhint("Use a wider bucket width or a shorter window.")));
		rs.nout = (int) nout;
	}
	rs.out = palloc_array(GrainAgg, Max(rs.nout, 1));
	rs.markers = grain_read_markers(&ctx, &rs.nmarkers, NULL);
	rs.watermark = grain_read_watermark(index);

	grain_scan_summary(&ctx, rs.outlevel, rs.first,
					   rs.first + (int64) rs.nout * usecs,
					   read_found_stored, &top);
	for (int i = 0; i < rs.nout; i++)
	{
		TimestampTz b = rs.first + i * usecs;

		grain_agg_init(&rs.out[i], b);
		read_bucket(&rs, rs.outlevel, b, &top, &rs.out[i]);
	}
	/* Scan at the output width, so a range returns a row per output bucket */
	for (int i = 0; i < rs.nscan; i++)
		grain_scan_range(&ctx, usecs, rs.scan[i].lo, rs.scan[i].hi,
						 read_found_rows, &rs);

	for (int i = 0; i < rs.nout; i++)
	{
		Datum		values[6];
		bool		nulls[6] = {false};
		GrainAgg   *agg = &rs.out[i];

		if (agg->n == 0)
			continue;
		values[0] = TimestampTzGetDatum(agg->bucket);
		values[1] = Int64GetDatum(agg->n);
		grain_agg_arrays(agg, spec->nvalues, &values[2]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
							 nulls);
	}

	grain_ctx_close(&ctx);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
	AtEOXact_GUC(false, save_nestlevel);

	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);

	return (Datum) 0;
}
