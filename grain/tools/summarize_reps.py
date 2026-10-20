#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Median and spread over repeated benchmark runs.

A: summarize_reps.py levels CSV...   (bench_levels.py results, one per repeat)
F: summarize_reps.py delete CSV      (bench_delete.py results)
"""

import csv
import statistics
import sys
from collections import defaultdict

TODAY = ["b0_seq", "b0_tuned", "b0_covering", "b1_brin"]


def levels(paths):
    lat = defaultdict(list)
    for p in paths:
        for row in csv.DictReader(open(p)):
            if row["latency_ms"]:
                lat[(int(row["rows"]), row["query"], row["variant"])].append(float(row["latency_ms"]))
    med = {k: statistics.median(v) for k, v in lat.items()}
    sizes = sorted({k[0] for k in lat})
    queries = ["hourly30d", "daily30d", "total365d"]
    print(f"runs per cell: {sorted({len(v) for v in lat.values()})}")
    print("| Rows | Query | Best today | median ms (min–max) | grain median ms (min–max) | Speedup (worst–best) "
          "| grain 5 dirty median ms (min–max) | Speedup (worst–best) |")
    print("|---|---|---|---|---|---|---|---|")
    for n in sizes:
        for q in queries:
            v = min(TODAY, key=lambda v: med[(n, q, v)])
            b = lat[(n, q, v)]
            cells = []
            for g in ("grain_clean", "grain_dirty5"):
                x = lat[(n, q, g)]
                cells.append(f"{statistics.median(x):.3f} ({min(x):.3f}–{max(x):.3f}) | "
                             f"{statistics.median(b) / statistics.median(x):.1f}x "
                             f"({min(b) / max(x):.1f}–{max(b) / min(x):.1f}x)")
            print(f"| {n} | {q} | {v} | {statistics.median(b):.3f} ({min(b):.3f}–{max(b):.3f}) | "
                  + " | ".join(cells) + " |")


def delete(path):
    lat = defaultdict(list)
    for row in csv.DictReader(open(path)):
        if row["latency_ms"]:
            lat[(row["workload"], row["server"])].append(float(row["latency_ms"]))
    work = sorted({w for w, _ in lat})
    print(f"runs per cell: {sorted({len(v) for v in lat.values()})}")
    print("| Workload | master median ms (min–max) | patch median ms (min–max) | patch vs master |")
    print("|---|---|---|---|")
    for w in work:
        m, p = lat.get((w, "master")), lat.get((w, "patch"))
        fm = f"{statistics.median(m):.3f} ({min(m):.3f}–{max(m):.3f})" if m else "—"
        fp = f"{statistics.median(p):.3f} ({min(p):.3f}–{max(p):.3f})" if p else "—"
        rel = f"{(statistics.median(p) / statistics.median(m) - 1) * 100:+.1f}%" if m and p else "—"
        print(f"| {w} | {fm} | {fp} | {rel} |")
    # the callback's own price: same server, table with vs without the index
    for w in ("bulk_delete", "bulk_update", "row_delete_insert", "row_update"):
        a, b = lat.get((f"{w}_fd_plain", "patch")), lat.get((f"{w}_fd_notify", "patch"))
        if a and b:
            print(f"{w}: notify index vs none, on the patch: "
                  f"{(statistics.median(b) / statistics.median(a) - 1) * 100:+.1f}%")


if __name__ == "__main__":
    {"levels": lambda a: levels(a), "delete": lambda a: delete(a[0])}[sys.argv[1]](sys.argv[2:])
