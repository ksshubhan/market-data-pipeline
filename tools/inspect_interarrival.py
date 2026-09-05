#!/usr/bin/env python3
"""Inter-arrival, burstiness and time-compression analysis for a capture.

TOOL_VERSION: inspect_interarrival/4 (B2)

This is the analysis §6.5 B2 specifies, and it exists to *inform* the
choice of compression factor rather than to rationalise one already made.
It answers three questions the plan poses separately:

  A. How much of the sub-millisecond clustering is real?
     Finer capture-gap buckets, down to 1 us. §6.1a puts the capture
     path's own jitter at plausibly tens of us to low ms, so any bucket
     below that is at or under the noise floor of the instrument that
     measured it and cannot be claimed as exchange-level structure.

  B. Is the clustering visible in a second, independent clock domain?
     Binance's event time E is ms-quantised and comes off the exchange's
     own clock, not this machine's. If messages that arrive close
     together in capture time also share an exchange millisecond far
     more often than distant ones do, the clustering is real at the
     exchange and the capture clock merely rendered it noisily.

  C. What does a given compression factor actually produce?
     Implied mean rate, compressed gap percentiles, and the fraction of
     gaps that collapse.

...and one the plan does not, which turns out to matter more than A:

  D. Does the compressed trace stress the queue, and does it stay inside
     the range where both arms are valid? A mean rate is not what fills
     a 16384-slot ring; a burst is. Section 4 reports peak windowed rate
     and simulates backlog against a constant drain.

THE SINGLE MOST IMPORTANT THING IN THIS FILE
--------------------------------------------

Section 3 replicates `build_replay_schedule` exactly, including its
integer truncation:

    gap' = uint64(double(gap) / C)        per gap, before accumulation

Both languages do the same IEEE-754 double division and then truncate
toward zero, and every gap here is far below 2^53, so the two agree
bit-for-bit. This is not pedantry. Analysing compression as real-valued
arithmetic would predict a distribution the harness will not produce,
and the discrepancy is largest exactly where the interesting structure
is: **every gap shorter than C nanoseconds compresses to zero, not to
some small positive value.** Those records get identical intended-send
offsets and the producer fires them back to back at its own ceiling.

§6.5 anticipates "a substantial fraction will clamp to the pacing floor".
The code does not clamp to a floor; it truncates to zero and the floor
then asserts itself through the producer's issue rate, with the shortfall
appearing as producer lag rather than as schedule spacing. Section 3
reports the zero fraction as its own line for that reason.

Usage:
    python3 tools/inspect_interarrival.py <capture.log> [mean_rate ...]

    mean_rate values are target *mean* offered rates in messages/sec; a
    compression factor is derived for each. Defaults are 10k, 100k, 500k
    and 1M. Standard library only, deliberately: the tables must print on
    a bare interpreter, like the rest of the post-processing.
"""

import bisect
import json
import math
import statistics
import sys
from pathlib import Path


# Candidate target mean rates, in messages per second, used when none are
# given on the command line. Chosen to bracket B1's measurable range: the
# SPSC arm stayed valid with zero drops to 10M/s, the tuned mutex arm
# failed §6.4's producer-lag gate at 2.5M/s and above.
DEFAULT_TARGET_RATES = (10_000.0, 100_000.0, 500_000.0, 1_000_000.0)

# The producer's own ceiling, measured by measure_pacing_floor with a
# null sink: ~50M records/s, about 20 ns per record. A compressed gap
# below this cannot be honoured however the schedule is written.
PRODUCER_FLOOR_NS = 20.0

# Ring capacity used by harness B (§8.0c finding 1).
RING_CAPACITY = 16_384

# Drain rates for the backlog simulation, in messages/sec. Both are
# grounded in B1 rather than invented: 10M/s is the highest offered rate
# at which the SPSC arm delivered 2,000,000 of 2,000,000 with zero drops,
# and 1M/s is the highest rate at which *both* arms passed the lag gate,
# so it is the ceiling for any comparison B2 wants to make.
DRAIN_RATES = (1_000_000.0, 10_000_000.0)

# Sliding windows for the peak-rate scan.
PEAK_WINDOWS_NS = (10_000, 100_000, 1_000_000)

# Capture-gap threshold below which §6.1a says the capture path's own
# jitter dominates. Used to split the exchange-time cross-check into
# "close" and "distant" pairs.
CLOSE_PAIR_NS = 1_000_000


def percentile(sorted_values, p):
    """Linear-interpolated percentile of an already-sorted sequence."""

    if not sorted_values:
        return None

    index = (len(sorted_values) - 1) * p
    lower = math.floor(index)
    upper = math.ceil(index)

    if lower == upper:
        return sorted_values[lower]

    fraction = index - lower

    return (
        sorted_values[lower] * (1 - fraction)
        + sorted_values[upper] * fraction
    )


def format_ns(value_ns):
    """Render a nanosecond figure in whichever unit reads clearly."""

    if value_ns is None:
        return "n/a"

    if value_ns < 1_000:
        return f"{value_ns:,.1f} ns"

    if value_ns < 1_000_000:
        return f"{value_ns / 1_000:,.3f} us"

    if value_ns < 1_000_000_000:
        return f"{value_ns / 1_000_000:,.3f} ms"

    return f"{value_ns / 1_000_000_000:,.3f} s"


# ---------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------

def load_capture(path: Path):
    """Read the capture log, returning parallel capture-ns and E-ms lists.

    Non-bookTicker payloads (subscription acknowledgements and anything
    else the socket delivered) are skipped, matching the converter's own
    notion of what counts as a message.
    """

    capture_ns = []
    event_ms = []

    with path.open() as file:
        for line in file:
            stripped = line.rstrip("\n")

            if not stripped:
                continue

            parts = stripped.split("\t", 1)

            if len(parts) != 2:
                continue

            capture_timestamp, raw_message = parts

            try:
                message = json.loads(raw_message)
            except json.JSONDecodeError:
                continue

            if not isinstance(message, dict):
                continue

            if not all(
                key in message
                for key in ("s", "b", "B", "a", "A")
            ):
                continue

            capture_ns.append(int(capture_timestamp))
            event_ms.append(
                int(message["E"]) if "E" in message else None
            )

    return capture_ns, event_ms


def clamped_gaps(capture_ns):
    """Gaps between consecutive capture timestamps, §6.1a rule 4.

    Written as an explicit comparison and *counted*, never silently
    skipped and never max(0, b - a). Python integers do not wrap, so the
    wrap hazard that makes this load-bearing in C++ cannot bite here —
    but the tool must agree with the schedule builder about what the
    schedule contains, and the schedule builder clamps.
    """

    gaps = []
    backwards_steps = 0
    clamped_ns = 0

    for n in range(1, len(capture_ns)):
        previous = capture_ns[n - 1]
        current = capture_ns[n]

        if current >= previous:
            gaps.append(current - previous)
        else:
            backwards_steps += 1
            clamped_ns += previous - current
            gaps.append(0)

    return gaps, backwards_steps, clamped_ns


# ---------------------------------------------------------------------
# Section 1 — raw gap distribution
# ---------------------------------------------------------------------

def report_raw_distribution(gaps_ns):
    ordered = sorted(gaps_ns)
    count = len(ordered)

    mean_ns = statistics.fmean(ordered)
    stddev_ns = statistics.pstdev(ordered)
    coefficient_of_variation = stddev_ns / mean_ns if mean_ns else 0.0

    print("1. RAW CAPTURE-GAP DISTRIBUTION")
    print()
    print(f"  gaps:    {count:,}")
    print(f"  min:     {format_ns(ordered[0])}")
    print(f"  p50:     {format_ns(percentile(ordered, 0.50))}")
    print(f"  p90:     {format_ns(percentile(ordered, 0.90))}")
    print(f"  p95:     {format_ns(percentile(ordered, 0.95))}")
    print(f"  p99:     {format_ns(percentile(ordered, 0.99))}")
    print(f"  p99.9:   {format_ns(percentile(ordered, 0.999))}")
    print(f"  max:     {format_ns(ordered[-1])}")
    print(f"  mean:    {format_ns(mean_ns)}")
    print()
    print(f"  mean/p50 ratio:           {mean_ns / percentile(ordered, 0.50):.2f}x")
    print(f"  coefficient of variation: {coefficient_of_variation:.3f}"
          "   (Poisson = 1.0)")
    print()

    # The fine buckets are item A. The point of splitting 1 ms into four
    # decades is that §6.1a's stated jitter floor sits inside it: if most
    # of the sub-millisecond population is also sub-100us, it is at or
    # below the resolution of the Python/asyncio capture path and cannot
    # be claimed as exchange-level microburst structure.
    print("  Cumulative fractions (item A — the sub-ms population split):")

    fine_limits = (
        ("<=     1 us", 1_000),
        ("<=    10 us", 10_000),
        ("<=   100 us", 100_000),
        ("<=     1 ms", 1_000_000),
        ("<=    10 ms", 10_000_000),
        ("<=   100 ms", 100_000_000),
    )

    for label, limit_ns in fine_limits:
        # bisect on the sorted list: O(log n) instead of a scan per bucket.
        at_most = bisect.bisect_right(ordered, limit_ns)
        print(f"    {label}:  {at_most / count * 100:8.4f}%"
              f"   ({at_most:,})")

    for label, limit_ns in ((">=     1 s", 1_000_000_000),
                            (">=     5 s", 5_000_000_000)):
        at_least = count - bisect.bisect_left(ordered, limit_ns)
        print(f"    {label}:  {at_least / count * 100:8.4f}%"
              f"   ({at_least:,})")

    print()

    return ordered, mean_ns


# ---------------------------------------------------------------------
# Section 2 — exchange-time cross-check (item B)
# ---------------------------------------------------------------------

# Disjoint capture-gap buckets for the cross-check. Disjoint, not
# cumulative: the question is whether the very fine population behaves
# differently from the merely-close one, and a cumulative bucket buries
# the fine population inside the coarse one that dominates it.
CROSS_CHECK_BUCKETS = (
    ("<=     1 us", 0, 1_000),
    ("1 us - 10 us", 1_000, 10_000),
    ("10 us - 100 us", 10_000, 100_000),
    ("100 us -  1 ms", 100_000, 1_000_000),
    ("1 ms -  10 ms", 1_000_000, 10_000_000),
    (">     10 ms", 10_000_000, None),
)

# Binance's E is quantised to a millisecond.
EXCHANGE_TICK_NS = 1_000_000


def report_exchange_cross_check(capture_ns, event_ms):
    """Compare capture-time clustering against Binance's own clock.

    THE CONFOUND, NAMED FIRST BECAUSE IT INVALIDATES THE OBVIOUS READING.

    Capture time tracks exchange time at every scale, so pairs close in
    capture time are close in E for the trivial reason that both measure
    the same arrival process. A single "close pairs share an E far more
    often than distant pairs" statistic is therefore guaranteed to come
    back large whether or not the sub-millisecond structure is real, and
    it is not evidence for anything.

    WHAT IS EVIDENCE, AND IT IS A PREDICTION WITH A NUMBER ON IT.

    E is quantised to a millisecond. Two messages genuinely emitted d
    apart share an exchange millisecond exactly when no boundary falls
    between them, which under uniform phase happens with probability
    1 - d/1ms for d below a millisecond, and never above it. So a bucket
    of pairs 10 us apart, *if genuinely emitted 10 us apart*, must share
    an exchange millisecond about 99% of the time. There is no room for
    interpretation in that: it follows from the quantisation alone.

    That turns each bucket into a two-sided test. Observed co-millisecond
    fraction near the predicted one means the exchange agrees those
    messages were emitted that close, and the capture clock rendered real
    structure. Observed fraction far below it means the pairs were
    actually much further apart and something between the exchange and
    the timestamp batched them - which for the finest buckets is the
    Python/asyncio scheduling jitter §6.1a warns about.

    The prediction is computed from the bucket's own mean gap rather than
    from its label, so it stays honest when a bucket is unevenly filled.
    """

    print("2. EXCHANGE-TIME CROSS-CHECK (item B - independent clock domain)")
    print()

    if any(value is None for value in event_ms):
        missing = sum(1 for value in event_ms if value is None)
        print(f"  {missing:,} messages carry no E field; cross-check skipped.")
        print()
        return

    pairs = []
    backwards_e = 0

    for n in range(1, len(event_ms)):
        delta_e = event_ms[n] - event_ms[n - 1]
        capture_gap = capture_ns[n] - capture_ns[n - 1]

        if delta_e < 0:
            backwards_e += 1

        pairs.append((max(capture_gap, 0), delta_e))

    total_pairs = len(pairs)
    same_ms_pairs = sum(1 for _, delta in pairs if delta == 0)

    print(f"  consecutive pairs:            {total_pairs:,}")
    print(f"  E decreases (non-monotonic):  {backwards_e:,}")
    print(f"  pairs sharing an exchange ms: {same_ms_pairs:,}"
          f"   ({same_ms_pairs / total_pairs * 100:.4f}%)")
    print()

    # The overall delta-E percentiles, printed beside the capture-gap
    # percentiles from section 1. This is the *coarse-scale* corroboration
    # and it is a separate claim from the bucket table below: if the two
    # distributions agree percentile by percentile, the burstiness is a
    # property of the feed rather than of the capture tool, whatever the
    # sub-millisecond buckets turn out to say.
    deltas_all = sorted(delta for _, delta in pairs)

    print("  Overall delta E distribution, against section 1's capture gaps:")
    print(f"    {'':8}{'exchange':>14}{'capture':>14}")

    for label, p in (("p50", 0.50), ("p90", 0.90),
                     ("p99", 0.99), ("p99.9", 0.999)):
        exchange_ms = percentile(deltas_all, p)
        capture_ms = percentile(
            sorted(gap for gap, _ in pairs), p
        ) / 1_000_000
        print(f"    {label:<8}{exchange_ms:>11,.0f} ms{capture_ms:>11,.0f} ms")

    exchange_max = deltas_all[-1]
    capture_max = max(gap for gap, _ in pairs) / 1_000_000
    print(f"    {'max':<8}{exchange_max:>11,.0f} ms{capture_max:>11,.0f} ms")
    print()

    header = (
        f"  {'capture gap':<16}{'pairs':>9}{'share E':>10}"
        f"{'predicted':>11}{'verdict':>12}{'dE p50':>10}{'dE p90':>10}"
    )
    print(header)
    print("  " + "-" * (len(header) - 2))

    for label, low_ns, high_ns in CROSS_CHECK_BUCKETS:
        if high_ns is None:
            members = [p for p in pairs if p[0] > low_ns]
        elif low_ns == 0:
            members = [p for p in pairs if p[0] <= high_ns]
        else:
            members = [p for p in pairs if low_ns < p[0] <= high_ns]

        if not members:
            print(f"  {label:<16}{0:>9}"
                  f"{'-':>10}{'-':>11}{'-':>12}{'-':>10}{'-':>10}")
            continue

        count = len(members)
        same = sum(1 for _, delta in members if delta == 0)
        observed = same / count

        mean_gap_ns = statistics.fmean(gap for gap, _ in members)
        predicted = max(0.0, 1.0 - mean_gap_ns / EXCHANGE_TICK_NS)

        if predicted < 0.02:
            verdict = "n/a"
        elif observed >= 0.80 * predicted:
            verdict = "REAL"
        elif observed <= 0.35 * predicted:
            verdict = "JITTER"
        else:
            verdict = "mixed"

        deltas = sorted(delta for _, delta in members)

        print(f"  {label:<16}{count:>9,}"
              f"{observed * 100:>9.2f}%{predicted * 100:>10.2f}%"
              f"{verdict:>12}"
              f"{percentile(deltas, 0.50):>9,.0f}m{percentile(deltas, 0.90):>9,.0f}m")

    print()
    print("  predicted = 1 - (bucket mean gap)/1ms, the co-millisecond rate")
    print("  a genuinely co-emitted pair must show given ms quantisation.")
    print("  REAL: observed within 20% of it. JITTER: below 35% of it -")
    print("  the pairs were further apart than the capture clock says.")
    print("  n/a: predicted rate too low for the test to discriminate.")
    print("  dE columns are in milliseconds, exchange clock.")
    print()


# ---------------------------------------------------------------------
# Section 3 — compression table (item C)
# ---------------------------------------------------------------------

def compress(gaps_ns, compression):
    """Exactly what build_replay_schedule does, in the same order.

    Per gap, double division, truncate toward zero, then accumulate. Not
    real-valued arithmetic on the series, and not accumulate-then-divide:
    both would disagree with the schedule the harness will actually
    build, and would hide the zero-collapse that is the finding here.
    """

    if compression == 1.0:
        return list(gaps_ns)

    return [int(float(gap) / compression) for gap in gaps_ns]


def report_compression(gaps_ns, mean_gap_ns, target_rates):
    print("3. TIME-COMPRESSION TABLE (item C)")
    print()
    print("  Compression divides every captured gap by C, per §6.5's")
    print("  time-compression mechanism. C is derived here from a target")
    print("  *mean* offered rate, and the achieved mean is reported back")
    print("  because truncation makes them differ.")
    print()

    exact_span_ns = sum(gaps_ns)

    rows = []

    for target_rate in target_rates:
        # mean gap after compression should be 1e9 / target_rate ns.
        compression = mean_gap_ns * target_rate / 1e9

        compressed = compress(gaps_ns, compression)
        span_ns = sum(compressed)
        ordered = sorted(compressed)
        count = len(ordered)

        achieved_rate = count / (span_ns / 1e9) if span_ns else float("inf")

        zeros = bisect.bisect_right(ordered, 0)

        truncation_loss_ns = (exact_span_ns / compression) - span_ns

        rows.append({
            "target_rate": target_rate,
            "compression": compression,
            "compressed": compressed,
            "ordered": ordered,
            "span_ns": span_ns,
            "achieved_rate": achieved_rate,
            "zeros": zeros,
            "truncation_loss_ns": truncation_loss_ns,
        })

        print(f"  --- target mean {target_rate:,.0f} msg/s"
              f"   ->   C = {compression:,.1f} ---")
        print(f"    achieved mean rate:   {achieved_rate:,.0f} msg/s")
        print(f"    compressed span:      {format_ns(span_ns)}"
              f"   (from {format_ns(exact_span_ns)})")
        print()
        print(f"    compressed p50:       {format_ns(percentile(ordered, 0.50))}")
        print(f"    compressed p90:       {format_ns(percentile(ordered, 0.90))}")
        print(f"    compressed p99:       {format_ns(percentile(ordered, 0.99))}")
        print(f"    compressed max:       {format_ns(ordered[-1])}")
        print()

        # The zero line first, because it is the one the plan does not
        # predict and the one that governs what the schedule means.
        print(f"    gaps truncated to 0:  {zeros / count * 100:8.4f}%"
              f"   ({zeros:,})   <- identical intended-send offsets")

        for label, limit_ns in (("< 20 ns (producer floor)", 20),
                                ("< 100 ns", 100),
                                ("< 500 ns", 500),
                                ("< 1 us", 1_000),
                                ("< 5 us", 5_000)):
            below = bisect.bisect_left(ordered, limit_ns)
            print(f"    {label:22s}{below / count * 100:8.4f}%"
                  f"   ({below:,})")

        print()

        if span_ns:
            share = truncation_loss_ns / span_ns * 100
            print(f"    truncation loss:      {format_ns(truncation_loss_ns)}"
                  f"   ({share:.4f}% of span)")
        else:
            print("    truncation loss:      n/a — compressed span is zero")

        print()

    return rows


# ---------------------------------------------------------------------
# Section 4 — does the compressed trace actually stress the queue?
# ---------------------------------------------------------------------

def peak_window_counts(offsets_ns, window_ns):
    """Largest number of arrivals inside any window of the given width.

    Two pointers over a non-decreasing series, so O(n).
    """

    best = 0
    left = 0

    for right in range(len(offsets_ns)):
        while offsets_ns[right] - offsets_ns[left] >= window_ns:
            left += 1

        best = max(best, right - left + 1)

    return best


def max_backlog(offsets_ns, drain_rate_hz):
    """Peak queue depth under a constant-rate single server.

    Lindley recursion:

        departure[i] = max(arrival[i], departure[i-1]) + service

    Depth immediately after arrival i is the number of arrivals not yet
    departed, which is (i + 1) minus the count of departures at or before
    arrival[i]. Departures are non-decreasing, so that count is a binary
    search rather than a scan.

    This is a deterministic-service approximation and deliberately so: it
    is a screening question — can this compressed trace fill 16,384 slots
    — not a model of either queue arm.
    """

    service_ns = 1e9 / drain_rate_hz

    departures = []
    previous_departure = -math.inf
    peak = 0

    for i, arrival in enumerate(offsets_ns):
        start = arrival if arrival > previous_departure else previous_departure
        departure = start + service_ns

        departures.append(departure)
        previous_departure = departure

        outstanding = (i + 1) - bisect.bisect_right(departures, arrival)

        if outstanding > peak:
            peak = outstanding

    return peak


def report_stress(rows):
    print("4. DOES THE COMPRESSED TRACE STRESS THE QUEUE?")
    print()
    print("  Not in §6.5's specified analysis, and it is the section that")
    print("  should decide C. B2 asks whether queue depth survives real")
    print("  microbursts; a mean rate does not fill a ring, a burst does.")
    print()
    print("  Two constraints pull against each other:")
    print(f"    - the peak must be high enough to load a {RING_CAPACITY:,}-slot ring")
    print("    - both arms must stay valid, and the tuned mutex baseline")
    print("      failed §6.4's producer-lag gate at 2.5M/s and above, so a")
    print("      peak far past ~1M/s buys a sweep with no comparison in it")
    print()

    for row in rows:
        offsets = []
        running = 0

        for gap in row["compressed"]:
            running += gap
            offsets.append(running)

        print(f"  --- target mean {row['target_rate']:,.0f} msg/s"
              f"   (C = {row['compression']:,.1f}) ---")

        for window_ns in PEAK_WINDOWS_NS:
            peak = peak_window_counts(offsets, window_ns)
            implied = peak / (window_ns / 1e9)
            print(f"    peak in {format_ns(window_ns):>10s} window: "
                  f"{peak:9,} msgs  ->  {implied:15,.0f} msg/s instantaneous")

        print()

        for drain_rate in DRAIN_RATES:
            peak_depth = max_backlog(offsets, drain_rate)
            verdict = (
                "EXCEEDS capacity — would drop"
                if peak_depth > RING_CAPACITY
                else f"{peak_depth / RING_CAPACITY * 100:.2f}% of capacity"
            )
            label = f"{drain_rate:,.0f} msg/s drain"
            print(f"    max backlog at {label:>21s}: "
                  f"{peak_depth:9,}   {verdict}")

        print()


# ---------------------------------------------------------------------

def main(argv):
    if len(argv) < 2:
        print("Usage: python3 tools/inspect_interarrival.py "
              "<capture_file> [target_mean_rate ...]")
        return 1

    path = Path(argv[1])

    target_rates = (
        tuple(float(value) for value in argv[2:])
        if len(argv) > 2
        else DEFAULT_TARGET_RATES
    )

    capture_ns, event_ms = load_capture(path)

    if len(capture_ns) < 2:
        print("Fewer than two bookTicker messages; nothing to analyse.")
        return 1

    if len(capture_ns) > 2_000_000:
        # Sections 3 and 4 hold several full-length lists and section 4's
        # Lindley recursion keeps every departure time. That is nothing on
        # ETHW's 55,775 messages and roughly a gigabyte on BTC's 13.7M.
        # B2 is an ETHW experiment; this is here so a mistyped path fails
        # with a sentence rather than with the OOM killer.
        print(f"warning: {len(capture_ns):,} messages. Sections 3 and 4 are "
              "O(n) in memory and\n         were sized for the ETHW capture. "
              "Expect minutes and gigabytes.\n")

    gaps_ns, backwards_steps, clamped_ns = clamped_gaps(capture_ns)

    print()
    print("=" * 70)
    print(f"File:     {path}")
    print(f"Messages: {len(capture_ns):,}")
    print("=" * 70)
    print()

    print("0. CLOCK HYGIENE (§6.1a)")
    print()
    print(f"  backwards capture-clock steps: {backwards_steps:,}")
    print(f"  total clamped:                 {format_ns(clamped_ns)}")

    if backwards_steps == 0:
        # §6.1a asks for exactly this statement when the count is zero:
        # it is a cleaner thing to say than "we clamp".
        print()
        print("  Zero backwards steps, so the cumulative clamped form and")
        print("  the endpoint subtraction are provably equivalent for this")
        print("  file. The clamp is still written as an explicit comparison")
        print("  because the next capture may not have this property.")
    else:
        print()
        print("  Non-zero. The cumulative form is NOT equivalent to an")
        print("  endpoint subtraction on this file, and any analysis or")
        print("  schedule built from endpoints is wrong by the clamped total.")

    print()

    ordered_raw, mean_gap_ns = report_raw_distribution(gaps_ns)

    report_exchange_cross_check(capture_ns, event_ms)

    rows = report_compression(gaps_ns, mean_gap_ns, target_rates)

    report_stress(rows)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))