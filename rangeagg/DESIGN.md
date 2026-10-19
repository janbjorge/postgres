# rangeagg: design

The build target. Anything not marked as jb's is my proposal. The facts it rests on are RESEARCH.md; the designs it replaced are ALTERNATIVES.md.

**Live today** (2026-10-19): nothing built. Branch `rangeagg-proto`; the next step is M1 (§7).

## 1. What, and the one question

Exact `count`, `sum`, `min` and `max` per time bucket on an ordinary table, made fast by a new index type built into PostgreSQL itself (jb 2026-10-19: "The goal here is NOT yet another postgres extension, i want this new index tobe part of postgres"). The case jb gave: a downloads log with a `done_at` timestamp, asked for downloads per hour over the last 30 days (jb 2026-10-19: "could i create an 'index' on this table and the done_at? and get a super fast view of the download, say for the last 30 days.").

**The one question** (mine): can a plain `GROUP BY` per hour over 30 days of a log table be answered exactly, from a few hundred summary rows plus the open hours, with no triggers and almost no cost on insert?

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
| D3 in core is a no-go; D2 rejected | mine | ALTERNATIVES.md |
| The code name rangeagg, which is also the name users would type in `USING` | mine; waiting on jb | TODO.md |

## 3. The design: sealed buckets behind a watermark

### 3.1 The declaration

```sql
CREATE INDEX downloads_hourly ON downloads
  USING rangeagg (done_at, bytes)
  WITH (bucket = '1 hour', lag = '2 hours');
```

- **First column:** the time key, `timestamptz` in M1.
- **The rest:** value columns, integer types in M1. Each gets `count`, `sum`, `min` and `max`, and every bucket also gets `count(*)`.
- **Options:**
  - `bucket` is the width;
  - `origin` is the `date_bin` origin, default `2000-01-01 00:00+00`;
  - `lag` is how far behind now the sealer stops.

### 3.2 State

- **The watermark W** lives in the index's metapage, block 0, as BRIN keeps `pagesPerRange` in its own (`brin_page.h:64-70`).
  - Writers read it under a share lock on that buffer, always after they have locked the table. No relcache and no catalog are involved.
  - Only the sealer moves it, and only forward. The write is WAL-logged and not transactional: a sealer that fails later leaves W ahead, which is safe (§5).
  - Users can't set it.
  - This is the "table flag" jb asked about: one value per index.
- **Summaries:** a hidden heap owned by the index, as a TOAST table is owned by its table (`toasting.c:129`).
  - One row `(bucket, n, nn[], sum[], min[], max[])` per sealed bucket, empty buckets included with `n = 0`.
  - **A bucket is sealed for a reader exactly when the reader's snapshot sees its row.** No separate "sealed up to" value is needed.
  - `nn`, `sum`, `min` and `max` hold one element per value column.
  - These are typed values, never serialized aggregate states. Those aren't stable across major versions, and a table of them would let users feed bytes to a deserialization function (RESEARCH.md §9.5). M4's `avg` keeps `sum` and `nn` the same way.
- **Dirty markers:** a second hidden heap of `(bucket)` rows, insert-only, with no index.

Both hidden heaps are ordinary WAL-logged MVCC heaps, so crash safety, standbys, every snapshot's view and freezing by autovacuum come for free. They are dropped with the index.

### 3.3 Writers and the sealer

**Writers.** A write whose row's time key is before W (the bucket is sealed, or about to be) inserts a dirty marker for its bucket in the same transaction. Normal writes, stamped "now", are after W and do nothing. Who calls what:
- an insert, and the new version of an update: `aminsert`;
- a delete, and the old version of an update: the new delete callback (§6).

Markers are de-duplicated per statement.

**The sealer** (`pg_rangeagg_seal(index)`), one at a time per index, under a self-conflicting lock on the index:
1. Write W = `date_bin(bucket, now() - lag)` into the metapage.
2. Wait, with `WaitForLockers(heap, ShareLock)`, until every transaction that held a writer's lock before step 1 has ended. This is CREATE INDEX CONCURRENTLY phase 2's wait (RESEARCH.md §5). New writers are never blocked: they lock the table after step 1, so they read the new W.
3. In a new transaction, under one snapshot:
   - compute the buckets from the last one with a row up to W, and every bucket with a marker this snapshot sees;
   - replace their summary rows;
   - delete those markers;
   - commit.

Later, autovacuum asks for sealing as a work item, as BRIN's autosummarize does (`brin.c:413`). That only decides when hours get sealed; correctness never depends on it (§4).

### 3.4 Readers

`pg_rangeagg_buckets(index, from, to)` in M1, a function that reads under one snapshot. Per bucket in the window:
- a summary row is visible, the bucket lies fully inside the window, and no marker is visible: the summary row;
- otherwise (open, dirty, or a partial bucket at an edge of the window): aggregate its rows with a range scan on the time key. The user's own btree or BRIN on it serves this scan.

From M2, the planner does this for plain `GROUP BY date_bin(...)` queries (§7).

### 3.5 Downloads per hour, worked through

- Downloads arrive stamped now, after W: no work on insert.
- Each hour, the sealer adds the hour that fell behind the lag.
- 30 days is 720 hourly rows, a year 8760 (NOTES.md M0.2). The last couple of open hours are scanned.
- A late log line, five hours old, marks its hour. That hour is scanned until the next seal recomputes it.
- Retention deletes mark their hours, and the next seal recomputes them as empty. Rows of empty hours below the oldest data can then be dropped: a bucket without a row is only scanned, and its scan finds nothing.

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
  - `amdelete(index, values, isnull, tid, heap, indexInfo)` in `IndexAmRoutine`, NULL for every other AM;
  - a new `ExecNotifyIndexesOfDelete` builds the index values of the old row version and calls it. Call sites:
    - `ExecDelete`, after a successful delete;
    - `ExecUpdate`, after a successful update that changed an indexed column;
    - `ExecSimpleRelationDelete` and `ExecSimpleRelationUpdate` in `execReplication.c`;
  - the old version is fetched only when some index on the table has the callback. The paths to cover are in RESEARCH.md §10.
- **The index AM `rangeagg`,** in `src/backend/access/rangeagg/`, next to BRIN:
  - its row in `pg_am.dat` (BRIN's is `:35`), its handler in `pg_proc.dat`, and a reloption kind for `bucket`, `origin` and `lag` in `reloptions.c`;
  - `amsummarizing = true`, so it doesn't block HOT;
  - `aminsert` and `amdelete` write markers with `simple_heap_insert`, de-duplicated per statement in `ii_AmCache`; `aminsertcleanup` frees that cache;
  - `ambuild` refuses unlogged, temp and catalog tables, gives the hidden heaps new storage, and writes the metapage;
  - the metapage is WAL-logged with generic WAL at first, as bloom does (`contrib/bloom/blinsert.c:54`), or with its own resource manager if reviewers want one;
  - VACUUM callbacks do nothing; `amcostestimate` keeps it out of scans.
- **The hidden heaps,** created as TOAST tables are (`toasting.c:129`). Open:
  - their namespace: `pg_toast` (`toasting.c:298`) or one of their own;
  - how the index finds them, and how pg_dump and pg_upgrade treat them (§5).
- **Functions** in `pg_proc.dat`: `pg_rangeagg_seal` and `pg_rangeagg_buckets`.
- **Docs and tests:**
  - a chapter beside BRIN's (`doc/src/sgml/brin.sgml`);
  - a regress test in `src/test/regress`;
  - isolation specs in `src/test/isolation`.

## 7. Milestones

Upstream wants the design discussed on pgsql-hackers, "potentially before the code is even written", and patches that are small and separately committable (RESEARCH.md §9.7). So (mine):

- **M0.5, the design email.** Problem, current behavior, proposed behavior, syntax, semantics, the delete callback, the proof sketch and the prior art, as a "WIP" or RFC mail. Drafted here; jb decides whether and when it goes out, since it is public. Feedback on the callback, the hidden heaps and the name lands before they are built.
- **M1, the first slice** (jb 2026-10-19: "lets start with some basic agg-functions, like sum, count, min, max?"), as a patch series:
  - **0001:** the delete callback alone, tested through `src/test/modules/dummy_index_am`;
  - **0002:** the AM, the hidden heaps, `pg_rangeagg_seal` and `pg_rangeagg_buckets` (§6), with its docs chapter;
  - `count(*)`, plus `count`, `sum`, `min` and `max` of integer value columns, a `timestamptz` key, plain tables;
  - regress tests against a plain `GROUP BY` under the same snapshot, and the isolation tests of §5.
- **M2, 0003:** the planner. A `GROUP BY date_bin(...)` with the declared aggregates and a range on the key uses the summaries. In core this is a path in grouping planning, shaped like partitionwise aggregation (RESEARCH.md §9.5), not a hook.
- **M3:** benchmarks, rerunnable by reviewers. Each says first what question it answers.
  - Sizes (jb 2026-10-19: "lets, try a few order of magnitude, like 1k, 100k, 1M, 10M, meybe 100M?"): 1k, 100k, 1M, 10M rows, and 100M if the smaller ones call for it.
  - Against a plain `GROUP BY` (B0) and the spec's B1 and B3, for 1 h, 24 h and 30 d windows.
  - Insert and delete throughput, with and without the index. Tables without it must show no cost from the callback.
- **M4:** `avg`, `numeric` and float value columns; other key types (`timestamp`, `bigint`, `integer`, SPEC.md §8).
- **M5:** tables that are already partitioned, one index per partition as with other AMs; coarser levels (day, month) built from the hourly ones.
- **M6:** sealing by autovacuum work items (§3.3).
- **M7:** the series to a CommitFest, outside an active one, with jb reviewing another patch of similar size as upstream expects (RESEARCH.md §9.7). jb's call; it is public.

## 8. Out

- Approximate answers, holistic aggregates and per-row windows (SPEC.md §10).
- An extension or a contrib module as the way to ship it (jb, §2).
- Triggers as the way to maintain it (jb, §2).
- A platform: no new table model, no job framework beyond one sealer (jb, §2).
- Unlogged, temp and catalog tables.
- In M1: expression columns, `FILTER`, group keys, partitioned tables.
