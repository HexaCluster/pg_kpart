# pg_kpart

A PostgreSQL extension that **rejects queries which would scan every partition
of a partitioned table without a usable predicate on the partition key**. It
prevents accidental full-hierarchy scans caused by missing `WHERE`/`JOIN`
conditions on the partition key.

## How it works

`pg_kpart` installs a `planner_hook`. After the standard planner produces a
plan, the plan tree is walked. For each `Append` / `MergeAppend` sitting on a
partition hierarchy:

* If the node carries run-time pruning info (`part_prune_info` - e.g. a
  parameterized predicate on the partition key), the key **is** used and the
  query is allowed.
* Otherwise, if the number of surviving leaf partitions equals the *total*
  number of leaf partitions of the queried table, no pruning happened: the
  partition key was not restricted, and the configured message is raised.

A predicate that prunes only *some* partitions (e.g. a range on the key
spanning a few partitions) leaves fewer leaves than the total and is accepted.

## Build & install

Requires the PostgreSQL server development files (`pg_config` on `PATH`).

```sh
make
make install      # may need sudo
```

The functional part is a planner hook installed at library load, so the
library must be **preloaded**. Add to `postgresql.conf` (cluster-wide):

```
shared_preload_libraries = 'pg_kpart'   # then restart
```

or per-session / per-database without a restart:

```
session_preload_libraries = 'pg_kpart'
-- or:  ALTER DATABASE mydb SET session_preload_libraries = 'pg_kpart';
```

The `CREATE EXTENSION` step is optional and may be used just to register the extension in the `pg_catalog.pg_extension` table.

## Configuration (GUCs)

| GUC | Default | Description |
|-----|---------|-------------|
| `pg_kpart.enabled` | `on` | Master switch. |
| `pg_kpart.message_level` | `error` | `error`, `warning`, `notice`, `log`, … Use `warning` to audit before enforcing. |
| `pg_kpart.min_partitions` | `2` | Only check tables with at least this many leaf partitions. |
| `pg_kpart.check_superuser` | `off` | When `off`, superusers bypass the check. |
| `pg_kpart.blacklisted` | _(empty)_ | Comma-separated partitioned tables the check applies to, **and their sub-partitions**. When set, only these tables are checked and `pg_kpart.whitelisted` is ignored. Empty = all partitioned tables. |
| `pg_kpart.whitelisted` | _(empty)_ | Comma-separated partitioned tables exempt from the check, **and their sub-partitions**. Ignored when `pg_kpart.blacklisted` is set. |
| `pg_kpart.nouser_change` | `off` | When `on`, disallow non superuser roles to mofidy the settings of the pg_kpart extension. |
| `pg_kpart.deny_partition_creation` | _(empty)_ | Comma-separated time-of-day windows (`HH:MM[:SS]-HH:MM[:SS]`) during which creating a partition is forbidden. Empty = no restriction. See [Forbidding partition creation at given hours](#forbidding-partition-creation-at-given-hours). |

Names may be schema-qualified (`schema.table`); unqualified names are resolved
through the current `search_path`. Listing a partitioned table also covers any
sub-partitioned tables beneath it, so a sub-partition queried directly is
matched when one of its ancestors is listed. Membership is decided from the
partitioned table referenced in the query.

```sql
-- only police these two tables (and their sub-partitions)
ALTER SYSTEM SET pg_kpart.blacklisted = 'public.measurement, public.orders';

-- alternatively: police everything except a few audit tables
ALTER SYSTEM SET pg_kpart.whitelisted = 'public.audit_log';
SELECT pg_reload_conf();
```

```sql
-- roll out in audit mode first
ALTER SYSTEM SET pg_kpart.message_level = 'warning';
SELECT pg_reload_conf();
```

## Behavior

```sql
-- partition key is logdate
SELECT * FROM measurement WHERE city_id = 5;          -- ERROR: would scan all N partitions
SELECT * FROM measurement WHERE logdate = '2024-03-01';  -- OK (pruned to 1)
SELECT * FROM measurement WHERE logdate >= '2024-06-01'; -- OK (key restricted)
SELECT * FROM measurement WHERE logdate = $1;         -- OK (run-time pruning)
```
If table `m_2025` is a partition of table `measurement` and have sub-partioning too,
a full scan on `m_2025` will report an error.
```sql
SELECT * FROM m_2025 WHERE logdate = '2025-03-01';  -- OK (pruned to 1 sub-partition)
SELECT * FROM m_2025 WHERE logdate >= '2025-01-01' AND logdate < '2026-01-01'; -- ERROR: would scan all N sub-partitions
```

Violations use the custom SQLSTATE `FS001`, so applications can trap them:

```sql
DO $$
BEGIN
  PERFORM count(*) FROM measurement;
EXCEPTION WHEN SQLSTATE 'FS001' THEN
  RAISE NOTICE 'caught a full-partition-scan attempt';
END $$;
```

## Forbidding partition creation at given hours

Creating or attaching a partition takes a lock on the parent partitioned
table, which can disturb a busy application. `pg_kpart.deny_partition_creation`
forbids it during the given time-of-day windows, for example during business
hours:

```sql
ALTER SYSTEM SET pg_kpart.deny_partition_creation = '08:00-20:00';
SELECT pg_reload_conf();
```

* Several windows can be given, separated by commas:
  `'08:00-12:00, 14:00-18:30'`. Seconds are optional (`'07:59:30-20:00'`).
* Each window includes its start and excludes its end: with `08:00-20:00`,
  creation is refused from 08:00:00 up to 19:59:59 and allowed again at 20:00.
  `24:00` may be used as the end of a window.
* A window whose start is after its end wraps around midnight:
  `'22:00-06:00'` forbids creation during the night.
* The time is the server clock read in **`log_timezone`**, not in the session
  `TimeZone`, so that a user cannot get around the rule with `SET TimeZone`.
* Invalid values are rejected when set.

The following statements are refused inside a window, whatever the role,
**superusers included** (`pg_kpart.check_superuser` does not apply here):

* `CREATE TABLE ... PARTITION OF ...` (and `CREATE FOREIGN TABLE ... PARTITION OF`),
* `ALTER TABLE ... ATTACH PARTITION ...`.

This also covers statements run from functions, procedures or tools such as
pg_partman. Other DDL is not affected: creating a partitioned table
(`PARTITION BY`), `DETACH PARTITION`, `ALTER INDEX ... ATTACH PARTITION`.
`pg_upgrade` (binary upgrade mode) is never blocked, but a `pg_restore` that
recreates partitions during a window is.

The error uses the custom SQLSTATE `FS002`:

```
ERROR:  creation of partition "m_2027" of table "measurement" is not allowed at this time
DETAIL:  Partition creation is forbidden between 08:00 and 20:00 by pg_kpart.deny_partition_creation; current time is 10:42:07 in time zone "Europe/Paris".
HINT:  Retry outside of the forbidden time window.
```

Like the other settings, the GUC is `USERSET` unless `pg_kpart.nouser_change`
is enabled, in which case only a superuser can change or clear it.

## Notes & limitations

* The check is a guardrail, not a hard barrier. `pg_kpart.enabled` and
  `pg_kpart.message_level` are `USERSET` GUCs, so any role can run
  `SET pg_kpart.enabled = off` (or lower the message level) to bypass
  enforcement for its own session. Treat it as protection against accidental
  full scans, not as a security control against deliberate ones.
* Enforcement happens at plan time, inside the planner hook. A prepared
  statement or cached generic plan is checked only when it is actually
  (re)planned; repeated `EXECUTE` of an already-cached full-scan plan will not
  re-trigger the check.
* A predicate that happens to match *all* partitions (e.g. `logdate > '1900-01-01'`)
  is treated as a full scan and rejected - it is one, in practice.
* The check also covers `UPDATE`/`DELETE` and `EXPLAIN` (without `ANALYZE`),
  since those go through the planner too.
* The check is a guardrail, not a hard barrier. `pg_kpart.enabled` and
  `pg_kpart.message_level` are `USERSET` GUCs, so any role can run
  `SET pg_kpart.enabled = off` (or lower the message level) to bypass
  enforcement for its own session. Treat it as protection against accidental
  full scans, not as a security control against deliberate ones. If you want
  to disallow non superuser roles to mofidy the pg_kpart settings, enable
  `pg_kpart.nouser_change`.
* Enforcement happens at plan time, inside the planner hook. A prepared
  statement or cached generic plan is checked only when it is actually
  (re)planned; repeated `EXECUTE` of an already-cached full-scan plan will not
  re-trigger the check.
* Tested on PostgreSQL >= 14.

## Authors

- Gilles Darold

## License

This extension is free software distributed under the PostgreSQL License.

    Copyright (c) 2026 HexaCluster Corp

