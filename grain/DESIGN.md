# grain: design

The build target. Anything not marked as jb's is my proposal. The facts it rests on are RESEARCH.md; the designs it replaced are ALTERNATIVES.md.

**Live today** (2026-10-19): P1 proved the read path, 241.4x at 100M rows (NOTES.md P1). Patch 0001, the delete callback, is built and tested, uncommitted (NOTES.md M1.1). Branch `grain-proto`; next is the rest of M1 (§7).

## 1. What, and the one question

Exact `count`, `sum`, `min` and `max` per time bucket on an ordinary table, made fast by a new index type built into PostgreSQL itself (jb 2026-10-19: "The goal here is NOT yet another postgres extension, i want this new index tobe part of postgres").

**The example: orders and revenue** (jb 2026-10-19: "that sounds really good, lets do orders."). "Revenue per day for the last year", "orders in the last hour", "largest order today", on an `orders` table with `created_at`, `amount_cents` and `status`. It was chosen over the first case, a downloads log (jb: "could i create an 'index' on this table and the done_at? and get a super fast view of the download, say for the last 30 days."), because:
- the answers are money, so exactness visibly matters;
- it uses every part of the design: a partial index on paid orders, refunds as updates that leave it, and backfilled orders as late writes;
- group keys follow naturally (`country`, `currency`).

An API request log is the second example, for volume (mine).

**The one question** (mine): can a plain `GROUP BY` over days or hours of a large append-mostly table, such as revenue per day for a year, be answered exactly from a few hundred stored rows plus the last minute or two of rows, with no triggers and almost no cost on insert?

## 2. Decided and proposed

| | Who | Where |
|---|---|---|
| The problem, constraints and deliverables of SPEC.md | jb 2026-10-19 (the spec) | SPEC.md |
| Part of PostgreSQL, a built-in index type; not an extension | jb 2026-10-19 ("The goal here is NOT yet another postgres extension, i want this new index tobe part of postgres") | §6 |
| Buckets are small, so a changed or open bucket is rescanned; nothing is decremented | jb 2026-10-19 ("Assuming that these buckets are failir small, i find it hard to beleave that this is a problem? ... I refuse that this cant solved in a clean and perfrment way") | §3 |
| Look past index AMs and btrees, at table-level flags | jb 2026-10-19 ("index might not be right option, nor that we should lean on btree, we might need to consider one of the 100s of table flags to get what we need") | §3.2, the watermark |
| An index-like declaration on the time column, then plain queries | jb 2026-10-19 ("could i create an 'index' on this table and the done_at? and get a super fast view") | §3.1 |
| No triggers | jb 2026-10-19 ("I know we can do triggers, but i think most ppl. fear triggers, as drift other issues creep in.") | §6; ALTERNATIVES.md §7 |
| Not TimescaleDB-sized | jb 2026-10-19 ("TimescaleDB is relted, but i think TimescaleDB is overkill for loads of problmes that are in this is sphare.") | §8 |
| Route 2 (this design) over Route 1 (partitions) | jb 2026-10-19, answering "Go with Route 2?" with "lets start with some basic agg-functions, like sum, count, min, max?" | §3; ALTERNATIVES.md §3 |
| Route 1 dropped | jb 2026-10-19 ("Yes, drop it, i feel that partitions has to meny wierd quarks and details.") | ALTERNATIVES.md §3 |
| Benchmark sizes 1k to 10M, 100M maybe | jb 2026-10-19 ("lets, try a few order of magnitude, like 1k, 100k, 1M, 10M, meybe 100M?") | §7, M3 |
| Built to upstream's submission rules from the start | jb 2026-10-19 ("so we make sure we do not have todo loads of double work") | §7; AGENTS.md |
| The first aggregates: count, sum, min, max | jb 2026-10-19 (same) | §7, M1 |
| Sealed buckets behind a watermark, kept by a built-in index AM with a new delete callback | mine | §3, §6 |
| The watermark in the index's metapage; summaries and markers in two hidden heaps owned by the index, like TOAST | mine | §3.2 |
| Sealing by a function first, by autovacuum work items later | mine | §3.3, §7 |
| Levels in one index (`buckets = '1 minute, 1 hour, 1 day'`), each built from the finer one | jb 2026-10-19 ("looks interesing"), after P1 showed the open tail dominates | §3.1, §3.3 |
| `lag` optional, default one finest width | mine, after jb asked "what does that do, why did we need it?" | §3.1 |
| Syntax: key columns are time key then group keys; aggregated values go in `INCLUDE`; partial indexes allowed; group keys built after M2 | jb 2026-10-19 ("sounds good, lets foruc on the syntax for now") | §3.1, §7 |
| D3 in core is a no-go; D2 rejected | mine | ALTERNATIVES.md |
| The name `grain`, Grouped Range Aggregate INdex, after GIN and BRIN | jb 2026-10-19 ("lets try grain for now"); the folder and branch renamed after jb's "ok" the same day | §3.1, §6 |

## 3. The design: sealed buckets behind a watermark

### 3.1 The declaration

The syntax is settled before M1, so later features don't change what users write (jb 2026-10-19: "sounds good, lets foruc on the syntax for now").

```sql
CREATE INDEX [ CONCURRENTLY ] name ON table
  USING grain ( time_key [, group_key ...] )
  [ INCLUDE ( value_column [, ...] ) ]
  WITH ( buckets = 'width [, width ...]' [, origin = 'timestamptz'] [, lag = 'interval'] )
  [ WHERE predicate ];
```

The orders case:

```sql
CREATE INDEX orders_paid_grain ON orders
  USING grain (created_at) INCLUDE (amount_cents)
  WITH (buckets = '1 minute, 1 hour, 1 day')
  WHERE status = 'paid';

-- served by it, unchanged SQL (from M2):
SELECT date_bin('1 day', created_at, '2000-01-01'), count(*), sum(amount_cents), max(amount_cents)
FROM orders
WHERE status = 'paid' AND created_at >= now() - interval '365 days'
GROUP BY 1 ORDER BY 1;
```

- **Key columns: what rows are grouped by.**
  - The first is the time key: a plain column, `timestamptz` in M1, then `timestamp`, `bigint` and `integer` (M4).
  - Any further key columns are group keys, as in `(created_at, country, currency)`. Built after M2. Until then CREATE INDEX refuses them with a clear error, so their meaning can't change later.
  - Rows whose time key is NULL are in no bucket. The planner uses the index only for queries with a range on the time key, which excludes NULLs anyway.
- **`INCLUDE`: what gets aggregated.**
  - Plain columns only; PostgreSQL's `INCLUDE` accepts nothing else.
  - Each gets `count`, `sum`, `min` and `max`. Integer types in M1; `numeric` and float, with `avg` from `sum` and `count`, in M4.
  - Every bucket also keeps `count(*)`, so `INCLUDE` is optional.
- **`buckets`: the levels.** Required.
  - One width or several, finest first. Each must divide the next exactly, as 1 minute → 1 hour → 1 day does. Reads use the coarsest level that fits (NOTES.md P2).
  - Fixed-length widths only: no months or years, which `date_bin` refuses too. A day is 24 hours from `origin`, so with the default origin, a UTC day.
- **`origin`:** where buckets start. All levels share it. Default `2000-01-01 00:00+00` (mine).
- **`lag`:** how long a closed finest bucket waits before it is sealed, to absorb stragglers (§3.3). Default one finest width. It only trades marker churn against tail size; answers are exact at any value.
- **`WHERE`: a partial index.** Summaries cover matching rows only. The planner uses the index only when the query's `WHERE` implies the predicate, as for partial btrees. The delete callback already skips old rows outside it (NOTES.md M1.1).
- **Refused:** `UNIQUE`, expressions, and opclass, collation or ordering options on key columns. Unlogged, temp and catalog tables too (§5).
- **Changing options:**
  - `lag` can be changed with `ALTER INDEX ... SET` at any time.
  - `buckets` and `origin` take effect at the next `REINDEX`, as BRIN's `pages_per_range` does.
- **AM flags:** `amcanmulticol` and `amcaninclude` true; `amcanunique` false; `amsummarizing` true.

### 3.2 State

- **The watermark W** lives in the index's metapage, block 0, as BRIN keeps `pagesPerRange` in its own (`brin_page.h:64-70`).
  - Writers read it under a share lock on that buffer, always after they have locked the table. No relcache and no catalog are involved.
  - Only the sealer moves it, and only forward. The write is WAL-logged and not transactional: a sealer that fails later leaves W ahead, which is safe (§5).
  - Users can't set it.
  - This is the "table flag" jb asked about: one value per index.
- **Summaries:** a hidden heap owned by the index, as a TOAST table is owned by its table (`toasting.c:129`).
  - One row `(level, bucket, n, nn[], sum[], min[], max[])` per sealed bucket of each level, empty buckets included with `n = 0`. Keyed on `(level, bucket)`, so a read is one range scan per level.
  - **A bucket is sealed for a reader exactly when the reader's snapshot sees its row.** No separate "sealed up to" value is needed.
  - `nn`, `sum`, `min` and `max` hold one element per value column.
  - These are typed values, never serialized aggregate states. Those aren't stable across major versions, and a table of them would let users feed bytes to a deserialization function (RESEARCH.md §9.5). M4's `avg` keeps `sum` and `nn` the same way.
- **Dirty markers:** a second hidden heap of `(bucket)` rows at the finest level, insert-only. A marker also makes the coarser buckets containing it dirty.

Both hidden heaps are ordinary WAL-logged MVCC heaps, so crash safety, standbys, every snapshot's view and freezing by autovacuum come for free. They are dropped with the index.

### 3.3 Writers and the sealer

**Writers.** A write whose row's time key is before W (the bucket is sealed, or about to be) inserts a dirty marker for its bucket in the same transaction. Normal writes, stamped "now", are after W and do nothing. Who calls what:
- an insert, and the new version of an update: `aminsert`;
- a delete, and the old version of an update: the new delete callback (§6).

Markers are de-duplicated per statement.

**The sealer** (`pg_grain_seal(index)`), one at a time per index, under a self-conflicting lock on the index:
1. Write W = `date_bin(finest width, now() - lag, origin)` into the metapage.
2. Wait, with `WaitForLockers(heap, ShareLock)`, until every transaction that held a writer's lock before step 1 has ended. This is CREATE INDEX CONCURRENTLY phase 2's wait (RESEARCH.md §5). New writers are never blocked: they lock the table after step 1, so they read the new W.
3. In a new transaction, under one snapshot:
   - compute the finest buckets from the last one with a row up to W, and every finest bucket with a marker this snapshot sees;
   - fold them into each coarser level whose bucket is now complete or was dirty, so coarse rows come from finer rows and never from a table scan;
   - replace their summary rows;
   - delete those markers;
   - commit.

Later, autovacuum asks for sealing as a work item, as BRIN's autosummarize does (`brin.c:413`). That only decides when hours get sealed; correctness never depends on it (§4).

### 3.4 Readers

`pg_grain_buckets(index, from, to)` in M1, a function that reads under one snapshot. Per bucket in the window:
- a summary row is visible, the bucket lies fully inside the window, and no marker is visible: the summary row;
- otherwise (open, dirty, or a partial bucket at an edge of the window): aggregate its rows with a range scan on the time key. The user's own btree or BRIN on it serves this scan.

From M2, the planner does this for plain `GROUP BY date_bin(...)` queries (§7).

### 3.5 Orders, worked through

With `orders_paid_grain` from §3.1:
- **New orders** are paid "now", after W: no work on insert beyond comparing the key with W.
- **The sealer** adds, each minute, the minute that fell behind the lag, and folds a complete hour or day into its row.
- **Reads:**
  - revenue per day for a year: about 365 day rows, today's hour and minute rows, and the last minute or two of rows;
  - revenue per hour for 30 days: about 720 hour rows, then the same tail (NOTES.md P2).
- **A refund** sets `status = 'refunded'` on an order paid last week. The old version satisfied the predicate, so the delete callback writes a marker for its minute. The new version doesn't, so it adds nothing. Until the next seal, reads drop to that hour's minute rows and scan the one dirty minute.
- **A backfill** of last month's orders from another system marks each minute it touches. One seal recomputes them.
- **Deleting orders past the retention period** marks their minutes. The next seal recomputes them as empty. Rows of empty buckets below the oldest data can then be dropped: a bucket without a row is only scanned, and its scan finds nothing.

## 4. Why it is exact

Take a reader with snapshot S and a bucket B for which S sees a summary row and no marker. Let X be the transaction that wrote that row version, under its snapshot Sx.

1. **X's view is in S's view.** Everything committed before Sx committed before X did, and X committed before S was taken, so S sees it all.
2. **Any extra change carries a marker S sees.** Suppose S sees a change to B's rows that Sx didn't, made by a transaction T.
   - T committed after Sx was taken. Had T held its table lock when X's step 2 checked, step 2 would have waited for T to end, before Sx was taken. So T locked the table after that check, which came after W was written.
   - T read W from the metapage after taking that lock, so it saw a W past B's end, and it wrote a marker for B in the same transaction.
   - The marker is visible to S exactly when T's change is. That contradicts "no marker visible", so no such T exists.
   - If a later seal deleted that marker, that seal saw T, and its row is the one S sees: it is X. Apply the argument to it instead.
3. **Aborted writers** leave neither rows nor markers.
4. **Snapshots older than X** see an older row version, or none, which means a scan. The same argument holds for whichever row they see.
5. **Buckets with no visible row, with a marker, or at an edge** are scanned, which is exact by definition.

Nothing here depends on the visibility map, VACUUM, transaction-id bookkeeping or clocks. W is computed from `now()`, but correctness needs only that W moves forward and that writers compare their row's key against it.

## 5. Races and their tests

| Event | Resolution | Test (M1 unless marked) |
|---|---|---|
| Late insert into a sealed bucket: committed, in progress, aborted | marker in the same transaction (§4.2, §4.3) | isolation `late-insert` |
| Delete or update of a row in a sealed bucket | delete callback writes a marker (§6) | isolation `delete`, `update` |
| Update moving a row between buckets | markers for both buckets | regress |
| Update touching no indexed column | no index call (HOT, `TU_None`); the aggregates can't change | regress |
| A writer holds its lock while W advances | step 2 waits for it | isolation `seal-wait` |
| A writer whose transaction began before W advanced, locking the table after | reads W from the metapage when it writes, not through its snapshot | isolation `seal-rr` |
| A reader older than the seal | sees the older row version (§4.4) | isolation `old-snapshot` |
| The sealer fails after step 1 | W is ahead; buckets below it with no row are scanned until the next seal | regress |
| Two sealers | the self-conflicting lock on the index | regress |
| TRUNCATE, REINDEX, VACUUM FULL, CLUSTER | `ambuild` gives both hidden heaps new, empty storage and writes a new metapage, so a rollback restores the old state | regress |
| REINDEX CONCURRENTLY, DROP INDEX | the hidden heaps belong to the index and go with it; a rebuilt index starts empty | regress |
| COPY, MERGE, ON CONFLICT DO UPDATE | through `aminsert` and the delete callback (RESEARCH.md §10 to check) | regress |
| Logical replication apply | callback also in `execReplication.c` (RESEARCH.md §6) | TAP, later |
| Prepared transactions | hold their locks, so step 2 waits for them, as CIC does | isolation, later |
| SERIALIZABLE | the reader scans the marker heap, which takes a relation SIREAD lock; a writer's marker insert conflicts with it | isolation `ssi` |
| Hot standby | metapage and hidden heaps are WAL-logged; standbys only read | TAP, later |
| pg_upgrade | open (§6). I: if the hidden heaps arrive empty, every bucket lacks a row and is scanned, so answers stay exact until a full reseal | TAP, later |
| Unlogged or temp table | refused at CREATE INDEX: a crash empties an unlogged heap but not the logged summaries | regress |
| Catalogs | refused: in-place updates bypass every write path (RESEARCH.md §1) | regress |

## 6. What goes into PostgreSQL

All of it, in core (jb, §2).

- **The delete callback,** new in the index AM API (RESEARCH.md §3 has none today):
  - `amnotifydelete(index, values, isnull, tid, heap, indexInfo)` in `IndexAmRoutine`, NULL for every other AM. Built in NOTES.md M1.1;
  - a new `ExecNotifyIndexesOfDelete` builds the index values of the old row version and calls it. Call sites (RESEARCH.md §6.1):
    - `ExecDeleteEpilogue`, which serves DELETE, MERGE's delete and moves between partitions;
    - `ExecUpdateEpilogue` when the result is not `TU_None`, which serves UPDATE, MERGE's update and ON CONFLICT DO UPDATE;
    - `ExecSimpleRelationDelete` and `ExecSimpleRelationUpdate` in `execReplication.c`;
    - not REPACK CONCURRENTLY's replay: its index starts with nothing sealed;
  - `ambuild` sets W to its minimum, so nothing is sealed until the first seal;
  - the old version is fetched only when some index on the table has the callback. The paths to cover are in RESEARCH.md §10.
- **The index AM `grain`,** in `src/backend/access/grain/`, next to BRIN:
  - its row in `pg_am.dat` (BRIN's is `:35`), its handler in `pg_proc.dat`, and a reloption kind for `bucket`, `origin` and `lag` in `reloptions.c`;
  - `amsummarizing = true`, so it doesn't block HOT;
  - `aminsert` and `amnotifydelete` write markers with `simple_heap_insert`, de-duplicated per statement in `ii_AmCache`; `aminsertcleanup` frees that cache;
  - `ambuild` refuses unlogged, temp and catalog tables, gives the hidden heaps new storage, and writes the metapage;
  - the metapage is WAL-logged with generic WAL at first, as bloom does (`contrib/bloom/blinsert.c:54`), or with its own resource manager if reviewers want one;
  - VACUUM callbacks do nothing; `amcostestimate` keeps it out of scans.
- **The hidden heaps,** created as TOAST tables are (`toasting.c:129`). Open:
  - their namespace: `pg_toast` (`toasting.c:298`) or one of their own;
  - how the index finds them, and how pg_dump and pg_upgrade treat them (§5).
- **Functions** in `pg_proc.dat`: `pg_grain_seal` and `pg_grain_buckets`.
- **Docs and tests:**
  - a chapter beside BRIN's (`doc/src/sgml/brin.sgml`);
  - a regress test in `src/test/regress`;
  - isolation specs in `src/test/isolation`.

## 7. Milestones

Upstream wants the design discussed on pgsql-hackers, "potentially before the code is even written", and patches that are small and separately committable (RESEARCH.md §9.7). So (mine):

**Proof comes first** (jb 2026-10-19: "before we do any of that, i want proof that this works, and that it brings the insame powerup of that i think it will"). Order: P1, then M1 and M3, then M0.5.

- **P1, the read ceiling,** in plain SQL with no grain code: grain's read path against a plain `GROUP BY`, at jb's sizes, with answers checked identical (NOTES.md P1).
- **M0.5, the design email, after M1 and M3.** Problem, current behavior, proposed behavior, syntax, semantics, the delete callback, the proof sketch and the prior art, as an RFC mail shaped like BRIN's first one (RESEARCH.md §9.7). Drafted here; jb decides whether and when it goes out, since it is public. Feedback on the callback, the hidden heaps and the name lands before they are built.
- **M1, the first slice** (jb 2026-10-19: "lets start with some basic agg-functions, like sum, count, min, max?"), as a patch series:
  - **0001:** the delete callback alone, tested through `src/test/modules/dummy_index_am`;
  - **0002:** the AM, the hidden heaps, `pg_grain_seal` and `pg_grain_buckets` (§6), with its docs chapter;
  - the §3.1 syntax with one key column, a `timestamptz` time key, integer `INCLUDE` columns, levels, `lag`, and partial indexes; group keys refused;
  - `count(*)`, plus `count`, `sum`, `min` and `max` of each `INCLUDE` column, on plain tables;
  - regress tests against a plain `GROUP BY` under the same snapshot, and the isolation tests of §5.
- **M2, 0003:** the planner. A `GROUP BY date_bin(...)` with the declared aggregates and a range on the key uses the summaries. In core this is a path in grouping planning, shaped like partitionwise aggregation (RESEARCH.md §9.5), not a hook.
- **M3:** benchmarks, rerunnable by reviewers. Each says first what question it answers.
  - Sizes (jb 2026-10-19: "lets, try a few order of magnitude, like 1k, 100k, 1M, 10M, meybe 100M?"): 1k, 100k, 1M, 10M rows, and 100M if the smaller ones call for it.
  - On an `orders` table with a partial index on paid orders, refunds as updates and backfills as late writes (jb 2026-10-19); a request log for volume.
  - Against a plain `GROUP BY` (B0) and the spec's B1 and B3, for 1 h, 24 h and 30 d windows.
  - Insert and delete throughput, with and without the index. Tables without it must show no cost from the callback.
- **M3.5, group keys:** the further key columns of §3.1, one summary row per bucket and key combination; the planner falls back to plain `GROUP BY` when combinations are too many.
- **M4:** `avg`, `numeric` and float value columns; other key types (`timestamp`, `bigint`, `integer`, SPEC.md §8).
- **M5:** tables that are already partitioned, one index per partition as with other AMs. Calendar levels (months, local-time days) need their own rules and are out until then.
- **M6:** sealing by autovacuum work items (§3.3).
- **M7:** the series to a CommitFest, outside an active one, with jb reviewing another patch of similar size as upstream expects (RESEARCH.md §9.7). jb's call; it is public.

## 8. Out

- Approximate answers, holistic aggregates and per-row windows (SPEC.md §10).
- An extension or a contrib module as the way to ship it (jb, §2).
- Triggers as the way to maintain it (jb, §2).
- A platform: no new table model, no job framework beyond one sealer (jb, §2).
- Unlogged, temp and catalog tables.
- In M1: expression columns, `FILTER`, group keys (M3.5), partitioned tables.
