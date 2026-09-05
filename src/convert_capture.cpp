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


// Byte offset of the first difference between two objects of the same
// size, or size when they are identical. It exists only so that a header
// read-back failure is diagnosable: "the header does not match" tells
// you nothing, "first differing byte at offset 8" points straight at
// record_count and offset 16 points at a seek that landed two bytes
// late.
std::size_t first_differing_byte(
    const void* left,
    const void* right,
    std::size_t size
) noexcept
{
    const auto* left_bytes =
        static_cast<const unsigned char*>(left);

    const auto* right_bytes =
        static_cast<const unsigned char*>(right);

    for (std::size_t i = 0; i < size; ++i) {
        if (left_bytes[i] != right_bytes[i]) {
            return i;
        }
    }

    return size;
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

    // -----------------------------------------------------------------
    // Header read-back — §7.6
    // -----------------------------------------------------------------
    //
    // record_count is the only field written by seeking backwards over
    // an already-written file rather than by appending, and three ways
    // that can go wrong leave the file's length untouched: a short
    // write, a seek to the wrong offset, and a buffer that never reached
    // the filesystem. The size check above passes in all three cases, so
    // it cannot stand in for this one.
    //
    // The read goes through a new handle, opened after the write stream
    // was closed. Re-reading through the stream that did the writing
    // would only confirm that its own buffer holds what was put into it,
    // and the buffer is not the layer that would have lost the bytes.
    //
    // The comparison covers all 64 bytes rather than record_count alone.
    // A seek landing at the wrong offset writes eight bytes of count
    // over whatever fields are there, so the damage is not confined to
    // the field being patched. BinaryHeader has no padding — all 64
    // bytes belong to a member, and every offset is statically asserted
    // in capture_file.hpp — so memcmp over it is well defined and
    // returns the same answer on every run.

    BinaryHeader expected_header = header;
    expected_header.record_count = record_count;

    BinaryHeader stored_header{};

    {
        std::ifstream readback(
            temp_path,
            std::ios::in | std::ios::binary
        );

        if (!readback) {
            std::cerr
                << "error: could not reopen the temporary file to"
                << " verify its header\n";

            remove_temp_file(temp_path);
            return 1;
        }

        readback.read(
            reinterpret_cast<char*>(&stored_header),
            static_cast<std::streamsize>(sizeof(stored_header))
        );

        // gcount, not the stream state: reading exactly 64 bytes from a
        // much longer file leaves the stream good, so its state says
        // nothing about how much came back.
        if (readback.gcount() !=
            static_cast<std::streamsize>(sizeof(stored_header))) {
            std::cerr
                << "error: short read while verifying the header\n"
                << "  expected: "
                << sizeof(stored_header)
                << " bytes\n"
                << "  read:     "
                << readback.gcount()
                << " bytes\n";

            remove_temp_file(temp_path);
            return 1;
        }
    }

    if (stored_header.record_count == kUnfinalizedRecordCount) {
        std::cerr
            << "error: the poison record_count is still on disk\n"
            << "  the seek-back-and-patch did not reach the file\n";

        remove_temp_file(temp_path);
        return 1;
    }

    if (stored_header.record_count != record_count) {
        std::cerr
            << "error: the stored record_count disagrees with the"
            << " counter\n"
            << "  counter: "
            << record_count
            << '\n'
            << "  on disk: "
            << stored_header.record_count
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    if (actual_file_size < header_size) {
        std::cerr
            << "error: the temporary file is shorter than its own"
            << " header\n";

        remove_temp_file(temp_path);
        return 1;
    }

    // §7.6 asks for the stored count to be checked against the file
    // length as well as against the counter. Given the two checks above
    // and the size check before them this cannot currently fire — it is
    // the third side of a triangle whose other two sides have already
    // been walked. It is written anyway because it is the constraint
    // §7.6 names, and because it ties the stored field to the file
    // length directly rather than through the in-memory counter, so it
    // still holds if the size check above is ever changed or moved.
    const std::uint64_t implied_record_count =
        (static_cast<std::uint64_t>(actual_file_size) - header_size)
        / record_size;

    if (stored_header.record_count != implied_record_count) {
        std::cerr
            << "error: the stored record_count disagrees with the file"
            << " length\n"
            << "  on disk:      "
            << stored_header.record_count
            << '\n'
            << "  file implies: "
            << implied_record_count
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    if (std::memcmp(
            &stored_header,
            &expected_header,
            sizeof(BinaryHeader)
        ) != 0) {
        const std::size_t offset = first_differing_byte(
            &stored_header,
            &expected_header,
            sizeof(BinaryHeader)
        );

        std::cerr
            << "error: the stored header does not match the header that"
            << " was written\n"
            << "  first differing byte at offset "
            << offset
            << '\n'
            << "  record_count already verified, so the damage is"
            << " elsewhere in the header\n";

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
        << "  bytes:   " << expected_file_size << '\n'
        << "  header:  read back and verified against 64 bytes\n";

    return 0;
}