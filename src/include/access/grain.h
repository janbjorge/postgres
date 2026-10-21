/*-------------------------------------------------------------------------
 *
 * grain.h
 *	  Public header for grain, the Grouped Range Aggregate INdex.
 *
 * A grain index keeps exact count, sum, min and max per time bucket of a
 * table, for buckets that have been summarized.  It answers no index scans.
 * The access method's internals are in grain_private.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/access/grain.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GRAIN_H
#define GRAIN_H

#include "access/attnum.h"
#include "datatype/timestamp.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

struct Aggref;

/* The most INCLUDE columns, the values each bucket aggregates */
#define GRAIN_MAX_VALUES	8

/*
 * Storage type for grain's reloptions.  "buckets" and "origin" are read
 * only by ambuild, which copies them into the metapage, so changing them
 * takes effect at the next REINDEX, as BRIN's pages_per_range does.
 */
typedef struct GrainOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			bucketsOffset;	/* "buckets": widths, finest first */
	int			originOffset;	/* "origin": where buckets start */
	int			delayOffset;	/* "delay": wait before summarizing */
} GrainOptions;

/*
 * Aggregates of one bucket.  sum is a numeric Datum, or 0 when no non-null
 * value has been added.
 */
typedef struct GrainAgg
{
	TimestampTz bucket;
	int64		n;				/* count(*) */
	int64		nn[GRAIN_MAX_VALUES];	/* count(value) */
	Datum		sum[GRAIN_MAX_VALUES];
	int64		min[GRAIN_MAX_VALUES];
	int64		max[GRAIN_MAX_VALUES];
} GrainAgg;

/* The aggregates a grain index keeps, as grain_aggref_column() names them */
typedef enum GrainColumn
{
	GRAIN_COLUMN_NONE,			/* not one grain keeps */
	GRAIN_COLUMN_BUCKET,		/* the date_bin() call, not an aggregate */
	GRAIN_COLUMN_COUNT_STAR,
	GRAIN_COLUMN_COUNT,
	GRAIN_COLUMN_SUM,
	GRAIN_COLUMN_MIN,
	GRAIN_COLUMN_MAX,
} GrainColumn;

typedef void (*GrainReadCallback) (void *arg, const GrainAgg *agg);

/* grain.c */
extern void grain_validate_buckets(const char *value);
extern void grain_validate_origin(const char *value);
extern void grain_validate_delay(const char *value);
extern bool grain_index_width(Relation index, const Interval *width,
							  TimestampTz origin, int64 *usecs);
extern GrainColumn grain_aggref_column(const struct Aggref *aggref,
									   const AttrNumber *valuecols,
									   int nvalues, int *value);

/* grain_state.c */
extern TimestampTz grain_read_watermark(Relation index);

/* grain_summarize.c */
extern void grain_read(Relation heap, Relation index, Snapshot snapshot,
					   int64 width, TimestampTz lower, TimestampTz upper,
					   GrainReadCallback emit, void *arg);

#endif							/* GRAIN_H */
