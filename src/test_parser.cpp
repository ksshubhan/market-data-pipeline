// test_parser.cpp: tests for parse_scaled_decimal and parse_book_ticker.
//
// Each decimal case parses one string and checks both the error and the
// output: an accepted case must produce the exact scaled value, and a
// rejected one must leave the output as it was. Each book ticker case
// parses one message and checks the error; the accepted ones also check
// all seven fields of the record by value.
//
// Failures are counted rather than returned on, so a single run reports
// every broken case; the exit status is 1 if any check failed.
//
// Related: parser.hpp and parser.cpp (the code under test), record.hpp
// (CaptureRecord), convert_capture.cpp (the parser's caller).

#include "parser.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>


// These tests deliberately do not use assert(). Both CMake presets set
// CMAKE_BUILD_TYPE=RelWithDebInfo, and CMake appends
// CMAKE_CXX_FLAGS_RELWITHDEBINFO (-O2 -g -DNDEBUG) after CMAKE_CXX_FLAGS,
// so an assert-based suite would compile to nothing and exit 0 whatever
// the parser does. The checks below are ordinary runtime comparisons.

namespace {

std::uint64_t g_failures = 0;

const char* error_name(ParseError error)
{
    switch (error) {
    case ParseError::none:
        return "none";
    case ParseError::empty_input:
        return "empty_input";
    case ParseError::invalid_character:
        return "invalid_character";
    case ParseError::multiple_decimal_points:
        return "multiple_decimal_points";
    case ParseError::missing_integer_digits:
        return "missing_integer_digits";
    case ParseError::missing_fractional_digits:
        return "missing_fractional_digits";
    case ParseError::too_many_fractional_digits:
        return "too_many_fractional_digits";
    case ParseError::overflow:
        return "overflow";
    case ParseError::malformed_json:
        return "malformed_json";
    case ParseError::missing_required_field:
        return "missing_required_field";
    case ParseError::wrong_field_type:
        return "wrong_field_type";
    case ParseError::wrong_event_type:
        return "wrong_event_type";
    case ParseError::symbol_mismatch:
        return "symbol_mismatch";
    }

    return "<unrecognised ParseError>";
}

void check_error(
    const char* test_name,
    ParseError observed,
    ParseError expected
)
{
    if (observed != expected) {
        std::cerr
            << "FAIL [" << test_name << "] error: expected "
            << error_name(expected)
            << ", got "
            << error_name(observed)
            << '\n';

        ++g_failures;
    }
}

void check_int64(
    const char* test_name,
    const char* field,
    std::int64_t observed,
    std::int64_t expected
)
{
    if (observed != expected) {
        std::cerr
            << "FAIL [" << test_name << "] " << field
            << ": expected " << expected
            << ", got " << observed
            << '\n';

        ++g_failures;
    }
}

void check_uint64(
    const char* test_name,
    const char* field,
    std::uint64_t observed,
    std::uint64_t expected
)
{
    if (observed != expected) {
        std::cerr
            << "FAIL [" << test_name << "] " << field
            << ": expected " << expected
            << ", got " << observed
            << '\n';

        ++g_failures;
    }
}


// ---------------------------------------------------------------------
// parse_scaled_decimal
// ---------------------------------------------------------------------

// Written into `output` before every call. A rejected parse must leave it
// untouched; an accepted parse must overwrite it. Using a sentinel rather
// than zero means an accepted parse of "0" is still a real check.
constexpr std::int64_t kSentinel = 12345;

struct DecimalCase {
    const char* name;
    std::string_view input;
    ParseError expected_error;

    // kSentinel for every case that must be rejected.
    std::int64_t expected_output;
};

constexpr std::int64_t kInt64Max =
    std::numeric_limits<std::int64_t>::max();

const DecimalCase kDecimalCases[] = {
    // Accepted.
    {"decimal/zero", "0", ParseError::none, 0},
    {"decimal/one", "1", ParseError::none, 100'000'000},
    {"decimal/large_integer", "72184", ParseError::none, 7'218'400'000'000},
    {"decimal/trailing_zero_fraction", "116234.50", ParseError::none,
        11'623'450'000'000},
    {"decimal/ethw_tick_size", "0.000100", ParseError::none, 10'000},
    {"decimal/one_unit_of_scale", "0.00000001", ParseError::none, 1},

    // Rejected: grammar.
    {"decimal/empty", "", ParseError::empty_input, kSentinel},
    {"decimal/no_integer_digits", ".5",
        ParseError::missing_integer_digits, kSentinel},
    {"decimal/no_fractional_digits", "5.",
        ParseError::missing_fractional_digits, kSentinel},
    {"decimal/two_decimal_points", "1.2.3",
        ParseError::multiple_decimal_points, kSentinel},
    {"decimal/embedded_letter", "12x3",
        ParseError::invalid_character, kSentinel},
    {"decimal/negative_sign", "-1.2",
        ParseError::invalid_character, kSentinel},
    {"decimal/positive_sign", "+1.2",
        ParseError::invalid_character, kSentinel},
    {"decimal/nine_fractional_digits", "1.123456789",
        ParseError::too_many_fractional_digits, kSentinel},

    // Rejected: overflow. The first overflows while accumulating the
    // mantissa. The second, an 11-digit integer, accumulates but exceeds
    // INT64_MAX once scaled by 10^8. The bound is exact, not a digit
    // count: the case below has the same number of integer digits.
    {"decimal/mantissa_overflow", "9223372036854775808",
        ParseError::overflow, kSentinel},
    {"decimal/scaling_overflow", "92233720369",
        ParseError::overflow, kSentinel},

    // The largest value the format can represent, accepted exactly, with
    // an 11-digit integer part.
    {"decimal/int64_max", "92233720368.54775807", ParseError::none,
        kInt64Max},
};

void run_decimal_cases()
{
    for (const DecimalCase& test : kDecimalCases) {
        std::int64_t output = kSentinel;

        const ParseError error =
            parse_scaled_decimal(test.input, output);

        check_error(test.name, error, test.expected_error);
        check_int64(test.name, "output", output, test.expected_output);
    }
}


// ---------------------------------------------------------------------
// parse_book_ticker
// ---------------------------------------------------------------------

constexpr std::uint64_t kCaptureNs = 1787231294033876000ULL;
constexpr std::uint64_t kExchangeMs = 1787231293915ULL;

constexpr std::int64_t kExpectedBidPrice = 7'200'900'000'000LL;
constexpr std::int64_t kExpectedBidQty = 306'500'000LL;
constexpr std::int64_t kExpectedAskPrice = 7'200'910'000'000LL;
constexpr std::int64_t kExpectedAskQty = 218'000'000LL;

struct BookTickerCase {
    const char* name;
    std::string_view json;

    // Supplied by the caller, never inferred from the message.
    std::string_view expected_symbol;

    ParseError expected_error;

    // Only meaningful when expected_error is none. After a failure the
    // caller must not read the record, so rejected cases check the status
    // and nothing else.
    bool check_fields;
};

const BookTickerCase kBookTickerCases[] = {
    {
        "book_ticker/genuine_futures_message",
        R"({"e":"bookTicker","u":11331904350110,"s":"BTCUSDT","ps":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915,"st":1})",
        "BTCUSDT",
        ParseError::none,
        true
    },
    {
        // No field-order assumption.
        "book_ticker/reordered_fields",
        R"({"A":"2.180","E":1787231293915,"b":"72009.00","s":"BTCUSDT","T":1787231293915,"a":"72009.10","e":"bookTicker","B":"3.065"})",
        "BTCUSDT",
        ParseError::none,
        true
    },
    {
        // Binance may add fields at any time.
        "book_ticker/unknown_field_tolerated",
        R"({"e":"bookTicker","s":"BTCUSDT","future_field":"ignored","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})",
        "BTCUSDT",
        ParseError::none,
        true
    },
    {
        // An unknown string value holding a closing brace and an escaped
        // copy of a "b" key and value. Skipping that string has to honour
        // the escapes and ignore the brace, or the rest of the message is
        // misread; the real top-level "b" must win.
        "book_ticker/decoy_brace_and_fake_key",
        R"({"e":"bookTicker","s":"BTCUSDT","future_field":"brace } and fake key \"b\":\"999.99\"","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})",
        "BTCUSDT",
        ParseError::none,
        true
    },
    {
        // Missing required "A" (ask quantity).
        "book_ticker/missing_ask_quantity",
        R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","T":1787231293915,"E":1787231293915})",
        "BTCUSDT",
        ParseError::missing_required_field,
        false
    },
    {
        "book_ticker/symbol_mismatch",
        R"({"e":"bookTicker","s":"ETHUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})",
        "BTCUSDT",
        ParseError::symbol_mismatch,
        false
    },
    {
        "book_ticker/wrong_event_type",
        R"({"e":"aggTrade","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})",
        "BTCUSDT",
        ParseError::wrong_event_type,
        false
    },
    {
        // Trailing comma before the closing brace. Must be reported as
        // malformed JSON, not as a missing field.
        "book_ticker/trailing_comma",
        R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915,})",
        "BTCUSDT",
        ParseError::malformed_json,
        false
    },
    {
        // E must be an unquoted uint64, not a JSON string.
        "book_ticker/quoted_event_time",
        R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":"1787231293915"})",
        "BTCUSDT",
        ParseError::wrong_field_type,
        false
    },
    {
        // Nine fractional digits against a fixed scale of 10^8. Hard
        // failure, never silent truncation.
        "book_ticker/nine_fractional_digits",
        R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.123456789","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})",
        "BTCUSDT",
        ParseError::too_many_fractional_digits,
        false
    },
};

void run_book_ticker_cases()
{
    for (const BookTickerCase& test : kBookTickerCases) {
        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            test.json,
            kCaptureNs,
            test.expected_symbol,
            record
        );

        check_error(test.name, error, test.expected_error);

        if (!test.check_fields || error != ParseError::none) {
            continue;
        }

        check_uint64(
            test.name,
            "capture_wall_time_ns",
            record.capture_wall_time_ns,
            kCaptureNs
        );

        check_uint64(
            test.name,
            "event_time_ms",
            record.event_time_ms,
            kExchangeMs
        );

        check_uint64(
            test.name,
            "transaction_time_ms",
            record.transaction_time_ms,
            kExchangeMs
        );

        // Checked by value because "b"/"B" and "a"/"A" differ only in
        // case: a branch that stored a price in its quantity's field, or
        // the reverse, would still return none.
        check_int64(
            test.name, "bid_price", record.bid_price, kExpectedBidPrice);
        check_int64(
            test.name, "bid_qty", record.bid_qty, kExpectedBidQty);
        check_int64(
            test.name, "ask_price", record.ask_price, kExpectedAskPrice);
        check_int64(
            test.name, "ask_qty", record.ask_qty, kExpectedAskQty);
    }
}

} // namespace


int main()
{
    run_decimal_cases();
    run_book_ticker_cases();

    const std::uint64_t cases =
        (sizeof(kDecimalCases) / sizeof(kDecimalCases[0])) +
        (sizeof(kBookTickerCases) / sizeof(kBookTickerCases[0]));

    if (g_failures != 0) {
        std::cerr
            << "parser tests FAILED: "
            << g_failures
            << " check(s) failed across "
            << cases
            << " cases\n";

        return 1;
    }

    std::cout
        << "parser tests passed ("
        << cases
        << " cases)\n";

    return 0;
}
