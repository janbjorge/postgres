#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib"]
# ///
"""P1 chart and table from bench_read.py's results.csv (NOTES.md P1).

Usage: plot_read.py RESULTS_CSV OUT_PNG
"""

import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

TODAY = ["b0_seq", "b0_tuned", "b0_btree", "b0_covering", "b1_brin"]
LABELS = {
    "b0_seq": "GROUP BY, no index",
    "b0_tuned": "GROUP BY, no index, work_mem 256MB, 8 workers",
    "b0_btree": "GROUP BY, btree",
    "b0_covering": "GROUP BY, covering btree",
    "b1_brin": "GROUP BY, BRIN",
    "grain_clean": "grain read path",
    "grain_dirty5": "grain read path, 5 dirty hours",
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
            lat[(int(row["rows"]), row["variant"])] = float(row["latency_ms"])
    sizes = sorted({n for n, _ in lat})

    print("| Rows | Best today | ms | grain ms | Speedup | grain, 5 dirty ms | Speedup |")
    print("|---|---|---|---|---|---|---|")
    best = {}
    for n in sizes:
        v = min(TODAY, key=lambda v: lat[(n, v)])
        best[n] = lat[(n, v)]
        g, d = lat[(n, "grain_clean")], lat[(n, "grain_dirty5")]
        print(f"| {size_label(n)} | {LABELS[v]} | {best[n]:.3f} | {g:.3f} | "
              f"{best[n] / g:.1f}x | {d:.3f} | {best[n] / d:.1f}x |")

    fig, (a, b) = plt.subplots(1, 2, figsize=(14, 5.5))
    for v, style in [(v, "--") for v in TODAY] + [("grain_clean", "-"), ("grain_dirty5", "-")]:
        a.plot(sizes, [lat[(n, v)] for n in sizes], style, marker="o", label=LABELS[v],
               linewidth=2.5 if v.startswith("grain") else 1.2)
    a.set_xscale("log")
    a.set_yscale("log")
    a.set_xticks(sizes, [size_label(n) for n in sizes])
    a.set_xlabel("rows in table (90 days of data)")
    a.set_ylabel("latency per query, ms (log)")
    a.set_title("30-day hourly count/sum/min/max: latency")
    a.grid(True, which="both", alpha=0.3)
    a.legend(fontsize=8)

    x = range(len(sizes))
    w = 0.38
    gs = [lat[(n, "grain_clean")] for n in sizes]
    bt = [best[n] for n in sizes]
    b.bar([i - w / 2 for i in x], bt, w, label="today: best plain GROUP BY")
    b.bar([i + w / 2 for i in x], gs, w, label="grain read path")
    for i, n in enumerate(sizes):
        b.annotate(f"{best[n] / gs[i]:.1f}x", (i + w / 2, gs[i]), ha="center",
                   va="bottom", fontsize=9, fontweight="bold")
    b.set_yscale("log")
    b.set_xticks(list(x), [size_label(n) for n in sizes])
    b.set_xlabel("rows in table")
    b.set_ylabel("latency per query, ms (log)")
    b.set_title("Today vs grain (label: speedup)")
    b.legend(fontsize=8)

    fig.text(0.01, 0.01,
             "Assumptions: warm cache; one client; master -O2 without asserts; shared_buffers 2GB; "
             "rows evenly spread over 90 days in time order; window 30 days; lag 2 h, so about 2.5 open hours scanned. "
             "grain = plain SQL simulating its read path, not grain itself; no write costs here.",
             fontsize=7, wrap=True)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    fig.savefig(out, dpi=130)
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
