#pragma once

#include <cstdint>
#include <string_view>
#include <cstddef>
#include "record.hpp"

enum class ParseError {
    none,

    // decimal errors
    empty_input,
    invalid_character,
    multiple_decimal_points,
    missing_integer_digits,
    missing_fractional_digits,
    too_many_fractional_digits,
    overflow,

    // bookTicker errors
    malformed_json,
    missing_required_field,
    wrong_field_type,
    wrong_event_type,
    symbol_mismatch
};

inline constexpr std::int64_t kFixedPointScale = 100'000'000;
inline constexpr std::size_t kFixedPointFractionalDigits = 8;

ParseError parse_scaled_decimal(
    std::string_view input,
    std::int64_t& output
) noexcept;

ParseError parse_book_ticker(
    std::string_view json,
    std::uint64_t capture_wall_time_ns,
    std::string_view expected_symbol,
    CaptureRecord& output
) noexcept;