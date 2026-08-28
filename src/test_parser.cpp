#include "parser.hpp"

#include <string_view>
#include <cassert>
#include <cstdint>
#include <limits>

int main()
{
    std::int64_t output = 0;

    auto error = parse_scaled_decimal("0", output);
    assert(error == ParseError::none);
    assert(output == 0);

    output = 0;
    error = parse_scaled_decimal("1", output);
    assert(error == ParseError::none);
    assert(output == 100'000'000);

    output = 0;
    error = parse_scaled_decimal("72184", output);
    assert(error == ParseError::none);
    assert(output == 7'218'400'000'000);

    output = 0;
    error = parse_scaled_decimal("116234.50", output);
    assert(error == ParseError::none);
    assert(output == 11'623'450'000'000);

    output = 0;
    error = parse_scaled_decimal("0.000100", output);
    assert(error == ParseError::none);
    assert(output == 10'000);

    output = 0;
    error = parse_scaled_decimal("0.00000001", output);
    assert(error == ParseError::none);
    assert(output == 1);

    output = 12345;
    error = parse_scaled_decimal("", output);
    assert(error == ParseError::empty_input);
    assert(output == 12345);

    output = 12345;
    error = parse_scaled_decimal(".5", output);
    assert(error == ParseError::missing_integer_digits);
    assert(output == 12345);

    output = 12345;
    error = parse_scaled_decimal("5.", output);
    assert(error == ParseError::missing_fractional_digits);
    assert(output == 12345);

    output = 12345;
    error = parse_scaled_decimal("1.2.3", output);
    assert(error == ParseError::multiple_decimal_points);
    assert(output == 12345);

    output = 12345;
    error = parse_scaled_decimal("12x3", output);
    assert(error == ParseError::invalid_character);
    assert(output == 12345);

    output = 12345;
    error = parse_scaled_decimal("-1.2", output);
    assert(error == ParseError::invalid_character);
    assert(output == 12345);

    output = 12345;
    error = parse_scaled_decimal("+1.2", output);
    assert(error == ParseError::invalid_character);
    assert(output == 12345);

    output = 12345;
    error = parse_scaled_decimal("1.123456789", output);
    assert(error == ParseError::too_many_fractional_digits);
    assert(output == 12345);

    // Overflow while accumulating the decimal digits.
    output = 12345;
    error = parse_scaled_decimal("9223372036854775808", output);
    assert(error == ParseError::overflow);
    assert(output == 12345);

    // Mantissa fits, but scaling by 10^8 would overflow.
    output = 12345;
    error = parse_scaled_decimal("92233720369", output);
    assert(error == ParseError::overflow);
    assert(output == 12345);

    output = 0;
    error = parse_scaled_decimal("92233720368.54775807", output);
    assert(error == ParseError::none);
    assert(output == std::numeric_limits<std::int64_t>::max());

    {
        const std::string_view json =
            R"({"e":"bookTicker","u":11331904350110,"s":"BTCUSDT","ps":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915,"st":1})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::none);

        assert(record.capture_wall_time_ns == 1787231294033876000ULL);
        assert(record.event_time_ms == 1787231293915ULL);
        assert(record.transaction_time_ms == 1787231293915ULL);

        assert(record.bid_price == 7'200'900'000'000LL);
        assert(record.bid_qty == 306'500'000LL);
        assert(record.ask_price == 7'200'910'000'000LL);
        assert(record.ask_qty == 218'000'000LL);
    }

    {
        const std::string_view json =
            R"({"A":"2.180","E":1787231293915,"b":"72009.00","s":"BTCUSDT","T":1787231293915,"a":"72009.10","e":"bookTicker","B":"3.065"})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::none);

        assert(record.capture_wall_time_ns == 1787231294033876000ULL);
        assert(record.event_time_ms == 1787231293915ULL);
        assert(record.transaction_time_ms == 1787231293915ULL);

        assert(record.bid_price == 7'200'900'000'000LL);
        assert(record.bid_qty == 306'500'000LL);
        assert(record.ask_price == 7'200'910'000'000LL);
        assert(record.ask_qty == 218'000'000LL);
    }

    {
        // Missing required "A" (ask quantity).
        const std::string_view json =
            R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","T":1787231293915,"E":1787231293915})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::missing_required_field);
    }

    {
        const std::string_view json =
            R"({"e":"bookTicker","s":"ETHUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::symbol_mismatch);
    }

    {
        const std::string_view json =
            R"({"e":"aggTrade","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::wrong_event_type);
    }

    {
        // Trailing comma before closing brace.
        const std::string_view json =
            R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915,})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::malformed_json);
    }

    {
        // E must be an unquoted uint64, not a JSON string.
        const std::string_view json =
            R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":"1787231293915"})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::wrong_field_type);
    }

    {
        // Bid price has 9 fractional digits; scale is fixed at 10^8.
        const std::string_view json =
            R"({"e":"bookTicker","s":"BTCUSDT","b":"72009.123456789","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::too_many_fractional_digits);
    }

    {
        const std::string_view json =
            R"({"e":"bookTicker","s":"BTCUSDT","future_field":"ignored","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::none);

        assert(record.bid_price == 7'200'900'000'000LL);
        assert(record.bid_qty == 306'500'000LL);
        assert(record.ask_price == 7'200'910'000'000LL);
        assert(record.ask_qty == 218'000'000LL);
    }

    {
        // An unknown string contains both a closing brace and text that
        // looks like another top-level "b" key. Neither must confuse
        // the top-level key scanner.
        const std::string_view json =
            R"({"e":"bookTicker","s":"BTCUSDT","future_field":"brace } and fake key \"b\":\"999.99\"","b":"72009.00","B":"3.065","a":"72009.10","A":"2.180","T":1787231293915,"E":1787231293915})";

        CaptureRecord record{};

        const ParseError error = parse_book_ticker(
            json,
            1787231294033876000ULL,
            "BTCUSDT",
            record
        );

        assert(error == ParseError::none);

        // Proves the fake "b":"999.99" inside the string was not
        // mistaken for the real top-level bid-price field.
        assert(record.bid_price == 7'200'900'000'000LL);
        assert(record.bid_qty == 306'500'000LL);
        assert(record.ask_price == 7'200'910'000'000LL);
        assert(record.ask_qty == 218'000'000LL);
    }

    return 0;
}