#!/usr/bin/env python3

import argparse
import json
import struct
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path


HEADER_FORMAT = "<8sQHHHBBBB16s20s2s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

RECORD_FORMAT = "<QQQqqqq"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

MAGIC = b"MDCAPBIN"
FORMAT_VERSION = 1
SCALE_EXPONENT = 8
SCALE = Decimal(10) ** SCALE_EXPONENT

MARKET_FUTURES = 1
STREAM_BOOK_TICKER = 1


def scaled_decimal(text: str) -> int:
    try:
        value = Decimal(text)
    except InvalidOperation as exc:
        raise ValueError(f"invalid decimal: {text!r}") from exc

    scaled = value * SCALE

    if scaled != scaled.to_integral_value():
        raise ValueError(
            f"value has more than {SCALE_EXPONENT} fractional digits: {text!r}"
        )

    result = int(scaled)

    if not -(2**63) <= result <= 2**63 - 1:
        raise ValueError(f"scaled value outside int64 range: {text!r}")

    return result


def decode_symbol(raw: bytes) -> str:
    symbol_bytes = raw.split(b"\0", 1)[0]

    try:
        return symbol_bytes.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("header symbol is not valid ASCII") from exc


def validate_header(header_bytes: bytes):
    if len(header_bytes) != HEADER_SIZE:
        raise ValueError(
            f"short header: expected {HEADER_SIZE} bytes, "
            f"got {len(header_bytes)}"
        )

    (
        magic,
        record_count,
        format_version,
        header_size,
        record_size,
        scale_exponent,
        market_type,
        stream_type,
        flags,
        symbol_raw,
        git_commit,
        reserved,
    ) = struct.unpack(HEADER_FORMAT, header_bytes)

    if magic != MAGIC:
        raise ValueError(
            f"bad magic: expected {MAGIC!r}, got {magic!r}"
        )

    if format_version != FORMAT_VERSION:
        raise ValueError(
            f"unsupported format version: {format_version}"
        )

    if header_size != HEADER_SIZE:
        raise ValueError(
            f"bad header size: expected {HEADER_SIZE}, got {header_size}"
        )

    if record_size != RECORD_SIZE:
        raise ValueError(
            f"bad record size: expected {RECORD_SIZE}, got {record_size}"
        )

    if scale_exponent != SCALE_EXPONENT:
        raise ValueError(
            f"bad scale exponent: expected {SCALE_EXPONENT}, "
            f"got {scale_exponent}"
        )

    if market_type != MARKET_FUTURES:
        raise ValueError(
            f"unexpected market type: {market_type}"
        )

    if stream_type != STREAM_BOOK_TICKER:
        raise ValueError(
            f"unexpected stream type: {stream_type}"
        )

    if flags & ~0x01:
        raise ValueError(
            f"unknown header flag bits set: 0x{flags:02x}"
        )

    if reserved != b"\0\0":
        raise ValueError(
            f"reserved bytes are non-zero: {reserved.hex()}"
        )

    symbol = decode_symbol(symbol_raw)

    return {
        "record_count": record_count,
        "symbol": symbol,
        "flags": flags,
        "git_commit": git_commit.hex(),
    }


class FeedObservations:
    """Properties of the feed itself, accumulated over every record.

    Separate from the round-trip check above, which asks whether the
    converter reproduced the log faithfully. These ask what the log
    actually contains — facts about Binance's data that §7.5 says to
    record rather than assume.

    Nothing here is inferred from a sample maximum being a bound. The
    counts are reported; only the two invariants that a violation would
    make a parser bug are treated as failures.
    """

    def __init__(self) -> None:
        self.records = 0

        # T vs E. Both are uint64 on the wire, so `E - T` UNDERFLOWS
        # rather than going negative — the same unsigned trap §6.1a
        # documents for the replay clamp. Compare the operands; never
        # test the sign of a difference.
        self.t_before_e = 0
        self.t_equals_e = 0
        self.t_after_e = 0
        self.max_e_minus_t_ms = 0
        self.max_t_minus_e_ms = 0

        # Range sanity. §7.5: validate actual invariants, not guesses —
        # do not assert strictly-positive quantities unless Binance
        # guarantees it. So zero quantities are counted, not rejected.
        self.min_bid_price = None
        self.min_ask_price = None
        self.min_bid_qty = None
        self.min_ask_qty = None
        self.max_bid_qty = None
        self.max_ask_qty = None
        self.zero_bid_qty = 0
        self.zero_ask_qty = 0

        # A crossed book (bid >= ask) is not impossible on a real feed,
        # but on top-of-book it is worth knowing about before it turns
        # up as an inexplicable result. Counted, not asserted.
        self.crossed = 0
        self.locked = 0

        # These two would mean a parser bug rather than a feed quirk.
        self.non_positive_price = 0
        self.negative_qty = 0

    @staticmethod
    def _track_min(current, value):
        return value if current is None or value < current else current

    @staticmethod
    def _track_max(current, value):
        return value if current is None or value > current else current

    def observe(self, record) -> None:
        (
            _capture_wall_time_ns,
            event_time_ms,
            transaction_time_ms,
            bid_price,
            ask_price,
            bid_qty,
            ask_qty,
        ) = record

        self.records += 1

        if transaction_time_ms < event_time_ms:
            self.t_before_e += 1
            self.max_e_minus_t_ms = max(
                self.max_e_minus_t_ms,
                event_time_ms - transaction_time_ms,
            )
        elif transaction_time_ms == event_time_ms:
            self.t_equals_e += 1
        else:
            self.t_after_e += 1
            self.max_t_minus_e_ms = max(
                self.max_t_minus_e_ms,
                transaction_time_ms - event_time_ms,
            )

        if bid_price <= 0 or ask_price <= 0:
            self.non_positive_price += 1

        if bid_qty < 0 or ask_qty < 0:
            self.negative_qty += 1

        self.min_bid_price = self._track_min(self.min_bid_price, bid_price)
        self.min_ask_price = self._track_min(self.min_ask_price, ask_price)
        self.min_bid_qty = self._track_min(self.min_bid_qty, bid_qty)
        self.min_ask_qty = self._track_min(self.min_ask_qty, ask_qty)
        self.max_bid_qty = self._track_max(self.max_bid_qty, bid_qty)
        self.max_ask_qty = self._track_max(self.max_ask_qty, ask_qty)

        if bid_qty == 0:
            self.zero_bid_qty += 1

        if ask_qty == 0:
            self.zero_ask_qty += 1

        if bid_price > ask_price:
            self.crossed += 1
        elif bid_price == ask_price:
            self.locked += 1

    def report(self, scale_exponent: int = SCALE_EXPONENT) -> None:
        divisor = 10 ** scale_exponent

        def real(value):
            return "n/a" if value is None else f"{value / divisor:.8f}"

        print()
        print("feed observations (§7.5)")
        print(f"  records:              {self.records:,}")

        print("  transaction vs event time (T vs E):")
        print(f"    T <  E:             {self.t_before_e:,}")
        print(f"    T == E:             {self.t_equals_e:,}")
        print(f"    T >  E:             {self.t_after_e:,}")

        if self.t_before_e:
            print(f"    max (E - T):        {self.max_e_minus_t_ms} ms")

        if self.t_after_e:
            print(f"    max (T - E):        {self.max_t_minus_e_ms} ms")

        if self.t_after_e == 0:
            print("    => T <= E holds for every record in this capture")
        else:
            print("    => T <= E does NOT hold universally; see the count")
            print("       above. Recorded as a fact about the feed, not")
            print("       treated as an error.")

        print("  ranges:")
        print(f"    min bid price:      {real(self.min_bid_price)}")
        print(f"    min ask price:      {real(self.min_ask_price)}")
        print(f"    bid qty:            {real(self.min_bid_qty)}"
              f"  ..  {real(self.max_bid_qty)}")
        print(f"    ask qty:            {real(self.min_ask_qty)}"
              f"  ..  {real(self.max_ask_qty)}")
        print(f"    zero bid qty:       {self.zero_bid_qty:,}")
        print(f"    zero ask qty:       {self.zero_ask_qty:,}")

        print("  book:")
        print(f"    crossed (bid>ask):  {self.crossed:,}")
        print(f"    locked  (bid==ask): {self.locked:,}")


def expected_record(line: str, expected_symbol: str):
    try:
        timestamp_text, raw_json = line.rstrip("\n").split("\t", 1)
    except ValueError as exc:
        raise ValueError("missing tab separator") from exc

    capture_wall_time_ns = int(timestamp_text)

    message = json.loads(raw_json)

    if message.get("e") != "bookTicker":
        raise ValueError(
            f"unexpected event type: {message.get('e')!r}"
        )

    if message.get("s") != expected_symbol:
        raise ValueError(
            f"symbol mismatch: expected {expected_symbol!r}, "
            f"got {message.get('s')!r}"
        )

    event_time_ms = message["E"]
    transaction_time_ms = message["T"]

    if (
        isinstance(event_time_ms, bool)
        or not isinstance(event_time_ms, int)
    ):
        raise ValueError("E is not an integer")

    if (
        isinstance(transaction_time_ms, bool)
        or not isinstance(transaction_time_ms, int)
    ):
        raise ValueError("T is not an integer")

    return (
        capture_wall_time_ns,
        event_time_ms,
        transaction_time_ms,
        scaled_decimal(message["b"]),
        scaled_decimal(message["a"]),
        scaled_decimal(message["B"]),
        scaled_decimal(message["A"]),
    )


def main() -> int:
    parser = argparse.ArgumentParser()

    parser.add_argument("log_path", type=Path)
    parser.add_argument("bin_path", type=Path)

    args = parser.parse_args()

    try:
        binary_size = args.bin_path.stat().st_size

        with args.bin_path.open("rb") as binary:
            header = validate_header(
                binary.read(HEADER_SIZE)
            )

            expected_size = (
                HEADER_SIZE
                + header["record_count"] * RECORD_SIZE
            )

            if binary_size != expected_size:
                raise ValueError(
                    f"binary file size mismatch: "
                    f"expected {expected_size}, got {binary_size}"
                )

            checked = 0
            observations = FeedObservations()

            with args.log_path.open(
                "r",
                encoding="utf-8",
            ) as log_file:

                for line_number, line in enumerate(
                    log_file,
                    start=1,
                ):
                    record_bytes = binary.read(RECORD_SIZE)

                    if len(record_bytes) != RECORD_SIZE:
                        raise ValueError(
                            f"binary ended before log on line "
                            f"{line_number}"
                        )

                    actual = struct.unpack(
                        RECORD_FORMAT,
                        record_bytes,
                    )

                    expected = expected_record(
                        line,
                        header["symbol"],
                    )

                    if actual != expected:
                        field_names = (
                            "capture_wall_time_ns",
                            "event_time_ms",
                            "transaction_time_ms",
                            "bid_price",
                            "ask_price",
                            "bid_qty",
                            "ask_qty",
                        )

                        print(
                            f"record mismatch on line {line_number}",
                            file=sys.stderr,
                        )

                        for name, wanted, got in zip(
                            field_names,
                            expected,
                            actual,
                        ):
                            if wanted != got:
                                print(
                                    f"  {name}: "
                                    f"expected {wanted}, got {got}",
                                    file=sys.stderr,
                                )

                        return 1

                    observations.observe(actual)

                    checked += 1

            if checked != header["record_count"]:
                raise ValueError(
                    f"record count mismatch: "
                    f"header says {header['record_count']}, "
                    f"log contains {checked}"
                )

            if binary.read(1):
                raise ValueError(
                    "binary contains unexpected trailing bytes"
                )

        print("validation complete")
        print(f"  records:    {checked}")
        print(f"  symbol:     {header['symbol']}")
        print(f"  git commit: {header['git_commit']}")
        print(
            f"  dirty:      "
            f"{'yes' if header['flags'] & 0x01 else 'no'}"
        )

        # SCALE_EXPONENT rather than a header field: validate_header has
        # already asserted the file's exponent equals it, so the constant
        # is a checked fact here rather than an assumption. The header
        # dict deliberately returns only what callers need.
        observations.report(SCALE_EXPONENT)

        # Only these two would indicate a parser bug rather than a
        # property of the feed, so only these two fail the run.
        if observations.non_positive_price:
            print(
                f"\nvalidation failed: "
                f"{observations.non_positive_price} record(s) with a "
                f"non-positive price",
                file=sys.stderr,
            )
            return 1

        if observations.negative_qty:
            print(
                f"\nvalidation failed: "
                f"{observations.negative_qty} record(s) with a negative "
                f"quantity",
                file=sys.stderr,
            )
            return 1

        return 0

    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        json.JSONDecodeError,
    ) as exc:
        print(f"validation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())