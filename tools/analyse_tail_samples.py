#!/usr/bin/env python3
"""Characterise the tail of a harness B run from its slow-sample dump.

Reads the dump written by `harness_b ... dump`, one row for every
message with latency >= 1000 ns, for spsc and the tuned mutex at 100k
and 1M records/s, and asks what shape the tail has rather than
assuming one.

Slow messages come in bursts: one event stalls the consumer for several
microseconds and every message due during it is delivered late
together. So the questions are how often stalls happen, how long they
last, and how many messages each disturbs.

Two per-cluster durations are reported, and they measure different
things:

  drain span    c[-1][1] - c[0][1], the span of *dequeue* timestamps
                across a cluster: how long the consumer took to work
                through the backlog once it got the CPU back. Near zero
                when the backlog drains at full speed, which is the
                normal case, so it says nothing about how long the CPU
                was away.

  stall length  the worst latency inside the cluster. With the queue
                empty as the stall begins, the worst-delayed message is
                the first one due after it began, up to one period in;
                it waits out the rest of the stall and is served first.
                So its latency is a *lower bound* on how long the CPU
                was away, to within one dequeue, and can fall short of
                it by up to one period. Hence the (>=). This is the
                column to quote.

Usage:
    python3 tools/analyse_tail_samples.py results/tail_samples_*.csv
"""

# Input: one CSV from harness_b's dump mode (src/harness_b.cpp); give
# exactly one path, or the usage text prints. Output: stdout only; the
# committed copy is results/tail_stalls_*.txt.

import csv
import statistics
import sys
from collections import defaultdict


# Two slow samples belong to the same stall if they are this close in
# index. Deliberately small: a stall delivers a contiguous run of
# messages late, so genuine members of one burst are adjacent or nearly
# so. Raising this merges distinct stalls; lowering it splits one stall
# that happened to contain a fast message.
CLUSTER_GAP = 4

# At or above this fraction the cost is not a stall at all — it is being
# paid on a large share of messages, which is a per-message cost wearing
# a tail's clothing.
PERVASIVE_FRACTION = 0.10

# The dump records no slice length, so this must match harness_b's
# kSliceLength.
SLICE_LENGTH = 2_000_000


def load(path):
    series = defaultdict(list)

    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            key = (row["arm"], float(row["rate_hz"]))
            series[key].append((
                int(row["index"]),
                int(row["dequeue_offset_ns"]),
                int(row["latency_ns"]),
            ))

    for key in series:
        series[key].sort()

    return series


def cluster(rows):
    """Group slow samples into stalls. Returns a list of row-lists."""
    if not rows:
        return []

    clusters = [[rows[0]]]

    for row in rows[1:]:
        if row[0] - clusters[-1][-1][0] <= CLUSTER_GAP:
            clusters[-1].append(row)
        else:
            clusters.append([row])

    return clusters


def summarise(values):
    """(median, p99, max) — zeros when empty."""
    if not values:
        return (0, 0, 0)

    ordered = sorted(values)
    median = statistics.median(ordered)
    p99 = ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))]

    return (median, p99, ordered[-1])


def fmt(value):
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f}ms"
    if value >= 1_000:
        return f"{value / 1_000:.2f}us"
    return f"{value:.0f}ns"


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2

    series = load(argv[1])

    if not series:
        print("no samples above the dump threshold — no tail to explain")
        return 0

    disturbed = {}

    print("=" * 78)
    print("TAIL STRUCTURE  (samples with latency >= 1000 ns)")
    print("=" * 78)
    print()
    print("drain span   = span of dequeue timestamps across a cluster: how")
    print("               long the backlog took to drain once the consumer")
    print("               was running again. NOT the length of the stall.")
    print("stall length = worst latency inside the cluster, a lower bound on")
    print("               how long the CPU was away. Quote this one.")

    for key in sorted(series):
        arm, rate = key
        rows = series[key]
        clusters = cluster(rows)

        fraction = len(rows) / SLICE_LENGTH
        disturbed[key] = len(rows)

        run_ns = max(r[1] for r in rows)
        run_s = run_ns / 1e9 if run_ns else 0.0

        sizes = [len(c) for c in clusters]

        # Dequeue timestamps, so this is backlog drain time, not stall
        # length. The consumer was already running again when the first
        # of these was recorded.
        drain_spans = [c[-1][1] - c[0][1] for c in clusters]

        # Lower bound on how long the CPU was away, to within one
        # dequeue: the worst-delayed message was due up to one period
        # after the stall began, so it waited out less than all of it,
        # and was served first.
        stall_lengths = [max(r[2] for r in c) for c in clusters]

        starts = [c[0][1] for c in clusters]
        between = [b - a for a, b in zip(starts, starts[1:])]

        size_med, _, size_max = summarise(sizes)
        drain_med, drain_p99, drain_max = summarise(drain_spans)
        stall_med, _, stall_max = summarise(stall_lengths)
        between_med, _, _ = summarise(between)

        per_second = (len(clusters) / run_s) if run_s else 0.0

        latency_med, latency_p99, latency_max = summarise(
            [r[2] for r in rows]
        )

        print()
        print(f"{arm}  @  {rate:,.0f} records/s   (run {run_s:.2f} s)")
        print(f"    messages disturbed   {len(rows):>10,}"
              f"   ({100 * fraction:.4f}% of {SLICE_LENGTH:,})")
        print(f"    their latency        median {fmt(latency_med):>9}"
              f"   p99 {fmt(latency_p99):>9}   max {fmt(latency_max):>9}")

        # Clustering only means something when the slow samples are a
        # minority. At high density, slow samples sit within CLUSTER_GAP
        # of each other because there are so many of them, not because a
        # stall delivered them together, so the stall statistics would
        # describe the density. Printing them would invite someone to
        # quote them.
        if fraction >= PERVASIVE_FRACTION:
            print("    (stall statistics omitted: at this density"
                  " adjacency is")
            print("     guaranteed and clustering carries no information)")
            print(f"    => PERVASIVE. {100 * fraction:.1f}% of messages are")
            print("       affected, so this is a cost paid per message, not")
            print("       an occasional stall. Look at the wait policy.")
        else:
            print(f"    stalls               {len(clusters):>10,}"
                  f"   ({per_second:,.0f}/s)")
            print(f"    messages per stall   median {size_med:>7,.0f}"
                  f"   max {size_max:,}")
            print(f"    drain span           median {fmt(drain_med):>9}"
                  f"   p99 {fmt(drain_p99):>9}   max {fmt(drain_max):>9}")
            print(f"    stall length (>=)    median {fmt(stall_med):>9}"
                  f"                   max {fmt(stall_max):>9}")
            print(f"    gap between stalls   median {fmt(between_med):>9}")

        if fraction >= PERVASIVE_FRACTION:
            pass
        elif size_med >= 3:
            print("    => BURSTY. Slow messages arrive in contiguous runs,")
            print("       which is a consumer stall delivering a backlog")
            print("       late. Consistent with a scheduling event.")
        else:
            print("    => ISOLATED. Slow messages are mostly singletons.")

    print()
    print("=" * 78)
    print("MESSAGES DISTURBED — arm comparison at equal offered rate")
    print("=" * 78)
    print("What p99.9 cannot express: how many messages each queue let the")
    print("machine disturb. Insensitive to the scheduler floor that")
    print("dominates both arms' upper percentiles.")
    print()

    for rate in sorted({r for (_, r) in series}):
        spsc = disturbed.get(("spsc", rate))
        mutex = disturbed.get(("mutex", rate))

        if spsc is None or mutex is None:
            continue

        line = (f"  {rate:>12,.0f} records/s   "
                f"spsc {spsc:>9,}   mutex {mutex:>9,}")

        if spsc:
            line += f"   {mutex / spsc:>7.1f}x"

        print(line)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))