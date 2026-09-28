// Implementation of parse_scaled_decimal and parse_book_ticker (parser.hpp).
//
// parse_book_ticker turns one Binance USD-M futures bookTicker message into
// a CaptureRecord: E and T as unsigned integers, b, B, a and A as signed
// integers at a fixed scale of 10^8, and the capture timestamp copied from
// the caller. It checks that e is "bookTicker" and that s is the symbol the
// caller expects, and writes the output only on success. No allocation,
// no exceptions, no floating point, and no document tree: each key is
// matched as it is scanned.
//
// This is a key scanner for one message schema, not a general JSON parser;
// skip_json_value says which inputs it does not handle. Called by
// convert_capture.cpp for every line of a capture log and by
// measure_parse_cost.cpp; tested by test_parser.cpp. See ARCHITECTURE.md.

#include "parser.hpp"
#include <limits>

namespace {
// Indexed by how many of the 8 fractional digits an input left out, 0 to 8,
// to scale its mantissa up to 10^8. The length is tied to
// kFixedPointFractionalDigits by hand, not checked by the compiler.
constexpr std::int64_t kPowersOfTen[] = {
    1,
    10,
    100,
    1'000,
    10'000,
    100'000,
    1'000'000,
    10'000'000,
    100'000'000
};

void skip_whitespace(
    std::string_view input,
    std::size_t& pos
) noexcept
{
    while (pos < input.size()) {
        const char c = input[pos];

        if (
            c != ' ' &&
            c != '\t' &&
            c != '\n' &&
            c != '\r'
        ) {
            break;
        }

        ++pos;
    }
}

// Reads keys and the six string values the parser uses. The result is a
// view of the raw bytes, which is the string's value only when nothing in
// it is escaped, so a backslash is rejected rather than decoded.
ParseError parse_json_string(
    std::string_view input,
    std::size_t& pos,
    std::string_view& output
) noexcept
{
    if (pos >= input.size() || input[pos] != '"') {
        return ParseError::malformed_json;
    }

    ++pos;

    const std::size_t start = pos;

    while (pos < input.size()) {
        const char c = input[pos];

        if (c == '"') {
            output = input.substr(start, pos - start);
            ++pos;
            return ParseError::none;
        }

        if (c == '\\') {
            return ParseError::malformed_json;
        }

        ++pos;
    }

    return ParseError::malformed_json;
}

// Leading zeros are accepted, which strict JSON forbids.
ParseError parse_json_uint64(
    std::string_view input,
    std::size_t& pos,
    std::uint64_t& output
) noexcept
{
    if (pos >= input.size() ||
        input[pos] < '0' ||
        input[pos] > '9') {
        return ParseError::wrong_field_type;
    }

    std::uint64_t value = 0;

    while (
        pos < input.size() &&
        input[pos] >= '0' &&
        input[pos] <= '9'
    ) {
        const std::uint64_t digit =
            static_cast<std::uint64_t>(input[pos] - '0');

        // value * 10 + digit fits exactly when value <= (max - digit) / 10.
        // Checked before the multiply, so the multiply never wraps.
        if (
            value >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10
        ) {
            return ParseError::overflow;
        }

        value = value * 10 + digit;
        ++pos;
    }

    output = value;
    return ParseError::none;
}


// Skips the value of a key the parser does not read.
ParseError skip_json_value(
    std::string_view input,
    std::size_t& pos
) noexcept
{
    skip_whitespace(input, pos);

    if (pos >= input.size()) {
        return ParseError::malformed_json;
    }

    // A string is skipped escape-aware, so a '}', a ',' or text such as
    // "b":"999.99" inside an unknown string is never read as structure.
    if (input[pos] == '"') {
        ++pos;

        while (pos < input.size()) {
            if (input[pos] == '\\') {
                ++pos;

                if (pos >= input.size()) {
                    return ParseError::malformed_json;
                }

                ++pos;
                continue;
            }

            if (input[pos] == '"') {
                ++pos;
                return ParseError::none;
            }

            ++pos;
        }

        return ParseError::malformed_json;
    }


    // Anything else is assumed to be a number, true, false or null, and is
    // skipped to the next ',' or '}' without being checked. Nested objects
    // and arrays are not supported: the skip stops at the first ',' or '}'
    // inside them.
    while (
        pos < input.size() &&
        input[pos] != ',' &&
        input[pos] != '}'
    ) {
        ++pos;
    }

    return ParseError::none;
}

}

// Accepts digits or digits.digits only: no sign, exponent, whitespace or
// bare decimal point. A ninth fractional digit is rejected, never
// truncated, because the scale is fixed at 10^8.
ParseError parse_scaled_decimal(
    std::string_view input,
    std::int64_t& output
) noexcept
{
    std::int64_t mantissa = 0;
    std::size_t fractional_digits = 0;

    bool seen_decimal_point = false;
    bool seen_integer_digit = false;
    bool seen_fractional_digit = false;

    if (input.empty()) {
        return ParseError::empty_input;
    }

    for (char c : input) {
        if (c >= '0' && c <= '9') {
            const std::int64_t digit = c - '0';

            if (seen_decimal_point) {
                if (fractional_digits >= kFixedPointFractionalDigits) {
                    return ParseError::too_many_fractional_digits;
                }

                ++fractional_digits;
                seen_fractional_digit = true;
            } else {
                seen_integer_digit = true;
            }

            if (
                mantissa >
                (std::numeric_limits<std::int64_t>::max() - digit) / 10
            ) {
                return ParseError::overflow;
            }

            mantissa = mantissa * 10 + digit;

            continue;
        }

        if (c == '.') {
            if (seen_decimal_point) {
                return ParseError::multiple_decimal_points;
            }

            if (!seen_integer_digit) {
                return ParseError::missing_integer_digits;
            }

            seen_decimal_point = true;
            continue;
        }

        return ParseError::invalid_character;

    }

    if (seen_decimal_point && !seen_fractional_digit) {
        return ParseError::missing_fractional_digits;
    }

    const std::size_t missing_fractional_digits =
    kFixedPointFractionalDigits - fractional_digits;

    const std::int64_t multiplier =
    kPowersOfTen[missing_fractional_digits];

    // The exact bound, not a digit count: the largest value accepted is
    // 92233720368.54775807, INT64_MAX at scale 10^8, so an 11-digit integer
    // part is accepted whenever the scaled value fits. The divisor comes
    // from the table, so this is a runtime division, once per field.
    if (
        mantissa >
        std::numeric_limits<std::int64_t>::max() / multiplier
    ) {
        return ParseError::overflow;
    }

    output = mantissa * multiplier;

    return ParseError::none;
}

ParseError parse_book_ticker(
    std::string_view json,
    std::uint64_t capture_wall_time_ns,
    std::string_view expected_symbol,
    CaptureRecord& output
) noexcept
{
    std::size_t pos = 0;

    // Filled locally and copied to output only once every check has passed,
    // so a rejected message leaves the caller's record untouched.
    CaptureRecord record{};
    record.capture_wall_time_ns = capture_wall_time_ns;

    bool seen_event_type = false;
    bool seen_symbol = false;
    bool seen_event_time = false;
    bool seen_transaction_time = false;
    bool seen_bid_price = false;
    bool seen_bid_qty = false;
    bool seen_ask_price = false;
    bool seen_ask_qty = false;

    skip_whitespace(json, pos);

    if (pos >= json.size() || json[pos] != '{') {
        return ParseError::malformed_json;
    }

    ++pos;

    // Fields may arrive in any order. Unknown keys are skipped, since the
    // captured messages already carry fields this parser does not read (u,
    // ps, st). A repeated key is not detected: its last value wins.
    while (true) {
        skip_whitespace(json, pos);

        if (pos >= json.size()) {
            return ParseError::malformed_json;
        }

        if (json[pos] == '}') {
            ++pos;
            break;
        }

        std::string_view key;

        ParseError error = parse_json_string(json, pos, key);

        if (error != ParseError::none) {
            return error;
        }

        skip_whitespace(json, pos);

        if (pos >= json.size() || json[pos] != ':') {
            return ParseError::malformed_json;
        }

        ++pos;
        skip_whitespace(json, pos);

        // Keys are compared byte for byte: "b"/"B" and "a"/"A" are price and
        // quantity. Matched case-insensitively, "B" would reach the price
        // branch first and the quantity would never be set, so every
        // message would fail missing_required_field.
        if (key == "e") {
            if (pos >= json.size() || json[pos] != '"') {
                return ParseError::wrong_field_type;
            }

            std::string_view event_type;

            error = parse_json_string(json, pos, event_type);

            if (error != ParseError::none) {
                return error;
            }

            if (event_type != "bookTicker") {
                return ParseError::wrong_event_type;
            }

            seen_event_type = true;
        }

        // The symbol is validated against the caller's, not stored:
        // CaptureRecord has no symbol field, and the converter writes it once
        // into the .bin header.
        else if (key == "s") {
            if (pos >= json.size() || json[pos] != '"') {
                return ParseError::wrong_field_type;
            }

            std::string_view symbol;

            error = parse_json_string(json, pos, symbol);

            if (error != ParseError::none) {
                return error;
            }

            if (symbol != expected_symbol) {
                return ParseError::symbol_mismatch;
            }

            seen_symbol = true;
        }

        else if (key == "E") {
            error = parse_json_uint64(
                json,
                pos,
                record.event_time_ms
            );

            if (error != ParseError::none) {
                return error;
            }

            seen_event_time = true;
        }

        else if (key == "T") {
            error = parse_json_uint64(
                json,
                pos,
                record.transaction_time_ms
            );

            if (error != ParseError::none) {
                return error;
            }

            seen_transaction_time = true;
        }

        else if (key == "b") {
            if (pos >= json.size() || json[pos] != '"') {
                return ParseError::wrong_field_type;
            }

            std::string_view value;

            error = parse_json_string(json, pos, value);

            if (error != ParseError::none) {
                return error;
            }

            error = parse_scaled_decimal(
                value,
                record.bid_price
            );

            if (error != ParseError::none) {
                return error;
            }

            seen_bid_price = true;
        }

        else if (key == "B") {
            if (pos >= json.size() || json[pos] != '"') {
                return ParseError::wrong_field_type;
            }

            std::string_view value;

            error = parse_json_string(json, pos, value);

            if (error != ParseError::none) {
                return error;
            }

            error = parse_scaled_decimal(
                value,
                record.bid_qty
            );

            if (error != ParseError::none) {
                return error;
            }

            seen_bid_qty = true;
        }

        else if (key == "a") {
            if (pos >= json.size() || json[pos] != '"') {
                return ParseError::wrong_field_type;
            }

            std::string_view value;

            error = parse_json_string(json, pos, value);

            if (error != ParseError::none) {
                return error;
            }

            error = parse_scaled_decimal(
                value,
                record.ask_price
            );

            if (error != ParseError::none) {
                return error;
            }

            seen_ask_price = true;
        }

        else if (key == "A") {
            if (pos >= json.size() || json[pos] != '"') {
                return ParseError::wrong_field_type;
            }

            std::string_view value;

            error = parse_json_string(json, pos, value);

            if (error != ParseError::none) {
                return error;
            }

            error = parse_scaled_decimal(
                value,
                record.ask_qty
            );

            if (error != ParseError::none) {
                return error;
            }

            seen_ask_qty = true;
        }

        else {
            error = skip_json_value(json, pos);

            if (error != ParseError::none) {
                return error;
            }
        }

        skip_whitespace(json, pos);

        if (pos >= json.size()) {
            return ParseError::malformed_json;
        }

        // A comma must be followed by another key, so a trailing comma is
        // malformed_json rather than a missing field.
        if (json[pos] == ',') {
            ++pos;

            skip_whitespace(json, pos);

            if (pos >= json.size() || json[pos] == '}') {
                return ParseError::malformed_json;
            }

            continue;
        }

        if (json[pos] == '}') {
            ++pos;
            break;
        }

        return ParseError::malformed_json;
    }

    skip_whitespace(json, pos);

    if (pos != json.size()) {
        return ParseError::malformed_json;
    }

    // Checked last, so a message that is both malformed and incomplete
    // reports malformed_json.
    if (
        !seen_event_type ||
        !seen_symbol ||
        !seen_event_time ||
        !seen_transaction_time ||
        !seen_bid_price ||
        !seen_bid_qty ||
        !seen_ask_price ||
        !seen_ask_qty
    ) {
        return ParseError::missing_required_field;
    }

    output = record;

    return ParseError::none;
}