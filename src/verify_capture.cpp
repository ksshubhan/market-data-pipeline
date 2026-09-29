// verify_capture.cpp: opens a capture .bin through CaptureFile, prints its
// header and mapping, and times warming and two full traversals.
//
// Usage: verify_capture <capture.bin> <expected-symbol>
//
// The unit tests build their files in memory; this opens one that
// convert_capture wrote, so the acceptance path runs on a real file. It
// exits 1 if the open fails, the two traversals disagree, warm() returns
// 0 or the record span does not match the header's count; otherwise it
// prints the first and last records and "verify_capture: ok".
//
// Related: capture_file.hpp (CaptureFile), convert_capture.cpp (writes the
// file), tools/validate_capture.py (checks every record against the
// capture log).

#include "capture_file.hpp"

#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>


namespace {

std::uint64_t now_ns() noexcept
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}


double seconds_since(std::uint64_t begin_ns) noexcept
{
    return static_cast<double>(now_ns() - begin_ns) / 1'000'000'000.0;
}


std::string hex_commit(const std::uint8_t (&commit)[20])
{
    static const char* digits = "0123456789abcdef";

    std::string out;
    out.reserve(40);

    for (const std::uint8_t byte : commit) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }

    return out;
}


// Reads two fields of every record, not one byte per page as warm() does.
// Records are 56 bytes apart, so every cache line is touched in order, as
// the replay producer does when it copies each record.
std::uint64_t traverse(std::span<const CaptureRecord> records) noexcept
{
    std::uint64_t sink = 0;

    for (const CaptureRecord& record : records) {
        sink += record.capture_wall_time_ns;
        sink += static_cast<std::uint64_t>(record.bid_price);
    }

    return sink;
}

} // namespace


int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0] << " <capture.bin> <expected-symbol>\n";

        return 1;
    }

    CaptureFile file;

    const std::uint64_t open_begin = now_ns();

    const CaptureFileError error =
        CaptureFile::open(argv[1], argv[2], file);

    const double open_seconds = seconds_since(open_begin);

    if (error != CaptureFileError::none) {
        std::cerr
            << "open failed: "
            << capture_file_error_name(error)
            << '\n';

        return 1;
    }

    const BinaryHeader& header = file.header();
    const std::span<const CaptureRecord> records = file.records();

    std::cout
        << "=== header ===\n"
        << "magic: "
        << std::string(header.magic, sizeof(header.magic)) << '\n'
        << "format_version: " << header.format_version << '\n'
        << "header_size: " << header.header_size << '\n'
        << "record_size: " << header.record_size << '\n'
        << "scale_exponent: "
        << static_cast<unsigned>(header.scale_exponent) << '\n'
        << "market_type: "
        << static_cast<unsigned>(header.market_type) << '\n'
        << "stream_type: "
        << static_cast<unsigned>(header.stream_type) << '\n'
        << "flags: " << static_cast<unsigned>(header.flags)
        << (((header.flags & kHeaderFlagDirtyBuild) != 0)
                ? "  (built from a dirty working tree)"
                : "")
        << '\n'
        << "symbol: " << header.symbol << '\n'
        << "git_commit: " << hex_commit(header.git_commit) << '\n'
        << "record_count: " << header.record_count << '\n';

    std::cout
        << "\n=== mapping ===\n"
        << "open_seconds: " << open_seconds << '\n'
        << "records_span: " << records.size() << '\n'
        << "records_aligned: "
        << ((reinterpret_cast<std::uintptr_t>(records.data()) %
                alignof(CaptureRecord) == 0) ? "yes" : "no")
        << '\n'
        << "page_size: " << file.page_size() << '\n'
        << "page_size_apis_agree: "
        << (file.page_size_agrees() ? "yes" : "no") << '\n';

    // Warm the mapping, then time two full traversals and print their
    // ratio. Nothing here judges it: a first lap slower than the second
    // can come from the caches alone, after warming has removed every
    // page fault.
    const std::uint64_t warm_begin = now_ns();
    const std::uint64_t warm_sink = file.warm();
    const double warm_seconds = seconds_since(warm_begin);

    const std::uint64_t lap1_begin = now_ns();
    const std::uint64_t lap1_sink = traverse(records);
    const double lap1_seconds = seconds_since(lap1_begin);

    const std::uint64_t lap2_begin = now_ns();
    const std::uint64_t lap2_sink = traverse(records);
    const double lap2_seconds = seconds_since(lap2_begin);

    std::cout
        << "\n=== warming (§6.4a) ===\n"
        << "warm_seconds: " << warm_seconds << '\n'
        << "lap1_seconds: " << lap1_seconds << '\n'
        << "lap2_seconds: " << lap2_seconds << '\n'
        << "lap1_over_lap2: "
        << (lap2_seconds > 0.0 ? lap1_seconds / lap2_seconds : 0.0)
        << '\n';

    if (lap1_sink != lap2_sink) {
        std::cerr << "\nlap sinks differ, which is impossible\n";
        return 1;
    }

    if (warm_sink == 0) {
        std::cerr << "\nwarm() returned zero\n";
        return 1;
    }

    if (records.size() != header.record_count) {
        std::cerr << "\nspan size does not match record_count\n";
        return 1;
    }

    // Printed for inspection, not checked. The independent check is
    // tools/validate_capture.py, which compares every record with the
    // capture log.
    if (!records.empty()) {
        std::cout
            << "\n=== first record ===\n"
            << "capture_wall_time_ns: "
            << records.front().capture_wall_time_ns << '\n'
            << "event_time_ms: " << records.front().event_time_ms << '\n'
            << "transaction_time_ms: "
            << records.front().transaction_time_ms << '\n'
            << "bid_price: " << records.front().bid_price << '\n'
            << "ask_price: " << records.front().ask_price << '\n'
            << "bid_qty: " << records.front().bid_qty << '\n'
            << "ask_qty: " << records.front().ask_qty << '\n';

        std::cout
            << "\n=== last record ===\n"
            << "capture_wall_time_ns: "
            << records.back().capture_wall_time_ns << '\n'
            << "bid_price: " << records.back().bid_price << '\n'
            << "ask_price: " << records.back().ask_price << '\n';
    }

    std::cout << "\nverify_capture: ok\n";

    return 0;
}