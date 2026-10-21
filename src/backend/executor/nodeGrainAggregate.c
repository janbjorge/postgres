/*-------------------------------------------------------------------------
 *
 * nodeGrainAggregate.c
 *	  Routines to compute GROUP BY date_bin(...) from a grain index.
 *
 * The planner uses a GrainAggregate node only when the index's buckets hold
 * exactly the rows the query groups (see add_grain_agg_paths()), so the
 * node reads the buckets of the window once and returns them, converting
 * each aggregate the index keeps to the type its Aggref returns.  HAVING is
 * the node's qual.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGrainAggregate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/grain.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeGrainAggregate.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

/*
 * Add one bucket to the tuplestore.  Called by grain_read() inside its SPI
 * connection; tuplestore_putvalues() copies the values out of it.
 */
static void
grain_agg_emit(void *arg, const GrainAgg *agg)
{
	GrainAggregateState *node = (GrainAggregateState *) arg;
	TupleDesc	tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	/* The date_bin() call comes first, then the Aggrefs */
	node->values[0] = TimestampTzGetDatum(agg->bucket);
	node->nulls[0] = false;
	for (int i = 1; i < tupdesc->natts; i++)
	{
		int			v = node->colvalue[i];
		Oid			type = TupleDescAttr(tupdesc, i)->atttypid;
		bool		empty = v >= 0 && agg->nn[v] == 0;
		int64		minmax;

		node->nulls[i] = false;
		switch ((GrainColumn) node->colkind[i])
		{
			case GRAIN_COLUMN_COUNT_STAR:
				node->values[i] = Int64GetDatum(agg->n);
				break;
			case GRAIN_COLUMN_COUNT:
				node->values[i] = Int64GetDatum(agg->nn[v]);
				break;
			case GRAIN_COLUMN_SUM:
				/* sum(int2) and sum(int4) return bigint, sum(int8) numeric */
				node->nulls[i] = empty;
				if (empty)
					break;
				if (type == NUMERICOID)
					node->values[i] = agg->sum[v];
				else
					node->values[i] = DirectFunctionCall1(numeric_int8,
														  agg->sum[v]);
				break;
			case GRAIN_COLUMN_MIN:
			case GRAIN_COLUMN_MAX:
				node->nulls[i] = empty;
				if (empty)
					break;
				minmax = node->colkind[i] == GRAIN_COLUMN_MIN ?
					agg->min[v] : agg->max[v];
				/* The value has the column's type, so it fits */
				if (type == INT2OID)
					node->values[i] = Int16GetDatum((int16) minmax);
				else if (type == INT4OID)
					node->values[i] = Int32GetDatum((int32) minmax);
				else
					node->values[i] = Int64GetDatum(minmax);
				break;
			case GRAIN_COLUMN_NONE:
				elog(ERROR, "unexpected grain column kind");
		}
	}
	tuplestore_putvalues(node->buckets, tupdesc, node->values, node->nulls);
}

/*
 * Evaluate one bound of the time key.  The reader takes [lower, upper), so
 * an exclusive lower or inclusive upper bound is bumped: time keys are whole
 * microseconds.  Returns false if the bound is NULL, which no row satisfies.
 */
static bool
grain_agg_bound(GrainAggregateState *node, ExprState *expr, bool bump,
				TimestampTz *result)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	bool		isnull;
	TimestampTz ts;

	ts = DatumGetTimestampTz(ExecEvalExprSwitchContext(expr, econtext,
													   &isnull));
	if (isnull)
		return false;

	/*
	 * grain keeps no bucket for an infinite time key, so a window reaching
	 * infinity can't be read.
	 */
	if (TIMESTAMP_NOT_FINITE(ts))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("grain index \"%s\" cannot read a window with an infinite bound",
						RelationGetRelationName(node->index)),
				 errhint("Set enable_grainagg to off to group the table's rows instead.")));

	if (bump)
		ts++;
	*result = ts;
	return true;
}

static void
grain_agg_read(GrainAggregateState *node)
{
	GrainAggregate *plan = (GrainAggregate *) node->ss.ps.plan;
	TimestampTz lower;
	TimestampTz upper;

	if (!grain_agg_bound(node, node->lower, !plan->lowerinclusive, &lower) ||
		!grain_agg_bound(node, node->upper, plan->upperinclusive, &upper))
		return;

	grain_read(node->ss.ss_currentRelation, node->index,
			   node->ss.ps.state->es_snapshot, plan->width, lower, upper,
			   grain_agg_emit, node);
}

static TupleTableSlot *
ExecGrainAggregate(PlanState *pstate)
{
	GrainAggregateState *node = castNode(GrainAggregateState, pstate);
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	CHECK_FOR_INTERRUPTS();

	if (!node->read)
	{
		grain_agg_read(node);
		node->read = true;
	}

	for (;;)
	{
		if (!tuplestore_gettupleslot(node->buckets, true, false, slot))
			return NULL;

		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;
		if (ExecQual(node->ss.ps.qual, econtext))
		{
			if (node->ss.ps.ps_ProjInfo == NULL)
				return slot;
			return ExecProject(node->ss.ps.ps_ProjInfo);
		}
		InstrCountFiltered1(node, 1);
	}
}

GrainAggregateState *
ExecInitGrainAggregate(GrainAggregate *node, EState *estate, int eflags)
{
	GrainAggregateState *state;
	TupleDesc	tupdesc;
	AttrNumber *valuecols;
	int			nvalues;
	int			natts;
	ListCell   *lc;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
	Assert(outerPlan(node) == NULL && innerPlan(node) == NULL);

	state = makeNode(GrainAggregateState);
	state->ss.ps.plan = (Plan *) node;
	state->ss.ps.state = estate;
	state->ss.ps.ExecProcNode = ExecGrainAggregate;

	ExecAssignExprContext(estate, &state->ss.ps);

	state->ss.ss_currentRelation = ExecOpenScanRelation(estate,
														node->heaprelid,
														eflags);
	state->index = index_open(node->indexid, AccessShareLock);

	tupdesc = ExecTypeFromTL(node->grain_scan_tlist);
	ExecInitScanTupleSlot(estate, &state->ss, tupdesc, &TTSOpsMinimalTuple, 0);
	ExecInitResultTypeTL(&state->ss.ps);
	ExecAssignScanProjectionInfoWithVarno(&state->ss, INDEX_VAR);

	state->ss.ps.qual = ExecInitQual(node->plan.qual, (PlanState *) state);
	state->lower = ExecInitExpr(node->lower, (PlanState *) state);
	state->upper = ExecInitExpr(node->upper, (PlanState *) state);

	/* The INCLUDE columns follow the time key */
	valuecols = &state->index->rd_index->indkey.values[1];
	nvalues = IndexRelationGetNumberOfAttributes(state->index) - 1;
	natts = list_length(node->grain_scan_tlist);
	state->colkind = palloc_array(int, natts);
	state->colvalue = palloc_array(int, natts);
	state->values = palloc_array(Datum, natts);
	state->nulls = palloc_array(bool, natts);
	for_each_from(lc, node->grain_scan_tlist, 1)
	{
		Aggref	   *aggref = castNode(Aggref,
									  ((TargetEntry *) lfirst(lc))->expr);
		int			i = foreach_current_index(lc);

		state->colkind[i] = grain_aggref_column(aggref, valuecols, nvalues,
												&state->colvalue[i]);
		if (state->colkind[i] == GRAIN_COLUMN_NONE)
			elog(ERROR, "aggregate not kept by grain index \"%s\"",
				 RelationGetRelationName(state->index));
	}

	state->buckets = tuplestore_begin_heap(false, false, work_mem);

	return state;
}

void
ExecEndGrainAggregate(GrainAggregateState *node)
{
	tuplestore_end(node->buckets);
	index_close(node->index, NoLock);
}

void
ExecReScanGrainAggregate(GrainAggregateState *node)
{
	/* Read again only if a parameter, as in the bounds, changed */
	if (node->ss.ps.chgParam == NULL)
		tuplestore_rescan(node->buckets);
	else
	{
		tuplestore_clear(node->buckets);
		node->read = false;
	}
}
