# grain tools

Benchmarks and checks for grain. The scripts are bash and only run things in order; the SQL they run is in plain, commented `.sql` files next to them, one directory per script. Run them directly, for example `./bench_levels.sh OUT 1000000`; run one with no arguments for its usage. The results, and what each run answered, are in [../NOTES.md](../NOTES.md).

## Builds and clusters

| Build | What | Cluster | Used by |
|---|---|---|---|
| `build-ra/` | master + patch, asserts on, `-O1` | `data`, port 54329 | `fuzz_read.sh` |
| `build-bench/` | master, `-O2`, no asserts | `data`, port 54330; `data-f`, port 54330 | `bench_read.sh`, `bench_levels.sh`; `bench_delete.sh` (master side) |
| `build-bench-patch/` | master + patch, `-O2`, no asserts | `data-f`, port 54331 | `bench_delete.sh` (patch side) |

Configure lines are in NOTES.md M0 and P3. Benchmark clusters run with `shared_buffers=2GB`, `autovacuum=off`, `timezone=UTC`. Every script starts the cluster it needs if it isn't running, and stops it on exit; a cluster you started yourself is left running.

## Scripts

| Script | Question | Run | SQL | Output |
|---|---|---|---|---|
| `bench_read.sh` | P1: one level of hour summaries vs plain `GROUP BY`, 30-day hourly | `./bench_read.sh OUT SIZE...` | `read/` | `OUT/results.csv`, pgbench logs, plans |
| `bench_levels.sh` | P2 and A: minute / hour / day levels vs plain `GROUP BY`, three queries; every answer checked | `[REPEATS=N] ./bench_levels.sh OUT SIZE...` | `levels/` | `OUT/results.csv`, or `OUT/repK/` per repeat |
| `bench_delete.sh` | F and F2: what the delete callback costs DELETE and UPDATE, master vs patch | `./bench_delete.sh OUT ROUNDS [--fresh]` | `delete/` | `OUT/results.csv` |
| `fuzz_read.sh` | E: random writes, seals and reads against grain's rules in SQL, each read compared with `GROUP BY` | `[SEEDS="SEED:ROWS ..."] ./fuzz_read.sh OUT [--mutants]` | `fuzz_read.sql` | `OUT/run.log`; fails on any difference or uncaught planted bug |
| `summarize_reps.py` | median and spread over repeats | `./summarize_reps.py levels OUT/rep*/results.csv`, `./summarize_reps.py delete OUT/results.csv` | — | a Markdown table |
| `plot_read.py`, `plot_levels.py` | P1 and P2 charts and tables | `./plot_levels.py RESULTS_CSV OUT_PNG` | — | PNG and a Markdown table |
| `aggs.py` | M0.3: which built-in aggregates have a combine function | `./aggs.py src/include/catalog/pg_aggregate.dat` | — | text |
| `vm_lsn_test.sql` | M0.1: does a SELECT set visibility-map bits | `psql -f` | — | text |

`SECS` sets seconds per timed variant (default 10) for the `bench_*` scripts. The `.py` scripts are uv scripts (standard library only, except matplotlib for the plots); they do the arithmetic.

## How the scripts are built

- `lib.sh`, sourced by every script: `use BUILD PORT [DATA]` points at a cluster, `up` starts it (stopped on exit), `sql` is psql, `render FILE` fills `{{NAME}}` in a `.sql` file from the shell variable `NAME`, `timed KEY QUERY` warms up, runs pgbench for `SECS` and saves the plan, `same KEY A B` checks two queries return the same rows.
- A script's constants (now, the watermark, the window) are at its top, exported, and used in the SQL as `{{T_END}}`, `{{W}}` and so on.
- The rendered queries are saved in `OUT/q_*.sql`, so every run shows exactly what it timed.

## Rules

- Before a run, say what it answers and what decision depends on it; write it up in NOTES.md.
- SQL goes in `.sql` files, not in strings in the scripts.
- Stop every cluster after use.
