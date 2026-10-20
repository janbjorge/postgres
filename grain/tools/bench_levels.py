#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""P2 and A: levels (NOTES.md P2, P3). Plain SQL, no grain code.

Question: with minute, hour and day levels and a 1-minute lag, how much
faster than a plain GROUP BY are (1) hourly over 30 days, (2) daily over
30 days, (3) a total over a year? (1) tests the small tail, (2) and (3)
test reading coarse levels. Every answer is checked against GROUP BY.

Usage: bench_levels.py OUTDIR SIZE... [--repeats N]
  With --repeats, each repeat goes to OUTDIR/repK (NOTES.md P3, A).
  Uses the build-bench cluster on port 54330; starts and stops it if it isn't running.
Env: SECS, seconds per variant (default 10).
"""

import argparse
import os
import sys
from pathlib import Path

from pg import BENCH_SETTINGS, Csv, Server, check_equal, plan_scans

O = "timestamptz '2000-01-01 00:00:00+00'"
T_END = "timestamptz '2026-10-19 12:30:17+00'"
W = "timestamptz '2026-10-19 12:29:00+00'"  # date_bin(1 minute, T_END - lag 1 minute)
F_HOUR = "timestamptz '2026-09-19 12:00:00+00'"  # 30 days, on the hour
F_DAY = "timestamptz '2026-09-19 00:00:00+00'"  # 30 days, on the day
F_YEAR = "timestamptz '2025-10-19 00:00:00+00'"  # 365 days, on the day

QUERIES = ["hourly30d", "daily30d", "total365d"]
TUNED = "-c work_mem=256MB -c max_parallel_workers_per_gather=8"

# The planner doesn't know the bucket lists are short and hash-joins whole
# level tables. A C reader looks rows up by key; make the SQL do the same.
GRAIN_PLAN = "-c enable_hashjoin=off -c enable_mergejoin=off -c enable_seqscan=off"


def b0_query(t: str, q: str) -> str:
    """The plain GROUP BY."""
    aggs = "count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx"
    if q == "hourly30d":
        return (f"SELECT date_bin('1 hour', done_at, {O}) AS bucket, {aggs} FROM {t} "
                f"WHERE done_at >= {F_HOUR} AND done_at < {T_END} GROUP BY 1 ORDER BY 1")
    if q == "daily30d":
        return (f"SELECT date_bin('1 day', done_at, {O}) AS bucket, {aggs} FROM {t} "
                f"WHERE done_at >= {F_DAY} AND done_at < {T_END} GROUP BY 1 ORDER BY 1")
    return f"SELECT {F_YEAR} AS bucket, {aggs} FROM {t} WHERE done_at >= {F_YEAR} AND done_at < {T_END}"


def grain_query(t: str, q: str) -> str:
    """grain's read path over three levels.

    Top down: a day row where the output is daily or coarser, the day is
    sealed and has no marker; else its hour rows where sealed and clean; else
    minute rows where sealed and clean; else the rows themselves (the tail
    after W, and every dirty minute). One statement, one snapshot.
    """
    f, outb, daysok = {
        "hourly30d": (F_HOUR, f"date_bin('1 hour', b, {O})", "false"),
        "daily30d": (F_DAY, f"date_bin('1 day', b, {O})", "true"),
        "total365d": (F_YEAR, F_YEAR, "true"),
    }[q]
    return (
        f"SELECT {outb} AS bucket, sum(n)::int8 AS n, sum(s)::int8 AS s, min(mn) AS mn, max(mx) AS mx FROM ("
        f"WITH mk AS (SELECT bucket FROM {t}_dirty WHERE bucket >= {f} AND bucket < {W}), "
        f"dd AS (SELECT d, ({daysok} AND d + interval '1 day' <= {W} AND NOT EXISTS (SELECT 1 FROM mk "
        f"WHERE mk.bucket >= d AND mk.bucket < d + interval '1 day')) AS ok "
        f"FROM generate_series({f}, {T_END} - interval '1 microsecond', interval '1 day') d), "
        f"hh AS (SELECT h, (h + interval '1 hour' <= {W} AND NOT EXISTS (SELECT 1 FROM mk "
        f"WHERE mk.bucket >= h AND mk.bucket < h + interval '1 hour')) AS ok "
        f"FROM dd, generate_series(dd.d, dd.d + interval '23 hours', interval '1 hour') h "
        f"WHERE NOT dd.ok AND h < {T_END}), "
        f"mm AS (SELECT m FROM hh, generate_series(hh.h, hh.h + interval '59 minutes', interval '1 minute') m "
        f"WHERE NOT hh.ok AND m + interval '1 minute' <= {W} AND NOT EXISTS (SELECT 1 FROM mk WHERE mk.bucket = m)) "
        f"SELECT g.bucket AS b, g.n, g.s, g.mn, g.mx FROM {t}_day g JOIN dd ON g.bucket = dd.d WHERE dd.ok "
        f"UNION ALL SELECT g.bucket, g.n, g.s, g.mn, g.mx FROM {t}_hour g JOIN hh ON g.bucket = hh.h WHERE hh.ok "
        f"UNION ALL SELECT g.bucket, g.n, g.s, g.mn, g.mx FROM {t}_min g JOIN mm ON g.bucket = mm.m "
        f"UNION ALL SELECT date_bin('1 minute', done_at, {O}), count(*), sum(bytes), min(bytes), max(bytes) "
        f"FROM {t} WHERE done_at >= {W} AND done_at < {T_END} GROUP BY 1 "
        f"UNION ALL SELECT k.bucket, count(*), sum(r.bytes), min(r.bytes), max(r.bytes) "
        f"FROM (SELECT DISTINCT bucket FROM mk) k JOIN {t} r "
        f"ON r.done_at >= k.bucket AND r.done_at < k.bucket + interval '1 minute' GROUP BY 1"
        f") x GROUP BY 1 HAVING sum(n) > 0 ORDER BY 1"
    )


def one_run(pg: Server, out: Path, sizes: list[int], secs: int) -> None:
    out.mkdir(parents=True, exist_ok=True)
    results = Csv(out / "results.csv", ["rows", "query", "variant", "latency_ms", "transactions", "plan"])

    def run(n: int, q: str, variant: str, extra: str = "") -> None:
        f = out / f"q_{n}_{q}_{'grain' if variant.startswith('grain') else 'b0'}.sql"
        pg.pgbench(f, out / "warmup.log", secs=2, extra=extra)
        lat, tx = pg.pgbench(f, out / f"pgbench_{n}_{q}_{variant}.log", secs=secs, extra=extra)
        plan = plan_scans(pg, f.read_text(), out / f"plan_{n}_{q}_{variant}.txt", extra=extra)
        results.row(n, q, variant, lat, tx, plan)
        print(f"  {q} {variant}: {lat} ms ({tx} runs)", flush=True)

    def check(n: int, q: str, label: str) -> None:
        rows, differ = check_equal(pg, b0_query(f"lv_{n}", q), grain_query(f"lv_{n}", q))
        results.row(n, q, f"check_{label}", "", "", f"{rows} buckets; {differ} differ")
        print(f"  check {q} {label}: {rows} buckets, {differ} differ", flush=True)
        if differ:
            sys.exit(f"MISMATCH at {n} {q} {label}")

    for n in sizes:
        t = f"lv_{n}"
        print(f"== {n} rows", flush=True)
        pg.psql(f"""
            DROP TABLE IF EXISTS {t}, {t}_min, {t}_hour, {t}_day, {t}_dirty;
            CREATE TABLE {t} (done_at timestamptz NOT NULL, bytes int4 NOT NULL, status int2 NOT NULL);
            SELECT setseed(0.42);
            INSERT INTO {t}
              SELECT {T_END} - interval '400 days' + (i - 1) * (interval '400 days' / {n}),
                     (random() * 1000000)::int4, (i % 5)::int2
              FROM generate_series(1, {n}) i;
            VACUUM (ANALYZE) {t};
        """)
        for q in QUERIES:
            (out / f"q_{n}_{q}_b0.sql").write_text(b0_query(t, q) + "\n")
            (out / f"q_{n}_{q}_grain.sql").write_text(grain_query(t, q) + "\n")

        for q in QUERIES:
            run(n, q, "b0_seq")
        for q in QUERIES:
            run(n, q, "b0_tuned", TUNED)
        pg.psql(f"CREATE INDEX {t}_cov ON {t} (done_at) INCLUDE (bytes); VACUUM (ANALYZE) {t};")
        for q in QUERIES:
            run(n, q, "b0_covering")
        pg.psql(f"DROP INDEX {t}_cov; CREATE INDEX {t}_brin ON {t} USING brin (done_at); ANALYZE {t};")
        for q in QUERIES:
            run(n, q, "b1_brin")

        # grain's state, as the sealer would leave it: a row for every sealed
        # minute (empty ones with n = 0), hours folded from minutes, days from
        # hours; no markers.
        pg.psql(f"""
            DROP INDEX {t}_brin;
            CREATE INDEX {t}_bt ON {t} (done_at);
            CREATE TABLE {t}_min (bucket timestamptz PRIMARY KEY, n int8, s int8, mn int4, mx int4);
            CREATE TABLE {t}_hour (LIKE {t}_min INCLUDING ALL);
            CREATE TABLE {t}_day (LIKE {t}_min INCLUDING ALL);
            CREATE TABLE {t}_dirty (bucket timestamptz NOT NULL);
            CREATE INDEX ON {t}_dirty (bucket);
            INSERT INTO {t}_min
              SELECT m, coalesce(a.n, 0), coalesce(a.s, 0), a.mn, a.mx
              FROM generate_series(date_bin('1 minute', {T_END} - interval '400 days', {O}),
                                   {W} - interval '1 minute', interval '1 minute') m
              LEFT JOIN (SELECT date_bin('1 minute', done_at, {O}) AS b, count(*) AS n, sum(bytes) AS s,
                                min(bytes) AS mn, max(bytes) AS mx
                         FROM {t} WHERE done_at < {W} GROUP BY 1) a ON a.b = m;
            INSERT INTO {t}_hour
              SELECT date_bin('1 hour', bucket, {O}), sum(n), sum(s), min(mn), max(mx) FROM {t}_min
              GROUP BY 1 HAVING date_bin('1 hour', bucket, {O}) + interval '1 hour' <= {W};
            INSERT INTO {t}_day
              SELECT date_bin('1 day', bucket, {O}), sum(n), sum(s), min(mn), max(mx) FROM {t}_hour
              GROUP BY 1 HAVING date_bin('1 day', bucket, {O}) + interval '1 day' <= {W};
            VACUUM (ANALYZE) {t}, {t}_min, {t}_hour, {t}_day, {t}_dirty;
        """)
        for q in QUERIES:
            check(n, q, "clean")
            run(n, q, "grain_clean", GRAIN_PLAN)

        # Five late rows into five sealed minutes, each with its marker, as
        # aminsert would write; the hour and day rolled up above are now stale.
        # One carries bytes above every other row, so a missed marker shows in max.
        late = f"{F_HOUR} + k * interval '100 hours' + interval '30 minutes 17 seconds'"
        pg.psql(f"""
            INSERT INTO {t} SELECT {late}, 7777777, 0 FROM generate_series(1, 5) k;
            INSERT INTO {t}_dirty SELECT date_bin('1 minute', {late}, {O}) FROM generate_series(1, 5) k;
            ANALYZE {t}, {t}_dirty;
        """)
        for q in QUERIES:
            check(n, q, "dirty5")
            run(n, q, "grain_dirty5", GRAIN_PLAN)

        sizes_ = pg.value(f"SELECT pg_size_pretty(pg_table_size('{t}')) || '|' || pg_size_pretty("
                          f"pg_table_size('{t}_min') + pg_table_size('{t}_hour') + pg_table_size('{t}_day'))")
        print(f"  sizes heap|levels: {sizes_}", flush=True)
        pg.psql(f"DROP TABLE {t}, {t}_min, {t}_hour, {t}_day, {t}_dirty;")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("sizes", type=int, nargs="+")
    ap.add_argument("--repeats", type=int)
    a = ap.parse_args()
    secs = int(os.environ.get("SECS", "10"))
    with Server("build-bench", 54330).up(BENCH_SETTINGS) as pg:
        if a.repeats is None:
            one_run(pg, a.outdir, a.sizes, secs)
        else:
            for k in range(1, a.repeats + 1):
                print(f"### rep {k}", flush=True)
                one_run(pg, a.outdir / f"rep{k}", a.sizes, secs)


if __name__ == "__main__":
    main()
