-- pg_kpart regression test
-- Run against a server started with: session_preload_libraries = 'pg_kpart'
-- The test role here is assumed to be a superuser, so we enable the check for
-- superusers. In production with non-superusers this is not needed.

CREATE EXTENSION IF NOT EXISTS pg_kpart;

SET pg_kpart.check_superuser = on;
SET pg_kpart.message_level = error;
SET pg_kpart.min_partitions = 2;

-- ========================================================================
-- RANGE partitioning (with a multi-level sub-partition)
-- ========================================================================
CREATE TABLE measurement (city_id int, logdate date, peaktemp int)
  PARTITION BY RANGE (logdate);
CREATE TABLE m_2024 PARTITION OF measurement
  FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE m_2025 PARTITION OF measurement
  FOR VALUES FROM ('2025-01-01') TO ('2026-01-01')
  PARTITION BY RANGE (logdate);
CREATE TABLE m_2025_h1 PARTITION OF m_2025
  FOR VALUES FROM ('2025-01-01') TO ('2025-07-01');
CREATE TABLE m_2025_h2 PARTITION OF m_2025
  FOR VALUES FROM ('2025-07-01') TO ('2026-01-01');
CREATE TABLE m_2026 PARTITION OF measurement
  FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');

\echo 'R1: no predicate on partition key            -> EXPECT ERROR'
SELECT * FROM measurement WHERE city_id = 5;

\echo 'R2: equality on partition key (1 partition)   -> EXPECT OK'
SELECT * FROM measurement WHERE logdate = DATE '2024-03-01';

\echo 'R3: range on key spanning 2 of 4 leaves       -> EXPECT OK'
SELECT * FROM measurement WHERE logdate >= DATE '2024-06-01'
                            AND logdate <  DATE '2025-03-01';

\echo 'R4: full scan, no predicate                   -> EXPECT ERROR'
SELECT count(*) FROM measurement;

\echo 'R5: parameterized key, generic plan (runtime) -> EXPECT OK'
SET plan_cache_mode = force_generic_plan;
PREPARE rq(date) AS SELECT * FROM measurement WHERE logdate = $1;
EXPLAIN (COSTS OFF) EXECUTE rq(DATE '2025-08-01');
SET plan_cache_mode = auto;

\echo 'R6: sub-partitioned table queried directly    -> EXPECT ERROR (2 parts)'
SELECT * FROM m_2025 WHERE city_id = 7;

\echo 'R7: UPDATE without partition key              -> EXPECT ERROR'
UPDATE measurement SET peaktemp = 0 WHERE city_id = 3;

\echo 'R8: DELETE without partition key              -> EXPECT ERROR'
DELETE FROM measurement WHERE city_id = 3;

\echo 'R9: range on key spanning 2 of 4 leaves       -> EXPECT OK'
UPDATE measurement SET peaktemp = 0 WHERE logdate >= DATE '2024-06-01'
                            AND logdate <  DATE '2025-03-01';

\echo 'R10: range on key spanning 2 of 4 leaves       -> EXPECT OK'
DELETE FROM measurement WHERE logdate >= DATE '2024-06-01'
                            AND logdate <  DATE '2025-03-01';

-- ========================================================================
-- HASH partitioning (only equality / IN prunes)
-- ========================================================================
CREATE TABLE evt (id bigint, payload text) PARTITION BY HASH (id);
CREATE TABLE evt_0 PARTITION OF evt FOR VALUES WITH (MODULUS 4, REMAINDER 0);
CREATE TABLE evt_1 PARTITION OF evt FOR VALUES WITH (MODULUS 4, REMAINDER 1);
CREATE TABLE evt_2 PARTITION OF evt FOR VALUES WITH (MODULUS 4, REMAINDER 2);
CREATE TABLE evt_3 PARTITION OF evt FOR VALUES WITH (MODULUS 4, REMAINDER 3);

\echo 'H1: equality on hash key                      -> EXPECT OK'
EXPLAIN (COSTS OFF) SELECT * FROM evt WHERE id = 42;

\echo 'H2: IN-list on hash key                       -> EXPECT OK'
EXPLAIN (COSTS OFF) SELECT * FROM evt WHERE id IN (42, 99);

\echo 'H3: filter on non-key column                  -> EXPECT ERROR'
SELECT * FROM evt WHERE payload = 'x';

\echo 'H4: range on hash key (cannot prune)          -> EXPECT ERROR'
SELECT * FROM evt WHERE id > 100;

\echo 'H5: parameterized equality, generic plan      -> EXPECT OK (runtime)'
SET plan_cache_mode = force_generic_plan;
PREPARE hq(bigint) AS SELECT * FROM evt WHERE id = $1;
EXPLAIN (COSTS OFF) EXECUTE hq(7);
SET plan_cache_mode = auto;

-- ========================================================================
-- LIST partitioning
-- ========================================================================
CREATE TABLE acct (region text, balance numeric) PARTITION BY LIST (region);
CREATE TABLE acct_emea PARTITION OF acct FOR VALUES IN ('eu', 'me', 'africa');
CREATE TABLE acct_amer PARTITION OF acct FOR VALUES IN ('na', 'sa');
CREATE TABLE acct_apac PARTITION OF acct FOR VALUES IN ('apac');

\echo 'L1: equality on list key                      -> EXPECT OK'
SELECT * FROM acct WHERE region = 'na';

\echo 'L2: no predicate on list key                  -> EXPECT ERROR'
SELECT count(*) FROM acct;

-- ========================================================================
-- Modes
-- ========================================================================
\echo 'M1: warning mode -> WARNING, query proceeds'
SET pg_kpart.message_level = warning;
SELECT count(*) FROM measurement;

\echo 'M2: disabled -> OK'
SET pg_kpart.message_level = error;
SET pg_kpart.enabled = off;
SELECT count(*) FROM measurement;
SET pg_kpart.enabled = on;

\echo 'M3: SQLSTATE FS001 is trappable'
DO $$
BEGIN
  PERFORM count(*) FROM measurement;
EXCEPTION WHEN SQLSTATE 'FS001' THEN
  RAISE NOTICE 'trapped pg_kpart violation via SQLSTATE FS001';
END $$;

-- ========================================================================
-- Blacklist / whitelist scoping
-- ========================================================================
CREATE TABLE acct2 (region text) PARTITION BY LIST (region);
CREATE TABLE acct2_na PARTITION OF acct2 FOR VALUES IN ('na');
CREATE TABLE acct2_eu PARTITION OF acct2 FOR VALUES IN ('eu');

\echo 'B1: blacklist=measurement (whitelist set but ignored)'
SET pg_kpart.blacklisted = 'measurement';
SET pg_kpart.whitelisted = 'measurement';
\echo '   measurement full scan        -> EXPECT ERROR'
SELECT count(*) FROM measurement;
\echo '   m_2025 sub-partition direct  -> EXPECT ERROR (ancestor blacklisted)'
SELECT count(*) FROM m_2025;
\echo '   acct2 full scan              -> EXPECT OK (not blacklisted)'
SELECT count(*) FROM acct2;

\echo 'W1: whitelist=measurement (all except it)'
RESET pg_kpart.blacklisted;
SET pg_kpart.whitelisted = 'measurement';
\echo '   measurement full scan        -> EXPECT OK (whitelisted)'
SELECT count(*) FROM measurement;
\echo '   m_2025 sub-partition direct  -> EXPECT OK (ancestor whitelisted)'
SELECT count(*) FROM m_2025;
\echo '   acct2 full scan              -> EXPECT ERROR (not whitelisted)'
SELECT count(*) FROM acct2;

\echo 'S1: schema-qualified name'
RESET pg_kpart.whitelisted;
SET pg_kpart.blacklisted = 'public.acct2';
\echo '   acct2 full scan              -> EXPECT ERROR'
SELECT count(*) FROM acct2;
\echo '   measurement full scan        -> EXPECT OK'
SELECT count(*) FROM measurement;
RESET pg_kpart.blacklisted;

