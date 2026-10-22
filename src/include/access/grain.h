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

extern void grain_validate_buckets(const char *value);
extern void grain_validate_origin(const char *value);
extern void grain_validate_delay(const char *value);

#endif							/* GRAIN_H */
