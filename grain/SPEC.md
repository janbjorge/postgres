# The starting spec

> As jb gave it, 2026-10-19, kept word for word. It was the research input. Where the work since has overtaken it: its freshness mechanism (a) is unsound on master (ALTERNATIVES.md §1); its lead candidate D1 is parked; the design being built is DESIGN.md §3. Several source references point at REL_18 and moved on master (RESEARCH.md §11).

---

# Spec: exact time-range aggregates on append-mostly tables in PostgreSQL

Status: draft, research input. Written 2026-10-19.
Audience: an agent working in the PostgreSQL source tree.

Markers used below:

- **[verified]**: read in PostgreSQL docs or source on 2026-10-19; link in section 12.
- **[inferred]**: reasoning from verified facts, not tested. Prove or refute it.
- **[to verify]**: a claim I did not check.

## 1. Problem

Many applications keep event or measurement tables with this shape:

- Rows are appended roughly in time order.
- Old rows are rarely updated. The newest rows ("open" buckets) may be updated
  often.
- Rows leave only through retention, as `DELETE` of the oldest rows.

The queries come in three forms.

1. **Range aggregate.** `SELECT agg(x) ... WHERE t >= t0 AND t < t1`, optionally
   with `GROUP BY` one to three low-cardinality keys (status, type, tenant). The
   aggregate is any of `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`, `stddev`,
   `bool_or`, ... (section 3 defines which ones qualify).
2. **Bucketed series.** The same, grouped by a time bucket:
   `GROUP BY date_bin('1 hour', t, origin)`.
3. **Window over the series.** Window functions on top of form 2: running
   totals since the first row, moving averages, period-over-period deltas.

A range can be a minute, a day, or the whole history ("since the dawn of
time").

Today every native access path costs at least O(rows in range): sequential
scan, btree range scan, BRIN bitmap scan and index-only scan all visit each
matching heap tuple or index entry. A one-year range over a high-volume table
reads the whole year, even though most of the data has not changed in months.

**Goal:** answer forms 1–3 exactly, with the result a sequential scan would
return under the same snapshot (see section 3.3 for floating point), at a cost
close to O(number of block ranges + rows in the "unclean" part of the range)
instead of O(rows).

## 2. Motivating example: PgQueuer

PgQueuer (Python job queue on PostgreSQL) has two tables involved.

- `pgqueuer_log`: one row per job state transition (queued, picked,
  successful, exception, ...). Columns include `created timestamptz`,
  `entrypoint`, `status`, `priority` and an `aggregated boolean` flag.
- `pgqueuer_statistics`: per-second buckets. A periodic job folds unaggregated
  log rows into buckets with `INSERT ... ON CONFLICT DO UPDATE SET count =
  count + EXCLUDED.count` and sets `aggregated = TRUE` on the log rows.

Query shapes:

- Prometheus-style "last 5 minutes" sums.
- Dashboard sums over 1 h, 6 h and 24 h.
- Per-minute time series.
- Job-duration percentiles from the log.
- Users want long-term totals (issue #622, discussion #621).

Observed costs:

- No statistics index leads with `created`, so every window read is a
  sequential scan.
- The log table has a partial index `USING btree ((1)) WHERE (NOT aggregated)`.
  Its predicate references `aggregated`, and predicate attributes always block
  HOT [verified: index AM docs on `amsummarizing`; relcache.c adds predicate
  attributes to the HOT-blocking set]. Every flag flip is therefore a non-HOT
  update, which inserts into every other index on the log table [inferred].

PgQueuer is one instance of the general problem. The design must not depend on
it. With a general solution, the statistics table and its upsert become
optional: the log table could answer the same questions directly.

## 3. Which aggregates qualify

### 3.1 The contract already exists: partial aggregation

PostgreSQL defines exactly the property needed. An aggregate "can support
partial aggregation": its transition function runs over subsets of the input
independently, and a **combine function** merges the subset states "to produce
the same state value that would have resulted from scanning all the input in a
single operation" [verified: docs 36.12.4]. States declared as `internal` also
need serialize/deserialize functions to a `bytea` blob [verified].

Parallel aggregation and partitionwise aggregation already use this. The docs
even say this mode "might also be used for purposes such as combining
aggregations over local and remote tables; but that is not implemented yet"
[verified].

**This spec's core idea:** a per-block-range summary stores the aggregate's
*serialized partial state* for its block range. A query combines the stored
states of covered ranges with partial states computed from the heap for the
rest, then runs the final function. To the planner, each block range is a
micro-partition with a precomputed partial aggregate.

### 3.2 Eligibility

An aggregate is eligible for a stored summary iff:

1. It has a combine function (and serial/deserial functions if its state is
   `internal`). The agent should list built-ins with
   `SELECT aggfnoid FROM pg_aggregate WHERE aggcombinefn <> 0`.
2. Its state is **bounded in size**, independent of the number of rows.
3. It is not order-sensitive. The docs state that combine functions are
   "usually impossible" for aggregates sensitive to input order [verified].
4. It has no `DISTINCT`, and no `FILTER` beyond what the summary's grouping
   keys can express.

| Class | Examples | Eligible |
|---|---|---|
| Distributive: the state is the answer | `count`, `sum`, `min`, `max`, `bool_and`/`bool_or`, `bit_and`/`bit_or` | yes |
| Algebraic: fixed-size state, finalised at the end | `avg` (sum, count), `stddev`/`variance` (count, sum, sum of squares or equivalent), `corr`, `regr_*` | yes, if bounded state and a combine function [to verify per aggregate] |
| Holistic: state grows with the data | `percentile_cont`/`percentile_disc`, `mode`, `count(DISTINCT x)`, `array_agg`, `string_agg` | no: ordered-set aggregates sort their input in the final function [verified: docs 36.12.3]; the others are unbounded [inferred] |

`MIN`/`MAX` are in scope. `planagg.c` already rewrites `MIN`/`MAX` into an
index-ordered `LIMIT 1` [verified], but only when there is an index on the
aggregated column itself [inferred from what the rewrite does]. `max(latency)
WHERE t in range` gets no help today; a range summary would help.

### 3.3 Floating point

Combining partial `float4`/`float8` states adds them in a different order from
a sequential scan, so the last bits can differ. Parallel aggregation already
behaves this way [inferred]. The exactness requirement is therefore: identical
to a sequential scan for exact types (integer types, `numeric`, timestamps), and
the same guarantee as parallel aggregation for floating point.

### 3.4 Window functions

Window functions are not aggregates over the table. They run over the rows of
a query result. Three cases:

- **Windows over the bucketed series (form 3)**, e.g.
  `sum(n) OVER (ORDER BY bucket ROWS 23 PRECEDING)` over hourly buckets.
  - In scope, indirectly: the summaries make the grouped input (form 2) cheap,
    and the existing WindowAgg runs over a few rows.
  - Nothing new is needed in the window machinery.
- **Running totals since the first row** (prefix aggregates).
  - In scope. With flat per-range summaries the cost is O(ranges).
  - A second, hierarchical summary level (section 6, D1 phase 2) makes it
    O(log ranges).
- **Per-row windows over raw rows**, e.g.
  `count(*) OVER (ORDER BY t RANGE '1 hour' PRECEDING)` for every event.
  - Out of scope: the output is O(rows).
  - The existing moving-aggregate mode (inverse transition functions) already
    makes these linear [verified: docs 36.12.1].

### 3.5 Bucket alignment

For form 2, a block range can be answered from its summary only if all of its
rows fall into one bucket, i.e. its time minmax lies inside one bucket.
Otherwise it is read from the heap [inferred].

So the gain depends on bucket width versus the time span one block range
covers:

- Hourly buckets over a table where a block range spans a few minutes: almost
  every range qualifies.
- Per-second buckets: almost none do.

The agent should measure this trade-off and its interaction with
`pages_per_range`.

## 4. What PostgreSQL provides today

| Fact | Status |
|---|---|
| Partial aggregation: combine, serialize and deserialize functions; used by parallel and partitionwise aggregation. | verified (docs 36.12.4, CREATE AGGREGATE) |
| Moving-aggregate mode: inverse transition functions let a window frame drop rows from the running state. | verified (docs 36.12.1) |
| Index entries carry no visibility information; index-only scans consult the visibility map (VM) and visit the heap for pages not marked all-visible. | verified (docs 11.9) |
| The index AM API has `aminsert`, `ambulkdelete` and `amvacuumcleanup`, but no per-row delete callback. Deletes reach an index only through VACUUM. | verified (`amapi.h`, `IndexAmRoutine`) |
| The index-only scan race argument: an insert clears the VM bit before updating the index, so a scan that sees a new TID also sees the cleared bit. Deletes are not serialized with the VM read, but are safe because the deleted tuple stays visible to snapshots that predate the delete. | verified (`nodeIndexonlyscan.c`, comment before `VM_ALL_VISIBLE`) |
| Heap insert, update and delete clear the VM bit under the buffer lock (`visibilitymap_clear_locked`). | verified (`heapam.c` call sites) |
| BRIN keeps per-block-range summaries. They are lossy, widened by inserts (`brininsert` → `add_values_to_range`) and never shrunk by deletes. New ranges stay unsummarized until VACUUM, `autosummarize`, or `brin_summarize_new_values`. `brin_desummarize_range` exists. | verified (docs ch. BRIN, `brin.c`) |
| BRIN opclasses: minmax, minmax-multi, inclusion, bloom. `timestamptz_minmax_multi_ops` exists from PG 14. | verified (docs, `pg_opclass.dat` REL_14) |
| A BRIN range summary is a set of `Datum`s per attribute (`BrinValues.bv_values`), with a serialize callback for complex summaries. | verified (`brin_tuple.h`) |
| Summarizing indexes (BRIN) do not block HOT (PG 16+). Predicate columns always block HOT. INCLUDE (non-key) columns block HOT. | verified (docs 66.7, PG 16 release notes, index AM docs, relcache.c comment) |
| Since PG 13, inserts alone can trigger autovacuum, so append-only tables get VM bits set. | verified (PG 13 release notes) |
| `planagg.c` rewrites `MIN`/`MAX` into index-ordered `LIMIT 1` subqueries. This is a precedent for planner-level aggregate rewrites. | verified (`planagg.c`, `preprocess_minmax_aggregates`) |
| Extension hooks: `create_upper_paths_hook` and `planner_hook`; CustomScan providers (documented as experimental); index AMs through `CREATE ACCESS METHOD`. | verified (`planner.h`, docs ch. 60) |
| `date_bin(interval, timestamptz, timestamptz)` is IMMUTABLE from PG 14. `date_trunc(text, timestamptz)` is STABLE. `date_trunc(text, timestamptz, text)` is IMMUTABLE from PG 16. | verified (`pg_proc.dat` REL_14/16/18) |
| A unique constraint on a partitioned table cannot use expressions and must include the partition key. | verified (docs 5.12) |
| Partitionwise aggregate exists, off by default. | verified (runtime-config-query) |
| Btree skip scan exists in PG 18. | verified (PG 18 release notes) |
| Statement-level AFTER triggers can read transition tables (`REFERENCING NEW TABLE`). | verified (CREATE TRIGGER docs) |
| `REFRESH MATERIALIZED VIEW` recomputes the whole view; no incremental maintenance in core. | verified (docs) |

## 5. Constraints any design must respect

1. **MVCC exactness.**
   - The answer must equal a sequential scan under the same snapshot, at every
     isolation level (floating point: section 3.3).
   - A summary built from tuples can include in-progress, aborted or deleted
     tuples. It is trustworthy only if something proves that no such tuple
     exists in the summarized region.
2. **No delete notification.**
   - The index AM never sees a delete until VACUUM.
   - An update that is not HOT inserts the new version through `aminsert`; the
     old version's death is not reported [verified for the API shape;
     consequence inferred].
3. **No new write hot spot.**
   - Inserts into a time-ordered table already concentrate on the newest
     pages.
   - Anything that updates shared state on every insert (for example ancestors
     in a btree) serializes writers [inferred; check against the nbtree
     README's Lehman–Yao design].
4. **Crash safety and replication.**
   - Summaries must be WAL-logged and correct after crash recovery and on hot
     standby.
   - Index-only scans work on standbys, so VM-based reasoning carries over
     [to verify].
5. **SERIALIZABLE.**
   - A read answered from summaries must take SIREAD locks covering everything
     it logically read; at minimum, the heap pages or the relation
     [inferred; check `predicate.c` and how index-only and bitmap scans lock].
6. **Bounded size.**
   - Number of aggregates × number of groups × state size must fit an index
     tuple [to verify the BRIN tuple size limit].
   - Overflow degrades to "not aggregatable", never to a wrong answer.
7. **Opt-in.** No behaviour change for existing indexes, tables or aggregates.

## 6. Candidate designs

### D1 (lead candidate): visibility-gated partial-aggregate summaries per block range

Summaries per block range, trusted only where the visibility map proves nothing
changed. This is the index-only-scan idea applied to partial aggregate states.

**Declaration.** The user declares which aggregates (and optional group keys)
an index maintains. Syntax is open, for example a storage parameter or opclass
options listing `count(*)`, `sum(n)`, `max(latency_ms)`, `GROUP BY status`.

**Summary contents** per range:

- the existing minmax of the range key (the time column);
- one serialized partial state per declared aggregate, per group key value, for
  a capped number of groups. On overflow the range is marked not aggregatable.

**Trust rule** [inferred; the core of the work]. A range summary may replace
reading the range iff all three hold:

1. the summary is flagged exact;
2. every heap page in the range is all-visible in the VM at read time;
3. the summary reflects the current contents of those pages.

(3) is the hard part. A page can be modified (VM bit cleared) and then vacuumed
(bit set again), and the bit alone does not prove the summary was recomputed.
Mechanisms to evaluate:

- **(a) VACUUM recomputes.** VACUUM recomputes the exact summary of any range in
  which it sets a VM bit, and orders the summary write and the VM-bit set so a
  reader never sees "all bits set" with a stale summary. Prove the ordering
  with the same rigour as the `nodeIndexonlyscan.c` comment.
- **(b) Generation counter.** A per-range generation number, bumped wherever a
  VM bit in the range is cleared. That needs a core hook at the
  `visibilitymap_clear*` call sites. The summary stores the generation it was
  computed at; readers compare.
- **(c) Separate fork.** Keep the exact summaries in a new relation fork
  maintained by the heap/VACUUM side instead of the index AM.

**Write path:**

- `aminsert` must not fold new rows into exact states, because the inserting
  transaction may abort. It only marks the range inexact, or keeps today's
  lossy minmax behaviour.
- Exact states come only from VACUUM or an explicit summarize function.

**Read path:**

- Reuse the existing partial-aggregation plan shape: Partial Aggregate inputs
  feeding a Finalize Aggregate.
- A new scan emits stored partial states for covered, trusted, bucket-aligned
  ranges (section 3.5). Edge ranges, untrusted ranges and unsummarized ranges
  go through a normal block-restricted heap scan and Partial Aggregate.
- Both feed the existing Finalize Aggregate. Window functions (form 3) sit on
  top unchanged.
- Entry points: a planner rewrite in the spirit of `planagg.c`, or upper paths
  via `create_upper_paths_hook` / CustomScan.
- Expected cost: O(ranges) plus heap reads of the edges and the dirty tail
  [inferred].

**Phase 2:** a hierarchical level that summarizes runs of ranges by combining
their states. This makes whole-history and running-total queries O(log ranges).

### D2: counted btree (subtree aggregates in internal pages)

Likely rejected; confirm and record why.

- Stored aggregates would include invisible tuples, since btree entries carry
  no visibility information.
- Updating ancestors on every insert conflicts with nbtree concurrency.
- Deletes arrive only at VACUUM time.

The agent should confirm against the nbtree README and write down the reasons,
so the idea is not retried later.

### D3: core-maintained rollup

This is incremental maintenance of `agg(x) ... GROUP BY bucket`, done at write
time.

- Deltas per statement are applied to rollup rows that are ordinary MVCC tuples,
  so they are correct by construction.
- `count`/`sum` deltas add up.
- `min`/`max` cannot be maintained under delete without rescanning, because
  they have no inverse. That limits D3 for retention deletes [inferred].
- Cost moves to write time: row-lock contention on the open bucket.
- A SQL version of this exists today (B2 below).

Evaluate it against D1 on write overhead, contention and aggregate coverage.

### D4: smaller improvements to existing paths

Look for cheap wins and report any that hold up, for example:

- An index-only `COUNT(*)` over a btree range that counts entries on
  all-visible pages without forming tuples [idea, unverified].
- BRIN cost estimates that know about fully-covered ranges.

## 7. Baselines (no core change), to measure first

- **B0.** Current state: sequential scan or the existing btree.
- **B1.** BRIN (`timestamptz_minmax_multi_ops`) on the time column, plus
  writer-side bucketing with `date_bin` (fewer, coarser rows).
- **B2.** Statement-level `AFTER INSERT` trigger with `REFERENCING NEW TABLE`
  upserting `count`/`sum`/`max` into rollup rows. Shard the counters (an extra
  shard column, combined at read) to spread row-lock contention [pattern
  inferred, not measured].
- **B3.** Multi-resolution rollups (1 s / 1 min / 1 h) with periodic compaction
  of old fine rows into coarse buckets.

A core design is worth pursuing only if it clearly beats B1–B3 on read latency
for long ranges without losing on write throughput.

## 8. Requirements

**Functional:**

- Correct results (section 3.3) under READ COMMITTED, REPEATABLE READ and
  SERIALIZABLE.
- Correct after crash recovery and on hot standby.
- Correct under concurrent insert, update, delete, abort, VACUUM, HOT pruning,
  freezing and relation truncation.
- Range key types: `timestamptz`, `timestamp`, `bigint`, `integer`.
- Aggregates: every built-in that passes section 3.2, at minimum `count(*)`,
  `count(col)`, `sum`, `min`, `max`, `avg`, `bool_and`/`bool_or`. User-defined
  aggregates qualify by the same rule.
- Query forms 1–3 from section 1.

**Non-functional (agent measures and reports):**

- Insert throughput change.
- WAL volume change.
- Summary/index size per declared aggregate.
- VACUUM time change.
- Query latency for windows of 1 min, 1 h, 1 day, 30 days and all history, for
  each of forms 1–3.

**Scope:**

- Target PostgreSQL master.
- No third-party extensions.
- A prototype may be an extension that uses only core APIs, as a step toward a
  core patch. List every core change it cannot avoid (for example a VM-clear
  hook).

## 9. Deliverables, in order

1. **Design note:**
   - the chosen design and its invariants;
   - a proof sketch of the trust rule, written against the index-only-scan
     argument;
   - an enumeration of every race (insert, update, delete, abort, VACUUM
     setting VM bits, truncation, freeze, crash, standby replay), each with its
     resolution.
2. **Go/no-go on D1 vs D3**, with reasons, before writing much code.
3. **Prototype.** Start with `count`/`sum`/`min`/`max` and no group keys, then
   group keys, then algebraic aggregates.
4. **Tests:**
   - regression tests comparing every eligible aggregate against a sequential
     scan under the same snapshot;
   - isolation tests (`src/test/isolation`), one per race in the design note;
   - TAP tests for crash and standby.
5. **Benchmarks:**
   - pgbench custom scripts on synthetic append-only data at 10M, 100M and 1B
     rows, with a realistic retention `DELETE` at the old end and an open-bucket
     update pattern at the new end;
   - every number taken from actual output, compared against B0–B3.
6. **Short report** with the numbers and a recommendation.

## 10. Non-goals

- Approximate answers (sketches, sampling, `reltuples`).
- Holistic aggregates: percentiles, `mode`, `count(DISTINCT)`, `array_agg`,
  `string_agg`.
- Per-row window functions over raw rows (output is O(rows)).
- Columns uncorrelated with physical order.

## 11. Open questions

- Which freshness mechanism, (a), (b) or (c), is provably correct and cheapest?
- How should aggregates be declared on the index, and how does the planner
  match a query's aggregates and `FILTER` clauses to them?
- Can grouped summaries stay bounded for realistic group cardinality, and what
  is the right overflow behaviour?
- Could inverse transition functions let VACUUM subtract removed rows instead of
  recomputing a range? Probably not: by the time `ambulkdelete` runs, the dead
  tuples' values are likely gone [inferred]. Confirm.
- What SIREAD locking does a summary-answered read need?
- Should this be a BRIN opclass extension, a BRIN AM change, or a new AM?
- How do `autosummarize`, `brin_desummarize_range` and the existing lossy
  summaries interact with exact ones?
- Can VACUUM's extra cost (recomputing states for ranges it touches) stay
  proportional to pages it already visits?
- Prior art: search pgsql-hackers archives for aggregate summaries on BRIN,
  counted btree, partial-aggregate pushdown, and incremental view maintenance,
  and record what was tried and why it stalled.

## 12. Sources (checked 2026-10-19)

PostgreSQL source, official GitHub mirror (line numbers as of that date):

- `src/include/access/amapi.h` (REL_18_STABLE), `IndexAmRoutine` members, L280
  (`amsummarizing`), L293–318:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/include/access/amapi.h
- `src/backend/executor/nodeIndexonlyscan.c`, VM race comment L129–160:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/executor/nodeIndexonlyscan.c
- `src/backend/access/heap/heapam.c`, `visibilitymap_clear_locked` call sites
  (e.g. L2151, L3118, L4288):
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/access/heap/heapam.c
- `src/backend/access/brin/brin.c`, `brininsert` L344, `add_values_to_range`
  L224:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/access/brin/brin.c
- `src/include/access/brin_tuple.h`, `BrinValues` L29–38:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/include/access/brin_tuple.h
- `src/backend/utils/cache/relcache.c`, predicate and INCLUDE attributes block
  HOT, L5443–5480:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/utils/cache/relcache.c
- `src/backend/optimizer/plan/planagg.c`, `preprocess_minmax_aggregates` L73:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/optimizer/plan/planagg.c
- `src/include/optimizer/planner.h`, `create_upper_paths_hook` L33–38:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/include/optimizer/planner.h
- `src/include/catalog/pg_proc.dat`, `date_trunc` / `date_bin` volatility on
  REL_14/15/16/17/18:
  https://github.com/postgres/postgres/blob/REL_18_STABLE/src/include/catalog/pg_proc.dat
- `src/include/catalog/pg_opclass.dat` (REL_14_STABLE), BRIN timestamptz
  opclasses:
  https://github.com/postgres/postgres/blob/REL_14_STABLE/src/include/catalog/pg_opclass.dat

PostgreSQL documentation:

- User-defined aggregates (moving-aggregate mode 36.12.1, ordered-set 36.12.3,
  partial aggregation 36.12.4): https://www.postgresql.org/docs/18/xaggr.html
- CREATE AGGREGATE: https://www.postgresql.org/docs/18/sql-createaggregate.html
- BRIN: https://www.postgresql.org/docs/18/brin.html
- Index-only scans and covering indexes:
  https://www.postgresql.org/docs/18/indexes-index-only-scans.html
- HOT: https://www.postgresql.org/docs/18/storage-hot.html
- Index AM API (`amsummarizing`): https://www.postgresql.org/docs/18/index-api.html
- Index AM functions (`ambulkdelete`):
  https://www.postgresql.org/docs/18/index-functions.html
- Custom scan providers: https://www.postgresql.org/docs/18/custom-scan.html
- Date/time functions (`date_bin`, `date_trunc`):
  https://www.postgresql.org/docs/18/functions-datetime.html
- Partitioning limits on unique constraints:
  https://www.postgresql.org/docs/18/ddl-partitioning.html
- Partitionwise aggregate GUC:
  https://www.postgresql.org/docs/18/runtime-config-query.html
- Transition tables: https://www.postgresql.org/docs/18/sql-createtrigger.html
- Materialized view refresh:
  https://www.postgresql.org/docs/18/sql-refreshmaterializedview.html
- Release notes: PG 13 (insert-triggered autovacuum)
  https://www.postgresql.org/docs/13/release-13.html, PG 16 (HOT with BRIN,
  immutable 3-arg `date_trunc`) https://www.postgresql.org/docs/16/release-16.html,
  PG 18 (btree skip scan) https://www.postgresql.org/docs/18/release-18.html

PgQueuer (for section 2), repository at commit fac207f:

- `pgqueuer/adapters/persistence/qb.py:788-827`: aggregation upsert.
- `pgqueuer/domain/schema/statistics.py:36-50`: unique bucket key.
- `pgqueuer/domain/schema/queue_log.py`: log table and its partial index.
- Issue: https://github.com/janbjorge/pgqueuer/issues/622
