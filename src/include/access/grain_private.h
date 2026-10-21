/*-------------------------------------------------------------------------
 *
 * grain_private.h
 *	  Internal declarations for grain, the Grouped Range Aggregate INdex.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/access/grain_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GRAIN_PRIVATE_H
#define GRAIN_PRIVATE_H

#include "access/grain.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "datatype/timestamp.h"
#include "fmgr.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

/* Sizes of the fixed arrays in the metapage and the aggregates */
#define GRAIN_MAX_LEVELS	8

/*
 * The metapage, block 0, the only page of a grain index.  Its special space
 * identifies the page to tools such as pg_filedump, as bloom's does.
 */
#define GRAIN_METAPAGE_BLKNO	0
#define GRAIN_META_MAGIC		0x47524149	/* "GRAI" */
#define GRAIN_CURRENT_VERSION	1
#define GRAIN_PAGE_ID			0xFF84

typedef struct GrainPageOpaqueData
{
	uint16		flags;			/* none yet */
	uint16		grain_page_id;	/* for identification of grain indexes */
} GrainPageOpaqueData;

typedef GrainPageOpaqueData *GrainPageOpaque;

#define GrainPageGetOpaque(page) \
	((GrainPageOpaque) PageGetSpecialPointer(page))

/*
 * The bucket levels are fixed when the index is built: they are copied here
 * from the reloptions, so that ALTER INDEX ... SET (buckets = ...) cannot
 * change how existing summary rows are read.
 */
typedef struct GrainMetaPageData
{
	uint32		magic;
	uint32		version;
	TimestampTz watermark;		/* buckets before this are summarized or
								 * marked */
	Oid			summaryrelid;	/* table of summary rows */
	Oid			summaryidxid;	/* its btree on (level, bucket) */
	Oid			markerrelid;	/* table of markers */
	int32		nlevels;
	TimestampTz origin;
	int64		width[GRAIN_MAX_LEVELS];	/* microseconds, finest first */
} GrainMetaPageData;

#define GrainPageGetMeta(page) \
	((GrainMetaPageData *) PageGetContents(page))

/* Column numbers of the summary table */
#define Anum_grain_summary_level	1
#define Anum_grain_summary_bucket	2
#define Anum_grain_summary_n		3
#define Anum_grain_summary_nn		4
#define Anum_grain_summary_sum		5
#define Anum_grain_summary_min		6
#define Anum_grain_summary_max		7
#define Natts_grain_summary			7

/* Column numbers of the marker table */
#define Anum_grain_marker_bucket	1
#define Natts_grain_marker			1

/*
 * The bucket levels of an index, in microseconds.  ambuild fills only the
 * levels and origin; delay and nvalues are set for the cache.
 */
typedef struct GrainSpec
{
	int			nlevels;
	int64		width[GRAIN_MAX_LEVELS];	/* finest first */
	TimestampTz origin;
	int64		delay;
	int			nvalues;		/* INCLUDE columns */
} GrainSpec;

/* What a grain index keeps in rd_amcache */
typedef struct GrainCache
{
	GrainSpec	spec;
	Oid			summaryrelid;
	Oid			summaryidxid;
	Oid			markerrelid;
} GrainCache;

/* grain.c */
extern int64 grain_options_delay(Relation index, int64 finest);
extern int64 grain_interval_usecs(const Interval *itv, const char *what);
extern TimestampTz grain_bin(TimestampTz ts, int64 width, TimestampTz origin);

/* grain_state.c */
extern void grain_build_state(Relation index, const GrainSpec *spec);
extern GrainCache *grain_get_cache(Relation index);
extern void grain_write_watermark(Relation index, TimestampTz watermark);
extern void grain_insert_marker(Relation markerrel, TimestampTz bucket);
extern void grain_agg_init(GrainAgg *agg, TimestampTz bucket);
extern void grain_agg_add(GrainAgg *agg, const GrainAgg *other, int nvalues);
extern void grain_agg_from_summary(HeapTuple tuple, TupleDesc tupdesc,
								   int nvalues, GrainAgg *agg);
extern void grain_agg_arrays(const GrainAgg *agg, int nvalues, Datum *out);
extern HeapTuple grain_agg_to_summary(TupleDesc tupdesc, int16 level,
									  const GrainAgg *agg, int nvalues);

#endif							/* GRAIN_PRIVATE_H */
