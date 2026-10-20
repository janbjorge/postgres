# grain — overview

grain is a proposed index type for PostgreSQL that answers time-bucket aggregates (`count`, `sum`, `min`, `max` per minute, hour or day) exactly, from precomputed summaries. This page explains why it exists and how it works.

**Status (2026-10-20):** a design. The read path has been measured with a plain-SQL stand-in (below). The one change it needs in PostgreSQL's index API, a delete callback, is written and tested. The index type itself isn't written yet.

## Why

"Revenue per day for the last year" on an `orders` table is a `GROUP BY` over every paid order of that year. The cost grows with the table, and every call does the same work again.

What PostgreSQL offers today, and what each costs:

| Option | Cost |
|---|---|
| btree or BRIN on `created_at` | still reads every row in the window |
| materialized view | stale until refreshed; a refresh recomputes everything |
| triggers that keep a rollup table | write cost and lock contention on every insert; drift when one is missed |
| TimescaleDB continuous aggregates | an extension and a whole platform, for what is often one query |

**grain:** declare an index once. After that, `count`, `sum`, `min` and `max` per minute, hour or day are answered from a few hundred stored rows plus the last minute or two of the table. Answers are exact for every transaction's snapshot. There are no triggers, and an insert stamped "now" does one comparison. It is built into PostgreSQL, next to BRIN, not shipped as an extension.

## Measured so far

A plain-SQL stand-in for grain's read path, against the fastest plain `GROUP BY` today (sequential scan, tuned scan, covering btree or BRIN, whichever won). Rows spread evenly over 400 days; levels 1 minute / 1 hour / 1 day; lag 1 minute; warm cache; one client. All 12 answers were checked identical to the plain `GROUP BY`.

| Rows | Query | Best today | grain | Speedup | grain, 5 dirty minutes | Speedup |
|---|---|---|---|---|---|---|
| 10M | hourly, 30 days | 168.354 ms | 1.170 ms | 144x | 1.560 ms | 108x |
| 10M | daily, 30 days | 168.706 ms | 0.340 ms | 496x | 0.725 ms | 233x |
| 10M | total, 365 days | 135.295 ms | 0.548 ms | 247x | 1.100 ms | 123x |
| 100M | hourly, 30 days | 1187.994 ms | 1.233 ms | 963x | 2.089 ms | 569x |
| 100M | daily, 30 days | 1196.245 ms | 0.375 ms | 3190x | 1.090 ms | 1097x |
| 100M | total, 365 days | 909.363 ms | 0.590 ms | 1541x | 1.261 ms | 721x |

grain's time stays flat as the table grows, so the speedup grows with it.

Smaller tables, same setup, median of 5 repeats (speedups taken within each repeat varied by a few percent):

| Rows | hourly, 30 days | daily, 30 days | total, 365 days |
|---|---|---|---|
| 1k | 0.1x | 0.2x | 0.1x |
| 100k | 0.9x | 2.4x | 7.8x |
| 1M | 10.3x | 34.7x | 34.1x |

Below about 100k rows a plain `GROUP BY` is faster: it takes under 0.1 ms at 1k rows, while the stand-in has a floor of 0.35–1.2 ms. The planner must skip grain there.

- **Correctness, randomized:** a SQL model of the writer, sealer and reader rules ran 6 random seeds, up to 1M rows: new and late rows, refunds in and out of the partial index, value changes, rows moved in time, deletes and seals, with 1847 reads at random windows and widths. Every read equaled a plain `GROUP BY`. The same check caught each of 9 deliberately planted bugs.
- **Cost of the delete callback:** on tables without an index that uses it, DELETE and UPDATE run as fast as on unpatched PostgreSQL, within a few percent of noise. With one, a bulk DELETE pays about 88 ns per row to fetch the old row and hand it to the index.

Not measured yet: write cost of grain itself, cold cache, concurrency, real-shaped data.

## Use cases

The example is orders and revenue: the answers are money, so exactness visibly matters.

```sql
CREATE INDEX orders_paid_grain ON orders
  USING grain (created_at) INCLUDE (amount_cents)
  WITH (buckets = '1 minute, 1 hour, 1 day')
  WHERE status = 'paid';
```

`created_at` is the time key; `INCLUDE` lists the columns to aggregate; `buckets` lists the levels, finest first; `WHERE` makes it a partial index over paid orders only.

| Use case | What grain does |
|---|---|
| Revenue per day for a year | about 365 day rows, today's hours and minutes, and the open last minute or two read from the table |
| Orders in the last hour | that hour's minute rows plus the open tail |
| Largest order today | `max` from hour rows plus the tail |
| A new order | after the watermark: nothing beyond one comparison |
| A refund of last week's order (`status` → `refunded`) | the old row left the partial index, so its minute is marked dirty; reads scan that one minute until the next seal |
| A backfill of last month | each minute it touches is marked; one seal recomputes them |
| Deleting orders past retention | their minutes are marked; the next seal recomputes them as empty |

Queries stay plain SQL. First through a function, `pg_grain_buckets(index, from, to)`; later the planner uses the index for a plain `GROUP BY date_bin(...)` by itself.

## How it works

The idea: **finished time buckets don't change, so summarize them once.** The few that do change are marked and rescanned, never patched in place.

- **Watermark W:** a timestamp. Buckets before it are **sealed**: they have stored summary rows. Buckets after it are **open** and are read from the table.
- **The sealer** moves W forward, by default one minute behind `now()` (the lag, which absorbs stragglers), and writes summary rows for the buckets it passes.
- **Writers** compare their row's time with W. After W (almost every write): nothing to do. Before W: they insert a **dirty marker** for that minute, in the same transaction.
- **Readers** use a summary row when it is visible and no marker is. Otherwise they scan that bucket's rows.

```
             time ──────────────────────────────────────────────────►
  days       [ day ][ day ][ day ] ... [ day ]
  hours                                       [h][h][h] ... [h]
  minutes                                                       [m][m][m]
  raw rows                                                               |  open tail  |
                                                                         W           now()
             ◄──────────────── sealed: summary rows ───────────────────►◄─ scanned ──►
```

A read descends from coarse to fine: whole days, then the hours at the edges, then the minutes at the edges, then the raw rows after W and in dirty minutes.

### Architecture

```
  ┌──────────────────────────┐   INSERT / UPDATE / DELETE    ┌────────────────────────────┐
  │  Application             │──────────────────────────────►│  Executor                  │
  └──────────────────────────┘                               │  aminsert (new version)    │
              │                                              │  amnotifydelete (old one)  │
              │ SELECT ... GROUP BY date_bin                 └─────────────┬──────────────┘
              ▼                                                            │ before W? marker
  ┌──────────────────────────┐                               ┌─────────────▼──────────────┐
  │  Reader                  │──── one snapshot ────────────►│  grain index               │
  │  pg_grain_buckets, then  │                               │  ├─ metapage: W            │
  │  the planner             │◄──── open + dirty buckets ────│  ├─ summaries heap         │
  └──────────────────────────┘      scanned from the table   │  └─ markers heap           │
                                                             └─────────────▲──────────────┘
  ┌──────────────────────────┐   move W, wait, recompute                   │
  │  Sealer: pg_grain_seal   │─────────────────────────────────────────────┘
  │  (autovacuum later)      │
  └──────────────────────────┘
```

## Storage

**The per-bucket aggregates belong to the index, but not in index pages.** A grain index has three parts, all created, rebuilt and dropped with it:

| Part | Where | Contents |
|---|---|---|
| Watermark W | the index's own first page (its metapage), where BRIN keeps its settings | one timestamp; moved forward only, by the sealer |
| Summaries | a hidden table owned by the index, the way a TOAST table is owned by its table | one row per sealed bucket per level: `(level, bucket, n, nn[], sum[], min[], max[])`, keyed on `(level, bucket)` |
| Dirty markers | a second hidden table owned by the index | `(bucket)` rows at the finest level, insert-only, deleted by the sealer |

- **Why tables and not index pages:** table rows have MVCC visibility (each transaction sees the row versions committed before its snapshot); index entries don't. Exactness rests on one rule: *a bucket is sealed for a reader exactly when the reader's snapshot sees its summary row.* An old snapshot sees the old row version, or none and scans. A plain table gives this for free, along with WAL, crash safety, standbys and freezing.
- **One array element per `INCLUDE` column** in `nn` (non-null count), `sum`, `min` and `max`; `n` is `count(*)`. These are typed values, never serialized aggregate states, so they stay stable across major versions.
- **Empty buckets get a row** with `n = 0`, so a gap in the data isn't mistaken for "not sealed".
- **Size** depends on time span and levels, not on row count: 400 days at 1 minute / 1 hour / 1 day took 34 MB, the same at 10M and 100M rows.
- **Open:** the schema the hidden tables live in, how the index finds them, and how pg_dump and pg_upgrade treat them.

## Model

```
┌───────────┐ 0..* ┌─────────────┐ 1..* ┌────────┐ 1..* ┌───────────────┐
│ Table     │─────►│ grain index │─────►│ Level  │─────►│ Bucket        │
└───────────┘      └──────┬──────┘      └────────┘      └───────┬───────┘
                          │ 1                                   │ 0..1
                          ▼                                     ▼
                   ┌─────────────┐                      ┌───────────────┐
                   │ Watermark W │                      │ Summary row   │
                   └─────────────┘                      │ Dirty markers │
                                                        └───────────────┘
```

| Concept | Type | Description |
|---|---|---|
| grain index | Entity | the declaration: time key, `INCLUDE` columns, levels, optional `WHERE` |
| Level | Value object | one bucket width, such as `1 hour`; each divides the next |
| Bucket | Value object | `[start, start + width)` on one level, aligned to a fixed origin |
| Watermark W | State | buckets before it may be sealed; buckets after it are open |
| Summary row | Entity | the aggregates of one sealed bucket, MVCC-visible |
| Dirty marker | Entity | "this finest bucket changed since it was sealed"; also dirties the coarser buckets around it |
| Sealer | Service | moves W, waits out older writers, recomputes from one snapshot |
| Reader | Service | stitches summary rows and scans into one exact answer |

## Flows

**Write:** lock the table → read W from the metapage → time key after W: done; before W: insert a marker in the same transaction. Inserts reach the index through `aminsert`, as for any index. Deletes, and the old row of an update, reach it through `amnotifydelete`, the new callback grain adds to the index API; no index type hears of deletes today until VACUUM.

**Seal** (`pg_grain_seal`, one at a time per index):
1. Write the new W into the metapage.
2. Wait for every transaction that locked the table before step 1 to end, as `CREATE INDEX CONCURRENTLY` does. New writers aren't blocked; they read the new W.
3. In one snapshot: compute the finest buckets up to W and every marked one, fold them into the coarser levels, replace their summary rows, delete the markers, commit.

**Read:** in one snapshot, for each bucket in the window: summary row visible, bucket fully inside the window, no marker → use the row. Otherwise → aggregate its rows with a range scan on the time key, served by the user's own btree or BRIN.

## Why it is exact

Take a reader whose snapshot S sees a bucket's summary row and no marker for it. The row was written by a seal transaction X.

1. X committed before S was taken, so S sees everything X saw.
2. Suppose S also sees a change to the bucket's rows that X didn't, made by transaction T. Then T took its table lock after the seal's wait (step 2), or the seal would have waited for T. So T read the new W, found its row before it, and wrote a marker in the same transaction. S sees T's change, so S sees the marker too: a contradiction.
3. Aborted writers leave neither rows nor markers. Buckets with no visible row, with a marker, or at a window edge are scanned, which is exact by definition.

Nothing depends on the visibility map, VACUUM or clocks; W only has to move forward.

## Key design decisions

| Decision | Why | Trade-off |
|---|---|---|
| In core, as an index type | users already know how to declare an index; not another extension | needs a new core callback and community review |
| Rescan dirty buckets; never decrement | buckets are small; `min` and `max` can't be decremented anyway | a dirty minute costs one small scan until the next seal |
| Seal with a watermark and a lag | normal writes, stamped "now", do no work | the last minute or two is always scanned |
| Levels in one index (`1 minute, 1 hour, 1 day`) | with one level the open tail dominated reads; levels make reads flat with table size | more summary rows to keep; 34 MB for 400 days |
| A delete callback, `amnotifydelete` | no index hook hears of deletes before VACUUM | an index API change; tables without such an index pay nothing |
| Summaries in hidden MVCC tables | per-snapshot exactness for free | an index owning tables is new in core |
| No triggers | they add write cost and drift | — |
| Fixed-width buckets only | the `date_bin` rule | no months or local-time days for now |

## Not in scope

Approximate answers, percentiles and other holistic aggregates, per-row windows, triggers, an extension, a general time-series platform, and unlogged, temporary or system-catalog tables.
