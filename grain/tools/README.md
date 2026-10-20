# grain benchmark scripts

The scripts behind the numbers in the grain design proposal on pgsql-hackers. grain is a proposed index type that keeps exact count, sum, min and max per time bucket. It isn't written yet, so its read path is simulated here in plain SQL: summary tables, a marker table and a btree. The cost of the delete callback it needs (`amnotifydelete`) is measured on a build with that patch.

The scripts are bash and only run steps in order; the SQL is in the `.sql` files next to them. Run a script with no arguments for its usage.

## Build

Two optimized builds at the top of this tree, with no assertions. Build `build-bench` first, from unpatched master:

```
mkdir build-bench && cd build-bench
../configure --prefix=$PWD/inst --without-icu --without-readline --enable-tap-tests=no CFLAGS=-O2
make -j8 -s && make -s install
```

Then apply the callback patch to the tree, and build `build-bench-patch` the same way, plus the test index AM that asks for the callback:

```
mkdir build-bench-patch && cd build-bench-patch
../configure --prefix=$PWD/inst --without-icu --without-readline --enable-tap-tests=no CFLAGS=-O2
make -j8 -s && make -s install
make -s -C src/test/modules/dummy_index_am install
```

Each script makes its own cluster (`initdb`) in the build directory if there is none, starts it if it isn't running, and stops it on exit. Benchmark clusters run with `shared_buffers=2GB`, `autovacuum=off` and `timezone=UTC`.

## Scripts

| Script | Question | Cluster |
|---|---|---|
| `bench_levels.sh OUT SIZE...` | How fast is an hourly or daily aggregate over 30 days, or a total over 365 days, when read from minute, hour and day summaries plus the rows they don't cover, against the fastest plain `GROUP BY` (no index, tuned, covering btree, BRIN)? Every answer is checked against `GROUP BY`, also after late writes. | `build-bench`, port 54330 |
| `bench_delete.sh OUT ROUNDS [--fresh]` | What does the callback cost DELETE and UPDATE: on tables without an index that asks for it, and with one? | `build-bench` and `build-bench-patch`, ports 54330 and 54331 |
| `fuzz_read.sh OUT [--mutants]` | Do grain's write, seal and read rules give the same answer as `GROUP BY` under random late writes, updates, deletes and seals? `--mutants` first checks that it catches nine planted bugs. | `build-bench`, port 54330 |
| `summarize_reps.py` | Median and spread over repeated runs. A [uv](https://docs.astral.sh/uv/) script. | — |

`SECS` sets the seconds per timed run (default 10); `REPEATS=N` repeats `bench_levels.sh` into `OUT/repK`.

## The proposal's numbers

```
./bench_levels.sh out/levels 100000000           # read path, 100 million rows
./bench_delete.sh out/delete 5                   # callback: DELETE and UPDATE workloads
./bench_delete.sh out/fresh 7 --fresh            # callback: one DELETE of 1M rows, fresh table
./summarize_reps.py delete out/delete/results.csv
./summarize_reps.py delete out/fresh/results.csv
./fuzz_read.sh out/fuzz --mutants
```

Each writes `OUT/results.csv` (or `OUT/run.log`), the pgbench logs, and the exact queries it ran.

## Files

- `lib.sh`: shared by the scripts: which cluster, start and stop, psql, pgbench, and `render`, which fills `{{NAME}}` in a `.sql` file from the shell variable `NAME`.
- `levels/`: the table, the plain `GROUP BY` queries, the summaries as a sealer would leave them, five late rows with their markers, and the simulated read (`grain.sql`).
- `delete/`: the 1M-row table, the index that asks for the callback, and the four workloads.
- `fuzz_read.sql`: a model of the index's rules in SQL; its header says what it checks.
