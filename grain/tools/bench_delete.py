#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""F and F2: what the delete callback (patch 0001) costs DELETE and UPDATE (NOTES.md P3).

Question: (1) on a table with no index that asks for the callback, is the
patched server as fast as master? (2) With one index that asks for it
(dummy_index_am), what does fetching the old row and calling the index cost
per row? Decides: whether "tables without such an index pay nothing" can be
claimed, and what the callback adds for grain.

master is build-bench/ (port 54330); the patch is build-bench-patch/ (port
54331); both -O2 without asserts, same settings, own clusters in data-f.
Rounds alternate between the two so drift hits both.

Default (F): four pgbench workloads on a 1M-row table. The bulk ones roll
back and repeat on the same table, so later runs also pay for checking the
aborted deletes.
--fresh (F2): one committed DELETE of 1M rows on a freshly built table each
time, which shows the callback's per-row price.

Usage: bench_delete.py OUTDIR ROUNDS [--fresh]
Env: SECS, seconds per one-row workload (default 10).
"""

import argparse
import os
from pathlib import Path

from pg import BENCH_SETTINGS, Csv, Server, find

ROWS = 1_000_000
SERVERS = {"master": Server("build-bench", 54330, data="data-f"),
           "patch": Server("build-bench-patch", 54331, data="data-f")}
SETTINGS = f"{BENCH_SETTINGS} -c max_wal_size=8GB"

# bulk_*: one statement over every row, rolled back, so the table stays the
# same. row_*: one row per transaction, kept the same size.
WORKLOADS = {
    "bulk_delete": "BEGIN;\nDELETE FROM TBL;\nROLLBACK;\n",
    "bulk_update": "BEGIN;\nUPDATE TBL SET k = k + 1;\nROLLBACK;\n",
    "row_delete_insert": ("\\set id random(1, 1000000)\nBEGIN;\nDELETE FROM TBL WHERE id = :id;\n"
                          "INSERT INTO TBL VALUES (:id, :id, :id);\nCOMMIT;\n"),
    "row_update": "\\set id random(1, 1000000)\nUPDATE TBL SET k = k + 1 WHERE id = :id;\n",
}


def create(pg: Server, table: str, notify: bool) -> None:
    """A 1M-row table; with notify, an index whose AM asks for the callback
    (dummy_index_am, installed only in the patched build)."""
    idx = (f"CREATE EXTENSION IF NOT EXISTS dummy_index_am; "
           f"CREATE INDEX ON {table} USING dummy_index_am (v);") if notify else ""
    pg.psql(f"""
        DROP TABLE IF EXISTS {table};
        CREATE TABLE {table} (id int4 PRIMARY KEY, k int4 NOT NULL, v int4 NOT NULL);
        INSERT INTO {table} SELECT i, i, i FROM generate_series(1, {ROWS}) i;
        CREATE INDEX ON {table} (k);
        {idx}
        VACUUM (ANALYZE) {table};
    """)


def workloads(out: Path, rounds: int, results: Csv, secs: int) -> None:
    create(SERVERS["master"], "fd_plain", False)
    create(SERVERS["patch"], "fd_plain", False)
    create(SERVERS["patch"], "fd_notify", True)

    def run(r: int, s: str, table: str, w: str) -> None:
        pg = SERVERS[s]
        script = out / f"{w}_{table}.sql"
        script.write_text(WORKLOADS[w].replace("TBL", table))
        pg.psql(f"VACUUM {table};")
        pg.psql("CHECKPOINT;")
        log = out / f"pgbench_{r}_{s}_{table}_{w}.log"
        if w.startswith("bulk_"):
            lat, tx = pg.pgbench(script, log, transactions=3)
        else:
            lat, tx = pg.pgbench(script, log, secs=secs)
        results.row(r, s, f"{w}_{table}", lat, tx)
        print(f"  round {r} {s} {w}_{table}: {lat} ms ({tx})", flush=True)

    for r in range(1, rounds + 1):
        for w in WORKLOADS:
            run(r, "master", "fd_plain", w)
            run(r, "patch", "fd_plain", w)
            run(r, "patch", "fd_notify", w)


def fresh(rounds: int, results: Csv) -> None:
    def run(r: int, s: str, notify: bool) -> None:
        pg = SERVERS[s]
        create(pg, "fd_bulk", notify)
        pg.psql("CHECKPOINT;")
        ms = find(r"Time: ([0-9.]+) ms", pg.psql("\\timing on\nDELETE FROM fd_bulk;\n"))
        w = "bulk_delete_fresh_fd_notify" if notify else "bulk_delete_fresh_fd_plain"
        results.row(r, s, w, ms, 1)
        print(f"  round {r} {s} {w}: {ms} ms", flush=True)

    for r in range(1, rounds + 1):
        run(r, "master", False)
        run(r, "patch", False)
        run(r, "patch", True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("rounds", type=int)
    ap.add_argument("--fresh", action="store_true")
    a = ap.parse_args()
    a.outdir.mkdir(parents=True, exist_ok=True)
    results = Csv(a.outdir / "results.csv", ["round", "server", "workload", "latency_ms", "transactions"])

    with SERVERS["master"].up(SETTINGS), SERVERS["patch"].up(SETTINGS):
        if a.fresh:
            fresh(a.rounds, results)
        else:
            workloads(a.outdir, a.rounds, results, int(os.environ.get("SECS", "10")))
    print("done")


if __name__ == "__main__":
    main()
