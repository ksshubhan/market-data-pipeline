import json
import math
import statistics
import sys
from pathlib import Path


def percentile(sorted_values, p):
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


def inspect_interarrival(path: Path) -> None:
    previous_capture_ns = None
    gaps_ns = []

    with path.open() as file:
        for line_number, line in enumerate(file, start=1):
            capture_timestamp, raw_message = (
                line.rstrip("\n").split("\t", 1)
            )

            message = json.loads(raw_message)

            # Ignore things such as subscription acknowledgements.
            if not isinstance(message, dict):
                continue

            if not all(
                key in message
                for key in ("s", "b", "B", "a", "A")
            ):
                continue

            capture_ns = int(capture_timestamp)

            if previous_capture_ns is not None:
                gap = capture_ns - previous_capture_ns

                if gap < 0:
                    raise ValueError(
                        f"Capture timestamp decreased "
                        f"at line {line_number}"
                    )

                gaps_ns.append(gap)

            previous_capture_ns = capture_ns

    if not gaps_ns:
        print("No inter-arrival gaps found.")
        return

    gaps_ms = [gap / 1_000_000 for gap in gaps_ns]
    gaps_ms.sort()

    mean_ms = statistics.mean(gaps_ms)
    median_ms = statistics.median(gaps_ms)
    stddev_ms = statistics.pstdev(gaps_ms)

    coefficient_of_variation = (
        stddev_ms / mean_ms
        if mean_ms != 0
        else 0
    )

    def fraction_at_most(limit_ms):
        count = sum(gap <= limit_ms for gap in gaps_ms)
        return count / len(gaps_ms)

    def fraction_at_least(limit_ms):
        count = sum(gap >= limit_ms for gap in gaps_ms)
        return count / len(gaps_ms)

    print()
    print(f"File: {path}")
    print(f"Inter-arrival gaps: {len(gaps_ms)}")
    print()

    print("Gap distribution (milliseconds):")
    print(f"  min:     {gaps_ms[0]:.6f}")
    print(f"  p50:     {median_ms:.6f}")
    print(f"  p90:     {percentile(gaps_ms, 0.90):.6f}")
    print(f"  p95:     {percentile(gaps_ms, 0.95):.6f}")
    print(f"  p99:     {percentile(gaps_ms, 0.99):.6f}")
    print(f"  p99.9:   {percentile(gaps_ms, 0.999):.6f}")
    print(f"  max:     {gaps_ms[-1]:.6f}")
    print(f"  mean:    {mean_ms:.6f}")
    print()

    print("Burstiness indicators:")
    print(
        "  coefficient of variation: "
        f"{coefficient_of_variation:.3f}"
    )
    print(
        f"  gaps <= 1 ms:    "
        f"{fraction_at_most(1) * 100:.2f}%"
    )
    print(
        f"  gaps <= 10 ms:   "
        f"{fraction_at_most(10) * 100:.2f}%"
    )
    print(
        f"  gaps <= 100 ms:  "
        f"{fraction_at_most(100) * 100:.2f}%"
    )
    print(
        f"  gaps >= 1 s:     "
        f"{fraction_at_least(1000) * 100:.2f}%"
    )
    print(
        f"  gaps >= 5 s:     "
        f"{fraction_at_least(5000) * 100:.2f}%"
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(
            "Usage: python tools/inspect_interarrival.py "
            "<capture_file>"
        )
        sys.exit(1)

    inspect_interarrival(Path(sys.argv[1]))