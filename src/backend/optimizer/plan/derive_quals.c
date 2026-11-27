/*-------------------------------------------------------------------------
 *
 * derive_quals.c
 *	  Derive indexable predicates implied by functional quals.
 *
 * A qual like
 *
 *		date(created_at) = '2017-06-15'
 *		CASE WHEN status = 'x' THEN 1 ELSE 0 END = 1
 *		abs(v) = 7
 *
 * cannot use an index on created_at/status/v, because the indexed column is
 * inside an expression.  When the expression is immutable and its preimage of
 * the constant is an interval or a small finite set, we can add an equivalent
 * (implied) predicate on the column:
 *
 *		created_at >= '2017-06-15' AND created_at < '2017-06-16'
 *		status = 'x'
 *		v = 7 OR v = -7
 *
 * The original qual is kept, so the derived predicate only adds an implied
 * condition and cannot change the result.  The derived predicate is a normal
 * qual: the existing path/cost code decides whether the index is worth using.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/derive_quals.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "common/int.h"
#include "datatype/timestamp.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "parser/parse_func.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/timestamp.h"

/* GUC: allow turning the derivation off for A/B runs. */
bool		enable_functional_quals = true;

/* fmgr functions not declared in headers */
extern Datum numeric_eq(PG_FUNCTION_ARGS);
extern Datum numeric_cmp(PG_FUNCTION_ARGS);
extern Datum numeric_uminus(PG_FUNCTION_ARGS);
extern Datum timestamp_in(PG_FUNCTION_ARGS);
extern Datum timestamp_pl_interval(PG_FUNCTION_ARGS);

/* Cached function OIDs, initialized on first use. */
static bool func_oids_ready = false;
static Oid	oid_date_ts;
static Oid	oid_date_trunc_ts;
static Oid	oid_extract_ts;
static Oid	oid_date_part_ts;
static Oid	oid_abs_int2;
static Oid	oid_abs_int4;
static Oid	oid_abs_int8;
static Oid	oid_abs_numeric;
static Oid	oid_sign_int2;
static Oid	oid_sign_int4;
static Oid	oid_sign_int8;
static Oid	oid_sign_numeric;
static Oid	oid_sign_float4;
static Oid	oid_sign_float8;
static Oid	oid_floor_numeric;
static Oid	oid_floor_float8;
static Oid	oid_ceil_numeric;
static Oid	oid_ceil_float8;
static Oid	oid_trunc_numeric;
static Oid	oid_trunc_numeric2;
static Oid	oid_trunc_float8;

static void init_func_oids(void);
static List *derive_from_clause(PlannerInfo *root, Node *clause);
static void append_derived_quals(PlannerInfo *root, List **args, List *derived);
static bool derived_quals_are_indexable(PlannerInfo *root, List *derived);
static bool var_has_leading_index(PlannerInfo *root, Var *var);
static List *derive_case(PlannerInfo *root, CaseExpr *ce, Const *c,
						 Oid inputcollid);
static List *derive_coalesce(PlannerInfo *root, CoalesceExpr *ce, Const *c,
							 Oid inputcollid);
static List *derive_nullif(PlannerInfo *root, NullIfExpr *ne, Const *c,
						   Oid inputcollid);
static List *derive_func(PlannerInfo *root, FuncExpr *f, Const *c,
						 Oid inputcollid);
static List *derive_arith(PlannerInfo *root, OpExpr *op, Const *c,
						  Oid inputcollid);
static bool simple_var_eq(Node *cond, Var **var, Const **cval, Oid *collid);
static Const *fold_const(Node *node);
static bool const_equal(Const *a, Const *b, Oid collid);
static bool var_is_simple(Node *node, Var **var);
static Expr *make_var_eq(Var *var, Const *c, Oid inputcollid);
static Expr *make_var_op_const(Var *var, const char *opname, Const *c,
							   Oid inputcollid);
static Expr *make_var_is_null(Var *var);
static Expr *make_bool_const(bool value);
static Expr *make_range(Var *var, const char *lop, Const *lo,
						const char *hip, Const *hi, Oid inputcollid);
static Oid	find_op(const char *name, Oid left, Oid right);
static bool type_is_int(Oid type);
static Const *make_int_const(Oid type, int64 value);
static Const *make_numeric_const(Numeric value);
static Const *make_float8_const(double value);
static Const *make_ts_const(Timestamp value);
static Const *make_zero_const(Oid type);
static bool compute_add(Oid type, Datum a, Datum b, Datum *result);
static bool compute_sub(Oid type, Datum a, Datum b, Datum *result);
static Timestamp ts_from_ymd(int year, int month, int day);
static bool derive_date_bounds(Const *c, Const **lo, Const **hi);
static bool derive_trunc_bounds(const char *unit, Const *c,
								Const **lo, Const **hi);
static bool derive_year_bounds(int year, Const **lo, Const **hi);
static bool interval_unit(const char *unit, Interval *iv);
static bool year_from_const(Const *c, int *year);
static bool derive_step_bounds(Oid funcid, Const *c, Const **lo, Const **hi,
							   const char **lop, const char **hip);
static bool sign_const_kind(Const *c, int *kind);

/*
 * derive_functional_quals
 *		Add indexable predicates implied by functional equality quals.
 *
 * Called with the top-level WHERE expression, after preprocessing and before
 * deconstruct_jointree() distributes quals to the relations.
 */
Node *
derive_functional_quals(PlannerInfo *root, Node *quals)
{
	List	   *args = NIL;
	ListCell   *lc;
	bool		changed = false;

	if (!enable_functional_quals || quals == NULL)
		return quals;

	init_func_oids();

	/*
	 * The top-level jointree qual is an implicit-AND list of clauses.
	 * deconstruct_jointree() expects that representation and rejects a
	 * nested AND clause, so flatten the derived conjunctions into it.
	 */
	if (!IsA(quals, List))
		return quals;

	foreach(lc, (List *) quals)
	{
		Node	   *arg = (Node *) lfirst(lc);
		List	   *derived = derive_from_clause(root, arg);

		args = lappend(args, arg);
		if (derived != NIL)
		{
			append_derived_quals(root, &args, derived);
			changed = true;
		}
	}

	return changed ? (Node *) args : quals;
}

/*
 * append_derived_quals
 *		Append derived quals, flattening top-level AND clauses.
 */
static void
append_derived_quals(PlannerInfo *root, List **args, List *derived)
{
	ListCell   *lc;

	foreach(lc, derived)
	{
		Node	   *q = (Node *) lfirst(lc);

		if (IsA(q, BoolExpr) && ((BoolExpr *) q)->boolop == AND_EXPR)
			append_derived_quals(root, args, ((BoolExpr *) q)->args);
		else
		{
			*args = lappend(*args, q);
			root->derived_quals = lappend(root->derived_quals, q);
		}
	}
}

/*
 * derive_from_clause
 *		Try to derive implied predicates from one top-level conjunct.
 */
static List *
derive_from_clause(PlannerInfo *root, Node *clause)
{
	OpExpr	   *op;
	Node	   *left,
			   *right,
			   *expr;
	Const	   *c;
	Oid			inputcollid;
	List	   *derived = NIL;

	if (!IsA(clause, OpExpr))
		return NIL;
	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return NIL;
	if (strcmp(get_opname(op->opno), "=") != 0)
		return NIL;

	left = (Node *) linitial(op->args);
	right = (Node *) lsecond(op->args);
	inputcollid = op->inputcollid;

	/*
	 * One side must be the expression; the other must fold to a non-null
	 * constant.  Parameters are not handled: the branch/value comparisons
	 * below need a value known at plan time.
	 */
	c = fold_const(right);
	if (c != NULL)
		expr = left;
	else
	{
		c = fold_const(left);
		if (c == NULL)
			return NIL;
		expr = right;
	}

	if (IsA(expr, CaseExpr))
		derived = derive_case(root, (CaseExpr *) expr, c, inputcollid);
	else if (IsA(expr, CoalesceExpr))
		derived = derive_coalesce(root, (CoalesceExpr *) expr, c, inputcollid);
	else if (IsA(expr, NullIfExpr))
		derived = derive_nullif(root, (NullIfExpr *) expr, c, inputcollid);
	else if (IsA(expr, FuncExpr))
		derived = derive_func(root, (FuncExpr *) expr, c, inputcollid);
	else if (IsA(expr, OpExpr))
		derived = derive_arith(root, (OpExpr *) expr, c, inputcollid);

	/*
	 * Only add the predicate if every column it constrains has a leading
	 * index column: otherwise it would just be an extra filter on a scan that
	 * cannot use it, which is the one thing this pass must not do.
	 */
	if (derived != NIL && !derived_quals_are_indexable(root, derived))
		return NIL;
	return derived;
}

/*
 * derived_quals_are_indexable
 *		True if every column constrained by the derived predicates has an
 *		index that can use it (leading key column).
 */
static bool
derived_quals_are_indexable(PlannerInfo *root, List *derived)
{
	ListCell   *lc;

	foreach(lc, derived)
	{
		List	   *vars;
		ListCell   *lc2;

		vars = pull_var_clause((Node *) lfirst(lc), 0);
		foreach(lc2, vars)
		{
			Node	   *node = (Node *) lfirst(lc2);

			if (!IsA(node, Var) || !var_has_leading_index(root, (Var *) node))
				return false;
		}
	}
	return true;
}

static bool
var_has_leading_index(PlannerInfo *root, Var *var)
{
	RangeTblEntry *rte;
	Relation	rel;
	List	   *indexoids;
	ListCell   *lc;
	bool		found = false;

	if (var->varlevelsup != 0 || var->varno <= 0 || var->varattno <= 0)
		return false;
	rte = rt_fetch(var->varno, root->parse->rtable);
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return false;

	rel = table_open(rte->relid, NoLock);
	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Relation	idxrel = index_open(lfirst_oid(lc), AccessShareLock);

		if (IndexRelationGetNumberOfKeyAttributes(idxrel) >= 1 &&
			idxrel->rd_index->indkey.values[0] == var->varattno)
			found = true;
		index_close(idxrel, AccessShareLock);
		if (found)
			break;
	}
	table_close(rel, NoLock);
	return found;
}

/*
 * derive_case
 *		Turn  CASE WHEN col = k THEN v ... END = c  into  col = k ...
 *
 * Only the common shape is handled: every WHEN condition is a simple
 * equality between the same column and a distinct constant, and every branch
 * result folds to a constant.  Then the branches whose value equals c are
 * exactly the constants that can produce c, and the first-match order is
 * irrelevant because the conditions are mutually exclusive.
 */
static List *
derive_case(PlannerInfo *root, CaseExpr *ce, Const *c, Oid inputcollid)
{
	Var		   *casevar = NULL;
	Oid			caseconsttype = InvalidOid;
	Oid			casecollid = InvalidOid;
	List	   *seen = NIL;
	List	   *matches = NIL;
	ListCell   *lc;
	Const	   *deffolded;

	if (exprType((Node *) ce) != c->consttype)
		return NIL;

	foreach(lc, ce->args)
	{
		CaseWhen   *w = lfirst_node(CaseWhen, lc);
		Const	   *rc;
		Var		   *v;
		Const	   *k;
		Oid			collid;
		bool		dup = false;
		ListCell   *lc2;

		rc = fold_const((Node *) w->result);
		if (rc == NULL && !IsA(w->result, Const))
			return NIL;			/* cannot know the result value */

		if (!simple_var_eq((Node *) w->expr, &v, &k, &collid))
			return NIL;
		if (casevar == NULL)
		{
			casevar = v;
			caseconsttype = k->consttype;
			casecollid = collid;
		}
		else if (!equal(casevar, v) ||
				 k->consttype != caseconsttype ||
				 collid != casecollid)
			return NIL;

		foreach(lc2, seen)
		{
			if (const_equal(k, (Const *) lfirst(lc2), collid))
			{
				dup = true;
				break;
			}
		}
		if (!dup)
			seen = lappend(seen, k);

		/* NULL result never equals c */
		if (rc != NULL && !dup && const_equal(rc, c, inputcollid))
			matches = lappend(matches, copyObject(w->expr));
	}

	if (ce->defresult != NULL)
	{
		deffolded = fold_const((Node *) ce->defresult);
		if (deffolded == NULL && !IsA(ce->defresult, Const))
			return NIL;
		if (deffolded != NULL &&
			const_equal(deffolded, c, inputcollid))
		{
			/*
			 * The ELSE branch produces c; its condition is the complement of
			 * all WHEN conditions, not an indexable predicate here.
			 */
			return NIL;
		}
	}

	if (matches == NIL)
		return list_make1(make_bool_const(false));
	if (list_length(matches) == 1)
		return list_make1(linitial(matches));
	return list_make1((Node *) makeBoolExpr(OR_EXPR, matches, -1));
}

/*
 * derive_coalesce
 *		COALESCE(a, b) = c  ->  predicates on a and/or b.
 */
static List *
derive_coalesce(PlannerInfo *root, CoalesceExpr *ce, Const *c,
				Oid inputcollid)
{
	Node	   *a1;
	Node	   *a2;
	Var		   *v1;
	Var		   *v2;
	Const	   *d;
	Expr	   *e1;
	Expr	   *e2;

	if (list_length(ce->args) != 2)
		return NIL;
	a1 = (Node *) linitial(ce->args);
	a2 = (Node *) lsecond(ce->args);

	if (var_is_simple(a1, &v1) && IsA(a2, Const))
	{
		d = (Const *) a2;
		if (d->consttype != v1->vartype || c->consttype != v1->vartype)
			return NIL;
		e1 = make_var_eq(v1, c, inputcollid);
		if (e1 == NULL)
			return NIL;
		if (d->constisnull || !const_equal(d, c, inputcollid))
			return list_make1((Node *) e1);
		return list_make1((Node *) makeBoolExpr(OR_EXPR,
												list_make2(e1,
														   make_var_is_null(v1)),
												-1));
	}

	if (IsA(a1, Const) && var_is_simple(a2, &v2))
	{
		d = (Const *) a1;
		if (d->consttype != v2->vartype || c->consttype != v2->vartype)
			return NIL;
		if (d->constisnull)
		{
			e2 = make_var_eq(v2, c, inputcollid);
			return e2 ? list_make1((Node *) e2) : NIL;
		}
		if (const_equal(d, c, inputcollid))
			return NIL;			/* always true; nothing to gain */
		return list_make1(make_bool_const(false));
	}

	if (var_is_simple(a1, &v1) && var_is_simple(a2, &v2))
	{
		e1 = make_var_eq(v1, c, inputcollid);
		e2 = make_var_eq(v2, c, inputcollid);
		if (e1 == NULL || e2 == NULL)
			return NIL;
		return list_make1((Node *) makeBoolExpr(OR_EXPR,
												list_make2(e1,
														   makeBoolExpr(AND_EXPR,
																		list_make2(make_var_is_null(v1),
																				   e2),
																		-1)),
												-1));
	}

	return NIL;
}

/*
 * derive_nullif
 *		NULLIF(col, k) = c  ->  col = c  (or false when c = k).
 */
static List *
derive_nullif(PlannerInfo *root, NullIfExpr *ne, Const *c, Oid inputcollid)
{
	Var		   *v;
	Const	   *d;
	Expr	   *e;

	if (list_length(ne->args) != 2)
		return NIL;
	if (!var_is_simple((Node *) linitial(ne->args), &v))
		return NIL;
	if (!IsA(lsecond(ne->args), Const))
		return NIL;
	d = (Const *) lsecond(ne->args);
	if (d->consttype != v->vartype || c->consttype != v->vartype)
		return NIL;
	if (d->constisnull)
	{
		e = make_var_eq(v, c, inputcollid);
		return e ? list_make1((Node *) e) : NIL;
	}
	if (const_equal(d, c, inputcollid))
		return list_make1(make_bool_const(false));
	e = make_var_eq(v, c, inputcollid);
	return e ? list_make1((Node *) e) : NIL;
}

/*
 * derive_func
 *		Derive predicates for the supported immutable function shapes.
 */
static List *
derive_func(PlannerInfo *root, FuncExpr *f, Const *c, Oid inputcollid)
{
	Node	   *arg1;
	Var		   *var;

	if (list_length(f->args) < 1)
		return NIL;
	arg1 = (Node *) linitial(f->args);

	/* date(timestamp) = date */
	if (f->funcid == oid_date_ts && list_length(f->args) == 1 &&
		var_is_simple(arg1, &var) && var->vartype == TIMESTAMPOID &&
		c->consttype == DATEOID && f->funcresulttype == DATEOID)
	{
		Const	   *lo;
		Const	   *hi;
		Expr	   *r;

		if (!derive_date_bounds(c, &lo, &hi))
			return NIL;
		r = make_range(var, ">=", lo, "<", hi, InvalidOid);
		return r ? list_make1((Node *) r) : NIL;
	}

	/* date_trunc('day'|'month'|'year', timestamp) = timestamp */
	if (f->funcid == oid_date_trunc_ts && list_length(f->args) == 2 &&
		IsA(linitial(f->args), Const) &&
		var_is_simple(lsecond(f->args), &var) &&
		var->vartype == TIMESTAMPOID && c->consttype == TIMESTAMPOID)
	{
		Const	   *uc = (Const *) linitial(f->args);
		Const	   *lo;
		Const	   *hi;
		Expr	   *r;

		if (uc->consttype != TEXTOID || uc->constisnull)
			return NIL;
		if (!derive_trunc_bounds(TextDatumGetCString(uc->constvalue), c,
								 &lo, &hi))
			return NIL;
		r = make_range(var, ">=", lo, "<", hi, InvalidOid);
		return r ? list_make1((Node *) r) : NIL;
	}

	/* extract('year', timestamp) = constant */
	if ((f->funcid == oid_extract_ts || f->funcid == oid_date_part_ts) &&
		list_length(f->args) == 2 &&
		IsA(linitial(f->args), Const) &&
		var_is_simple(lsecond(f->args), &var) &&
		var->vartype == TIMESTAMPOID)
	{
		Const	   *fc = (Const *) linitial(f->args);
		int			year;
		Const	   *lo;
		Const	   *hi;
		Expr	   *r;

		if (fc->consttype != TEXTOID || fc->constisnull)
			return NIL;
		if (strcmp(TextDatumGetCString(fc->constvalue), "year") != 0)
			return NIL;
		if (!year_from_const(c, &year))
			return NIL;
		if (!derive_year_bounds(year, &lo, &hi))
			return NIL;
		r = make_range(var, ">=", lo, "<", hi, InvalidOid);
		return r ? list_make1((Node *) r) : NIL;
	}

	/* trunc(numeric, 0) is how trunc(numeric) is deparsed */
	if (f->funcid == oid_trunc_numeric2 && list_length(f->args) == 2 &&
		var_is_simple(arg1, &var) && var->vartype == NUMERICOID &&
		c->consttype == NUMERICOID && IsA(lsecond(f->args), Const))
	{
		Const	   *dc = (Const *) lsecond(f->args);
		Const	   *lo;
		Const	   *hi;
		const char *lop;
		const char *hip;
		Expr	   *r;

		if (dc->consttype != INT4OID || dc->constisnull ||
			DatumGetInt32(dc->constvalue) != 0)
			return NIL;
		if (!derive_step_bounds(oid_trunc_numeric, c, &lo, &hi, &lop, &hip))
			return NIL;
		r = make_range(var, lop, lo, hip, hi, inputcollid);
		return r ? list_make1((Node *) r) : NIL;
	}

	/* abs(x) = c */
	if (list_length(f->args) == 1 && var_is_simple(arg1, &var) &&
		c->consttype == f->funcresulttype && c->consttype == var->vartype)
	{
		Expr	   *e = NULL;

		if (f->funcid == oid_abs_int2 || f->funcid == oid_abs_int4 ||
			f->funcid == oid_abs_int8)
		{
			int64		v;
			int64		negv;

			if (c->consttype == INT2OID)
				v = DatumGetInt16(c->constvalue);
			else if (c->consttype == INT4OID)
				v = DatumGetInt32(c->constvalue);
			else
				v = DatumGetInt64(c->constvalue);
			if (v < 0)
				return list_make1(make_bool_const(false));
			if (v == 0)
			{
				Expr	   *z = make_var_eq(var, c, inputcollid);

				return z ? list_make1((Node *) z) : NIL;
			}
			negv = -v;
			if ((c->consttype == INT2OID && negv > PG_INT16_MAX) ||
				(c->consttype == INT4OID && negv > PG_INT32_MAX))
				return NIL;
			e = make_var_eq(var, c, inputcollid);
			if (e == NULL)
				return NIL;
			{
				Const	   *nc = make_int_const(c->consttype, negv);
				Expr	   *e2 = make_var_eq(var, nc, inputcollid);

				if (e2 == NULL)
					return NIL;
				e = (Expr *) makeBoolExpr(OR_EXPR, list_make2(e, e2), -1);
			}
			return list_make1((Node *) e);
		}

		if (f->funcid == oid_abs_numeric)
		{
			Numeric		n = DatumGetNumeric(c->constvalue);
			Numeric		zero = int64_to_numeric(0);
			int32		cmp;

			if (numeric_is_nan(n) || numeric_is_inf(n))
				return NIL;
			cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp,
													NumericGetDatum(n),
													NumericGetDatum(zero)));
			if (cmp < 0)
				return list_make1(make_bool_const(false));
			e = make_var_eq(var, c, inputcollid);
			if (e == NULL)
				return NIL;
			if (cmp == 0)
				return list_make1((Node *) e);
			{
				Numeric		neg = DatumGetNumeric(DirectFunctionCall1(numeric_uminus,
																	 NumericGetDatum(n)));
				Expr	   *e2 = make_var_eq(var, make_numeric_const(neg),
											 inputcollid);

				if (e2 == NULL)
					return NIL;
				return list_make1((Node *) makeBoolExpr(OR_EXPR,
														list_make2(e, e2), -1));
			}
		}

		if (f->funcid == oid_sign_int2 || f->funcid == oid_sign_int4 ||
			f->funcid == oid_sign_int8 || f->funcid == oid_sign_numeric ||
			f->funcid == oid_sign_float4 || f->funcid == oid_sign_float8)
		{
			int			sgn;

			if (!sign_const_kind(c, &sgn))
				return list_make1(make_bool_const(false));
			if (sgn == 0)
				return list_make1((Node *) make_var_eq(var, c, inputcollid));
			e = make_var_op_const(var, sgn < 0 ? "<" : ">",
								  make_zero_const(var->vartype),
								  inputcollid);
			return e ? list_make1((Node *) e) : NIL;
		}

		if (f->funcid == oid_floor_numeric || f->funcid == oid_floor_float8 ||
			f->funcid == oid_ceil_numeric || f->funcid == oid_ceil_float8 ||
			f->funcid == oid_trunc_numeric || f->funcid == oid_trunc_float8)
		{
			Const	   *lo;
			Const	   *hi;
			const char *lop;
			const char *hip;
			Expr	   *r;

			if (!derive_step_bounds(f->funcid, c, &lo, &hi, &lop, &hip))
				return NIL;
			r = make_range(var, lop, lo, hip, hi, inputcollid);
			return r ? list_make1((Node *) r) : NIL;
		}
	}

	return NIL;
}

/*
 * derive_arith
 *		col + k = c, col - k = c, k - col = c, -col = c, col * k = c.
 */
static List *
derive_arith(PlannerInfo *root, OpExpr *op, Const *c, Oid inputcollid)
{
	const char *opname = get_opname(op->opno);
	Var		   *var = NULL;
	Const	   *k = NULL;
	Oid			type = exprType((Node *) op);
	Datum		result;
	Const	   *rc;
	Expr	   *e;

	if (opname == NULL)
		return NIL;
	if (!type_is_int(type))
	{
		/* numeric +,- are also safe; multiplication is not exact */
		if (type != NUMERICOID)
			return NIL;
		if (strcmp(opname, "+") != 0 && strcmp(opname, "-") != 0)
			return NIL;
	}
	if (c->consttype != type)
		return NIL;

	if (list_length(op->args) == 1)
	{
		if (strcmp(opname, "-") != 0)
			return NIL;
		if (!var_is_simple(linitial(op->args), &var))
			return NIL;
		if (type == INT2OID)
		{
			int32		v = -(int32) DatumGetInt16(c->constvalue);

			if (v < PG_INT16_MIN || v > PG_INT16_MAX)
				return NIL;
			rc = make_int_const(type, v);
		}
		else if (type == INT4OID)
		{
			int32		v;

			if (c->constvalue == Int32GetDatum(PG_INT32_MIN))
				return NIL;
			v = -DatumGetInt32(c->constvalue);
			rc = make_int_const(type, v);
		}
		else if (type == INT8OID)
		{
			int64		v;

			if (DatumGetInt64(c->constvalue) == PG_INT64_MIN)
				return NIL;
			v = -DatumGetInt64(c->constvalue);
			rc = make_int_const(type, v);
		}
		else
		{
			Numeric		neg = DatumGetNumeric(DirectFunctionCall1(numeric_uminus,
																 NumericGetDatum(DatumGetNumeric(c->constvalue))));

			rc = make_numeric_const(neg);
		}
		e = make_var_eq(var, rc, inputcollid);
		return e ? list_make1((Node *) e) : NIL;
	}

	if (list_length(op->args) != 2)
		return NIL;

	if (var_is_simple(linitial(op->args), &var) &&
		IsA(lsecond(op->args), Const))
		k = (Const *) lsecond(op->args);
	else if (var_is_simple(lsecond(op->args), &var) &&
			 IsA(linitial(op->args), Const))
		k = (Const *) linitial(op->args);
	else
		return NIL;
	if (k->consttype != type || k->constisnull)
		return NIL;

	if (strcmp(opname, "+") == 0)
	{
		if (!compute_sub(type, c->constvalue, k->constvalue, &result))
			return NIL;
	}
	else if (strcmp(opname, "-") == 0)
	{
		if (var == (Var *) lsecond(op->args))
		{
			/* k - col = c  ->  col = k - c */
			if (!compute_sub(type, k->constvalue, c->constvalue, &result))
				return NIL;
		}
		else
		{
			/* col - k = c  ->  col = c + k */
			if (!compute_add(type, c->constvalue, k->constvalue, &result))
				return NIL;
		}
	}
	else if (strcmp(opname, "*") == 0 && type_is_int(type))
	{
		int64		kc = (type == INT2OID) ? DatumGetInt16(k->constvalue) :
			(type == INT4OID) ? DatumGetInt32(k->constvalue) :
			DatumGetInt64(k->constvalue);
		int64		cc = (type == INT2OID) ? DatumGetInt16(c->constvalue) :
			(type == INT4OID) ? DatumGetInt32(c->constvalue) :
			DatumGetInt64(c->constvalue);
		int64		q;

		if (kc == 0)
			return NIL;
		if (cc % kc != 0)
			return list_make1(make_bool_const(false));
		if (type == INT8OID && kc == -1 && cc == PG_INT64_MIN)
			return NIL;
		q = cc / kc;
		if (type == INT2OID && (q < PG_INT16_MIN || q > PG_INT16_MAX))
			return NIL;
		if (type == INT4OID && (q < PG_INT32_MIN || q > PG_INT32_MAX))
			return NIL;
		rc = make_int_const(type, q);
		e = make_var_eq(var, rc, inputcollid);
		return e ? list_make1((Node *) e) : NIL;
	}
	else
		return NIL;

	if (type == NUMERICOID)
		rc = make_numeric_const(DatumGetNumeric(result));
	else
		rc = make_int_const(type, (type == INT2OID) ? DatumGetInt16(result) :
							(type == INT4OID) ? DatumGetInt32(result) :
							DatumGetInt64(result));
	e = make_var_eq(var, rc, inputcollid);
	return e ? list_make1((Node *) e) : NIL;
}

/* ---------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------
 */

static void
init_func_oids(void)
{
	Oid			args_ts[1] = {TIMESTAMPOID};
	Oid			args_trunc[2] = {TEXTOID, TIMESTAMPOID};
	Oid			args_int2[1] = {INT2OID};
	Oid			args_int4[1] = {INT4OID};
	Oid			args_int8[1] = {INT8OID};
	Oid			args_numeric[1] = {NUMERICOID};
	Oid			args_numeric_i[2] = {NUMERICOID, INT4OID};
	Oid			args_float4[1] = {FLOAT4OID};
	Oid			args_float8[1] = {FLOAT8OID};

	if (func_oids_ready)
		return;

	oid_date_ts = LookupFuncName(list_make1(makeString("date")), 1, args_ts, true);
	oid_date_trunc_ts = LookupFuncName(list_make1(makeString("date_trunc")), 2,
									   args_trunc, true);
	oid_extract_ts = LookupFuncName(list_make1(makeString("extract")), 2,
									args_trunc, true);
	oid_date_part_ts = LookupFuncName(list_make1(makeString("date_part")), 2,
									  args_trunc, true);
	oid_abs_int2 = LookupFuncName(list_make1(makeString("abs")), 1, args_int2, true);
	oid_abs_int4 = LookupFuncName(list_make1(makeString("abs")), 1, args_int4, true);
	oid_abs_int8 = LookupFuncName(list_make1(makeString("abs")), 1, args_int8, true);
	oid_abs_numeric = LookupFuncName(list_make1(makeString("abs")), 1, args_numeric, true);
	oid_sign_int2 = LookupFuncName(list_make1(makeString("sign")), 1, args_int2, true);
	oid_sign_int4 = LookupFuncName(list_make1(makeString("sign")), 1, args_int4, true);
	oid_sign_int8 = LookupFuncName(list_make1(makeString("sign")), 1, args_int8, true);
	oid_sign_numeric = LookupFuncName(list_make1(makeString("sign")), 1, args_numeric, true);
	oid_sign_float4 = LookupFuncName(list_make1(makeString("sign")), 1, args_float4, true);
	oid_sign_float8 = LookupFuncName(list_make1(makeString("sign")), 1, args_float8, true);
	oid_floor_numeric = LookupFuncName(list_make1(makeString("floor")), 1, args_numeric, true);
	oid_floor_float8 = LookupFuncName(list_make1(makeString("floor")), 1, args_float8, true);
	oid_ceil_numeric = LookupFuncName(list_make1(makeString("ceil")), 1, args_numeric, true);
	oid_ceil_float8 = LookupFuncName(list_make1(makeString("ceil")), 1, args_float8, true);
	oid_trunc_numeric = LookupFuncName(list_make1(makeString("trunc")), 1, args_numeric, true);
	oid_trunc_numeric2 = LookupFuncName(list_make1(makeString("trunc")), 2, args_numeric_i, true);
	oid_trunc_float8 = LookupFuncName(list_make1(makeString("trunc")), 1, args_float8, true);

	func_oids_ready = true;
}

static bool
simple_var_eq(Node *cond, Var **var, Const **cval, Oid *collid)
{
	OpExpr	   *op;
	Node	   *l;
	Node	   *r;
	Var		   *v;
	Const	   *c;

	if (!IsA(cond, OpExpr))
		return false;
	op = (OpExpr *) cond;
	if (list_length(op->args) != 2 ||
		strcmp(get_opname(op->opno), "=") != 0)
		return false;
	l = (Node *) linitial(op->args);
	r = (Node *) lsecond(op->args);
	if (var_is_simple(l, &v) && IsA(r, Const))
	{
		c = (Const *) r;
	}
	else if (var_is_simple(r, &v) && IsA(l, Const))
	{
		c = (Const *) l;
	}
	else
		return false;
	if (c->constisnull)
		return false;
	*var = v;
	*cval = c;
	*collid = op->inputcollid;
	return true;
}

static Const *
fold_const(Node *node)
{
	Node	   *folded;

	if (node == NULL)
		return NULL;
	folded = IsA(node, Const) ? node : eval_const_expressions(NULL, node);
	if (!IsA(folded, Const) || ((Const *) folded)->constisnull)
		return NULL;
	return (Const *) folded;
}

static bool
const_equal(Const *a, Const *b, Oid collid)
{
	Oid			eq;
	Oid			eqfunc;

	if (a == NULL || b == NULL || a->constisnull || b->constisnull)
		return false;
	if (a->consttype != b->consttype)
		return false;
	eq = find_op("=", a->consttype, b->consttype);
	if (!OidIsValid(eq))
		return false;
	eqfunc = get_opcode(eq);
	return DatumGetBool(OidFunctionCall2Coll(eqfunc, collid,
											 a->constvalue, b->constvalue));
}

static bool
var_is_simple(Node *node, Var **var)
{
	Var		   *v;

	if (node == NULL || !IsA(node, Var))
		return false;
	v = (Var *) node;
	if (v->varlevelsup != 0 || v->varno == 0 || v->varattno <= 0)
		return false;
	*var = v;
	return true;
}

static Oid
find_op(const char *name, Oid left, Oid right)
{
	return OpernameGetOprid(list_make1(makeString((char *) name)), left, right);
}

static Expr *
make_var_eq(Var *var, Const *c, Oid inputcollid)
{
	Oid			eq;

	if (c == NULL || c->constisnull || var->vartype != c->consttype)
		return NULL;
	eq = find_op("=", var->vartype, c->consttype);
	if (!OidIsValid(eq))
		return NULL;
	return make_opclause(eq, BOOLOID, false, (Expr *) copyObject(var),
						 (Expr *) copyObject(c), InvalidOid, inputcollid);
}

static Expr *
make_var_op_const(Var *var, const char *opname, Const *c, Oid inputcollid)
{
	Oid			op;

	if (c == NULL || var->vartype != c->consttype)
		return NULL;
	op = find_op(opname, var->vartype, c->consttype);
	if (!OidIsValid(op))
		return NULL;
	return make_opclause(op, BOOLOID, false, (Expr *) copyObject(var),
						 (Expr *) copyObject(c), InvalidOid, inputcollid);
}

static Expr *
make_var_is_null(Var *var)
{
	NullTest   *nt = makeNode(NullTest);

	nt->arg = (Expr *) copyObject(var);
	nt->nulltesttype = IS_NULL;
	nt->argisrow = false;
	nt->location = -1;
	return (Expr *) nt;
}

static Expr *
make_bool_const(bool value)
{
	return (Expr *) makeBoolConst(value, false);
}

static Expr *
make_range(Var *var, const char *lop, Const *lo,
		   const char *hip, Const *hi, Oid inputcollid)
{
	Expr	   *a;
	Expr	   *b;

	a = make_var_op_const(var, lop, lo, inputcollid);
	b = make_var_op_const(var, hip, hi, inputcollid);
	if (a == NULL || b == NULL)
		return NULL;
	return (Expr *) makeBoolExpr(AND_EXPR, list_make2(a, b), -1);
}

static bool
type_is_int(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID;
}

static Const *
make_int_const(Oid type, int64 value)
{
	switch (type)
	{
		case INT2OID:
			return makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
							 Int16GetDatum((int16) value), false, true);
		case INT4OID:
			return makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
							 Int32GetDatum((int32) value), false, true);
		case INT8OID:
			return makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
							 Int64GetDatum(value), false, true);
		default:
			return NULL;
	}
}

static Const *
make_numeric_const(Numeric value)
{
	return makeConst(NUMERICOID, -1, InvalidOid, -1,
					 NumericGetDatum(value), false, false);
}

static Const *
make_float8_const(double value)
{
	return makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
					 Float8GetDatum(value), false, true);
}

static Const *
make_ts_const(Timestamp value)
{
	return makeConst(TIMESTAMPOID, -1, InvalidOid, sizeof(Timestamp),
					 TimestampGetDatum(value), false, true);
}

static Const *
make_zero_const(Oid type)
{
	switch (type)
	{
		case INT2OID:
			return make_int_const(type, 0);
		case INT4OID:
			return make_int_const(type, 0);
		case INT8OID:
			return make_int_const(type, 0);
		case NUMERICOID:
			return make_numeric_const(int64_to_numeric(0));
		case FLOAT4OID:
			return makeConst(FLOAT4OID, -1, InvalidOid, sizeof(float4),
							 Float4GetDatum(0.0f), false, true);
		case FLOAT8OID:
			return make_float8_const(0.0);
		default:
			return NULL;
	}
}

static bool
compute_add(Oid type, Datum a, Datum b, Datum *result)
{
	switch (type)
	{
		case INT2OID:
			{
				int32		r = (int32) DatumGetInt16(a) + (int32) DatumGetInt16(b);

				if (r < PG_INT16_MIN || r > PG_INT16_MAX)
					return false;
				*result = Int16GetDatum((int16) r);
				return true;
			}
		case INT4OID:
			{
				int32		r;

				if (pg_add_s32_overflow(DatumGetInt32(a), DatumGetInt32(b), &r))
					return false;
				*result = Int32GetDatum(r);
				return true;
			}
		case INT8OID:
			{
				int64		r;

				if (pg_add_s64_overflow(DatumGetInt64(a), DatumGetInt64(b), &r))
					return false;
				*result = Int64GetDatum(r);
				return true;
			}
		case NUMERICOID:
			{
				Numeric		r = numeric_add_safe(DatumGetNumeric(a),
												 DatumGetNumeric(b), NULL);

				if (r == NULL)
					return false;
				*result = NumericGetDatum(r);
				return true;
			}
		default:
			return false;
	}
}

static bool
compute_sub(Oid type, Datum a, Datum b, Datum *result)
{
	switch (type)
	{
		case INT2OID:
			{
				int32		r = (int32) DatumGetInt16(a) - (int32) DatumGetInt16(b);

				if (r < PG_INT16_MIN || r > PG_INT16_MAX)
					return false;
				*result = Int16GetDatum((int16) r);
				return true;
			}
		case INT4OID:
			{
				int32		r;

				if (pg_sub_s32_overflow(DatumGetInt32(a), DatumGetInt32(b), &r))
					return false;
				*result = Int32GetDatum(r);
				return true;
			}
		case INT8OID:
			{
				int64		r;

				if (pg_sub_s64_overflow(DatumGetInt64(a), DatumGetInt64(b), &r))
					return false;
				*result = Int64GetDatum(r);
				return true;
			}
		case NUMERICOID:
			{
				Numeric		r = numeric_sub_safe(DatumGetNumeric(a),
												 DatumGetNumeric(b), NULL);

				if (r == NULL)
					return false;
				*result = NumericGetDatum(r);
				return true;
			}
		default:
			return false;
	}
}

static Timestamp
ts_from_ymd(int year, int month, int day)
{
	char		buf[64];

	snprintf(buf, sizeof(buf), "%04d-%02d-%02d 00:00:00", year, month, day);
	return DatumGetTimestamp(DirectFunctionCall3(timestamp_in,
												 CStringGetDatum(buf),
												 ObjectIdGetDatum(InvalidOid),
												 Int32GetDatum(-1)));
}

static bool
derive_date_bounds(Const *c, Const **lo, Const **hi)
{
	DateADT		d;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (c->consttype != DATEOID || c->constisnull)
		return false;
	d = DatumGetDateADT(c->constvalue);
	if (DATE_IS_NOBEGIN(d) || DATE_IS_NOEND(d))
		return false;
	{
		Timestamp	lo_ts = date2timestamp_safe(d, (Node *) &escontext);
		Timestamp	hi_ts;

		if (escontext.error_occurred || !IS_VALID_TIMESTAMP(lo_ts))
			return false;
		hi_ts = lo_ts + USECS_PER_DAY;
		if (!IS_VALID_TIMESTAMP(hi_ts))
			return false;
		*lo = make_ts_const(lo_ts);
		*hi = make_ts_const(hi_ts);
	}
	return true;
}

static bool
interval_unit(const char *unit, Interval *iv)
{
	memset(iv, 0, sizeof(*iv));
	if (strcmp(unit, "day") == 0)
		iv->day = 1;
	else if (strcmp(unit, "month") == 0)
		iv->month = 1;
	else if (strcmp(unit, "year") == 0)
		iv->month = 12;
	else
		return false;
	return true;
}

static bool
derive_trunc_bounds(const char *unit, Const *c, Const **lo, Const **hi)
{
	Interval	iv;
	Timestamp	lo_ts;
	Timestamp	hi_ts;
	const Timestamp safe_lo = MIN_TIMESTAMP + 400 * USECS_PER_DAY;
	const Timestamp safe_hi = END_TIMESTAMP - 400 * USECS_PER_DAY;

	if (c->consttype != TIMESTAMPOID || c->constisnull)
		return false;
	if (!interval_unit(unit, &iv))
		return false;
	lo_ts = DatumGetTimestamp(c->constvalue);
	if (!IS_VALID_TIMESTAMP(lo_ts) || lo_ts < safe_lo || lo_ts > safe_hi)
		return false;
	hi_ts = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
												  TimestampGetDatum(lo_ts),
												  IntervalPGetDatum(&iv)));
	if (!IS_VALID_TIMESTAMP(hi_ts))
		return false;
	*lo = make_ts_const(lo_ts);
	*hi = make_ts_const(hi_ts);
	return true;
}

static bool
derive_year_bounds(int year, Const **lo, Const **hi)
{
	if (year < 1 || year > 9998)
		return false;
	*lo = make_ts_const(ts_from_ymd(year, 1, 1));
	*hi = make_ts_const(ts_from_ymd(year + 1, 1, 1));
	return true;
}

/*
 * year_from_const
 *		Extract an integral year from the numeric/float/int constant.
 */
static bool
year_from_const(Const *c, int *year)
{
	if (c->constisnull)
		return false;
	if (c->consttype == INT4OID)
	{
		*year = DatumGetInt32(c->constvalue);
		return true;
	}
	if (c->consttype == INT8OID)
	{
		int64		y = DatumGetInt64(c->constvalue);

		if (y < PG_INT32_MIN || y > PG_INT32_MAX)
			return false;
		*year = (int) y;
		return true;
	}
	if (c->consttype == NUMERICOID)
	{
		Numeric		n = DatumGetNumeric(c->constvalue);
		Numeric		back;
		int32		y;
		ErrorSaveContext escontext = {T_ErrorSaveContext};

		if (numeric_is_nan(n) || numeric_is_inf(n))
			return false;
		y = numeric_int4_safe(n, (Node *) &escontext);
		if (escontext.error_occurred)
			return false;
		back = int64_to_numeric(y);
		if (!DatumGetBool(DirectFunctionCall2(numeric_eq,
											 NumericGetDatum(n),
											 NumericGetDatum(back))))
			return false;
		*year = y;
		return true;
	}
	if (c->consttype == FLOAT8OID)
	{
		float8		f = DatumGetFloat8(c->constvalue);

		if (isnan(f) || isinf(f) || f != floor(f) ||
			f < PG_INT32_MIN || f > PG_INT32_MAX)
			return false;
		*year = (int) f;
		return true;
	}
	return false;
}

/*
 * sign_const_kind
 *		Return -1/0/1 when c is exactly -1/0/1 in its type; otherwise false.
 *
 * sign(x) = c only makes sense for those three values; any other constant
 * means the qual is never true.
 */
static bool
sign_const_kind(Const *c, int *kind)
{
	if (c->constisnull)
		return false;

	switch (c->consttype)
	{
		case INT2OID:
			{
				int16		v = DatumGetInt16(c->constvalue);

				if (v < -1 || v > 1)
					return false;
				*kind = v;
				return true;
			}
		case INT4OID:
			{
				int32		v = DatumGetInt32(c->constvalue);

				if (v < -1 || v > 1)
					return false;
				*kind = v;
				return true;
			}
		case INT8OID:
			{
				int64		v = DatumGetInt64(c->constvalue);

				if (v < -1 || v > 1)
					return false;
				*kind = v;
				return true;
			}
		case NUMERICOID:
			{
				Numeric		n = DatumGetNumeric(c->constvalue);
				Numeric		zero;
				Numeric		one;
				Numeric		mone;

				if (numeric_is_nan(n))
					return false;
				zero = int64_to_numeric(0);
				one = int64_to_numeric(1);
				mone = int64_to_numeric(-1);
				if (DatumGetBool(DirectFunctionCall2(numeric_eq,
													 NumericGetDatum(n),
													 NumericGetDatum(zero))))
					*kind = 0;
				else if (DatumGetBool(DirectFunctionCall2(numeric_eq,
														  NumericGetDatum(n),
														  NumericGetDatum(one))))
					*kind = 1;
				else if (DatumGetBool(DirectFunctionCall2(numeric_eq,
														  NumericGetDatum(n),
														  NumericGetDatum(mone))))
					*kind = -1;
				else
					return false;
				return true;
			}
		case FLOAT4OID:
			{
				float4		f = DatumGetFloat4(c->constvalue);

				if (isnan(f) || f < -1.0f || f > 1.0f)
					return false;
				*kind = (f < 0.0f) ? -1 : (f > 0.0f) ? 1 : 0;
				return true;
			}
		case FLOAT8OID:
			{
				float8		f = DatumGetFloat8(c->constvalue);

				if (isnan(f) || f < -1.0 || f > 1.0)
					return false;
				*kind = (f < 0.0) ? -1 : (f > 0.0) ? 1 : 0;
				return true;
			}
		default:
			return false;
	}
}

/*
 * derive_step_bounds
 *		Bounds for floor/ceil/trunc(x) = c.
 */
static bool
derive_step_bounds(Oid funcid, Const *c, Const **lo, Const **hi,
				   const char **lop, const char **hip)
{
	if (c->consttype == NUMERICOID)
	{
		Numeric		n = DatumGetNumeric(c->constvalue);
		Numeric		one = int64_to_numeric(1);
		Numeric		lo_n;
		Numeric		hi_n;
		int32		cmp0;

		if (numeric_is_nan(n) || numeric_is_inf(n))
			return false;
		cmp0 = DatumGetInt32(DirectFunctionCall2(numeric_cmp,
												 NumericGetDatum(n),
												 NumericGetDatum(int64_to_numeric(0))));

		if (funcid == oid_floor_numeric)
		{
			lo_n = n;
			hi_n = numeric_add_safe(n, one, NULL);
			*lop = ">=";
			*hip = "<";
		}
		else if (funcid == oid_ceil_numeric)
		{
			lo_n = numeric_sub_safe(n, one, NULL);
			hi_n = n;
			*lop = ">";
			*hip = "<=";
		}
		else
		{
			if (cmp0 == 0)
			{
				lo_n = numeric_sub_safe(n, one, NULL);
				hi_n = numeric_add_safe(n, one, NULL);
				*lop = ">";
				*hip = "<";
			}
			else if (cmp0 > 0)
			{
				lo_n = n;
				hi_n = numeric_add_safe(n, one, NULL);
				*lop = ">=";
				*hip = "<";
			}
			else
			{
				lo_n = numeric_sub_safe(n, one, NULL);
				hi_n = n;
				*lop = ">";
				*hip = "<=";
			}
		}
		if (lo_n == NULL || hi_n == NULL)
			return false;
		*lo = make_numeric_const(lo_n);
		*hi = make_numeric_const(hi_n);
		return true;
	}

	if (c->consttype == FLOAT8OID)
	{
		double		f = DatumGetFloat8(c->constvalue);
		double		lo_f;
		double		hi_f;

		if (isnan(f) || isinf(f) || fabs(f) >= 9.0e15)
			return false;
		if (funcid == oid_floor_float8)
		{
			lo_f = f;
			hi_f = f + 1.0;
			*lop = ">=";
			*hip = "<";
		}
		else if (funcid == oid_ceil_float8)
		{
			lo_f = f - 1.0;
			hi_f = f;
			*lop = ">";
			*hip = "<=";
		}
		else
		{
			if (f == 0.0)
			{
				lo_f = -1.0;
				hi_f = 1.0;
				*lop = ">";
				*hip = "<";
			}
			else if (f > 0.0)
			{
				lo_f = f;
				hi_f = f + 1.0;
				*lop = ">=";
				*hip = "<";
			}
			else
			{
				lo_f = f - 1.0;
				hi_f = f;
				*lop = ">";
				*hip = "<=";
			}
		}
		if (isinf(lo_f) || isinf(hi_f) || !(lo_f < hi_f))
			return false;
		*lo = make_float8_const(lo_f);
		*hi = make_float8_const(hi_f);
		return true;
	}

	return false;
}
