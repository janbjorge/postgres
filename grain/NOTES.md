# grain notes

One dated entry per step: what was built, measured, and what surprised. Numbers come only from command output.

## M0, 2026-10-19: the tree and the build

jb gave the spec (SPEC.md) and asked for its first two deliverables: a design note and a go/no-go.

- **Tree:** master at `0f3b42ce806`. Branch `grain-proto` was made later the same day (jb: "lets start with some basic agg-functions, like sum, count, min, max?").
- **Build:** meson isn't installed, so this is an autoconf VPATH build in `build-ra/`, installed to `build-ra/inst`. Assertions and debug info are on. Build and install exited 0.

  ```
  ../configure --prefix=$PWD/inst --without-icu --without-readline \
      --enable-cassert --enable-debug --enable-tap-tests=no CFLAGS='-O1 -g'
  make -j10 -s && make -s install
  make -s -C contrib/pageinspect install && make -s -C contrib/pg_visibility install
  ```
- **Test cluster:**
  - `build-ra/data`, port 54329, socket in `build-ra/`, `autovacuum=off`;
  - superuser `janbjorge`, set by initdb; the default role `pio` doesn't exist there;
  - started with `pg_ctl -D build-ra/data -l build-ra/server.log -o "-p 54329 -k <abs build-ra> -c listen_addresses='' -c autovacuum=off" -w start`;
  - stopped after use.
- **Surprised:** plain `python3` is a shim that fails ("Unable to determine target Python version"). Use `/usr/bin/python3 -I`.

## M0.1, 2026-10-19: a SELECT sets VM bits, and the VM page LSN catches it

**Question:** on master, can a summary trusted because "all VM bits are set" be stale? Spec mechanism (a) depends on the answer (ALTERNATIVES.md §1).

**How:** tools/vm_lsn_test.sql.
- Table `fillfactor = 90`, autovacuum off, 2000 rows, then VACUUM.
- Then UPDATE `ctid '(0,1)'`, then a read-only `SELECT count(*)`.
- After each step, record block 0's VM bits (pg_visibility), the VM page LSN (pageinspect) and `sum(n)` over block 0.

| Step | VM bits, block 0 (visible/frozen) | VM page LSN | sum(n), block 0 |
|---|---|---|---|
| 1, after VACUUM; a "summary" taken here | true/false | 0/017DD790 | 14028 |
| 2, after the UPDATE on block 0 | false/false | 0/017F4DA8 | 1014028 |
| 3, after a read-only SELECT, no VACUUM | true/false | 0/017F4E18 | 1014028 |
| 4, again, nothing done | true/false | 0/017F4E18 | 1014028 |

- **Answer: yes.** At step 3 the bit is set again with no VACUUM (on-access pruning, RESEARCH.md §1). So a summary from step 1 (14028) would pass "all bits set" while the true sum is 1014028.
- **The VM page LSN moved at every change,** including the clear at step 2.
- **Surprised:** `pg_stat_user_tables.n_tup_hot_upd` read 0 in the same session. Stats weren't flushed yet, so it doesn't say whether the update was HOT.

## M0.2, 2026-10-19: layout numbers (computed, not measured)

From `/usr/bin/python3 -I -c` with the constants in RESEARCH.md §1 and §3:
- `MAPSIZE` 8168 bytes, so `HEAPBLOCKS_PER_PAGE` 32672, and a VM page covers 255.25 MiB of heap.
- `BrinMaxItemSize` 8152 bytes.
- The divisors of 32672 are 1, 2, 4, 8, 16, 32, 1021, ... A 128-block range doesn't divide it: 255.25 ranges per VM page, so ranges can straddle two VM pages.

**Heap sizes for the benchmark,** for a row of timestamptz + int4 + int2: 185 rows per page.

| Rows | Heap | VM pages | 128-block ranges |
|---|---|---|---|
| 10M | 0.4 GiB | 2 | 423 |
| 100M | 4.1 GiB | 17 | 4223 |
| 1B | 41.2 GiB | 166 | 42230 |

**Heap sizes by GiB:**

| Heap | 128-block ranges | VM pages |
|---|---|---|
| 1 GiB | 1024 | 5 |
| 10 GiB | 10240 | 41 |
| 100 GiB | 102400 | 402 |
| 1000 GiB | 1024000 | 4012 |

**Hourly buckets:** 720 in 30 days, 8760 in a year.

## M0.3, 2026-10-19: which built-in aggregates combine

tools/aggs.py parses `src/include/catalog/pg_aggregate.dat`:
- 139 entries have `aggcombinefn` and 26 don't (lists in RESEARCH.md §4).
- **Surprised:** the first parse missed every entry whose `agginitval` holds braces, such as `'{0,0}'`. Its regex now allows quoted text.

## M0.4, 2026-10-19: the design talk (no code)

jb's questions moved the design four times. Each step and its reason is in ALTERNATIVES.md, and the result is DESIGN.md §3. The jb quotes are in DESIGN.md §2 and docs/done.md.

## M1.1, 2026-10-19: patch 0001, the delete callback

**Question:** can every path that deletes or replaces a row tell an opted-in index the old values, at no cost to other tables? It decides whether DESIGN.md §6's core change works as designed.

**Built** (uncommitted, on `grain-proto`):
- `amnotifydelete` in `IndexAmRoutine`, NULL for every existing AM, and `index_notify_delete` (`indexam.c`). Named so it isn't read as deleting index entries (mine).
- `ExecNotifyIndexesOfDelete` (`execIndexing.c`) fetches the old version once and calls each opted-in index, skipping indexes not ready for inserts, non-summarizing ones on a summarizing-only update, and partial indexes the old version wasn't in.
- Called from `ExecDeleteEpilogue`, `ExecUpdateEpilogue` (not on `TU_None`) and both apply paths in `execReplication.c` (RESEARCH.md §6.1).
- **No cost elsewhere:** the relcache caches the columns of opted-in indexes (`INDEX_ATTR_BITMAP_DELETE_NOTIFY`). A DELETE opens the indexes only when that set is non-empty, asked once per statement and result relation.
- Docs in `indexam.sgml`.
- Test: `dummy_index_am` reports each call, and `notify_delete.sql` covers each path (below).

**Tested** (build-ra, assert-enabled):
- `notify_delete` reports, in order:
  - DELETE: 10;
  - UPDATE of the indexed column: 20;
  - nothing on a HOT update of an unindexed column;
  - 30 when another index's column changes the TID;
  - MERGE: 21 (update) and 40 (delete);
  - ON CONFLICT: 22;
  - the partial index only for its row, 150;
  - a partition move from the partition left;
  - nothing on a table without such an index.
- Core's `delete`, `update`, `merge`, `insert_conflict` (with `test_setup`, `create_index`) pass.
- Not run: logical-replication apply (TAP, later), and the SGML check (no DocBook DTD here).

**Surprised:** `build-ra` had been configured without `--enable-depend`. After the struct changes, stale objects made the logical replication launcher crash with signal 11 at startup. Reconfigured with `--enable-depend` and rebuilt clean.

## P1, 2026-10-19: the read ceiling, in plain SQL

**Question** (jb 2026-10-19: "i want proof that this works, and that it brings the insame powerup of that i think it will"): how much faster is a 30-day hourly count/sum/min/max when answered the way grain would answer it? That means stored rows for sealed hours, plus scans of the open hours and the dirty ones. **Decides:** whether building grain in core is worth it.

**Setup:**
- `tools/bench_read.py` (then `.sh`), an optimized master build (`build-bench/`: -O2, no asserts) with shared_buffers 2GB, autovacuum off, timezone UTC, and one pgbench client for 10 s per variant, warm.
- Table `(done_at timestamptz, bytes int4, status int2)`, with rows spread evenly over 90 days in time order. Window: 30 days ending at the open hour. Lag 2 h.
- grain's read path is simulated in SQL: a summary table, a marker table and a btree on `done_at`. It is not grain itself, and it measures no write costs.

**Exactness:** at every size the simulated answer equals the plain `GROUP BY`, both clean and after 5 late rows into sealed hours with their markers. One late row carries a value above every other, so a missed marker would show in `max`. Result: 0 rows differ in all 10 checks.

**Results** (`bench/P1/results.csv`; table printed by `tools/plot_read.py`; chart `bench/P1/p1_read.png`). "Best today" is the fastest of: no index, the same tuned (work_mem 256MB, 8 workers), btree, covering btree, and BRIN.

| Rows | Best today | ms | grain ms | Speedup | grain, 5 dirty ms | Speedup |
|---|---|---|---|---|---|---|
| 1k | GROUP BY, covering btree | 0.242 | 0.545 | 0.4x | 0.395 | 0.6x |
| 100k | GROUP BY, covering btree | 5.127 | 0.916 | 5.6x | 0.615 | 8.3x |
| 1M | GROUP BY, no index, tuned | 69.446 | 1.109 | 62.6x | 1.063 | 65.3x |
| 10M | GROUP BY, no index, tuned | 543.894 | 2.924 | 186.0x | 5.317 | 102.3x |
| 100M | GROUP BY, BRIN | 6752.206 | 27.970 | 241.4x | 55.634 | 121.4x |

The summary table is 160 kB at every size from 100k up; the heap is 4224 MB at 100M.

**Reading** (I):
- Plain `GROUP BY` grows with the rows in the window. No index type helps much: at 100M, BRIN, the covering btree and the tuned seq scan are all near 7 s.
- grain's cost is the open and dirty hours' rows, not the window. It grows only because each hour holds more rows at larger sizes. A shorter `lag` shrinks it.
- Below about 100k rows grain is no faster. The 3-armed SQL costs more than scanning a tiny table.
- The marker check in SQL picked a nested-loop anti join over an empty table, so "clean" ran slower than "dirty" at 1k and 100k. A C reader wouldn't do that.

**Not measured yet:** write cost (M3, needs grain itself), cold cache, concurrency.

## P2, 2026-10-19: levels and a 1-minute lag

**Question** (jb 2026-10-19: "i was hoping for ~ 1000x on the larger 10M and 100M, why are we not hitting that?"): P1's cost was the open 2.5 hours. With levels `1 minute, 1 hour, 1 day` in one index and lag 1 minute, how close does the read path get? **Decides:** whether levels go into M1.

**Setup:** as P1 (`tools/bench_levels.py`, then `.sh`; `build-bench/`), except rows spread evenly over 400 days, levels 1 minute / 1 hour / 1 day, lag 1 minute. Three queries: hourly for 30 days, daily for 30 days, one total for 365 days. The simulated read descends top-down: days, then hours, then minutes at the edges, then raw rows after W and for dirty minutes. The SQL stand-in needed `enable_hashjoin`, `enable_mergejoin` and `enable_seqscan` off to get key lookups (otherwise it hash-joined whole level tables, about 105 ms); a C reader does lookups by design.

**Exactness:** 0 rows differ in all 12 checks (3 queries × clean and 5 dirty minutes × 2 sizes).

**Results** (`bench/P2/results.csv`; table from `tools/plot_levels.py`; chart `bench/P2/p2_levels.png`). "Best today" as in P1.

| Rows | Query | Best today | ms | grain ms | Speedup | grain, 5 dirty ms | Speedup |
|---|---|---|---|---|---|---|---|
| 10M | hourly, 30 days | covering btree | 168.354 | 1.170 | 144x | 1.560 | 108x |
| 10M | daily, 30 days | covering btree | 168.706 | 0.340 | 496x | 0.725 | 233x |
| 10M | total, 365 days | no index, tuned | 135.295 | 0.548 | 247x | 1.100 | 123x |
| 100M | hourly, 30 days | BRIN | 1187.994 | 1.233 | 963x | 2.089 | 569x |
| 100M | daily, 30 days | BRIN | 1196.245 | 0.375 | 3190x | 1.090 | 1097x |
| 100M | total, 365 days | no index, tuned | 909.363 | 0.590 | 1541x | 1.261 | 721x |

Levels are 34 MB at both sizes; the heap is 422 MB at 10M and 4224 MB at 100M.

**Reading** (I):
- grain's time is now flat with table size (1.170 → 1.233 ms hourly), so the speedup grows with the rows. At 100M it reaches about 1000x and above.
- Not comparable to P1 one for one: 400 days instead of 90, so each 30-day window holds fewer rows per size.

**Not measured yet:** write cost (M3), cold cache, concurrency.

## P3, 2026-10-20: how sure are the numbers, up to 1M rows

jb asked "How sure are we about our bechmark numbers, are ther othre tests we can do, lets focus on upto 1M rows for now", and picked three tests ("lets do that").

### A: repeats at 1k, 100k, 1M

**Question:** does P2's read path hold at small sizes, and how noisy is one run? **Decides:** whether the numbers can be quoted; where the planner should skip grain.

**Setup:** `tools/bench_levels.py` (then `.sh`) unchanged (P2's data and queries, `build-bench/`, master -O2), 5 whole repeats, `SECS=5`. Summary by `tools/summarize_reps.py`; raw in `bench/A/rep1..5`.

**Exactness:** 90 checks, 0 differ.

**Noise:** repeats 4 and 5 ran 15% and 39% slower than the median on average, across every variant including plain `GROUP BY`: the machine slowed, not one method. So speedups are taken within each repeat.

| Rows | Query | Best today, median ms | grain, median ms | Speedup, median (min–max over repeats) | 5 dirty minutes |
|---|---|---|---|---|---|
| 1k | hourly, 30 days | 0.076 | 0.953 | 0.1x (0.1–0.1x) | 0.1x (0.1–0.1x) |
| 1k | daily, 30 days | 0.062 | 0.348 | 0.2x (0.2–0.2x) | 0.1x (0.1–0.1x) |
| 1k | total, 365 days | 0.077 | 0.552 | 0.1x (0.1–0.1x) | 0.1x (0.1–0.1x) |
| 100k | hourly, 30 days | 1.053 | 1.181 | 0.9x (0.9–0.9x) | 0.7x (0.5–0.7x) |
| 100k | daily, 30 days | 0.834 | 0.347 | 2.4x (2.4–2.5x) | 1.2x (0.8–1.2x) |
| 100k | total, 365 days | 4.262 | 0.548 | 7.8x (7.7–7.9x) | 3.9x (2.6–3.9x) |
| 1M | hourly, 30 days | 12.294 | 1.192 | 10.3x (10.1–10.6x) | 7.8x (7.4–8.0x) |
| 1M | daily, 30 days | 12.188 | 0.351 | 34.7x (34.4–34.9x) | 16.6x (16.4–16.7x) |
| 1M | total, 365 days | 18.936 | 0.556 | 34.1x (31.8–34.3x) | 17.1x (16.0–17.3x) |

"Best today" was the covering btree in every row but one (1k total: tuned seq scan).

**Reading** (I):
- Within a repeat the speedup varies by a few percent; one 10 s run, as in P1 and P2, is enough for ratios, not for absolute ms.
- The SQL stand-in has a floor of about 0.35–1.2 ms whatever the size; at 1k rows a plain `GROUP BY` takes under 0.1 ms. Below about 100k rows (400 days of data) the planner must not use grain. A C reader's floor is unknown until it exists.
- Lower than P1's 62.6x at 1M because P2's data spreads over 400 days, not 90: a 30-day window holds fewer rows.

### E: randomized check of the write, seal and read rules

**Question:** do the marker rule, the sealer and the top-down reader equal a plain `GROUP BY` for any window, width and write mix? **Decides:** trust in the read logic before it is written in C.

**How:** `tools/fuzz_read.sql`, run per seed by `tools/fuzz_read.py` (`--mutants` adds the planted bugs below), on `build-ra/` (asserts on). One SQL model of the index: W, summary rows per level, markers; a partial predicate `status = 'paid'`; levels 1 minute / 1 hour / 1 day; lag 1 minute. 10000 random operations per seed: new rows (55%), late rows anywhere in the past 3 days (10%, some with a value above all others, some NULL), refunds and un-refunds in and out of the predicate (10%), value changes (7%), moves in time by up to ±3 hours (6%), deletes (8%), seals (4%). About 300 reads per seed, spread over the run: windows aligned and not, widths minute, hour, day and total, many ending in the open tail. Each read compared row by row with `GROUP BY`. Not tested: concurrency (one session), the real executor callbacks.

**Does it catch bugs?** 9 copies, each with one planted bug, all caught on a small run (2000 rows, 2000 operations, 70 reads):

| Planted bug | Bad reads of 70 |
|---|---|
| delete doesn't mark the old row | 61 |
| update doesn't mark the old row | 65 |
| update doesn't mark the new row | 61 |
| writer compares with W one minute early | 15 |
| sealer skips marked minutes | 65 |
| reader ignores markers | 51 |
| reader checks a marker only at a bucket's own start | 46 |
| reader uses a row at the window's edge | 50 |
| reader uses a day row for hourly output | 13 |

**Result** (`bench/E/run.log`): 6 seeds, 5 at 20k rows and 1 at 1M rows; 1847 reads, 90626 buckets checked, 0 differ. 1704 reads ran with markers present (up to 102 at once); 514450 summary rows were used.

### F: what the delete callback costs DELETE and UPDATE

**Question:** is the patched server as fast as master on tables without an index that asks for the callback; and what does the callback cost when one does? **Decides:** whether the design email can say tables without grain pay nothing, and the callback's per-row price.

**Setup:** `tools/bench_delete.py` (then `.sh`): master (`build-bench/`) and the patch (`build-bench-patch/`, same configure, -O2, no asserts), fresh clusters, shared_buffers 2GB, autovacuum off. Table of 1M rows `(id primary key, k indexed, v)`; on the patch also a copy with a `dummy_index_am` index on `v`, which asks for the callback. 5 rounds alternating the servers. Summary by `tools/summarize_reps.py`.

| Workload | master, median ms | patch, median ms | patch vs master | patch, with callback index |
|---|---|---|---|---|
| one row: DELETE + INSERT per transaction | 0.106 | 0.105 | -0.9% | 0.102 |
| one row: UPDATE of an indexed column | 0.072 | 0.070 | -2.8% | 0.074 |
| 1M rows: one UPDATE, rolled back | 4808.265 | 4572.124 | -4.9% | 4590.116 |
| 1M rows: one DELETE, rolled back | 685.974 | 691.932 | +0.9% | 704.512 |

**Surprised:** the rolled-back DELETE ran 407–424 ms in round 1 and 674–724 ms after, on both servers: each run must check the rows' aborted deletes, which hides the callback. So F2.

**F2** (`tools/bench_delete.py --fresh`, then `bench_bulk_delete.sh`; `bench/F2/`): one committed DELETE of 1M rows on a freshly built table each time, 7 rounds.
- Master's first 3 rounds were outliers (845.669, 1015.431, 880.740 ms); from round 4 on, master's median is 463.349 ms and the patch's 469.150 ms, +1.3%.
- With the callback index, against the same round without: median +18.7% (min +10.6%, max +27.1%), a median of 87.5 ms per 1M rows, so about 87.5 ns per row.

**Reading** (I):
- Without an index that asks, the patch is within the noise of master in every workload (-4.9% to +1.3%). The resolution here is a few percent, not zero.
- With one, a bulk DELETE pays about 88 ns per row: fetching the old row again, forming the index values, and the call. One-row workloads don't show it. For grain that is the floor; its own work for a row after W is one comparison.
- The old row is fetched again because DELETE doesn't keep it. If reviewers find 88 ns too much, it could be handed over where the executor already has it.

**Not measured yet:** windows other than three (B), dirty-minute sweep (C), real-shaped orders data (D), write cost of grain itself, cold cache, concurrency.

### Tools rewritten in Python, 2026-10-20

jb: "I hate bash-scripts, they are cancer, is can we re-rewrite these script to use python insted, ideally uv scripts?"

- The four bash scripts are now uv scripts with no dependencies, sharing `tools/pg.py` (server, psql, pgbench): `bench_read.py` (P1), `bench_levels.py` (P2, and A with `--repeats`), `bench_delete.py` (F, and F2 with `--fresh`), `fuzz_read.py` (E's seeds, and the planted bugs with `--mutants`). Same SQL, same CSV columns, so `plot_*.py` and `summarize_reps.py` read old and new results alike.
- Each starts its cluster if it isn't running and stops it after.
- Checked on small inputs: the 9 planted bugs gave the same bad-read counts as before (61, 65, 61, 15, 65, 51, 46, 50, 13 of 70) and seed 0.42 the same totals; every exactness check 0 differ; both plots and both summaries run.
- P1–P3's numbers above came from the bash versions.
