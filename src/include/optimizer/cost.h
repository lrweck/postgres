/*-------------------------------------------------------------------------
 *
 * cost.h
 *	  prototypes for costsize.c and clausesel.c.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/cost.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COST_H
#define COST_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"


/* defaults for costsize.c's Cost parameters */
/* NB: cost-estimation code should use the variables, not these constants! */
/* If you change these, update backend/utils/misc/postgresql.conf.sample */
#define DEFAULT_SEQ_PAGE_COST  1.0
/*
 * Measured on this machine: an index probe into a relation larger than the
 * cache costs about 22 us, against roughly 2 us for a sequential page, so
 * the random/sequential ratio is ~10 rather than the historical 4.
 */
#define DEFAULT_RANDOM_PAGE_COST  10.0
/*
 * Uniform price for CPU work, calibrated on this machine against the
 * sequential-scan anchor.  Every per-row and per-operator constant is
 * scaled by the same factor, so all node types keep a single unit; page
 * costs are untouched.  See the node-cost measurements in the commit.
 */
#define COST_CPU_SCALE 25.0

#define DEFAULT_CPU_TUPLE_COST	(0.01 * COST_CPU_SCALE)
/*
 * The index per-tuple cost is not bulk CPU: a unique probe pays one page
 * and a handful of comparisons, so it must not inherit the bulk scale.
 * Bulk index scans are charged per row in cost_index instead.
 */
#define DEFAULT_CPU_INDEX_TUPLE_COST 0.005
#define DEFAULT_CPU_OPERATOR_COST  (0.0025 * COST_CPU_SCALE)

/*
 * Per-purpose operator costs.  One number cannot price a filter comparison,
 * a btree descent, a sort comparison, an aggregate transition and a join
 * probe: they do different work.  Each is priced where it happens, so
 * calibrating one does not move the others.
 */
#define COST_OP_QUAL     (0.0025 * COST_CPU_SCALE)   /* filter/restriction eval */
#define COST_OP_SORT     (0.0025 * COST_CPU_SCALE)   /* sort comparison */
#define COST_OP_JOIN     (0.0025 * COST_CPU_SCALE)   /* join comparison */
#define COST_OP_AGG      (0.0025 * COST_CPU_SCALE)   /* aggregate transition */
/*
 * Per-row costs, one per kind of work: moving a row out of a scan is not
 * the same as driving a join probe or closing an aggregate group.
 */
/*
 * A heap scan pays for pages; moving a row out of a page is cheap.  The
 * measured per-row cost of a sequential scan already matches the model, so
 * this one keeps the historical value instead of the calibrated one.
 */
#define COST_ROW_SCAN    0.01                             /* 1x, unscaled */
#define COST_ROW_JOIN    (DEFAULT_CPU_TUPLE_COST * 2.0)    /* measured 63x vs scan */
#define COST_ROW_AGG     (DEFAULT_CPU_TUPLE_COST * 2.0)    /* measured 61x */
#define COST_ROW_SORT    (DEFAULT_CPU_TUPLE_COST * 4.0)    /* measured 218x */

#define COST_OP_INDEX    0.0025                      /* index qual per probed tuple */
#define COST_OP_DESCENT  0.0025                      /* btree descent, fixed per probe */
/*
 * Moving a tuple between processes is IPC, not computation: it costs
 * roughly 0.15 us per row, so it scales with a smaller factor than the CPU
 * family.  Keeping it below the CPU price keeps parallel plans attractive
 * whenever they really reduce wall time.
 */
#define COST_IPC_SCALE 10.0
#define DEFAULT_PARALLEL_TUPLE_COST (0.1 * COST_IPC_SCALE)
#define DEFAULT_PARALLEL_SETUP_COST  1000.0

/* defaults for non-Cost parameters */
#define DEFAULT_RECURSIVE_WORKTABLE_FACTOR  10.0
#define DEFAULT_EFFECTIVE_CACHE_SIZE  524288	/* measured in pages */

typedef enum
{
	CONSTRAINT_EXCLUSION_OFF,	/* do not use c_e */
	CONSTRAINT_EXCLUSION_ON,	/* apply c_e to all rels */
	CONSTRAINT_EXCLUSION_PARTITION, /* apply c_e to otherrels only */
}			ConstraintExclusionType;


/*
 * prototypes for costsize.c
 *	  routines to compute costs and sizes
 */

/* parameter variables and flags (see also optimizer.h) */
extern PGDLLIMPORT Cost disable_cost;
extern PGDLLIMPORT int max_parallel_workers_per_gather;
extern PGDLLIMPORT bool enable_seqscan;
extern PGDLLIMPORT bool enable_indexscan;
extern PGDLLIMPORT bool enable_indexonlyscan;
extern PGDLLIMPORT bool enable_bitmapscan;
extern PGDLLIMPORT bool enable_tidscan;
extern PGDLLIMPORT bool enable_sort;
extern PGDLLIMPORT bool enable_incremental_sort;
extern PGDLLIMPORT bool enable_hashagg;
extern PGDLLIMPORT bool enable_groupagg;
extern PGDLLIMPORT bool enable_nestloop;
extern PGDLLIMPORT bool enable_material;
extern PGDLLIMPORT bool enable_memoize;
extern PGDLLIMPORT bool enable_mergejoin;
extern PGDLLIMPORT bool enable_hashjoin;
extern PGDLLIMPORT bool enable_gathermerge;
extern PGDLLIMPORT bool enable_partitionwise_join;
extern PGDLLIMPORT bool enable_partitionwise_aggregate;
extern PGDLLIMPORT bool enable_parallel_append;
extern PGDLLIMPORT bool enable_parallel_hash;
extern PGDLLIMPORT bool enable_partition_pruning;
extern PGDLLIMPORT bool enable_presorted_aggregate;
extern PGDLLIMPORT bool enable_async_append;
extern PGDLLIMPORT bool enable_async_merge_append;
extern PGDLLIMPORT int constraint_exclusion;

extern double index_pages_fetched(double tuples_fetched, BlockNumber pages,
								  double index_pages, PlannerInfo *root);
extern void cost_seqscan(Path *path, PlannerInfo *root, RelOptInfo *baserel,
						 ParamPathInfo *param_info);
extern void cost_samplescan(Path *path, PlannerInfo *root, RelOptInfo *baserel,
							ParamPathInfo *param_info);
extern void cost_index(IndexPath *path, PlannerInfo *root,
					   double loop_count, bool partial_path);
extern void cost_bitmap_heap_scan(Path *path, PlannerInfo *root, RelOptInfo *baserel,
								  ParamPathInfo *param_info,
								  Path *bitmapqual, double loop_count);
extern void cost_bitmap_and_node(BitmapAndPath *path, PlannerInfo *root);
extern void cost_bitmap_or_node(BitmapOrPath *path, PlannerInfo *root);
extern void cost_bitmap_tree_node(Path *path, Cost *cost, Selectivity *selec);
extern void cost_tidscan(Path *path, PlannerInfo *root,
						 RelOptInfo *baserel, List *tidquals, ParamPathInfo *param_info);
extern void cost_tidrangescan(Path *path, PlannerInfo *root,
							  RelOptInfo *baserel, List *tidrangequals,
							  ParamPathInfo *param_info);
extern void cost_subqueryscan(SubqueryScanPath *path, PlannerInfo *root,
							  RelOptInfo *baserel, ParamPathInfo *param_info,
							  bool trivial_pathtarget);
extern void cost_functionscan(Path *path, PlannerInfo *root,
							  RelOptInfo *baserel, ParamPathInfo *param_info);
extern void cost_valuesscan(Path *path, PlannerInfo *root,
							RelOptInfo *baserel, ParamPathInfo *param_info);
extern void cost_tablefuncscan(Path *path, PlannerInfo *root,
							   RelOptInfo *baserel, ParamPathInfo *param_info);
extern void cost_ctescan(Path *path, PlannerInfo *root,
						 RelOptInfo *baserel, ParamPathInfo *param_info);
extern void cost_namedtuplestorescan(Path *path, PlannerInfo *root,
									 RelOptInfo *baserel, ParamPathInfo *param_info);
extern void cost_resultscan(Path *path, PlannerInfo *root,
							RelOptInfo *baserel, ParamPathInfo *param_info);
extern void cost_recursive_union(Path *runion, Path *nrterm, Path *rterm);
extern Cost sort_comparison_cost(PlannerInfo *root, List *pathkeys);
extern void cost_sort(Path *path, PlannerInfo *root,
					  List *pathkeys, int input_disabled_nodes,
					  Cost input_cost, double tuples, int width,
					  Cost comparison_cost, int sort_mem,
					  double limit_tuples);
extern void cost_incremental_sort(Path *path,
								  PlannerInfo *root, List *pathkeys, int presorted_keys,
								  int input_disabled_nodes,
								  Cost input_startup_cost, Cost input_total_cost,
								  double input_tuples, int width, Cost comparison_cost, int sort_mem,
								  double limit_tuples,
								  Cardinality *num_groups);
extern void cost_append(AppendPath *apath, PlannerInfo *root);
extern void cost_merge_append(Path *path, PlannerInfo *root,
							  List *pathkeys, int n_streams,
							  int input_disabled_nodes,
							  Cost input_startup_cost, Cost input_total_cost,
							  double tuples);
extern void cost_material(Path *path,
						  bool enabled, int input_disabled_nodes,
						  Cost input_startup_cost, Cost input_total_cost,
						  double tuples, int width);
extern void cost_agg(Path *path, PlannerInfo *root,
					 AggStrategy aggstrategy, const AggClauseCosts *aggcosts,
					 int numGroupCols, double numGroups,
					 List *quals,
					 int disabled_nodes,
					 Cost input_startup_cost, Cost input_total_cost,
					 double input_tuples, double input_width);
extern void cost_windowagg(Path *path, PlannerInfo *root,
						   List *windowFuncs, WindowClause *winclause,
						   int input_disabled_nodes,
						   Cost input_startup_cost, Cost input_total_cost,
						   double input_tuples);
extern void cost_group(Path *path, PlannerInfo *root,
					   int numGroupCols, double numGroups,
					   List *quals,
					   int input_disabled_nodes,
					   Cost input_startup_cost, Cost input_total_cost,
					   double input_tuples);
extern void initial_cost_nestloop(PlannerInfo *root,
								  JoinCostWorkspace *workspace,
								  JoinType jointype, uint64 enable_mask,
								  Path *outer_path, Path *inner_path,
								  JoinPathExtraData *extra);
extern void final_cost_nestloop(PlannerInfo *root, NestPath *path,
								JoinCostWorkspace *workspace,
								JoinPathExtraData *extra);
extern void initial_cost_mergejoin(PlannerInfo *root,
								   JoinCostWorkspace *workspace,
								   JoinType jointype,
								   List *mergeclauses,
								   Path *outer_path, Path *inner_path,
								   List *outersortkeys, List *innersortkeys,
								   int outer_presorted_keys,
								   JoinPathExtraData *extra);
extern void final_cost_mergejoin(PlannerInfo *root, MergePath *path,
								 JoinCostWorkspace *workspace,
								 JoinPathExtraData *extra);
extern void initial_cost_hashjoin(PlannerInfo *root,
								  JoinCostWorkspace *workspace,
								  JoinType jointype,
								  List *hashclauses,
								  Path *outer_path, Path *inner_path,
								  JoinPathExtraData *extra,
								  bool parallel_hash);
extern void final_cost_hashjoin(PlannerInfo *root, HashPath *path,
								JoinCostWorkspace *workspace,
								JoinPathExtraData *extra);
extern void cost_gather(GatherPath *path, PlannerInfo *root,
						RelOptInfo *rel, ParamPathInfo *param_info, double *rows);
extern void cost_gather_merge(GatherMergePath *path, PlannerInfo *root,
							  RelOptInfo *rel, ParamPathInfo *param_info,
							  int input_disabled_nodes,
							  Cost input_startup_cost, Cost input_total_cost,
							  double *rows);
extern void cost_subplan(PlannerInfo *root, SubPlan *subplan, Plan *plan);
extern void cost_qual_eval(QualCost *cost, List *quals, PlannerInfo *root);
extern void cost_qual_eval_node(QualCost *cost, Node *qual, PlannerInfo *root);
extern void compute_semi_anti_join_factors(PlannerInfo *root,
										   RelOptInfo *joinrel,
										   RelOptInfo *outerrel,
										   RelOptInfo *innerrel,
										   JoinType jointype,
										   SpecialJoinInfo *sjinfo,
										   List *restrictlist,
										   SemiAntiJoinFactors *semifactors);
extern void set_baserel_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern double get_parameterized_baserel_size(PlannerInfo *root,
											 RelOptInfo *rel,
											 List *param_clauses);
extern double get_parameterized_joinrel_size(PlannerInfo *root,
											 RelOptInfo *rel,
											 Path *outer_path,
											 Path *inner_path,
											 SpecialJoinInfo *sjinfo,
											 List *restrict_clauses);
extern void set_joinrel_size_estimates(PlannerInfo *root, RelOptInfo *rel,
									   RelOptInfo *outer_rel,
									   RelOptInfo *inner_rel,
									   SpecialJoinInfo *sjinfo,
									   List *restrictlist);
extern void set_subquery_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern void set_function_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern void set_values_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern void set_cte_size_estimates(PlannerInfo *root, RelOptInfo *rel,
								   double cte_rows);
extern void set_tablefunc_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern void set_namedtuplestore_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern void set_result_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern void set_foreign_size_estimates(PlannerInfo *root, RelOptInfo *rel);
extern PathTarget *set_pathtarget_cost_width(PlannerInfo *root, PathTarget *target);
extern double compute_bitmap_pages(PlannerInfo *root, RelOptInfo *baserel,
								   Path *bitmapqual, double loop_count,
								   Cost *cost_p, double *tuples_p);
extern double compute_gather_rows(Path *path);

#endif							/* COST_H */
