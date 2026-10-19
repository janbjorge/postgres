# rangeagg notes

One dated entry per step: what was built, measured, and what surprised. Numbers come only from command output.

## M0, 2026-10-19: the tree and the build

jb gave the spec (SPEC.md) and asked for its first two deliverables: a design note and a go/no-go.

- **Tree:** master at `0f3b42ce806`. Branch `rangeagg-proto` was made later the same day (jb: "lets start with some basic agg-functions, like sum, count, min, max?").
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
