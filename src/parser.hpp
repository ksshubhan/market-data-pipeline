// The bookTicker parser's interface: its error codes, the fixed-point
// scale, and the two parsing functions, implemented in parser.cpp.
//
// parse_book_ticker turns one Binance USD-M futures bookTicker message into
// a CaptureRecord (record.hpp); parse_scaled_decimal converts one price or
// quantity string. Both report through a ParseError, write their output
// only on success, and neither allocates nor throws. See ARCHITECTURE.md.

#pragma once

#include <cstdint>
#include <string_view>
#include <cstddef>
#include "record.hpp"

// measure_parse_cost prints a failure as this enum's number and points
// here, so the order is part of that output: none is 0 and symbol_mismatch
// is 12. convert_capture and test_parser print the names instead.
enum class ParseError {
    none,

    // From parse_scaled_decimal, and passed through by parse_book_ticker
    // for b, B, a and A. overflow is also returned for E and T.
    empty_input,
    invalid_character,
    multiple_decimal_points,
    missing_integer_digits,
    missing_fractional_digits,
    too_many_fractional_digits,
    overflow,

    // From parse_book_ticker only.
    malformed_json,
    missing_required_field,
    wrong_field_type,
    wrong_event_type,
    symbol_mismatch
};

// Prices and quantities are stored as integers scaled by 10^8. The
// captures need at most 6 fractional digits (results/inspect_capture_*), so
// 8 is headroom. kFixedPointFractionalDigits is the one definition: the
// length of kPowersOfTen in parser.cpp and kCaptureScaleExponent in
// capture_file.hpp, which convert_capture writes into every .bin header,
// are each checked against it at compile time.
inline constexpr std::size_t kFixedPointFractionalDigits = 8;

// Converts "digits" or "digits.digits", with at most 8 fractional digits,
// to an integer scaled by 10^8. The largest value accepted is
// 92233720368.54775807. output is written only when the result is none.
ParseError parse_scaled_decimal(
    std::string_view input,
    std::int64_t& output
) noexcept;

// Parses one message. expected_symbol is compared byte for byte with the
// message's s field and is not stored; capture_wall_time_ns is copied into
// the record unchanged. output is written only when the result is none.
ParseError parse_book_ticker(
    std::string_view json,
    std::uint64_t capture_wall_time_ns,
    std::string_view expected_symbol,
    CaptureRecord& output
) noexcept;