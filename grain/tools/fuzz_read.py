#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""E: run fuzz_read.sql over several seeds, and check it catches planted bugs (NOTES.md P3).

fuzz_read.sql models grain's writer, sealer and reader rules in SQL and
compares every read with a plain GROUP BY; its header says what it answers.
This runs it per seed and totals the results. With --mutants it first runs
copies with one bug planted each, and fails unless every copy finds a
difference.

Usage: fuzz_read.py OUTDIR [--seeds 0.11:20000 ...] [--ops N] [--reads N] [--mutants]
  Uses the build-ra cluster (asserts on) on port 54329; starts and stops it
  if it isn't running.
"""

import argparse
import re
import sys
from pathlib import Path

from pg import Server

HERE = Path(__file__).resolve().parent
SQL = HERE / "fuzz_read.sql"

# One bug each: (name, text in fuzz_read.sql, replacement).
MUTANTS = [
    ("delete doesn't mark the old row",
     "  IF FOUND THEN PERFORM touch(r.ts, r.status); END IF;", "  NULL;"),
    ("update doesn't mark the old row",
     "  PERFORM touch(r.ts, r.status);\n  PERFORM touch(p_ts, p_status);", "  PERFORM touch(p_ts, p_status);"),
    ("update doesn't mark the new row",
     "  PERFORM touch(r.ts, r.status);\n  PERFORM touch(p_ts, p_status);", "  PERFORM touch(r.ts, r.status);"),
    ("writer compares with W one minute early",
     "p_status = 'paid' AND p_ts < meta.w", "p_status = 'paid' AND p_ts < meta.w - interval '1 minute'"),
    ("sealer skips marked minutes",
     "UNION SELECT bucket FROM mark;", "UNION SELECT bucket FROM mark WHERE false;"),
    ("reader ignores markers",
     "NOT EXISTS (SELECT 1 FROM mark WHERE mark.bucket >= vb",
     "NOT EXISTS (SELECT 1 FROM mark WHERE false AND mark.bucket >= vb"),
    ("reader checks a marker only at a bucket's own start",
     "mark.bucket >= vb AND mark.bucket < vb + width(vl)", "mark.bucket = vb"),
    ("reader uses a row at the window's edge",
     "IF vb >= f AND vb + width(vl) <= tt AND", "IF true AND"),
    ("reader uses a day row for hourly output",
     "(wd IS NULL OR width(vl) <= wd)", "true"),
]

SUMMARY = re.compile(r"^\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|"
                     r"\s*(\d+)\s*\|\s*(\d+)\s*$", re.M)
FIELDS = ["reads", "rows_differ", "bad_reads", "buckets_checked", "reads_with_markers",
          "max_markers", "summary_rows_used", "reads_all_scanned"]


def run(pg: Server, sql: str, seed: str, rows: int, ops: int, reads: int) -> tuple[dict, str]:
    out = pg.psql(sql, "-v", f"seed={seed}", "-v", f"rows={rows}", "-v", f"ops={ops}", "-v", f"reads={reads}")
    m = SUMMARY.search(out)
    if m is None:
        raise RuntimeError(f"no summary line in:\n{out}")
    return dict(zip(FIELDS, map(int, m.groups()))), out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--seeds", nargs="+", default=["0.11:20000", "0.22:20000", "0.33:20000",
                                                   "0.44:20000", "0.55:20000", "0.66:1000000"],
                    help="SEED:ROWS pairs")
    ap.add_argument("--ops", type=int, default=10000)
    ap.add_argument("--reads", type=int, default=300)
    ap.add_argument("--mutants", action="store_true")
    a = ap.parse_args()
    a.outdir.mkdir(parents=True, exist_ok=True)
    sql = SQL.read_text()
    log = (a.outdir / "run.log").open("w")

    with Server("build-ra", 54329, options="-c client_min_messages=error").up() as pg:
        if a.mutants:
            missed = []
            for name, old, new in MUTANTS:
                if sql.count(old) != 1:
                    sys.exit(f"mutant {name!r}: its text is in fuzz_read.sql {sql.count(old)} times, not once")
                r, _ = run(pg, sql.replace(old, new), "0.42", 2000, 2000, 60)
                print(f"mutant: {name}: {r['bad_reads']} bad reads of {r['reads']}", file=log, flush=True)
                print(f"  mutant: {name}: {r['bad_reads']} bad reads of {r['reads']}", flush=True)
                if r["bad_reads"] == 0:
                    missed.append(name)
            if missed:
                sys.exit(f"planted bugs not caught: {missed}")

        total = dict.fromkeys(FIELDS, 0)
        for pair in a.seeds:
            seed, rows = pair.split(":")
            r, out = run(pg, sql, seed, int(rows), a.ops, a.reads)
            log.write(f"== seed {seed}, {rows} rows\n{out}\n")
            log.flush()
            print(f"  seed {seed}, {rows} rows: " + ", ".join(f"{k} {v}" for k, v in r.items()), flush=True)
            for k in FIELDS:
                total[k] = max(total[k], r[k]) if k == "max_markers" else total[k] + r[k]
        print("total: " + ", ".join(f"{k} {v}" for k, v in total.items()))
        print("total: " + ", ".join(f"{k} {v}" for k, v in total.items()), file=log)
        if total["rows_differ"]:
            sys.exit("MISMATCH: see run.log")


if __name__ == "__main__":
    main()
