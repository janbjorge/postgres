# What PostgreSQL does, as read in this tree

Facts the design rests on. Each one lives here once; other files link to its section.

**Tree:** master at `0f3b42ce806` (2026-10-19). Every file:line below is at that commit; re-check when the branch moves to a new master.

**Tags:**
- **R** — read in this tree, or in a linked source;
- **T** — tested here (NOTES.md ID);
- **U** — from memory, not checked;
- **I** — inference.

## 1. The visibility map on master

- **R** Two bits per heap page, all-visible and all-frozen. One VM page covers `HEAPBLOCKS_PER_PAGE` heap pages (`visibilitymap.c:119-125`): 32672 heap pages, 255.25 MiB of heap at 8 KiB blocks (NOTES.md M0.2).
- **R** `visibilitymap_set` (`visibilitymap.c:262-306`) neither WAL-logs nor stamps the VM page LSN. It asserts it runs in the caller's critical section (`:281`), and the caller logs the change and stamps the page.
- **R Bits are set in four places on the primary, and each stamps the VM page LSN before unlocking:**
  - prune/freeze, for VACUUM and on-access alike: set `pruneheap.c:1347`, stamp `:2780`, unlock `:1374`;
  - VACUUM on an empty page `vacuumlazy.c:1967`, and in phase III `:2849`; both log through `log_heap_prune_and_freeze`;
  - COPY FREEZE: set `heapam.c:2489`, stamp `:2631`, unlock `:2641`.
- **R In redo:** sets at `heapam_xlog.c:286-287` and `:741-742` stamp only when the bits changed. Clears at `:59-60` and `:835-858` stamp likewise.
- **R Clears stamp the VM page LSN too** when the VM buffer changed, because the VM block is now registered in the heap record: insert `heapam.c:2177-2185`, multi-insert `:2616-2631`, delete `:3147-3155`, update `:4305-4308`, tuple locks `:3956`, `:5421`, `:6199`. Commit `ed62d26caca` (2026-07-15), "Fix VM clear WAL logging by registering VM blocks".
- **R Two clears are not stamped:**
  - the tail of the last VM page on truncation, `visibilitymap.c:506-518`, unless checksums need a full-page image (`:529-530`);
  - the corruption fix, `pruneheap.c:963`, which is not WAL-logged.
- **R Content changes clear all-visible first.** Insert `heapam.c:2076`, multi-insert `:2478`, delete `:3052`, update `:4240` and `:4264`. Tuple locks clear only all-frozen and change no values (`:3926`, `:5376`, `:6169`).
- **R The exception:** `heap_inplace_update_and_unlock` (`heapam.c:6728`) rewrites tuple data without touching the VM. Only catalogs use it.
- **R On-access pruning sets the VM in read-only scans.** `heap_page_prune_opt` passes `HEAP_PAGE_PRUNE_SET_VM` when `rel_read_only` (`pruneheap.c:366`). Commit `b46e1e54d07` (2026-03-30), "Allow on-access pruning to set pages all-visible".
  - T: a `SELECT` set a bit again after an `UPDATE`, with no VACUUM (NOTES.md M0.1).
  - The header comment "only VACUUM currently sets visibility map bits" (`visibilitymap.c:81`) is stale.
- **R `XLOG_HEAP2_VISIBLE` is gone** (`a881cc9c7e8`, 2026-03-24). COPY FREEZE stopped using it in `d96f87332b3` (2025-10-09).
- **R Standby conflicts.** A prune record that sets the VM carries a conflict horizon of at least the newest live xmin (`pruneheap.c:1272-1281`). Replay cancels conflicting standby snapshots first (`heapam_xlog.c:108-124`).
- **R Index-only scans on master:**
  - the VM check is in `heapam_indexscan.c:364`;
  - it takes `PredicateLockPage` per heap page skipped (`:389`);
  - the race argument now sits above `visibilitymap_get_status` (`visibilitymap.c:329-350`).

## 2. Heap and VACUUM

- **R** By the time index vacuuming runs, dead tuples are LP_DEAD with no storage (`vacuumlazy.c:2831`). The bulk-delete callback gets only a TID (`genam.h:96`), so an index can't subtract a dead row's values at VACUUM time.
- **R** Every sequential scan takes `PredicateLockRelation` (`heapam.c:1249`).

## 3. The index AM API

- **R** Callbacks of `IndexAmRoutine` (`amapi.h`) include `ambuild`, `ambuildempty`, `aminsert`, `aminsertcleanup` (`:131`, `:299`), `ambulkdelete`, `amvacuumcleanup`, `amcostestimate`, `amoptions` and the scan calls. **There is no per-row delete callback**, so an index hears of a delete only through VACUUM.
- **R `aminsertcleanup` is called** at the end of a batch of inserts: `execIndexing.c:257` (ExecCloseIndices), `constraint.c:167` and `index.c:3586`.
- **R** An index AM without predicate locks makes every scan lock the whole index (`indexam.c:324-325`).
- **R BRIN:**
  - `ampredlocks = false` (`brin.c:274`); `amsummarizing = true` (`:279`);
  - `brinvacuumcleanup` summarizes new ranges, but never the partial last one (`brin.c:1341`, `:1928-1936`);
  - largest item 8152 bytes at 8 KiB blocks (`BrinMaxItemSize`, `brin_pageops.c:28`; NOTES.md M0.2).
- **R Custom reloptions:**
  - `add_reloption_kind` and `add_*_reloption`, used by bloom (`contrib/bloom/blutils.c:54-69`);
  - `add_string_reloption` takes the lock level an `ALTER ... SET` needs (`reloptions.h:203-205`);
  - custom options join the lookup table (`reloptions.c:812-814`), and `ALTER ... SET` takes the highest lock level of the options it names (`AlterTableGetRelOptionsLockLevel`, `reloptions.c:2456-2484`, used at `tablecmds.c:4887`). DESIGN.md no longer keeps W in a reloption, so this now matters only for `bucket`, `origin` and `lag`;
  - history: custom options first defaulted to AccessExclusiveLock (`736b84eede`), then got the lock argument (`69f94108079`), after a bloom bug in 2019 (§9.4).
- **R The nbtree concurrency design:**
  - Lehman & Yao with right-links; one page locked at a time except at splits (`nbtree/README:14-30`, `:139-143`);
  - LP_DEAD marking by kill_prior_tuple, plus simple and bottom-up deletion (`:474-518`).

## 4. Aggregates and the planner

- **R Combine functions** (`pg_aggregate.dat`, parsed by tools/aggs.py, NOTES.md M0.3): 139 aggregates have one and 26 don't.
  - With one, among others: count, sum, min, max, avg, stddev, variance, corr, covar, regr_*, bool_and/or, bit_*, and also array_agg and string_agg, whose state isn't bounded.
  - Without one: json[b]_agg*, xmlagg, range_agg, and the ordered-set and hypothetical aggregates.
  - So having a combine function doesn't imply a bounded state, and the catalog has no flag for boundedness.
- **R Transition functions with `internal` state** refuse to run outside an Agg node: `AggCheckCallContext` in `numeric.c:4794`, `:5018`, `:5118` and others. Partial states have to come from a real Agg node.
- **R Planner hooks:** `create_upper_paths_hook` (`planner.h:55`) and `mark_partial_aggref` (`planner.h:75`) are exported.
- **R** `date_trunc(text, timestamptz)` is STABLE (`pg_proc.dat:2594`), while `date_trunc(text, timestamptz, text)` and `date_bin` are immutable (`:2598`, `:6462-6467`).

## 5. Locks, invalidation and waiting (the CIC pattern)

- **R Taking a lock processes pending invalidations,** so relcache entries are fresh after it (`lmgr.c:136`, likewise `:171`, `:202`, `:265`). Except when the lock was already held and cleared.
- **R `WaitForLockers` / `WaitForLockersMultiple`** (`lmgr.c:911`, `:989`) wait until no transaction holds a conflicting lock. They wait on the holders' VXIDs and don't take the lock, so new lockers are never blocked.
- **R CREATE INDEX CONCURRENTLY phase 2** (`indexcmds.c:1706-1721`): "wait until no running transaction could have the table open with the old list of indexes ... we do not need to worry about xacts that open the table for writing after this point; they will see the new index when they open it". The watermark protocol in DESIGN.md §3.3 reuses this argument.
- **R** `WaitForOlderSnapshots` (`indexcmds.c:446`, used `:1832`) waits out older snapshots, as CIC phase 3 does.
- **R** `XidInMVCCSnapshot` is exported (`snapmgr.h:133`).
- **R** plpgsql runs `COMMIT; SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;` in a procedure (`plpgsql_transaction.sql:474-488` and its expected output).

## 6. Writes outside the executor's main path

- **R Logical-replication apply deletes through `simple_table_tuple_delete`** (`execReplication.c:1038`), not `ExecDelete`, and fires the after-row triggers itself (`:1041`). So a delete hook placed only in `ExecDelete` would miss replicated deletes.

## 7. The table AM

- **R** `GetHeapamTableAmRoutine` is exported (`tableam.h:2163`).
- **R `HEAP_TABLE_AM_OID` is checked in 11 places**, so a heap wrapped in another table AM is refused or misread there:
  - amcheck `verify_heapam.c`, pageinspect `heapfuncs.c`, pgstattuple `pgstattuple.c` and `pgstatapprox.c`, pgrowlocks, pg_surgery;
  - logical replication's `conflict.c`, and bootstrap.

## 8. Precedent: a VM shortcut found unsound

- **R** `459e7bf8e2f` (2025-04-02) removed bitmap heap scan's `skip_fetch`. A concurrent VACUUM can remove dead TIDs and set pages all-visible while those TIDs sit in the bitmap, so a count returns too many rows. Any VM shortcut needs a proof like ALTERNATIVES.md §2.2.

## 9. Prior art

Searched 2026-10-19 by three research agents (jb 2026-10-19: "I think we need to seach postgres mailing lists"). Here **R** means the agent opened the link and read it; "snippet" means it saw only a search result. Not yet re-read by me.

### 9.1 IVM in core, and pg_ivm

- **R "Implementing Incremental View Maintenance"**, Yugo Nagata, Takuma Hoshiai, from 2018-12-27 ([thread](https://www.postgresql.org/message-id/20181227215726.4d166b4874f8983a641123f5@sraoss.co.jp)):
  - AFTER triggers with transition tables and delta tables;
  - a pgbench run with one view went from about 10k TPS to 141 (v19, 2020-11). v20 reached 455 against 1232 with a weaker lock for single-table views;
  - Greg Stark (2019-04-03) suggested keeping moving-aggregate states and using the inverse transition function. I: min and max have none.
- **R CF #2138** ([entry](https://commitfest.postgresql.org/patch/2138/)): returned with feedback 2022-10-12, after moving from CF to CF since 2019-07, with no committer. Why it was returned was not found.
- **R "take 2", CF #4337** ([entry](https://commitfest.postgresql.org/patch/4337/)): from 2023-05-31, Needs review in PG20-Drafts; v40 2026-08-02 needs a rebase; no committer.
  - Mechanism ([wiki](https://wiki.postgresql.org/wiki/Incremental_View_Maintenance)):
    - statement-level triggers with transition tables;
    - the pre-update state rebuilt by filtering on xmin/cmin;
    - hidden `__ivm_count__` columns;
    - count, sum, avg, min and max, where deleting a group's min or max recomputes the group from the base table;
    - an exclusive lock on the view for DELETE and UPDATE, and an error under REPEATABLE READ and SERIALIZABLE.
  - Objections ([2025-08-30](https://www.postgresql.org/message-id/20250830050403.73fd450cd2712acca11494a8%40sraoss.co.jp) and the take-2 thread):
    - Kirill Reshke (2024-07-27): "far from being committable"; he disliked the hidden columns and the per-aggregate SPI code, and wanted reloptions instead of new keywords;
    - Reshke: reading the pre-update state via `table_tuple_fetch_row_version` breaks on some table AMs.
  - Nagata's own open questions:
    - triggers or an executor-level design "like declarative partitioning";
    - a clean pre-update state needs a per-table AS OF snapshot scan, "not trivial";
    - whether to drop aggregates from the first version.
- **R pg_ivm** ([repo](https://github.com/sraoss/pg_ivm)), the same design as an extension:
  - eight statement-level triggers per base table ([createas.c](https://raw.githubusercontent.com/sraoss/pg_ivm/main/createas.c));
  - an ExclusiveLock on the view unless single-table and insert-only;
  - an error under REPEATABLE READ and SERIALIZABLE;
  - no partitioned tables or logical replication;
  - PG 13 to 19, v1.17 of 2026-10-17.

### 9.2 Other pgsql-hackers threads

- **R "Accelerating aggregates"**, Steve Atkins, 2004-06 ([thread](https://www.postgresql.org/message-id/flat/20040611075240.GA7860%40gp.word-to-the-wise.com)). Shared-memory aggregate states merged at commit. Tom Lane: every snapshot would need its own copy and the update can't be atomic with commit, "the overhead of this scheme would be enormous". This is ALTERNATIVES.md §5's objection; DESIGN.md answers it by keeping state in MVCC tables.
- **R "counting algorithm for incremental matview maintenance"**, Kevin Grittner, 2013-05-14 ([mail](https://www.postgresql.org/message-id/1368561126.64093.YahooMailNeo%40web162904.mail.bf1.yahoo.com)): a `count_t` column and signed delta relations; min and max not discussed.
- **R "Query Rewrite with Postgres' materialized views"**, Tom Lane, 2015-02-21 ([mail](https://www.postgresql.org/message-id/10883.1424541848%40sss.pgh.pa.us)): "unlikely that matviews would ever be so transparent that it would be okay for the planner to automatically substitute them." I: the reason is staleness, which exact summaries don't have. Bears on M2.
- **Snippet:** Heikki Linnakangas, GSoC 2010 matviews: maintenance alone isn't very useful unless the planner substitutes the view ([mail](https://www.postgresql.org/message-id/4BC1DBFB.8050700%40enterprisedb.com)).
- **R** Nico Williams, 2016-11-23: matviews with history tables and triggers, no outcome ([mail](https://www.postgresql.org/message-id/20161123031126.GQ32683%40localhost)).
- No pgsql-hackers thread on TimescaleDB's continuous aggregates was found.

### 9.3 TimescaleDB continuous aggregates

- **R How changes are captured now** ([README](https://github.com/timescale/timescaledb/blob/main/tsl/src/continuous_aggs/README.md)):
  - from hooks in their own ModifyHypertable node and in COPY;
  - each transaction writes one [min, max] time range at its end;
  - nothing is logged above the invalidation threshold, their watermark;
  - refresh moves the threshold in its own transaction first, "otherwise invalidations would be lost". This matches DESIGN.md §3.3 step 1.
- **R The original design**, PR #1184, 2019-04 ([PR](https://github.com/timescale/timescaledb/pull/1184)):
  - a trigger collected min/max and wrote them at commit;
  - logging was skipped only at READ COMMITTED or weaker and above the threshold. I: a snapshot problem at higher isolation levels;
  - moving the threshold took an AccessExclusiveLock.
- **R Off triggers** ([blog, 2025-11-26](https://www.tigerdata.com/blog/timescaledb-2-22-2-23-90x-faster-distinct-queries-postgres-18-support-configurable-columnstore-indexes-uuidv7)):
  - 2.22 tried invalidation read from WAL, retired for missing its IOPS goal;
  - 2.23 moved it into "tuple-routing logic inside Timescale's core", for 10-20% better DML throughput.

  I: they own the insert path of a hypertable; a plain table gives an extension no such path, which is why DESIGN.md §6 needs a core callback.
- **R Real-time aggregation** ([docs](https://www.tigerdata.com/docs/use-timescale/latest/continuous-aggregates/real-time-aggregates)): materialized buckets below the watermark, raw rows above. Off by default since 2.13. I: a write into an already-materialized bucket is not seen until the next refresh, so results are not exact; DESIGN.md §3.4's markers are the difference.
- **R Refresh** ([blog, 2026-08-14](https://www.tigerdata.com/blog/continuous-aggregate-refresh-demystified)):
  - UPDATE and DELETE are logged like INSERT, and buckets they touch are recomputed whole;
  - the first refresh transaction briefly blocks writes;
  - buckets outside the policy window are never revisited.

### 9.4 Index AM delete notification, BRIN aggregates

**No proposal of an executor-called per-row delete callback for index AMs was found.** The nearest:

- **R** Peter Geoghegan, 2020-11-10, while adding the `indexUnchanged` hint, committed as `9dc718bdf2b` ([mail](https://www.postgresql.org/message-id/CAH2-WzmL%2Bp-F1JOb7b6gJRhV-qiO_%3DaNETG43%2BV%2B7EPKxSKP5g%40mail.gmail.com)):
  - "maybe I should add a new amupdate() routine?";
  - delete-marking on DELETE only as a hint that need not be reliable; a reliable view of update chains "would be very brittle";
  - the replies were not seen.
- **R "Retail IndexTuple deletion"**, Andrey Lepikhov, 2018-06 to 2019-01, returned with feedback 2019-01-31 ([thread](https://www.postgresql.org/message-id/flat/11d32604-c0b9-7c05-edee-76582374404c%40postgrespro.ru)). `amtargetdelete(info, stats, values, isnull)`, but driven by VACUUM and pruning, not by the executor.
- **R WARM**, Pavan Deolasee, 2016-2017, returned 2017-10-02 ([CF #775](https://commitfest.postgresql.org/patch/775/)):
  - Alvaro Herrera objected to the AM computing index values from a heap tuple, "a problem in itself" ([mail](https://www.postgresql.org/message-id/20170314191609.tzw6bz3uigbqmtif%40alvherre.pgsql));
  - Robert Haas favoured the executor passing the old and new values to the AM ([mail](https://www.postgresql.org/message-id/CA%2BTgmob4RAhFm_is%3DszyvXWGvtV%3D5-9gbr9Ni391NFWpce6tOg%40mail.gmail.com)). That is DESIGN.md §6's shape.
- **R Indirect indexes**, Alvaro Herrera, 2016, returned 2017-04-01 ([mail](https://www.postgresql.org/message-id/20161018182843.xczrxsa2yd47pnru@alvherre.pgsql)). They don't address deletes.
- **R "Thick indexes"**, Gokulakannan Somasundaram, 2007-2008 ([thread](https://www.postgresql.org/message-id/flat/9362e74e0710072342m5899fa7ckbe07dc3ae2ea1ca4%40mail.gmail.com)). Heikki Linnakangas:
  - recomputing a heap tuple's index keys fails if an expression's function is broken;
  - a delete on a table with 3 indexes costs "3 index lookups + one heap lookup";
  - locking all entries at once is "prone to deadlocks".
  - I: the first is why DESIGN.md §8 keeps expression columns out; the second is paid only by tables that opt in.
- **R Alexander Korotkov:**
  - 2019-04-10, delete-marking means a "random lookup index twice for every deleted index tuple" ([mail](https://postgresql.org/message-id/CAPpHfdu286b3ALZLxvCJ_dD-ccdRwv2GV-2e%3DWyCMTGSh0w3xQ%40mail.gmail.com));
  - "Table AM Interface Enhancements", 2023-11 ([mail](https://www.postgresql.org/message-id/CAPpHfdurb9ycV8udYqM%3Do0sPS66PJ4RCBM1g-bBpvzUfogY0EA%40mail.gmail.com)): in 2024-03, index deletes happen "later by vacuum" and the "index AM should be revised for that", deferred ([mail](https://www.mail-archive.com/pgsql-hackers@lists.postgresql.org/msg163535.html)). Snippet: returned 2024-09;
  - OrioleDB blog, 2025-03-24: the index AM API lacks a "point delete" ([blog](https://www.orioledb.com/blog/better-table-access-methods)). A vendor, not a hackers thread.
- **R Tom Lane, "Much Ado About COUNT(\*)"**, 2005-01-12: "we're not willing to slow down every other operation" ([mail](https://www.postgresql.org/message-id/29927.1105554266%40sss.pgh.pa.us)). I: the callback is opt-in, so tables without it pay nothing.

**Summarizing indexes:**

- **R** HOT with BRIN went in as `5753d4ee32` (2021-11-30) and was reverted as `e3fcca0d0d` (2022-06-16) because the index missed rows; Robert Haas: HOT can skip BRIN, but "that doesn't mean that we can ignore the need to update those indexes" ([thread](https://www.postgresql.org/message-id/flat/05ebcb44-f383-86e3-4f31-0a97a55634cf@enterprisedb.com)).
- **R** It was recommitted as `19d8e2308bc` (2023-03-20, PG16) with `amsummarizing` and `TU_UpdateIndexes` ([commit](https://git.postgresql.org/gitweb/?p=postgresql.git;a=commit;h=19d8e2308bc51ec4ab993ce90077342c915dd116)). BRIN is the only summarizing AM in core ([docs](https://www.postgresql.org/docs/current/storage-hot.html)).
- **R** The "Heap HOT Selective Index Updates" RFC would replace `TU_UpdateIndexes` with a bitmap of modified attributes; no thread yet ([wiki](https://wiki.postgresql.org/wiki/Heap_HOT_Selective_Index_Updates)). I: it touches the same lines as DESIGN.md §6's `ExecUpdate` change.

**Aggregates from indexes:**

- **R** BRIN for MIN/MAX, Alvaro Herrera 2015-09-27: needs a planagg path, "doesn't look like a simple project"; no patch ([mail](https://postgresql.org/message-id/20150927211306.GS295765%40alvherre.pgsql)).
- **R** Tom Lane, pgsql-general 2019-08-05: extremal BRIN ranges may "contain no live tuples" ([mail](https://postgresql.org/message-id/2667.1565020427%40sss.pgh.pa.us)). I: BRIN summaries aren't exact after deletes (ALTERNATIVES.md §11).
- **R** BRIN Sort, Tomas Vondra 2022: Tom Lane, "vastly more complexity than benefit" ([mail](https://www.postgresql.org/message-id/e70fa091-e338-1598-9de4-6d0ef6b693e2@enterprisedb.com)).
- **R** Index-only `count(*)`, Alexander Kuzmenkov 2017: the executor half became `skip_fetch` (`7c70996ebf0`), removed by §8's `459e7bf8e2f` ([thread](https://www.postgresql.org/message-id/flat/239a8955-c0fc-f506-026d-c837e86c827b@postgrespro.ru)).
- Nothing on a counted btree, or an index that stores sums or counts.

**Reloptions:**

- **R** Michael Paquier, 2019-09-20: `ALTER INDEX ... SET` on a bloom index failed with "unrecognized lock mode" ([mail](https://www.postgresql.org/message-id/20190920013831.GD1844@paquier.xyz)); fixed as in §3.
- Local git: custom reloptions for table AMs went in as `9bd99f4c26` (2024-04-08) and were reverted 2024-04-11.

### 9.5 The planner side, and stored aggregate states

- **R Rewriting queries onto matviews** has never had a core patch:
  - 2015 (thread under §9.2's Tom Lane mail): Robert Haas, not until matviews are maintained automatically; Tom Lane, matviews are "going to be at least a little bit stale";
  - John Dent, 2018, an extension only (pg_fdw_mv_rewrite) ([thread](https://www.postgresql.org/message-id/flat/DB608841-3837-48AA-A10E-7F1345208BD8%40QQdd.eu)). Jim Finnerty asked for an exact eligibility spec and warned stale views give "wrong results"; Corey Huinker, maintained views first;
  - snippet: Josh Berkus, 2005, "Automatically, and by default, no." ([mail](https://www.postgresql.org/message-id/20050103165532.GA21212%40zoom.spline.inf.fu-berlin.de)).
  - I: every objection is staleness. Exact summaries answer it, which is M2's case.
- **R Plan shapes already in core:**
  - combine functions, committed 2016-01-25 ([CF #451](https://commitfest.postgresql.org/patch/451/));
  - partitionwise aggregation, a partial aggregate per partition finalized above the Append, `e2f1eb0ee30` (2018-03-22, in this tree);
  - eager aggregation, partial aggregates below joins, `8e11859102f` (2025-10-08, in this tree). A PG19 open item on it is still open ([wiki](https://wiki.postgresql.org/wiki/PostgreSQL_19_Open_Items)).
  - I: M2's plan is partitionwise aggregation's shape, with summary rows as one arm.
- **R Serialized aggregate states must not be stored:**
  - the docs say serial/deserial functions move a state "from one process to another" ([docs 36.12.4](https://www.postgresql.org/docs/current/xaggr.html));
  - "Partial aggregates push down", Alexander Pyhalov then Yuki Fujii, returned with feedback CF 2024-01 ([CF #4019](https://commitfest.postgresql.org/patch/4019/)):
    - Tomas Vondra (2021, and [2024-08-21](https://postgresql.org/message-id/982a903e-cb59-4b60-911a-bdbb88da594e%40vondra.me)): the functions "were not designed to be portable"; no cross-version stability;
    - Robert Haas ([2023-11-27](https://www.postgresql.org/message-id/CA%2BTgmoYQKF-8vL8sctWuw_dskBiQWcspuNPJuH4ttusuiP529g%40mail.gmail.com)): it would let users feed arbitrary data to a deserialization function;
    - Tom Lane ([2024-03-19](https://www.postgresql.org/message-id/4020145.1710883747%40sss.pgh.pa.us)): "fairly unsafe" beyond the same major version;
  - TimescaleDB stored bytea partial states, and after PG13 to PG14 they failed with "insufficient data left in message" when numeric's serial format changed ([issue #4403](https://github.com/timescale/timescaledb/issues/4403)). Snippet: 2.7 (PR #4269) moved to finalized values.
  - So DESIGN.md §3.2 keeps typed values, never serialized states.
- **R Citus** ships states between nodes as text, bytea from send functions, or serialfn output, and combines them on the coordinator ([aggregate_utils.c](https://github.com/citusdata/citus/blob/main/src/backend/distributed/utils/aggregate_utils.c)). Between processes of the same build, not stored.
- **R Aggregation push-down**, Antonin Houska: withdrawn 2021-03-11 ([CF #1247](https://commitfest.postgresql.org/patch/1247/)); take 2 returned 2024-01-22 ([CF #3764](https://commitfest.postgresql.org/patch/3764/)).
- No core proposal for combining stored states from SQL was found.

### 9.6 Not found or not read

- Why CF #2138 was returned; the IVM mails of 2021 to 2022; TimescaleDB 2.7's "finalized form" redesign.
- The replies to Geoghegan's `amupdate()` question; the closing state of Korotkov's table AM entry.
- Any follow-up to partial-aggregate pushdown after 2024-08.

### 9.7 Getting a patch into PostgreSQL

Read 2026-10-19 (jb: "lets read up on https://wiki.postgresql.org/wiki/Submitting_a_Patch so we make sure we do not have todo loads of double work").

- **R From the wiki page** ([Submitting a Patch](https://wiki.postgresql.org/wiki/Submitting_a_Patch)):
  - **Design first.** Non-trivial changes "should be discussed (potentially before the code is even written) on the pgsql-hackers list":
    - whether and how users interact with the feature, its syntax, its exact semantics, and backward compatibility;
    - the email opens with the problem, the current behavior and the proposed behavior;
    - start small, not "a giant patch all at once".
  - **Format.** One patch normally. A large one may be split into "distinct and separately commit-able sections", with their order stated. `git format-patch`.
  - **Style:**
    - "follow the style of the adjacent code!", and pgindent;
    - no `#ifdef` to switch the change on;
    - comments say why, not what;
    - no stray whitespace (`git diff --check`).
  - **Docs and tests.** Docs on how to use it, with examples, and regression tests. Without both, a patch is "automatically considered a WIP one".
  - **Performance.** Describe the effect, with tests a reviewer can rerun.
  - **The email:**
    - a uniquely named file per version;
    - what it does;
    - WIP or for application;
    - the target branch;
    - compile and test status, and platforms;
    - docs, performance and design rationale;
    - "WIP" in the subject when asking for design feedback.
  - **Design rationale** may also live in code comments or a README in the code's directory.
  - **The commitfest:**
    - post to pgsql-hackers, then add it to the next CommitFest;
    - avoid new submissions during an active one;
    - each submitter is expected to review at least one patch of similar size;
    - expect at least three versions;
    - unrelated changes can get a patch rejected.
  - **License.** Posting gives the PostgreSQL Global Development Group the right to distribute it under the PostgreSQL license.
  - **Linked, not read:** [Creating Clean Patches](https://wiki.postgresql.org/wiki/Creating_Clean_Patches), [Reviewing a Patch](https://wiki.postgresql.org/wiki/Reviewing_a_Patch), [Regression test authoring](https://wiki.postgresql.org/wiki/Regression_test_authoring), [Developer FAQ](https://wiki.postgresql.org/wiki/Developer_FAQ), [CommitFest](https://wiki.postgresql.org/wiki/CommitFest), [Working with Git](https://wiki.postgresql.org/wiki/Working_with_Git).
- **R In this tree:**
  - **OIDs** for a patch not committed soon: a consecutive group from a random start in 8000–9999; renumbered after commit with `renumber_oids.pl` (`doc/src/sgml/bki.sgml:410-424`).
  - **Build files.** A backend directory carries both a `Makefile` and a `meson.build`, as BRIN's does. Meson isn't installed here (NOTES.md M0), so meson files can't be checked locally.
  - **A test index AM** already exists to model a callback's tests on: `src/test/modules/dummy_index_am`.
- **U** The catversion bump is usually left to the committer, with the patch noting it is needed.

## 10. Open, to check

- [ ] Does `amvacuumcleanup` still run when VACUUM bypasses index vacuuming (INDEX_CLEANUP AUTO)? It matters only for the parked visibility-map routes.
- [ ] Do a primary and a standby carry the same VM page LSN for the same state? Does pg_upgrade keep LSNs rising? Both are parked routes too.
- [ ] Which executor paths delete or update a row: ExecDelete, ExecUpdate, MERGE's actions, ON CONFLICT DO UPDATE, cross-partition moves, `execReplication.c`. For DESIGN.md §6.
- [ ] Which update result (`TU_UpdateIndexes`) means indexed columns changed, for HOT and for summarizing AMs. For DESIGN.md §6.
- [ ] Must an index AM have `amgettuple` or `amgetbitmap`? `rangeagg` scans nothing.
- [x] Is `ALTER INDEX ... SET (custom option)` taken at the option's own lock level for a custom AM? Yes, read (§3, custom reloptions); not tested. Moot for W, now in the metapage.
- [ ] How core can own hidden heaps per index: creation as in `toasting.c:129`, the namespace, how the index finds them, pg_dump and pg_upgrade. For DESIGN.md §6.
- [ ] Does `WaitForLockers` skip the caller's own locks, and how does autovacuum's work-item loop (`AutoVacuumRequestWork`, `autovacuum.h:66`) run a multi-step job? For DESIGN.md §3.3.

## 11. Where the spec's sources moved on master

The spec checked REL_18_STABLE; this tree is master.

| Spec | Master |
|---|---|
| IOS race comment in `nodeIndexonlyscan.c` before `VM_ALL_VISIBLE` | §1, last item |
| `visibilitymap_clear_locked` | `visibilitymap_clear` (`visibilitymap.c:152`), VM buffer locked by the caller |
| `visibilitymap_set` WAL-logs | §1, second item |
| Only VACUUM sets bits (its mechanism (a)) | §1, on-access pruning; ALTERNATIVES.md §1 |
| BRIN tuple size [to verify] | §3, BRIN |
| Eligible aggregates [to verify per aggregate] | §4 |
| `ambulkdelete` values gone [inferred] | §2 |
