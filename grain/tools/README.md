# grain benchmarks

Scripts behind the numbers in the grain design proposal on pgsql-hackers.

## Build

An optimized build of master, without assertions, in `build-bench/` at the top of the tree:

```
mkdir build-bench && cd build-bench
../configure --prefix=$PWD/inst --without-icu --without-readline --enable-tap-tests=no CFLAGS=-O2
make -j10 -s && make -s install
```

## Read path: `bench_levels.sh`

Simulates grain's read path in plain SQL, with summary tables for minute, hour and day levels, a marker table, and a btree on the time key. There is no grain code in it. It compares that against the fastest plain `GROUP BY` with no index, a tuned no-index run, a covering btree, and BRIN. Every answer is checked against the plain `GROUP BY`, both clean and after five late rows into sealed minutes.

```
build-bench/inst/bin/initdb -D build-bench/data
build-bench/inst/bin/pg_ctl -D build-bench/data -l build-bench/server.log -w start \
    -o "-p 54330 -k $PWD/build-bench -c listen_addresses='' -c shared_buffers=2GB -c autovacuum=off -c timezone=UTC"
grain/tools/bench_levels.sh out/levels 10000000 100000000
grain/tools/plot_levels.py out/levels/results.csv out/levels/levels.png
```

## Delete callback: `bench_delete.sh`, `bench_bulk_delete.sh`

These compare master (`build-bench/`, port 54330) with a build of the delete callback patch (`build-bench-patch/`, port 54331, same configure). The table with the callback uses `dummy_index_am`, which asks for it. The patch isn't public yet, so these two can't be rerun until it is.

```
grain/tools/bench_delete.sh out/delete 5
grain/tools/bench_bulk_delete.sh out/bulk 7
python3 grain/tools/summarize_reps.py delete out/delete/results.csv
```
