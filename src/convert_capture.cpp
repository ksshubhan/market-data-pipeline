#include "capture_file.hpp"
#include "parser.hpp"
#include "record.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>


static_assert(sizeof(BinaryHeader) == 64);
static_assert(sizeof(CaptureRecord) == 56);
static_assert(kCaptureRecordSize == sizeof(CaptureRecord));


namespace {

const char* parse_error_name(ParseError error) noexcept
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

    return "unknown_parse_error";
}


bool parse_uint64(
    std::string_view text,
    std::uint64_t& output
) noexcept
{
    if (text.empty()) {
        return false;
    }

    const char* const begin = text.data();
    const char* const end = text.data() + text.size();

    const auto result = std::from_chars(
        begin,
        end,
        output
    );

    return result.ec == std::errc{} && result.ptr == end;
}


int hex_digit_value(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }

    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }

    return -1;
}


bool parse_git_commit(
    std::string_view hex,
    std::uint8_t (&output)[20]
) noexcept
{
    if (hex.size() != 40) {
        return false;
    }

    for (std::size_t i = 0; i < 20; ++i) {
        const int high = hex_digit_value(hex[i * 2]);
        const int low = hex_digit_value(hex[i * 2 + 1]);

        if (high < 0 || low < 0) {
            return false;
        }

        output[i] = static_cast<std::uint8_t>(
            (high << 4) | low
        );
    }

    return true;
}


bool write_bytes(
    std::ofstream& output,
    const void* data,
    std::size_t size
)
{
    output.write(
        static_cast<const char*>(data),
        static_cast<std::streamsize>(size)
    );

    return static_cast<bool>(output);
}


void remove_temp_file(
    const std::filesystem::path& temp_path
) noexcept
{
    std::error_code error;
    std::filesystem::remove(temp_path, error);
}

} // namespace


int main(int argc, char* argv[])
{
    if (argc != 6) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0]
            << " <input.log> <output.bin> <symbol>"
            << " <git-commit-40-hex> <dirty:0|1>\n";

        return 1;
    }

    const std::filesystem::path input_path = argv[1];
    const std::filesystem::path output_path = argv[2];
    const std::string symbol = argv[3];
    const std::string git_commit_hex = argv[4];
    const std::string dirty_text = argv[5];

    if (symbol.empty() || symbol.size() > 15) {
        std::cerr
            << "error: symbol must contain between 1 and 15 characters\n";
        return 1;
    }

    bool dirty = false;

    if (dirty_text == "0") {
        dirty = false;
    } else if (dirty_text == "1") {
        dirty = true;
    } else {
        std::cerr
            << "error: dirty flag must be either 0 or 1\n";
        return 1;
    }

    BinaryHeader header{};

    std::memcpy(
        header.magic,
        kCaptureMagic,
        sizeof(kCaptureMagic)
    );

    header.record_count = kUnfinalizedRecordCount;
    header.format_version = kCaptureFormatVersion;
    header.header_size = kCaptureHeaderSize;
    header.record_size = kCaptureRecordSize;
    header.scale_exponent = kCaptureScaleExponent;

    header.market_type =
        static_cast<std::uint8_t>(MarketType::futures);

    header.stream_type =
        static_cast<std::uint8_t>(StreamType::book_ticker);

    header.flags =
        dirty ? kHeaderFlagDirtyBuild : 0;

    std::memcpy(
        header.symbol,
        symbol.data(),
        symbol.size()
    );

    if (!parse_git_commit(
            git_commit_hex,
            header.git_commit
        )) {
        std::cerr
            << "error: git commit must be exactly 40 hexadecimal characters\n";
        return 1;
    }

    std::error_code filesystem_error;

    if (!std::filesystem::exists(
            input_path,
            filesystem_error
        )) {
        std::cerr
            << "error: input file does not exist: "
            << input_path << '\n';
        return 1;
    }

    if (filesystem_error) {
        std::cerr
            << "error: could not inspect input file: "
            << filesystem_error.message() << '\n';
        return 1;
    }

    if (std::filesystem::exists(
            output_path,
            filesystem_error
        )) {
        std::cerr
            << "error: refusing to overwrite existing output file: "
            << output_path << '\n';
        return 1;
    }

    if (filesystem_error) {
        std::cerr
            << "error: could not inspect output path: "
            << filesystem_error.message() << '\n';
        return 1;
    }

    std::filesystem::path temp_path = output_path;
    temp_path += ".tmp";

    if (std::filesystem::exists(
            temp_path,
            filesystem_error
        )) {
        std::cerr
            << "error: temporary file already exists: "
            << temp_path << '\n'
            << "Remove or inspect it before retrying.\n";
        return 1;
    }

    if (filesystem_error) {
        std::cerr
            << "error: could not inspect temporary path: "
            << filesystem_error.message() << '\n';
        return 1;
    }

    std::ifstream input(
        input_path,
        std::ios::in | std::ios::binary
    );

    if (!input) {
        std::cerr
            << "error: could not open input file: "
            << input_path << '\n';
        return 1;
    }

    std::ofstream output(
        temp_path,
        std::ios::out |
        std::ios::binary |
        std::ios::trunc
    );

    if (!output) {
        std::cerr
            << "error: could not create temporary output file: "
            << temp_path << '\n';
        return 1;
    }

    if (!write_bytes(
            output,
            &header,
            sizeof(header)
        )) {
        std::cerr << "error: failed to write binary header\n";
        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    std::string line;
    std::uint64_t line_number = 0;
    std::uint64_t record_count = 0;

    while (std::getline(input, line)) {
        ++line_number;

        const std::size_t tab_position =
            line.find('\t');

        if (tab_position == std::string::npos) {
            std::cerr
                << "error: line "
                << line_number
                << " does not contain the expected tab separator\n";

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        const std::string_view timestamp_text(
            line.data(),
            tab_position
        );

        const std::string_view json(
            line.data() + tab_position + 1,
            line.size() - tab_position - 1
        );

        std::uint64_t capture_wall_time_ns = 0;

        if (!parse_uint64(
                timestamp_text,
                capture_wall_time_ns
            )) {
            std::cerr
                << "error: invalid capture timestamp on line "
                << line_number << '\n';

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        CaptureRecord record{};

        const ParseError parse_error =
            parse_book_ticker(
                json,
                capture_wall_time_ns,
                symbol,
                record
            );

        if (parse_error != ParseError::none) {
            std::cerr
                << "error: parser failure on line "
                << line_number
                << ": "
                << parse_error_name(parse_error)
                << '\n';

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        if (!write_bytes(
                output,
                &record,
                sizeof(record)
            )) {
            std::cerr
                << "error: failed writing record from line "
                << line_number << '\n';

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        if (record_count ==
            std::numeric_limits<std::uint64_t>::max()) {
            std::cerr
                << "error: record count overflow\n";

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        ++record_count;
    }

    if (input.bad()) {
        std::cerr
            << "error: I/O failure while reading input file\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    output.seekp(
        static_cast<std::streamoff>(
            offsetof(BinaryHeader, record_count)
        ),
        std::ios::beg
    );

    if (!output) {
        std::cerr
            << "error: failed to seek back to record_count\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    if (!write_bytes(
            output,
            &record_count,
            sizeof(record_count)
        )) {
        std::cerr
            << "error: failed to finalize record_count\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    output.flush();

    if (!output) {
        std::cerr
            << "error: failed to flush output file\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    output.close();

    if (!output) {
        std::cerr
            << "error: failed to close output file cleanly\n";

        remove_temp_file(temp_path);
        return 1;
    }

    constexpr std::uint64_t header_size =
        sizeof(BinaryHeader);

    constexpr std::uint64_t record_size =
        sizeof(CaptureRecord);

    if (
        record_count >
        (
            std::numeric_limits<std::uint64_t>::max()
            - header_size
        ) / record_size
    ) {
        std::cerr
            << "error: expected file size would overflow uint64_t\n";

        remove_temp_file(temp_path);
        return 1;
    }

    const std::uint64_t expected_file_size =
        header_size + record_count * record_size;

    const std::uintmax_t actual_file_size =
        std::filesystem::file_size(
            temp_path,
            filesystem_error
        );

    if (filesystem_error) {
        std::cerr
            << "error: could not determine temporary file size: "
            << filesystem_error.message()
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    if (actual_file_size != expected_file_size) {
        std::cerr
            << "error: binary size validation failed\n"
            << "  expected: "
            << expected_file_size
            << " bytes\n"
            << "  actual:   "
            << actual_file_size
            << " bytes\n";

        remove_temp_file(temp_path);
        return 1;
    }

    std::filesystem::rename(
        temp_path,
        output_path,
        filesystem_error
    );

    if (filesystem_error) {
        std::cerr
            << "error: could not rename temporary file to final output: "
            << filesystem_error.message()
            << '\n';

        return 1;
    }

    std::cout
        << "conversion complete\n"
        << "  input:   " << input_path << '\n'
        << "  output:  " << output_path << '\n'
        << "  symbol:  " << symbol << '\n'
        << "  records: " << record_count << '\n'
        << "  bytes:   " << expected_file_size << '\n';

    return 0;
}