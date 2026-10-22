/*-------------------------------------------------------------------------
 *
 * grain_state.c
 *	  The state a grain index keeps: the metapage, and a summary table and a
 *	  marker table.
 *
 * The metapage, block 0, holds the bucket levels, the watermark W and the
 * OIDs of the two tables.  W only moves forward; it is WAL-logged with
 * generic WAL and is not transactional.
 *
 * The two tables are ordinary heaps in pg_toast, owned by the index the way
 * a TOAST table is owned by its table (an INTERNAL dependency), so they drop
 * with it and live in its tablespace.  The summary table holds one row per
 * summarized bucket per level, with a btree on (level, bucket).  The marker
 * table holds one row per finest bucket that changed after it was
 * summarized.  Being MVCC heaps, both give every snapshot its own view.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/grain/grain_state.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/grain_private.h"
#include "access/heapam.h"
#include "access/toast_compression.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/storage.h"
#include "commands/tablecmds.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

/*
 * Make a tuple descriptor whose columns are never toasted or compressed: the
 * summary and marker tables have no TOAST table, and their rows are small.
 */
static TupleDesc
grain_plain_tupdesc(int natts, const char *const *names, const Oid *types)
{
	TupleDesc	tupdesc = CreateTemplateTupleDesc(natts);

	for (int i = 0; i < natts; i++)
	{
		TupleDescInitEntry(tupdesc, (AttrNumber) (i + 1), names[i], types[i],
						   -1, 0);
		TupleDescAttr(tupdesc, i)->attstorage = TYPSTORAGE_PLAIN;
		TupleDescAttr(tupdesc, i)->attcompression = InvalidCompressionMethod;
		populate_compact_attribute(tupdesc, i);
	}
	TupleDescFinalize(tupdesc);
	return tupdesc;
}

static Oid
grain_create_heap(Relation index, const char *name, TupleDesc tupdesc)
{
	return heap_create_with_catalog(name,
									PG_TOAST_NAMESPACE,
									index->rd_rel->reltablespace,
									InvalidOid,
									InvalidOid,
									InvalidOid,
									BOOTSTRAP_SUPERUSERID,
									HEAP_TABLE_AM_OID,
									tupdesc,
									NIL,
									RELKIND_RELATION,
									RELPERSISTENCE_PERMANENT,
									false,
									false,
									ONCOMMIT_NOOP,
									(Datum) 0,
									false,
									true,
									true,
									InvalidOid,
									NULL);
}

/*
 * Create the summary and marker tables of a new index, and the summary
 * table's btree.  They are owned by the bootstrap superuser, so the table's
 * owner can't write summaries or delete markers; grain itself reads and
 * writes them below the permission checks.
 */
static void
grain_create_state(Relation index,
				   Oid *summaryrelid, Oid *summaryidxid, Oid *markerrelid)
{
	static const char *const summary_names[] = {
		"level", "bucket", "n", "nn", "sum", "min", "max"
	};
	static const Oid summary_types[] = {
		INT2OID, TIMESTAMPTZOID, INT8OID, INT8ARRAYOID, NUMERICARRAYOID,
		INT8ARRAYOID, INT8ARRAYOID
	};
	static const char *const marker_names[] = {"bucket"};
	static const Oid marker_types[] = {TIMESTAMPTZOID};
	Oid			indexoid = RelationGetRelid(index);
	char		name[NAMEDATALEN];
	char		idxname[NAMEDATALEN];
	Relation	summaryrel;
	IndexInfo  *indexInfo;
	Oid			collationIds[2] = {InvalidOid, InvalidOid};
	Oid			opclassIds[2] = {INT2_BTREE_OPS_OID, TIMESTAMPTZ_BTREE_OPS_OID};
	int16		coloptions[2] = {0, 0};
	ObjectAddress indexobj;
	ObjectAddress child;

	snprintf(name, sizeof(name), "pg_grain_%u_summary", indexoid);
	*summaryrelid = grain_create_heap(index, name,
									  grain_plain_tupdesc(Natts_grain_summary,
														  summary_names,
														  summary_types));
	snprintf(name, sizeof(name), "pg_grain_%u_marker", indexoid);
	*markerrelid = grain_create_heap(index, name,
									 grain_plain_tupdesc(Natts_grain_marker,
														 marker_names,
														 marker_types));

	/* make the heaps visible, else table_open will fail */
	CommandCounterIncrement();

	summaryrel = table_open(*summaryrelid, ShareLock);

	indexInfo = makeIndexInfo(2, 2, BTREE_AM_OID, NIL, NIL,
							  false, false, true, false, false, false);
	indexInfo->ii_IndexAttrNumbers[0] = Anum_grain_summary_level;
	indexInfo->ii_IndexAttrNumbers[1] = Anum_grain_summary_bucket;

	snprintf(idxname, sizeof(idxname), "pg_grain_%u_summary_index", indexoid);
	*summaryidxid = index_create(summaryrel, idxname, InvalidOid, InvalidOid,
								 InvalidOid, InvalidOid, indexInfo,
								 list_make2("level", "bucket"),
								 BTREE_AM_OID, index->rd_rel->reltablespace,
								 collationIds, opclassIds, NULL, coloptions,
								 NULL, (Datum) 0,
								 INDEX_CREATE_SUPPRESS_PROGRESS,
								 0, true, true, NULL);
	table_close(summaryrel, NoLock);

	/* The tables belong to the index, and drop with it */
	ObjectAddressSet(indexobj, RelationRelationId, indexoid);
	ObjectAddressSet(child, RelationRelationId, *summaryrelid);
	recordDependencyOn(&child, &indexobj, DEPENDENCY_INTERNAL);
	ObjectAddressSet(child, RelationRelationId, *markerrelid);
	recordDependencyOn(&child, &indexobj, DEPENDENCY_INTERNAL);

	CommandCounterIncrement();
}

/*
 * Give a summary or marker table, or the summary table's btree, new and
 * empty storage in the index's tablespace, as REINDEX gives an index new
 * storage (reindex_index).  That is transactional, so a rollback brings the
 * old rows back with the old metapage.
 */
static void
grain_reset_storage(Relation rel, Oid tablespace)
{
	if (CheckRelationTableSpaceMove(rel, tablespace))
	{
		SetRelationTableSpace(rel, tablespace, InvalidRelFileNumber);
		RelationDropStorage(rel);
		RelationAssumeNewRelfilelocator(rel);
		CommandCounterIncrement();
	}
	RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
}

/*
 * Empty the summary and marker tables of an index being rebuilt (REINDEX,
 * TRUNCATE, VACUUM FULL, CLUSTER), and follow the index if REINDEX moved it
 * to another tablespace.  reindex_relation() can't be used: it refuses to
 * run inside the REINDEX of the grain index itself.
 */
static void
grain_reset_state(Relation index, Oid summaryrelid, Oid summaryidxid,
				  Oid markerrelid)
{
	Oid			tablespace = index->rd_rel->reltablespace;
	Relation	summaryrel = table_open(summaryrelid, AccessExclusiveLock);
	Relation	markerrel = table_open(markerrelid, AccessExclusiveLock);
	Relation	summaryidx = index_open(summaryidxid, AccessExclusiveLock);

	grain_reset_storage(summaryrel, tablespace);
	grain_reset_storage(markerrel, tablespace);
	grain_reset_storage(summaryidx, tablespace);
	index_build(summaryrel, summaryidx, BuildIndexInfo(summaryidx),
				true, false, false);

	index_close(summaryidx, NoLock);
	table_close(markerrel, NoLock);
	table_close(summaryrel, NoLock);
}

/*
 * Find the summary and marker tables of an index through their INTERNAL
 * dependencies on it, as getOwnedSequences() finds a table's sequences.
 * Returns false if the index has none yet.
 */
static bool
grain_find_state(Relation index, Oid *summaryrelid, Oid *summaryidxid,
				 Oid *markerrelid)
{
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;
	Relation	summaryrel;
	List	   *idxlist;

	*summaryrelid = InvalidOid;
	*markerrelid = InvalidOid;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(index)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		if (deprec->classid != RelationRelationId ||
			deprec->deptype != DEPENDENCY_INTERNAL)
			continue;

		/* Only the summary table has a "level" column */
		if (get_attnum(deprec->objid, "level") != InvalidAttrNumber)
			*summaryrelid = deprec->objid;
		else
			*markerrelid = deprec->objid;
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	if (!OidIsValid(*summaryrelid) && !OidIsValid(*markerrelid))
		return false;
	if (!OidIsValid(*summaryrelid) || !OidIsValid(*markerrelid))
		elog(ERROR,
			 "grain index \"%s\" is missing its summary or marker table",
			 RelationGetRelationName(index));

	summaryrel = table_open(*summaryrelid, AccessShareLock);
	idxlist = RelationGetIndexList(summaryrel);
	if (list_length(idxlist) != 1)
		elog(ERROR,
			 "summary table of grain index \"%s\" has %d indexes, expected 1",
			 RelationGetRelationName(index), list_length(idxlist));
	*summaryidxid = linitial_oid(idxlist);
	list_free(idxlist);
	table_close(summaryrel, AccessShareLock);
	return true;
}

/*
 * Called by ambuild on new, empty index storage: find or create the summary
 * and marker tables, empty them, and write a metapage with the bucket levels
 * and the watermark at -infinity.
 */
void
grain_build_state(Relation index, const GrainSpec *spec)
{
	Oid			summaryrelid;
	Oid			summaryidxid;
	Oid			markerrelid;
	Buffer		buf;
	Page		page;
	GenericXLogState *xlogstate;
	GrainMetaPageData *meta;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	if (grain_find_state(index, &summaryrelid, &summaryidxid, &markerrelid))
		grain_reset_state(index, summaryrelid, summaryidxid, markerrelid);
	else
		grain_create_state(index, &summaryrelid, &summaryidxid, &markerrelid);

	buf = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(buf) == GRAIN_METAPAGE_BLKNO);

	xlogstate = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(xlogstate, buf, GENERIC_XLOG_FULL_IMAGE);
	PageInit(page, BLCKSZ, sizeof(GrainPageOpaqueData));
	GrainPageGetOpaque(page)->grain_page_id = GRAIN_PAGE_ID;
	meta = GrainPageGetMeta(page);
	meta->magic = GRAIN_META_MAGIC;
	meta->version = GRAIN_CURRENT_VERSION;
	meta->watermark = DT_NOBEGIN;
	meta->summaryrelid = summaryrelid;
	meta->summaryidxid = summaryidxid;
	meta->markerrelid = markerrelid;
	meta->nlevels = spec->nlevels;
	meta->origin = spec->origin;
	memcpy(meta->width, spec->width, sizeof(meta->width));
	((PageHeader) page)->pd_lower =
		((char *) meta + sizeof(GrainMetaPageData)) - (char *) page;
	GenericXLogFinish(xlogstate);

	UnlockReleaseBuffer(buf);

	/* The cached OIDs and levels may be stale */
	if (index->rd_amcache)
	{
		pfree(index->rd_amcache);
		index->rd_amcache = NULL;
	}
}

static GrainMetaPageData
grain_read_meta(Relation index)
{
	Buffer		buf = ReadBuffer(index, GRAIN_METAPAGE_BLKNO);
	Page		page;
	GrainMetaPageData meta;
	bool		valid;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	valid = PageGetSpecialSize(page) ==
		MAXALIGN(sizeof(GrainPageOpaqueData)) &&
		GrainPageGetOpaque(page)->grain_page_id == GRAIN_PAGE_ID;
	meta = *GrainPageGetMeta(page);
	UnlockReleaseBuffer(buf);

	if (!valid ||
		meta.magic != GRAIN_META_MAGIC ||
		meta.version != GRAIN_CURRENT_VERSION ||
		meta.nlevels < 1 || meta.nlevels > GRAIN_MAX_LEVELS)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid grain index",
						RelationGetRelationName(index)),
				 errhint("Use REINDEX to rebuild it.")));
	return meta;
}

/*
 * The bucket levels and table OIDs of an index, from its metapage, cached
 * in rd_amcache until the next relcache invalidation.
 */
GrainCache *
grain_get_cache(Relation index)
{
	GrainCache *cache = (GrainCache *) index->rd_amcache;
	GrainMetaPageData meta;

	if (cache != NULL)
		return cache;

	meta = grain_read_meta(index);
	cache = MemoryContextAllocZero(index->rd_indexcxt, sizeof(GrainCache));
	cache->spec.nlevels = meta.nlevels;
	cache->spec.origin = meta.origin;
	memcpy(cache->spec.width, meta.width, sizeof(cache->spec.width));
	cache->spec.delay = grain_options_delay(index, meta.width[0]);
	cache->spec.nvalues = IndexRelationGetNumberOfAttributes(index) -
		IndexRelationGetNumberOfKeyAttributes(index);
	cache->summaryrelid = meta.summaryrelid;
	cache->summaryidxid = meta.summaryidxid;
	cache->markerrelid = meta.markerrelid;
	index->rd_amcache = cache;
	return cache;
}

TimestampTz
grain_read_watermark(Relation index)
{
	return grain_read_meta(index).watermark;
}

/*
 * Move the watermark.  Only summarization calls this, under a
 * self-conflicting lock, and only forward.
 */
void
grain_write_watermark(Relation index, TimestampTz watermark)
{
	Buffer		buf = ReadBuffer(index, GRAIN_METAPAGE_BLKNO);
	GenericXLogState *xlogstate;
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	xlogstate = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(xlogstate, buf, 0);
	Assert(watermark >= GrainPageGetMeta(page)->watermark);
	GrainPageGetMeta(page)->watermark = watermark;
	GenericXLogFinish(xlogstate);
	UnlockReleaseBuffer(buf);
}

void
grain_insert_marker(Relation markerrel, TimestampTz bucket)
{
	Datum		values[Natts_grain_marker];
	bool		nulls[Natts_grain_marker] = {false};
	HeapTuple	tuple;

	values[Anum_grain_marker_bucket - 1] = TimestampTzGetDatum(bucket);
	tuple = heap_form_tuple(RelationGetDescr(markerrel), values, nulls);
	simple_heap_insert(markerrel, tuple);
	heap_freetuple(tuple);
}

void
grain_agg_init(GrainAgg *agg, TimestampTz bucket)
{
	memset(agg, 0, sizeof(GrainAgg));
	agg->bucket = bucket;
}

/*
 * agg += other
 */
void
grain_agg_add(GrainAgg *agg, const GrainAgg *other, int nvalues)
{
	agg->n += other->n;
	for (int i = 0; i < nvalues; i++)
	{
		if (other->nn[i] == 0)
			continue;
		if (agg->nn[i] == 0)
		{
			agg->sum[i] = other->sum[i];
			agg->min[i] = other->min[i];
			agg->max[i] = other->max[i];
		}
		else
		{
			agg->sum[i] = DirectFunctionCall2(numeric_add, agg->sum[i],
											  other->sum[i]);
			agg->min[i] = Min(agg->min[i], other->min[i]);
			agg->max[i] = Max(agg->max[i], other->max[i]);
		}
		agg->nn[i] += other->nn[i];
	}
}

/* Storage of the array element types grain uses, without a syscache lookup */
static void
grain_elem_info(Oid elmtype, int16 *typlen, bool *typbyval, char *typalign)
{
	switch (elmtype)
	{
		case INT8OID:
			*typlen = sizeof(int64);
			*typbyval = FLOAT8PASSBYVAL;
			*typalign = TYPALIGN_DOUBLE;
			break;
		case NUMERICOID:
			*typlen = -1;
			*typbyval = false;
			*typalign = TYPALIGN_INT;
			break;
		default:
			elog(ERROR, "unexpected grain array element type %u", elmtype);
	}
}

static Datum
grain_make_array(const Datum *elems, const bool *nulls, int n, Oid elmtype)
{
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			dims[1] = {n};
	int			lbs[1] = {1};

	if (n == 0)
		return PointerGetDatum(construct_empty_array(elmtype));
	grain_elem_info(elmtype, &typlen, &typbyval, &typalign);
	return PointerGetDatum(construct_md_array((Datum *) elems, (bool *) nulls,
											  1, dims, lbs, elmtype,
											  typlen, typbyval, typalign));
}

/*
 * The nn, sum, min and max arrays of an aggregate, as the summary table
 * stores them and grain_buckets() returns them: sum, min and max are NULL
 * for a value column with no non-null values.
 */
void
grain_agg_arrays(const GrainAgg *agg, int nvalues, Datum *out)
{
	Datum		nn[GRAIN_MAX_VALUES];
	Datum		sum[GRAIN_MAX_VALUES];
	Datum		min[GRAIN_MAX_VALUES];
	Datum		max[GRAIN_MAX_VALUES];
	bool		empty[GRAIN_MAX_VALUES];
	bool		nonull[GRAIN_MAX_VALUES] = {false};

	for (int i = 0; i < nvalues; i++)
	{
		nn[i] = Int64GetDatum(agg->nn[i]);
		empty[i] = agg->nn[i] == 0;
		sum[i] = empty[i] ? (Datum) 0 : agg->sum[i];
		min[i] = Int64GetDatum(empty[i] ? 0 : agg->min[i]);
		max[i] = Int64GetDatum(empty[i] ? 0 : agg->max[i]);
	}
	out[0] = grain_make_array(nn, nonull, nvalues, INT8OID);
	out[1] = grain_make_array(sum, empty, nvalues, NUMERICOID);
	out[2] = grain_make_array(min, empty, nvalues, INT8OID);
	out[3] = grain_make_array(max, empty, nvalues, INT8OID);
}

HeapTuple
grain_agg_to_summary(TupleDesc tupdesc, int16 level, const GrainAgg *agg,
					 int nvalues)
{
	Datum		values[Natts_grain_summary];
	bool		nulls[Natts_grain_summary] = {false};

	values[Anum_grain_summary_level - 1] = Int16GetDatum(level);
	values[Anum_grain_summary_bucket - 1] = TimestampTzGetDatum(agg->bucket);
	values[Anum_grain_summary_n - 1] = Int64GetDatum(agg->n);
	/* nn, sum, min and max are the last four columns, in that order */
	grain_agg_arrays(agg, nvalues, &values[Anum_grain_summary_nn - 1]);

	return heap_form_tuple(tupdesc, values, nulls);
}

static Datum *
grain_read_array(Datum d, Oid elmtype, int nvalues)
{
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int16		typlen;
	bool		typbyval;
	char		typalign;

	grain_elem_info(elmtype, &typlen, &typbyval, &typalign);
	deconstruct_array(DatumGetArrayTypeP(d), elmtype, typlen, typbyval,
					  typalign, &elems, &nulls, &n);
	if (n != nvalues)
		elog(ERROR, "grain summary row has %d values, expected %d",
			 n, nvalues);
	return elems;
}

/*
 * The aggregate of a summary row.  sum, min and max are NULL exactly where
 * nn is 0, and grain_agg_init() leaves those at 0.
 */
void
grain_agg_from_summary(HeapTuple tuple, TupleDesc tupdesc, int nvalues,
					   GrainAgg *agg)
{
	Datum		values[Natts_grain_summary];
	bool		nulls[Natts_grain_summary];
	Datum	   *nn;
	Datum	   *sum;
	Datum	   *min;
	Datum	   *max;

	heap_deform_tuple(tuple, tupdesc, values, nulls);
	grain_agg_init(agg,
				   DatumGetTimestampTz(values[Anum_grain_summary_bucket - 1]));
	agg->n = DatumGetInt64(values[Anum_grain_summary_n - 1]);
	if (nvalues == 0)
		return;

	nn = grain_read_array(values[Anum_grain_summary_nn - 1], INT8OID, nvalues);
	sum = grain_read_array(values[Anum_grain_summary_sum - 1], NUMERICOID,
						   nvalues);
	min = grain_read_array(values[Anum_grain_summary_min - 1], INT8OID,
						   nvalues);
	max = grain_read_array(values[Anum_grain_summary_max - 1], INT8OID,
						   nvalues);
	for (int i = 0; i < nvalues; i++)
	{
		agg->nn[i] = DatumGetInt64(nn[i]);
		if (agg->nn[i] == 0)
			continue;
		/* The array is stored plain, so sum[i] points into the buffer */
		agg->sum[i] = datumCopy(sum[i], false, -1);
		agg->min[i] = DatumGetInt64(min[i]);
		agg->max[i] = DatumGetInt64(max[i]);
	}
}
