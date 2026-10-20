# grain tools

Benchmarks and checks for grain. Each is a uv script (`#!/usr/bin/env -S uv run --script`) and is run directly, for example `./bench_levels.py OUT 1000000`. None needs a package beyond the standard library, except the plots (matplotlib). The results, and what each run answered, are in [../NOTES.md](../NOTES.md).

## Builds and clusters

| Build | What | Cluster | Used by |
|---|---|---|---|
| `build-ra/` | master + patch, asserts on, `-O1` | `data`, port 54329 | `fuzz_read.py` |
| `build-bench/` | master, `-O2`, no asserts | `data` port 54330; `data-f` port 54330 | `bench_read.py`, `bench_levels.py`, `bench_delete.py` (master side) |
| `build-bench-patch/` | master + patch, `-O2`, no asserts | `data-f`, port 54331 | `bench_delete.py` (patch side) |

Configure lines are in NOTES.md M0 and P3. Benchmark clusters run with `shared_buffers=2GB`, `autovacuum=off`, `timezone=UTC`. Every script starts the cluster it needs if it isn't running, and stops it afterwards; a cluster you started yourself is left running.

## Scripts

| Script | Question | Run | Output |
|---|---|---|---|
| `bench_read.py` | P1: one level of hour summaries vs plain `GROUP BY`, 30-day hourly | `./bench_read.py OUT SIZE...` | `OUT/results.csv`, pgbench logs, plans |
| `bench_levels.py` | P2 and A: minute / hour / day levels vs plain `GROUP BY`, three queries; every answer checked | `./bench_levels.py OUT SIZE... [--repeats N]` | `OUT/results.csv`, or `OUT/repK/` per repeat |
| `bench_delete.py` | F and F2: what the delete callback costs DELETE and UPDATE, master vs patch | `./bench_delete.py OUT ROUNDS [--fresh]` | `OUT/results.csv` |
| `fuzz_read.py` | E: random writes, seals and reads against grain's rules in SQL, each read compared with `GROUP BY` | `./fuzz_read.py OUT [--seeds SEED:ROWS ...] [--mutants]` | `OUT/run.log`; exits non-zero on any difference or uncaught planted bug |
| `summarize_reps.py` | median and spread over repeats | `./summarize_reps.py levels OUT/rep*/results.csv`, `./summarize_reps.py delete OUT/results.csv` | a Markdown table |
| `plot_read.py`, `plot_levels.py` | P1 and P2 charts and tables | `./plot_levels.py RESULTS_CSV OUT_PNG` | PNG and a Markdown table |
| `pg.py` | shared: a build's server, `psql`, `pgbench`, start and stop | imported | — |
| `fuzz_read.sql` | the SQL model `fuzz_read.py` runs; its header says what it checks | via `fuzz_read.py` | — |
| `aggs.py` | M0.3: which built-in aggregates have a combine function | `./aggs.py src/include/catalog/pg_aggregate.dat` | text |
| `vm_lsn_test.sql` | M0.1: does a SELECT set visibility-map bits | `psql -f` | text |

`SECS` sets seconds per timed variant (default 10) for the `bench_*` scripts.

## Rules

- Before a run, say what it answers and what decision depends on it; write it up in NOTES.md.
- Python only; no bash (AGENTS.md).
- Stop every cluster after use.
