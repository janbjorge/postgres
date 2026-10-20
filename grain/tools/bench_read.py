#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""P1: the read ceiling (NOTES.md P1). Plain SQL, no grain code.

Question: how much faster is a 30-day hourly count/sum/min/max when answered
the way grain would (stored rows for sealed hours, a scan of the open hours
and of dirty hours) than by a plain GROUP BY over the rows?

Usage: bench_read.py OUTDIR SIZE...
  Uses the build-bench cluster on port 54330; starts and stops it if it isn't running.
Env: SECS, seconds per variant (default 10).
"""

import os
import sys
from pathlib import Path

from pg import BENCH_SETTINGS, Csv, Server, check_equal, plan_scans

T_END = "timestamptz '2026-10-19 12:30:00+00'"
FROM = "timestamptz '2026-09-19 12:00:00+00'"  # 30 days back, on the hour
W = "timestamptz '2026-10-19 10:00:00+00'"  # date_bin(1 hour, T_END - 2 hours)
ORIGIN = "timestamptz '2000-01-01 00:00:00+00'"
TUNED = "-c work_mem=256MB -c max_parallel_workers_per_gather=8"


def main(pg: Server, out: Path, sizes: list[int]) -> None:
    secs = int(os.environ.get("SECS", "10"))
    out.mkdir(parents=True, exist_ok=True)
    results = Csv(out / "results.csv", ["rows", "variant", "latency_ms", "transactions", "plan"])

    def run(n: int, variant: str, query: Path, extra: str = "") -> None:
        pg.pgbench(query, out / "warmup.log", secs=2, extra=extra)
        lat, tx = pg.pgbench(query, out / f"pgbench_{n}_{variant}.log", secs=secs, extra=extra)
        plan = plan_scans(pg, query.read_text(), out / f"plan_{n}_{variant}.txt", extra=extra)
        results.row(n, variant, lat, tx, plan)
        print(f"  {variant}: {lat} ms ({tx} runs) {plan}", flush=True)

    def check(n: int, label: str, b0: str, grain: str) -> None:
        rows, differ = check_equal(pg, b0, grain)
        results.row(n, f"check_{label}", "", "", f"{rows} buckets; {differ} differ")
        print(f"  check {label}: {rows} buckets, {differ} differ", flush=True)
        if differ:
            sys.exit(f"MISMATCH at {n} {label}")

    for n in sizes:
        t = f"downloads_{n}"
        print(f"== {n} rows", flush=True)
        pg.psql(f"""
            DROP TABLE IF EXISTS {t}, {t}_summary, {t}_dirty;
            CREATE TABLE {t} (done_at timestamptz NOT NULL, bytes int4 NOT NULL, status int2 NOT NULL);
            SELECT setseed(0.42);
            INSERT INTO {t}
              SELECT {T_END} - interval '90 days' + (i - 1) * (interval '90 days' / {n}),
                     (random() * 1000000)::int4, (i % 5)::int2
              FROM generate_series(1, {n}) i;
            VACUUM (ANALYZE) {t};
        """)

        b0 = (f"SELECT date_bin('1 hour', done_at, {ORIGIN}) AS bucket, count(*) AS n, sum(bytes) AS s, "
              f"min(bytes) AS mn, max(bytes) AS mx FROM {t} WHERE done_at >= {FROM} AND done_at < {T_END} "
              f"GROUP BY 1 ORDER BY 1")
        qb0 = out / f"q_{n}_b0.sql"
        qb0.write_text(b0 + "\n")

        run(n, "b0_seq", qb0)
        run(n, "b0_tuned", qb0, TUNED)
        pg.psql(f"CREATE INDEX {t}_bt ON {t} (done_at); ANALYZE {t};")
        run(n, "b0_btree", qb0)
        pg.psql(f"DROP INDEX {t}_bt; CREATE INDEX {t}_cov ON {t} (done_at) INCLUDE (bytes); VACUUM (ANALYZE) {t};")
        run(n, "b0_covering", qb0)
        pg.psql(f"DROP INDEX {t}_cov; CREATE INDEX {t}_brin ON {t} USING brin (done_at); ANALYZE {t};")
        run(n, "b1_brin", qb0)

        # grain's state, built as the sealer would: a row for every hour below
        # W (empty ones with n = 0), and an empty marker table.
        pg.psql(f"""
            DROP INDEX {t}_brin;
            CREATE INDEX {t}_bt ON {t} (done_at);
            CREATE TABLE {t}_summary (bucket timestamptz PRIMARY KEY, n int8, s int8, mn int4, mx int4);
            CREATE TABLE {t}_dirty (bucket timestamptz NOT NULL);
            INSERT INTO {t}_summary
              SELECT h, count(t.done_at), coalesce(sum(t.bytes), 0), min(t.bytes), max(t.bytes)
              FROM generate_series(date_bin('1 hour', {T_END} - interval '90 days', {ORIGIN}),
                                   {W} - interval '1 hour', interval '1 hour') h
              LEFT JOIN {t} t ON t.done_at >= h AND t.done_at < h + interval '1 hour'
              GROUP BY h;
            VACUUM (ANALYZE) {t}, {t}_summary, {t}_dirty;
        """)

        # grain's read path: clean sealed hours from their rows, the open hours
        # and the dirty hours from the table, in one statement (one snapshot).
        grain = (f"SELECT bucket, n, s, mn, mx FROM (SELECT s.bucket, s.n, s.s, s.mn, s.mx FROM {t}_summary s "
                 f"WHERE s.bucket >= {FROM} AND s.bucket < {W} AND s.n > 0 AND NOT EXISTS "
                 f"(SELECT 1 FROM {t}_dirty d WHERE d.bucket = s.bucket) UNION ALL "
                 f"SELECT date_bin('1 hour', done_at, {ORIGIN}), count(*), sum(bytes), min(bytes), max(bytes) "
                 f"FROM {t} WHERE done_at >= {W} AND done_at < {T_END} GROUP BY 1 UNION ALL "
                 f"SELECT d.bucket, count(*), sum(t.bytes), min(t.bytes), max(t.bytes) FROM "
                 f"(SELECT DISTINCT bucket FROM {t}_dirty WHERE bucket >= {FROM} AND bucket < {W}) d "
                 f"JOIN {t} t ON t.done_at >= d.bucket AND t.done_at < d.bucket + interval '1 hour' "
                 f"GROUP BY 1) x ORDER BY 1")
        qg = out / f"q_{n}_grain.sql"
        qg.write_text(grain + "\n")

        check(n, "clean", b0, grain)
        run(n, "grain_clean", qg)

        # Five late rows into five sealed hours, each with its marker, as
        # aminsert would write. One has bytes above every other row, so a
        # missed marker would show in max().
        pg.psql(f"""
            INSERT INTO {t}
              SELECT {FROM} + k * interval '100 hours' + interval '30 minutes', 7777777, 0
              FROM generate_series(1, 5) k;
            INSERT INTO {t}_dirty
              SELECT date_bin('1 hour', {FROM} + k * interval '100 hours' + interval '30 minutes', {ORIGIN})
              FROM generate_series(1, 5) k;
            ANALYZE {t}, {t}_dirty;
        """)
        check(n, "dirty5", b0, grain)
        run(n, "grain_dirty5", qg)

        sizes_ = pg.value(f"SELECT pg_size_pretty(pg_table_size('{t}')) || '|' || "
                          f"pg_size_pretty(pg_table_size('{t}_summary'))")
        print(f"  sizes heap|summary: {sizes_}", flush=True)
        pg.psql(f"DROP TABLE {t}, {t}_summary, {t}_dirty;")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with Server("build-bench", 54330).up(BENCH_SETTINGS) as pg:
        main(pg, Path(sys.argv[1]), [int(s) for s in sys.argv[2:]])
