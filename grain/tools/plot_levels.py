#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib"]
# ///
"""P2 chart and table from bench_levels.py's results.csv (NOTES.md P2).

Usage: plot_levels.py RESULTS_CSV OUT_PNG
"""

import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

TODAY = ["b0_seq", "b0_tuned", "b0_covering", "b1_brin"]
LABELS = {
    "b0_seq": "no index",
    "b0_tuned": "no index, tuned",
    "b0_covering": "covering btree",
    "b1_brin": "BRIN",
}
QUERIES = {
    "hourly30d": "hourly, 30 days",
    "daily30d": "daily, 30 days",
    "total365d": "total, 365 days",
}


def size_label(n):
    for div, suffix in ((10**9, "B"), (10**6, "M"), (10**3, "k")):
        if n >= div:
            return f"{n // div}{suffix}"
    return str(n)


def main(path, out):
    lat = {}
    for row in csv.DictReader(open(path)):
        if row["latency_ms"]:
            lat[(int(row["rows"]), row["query"], row["variant"])] = float(row["latency_ms"])
    sizes = sorted({n for n, _, _ in lat})

    print("| Rows | Query | Best today | ms | grain ms | Speedup | grain, 5 dirty ms | Speedup |")
    print("|---|---|---|---|---|---|---|---|")
    best = {}
    for n in sizes:
        for q in QUERIES:
            v = min(TODAY, key=lambda v: lat[(n, q, v)])
            best[(n, q)] = lat[(n, q, v)]
            g, d = lat[(n, q, "grain_clean")], lat[(n, q, "grain_dirty5")]
            print(f"| {size_label(n)} | {QUERIES[q]} | {LABELS[v]} | {best[(n, q)]:.3f} | "
                  f"{g:.3f} | {best[(n, q)] / g:.0f}x | {d:.3f} | {best[(n, q)] / d:.0f}x |")

    fig, axes = plt.subplots(1, len(sizes), figsize=(7 * len(sizes), 5.5), squeeze=False)
    for ax, n in zip(axes[0], sizes):
        x = range(len(QUERIES))
        w = 0.27
        bt = [best[(n, q)] for q in QUERIES]
        gc = [lat[(n, q, "grain_clean")] for q in QUERIES]
        gd = [lat[(n, q, "grain_dirty5")] for q in QUERIES]
        ax.bar([i - w for i in x], bt, w, label="today: best plain GROUP BY")
        ax.bar(list(x), gc, w, label="grain read path")
        ax.bar([i + w for i in x], gd, w, label="grain, 5 dirty minutes")
        for i in x:
            ax.annotate(f"{bt[i] / gc[i]:.0f}x", (i, gc[i]), ha="center", va="bottom",
                        fontsize=9, fontweight="bold")
            ax.annotate(f"{bt[i] / gd[i]:.0f}x", (i + w, gd[i]), ha="center", va="bottom",
                        fontsize=8)
        ax.set_yscale("log")
        ax.set_xticks(list(x), list(QUERIES.values()))
        ax.set_ylabel("latency per query, ms (log)")
        ax.set_title(f"{size_label(n)} rows: today vs grain (label: speedup)")
        ax.legend(fontsize=8)

    fig.text(0.01, 0.01,
             "Assumptions: warm cache; one client; master -O2 without asserts; shared_buffers 2GB; "
             "rows evenly spread over 400 days in time order; levels 1 minute / 1 hour / 1 day; lag 1 minute. "
             "grain = plain SQL simulating its read path with key lookups, not grain itself; no write costs here.",
             fontsize=7, wrap=True)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    fig.savefig(out, dpi=130)
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
