/*-------------------------------------------------------------------------
 *
 * plangrain.c
 *	  Planning of GROUP BY date_bin(...) queries that a grain index answers.
 *
 * A grain index keeps count(*), and count, sum, min and max of each INCLUDE
 * column, per time bucket.  A query like
 *		SELECT date_bin('1 day', key, origin), count(*), sum(col)
 *		FROM tab
 *		WHERE key >= lower AND key < upper AND <the index's predicate>
 *		GROUP BY 1
 * can read those buckets instead of grouping the rows.  The answer must be
 * exactly what grouping the rows gives, so we accept only queries whose
 * groups are the index's buckets, whose aggregates are ones the index keeps,
 * and whose rows are exactly the rows the index summarizes in the window.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/plangrain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/grain.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

typedef struct GrainTargetContext
{
	Expr	   *groupexpr;
	const AttrNumber *valuecols;
	int			nvalues;
	List	   *aggrefs;
} GrainTargetContext;

/*
 * Check that an expression of the grouped target or HAVING uses the table's
 * rows only through the date_bin() call and aggregates the index keeps, and
 * collect those Aggrefs.  Returns true if it uses them otherwise.
 */
static bool
grain_target_walker(Node *node, GrainTargetContext *context)
{
	int			value;

	if (node == NULL)
		return false;
	if (equal(node, context->groupexpr))
		return false;
	if (IsA(node, Aggref))
	{
		if (grain_aggref_column((Aggref *) node, context->valuecols,
								context->nvalues, &value) == GRAIN_COLUMN_NONE)
			return true;
		context->aggrefs = list_append_unique(context->aggrefs, node);
		return false;
	}
	if (IsA(node, Var) || IsA(node, PlaceHolderVar) ||
		IsA(node, SubLink) || IsA(node, SubPlan))
		return true;
	return expression_tree_walker(node, grain_target_walker, context);
}

/*
 * If clause compares the time key with an expression the executor can
 * evaluate before reading, return that expression, and whether it is a
 * lower bound and inclusive.
 */
static bool
grain_key_bound(Expr *clause, Var *key, Oid opfamily, Expr **bound,
				bool *lower, bool *inclusive)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Oid			opno;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;

	if (!is_opclause(clause) || list_length(((OpExpr *) clause)->args) != 2)
		return false;
	op = (OpExpr *) clause;
	opno = op->opno;
	left = linitial(op->args);
	right = lsecond(op->args);
	if (equal(right, key))
	{
		Node	   *tmp = left;

		left = right;
		right = tmp;
		opno = get_commutator(opno);
	}
	if (!equal(left, key) || !OidIsValid(opno) ||
		!op_in_opfamily(opno, opfamily) ||
		exprType(right) != TIMESTAMPTZOID ||
		contain_var_clause(right) || contain_volatile_functions(right) ||
		contain_subplans(right))
		return false;

	get_op_opfamily_properties(opno, opfamily, false, &strategy,
							   &lefttype, &righttype);
	if (lefttype != TIMESTAMPTZOID || righttype != TIMESTAMPTZOID)
		return false;
	switch (strategy)
	{
		case BTLessStrategyNumber:
			*lower = false;
			*inclusive = false;
			break;
		case BTLessEqualStrategyNumber:
			*lower = false;
			*inclusive = true;
			break;
		case BTGreaterEqualStrategyNumber:
			*lower = true;
			*inclusive = true;
			break;
		case BTGreaterStrategyNumber:
			*lower = true;
			*inclusive = false;
			break;
		default:
			return false;
	}
	*bound = (Expr *) right;
	return true;
}

/*
 * The fraction of [lower, upper) after the watermark, which no summary
 * covers.  When the bounds aren't known when planning, the whole window is
 * counted as covered.
 */
static double
grain_tail_fraction(PlannerInfo *root, Expr *lower, Expr *upper,
					TimestampTz watermark, int64 width)
{
	Node	   *lo = estimate_expression_value(root, (Node *) lower);
	Node	   *hi = estimate_expression_value(root, (Node *) upper);
	TimestampTz l;
	TimestampTz h;

	if (!IsA(lo, Const) || ((Const *) lo)->constisnull ||
		!IsA(hi, Const) || ((Const *) hi)->constisnull)
		return 0.0;
	l = DatumGetTimestampTz(((Const *) lo)->constvalue);
	h = DatumGetTimestampTz(((Const *) hi)->constvalue);
	if (TIMESTAMP_NOT_FINITE(l) || TIMESTAMP_NOT_FINITE(h) || l >= h)
		return 0.0;
	if (watermark <= l)
		return 1.0;
	if (watermark >= h)
		return 0.0;
	return (double) (h - watermark) / (double) (h - l);
}

/*
 * add_grain_agg_paths
 *		Add a path computing the grouping from a grain index, if the query
 *		is one a grain index answers exactly.
 *
 * That is: one plain table; GROUP BY date_bin(width, key, origin) alone,
 * with the width one of the index's levels and the origin on one of its
 * bucket boundaries; only aggregates the index keeps, of its INCLUDE
 * columns; and a WHERE clause that bounds key on both sides and is
 * otherwise equivalent to the index's predicate, so that the index
 * summarizes exactly the rows the query groups.
 */
void
add_grain_agg_paths(PlannerInfo *root, RelOptInfo *input_rel,
					RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	RangeTblEntry *rte;
	FuncExpr   *groupexpr;
	Const	   *width;
	Var		   *key;
	Const	   *origin;
	Expr	   *lower = NULL;
	bool		lowerinclusive = false;
	Expr	   *upper = NULL;
	bool		upperinclusive = false;
	List	   *otherquals = NIL;
	Oid			opfamily;
	ListCell   *lc;

	/* One plain table, grouped by a single date_bin() call */
	if (input_rel->reloptkind != RELOPT_BASEREL ||
		input_rel->rtekind != RTE_RELATION ||
		IS_DUMMY_REL(input_rel) ||
		parse->groupingSets != NIL ||
		list_length(parse->groupClause) != 1)
		return;
	rte = planner_rt_fetch(input_rel->relid, root);
	if (rte->inh || rte->relkind != RELKIND_RELATION ||
		rte->tablesample != NULL)
		return;

	groupexpr = (FuncExpr *)
		get_sortgroupclause_expr(linitial(parse->groupClause),
								 root->processed_tlist);
	if (!IsA(groupexpr, FuncExpr) ||
		groupexpr->funcid != F_DATE_BIN_INTERVAL_TIMESTAMPTZ_TIMESTAMPTZ)
		return;
	width = (Const *) linitial(groupexpr->args);
	key = (Var *) lsecond(groupexpr->args);
	origin = (Const *) lthird(groupexpr->args);
	if (!IsA(width, Const) || width->constisnull ||
		!IsA(key, Var) || key->varno != input_rel->relid ||
		key->varlevelsup != 0 ||
		!IsA(origin, Const) || origin->constisnull)
		return;

	/* WHERE: one bound on each side of the key, and the rest */
	opfamily = lookup_type_cache(TIMESTAMPTZOID,
								 TYPECACHE_BTREE_OPFAMILY)->btree_opf;
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Expr	   *bound;
		bool		islower;
		bool		inclusive;

		/* Gating and security barrier quals would be lost */
		if (rinfo->pseudoconstant || rinfo->security_level > 0)
			return;
		if (!grain_key_bound(rinfo->clause, key, opfamily, &bound,
							 &islower, &inclusive))
			otherquals = lappend(otherquals, rinfo->clause);
		else if (islower && lower == NULL)
		{
			lower = bound;
			lowerinclusive = inclusive;
		}
		else if (!islower && upper == NULL)
		{
			upper = bound;
			upperinclusive = inclusive;
		}
		else
			return;
	}
	if (lower == NULL || upper == NULL)
		return;

	foreach(lc, input_rel->indexlist)
	{
		IndexOptInfo *index = lfirst_node(IndexOptInfo, lc);
		AttrNumber	valuecols[GRAIN_MAX_VALUES];
		int			nvalues = index->ncolumns - 1;
		GrainTargetContext context;
		Relation	irel;
		bool		ok;
		int64		usecs;
		TimestampTz watermark;
		double		numGroups;

		if (index->relam != GRAIN_AM_OID ||
			index->indexkeys[0] != key->varattno)
			continue;

		/* The index summarizes exactly the rows the WHERE clause keeps */
		if (!predicate_implied_by(index->indpred, otherquals, false) ||
			!predicate_implied_by(otherquals, index->indpred, false))
			continue;

		Assert(nvalues <= GRAIN_MAX_VALUES);
		for (int i = 0; i < nvalues; i++)
			valuecols[i] = index->indexkeys[i + 1];
		context.groupexpr = (Expr *) groupexpr;
		context.valuecols = valuecols;
		context.nvalues = nvalues;
		context.aggrefs = NIL;
		if (grain_target_walker((Node *) grouped_rel->reltarget->exprs,
								&context) ||
			grain_target_walker(parse->havingQual, &context))
			continue;

		/* The index is locked since get_relation_info() */
		irel = index_open(index->indexoid, NoLock);
		ok = grain_index_width(irel, DatumGetIntervalP(width->constvalue),
							   DatumGetTimestampTz(origin->constvalue),
							   &usecs);
		watermark = grain_read_watermark(irel);
		index_close(irel, NoLock);
		if (!ok)
			continue;

		numGroups = estimate_num_groups(root,
										list_make1(groupexpr),
										input_rel->rows,
										NULL, NULL);
		add_path(grouped_rel, (Path *)
				 create_grainagg_path(root, grouped_rel, input_rel,
									  index->indexoid, usecs,
									  lower, lowerinclusive,
									  upper, upperinclusive,
									  (Expr *) groupexpr, context.aggrefs,
									  (List *) parse->havingQual, numGroups,
									  grain_tail_fraction(root, lower, upper,
														  watermark, usecs)));
	}
}
