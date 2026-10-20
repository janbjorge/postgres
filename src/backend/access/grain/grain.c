/*-------------------------------------------------------------------------
 *
 * grain.c
 *	  grain, the Grouped Range Aggregate INdex.
 *
 * A grain index keeps exact count, sum, min and max per time bucket.  Rows
 * stamped after the watermark W cost one comparison on insert.  A write to a
 * bucket before W leaves a marker in the same transaction, so readers know to
 * scan that bucket until the next summarization recomputes it.  See
 * grain_summarize.c for summarization and the reader, and grain_state.c for
 * the metapage and the summary and marker tables.
 *
 * The index answers no scans: amgettuple and amgetbitmap are NULL, so the
 * planner never builds a path for it.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/grain/grain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/grain_private.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_opclass.h"
#include "commands/vacuum.h"
#include "common/int.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/* Default origin: 2000-01-01 00:00+00, timestamptz zero */
#define GRAIN_DEFAULT_ORIGIN	((TimestampTz) 0)

/*
 * What aminsert keeps for one statement in ii_AmCache: the watermark, read
 * once, and the buckets already marked.
 */
typedef struct GrainInsertState
{
	TimestampTz watermark;
	HTAB	   *marked;
	Relation	markerrel;
} GrainInsertState;

/*
 * An interval in microseconds.  Months and years are refused, as date_bin()
 * refuses them.  "what" names the interval in error messages.
 */
int64
grain_interval_usecs(const Interval *itv, const char *what)
{
	int64		usecs;

	if (INTERVAL_NOT_FINITE(itv))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s must be finite", what)));
	if (itv->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cannot contain months or years", what)));
	if (pg_mul_s64_overflow((int64) itv->day, USECS_PER_DAY, &usecs) ||
		pg_add_s64_overflow(usecs, itv->time, &usecs))
		ereport(ERROR,
				(errcode(ERRCODE_INTERVAL_FIELD_OVERFLOW),
				 errmsg("%s is out of range", what)));
	return usecs;
}

/*
 * Parse an interval for a reloption, in microseconds.
 */
static int64
grain_parse_width(const char *text, const char *option, bool allow_zero)
{
	Interval   *itv;
	int64		usecs;
	char	   *what = psprintf("\"%s\" of a grain index", option);

	itv = DatumGetIntervalP(DirectFunctionCall3(interval_in,
												CStringGetDatum(text),
												ObjectIdGetDatum(InvalidOid),
												Int32GetDatum(-1)));
	usecs = grain_interval_usecs(itv, what);
	pfree(what);
	if (usecs < 0 && allow_zero)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" of a grain index cannot be negative",
						option)));
	if (usecs <= 0 && !allow_zero)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" of a grain index must be positive", option)));
	return usecs;
}

/*
 * Parse "buckets": one or more widths, finest first, each dividing the next.
 */
static int
grain_parse_buckets(const char *value, int64 *width)
{
	char	   *copy = pstrdup(value);
	char	   *tok;
	char	   *save;
	int			n = 0;

	for (tok = strtok_r(copy, ",", &save); tok;
		 tok = strtok_r(NULL, ",", &save))
	{
		if (n >= GRAIN_MAX_LEVELS)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a grain index can have at most %d bucket widths",
							GRAIN_MAX_LEVELS)));
		width[n] = grain_parse_width(tok, "buckets", false);
		if (n > 0 && (width[n] <= width[n - 1] ||
					  width[n] % width[n - 1] != 0))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("each bucket width of a grain index must be a multiple of the one before it"),
					 errhint("List the widths finest first, as in '1 minute, 1 hour, 1 day'.")));
		n++;
	}
	pfree(copy);

	if (n == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"buckets\" of a grain index must list at least one width")));
	return n;
}

static TimestampTz
grain_parse_origin(const char *value)
{
	Datum		d;
	TimestampTz origin;

	d = DirectFunctionCall3(timestamptz_in, CStringGetDatum(value),
							ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	origin = DatumGetTimestampTz(d);
	if (TIMESTAMP_NOT_FINITE(origin))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"origin\" of a grain index must be finite")));
	return origin;
}

void
grain_validate_buckets(const char *value)
{
	int64		width[GRAIN_MAX_LEVELS];

	if (value != NULL)
		(void) grain_parse_buckets(value, width);
}

void
grain_validate_origin(const char *value)
{
	if (value != NULL)
		(void) grain_parse_origin(value);
}

void
grain_validate_delay(const char *value)
{
	if (value != NULL)
		(void) grain_parse_width(value, "delay", true);
}

/*
 * The bucket levels as the reloptions give them.  Only ambuild reads them
 * here; everyone else reads the copy in the metapage (grain_get_cache).
 */
static void
grain_options_spec(Relation index, GrainSpec *spec)
{
	GrainOptions *opts = (GrainOptions *) index->rd_options;
	const char *buckets =
		opts ? GET_STRING_RELOPTION(opts, bucketsOffset) : NULL;
	const char *origin =
		opts ? GET_STRING_RELOPTION(opts, originOffset) : NULL;

	if (buckets == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("grain index \"%s\" needs the \"buckets\" option",
						RelationGetRelationName(index)),
				 errhint("For example: WITH (buckets = '1 minute, 1 hour, 1 day').")));

	spec->nlevels = grain_parse_buckets(buckets, spec->width);
	spec->origin = origin ? grain_parse_origin(origin) : GRAIN_DEFAULT_ORIGIN;
}

/*
 * The "delay" reloption, or the finest width when it is not set.  Unlike
 * the levels it can change at any time: it only decides how far the next
 * summarization goes.
 */
int64
grain_options_delay(Relation index, int64 finest)
{
	GrainOptions *opts = (GrainOptions *) index->rd_options;
	const char *delay = opts ? GET_STRING_RELOPTION(opts, delayOffset) : NULL;

	return delay ? grain_parse_width(delay, "delay", true) : finest;
}

/*
 * date_bin(width, ts, origin), for a width in microseconds.  This is the
 * arithmetic of timestamptz_bin(), without its interval handling.
 */
TimestampTz
grain_bin(TimestampTz ts, int64 width, TimestampTz origin)
{
	int64		diff;
	int64		delta;
	TimestampTz result;

	if (pg_sub_s64_overflow(ts, origin, &diff))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	delta = diff - diff % width;
	if (diff < 0 && diff % width != 0)
		delta -= width;
	if (pg_add_s64_overflow(origin, delta, &result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	return result;
}

static bytea *
grainoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"buckets", RELOPT_TYPE_STRING, offsetof(GrainOptions, bucketsOffset)},
		{"origin", RELOPT_TYPE_STRING, offsetof(GrainOptions, originOffset)},
		{"delay", RELOPT_TYPE_STRING, offsetof(GrainOptions, delayOffset)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_GRAIN,
									  sizeof(GrainOptions),
									  tab, lengthof(tab));
}

/*
 * Refuse what grain doesn't support yet, then create the metapage and the
 * summary and marker tables.  Nothing is summarized: the watermark starts at
 * -infinity, so no write needs a marker until the first summarization.
 */
static IndexBuildResult *
grainbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	TupleDesc	itupdesc = RelationGetDescr(index);
	GrainSpec	spec;
	int			nkeys = IndexRelationGetNumberOfKeyAttributes(index);
	int			natts = IndexRelationGetNumberOfAttributes(index);

	if (heap->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain indexes are not supported on temporary or unlogged tables")));
	if (IsCatalogRelation(heap))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain indexes are not supported on system catalogs")));
	if (IsBinaryUpgrade)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain indexes are not supported by binary upgrade"),
				 errhint("Drop the grain index before the upgrade and create it again after.")));
	if (indexInfo->ii_Expressions != NIL ||
		indexInfo->ii_IndexAttrNumbers[0] == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain indexes do not support expressions")));
	if (nkeys != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain indexes support only one key column, the time key")));
	if (natts - nkeys > GRAIN_MAX_VALUES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("grain indexes support at most %d INCLUDE columns",
						GRAIN_MAX_VALUES)));
	for (int i = nkeys; i < natts; i++)
	{
		Oid			typid = TupleDescAttr(itupdesc, i)->atttypid;

		if (typid != INT2OID && typid != INT4OID && typid != INT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INCLUDE column \"%s\" of a grain index must be smallint, integer or bigint",
							NameStr(TupleDescAttr(itupdesc, i)->attname))));
	}

	/* Fails if "buckets" is missing */
	grain_options_spec(index, &spec);

	grain_build_state(index, &spec);

	result = palloc0_object(IndexBuildResult);
	/* Leave the table's reltuples alone: the build reads no rows */
	result->heap_tuples = heap->rd_rel->reltuples;
	result->index_tuples = 0;
	return result;
}

static void
grainbuildempty(Relation index)
{
	/* Unlogged tables are refused by grainbuild */
	elog(ERROR, "grain indexes are not supported on unlogged tables");
}

/*
 * The per-statement insert state, made on first use.
 */
static GrainInsertState *
grain_insert_state(Relation index, IndexInfo *indexInfo)
{
	GrainInsertState *state = (GrainInsertState *) indexInfo->ii_AmCache;

	if (state != NULL)
		return state;

	state = MemoryContextAllocZero(indexInfo->ii_Context,
								   sizeof(GrainInsertState));

	/*
	 * The table is locked for the whole statement, so a watermark read once
	 * serves it: a summarization that moves W after this waits for our
	 * transaction to end before it reads rows (grain_summarize.c).
	 */
	state->watermark = grain_read_watermark(index);

	indexInfo->ii_AmCache = state;
	return state;
}

/*
 * A row version with this time key appeared or went away.  If its bucket
 * lies before the watermark, insert a marker for it, so that readers scan
 * the bucket and the next summarization recomputes it.
 */
static void
grain_note_write(Relation index, IndexInfo *indexInfo, Datum key, bool isnull)
{
	GrainInsertState *state;
	GrainCache *cache;
	TimestampTz ts;
	TimestampTz bucket;
	bool		found;

	/* Rows with a NULL time key are in no bucket */
	if (isnull)
		return;

	/* Neither are infinite ones: windows are finite */
	ts = DatumGetTimestampTz(key);
	if (TIMESTAMP_NOT_FINITE(ts))
		return;

	state = grain_insert_state(index, indexInfo);
	if (ts >= state->watermark)
		return;

	cache = grain_get_cache(index);
	bucket = grain_bin(ts, cache->spec.width[0], cache->spec.origin);

	/* Made on the first write behind W; most statements never get here */
	if (state->marked == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(TimestampTz);
		ctl.entrysize = sizeof(TimestampTz);
		ctl.hcxt = indexInfo->ii_Context;
		state->marked = hash_create("grain marked buckets", 64, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		state->markerrel = table_open(cache->markerrelid, RowExclusiveLock);
	}

	(void) hash_search(state->marked, &bucket, HASH_ENTER, &found);
	if (found)
		return;

	grain_insert_marker(state->markerrel, bucket);
}

static bool
graininsert(Relation index, Datum *values, bool *isnull,
			ItemPointer ht_ctid, Relation heapRel,
			IndexUniqueCheck checkUnique,
			bool indexUnchanged,
			IndexInfo *indexInfo)
{
	grain_note_write(index, indexInfo, values[0], isnull[0]);
	return false;
}

static void
grainnotifydelete(Relation index, Datum *values, bool *isnull,
				  ItemPointer ht_ctid, Relation heapRel,
				  IndexInfo *indexInfo)
{
	grain_note_write(index, indexInfo, values[0], isnull[0]);
}

static void
graininsertcleanup(Relation index, IndexInfo *indexInfo)
{
	GrainInsertState *state = (GrainInsertState *) indexInfo->ii_AmCache;

	if (state == NULL)
		return;

	/* clear the cache first, to avoid a dangling pointer if we fail below */
	indexInfo->ii_AmCache = NULL;
	if (state->marked != NULL)
	{
		table_close(state->markerrel, NoLock);
		hash_destroy(state->marked);
	}
	pfree(state);
}

/*
 * The index holds no pointers to table rows, so VACUUM has nothing to do.
 * The summary and marker tables are vacuumed as tables of their own.
 */
static IndexBulkDeleteResult *
grainbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				IndexBulkDeleteCallback callback, void *callback_state)
{
	return stats;
}

static IndexBulkDeleteResult *
grainvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return stats;
}

/*
 * Never reached: with no amgettuple or amgetbitmap the planner builds no
 * path for the index.
 */
static void
graincostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				  Cost *indexStartupCost, Cost *indexTotalCost,
				  Selectivity *indexSelectivity, double *indexCorrelation,
				  double *indexPages)
{
	*indexStartupCost = 1.0e10;
	*indexTotalCost = 1.0e10;
	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;
	*indexPages = 1.0;
}

/*
 * Validator for a grain opclass.  grain answers no scans and calls no
 * support functions, so an opclass only names the time key's type.  Its
 * opfamily holds the type's equality operator, so that the opclass has a
 * member as every opclass must.
 */
static bool
grainvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	char	   *opclassname;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opclassname = NameStr(classform->opcname);
	opfamilyname = get_opfamily_name(opfamilyoid, false);

	if (opcintype != TIMESTAMPTZOID)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("grain opclass %s has input type %s, but only timestamp with time zone is supported",
						opclassname, format_type_be(opcintype))));
		result = false;
	}

	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));
	if (proclist->n_members > 0)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("grain opfamily %s contains support functions, but grain uses none",
						opfamilyname)));
		result = false;
	}

	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	for (int i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		if (oprform->amopstrategy != 1 ||
			oprform->amoppurpose != AMOP_SEARCH ||
			oprform->amoplefttype != oprform->amoprighttype ||
			!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("grain opfamily %s contains operator %s, but only an equality operator with strategy 1 is allowed",
							opfamilyname,
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(classtup);

	return result;
}

static IndexScanDesc
grainbeginscan(Relation index, int nkeys, int norderbys)
{
	elog(ERROR, "grain indexes do not support scans");
	return NULL;				/* keep compiler quiet */
}

static void
grainrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
			ScanKey orderbys, int norderbys)
{
	elog(ERROR, "grain indexes do not support scans");
}

static void
grainendscan(IndexScanDesc scan)
{
	elog(ERROR, "grain indexes do not support scans");
}

Datum
grainhandler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amstrategies = 0,
		.amsupport = 0,
		.amoptsprocnum = 0,
		.amcanorder = false,
		.amcanorderbyop = false,
		.amcanhash = false,
		.amconsistentequality = false,
		.amconsistentordering = false,
		.amcanbackward = false,
		.amcanunique = false,
		.amcanmulticol = false,
		.amoptionalkey = true,
		.amsearcharray = false,
		.amsearchnulls = false,
		.amstorage = false,
		.amclusterable = false,
		.ampredlocks = false,
		.amcanparallel = false,
		.amcanbuildparallel = false,
		.amcaninclude = true,
		.amusemaintenanceworkmem = false,
		.amsummarizing = true,
		.amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL,
		.amkeytype = InvalidOid,

		.ambuild = grainbuild,
		.ambuildempty = grainbuildempty,
		.aminsert = graininsert,
		.aminsertcleanup = graininsertcleanup,
		.amnotifydelete = grainnotifydelete,
		.ambulkdelete = grainbulkdelete,
		.amvacuumcleanup = grainvacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = graincostestimate,
		.amgettreeheight = NULL,
		.amoptions = grainoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = grainvalidate,
		.amadjustmembers = NULL,
		.ambeginscan = grainbeginscan,
		.amrescan = grainrescan,
		.amgettuple = NULL,
		.amgetbitmap = NULL,
		.amendscan = grainendscan,
		.ammarkpos = NULL,
		.amrestrpos = NULL,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
		.amtranslatestrategy = NULL,
		.amtranslatecmptype = NULL,
	};

	PG_RETURN_POINTER(&amroutine);
}
