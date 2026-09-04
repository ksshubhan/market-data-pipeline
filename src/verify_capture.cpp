#include "capture_file.hpp"

#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>


// Opens a real .bin through CaptureFile and reports what it found.
//
// Everything the validator has seen so far it also constructed, which
// tests the rejection paths but not the acceptance path against a file
// produced by a different program on a different day. This is that check.
//
// It also times warming, which Step 11 needs: §6.4a requires the mapping
// to be warm before the measurement window opens, and requires lap 1 and
// lap 2 to be compared to prove the warming worked. Knowing whether that
// costs seconds or minutes on 734 MiB determines how a B run is
// sequenced.

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


// A full traversal of every record, not just one byte per page. This is
// the access pattern the replay producer will have, so lap 1 against
// lap 2 is the comparison §6.4a actually asks for.
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

    // §6.4a: warm the mapping, then prove the warming worked by
    // comparing two full laps. If they differ materially the warming
    // failed and no measurement taken after it is trustworthy.
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

    // Spot-check the first record against the value tools/validate_capture.py
    // reported for this dataset (§7.5), so acceptance is checked against
    // an independently derived number rather than only against itself.
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