#!/usr/bin/env python3
"""The routine capture inspector — §7.0.

One pass over a capture .log, reporting everything routinely known about
it: fractional-digit maxima, quantity maxima, raw-line against
payload-message counts, unusual leading zeros, E monotonicity, and
capture-clock monotonicity with backwards-step magnitudes.

§7.0 wants exactly one command that does this, rather than a per-check
script accreting for each new question. The rule is about lifecycle, not
line count: this is the *gate*, run on every capture before anything
downstream is trusted, and it stays the single routine inspector.
tools/inspect_interarrival.py is a different kind of thing — a closed
one-off study whose output is a committed artifact of B2 — and keeping it
separate is not accretion.

WHAT THIS FILE WAS MISSING UNTIL 5 SEP, AND WHY IT MATTERED.

It was named inspect_capture_precision.py and did five of the six checks
above. Capture-clock monotonicity was absent: the capture timestamp was
parsed only to confirm it was an integer, then discarded. §7.0 and §6.1a
both described this file as reporting it, and §6.1a built an instruction
on top of that description — read the backwards-step count before
building the pacer, and if it is zero, assert that at load time. That
instruction was not executable from the tool it named.

The count did exist, but in inspect_interarrival.py, which was only ever
run against ETHW. So the BTC capture — the file every B1 datapoint
replays — had never had its clock checked at all.

Nothing reported was affected: harness B paces from
build_fixed_rate_schedule, which ignores captured gaps entirely, and B2
was closed as an analysis rather than run. But this is the third defect
of the same kind after §6.5a's gap reconciliation and §7.3a's
capture_index formula — prose describing code, read many times and
executed zero times.

Usage:
    python3 tools/inspect_capture.py <capture.log> [capture.log ...]
"""

import json
import sys
from decimal import Decimal
from pathlib import Path


REQUIRED_BOOKTICKER_FIELDS = {
    "s",  # symbol
    "E",  # event time
    "T",  # transaction time
    "b",  # bid price
    "B",  # bid quantity
    "a",  # ask price
    "A",  # ask quantity
}


def fractional_digits(value: str) -> int:
    if "." not in value:
        return 0

    return len(value.split(".", 1)[1])


def has_unusual_leading_zero(value: str) -> bool:
    """
    Examples:
        "0.123"   -> False
        "0"       -> False
        "123.45"  -> False
        "0123.45" -> True
        "0007"    -> True
    """
    integer_part = value.split(".", 1)[0]

    return len(integer_part) > 1 and integer_part.startswith("0")


def looks_like_bookticker(message: dict) -> bool:
    # Explicit event type is the strongest signal.
    if message.get("e") == "bookTicker":
        return True

    # Also recognise a message that clearly resembles bookTicker,
    # even if the event-type field changes or is absent.
    characteristic_fields = {"s", "b", "B", "a", "A"}

    return len(characteristic_fields.intersection(message.keys())) >= 3


def inspect_capture(path: Path) -> None:
    max_fractional = {
        "bid_price": 0,
        "ask_price": 0,
        "bid_qty": 0,
        "ask_qty": 0,
    }

    max_quantity = {
        "bid_qty": Decimal("0"),
        "ask_qty": Decimal("0"),
    }

    raw_line_count = 0
    bookticker_count = 0
    non_payload_count = 0

    symbol = None

    previous_event_time = None
    event_time_decreases = 0
    event_time_decrease_examples = []

    leading_zero_count = 0
    leading_zero_examples = []

    # Capture-clock hygiene, §6.1a. The capture timestamp comes from
    # Python's time.time_ns() — CLOCK_REALTIME, NTP-disciplined, and
    # therefore able to step backwards mid-capture.
    #
    # Tracked over the *payload* sequence rather than over raw lines,
    # because that is the sequence the converter writes and therefore the
    # one build_replay_schedule consumes. A non-payload line between two
    # payload messages is not a gap in the schedule.
    previous_capture_ns = None
    backwards_steps = 0
    clamped_ns = 0
    largest_backwards_step_ns = 0
    backwards_step_examples = []

    non_payload_examples = []

    with path.open() as file:
        for line_number, line in enumerate(file, start=1):
            raw_line_count += 1

            try:
                capture_timestamp, raw_message = (
                    line.rstrip("\n").split("\t", 1)
                )

                # Kept, not discarded: this is the clock §6.1a is
                # about, and it used to be parsed only to prove it was
                # an integer.
                capture_ns = int(capture_timestamp)

                message = json.loads(raw_message)

                if not isinstance(message, dict):
                    raise ValueError("JSON payload is not an object")

                # Subscription acknowledgements / other non-payload
                # messages are counted separately.
                if not looks_like_bookticker(message):
                    non_payload_count += 1

                    if len(non_payload_examples) < 5:
                        non_payload_examples.append(
                            (line_number, raw_message[:200])
                        )

                    continue

                missing_fields = (
                    REQUIRED_BOOKTICKER_FIELDS - message.keys()
                )

                if missing_fields:
                    raise ValueError(
                        "bookTicker-like message is missing required "
                        f"fields: {sorted(missing_fields)}"
                    )

                current_symbol = message["s"]

                if symbol is None:
                    symbol = current_symbol
                elif current_symbol != symbol:
                    raise ValueError(
                        f"symbol changed from {symbol} "
                        f"to {current_symbol}"
                    )

                bid_price = message["b"]
                ask_price = message["a"]
                bid_qty = message["B"]
                ask_qty = message["A"]

                fields = {
                    "bid_price": bid_price,
                    "ask_price": ask_price,
                    "bid_qty": bid_qty,
                    "ask_qty": ask_qty,
                }

                # Precision and leading-zero checks.
                for field_name, value in fields.items():
                    if not isinstance(value, str):
                        raise ValueError(
                            f"{field_name} is not a string"
                        )

                    max_fractional[field_name] = max(
                        max_fractional[field_name],
                        fractional_digits(value),
                    )

                    if has_unusual_leading_zero(value):
                        leading_zero_count += 1

                        if len(leading_zero_examples) < 5:
                            leading_zero_examples.append(
                                (
                                    line_number,
                                    field_name,
                                    value,
                                )
                            )

                # Quantity magnitude.
                max_quantity["bid_qty"] = max(
                    max_quantity["bid_qty"],
                    Decimal(bid_qty),
                )

                max_quantity["ask_qty"] = max(
                    max_quantity["ask_qty"],
                    Decimal(ask_qty),
                )

                # Exchange event-time ordering.
                event_time = int(message["E"])

                if (
                    previous_event_time is not None
                    and event_time < previous_event_time
                ):
                    event_time_decreases += 1

                    if len(event_time_decrease_examples) < 5:
                        event_time_decrease_examples.append(
                            (
                                line_number,
                                previous_event_time,
                                event_time,
                            )
                        )

                previous_event_time = event_time

                # Capture-clock ordering. Written as an explicit
                # comparison for the same reason replay_schedule.cpp
                # writes it that way: in C++ these are uint64_t, so a
                # backwards step wraps rather than going negative and
                # max(0, b - a) would pass a value near 2^64 straight
                # through. Python has no such trap, and the check is
                # written in the same shape anyway so the two read alike
                # and the clamped total is directly comparable to
                # ReplaySchedule::clamped_ns.
                if previous_capture_ns is not None:
                    if capture_ns < previous_capture_ns:
                        step = previous_capture_ns - capture_ns

                        backwards_steps += 1
                        clamped_ns += step

                        largest_backwards_step_ns = max(
                            largest_backwards_step_ns, step
                        )

                        if len(backwards_step_examples) < 5:
                            backwards_step_examples.append(
                                (
                                    line_number,
                                    previous_capture_ns,
                                    capture_ns,
                                    step,
                                )
                            )

                previous_capture_ns = capture_ns

                bookticker_count += 1

            except Exception as error:
                print()
                print(f"ERROR: {path}")
                print(f"Line: {line_number}")
                print(f"Reason: {error}")
                raise

    overall_max_fractional = max(max_fractional.values())
    overall_max_quantity = max(max_quantity.values())

    print()
    print("=" * 70)
    print(f"File: {path}")
    print(f"Symbol: {symbol}")
    print()

    print("Message counts:")
    print(f"  raw lines:             {raw_line_count}")
    print(f"  bookTicker messages:   {bookticker_count}")
    print(f"  non-payload messages:  {non_payload_count}")
    print()

    print("Maximum fractional digits:")
    print(f"  bid price: {max_fractional['bid_price']}")
    print(f"  ask price: {max_fractional['ask_price']}")
    print(f"  bid qty:   {max_fractional['bid_qty']}")
    print(f"  ask qty:   {max_fractional['ask_qty']}")
    print(f"  overall:   {overall_max_fractional}")
    print()

    print("Maximum quantities:")
    print(f"  bid qty:   {max_quantity['bid_qty']}")
    print(f"  ask qty:   {max_quantity['ask_qty']}")
    print(f"  overall:   {overall_max_quantity}")
    print()

    print("Leading-zero check:")
    print(f"  unusual values: {leading_zero_count}")

    for example in leading_zero_examples:
        line_number, field_name, value = example
        print(
            f"    line {line_number}: "
            f"{field_name} = {value!r}"
        )

    print()

    print("Event-time ordering:")
    print(f"  E decreases: {event_time_decreases}")

    for example in event_time_decrease_examples:
        line_number, previous_e, current_e = example
        print(
            f"    line {line_number}: "
            f"{previous_e} -> {current_e}"
        )

    print()

    print("Capture-clock hygiene (\u00a76.1a):")
    print(f"  backwards steps:       {backwards_steps}")
    print(f"  total clamped:         {clamped_ns} ns")
    print(f"  largest single step:   {largest_backwards_step_ns} ns")

    for example in backwards_step_examples:
        line_number, previous_ns, current_ns, step = example
        print(
            f"    line {line_number}: "
            f"{previous_ns} -> {current_ns}  (back {step} ns)"
        )

    if backwards_steps == 0:
        print()
        print("  Zero backwards steps, so for this file the cumulative")
        print("  clamped form and a plain endpoint subtraction are")
        print("  provably equivalent. Assert that at load time rather")
        print("  than assuming it; the next capture may differ. The")
        print("  clamp stays an explicit comparison regardless.")
    else:
        print()
        print("  NTP stepped the capture clock backwards during this")
        print("  capture. The endpoint form is NOT equivalent here: it")
        print("  would silently re-absorb every gap the clamp removed.")

    if non_payload_examples:
        print()
        print("Non-payload examples:")

        for line_number, raw in non_payload_examples:
            print(f"  line {line_number}: {raw}")

    print("=" * 70)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(
            "Usage: python3 tools/inspect_capture.py "
            "<capture_file> [capture_file ...]"
        )
        sys.exit(1)

    for filename in sys.argv[1:]:
        inspect_capture(Path(filename))