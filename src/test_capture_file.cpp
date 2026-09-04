#include "capture_file.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>


// A validator tested only against a good file tests nothing: the whole
// job is rejecting bad ones. So each case builds a valid 10-record file
// in memory, corrupts exactly one field, writes it to a temporary path,
// and asserts the specific error comes back.
//
// No assert() anywhere: every configured build defines NDEBUG.

namespace {

std::uint64_t g_failures = 0;

constexpr std::uint64_t kTestRecordCount = 10;
constexpr const char* kTestSymbol = "BTCUSDT";


std::vector<std::uint8_t> make_valid_file()
{
    BinaryHeader header = {};

    std::memcpy(header.magic, kCaptureMagic, sizeof(kCaptureMagic));

    header.record_count = kTestRecordCount;
    header.format_version = kCaptureFormatVersion;
    header.header_size = kCaptureHeaderSize;
    header.record_size = kCaptureRecordSize;
    header.scale_exponent = kCaptureScaleExponent;
    header.market_type = static_cast<std::uint8_t>(MarketType::futures);
    header.stream_type = static_cast<std::uint8_t>(StreamType::book_ticker);
    header.flags = 0;

    std::memcpy(header.symbol, kTestSymbol, std::strlen(kTestSymbol));

    std::vector<std::uint8_t> bytes(
        kCaptureHeaderSize + kTestRecordCount * sizeof(CaptureRecord)
    );

    std::memcpy(bytes.data(), &header, sizeof(header));

    for (std::uint64_t i = 0; i < kTestRecordCount; ++i) {
        CaptureRecord record = {};

        record.capture_wall_time_ns = 1787231294033876000ULL + i;
        record.event_time_ms = 1787231293915ULL + i;
        record.transaction_time_ms = 1787231293915ULL + i;
        record.bid_price = 7'200'900'000'000LL + static_cast<std::int64_t>(i);
        record.ask_price = 7'200'910'000'000LL + static_cast<std::int64_t>(i);
        record.bid_qty = 306'500'000LL;
        record.ask_qty = 218'000'000LL;

        std::memcpy(
            bytes.data() + kCaptureHeaderSize + i * sizeof(CaptureRecord),
            &record,
            sizeof(record)
        );
    }

    return bytes;
}


BinaryHeader* header_of(std::vector<std::uint8_t>& bytes)
{
    return reinterpret_cast<BinaryHeader*>(bytes.data());
}


bool write_temp(const std::vector<std::uint8_t>& bytes, std::string& path)
{
    char name[] = "/tmp/mdcapbin_test_XXXXXX";

    const int fd = ::mkstemp(name);

    if (fd < 0) {
        return false;
    }

    const bool ok =
        bytes.empty() ||
        ::write(fd, bytes.data(), bytes.size()) ==
            static_cast<ssize_t>(bytes.size());

    ::close(fd);

    path = name;

    return ok;
}


void expect(
    const char* test_name,
    const std::vector<std::uint8_t>& bytes,
    std::string_view expected_symbol,
    CaptureFileError expected
)
{
    std::string path;

    if (!write_temp(bytes, path)) {
        std::cerr
            << "FAIL [" << test_name << "] could not write fixture\n";
        ++g_failures;
        return;
    }

    CaptureFile file;

    const CaptureFileError observed =
        CaptureFile::open(path.c_str(), expected_symbol, file);

    if (observed != expected) {
        std::cerr
            << "FAIL [" << test_name << "] expected "
            << capture_file_error_name(expected)
            << ", got "
            << capture_file_error_name(observed)
            << '\n';

        ++g_failures;
    }

    // A rejected open must leave nothing mapped.
    if (observed != CaptureFileError::none && file.is_open()) {
        std::cerr
            << "FAIL [" << test_name
            << "] rejected file left a mapping open\n";

        ++g_failures;
    }

    ::unlink(path.c_str());
}


void check(const char* test_name, bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL [" << test_name << "] " << what << '\n';
        ++g_failures;
    }
}


void test_valid_file_contents()
{
    const char* name = "valid/contents";

    std::string path;

    if (!write_temp(make_valid_file(), path)) {
        std::cerr << "FAIL [" << name << "] could not write fixture\n";
        ++g_failures;
        return;
    }

    CaptureFile file;

    const CaptureFileError error =
        CaptureFile::open(path.c_str(), kTestSymbol, file);

    if (error != CaptureFileError::none) {
        std::cerr
            << "FAIL [" << name << "] open returned "
            << capture_file_error_name(error) << '\n';
        ++g_failures;
        ::unlink(path.c_str());
        return;
    }

    check(name, file.is_open(), "is_open() false after a good open");

    check(
        name,
        file.header().record_count == kTestRecordCount,
        "header record_count wrong"
    );

    const std::span<const CaptureRecord> records = file.records();

    check(name, records.size() == kTestRecordCount, "span size wrong");

    bool contents_ok = true;

    for (std::uint64_t i = 0; i < kTestRecordCount; ++i) {
        if (records[i].capture_wall_time_ns !=
                1787231294033876000ULL + i ||
            records[i].bid_price !=
                7'200'900'000'000LL + static_cast<std::int64_t>(i)) {
            contents_ok = false;
        }
    }

    check(name, contents_ok, "record contents do not round-trip");

    check(
        name,
        reinterpret_cast<std::uintptr_t>(records.data()) %
            alignof(CaptureRecord) == 0,
        "records span is not aligned"
    );

    check(name, file.page_size() >= 4096, "page size implausible");

    // The mapping is small enough to be resident already; warm() must
    // still return something derived from the bytes so the loop cannot
    // be optimised away.
    check(name, file.warm() != 0, "warm() returned zero");

    // Moving must transfer the mapping rather than double-unmapping it.
    CaptureFile moved = std::move(file);

    check(name, moved.is_open(), "moved-to file is not open");
    check(name, !file.is_open(), "moved-from file still reports open");
    check(
        name,
        moved.records().size() == kTestRecordCount,
        "moved file lost its records"
    );

    ::unlink(path.c_str());
}

} // namespace


int main()
{
    test_valid_file_contents();

    expect(
        "valid/open",
        make_valid_file(),
        kTestSymbol,
        CaptureFileError::none
    );

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->magic[0] = 'X';
        expect("reject/magic", bytes, kTestSymbol,
            CaptureFileError::bad_magic);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->format_version = 2;
        expect("reject/format_version", bytes, kTestSymbol,
            CaptureFileError::unsupported_format_version);
    }

    {
        // Checked after magic and version, never before them.
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->header_size = 32;
        expect("reject/header_size", bytes, kTestSymbol,
            CaptureFileError::bad_header_size);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->record_size = 48;
        expect("reject/record_size", bytes, kTestSymbol,
            CaptureFileError::record_size_mismatch);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->scale_exponent = 6;
        expect("reject/scale_exponent", bytes, kTestSymbol,
            CaptureFileError::bad_scale_exponent);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->market_type = 9;
        expect("reject/market_type", bytes, kTestSymbol,
            CaptureFileError::unknown_market_type);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->stream_type = 9;
        expect("reject/stream_type_unknown", bytes, kTestSymbol,
            CaptureFileError::unknown_stream_type);
    }

    {
        // §7.1: depth is a recognised stream this code refuses to read,
        // which is a different answer from an unrecognised one.
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->stream_type =
            static_cast<std::uint8_t>(StreamType::depth);
        expect("reject/stream_type_depth", bytes, kTestSymbol,
            CaptureFileError::unsupported_stream_type);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->flags = 0x80;
        expect("reject/unknown_flag", bytes, kTestSymbol,
            CaptureFileError::unknown_flags);
    }

    {
        // The dirty-build bit is known and must be accepted.
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->flags = kHeaderFlagDirtyBuild;
        expect("accept/dirty_flag", bytes, kTestSymbol,
            CaptureFileError::none);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->reserved[1] = 1;
        expect("reject/reserved_not_zero", bytes, kTestSymbol,
            CaptureFileError::reserved_not_zero);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        std::memset(header_of(bytes)->symbol, 'A',
            sizeof(header_of(bytes)->symbol));
        expect("reject/symbol_not_terminated", bytes, kTestSymbol,
            CaptureFileError::symbol_not_terminated);
    }

    {
        // Replaying ETHW as BTC is the mistake the symbol field exists
        // to prevent (§7.6).
        expect("reject/symbol_mismatch", make_valid_file(), "ETHWUSDT",
            CaptureFileError::symbol_mismatch);
    }

    {
        // A crash between "write records" and "patch count" leaves a file
        // that is structurally complete. UINT64_MAX is what makes it
        // report as incomplete rather than as a size mismatch (§7.6).
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->record_count = kUnfinalizedRecordCount;
        expect("reject/unfinalized_count", bytes, kTestSymbol,
            CaptureFileError::unfinalized_record_count);
    }

    {
        // Large enough that header_size + count * record_size would wrap
        // if the multiply were not guarded.
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->record_count =
            std::numeric_limits<std::uint64_t>::max() / 8;
        expect("reject/record_count_overflow", bytes, kTestSymbol,
            CaptureFileError::record_count_overflow);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        header_of(bytes)->record_count = kTestRecordCount + 1;
        expect("reject/count_exceeds_file", bytes, kTestSymbol,
            CaptureFileError::file_size_mismatch);
    }

    {
        // Truncated mid-record: the size check catches what a count check
        // alone would not.
        std::vector<std::uint8_t> bytes = make_valid_file();
        bytes.resize(bytes.size() - 20);
        expect("reject/truncated", bytes, kTestSymbol,
            CaptureFileError::file_size_mismatch);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        bytes.push_back(0);
        expect("reject/trailing_bytes", bytes, kTestSymbol,
            CaptureFileError::file_size_mismatch);
    }

    {
        std::vector<std::uint8_t> bytes = make_valid_file();
        bytes.resize(kCaptureHeaderSize - 1);
        expect("reject/shorter_than_header", bytes, kTestSymbol,
            CaptureFileError::file_smaller_than_header);
    }

    {
        expect("reject/empty_file", {}, kTestSymbol,
            CaptureFileError::file_smaller_than_header);
    }

    {
        // A zero-record file is structurally legal.
        std::vector<std::uint8_t> bytes = make_valid_file();
        bytes.resize(kCaptureHeaderSize);
        header_of(bytes)->record_count = 0;
        expect("accept/zero_records", bytes, kTestSymbol,
            CaptureFileError::none);
    }

    {
        CaptureFile file;

        const CaptureFileError error = CaptureFile::open(
            "/tmp/mdcapbin_definitely_does_not_exist",
            kTestSymbol,
            file
        );

        check(
            "reject/missing_file",
            error == CaptureFileError::cannot_open,
            "opening a missing file did not report cannot_open"
        );
    }

    if (g_failures != 0) {
        std::cerr
            << "capture file tests FAILED: "
            << g_failures
            << " check(s) failed\n";

        return 1;
    }

    std::cout << "capture file tests passed\n";

    return 0;
}