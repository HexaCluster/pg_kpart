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
 * The module also installs a ProcessUtility_hook that can forbid the
 * creation of new partitions (CREATE TABLE ... PARTITION OF and
 * ALTER TABLE ... ATTACH PARTITION) during the time-of-day windows set in
 * pg_kpart.deny_partition_creation.
 *
 * Authors : Gilles Darold <gilles@darold.net>
 * IA      : IA drive, code review and fixes by the author.
 * Licence : PostgreSQL
 * Copyright (c) 2026, Hexacluster Corp.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
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
#include "pgtime.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* Custom SQLSTATE so applications can trap the condition (class "FS"). */
#define ERRCODE_PG_KPART_VIOLATION MAKE_SQLSTATE('F', 'S', '0', '0', '1')
/* Partition creation attempted during a forbidden time window. */
#define ERRCODE_PG_KPART_CREATION_DENIED MAKE_SQLSTATE('F', 'S', '0', '0', '2')

#define SECS_IN_DAY		(24 * 60 * 60)

/*
 * One time-of-day window, in seconds since midnight. The window is the
 * half-open interval [start, end). When start > end the window wraps around
 * midnight (e.g. 22:00-06:00).
 */
typedef struct KPartTimeWindow
{
	int			start;
	int			end;
} KPartTimeWindow;

/* Parsed form of pg_kpart.deny_partition_creation (GUC "extra" data). */
typedef struct KPartTimeWindows
{
	int			nwindows;
	KPartTimeWindow windows[FLEXIBLE_ARRAY_MEMBER];
} KPartTimeWindows;

/*
 * GUC "extra" data allocation. Since PG16, guc_malloc()/guc_free() are
 * exported and allocate in GUCMemoryContext, so they must be used. Before
 * PG16, guc_malloc() is static to guc.c and extra data must be allocated
 * with plain malloc(), guc.c releasing it with free().
 */
#if PG_VERSION_NUM >= 160000
#define KPART_GUC_MALLOC(sz)	guc_malloc(LOG, (sz))
#define KPART_GUC_FREE(p)		guc_free(p)
#else
static void *
kpart_guc_malloc(size_t size)
{
	void	   *p = malloc(size);

	if (p == NULL)
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return p;
}
#define KPART_GUC_MALLOC(sz)	kpart_guc_malloc(sz)
#define KPART_GUC_FREE(p)		free(p)
#endif

/* ---- GUC-backed configuration ---- */
static bool pg_kpart_enabled = true;
static bool pg_kpart_check_superuser = false;
static bool pg_kpart_nouser_change = false;
static int  pg_kpart_min_partitions = 2;
static int  pg_kpart_message_level = ERROR;
static char *pg_kpart_blacklist = NULL;
static char *pg_kpart_whitelist = NULL;
static char *pg_kpart_deny_creation = NULL;

/* Parsed windows, set by the assign hook; NULL means no restriction. */
static KPartTimeWindows *deny_creation_windows = NULL;

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

/* Saved previous hooks (chaining). */
static planner_hook_type prev_planner_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

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
#if PG_VERSION_NUM >= 190000
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

/* ---- partition creation time windows ------------------------------------ */

/*
 * Parse a time of day "H:MM", "HH:MM" or "HH:MM:SS" at *p, advancing *p past
 * it. 24:00[:00] is accepted (end of day). Returns false on bad syntax.
 */
static bool
parse_time_of_day(const char **p, int *secs)
{
	const char *s = *p;
	int			h = 0,
				m = 0,
				sec = 0,
				nd;

	for (nd = 0; nd < 2 && isdigit((unsigned char) *s); nd++, s++)
		h = h * 10 + (*s - '0');
	if (nd == 0 || *s != ':')
		return false;
	s++;

	for (nd = 0; nd < 2 && isdigit((unsigned char) *s); nd++, s++)
		m = m * 10 + (*s - '0');
	if (nd != 2)
		return false;

	if (*s == ':')
	{
		s++;
		for (nd = 0; nd < 2 && isdigit((unsigned char) *s); nd++, s++)
			sec = sec * 10 + (*s - '0');
		if (nd != 2)
			return false;
	}

	if (h > 24 || m > 59 || sec > 59 || (h == 24 && (m != 0 || sec != 0)))
		return false;

	*secs = h * 3600 + m * 60 + sec;
	*p = s;
	return true;
}

/* Format seconds since midnight as HH:MM, or HH:MM:SS when needed. */
static void
format_time_of_day(int secs, char *buf, size_t len)
{
	if (secs % 60 != 0)
		snprintf(buf, len, "%02d:%02d:%02d",
				 secs / 3600, (secs / 60) % 60, secs % 60);
	else
		snprintf(buf, len, "%02d:%02d", secs / 3600, (secs / 60) % 60);
}

/*
 * GUC check hook for pg_kpart.deny_partition_creation.
 *
 * Accepts a comma-separated list of "HH:MM[:SS]-HH:MM[:SS]" ranges. Empty
 * items are ignored; an empty value means no restriction. The parsed windows
 * are handed to the assign hook through *extra.
 */
static bool
check_deny_creation(char **newval, void **extra, GucSource source)
{
	const char *p = *newval;
	int			maxw = 1;
	KPartTimeWindows *w;

	if (p == NULL)
		p = "";

	for (; *p; p++)
		if (*p == ',')
			maxw++;

	w = (KPartTimeWindows *) KPART_GUC_MALLOC(offsetof(KPartTimeWindows, windows) +
											  maxw * sizeof(KPartTimeWindow));
	if (w == NULL)
		return false;
	w->nwindows = 0;

	p = (*newval != NULL) ? *newval : "";
	for (;;)
	{
		int			start,
					end;

		while (isspace((unsigned char) *p))
			p++;
		if (*p == '\0')
			break;
		if (*p == ',')
		{
			p++;
			continue;
		}

		if (!parse_time_of_day(&p, &start))
			goto syntax_error;
		while (isspace((unsigned char) *p))
			p++;
		if (*p != '-')
			goto syntax_error;
		p++;
		while (isspace((unsigned char) *p))
			p++;
		if (!parse_time_of_day(&p, &end))
			goto syntax_error;
		while (isspace((unsigned char) *p))
			p++;
		if (*p != ',' && *p != '\0')
			goto syntax_error;

		if (start == SECS_IN_DAY)
		{
			GUC_check_errdetail("24:00 is only allowed as the end of a time window.");
			KPART_GUC_FREE(w);
			return false;
		}
		if (start == end)
		{
			GUC_check_errdetail("A time window must not start and end at the same time.");
			KPART_GUC_FREE(w);
			return false;
		}

		Assert(w->nwindows < maxw);
		w->windows[w->nwindows].start = start;
		w->windows[w->nwindows].end = end;
		w->nwindows++;
	}

	*extra = w;
	return true;

syntax_error:
	GUC_check_errdetail("Expected a comma-separated list of time windows "
						"in the form HH:MM[:SS]-HH:MM[:SS].");
	KPART_GUC_FREE(w);
	return false;
}

static void
assign_deny_creation(const char *newval, void *extra)
{
	KPartTimeWindows *w = (KPartTimeWindows *) extra;

	deny_creation_windows = (w != NULL && w->nwindows > 0) ? w : NULL;
}

/*
 * Is the current wall-clock time, read in the server's log_timezone, inside
 * one of the forbidden windows? log_timezone is used rather than the session
 * TimeZone because the latter can be changed by any user to dodge the rule.
 * On a match, *hit is the matching window and *now_secs the current time of
 * day in seconds.
 */
static bool
in_deny_creation_window(const KPartTimeWindow **hit, int *now_secs)
{
	struct pg_tm tm;
	fsec_t		fsec;
	int			tz;
	int			t;
	int			i;

	if (deny_creation_windows == NULL)
		return false;

	if (timestamp2tm(GetCurrentTimestamp(), &tz, &tm, &fsec, NULL,
					 log_timezone) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	t = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

	for (i = 0; i < deny_creation_windows->nwindows; i++)
	{
		const KPartTimeWindow *w = &deny_creation_windows->windows[i];
		bool		inside;

		if (w->start < w->end)
			inside = (t >= w->start && t < w->end);
		else					/* wraps around midnight */
			inside = (t >= w->start || t < w->end);

		if (inside)
		{
			*hit = w;
			*now_secs = t;
			return true;
		}
	}
	return false;
}

/* "schema.rel" or "rel", as written by the user. */
static const char *
rangevar_display_name(const RangeVar *rv)
{
	if (rv == NULL)
		return "?";
	if (rv->schemaname != NULL)
		return psprintf("%s.%s", rv->schemaname, rv->relname);
	return rv->relname;
}

/*
 * Raise an error if parsetree creates a new partition (CREATE [FOREIGN] TABLE
 * ... PARTITION OF, or ALTER TABLE ... ATTACH PARTITION) while the current
 * time falls inside a forbidden window.
 */
static void
check_partition_creation(Node *parsetree)
{
	RangeVar   *part = NULL;
	RangeVar   *parent = NULL;
	bool		attach = false;
	const KPartTimeWindow *hit;
	int			now_secs;
	char		sbuf[16],
				ebuf[16],
				nbuf[16];

	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			{
				/* CreateForeignTableStmt embeds a CreateStmt as first field */
				CreateStmt *cs = (CreateStmt *) parsetree;

				if (cs->partbound != NULL)
				{
					part = cs->relation;
					if (cs->inhRelations != NIL)
						parent = (RangeVar *) linitial(cs->inhRelations);
				}
			}
			break;
		case T_AlterTableStmt:
			{
				AlterTableStmt *ats = (AlterTableStmt *) parsetree;
				ListCell   *lc;

				/* Skip ALTER INDEX ... ATTACH PARTITION. */
#if PG_VERSION_NUM >= 140000
				if (ats->objtype != OBJECT_TABLE)
#else
				if (ats->relkind != OBJECT_TABLE)
#endif
					break;

				foreach(lc, ats->cmds)
				{
					AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);

					if (cmd->subtype == AT_AttachPartition)
					{
						part = ((PartitionCmd *) cmd->def)->name;
						parent = ats->relation;
						attach = true;
						break;
					}
				}
			}
			break;
		default:
			break;
	}

	if (part == NULL)
		return;

	if (!in_deny_creation_window(&hit, &now_secs))
		return;

	format_time_of_day(hit->start, sbuf, sizeof(sbuf));
	format_time_of_day(hit->end, ebuf, sizeof(ebuf));
	format_time_of_day(now_secs, nbuf, sizeof(nbuf));

	ereport(ERROR,
			(errcode(ERRCODE_PG_KPART_CREATION_DENIED),
			 attach ?
			 errmsg("attaching partition \"%s\" to table \"%s\" is not allowed at this time",
					rangevar_display_name(part), rangevar_display_name(parent)) :
			 errmsg("creation of partition \"%s\" of table \"%s\" is not allowed at this time",
					rangevar_display_name(part), rangevar_display_name(parent)),
			 errdetail("Partition creation is forbidden between %s and %s by pg_kpart.deny_partition_creation; current time is %s in time zone \"%s\".",
					   sbuf, ebuf, nbuf, pg_get_timezone_name(log_timezone)),
			 errhint("Retry outside of the forbidden time window.")));
}

/* ---- utility hook ------------------------------------------------------- */

static void
pg_kpart_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
#if PG_VERSION_NUM >= 140000
						bool readOnlyTree,
#endif
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	/* Never interfere with pg_upgrade restoring the schema. */
	if (deny_creation_windows != NULL && !IsBinaryUpgrade)
		check_partition_creation(pstmt->utilityStmt);

	if (prev_ProcessUtility_hook)
		prev_ProcessUtility_hook(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
								 readOnlyTree,
#endif
								 context, params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
								readOnlyTree,
#endif
								context, params, queryEnv, dest, qc);
}

/* ---- module init / fini ------------------------------------------------- */

void
_PG_init(void)
{
	GucContext context = PGC_USERSET;

	DefineCustomBoolVariable("pg_kpart.check_superuser",
							 "Apply the check to superusers as well.",
							 NULL,
							 &pg_kpart_check_superuser,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_kpart.nouser_change",
							 "Disallow non superusers to change the settings of the pg_kpart extension.",
							 NULL,
							 &pg_kpart_nouser_change,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	if (pg_kpart_nouser_change)
		context = PGC_SUSET;

	DefineCustomBoolVariable("pg_kpart.enabled",
							 "Enable enforcement of partition-key usage.",
							 NULL,
							 &pg_kpart_enabled,
							 true,
							 context,
							 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("pg_kpart.min_partitions",
							"Minimum number of leaf partitions before the check applies.",
							NULL,
							&pg_kpart_min_partitions,
							2, 1, INT_MAX,
							context,
							0,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("pg_kpart.message_level",
							 "Message level emitted when a full partition scan is detected.",
							 NULL,
							 &pg_kpart_message_level,
							 ERROR,
							 message_level_options,
							 context,
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
							   context,
							   GUC_LIST_INPUT,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("pg_kpart.whitelisted",
							   "Comma-separated partitioned tables exempt from the "
							   "check (sub-partitions included).",
							   "Ignored when pg_kpart.blacklisted is set.",
							   &pg_kpart_whitelist,
							   "",
							   context,
							   GUC_LIST_INPUT,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("pg_kpart.deny_partition_creation",
							   "Comma-separated time-of-day windows during which "
							   "partition creation is forbidden.",
							   "Each window is HH:MM[:SS]-HH:MM[:SS], start included, "
							   "end excluded, evaluated in log_timezone; a window may "
							   "wrap around midnight (e.g. 22:00-06:00). Applies to "
							   "CREATE TABLE ... PARTITION OF and ALTER TABLE ... "
							   "ATTACH PARTITION, for all roles. Empty means no "
							   "restriction.",
							   &pg_kpart_deny_creation,
							   "",
							   context,
							   GUC_LIST_INPUT,
							   check_deny_creation,
							   assign_deny_creation,
							   NULL);

#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("pg_kpart");
#else
	EmitWarningsOnPlaceholders("pg_kpart");
#endif

	prev_planner_hook = planner_hook;
	planner_hook = pg_kpart_planner;

	prev_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = pg_kpart_ProcessUtility;
}

void
_PG_fini(void)
{
	planner_hook = prev_planner_hook;
	ProcessUtility_hook = prev_ProcessUtility_hook;
}

