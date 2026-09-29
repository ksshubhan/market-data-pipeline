#!/usr/bin/env python3
"""Post-process harness B sweeps into the B1 comparison.

Reads one or more harness_b CSVs, medians each datapoint across passes,
and prints the latency-vs-load comparison. If matplotlib is installed it
also writes the two graphs the README shows, into results/.

Deliberately offline. harness_b's consumer records raw dequeue
timestamps; latencies and percentiles are computed only after both
threads join, so no histogram is built on the measured path. This
script medians those percentiles across passes and draws the graphs.

Usage:
    python3 tools/analyse_harness_b.py results/harness_b_spin8192_*.csv \\
                                       results/harness_b_spin1000_*.csv

Standard library only, except matplotlib for the graphs. If matplotlib
is absent the tables still print and the graphs are skipped with a note —
the numbers are the result, the plots are presentation.
"""

# Input: src/harness_b.cpp writes one CSV row per pass, arm and rate,
# with the six latency percentiles and each validity gate's verdict.
# Output: stdout tables, and the two PNGs that open the README's Results.
# The graph paths are relative, so run it from the repository root, as
# the README's reproduce block does.

import csv
import statistics
import sys
from collections import defaultdict


PERCENTILES = ["p50_ns", "p90_ns", "p99_ns", "p999_ns", "p9999_ns", "max_ns"]


def read_sweep(path):
    """Return (metadata, rows). Comment lines carry the provenance."""
    meta = {}
    data_lines = []

    with open(path, newline="") as handle:
        for line in handle:
            if line.startswith("#"):
                body = line[1:].strip()
                if ":" in body:
                    key, _, value = body.partition(":")
                    key = key.strip()
                    # A key never contains a space, so a prose note
                    # with a colon in it is not taken for metadata.
                    if key and " " not in key:
                        meta[key] = value.strip()
            else:
                data_lines.append(line)

    rows = list(csv.DictReader(data_lines))
    return meta, rows


def config_label(meta, rows):
    """A short name for what this file's mutex arm was."""
    # Read from the rows: the header's spin_count line carries a note
    # after the number.
    spins = {r["spin_count"] for r in rows if r["arm"] == "mutex"}
    spins.discard("0")

    if spins == {"8192"}:
        return "mutex-tuned"
    if spins == {"1000"}:
        return "mutex-parking"
    return "mutex-spin" + "/".join(sorted(spins))


def collect(paths):
    """(config, rate) -> {metric: [values across passes]}, valid rows only."""
    series = defaultdict(lambda: defaultdict(list))
    counts = defaultdict(lambda: [0, 0])   # [valid, total]
    metas = {}
    spsc_by_file = defaultdict(lambda: defaultdict(list))

    for path in paths:
        meta, rows = read_sweep(path)
        metas[path] = meta
        label = config_label(meta, rows)

        for row in rows:
            rate = float(row["rate_hz"])
            # Every file's spsc rows share one key: the arm is configured
            # identically in each, so two files give up to six passes a cell.
            config = label if row["arm"] == "mutex" else "spsc"

            counts[(config, rate)][1] += 1

            if row["valid"] != "yes":
                continue

            counts[(config, rate)][0] += 1

            for metric in PERCENTILES + ["p99_lag_ns", "parks", "signals"]:
                if row.get(metric):
                    series[(config, rate)][metric].append(float(row[metric]))

            # The SPSC arm is identically configured in every file, so its
            # per-file medians are a free reproducibility check.
            if config == "spsc":
                for metric in PERCENTILES:
                    spsc_by_file[path][rate].append(float(row[metric]))

    return series, counts, metas, spsc_by_file


def median_or_none(values):
    return statistics.median(values) if values else None


def fmt_ns(value):
    if value is None:
        return "     -"
    if value >= 1_000_000:
        return f"{value / 1_000_000:7.2f}ms"
    if value >= 1_000:
        return f"{value / 1_000:7.2f}us"
    return f"{value:7.0f}ns"


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    paths = argv[1:]
    series, counts, metas, spsc_by_file = collect(paths)

    # From counts, which include invalid rows, so a rate where every
    # datapoint failed a gate still gets a row in every table.
    configs = sorted({c for (c, _) in counts})
    rates = sorted({r for (_, r) in counts})

    print("=" * 78)
    print("PROVENANCE")
    print("=" * 78)
    for path in paths:
        meta = metas[path]
        print(f"{path}")
        print(f"    commit {meta.get('git_commit', '?')[:12]}"
              f"  dirty={meta.get('git_dirty', '?')}"
              f"  passes={meta.get('passes', '?')}"
              f"  capacity={meta.get('capacity', '?')}"
              f"  spin={meta.get('spin_count', '?')}")
        print(f"    lap_ratio={meta.get('lap_ratio', '?')}")

    print()
    print("=" * 78)
    print("VALID DATAPOINTS  (valid/total across passes; drop and lag gates)")
    print("=" * 78)
    header = f"{'rate':>12}  " + "  ".join(f"{c:>16}" for c in configs)
    print(header)
    for rate in rates:
        cells = []
        for config in configs:
            valid, total = counts[(config, rate)]
            cells.append(f"{valid}/{total}".rjust(16))
        print(f"{rate:>12,.0f}  " + "  ".join(cells))

    print()
    print("=" * 78)
    print("REPRODUCIBILITY: spsc p99.9 median, per file")
    print("=" * 78)
    print("The spsc arm is identically configured in every file, so a")
    print("disagreement here is run-to-run variance, not a result.")
    print()
    for path in paths:
        per_rate = spsc_by_file[path]
        if not per_rate:
            continue
        print(f"  {path.split('/')[-1]}")
        for rate in sorted(per_rate):
            # p999_ns is index 3 of PERCENTILES, stored flat per row
            values = per_rate[rate][3::len(PERCENTILES)]
            print(f"      {rate:>12,.0f}  {fmt_ns(median_or_none(values))}")

    for metric, title in [
        ("p50_ns", "MEDIAN LATENCY (p50)"),
        ("p99_ns", "TAIL LATENCY (p99)"),
        ("p999_ns", "TAIL LATENCY (p99.9)"),
        ("max_ns", "MAX LATENCY"),
    ]:
        print()
        print("=" * 78)
        print(title)
        print("=" * 78)
        print(f"{'rate':>12}  " + "  ".join(f"{c:>16}" for c in configs)
              + "   ratio")
        for rate in rates:
            cells = []
            values = {}
            for config in configs:
                value = median_or_none(series[(config, rate)].get(metric))
                values[config] = value
                cells.append(fmt_ns(value).rjust(16))

            ratio = ""
            spsc = values.get("spsc")
            tuned = values.get("mutex-tuned")
            parking = values.get("mutex-parking")

            if spsc:
                parts = []
                if tuned:
                    parts.append(f"tuned {tuned / spsc:.2f}x")
                if parking:
                    parts.append(f"park {parking / spsc:.2f}x")
                ratio = "  " + ", ".join(parts)

            print(f"{rate:>12,.0f}  " + "  ".join(cells) + ratio)

    print()
    print("=" * 78)
    print("BASELINE WAIT BEHAVIOUR (median parks per 2,000,000 messages)")
    print("=" * 78)
    for rate in rates:
        cells = []
        for config in configs:
            if config == "spsc":
                continue
            value = median_or_none(series[(config, rate)].get("parks"))
            cells.append(f"{config}={value if value is None else int(value):,}"
                         if value is not None else f"{config}=-")
        if cells:
            print(f"{rate:>12,.0f}  " + "   ".join(cells))

    try:
        import matplotlib
        # Agg renders straight to files, so no display is needed.
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print()
        print("matplotlib not installed; skipping graphs.")
        print("  pip3 install matplotlib   (then re-run)")
        return 0

    # Graph 1: latency vs offered load, p99.9.
    fig, axis = plt.subplots(figsize=(9, 5.5))
    for config in configs:
        xs, ys = [], []
        for rate in rates:
            value = median_or_none(series[(config, rate)].get("p999_ns"))
            if value is not None:
                xs.append(rate)
                ys.append(value / 1000.0)
        if xs:
            axis.plot(xs, ys, marker="o", label=config)

    axis.set_xscale("log")
    axis.set_yscale("log")
    axis.set_xlabel("offered rate (records/s)")
    axis.set_ylabel("p99.9 end-to-end latency (us)")
    axis.set_title("B1: p99.9 latency vs offered load "
                   "(valid datapoints only, median of passes)")
    axis.grid(True, which="both", alpha=0.3)
    axis.legend()
    fig.tight_layout()
    fig.savefig("results/b1_latency_vs_load.png", dpi=150)
    print("\nwrote results/b1_latency_vs_load.png")

    # Graph 2: percentile distribution at the rate with the most
    # configurations valid, chosen by the data rather than by hand. A tie
    # goes to the lowest such rate: rates ascend, and only a strictly
    # larger count replaces the pick.
    best_rate, best_count = None, -1
    for rate in rates:
        n = sum(
            1 for c in configs
            if median_or_none(series[(c, rate)].get("p999_ns")) is not None
        )
        if n > best_count:
            best_rate, best_count = rate, n

    fig2, axis2 = plt.subplots(figsize=(9, 5.5))
    labels = ["p50", "p90", "p99", "p99.9", "p99.99", "max"]
    for config in configs:
        ys = [
            median_or_none(series[(config, best_rate)].get(m))
            for m in PERCENTILES
        ]
        if all(y is not None for y in ys):
            axis2.plot(labels, [y / 1000.0 for y in ys],
                       marker="o", label=config)

    axis2.set_yscale("log")
    axis2.set_ylabel("latency (us)")
    axis2.set_title(f"B1: latency distribution at {best_rate:,.0f} records/s")
    axis2.grid(True, which="both", alpha=0.3)
    axis2.legend()
    fig2.tight_layout()
    fig2.savefig("results/b1_percentile_distribution.png", dpi=150)
    print("wrote results/b1_percentile_distribution.png")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))