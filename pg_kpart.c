/*-------------------------------------------------------------------------
 *
 * pg_kpart.c
 *		Reject queries that scan every partition of a partitioned table
 *		without a usable predicate on the partition key.
 *
 * The module installs a planner_hook. After the standard planner has built
 * a plan, the resulting plan tree is walked. Every Append / MergeAppend that
 * sits on top of a partition hierarchy is examined:
 *
 *   - If the node carries run-time pruning information (part_prune_info),
 *     the partition key *is* being used (pruning will happen at execution
 *     time, e.g. for a parameterized predicate), so the node is allowed.
 *
 *   - Otherwise, if the number of leaf partitions that survived planning is
 *     equal to the total number of leaf partitions of the queried
 *     partitioned table, then no pruning took place: the query did not
 *     restrict the partition key and would scan every partition. This is
 *     reported at the configured message level (ERROR by default).
 *
 * A predicate that only prunes *some* partitions (e.g. a range on the
 * partition key spanning a few partitions) leaves fewer leaves than the
 * total, so it is accepted.
 *
 * Authors : Gilles Darold <gilles@darold.net>
 * IA      : IA drive, code review and fixes by the author.
 * Licence : PostgreSQL
 * Copyright (c) 2026, Hexacluster Corp.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

/* Custom SQLSTATE so applications can trap the condition (class "FS"). */
#define ERRCODE_PG_KPART_VIOLATION MAKE_SQLSTATE('F', 'S', '0', '0', '1')

/* ---- GUC-backed configuration ---- */
static bool pg_kpart_enabled = true;
static bool pg_kpart_check_superuser = false;
static int  pg_kpart_min_partitions = 2;
static int  pg_kpart_message_level = ERROR;
static char *pg_kpart_blacklist = NULL;
static char *pg_kpart_whitelist = NULL;

static const struct config_enum_entry message_level_options[] = {
	{"debug", DEBUG2, true},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"log", LOG, false},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{NULL, 0, false}
};

/* Saved previous hook (chaining). */
static planner_hook_type prev_planner_hook = NULL;

/* Walk context threaded through the recursion. */
typedef struct KPartContext
{
	PlannedStmt *pstmt;
	List	   *part_relids;	/* relids of partitioned-table RTEs in stmt */
} KPartContext;

void		_PG_init(void);
void		_PG_fini(void);

/* ---- helpers ------------------------------------------------------------ */

/* Is this relation a partition (i.e. relispartition is true)? */
static bool
rel_is_partition(Oid relid)
{
	HeapTuple	tp;
	bool		result = false;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		result = ((Form_pg_class) GETSTRUCT(tp))->relispartition;
		ReleaseSysCache(tp);
	}
	return result;
}

/* Recursively count the leaf partitions below relid. */
static int
count_leaf_partitions(Oid relid)
{
	Relation	rel;
	int			count = 0;

	rel = table_open(relid, AccessShareLock);

	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc pd;
		int			i;

#if PG_VERSION_NUM >= 140000
		pd = RelationGetPartitionDesc(rel, true);
#else
		pd = RelationGetPartitionDesc(rel);
#endif
		for (i = 0; i < pd->nparts; i++)
			count += count_leaf_partitions(pd->oids[i]);
	}
	else
		count = 1;

	table_close(rel, AccessShareLock);
	return count;
}

/*
 * Given a leaf partition, return the highest ancestor that actually appears
 * as a partitioned-table RTE in this statement. That is the partitioned
 * table the user referenced (which may be the top root or an intermediate
 * partitioned table when sub-partitioning is queried directly).
 */
static Oid
relevant_root(Oid leaf, List *part_relids)
{
	List	   *ancestors = get_partition_ancestors(leaf);
	ListCell   *lc;
	Oid			root = InvalidOid;

	/* ancestors are ordered immediate-parent first, top root last */
	foreach(lc, ancestors)
	{
		Oid			anc = lfirst_oid(lc);

		if (list_member_oid(part_relids, anc))
			root = anc;			/* keep the last (highest) match */
	}
	list_free(ancestors);
	return root;
}

/*
 * Resolve a single (possibly schema-qualified) relation name to its OID.
 * Returns InvalidOid if the name does not resolve. Never raises: unqualified
 * names are looked up through the current search_path.
 */
static Oid
resolve_rel_name(const char *name)
{
	char	   *copy = pstrdup(name);
	char	   *dot;
	RangeVar   *rv;
	Oid			relid;

	dot = strchr(copy, '.');
	if (dot != NULL)
	{
		*dot = '\0';
		rv = makeRangeVar(copy, dot + 1, -1);
	}
	else
		rv = makeRangeVar(NULL, copy, -1);

	relid = RangeVarGetRelid(rv, NoLock, true /* missing_ok */ );

	pfree(copy);
	return relid;
}

/*
 * Parse a comma-separated list of relation names into a list of OIDs,
 * skipping blank or unresolvable entries.
 */
static List *
parse_relname_list(const char *raw)
{
	List	   *result = NIL;
	char	   *copy;
	char	   *tok;
	char	   *saveptr;

	if (raw == NULL || raw[0] == '\0')
		return NIL;

	copy = pstrdup(raw);
	for (tok = strtok_r(copy, ",", &saveptr);
		 tok != NULL;
		 tok = strtok_r(NULL, ",", &saveptr))
	{
		Oid			relid;

		/* trim leading and trailing whitespace */
		while (*tok == ' ' || *tok == '\t' || *tok == '\n' || *tok == '\r')
			tok++;
		{
			char	   *end = tok + strlen(tok);

			while (end > tok &&
				   (end[-1] == ' ' || end[-1] == '\t' ||
					end[-1] == '\n' || end[-1] == '\r'))
				*(--end) = '\0';
		}
		if (*tok == '\0')
			continue;

		relid = resolve_rel_name(tok);
		if (OidIsValid(relid))
			result = lappend_oid(result, relid);
	}
	pfree(copy);
	return result;
}

/*
 * Is root, or any of its partition ancestors, named in the comma-separated
 * list? This is what makes a listed table also cover its sub-partitions: a
 * sub-partitioned table queried directly matches if one of its ancestors is
 * listed.
 */
static bool
root_or_ancestor_listed(Oid root, const char *raw)
{
	List	   *oids = parse_relname_list(raw);
	bool		found = false;

	if (oids == NIL)
		return false;

	if (list_member_oid(oids, root))
		found = true;
	else
	{
		List	   *ancestors = get_partition_ancestors(root);
		ListCell   *lc;

		foreach(lc, ancestors)
		{
			if (list_member_oid(oids, lfirst_oid(lc)))
			{
				found = true;
				break;
			}
		}
		list_free(ancestors);
	}

	list_free(oids);
	return found;
}

/*
 * Decide whether the partitioned table "root" is subject to the check.
 *
 *   - pg_kpart.blacklisted set  -> only listed tables (and sub-partitions);
 *                                  pg_kpart.whitelisted is ignored.
 *   - else pg_kpart.whitelisted -> all tables except the listed ones.
 *   - else                      -> all partitioned tables.
 */
static bool
table_in_scope(Oid root)
{
	if (pg_kpart_blacklist != NULL && pg_kpart_blacklist[0] != '\0')
		return root_or_ancestor_listed(root, pg_kpart_blacklist);

	if (pg_kpart_whitelist != NULL && pg_kpart_whitelist[0] != '\0')
		return !root_or_ancestor_listed(root, pg_kpart_whitelist);

	return true;
}

/* If plan is a relation scan node, return its scanrelid, else 0. */
static Index
plan_scanrelid(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
#if PG_VERSION_NUM >= 140000
		case T_TidRangeScan:
#endif
		case T_ForeignScan:
		case T_CustomScan:
			return ((Scan *) plan)->scanrelid;
		default:
			return 0;
	}
}

/*
 * Inspect the children of one Append / MergeAppend. If run-time pruning is in
 * play (the caller resolves this in a version-aware way), the partition key is
 * used and the node is fine.
 */
static void
check_append_children(List *subplans, bool has_runtime_pruning,
					   KPartContext *cxt)
{
	ListCell   *lc;
	int			scanned = 0;
	Oid			leaf = InvalidOid;
	Oid			root;
	int			total;

	/* Run-time pruning present => the partition key is used. */
	if (has_runtime_pruning)
		return;

	foreach(lc, subplans)
	{
		Plan	   *child = (Plan *) lfirst(lc);
		Index		scanrelid = plan_scanrelid(child);

		if (scanrelid > 0)
		{
			RangeTblEntry *rte = rt_fetch(scanrelid, cxt->pstmt->rtable);

			if (rte->rtekind == RTE_RELATION &&
				OidIsValid(rte->relid) &&
				rel_is_partition(rte->relid))
			{
				scanned++;
				if (!OidIsValid(leaf))
					leaf = rte->relid;
			}
		}
	}

	/* Nothing to prune if we only touch one (or zero) partitions. */
	if (scanned < 2 || !OidIsValid(leaf))
		return;

	root = relevant_root(leaf, cxt->part_relids);
	if (!OidIsValid(root))
		return;					/* could not anchor; stay silent */

	/* Honour the blacklist / whitelist scoping. */
	if (!table_in_scope(root))
		return;

	total = count_leaf_partitions(root);

	if (total >= pg_kpart_min_partitions && scanned >= total)
		ereport(pg_kpart_message_level,
				(errcode(ERRCODE_PG_KPART_VIOLATION),
				 errmsg("query on partitioned table \"%s\" would scan all %d partitions",
						get_rel_name(root), total),
				 errdetail("No partition pruning is possible because the query does not restrict the partition key."),
				 errhint("Add a condition on the partition key, or set pg_kpart.enabled = off to bypass this check.")));
}

/* Recursively walk a plan subtree. */
static void
check_plan(Plan *plan, KPartContext *cxt)
{
	if (plan == NULL)
		return;

	switch (nodeTag(plan))
	{
		case T_Append:
			{
				Append	   *a = (Append *) plan;
				bool		prune;

#if PG_VERSION_NUM >= 180000
				prune = (a->part_prune_index >= 0);
#else
				prune = (a->part_prune_info != NULL);
#endif
				check_append_children(a->appendplans, prune, cxt);
			}
			break;
		case T_MergeAppend:
			{
				MergeAppend *m = (MergeAppend *) plan;
				bool		prune;

#if PG_VERSION_NUM >= 180000
				prune = (m->part_prune_index >= 0);
#else
				prune = (m->part_prune_info != NULL);
#endif
				check_append_children(m->mergeplans, prune, cxt);
			}
			break;
		case T_SubqueryScan:
			check_plan(((SubqueryScan *) plan)->subplan, cxt);
			break;
		case T_BitmapAnd:
			{
				ListCell   *lc;

				foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
					check_plan((Plan *) lfirst(lc), cxt);
			}
			break;
		case T_BitmapOr:
			{
				ListCell   *lc;

				foreach(lc, ((BitmapOr *) plan)->bitmapplans)
					check_plan((Plan *) lfirst(lc), cxt);
			}
			break;
#if PG_VERSION_NUM < 140000
		case T_ModifyTable:
			{
				ListCell   *lc;

				foreach(lc, ((ModifyTable *) plan)->plans)
					check_plan((Plan *) lfirst(lc), cxt);
			}
			break;
#endif
		default:
			break;
	}

	/*
	 * Recurse into Append/MergeAppend children (harmless: they are leaf
	 * scans) and into the generic left/right subtrees. From PG14 on a
	 * ModifyTable's source plan is reachable via outerPlan (lefttree).
	 */
	if (IsA(plan, Append))
	{
		ListCell   *lc;

		foreach(lc, ((Append *) plan)->appendplans)
			check_plan((Plan *) lfirst(lc), cxt);
	}
	else if (IsA(plan, MergeAppend))
	{
		ListCell   *lc;

		foreach(lc, ((MergeAppend *) plan)->mergeplans)
			check_plan((Plan *) lfirst(lc), cxt);
	}

	check_plan(plan->lefttree, cxt);
	check_plan(plan->righttree, cxt);
}

/* Top-level entry: build context and walk the whole PlannedStmt. */
static void
check_planned_stmt(PlannedStmt *pstmt)
{
	KPartContext cxt;
	ListCell   *lc;

	cxt.pstmt = pstmt;
	cxt.part_relids = NIL;

	/* Collect partitioned-table relids; also acts as a fast pre-check. */
	foreach(lc, pstmt->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_RELATION &&
			rte->relkind == RELKIND_PARTITIONED_TABLE &&
			OidIsValid(rte->relid))
			cxt.part_relids = lappend_oid(cxt.part_relids, rte->relid);
	}

	if (cxt.part_relids == NIL)
		return;

	check_plan(pstmt->planTree, &cxt);

	foreach(lc, pstmt->subplans)
		check_plan((Plan *) lfirst(lc), &cxt);

	list_free(cxt.part_relids);
}

/* ---- planner hook ------------------------------------------------------- */

static PlannedStmt *
pg_kpart_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams
#if PG_VERSION_NUM >= 190000
				 , ExplainState *es
#endif
				 )
{
	PlannedStmt *result;

	if (prev_planner_hook)
		result = prev_planner_hook(parse, query_string, cursorOptions,
								   boundParams
#if PG_VERSION_NUM >= 190000
								   , es
#endif
								   );
	else
		result = standard_planner(parse, query_string, cursorOptions,
								  boundParams
#if PG_VERSION_NUM >= 150000
								   , es
#endif
								  );

	if (pg_kpart_enabled &&
		result != NULL &&
		result->commandType != CMD_UTILITY &&
		(pg_kpart_check_superuser || !superuser()))
	{
		check_planned_stmt(result);
	}

	return result;
}

/* ---- module init / fini ------------------------------------------------- */

void
_PG_init(void)
{
	DefineCustomBoolVariable("pg_kpart.enabled",
							 "Enable enforcement of partition-key usage.",
							 NULL,
							 &pg_kpart_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_kpart.check_superuser",
							 "Apply the check to superusers as well.",
							 NULL,
							 &pg_kpart_check_superuser,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_kpart.min_partitions",
							"Minimum number of leaf partitions before the check applies.",
							NULL,
							&pg_kpart_min_partitions,
							2, 1, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("pg_kpart.message_level",
							 "Message level emitted when a full partition scan is detected.",
							 NULL,
							 &pg_kpart_message_level,
							 ERROR,
							 message_level_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("pg_kpart.blacklisted",
							   "Comma-separated partitioned tables the check "
							   "applies to (sub-partitions included).",
							   "When set, the check is applied only to these tables; "
							   "pg_kpart.whitelisted is ignored. Empty means all "
							   "partitioned tables.",
							   &pg_kpart_blacklist,
							   "",
							   PGC_USERSET,
							   GUC_LIST_INPUT,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("pg_kpart.whitelisted",
							   "Comma-separated partitioned tables exempt from the "
							   "check (sub-partitions included).",
							   "Ignored when pg_kpart.blacklisted is set.",
							   &pg_kpart_whitelist,
							   "",
							   PGC_USERSET,
							   GUC_LIST_INPUT,
							   NULL, NULL, NULL);


#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("pg_kpart");
#else
	EmitWarningsOnPlaceholders("pg_kpart");
#endif

	prev_planner_hook = planner_hook;
	planner_hook = pg_kpart_planner;
}

void
_PG_fini(void)
{
	planner_hook = prev_planner_hook;
}

