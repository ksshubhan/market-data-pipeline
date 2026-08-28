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

    non_payload_examples = []

    with path.open() as file:
        for line_number, line in enumerate(file, start=1):
            raw_line_count += 1

            try:
                capture_timestamp, raw_message = (
                    line.rstrip("\n").split("\t", 1)
                )

                # Validate that the capture-side timestamp is numeric.
                int(capture_timestamp)

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

    if non_payload_examples:
        print()
        print("Non-payload examples:")

        for line_number, raw in non_payload_examples:
            print(f"  line {line_number}: {raw}")

    print("=" * 70)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(
            "Usage: python tools/inspect_capture_precision.py "
            "<capture_file> [capture_file ...]"
        )
        sys.exit(1)

    for filename in sys.argv[1:]:
        inspect_capture(Path(filename))