/*-------------------------------------------------------------------------
 *
 * nodeGrainAggregate.h
 *	  prototypes for nodeGrainAggregate.c
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeGrainAggregate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGRAINAGGREGATE_H
#define NODEGRAINAGGREGATE_H

#include "nodes/execnodes.h"

extern GrainAggregateState *ExecInitGrainAggregate(GrainAggregate *node,
												   EState *estate,
												   int eflags);
extern void ExecEndGrainAggregate(GrainAggregateState *node);
extern void ExecReScanGrainAggregate(GrainAggregateState *node);

#endif							/* NODEGRAINAGGREGATE_H */
