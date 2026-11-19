--
-- Tests for the per-comparison charge of a sort.
--
-- The run-count correction in tuplesort_run_count() is not observable from SQL:
-- it changes the number of merge passes only where the estimated in-memory
-- footprint crosses a merge-order boundary, and the boundary depends on
-- sizeof(SortTuple).  It is validated against the executor's trace_sort output
-- instead.
--
-- The planner looks the sort comparator up through the sort key's btree
-- opfamily and charges what pg_proc.procost says, scaled by the collation
-- provider.  Both inputs are moved here, and the assertions are equalities or
-- tolerance bands on the comparison component rather than absolute costs: the
-- component is isolated by subtracting the scan the sort already charged for,
-- and no exact cost is printed.
--
CREATE FUNCTION sort_plan_cost(query text) RETURNS numeric
LANGUAGE plpgsql AS
$$
DECLARE
    result json;
BEGIN
    EXECUTE format('EXPLAIN (FORMAT JSON) %s', query) INTO result;
    RETURN (result -> 0 -> 'Plan' ->> 'Total Cost')::numeric;
END
$$;

CREATE TABLE sort_cost_t (t text);
INSERT INTO sort_cost_t
    SELECT 'value' || lpad(g::text, 6, '0') FROM generate_series(1, 2000) g;
ANALYZE sort_cost_t;

-- The scan alone, so that the comparison component can be isolated from the
-- rest of the plan.
CREATE TEMP TABLE sort_cost_base AS
SELECT sort_plan_cost('SELECT * FROM sort_cost_t') AS scan,
       sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE "C"') AS sorted;

-- procost is what the planner reads.  At its default of 1.0 the charge is
-- exactly the same as before this mechanism existed.
SELECT sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE "C"') = sorted
           AS procost_1_is_noop
  FROM sort_cost_base;

-- Raising it raises the comparison component by about the same factor.  The
-- bands, not exact values, because the component also contains the per-tuple
-- output charge, whose share is a property of the plan and not of this
-- mechanism.
ALTER FUNCTION bttextcmp(text, text) COST 2;
SELECT (sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE "C"') - scan)
           BETWEEN 1.9 * (sorted - scan) AND 2.1 * (sorted - scan)
           AS procost_2_doubles_component
  FROM sort_cost_base;

ALTER FUNCTION bttextcmp(text, text) COST 4;
SELECT (sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE "C"') - scan)
           BETWEEN 3.7 * (sorted - scan) AND 4.0 * (sorted - scan)
           AS procost_4_quadruples_component
  FROM sort_cost_base;

ALTER FUNCTION bttextcmp(text, text) COST 1;
SELECT sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE "C"') = sorted
           AS procost_back_to_1_restores
  FROM sort_cost_base;

-- POSIX is an alias of C, and both are recognized as the C locale: the charge
-- is the same.  Comparing against C_COLLATION_OID would miss the alias.
SELECT sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE "POSIX"') = sorted
           AS posix_is_c
  FROM sort_cost_base;

-- The builtin provider compares code points and is charged slightly more than
-- C; ICU goes through the collation levels and is charged much more.
SELECT (sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE ucs_basic') - scan)
           BETWEEN 1.03 * (sorted - scan) AND 1.07 * (sorted - scan)
           AS builtin_provider_costs_more
  FROM sort_cost_base;

SELECT EXISTS (SELECT 1 FROM pg_collation
               WHERE collname = 'en-x-icu' AND collprovider = 'i') AS have_icu
\gset
\if :have_icu
SELECT (sort_plan_cost('SELECT * FROM sort_cost_t ORDER BY t COLLATE "en-x-icu"') - scan)
           BETWEEN 2.2 * (sorted - scan) AND 2.5 * (sorted - scan)
           AS icu_costs_much_more
  FROM sort_cost_base;
\endif

-- MergeAppend merges with the same comparators a sort uses, and was charged
-- the flat amount before this change.
CREATE TABLE sort_cost_p (t text) PARTITION BY RANGE (t);
CREATE TABLE sort_cost_p1 PARTITION OF sort_cost_p FOR VALUES FROM ('a') TO ('m');
CREATE TABLE sort_cost_p2 PARTITION OF sort_cost_p FOR VALUES FROM ('m') TO ('z');
INSERT INTO sort_cost_p
    SELECT 'a' || lpad(g::text, 6, '0') FROM generate_series(1, 500) g
    UNION ALL
    SELECT 'v' || lpad(g::text, 6, '0') FROM generate_series(1, 500) g;
CREATE INDEX ON sort_cost_p1 (t);
CREATE INDEX ON sort_cost_p2 (t);
ANALYZE sort_cost_p;

SET enable_seqscan = off;

CREATE TEMP TABLE sort_cost_merge AS
SELECT sort_plan_cost('SELECT * FROM sort_cost_p') AS append,
       sort_plan_cost('SELECT * FROM sort_cost_p ORDER BY t COLLATE "C"') AS merged;

ALTER FUNCTION bttextcmp(text, text) COST 4;
SELECT (sort_plan_cost('SELECT * FROM sort_cost_p ORDER BY t COLLATE "C"') - append)
           BETWEEN 3.0 * (merged - append) AND 4.2 * (merged - append)
           AS merge_append_uses_comparator_cost
  FROM sort_cost_merge;
ALTER FUNCTION bttextcmp(text, text) COST 1;

RESET enable_seqscan;

DROP TABLE sort_cost_p;
DROP TABLE sort_cost_t;
DROP FUNCTION sort_plan_cost(text);
