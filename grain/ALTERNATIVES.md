# Designs weighed

Every design considered, in the order the talk took them (2026-10-19), with why it was dropped or parked. So none is retried by accident. Facts are linked into RESEARCH.md. What is being built is DESIGN.md.

| § | Design | Status |
|---|---|---|
| 1 | The spec's freshness options (a), (b), (c) | (a) unsound on master; (b), (c) folded into §2 |
| 2 | D1: per-range summaries trusted by the visibility map and its page LSN | parked |
| 3 | Route 1: time partitions with D1 per partition | dropped (jb) |
| 4 | D2: counted btree | rejected |
| 5 | D3: rollup rows kept at write time | rejected for core |
| 6 | D5: deltas tagged with transaction ids | superseded by DESIGN.md |
| 7 | Triggers | rejected by jb |
| 8 | A heap wrapped in a custom table AM | rejected |
| 9 | Changes read from WAL (logical decoding) | rejected |
| 10 | TimescaleDB | out (jb) |
| 11 | D4: small wins on existing paths | notes |

## 0. The trade, in one table

What each route gives up (mine, 2026-10-19). Settled the same day: jb wants the index in PostgreSQL itself (DESIGN.md §2), so the "Stock PostgreSQL" column no longer decides anything and DESIGN.md stands. Deletes are the crux. An index AM hears of every new row version through `aminsert`, but never of a delete (RESEARCH.md §3). On stock PostgreSQL the other per-row channels are triggers (§7), a wrapped table AM (§8) and WAL (§9). That there is no fourth is I.

| Route | Plain table | No triggers | Stock PostgreSQL | Exact under deletes and updates |
|---|---|---|---|---|
| DESIGN.md: the watermark plus a core delete callback | yes | yes | no: one core callback | yes |
| §3 Route 1 (dropped): day partitions plus the VM page LSN | no: partitioned by day | yes | yes, plus an extension C function; proof read on master only | yes |
| §7: the same design in SQL, with triggers for writes | yes | no | yes | yes |
| DESIGN.md without the callback | yes | yes | yes | no: deletes, and the old row of updates, are missed silently |

## 1. The spec's freshness options

The spec's D1 trusts a block range's summary when every heap page in it is all-visible and the summary is current. Its three ways to know "current" (SPEC.md §6):

- **(a) VACUUM recomputes** the summary wherever it sets a VM bit. **Unsound on master:**
  - bits are also set by on-access pruning in read-only scans, and by COPY FREEZE (RESEARCH.md §1);
  - a `SELECT` set a bit again after an `UPDATE` with no VACUUM, so a stale summary passed "all bits set" (NOTES.md M0.1);
  - recomputing inside a read query's critical section is not acceptable.
- **(b) A generation counter bumped at every clear.** Sound if complete, but needs a hook at every clear, including:
  - redo;
  - truncation, whose clears don't go through `visibilitymap_clear`;
  - `pg_truncate_visibility_map`;
  - the unlogged corruption fix (RESEARCH.md §1).

  It also needs a crash-safe home. §2's LSN is this counter at VM-page grain, already kept by core.
- **(c) A separate fork** is where summaries live, not a proof they're current. It matters only for per-page states (§2.4).

## 2. D1 with the VM page LSN, "mechanism (d)"

### 2.1 Mechanism

On master, every WAL-logged 0→1 VM change stamps the VM page with its record's LSN before the VM buffer is unlocked (RESEARCH.md §1). Clears stamp too.

An entry for blocks [b0, b0+N) stores:
- its partial states and the exact min/max of the key;
- `k`, the blocks covered;
- `L[]`, the LSN of each VM page it overlaps (at most 2; 128-block ranges straddle, NOTES.md M0.2).

**Summarizer:**
1. Read the heap size `n`; set `k = min(N, n - b0)`.
2. Per VM page: under a share lock, require every bit of the range set, and record `L[i]`.
3. Read the heap pages under share locks, and feed every tuple with storage, detoasted, into a real Agg node in `AGGSPLIT_INITIAL_SERIAL`. Internal states refuse to run outside one (RESEARCH.md §4).
4. Write the entry.

**Reader** (snapshot σ earlier; `n` read after σ):
- **T0:** the heap is WAL-logged, not a catalog, not temp or unlogged, and not a skip-WAL relfilenode of this transaction;
- read the entry;
- **T1:** `k == min(N, n - b0)`;
- **T2:** then, unlocked, every bit is set;
- **T3:** then, under a share lock per VM page, `LSN == L[i] != 0`.

Trust it iff all pass.

### 2.2 Proof sketch, against the index-only-scan argument

Fix block p. Order: step 2, step 3, entry written, entry read, T2, T3.

1. **p's content didn't change between step 2 and T2.** A change clears p's bit (RESEARCH.md §1).
   - If the reader sees the bit clear, T2 fails.
   - If it sees it set again, a 0→1 transition stamped a new LSN under the exclusive VM lock. T3's share lock comes after that unlock, so T3 fails.
   - If it reads a stale set bit, the changing transaction is invisible to σ. That is the `visibilitymap.c:341-350` argument: a clear by a transaction σ sees precedes σ's ProcArrayLock.
2. **That content is exactly σ's view.**
   - The page is all-visible at T2.
   - If σ predates the bit being set, σ was alive then, so the setter's horizon was at most σ.xmin.
   - If σ is newer, it sees every committed tuple.
   - Pruned tuples were dead to σ either way.
3. **The range:** combining the pages' states equals one pass (the combine contract, docs 36.12.4). Floats behave as in parallel aggregation.

**Standby:** a bit is visible only after replay has resolved conflicts (RESEARCH.md §1); redo stamps the same record LSN. That the LSNs match is not checked (RESEARCH.md §10). If they differ, entries are only refused.

### 2.3 Races

| Event | Resolution |
|---|---|
| Insert, update or delete on a page in the range | clear, so T2 fails, or T3 if the bit was set again |
| Abort | aborted tuples are pruned before any set |
| Append into the partial last range | T1, and the new page's bit starts at 0 |
| On-access pruning or VACUUM sets a bit again | stamp, so T3 fails |
| COPY FREEZE | stamps when WAL is written; skip-WAL is refused by T0 |
| HOT pruning, freezing, tuple locks | values unchanged; their stamps only cause re-summarizing |
| Truncation, then the heap grows again | T1; the new bits start at 0; any set stamps |
| Crash | VM blocks are registered in WAL (`ed62d26caca`); entries are logged |
| SERIALIZABLE | `PredicateLockRelation(heap)`, as a seqscan takes |
| Catalog in-place update | catalogs refused |

### 2.4 Why it is parked

**Freshness is per VM page:** 32672 heap pages, 255.25 MiB (NOTES.md M0.2).
- Any change in that slice makes every entry in it stale: a set, a clear, a freeze.
- The newest slice and the retention slice are therefore nearly always stale.
- At 10M rows the whole heap is 2 VM pages, so there is almost no gain. At 100M rows it's 17 slices, at 1B 166 (NOTES.md M0.2).

**Finer grain needs core:**
- per-chunk stamps in a fork, at the sites that stamp the VM page;
- or a new VM format;
- or validating ranges by heap page LSN. On master a prune record may skip stamping the heap page (`pruneheap.c:2784-2791`), so that is unproved.

DESIGN.md needs neither.

## 3. Route 1: time partitions with §2 per partition

**Shape** (mine, 2026-10-19):
- partition by day on the key, so retention is `DROP PARTITION` and no deletes need tracking;
- store each closed day's hourly states with its VM page LSN once autovacuum has set every page all-visible;
- any late write moves the LSN, so that day is scanned until resealed;
- writers pay nothing, and nothing sits in the write path.

Each partition has its own VM, so a day up to 255.25 MiB is one VM page and §2.4's granularity problem disappears. The §2.2 proof holds per partition.

**Dropped** (jb 2026-10-19: "Yes, drop it, i feel that partitions has to meny wierd quarks and details."). In core it was no longer needed as a fallback: a table that is already partitioned gets an index per partition (DESIGN.md §7, M5).

It had been parked because it requires a partitioned table. For a modest log table that is the kind of weight jb ruled out (DESIGN.md §2, TimescaleDB). It stays the fallback for tables already partitioned. It needs a small C function to read the VM page LSN; pageinspect can, but only as superuser.

## 4. D2: counted btree, rejected

- **No visibility in index entries** (docs 11.9). Counts would include invisible and dead entries.
- **Lehman & Yao locks one page at a time, except at splits** (RESEARCH.md §3). Subtree counts mean updating and WAL-logging every ancestor on every insert, which makes the root a write hot spot (SPEC.md §5, constraint 3).
- **Entries also vanish through untracked paths:** VACUUM, simple and bottom-up deletion, and unlogged LP_DEAD marking. Each would need ancestor upkeep.

## 5. D3: rollup rows kept at write time

jb asked why not find the bucket and increment or decrement it (2026-10-19: "it shoud thus be pretty easy to find that relevent bucket, and incremt / decremt it?"). Finding the bucket is easy. Keeping one number per bucket isn't:

| Problem | Why |
|---|---|
| Snapshots need different answers | an uncommitted insert must not count for others; an older REPEATABLE READ snapshot must keep its old count. One stored number can't serve both. |
| Abort | Postgres has no undo, and nothing tells an index about a rollback |
| Delete | the index AM never hears of it (RESEARCH.md §3) |
| min/max | can't be decremented |

**As ordinary MVCC rows** (the spec's B2), it is correct but:
- every writer to the open bucket updates one row and holds its lock until commit;
- it adds WAL and bloat;
- min/max still fail under deletes.

**Insert-only delta rows** (`(bucket, +n)`, folded later) remove the lock contention. That became §7's SQL variant.

**No-go for core** (mine): it needs IVM-sized machinery, and SQL already does it.

## 6. D5: deltas tagged with transaction ids, superseded

jb: "incrent / decremtn, was one suggestion, there must be tricks we can do" (2026-10-19). Shape (mine):
- per range, a base plus deltas `(xid, +state)`; a reader applies the deltas its snapshot sees (`XidInMVCCSnapshot`, RESEARCH.md §5);
- aborted deltas are never visible; each transaction writes its own slot under a short buffer lock;
- min/max survive whole-range deletes by "count reached 0"; partial deletes and float subtraction mark the range dirty;
- statement batching through `aminsertcleanup` (RESEARCH.md §3).

**Risks:**
- xids must be folded before `relfrozenxid` passes them, including under `INDEX_CLEANUP off`;
- cleanup records need a standby conflict horizon;
- subtransactions and command ids;
- it needs the same delete callback DESIGN.md §6 adds.

**Superseded** when jb pointed out that buckets are small (DESIGN.md §2). Rescanning a dirty bucket needs no deltas, no inverses and no xid lifetime.

## 7. Triggers, rejected by jb

jb 2026-10-19: "I know we can do triggers, but i think most ppl. fear triggers, as drift other issues creep in."
- **They can be switched off:** `ALTER TABLE ... DISABLE TRIGGER`, and `session_replication_role = replica`, which restores and replication tools set.
- **They can be forgotten** on a newly attached partition.
- **Their logic can drift** from the queries that depend on them.
- **One missed write is a silently wrong count.**

**The SQL form of DESIGN.md** (a trigger writes the dirty markers; a plpgsql sealer with REPEATABLE READ step 3; a reader function) is written out in the talk of 2026-10-19. Two extra conditions apply to it:
- with the watermark read through the writer's snapshot, its sealer must also wait for older snapshots (`WaitForOlderSnapshots`, RESEARCH.md §5);
- logical-replication deletes bypass `ExecDelete` (RESEARCH.md §6).

It could serve as a reference point in benchmarks (mine).

## 8. A heap wrapped in a custom table AM, rejected

It would see every insert, update and delete without triggers or core changes, since `GetHeapamTableAmRoutine` is exported (RESEARCH.md §7). But 11 code paths check for the built-in heap AM: amcheck, pageinspect, pgstattuple, pgrowlocks, pg_surgery and logical replication's conflict detection among them. On such a table they refuse or break.

## 9. Changes read from WAL, rejected

Logical decoding sees every change and needs no triggers. But it is asynchronous: a reader can see summaries that don't yet reflect committed changes. It is not exact unless every reader waits for the decoder to catch up.

## 10. TimescaleDB, out

- **jb 2026-10-19:** "TimescaleDB is relted, but i think TimescaleDB is overkill for loads of problmes that are in this is sphare."
- **The spec rules out third-party extensions** as the solution (SPEC.md §8).
- **Its continuous aggregates are close** to DESIGN.md: a watermark moved in its own transaction, changes below it logged, buckets recomputed whole. But they hook their own hypertable insert path, and real-time results are not exact (RESEARCH.md §9.3).

## 11. D4: small wins

- **The `skip_fetch` removal** (RESEARCH.md §8) is why VM shortcuts need §2.2's kind of proof. §2 avoids that bug's class: it carries no TIDs, and decides trust after reading the entry.
- **An index-only `count(*)` that forms no tuples** is still O(rows). Low value.
- **BRIN that tightens min/max on resummarize** would fix BRIN's decay after retention deletes, aggregates or not. A possible small patch of its own (idea).
