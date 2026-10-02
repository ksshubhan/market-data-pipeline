// measure_parse_cost: B3, what parsing one message costs against handing it
// off through the ring.
//
// Usage: measure_parse_cost <git-commit-40-hex> <dirty:0|1> <capture.log>
//        <SYMBOL>
// The commit and dirty flag are checked against git before anything runs;
// see provenance.hpp.
// Reads the first 200,000 lines of a capture log and prints provenance,
// three per-message costs and two ratios on stdout; the committed run is
// results/parse_cost_20260928_171443.txt. Exits 2 on bad arguments or
// unverified provenance and 1 on any other failure; macOS only, since it
// exits 1 unless the QoS class is applied.
// Related: parser.cpp (parse_book_ticker, shared with convert_capture),
// spsc_ring_buffer.hpp (the queue), record.hpp (CaptureRecord and Record).
//
// Why a direct comparison rather than an end-to-end --parse-in-ingest arm
// in harness B. Above p99 the end-to-end distribution sits on a ~12 us
// scheduler floor, so a ~170 ns parse would be visible only at p50. And
// the producer would have to hold the raw lines beside the 734 MiB
// dataset: this capture's first 200,000 messages are about 33 MB of JSON,
// so a 2,000,000-message run would need roughly 330 MB. "What does parsing
// cost relative to a handoff" is a question about a mean, and batched
// timing answers it at far higher resolution.
//
// Three arms, interleaved round by round, each reported as the median over
// 20 rounds of ns per message:
//   parse    parse_book_ticker over the captured JSON: the same function
//            the offline converter calls, so one parser is measured.
//   copy     one 56-byte CaptureRecord assignment: the pre-parsed path.
//   handoff  a try_push/try_pop pair through the real ring.
//
// This is a schema-specific key scanner, not a general JSON parser, and the
// output says so. Quoting it as "the cost of JSON parsing" would overclaim
// in the flattering direction.


#include "measurement_thread.hpp"
#include "parser.hpp"
#include "provenance.hpp"
#include "record.hpp"
#include "spsc_ring_buffer.hpp"

#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>


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


// 20 rounds, reported as the median, so one disturbed round cannot move
// the result.
constexpr std::size_t kRounds = 20;

// Enough that each round spans many clock ticks: for this capture, 200,000
// messages are about 33 MB of JSON, loaded once before any timing.
constexpr std::size_t kMessages = 200'000;

// The handoff arm pushes then pops, so occupancy never exceeds one and any
// capacity works; this sets how many slots the loop cycles through.
constexpr std::size_t kRingCapacity = 1024;


void require_qos(const char* arm, QosResult qos)
{
    if (qos != QosResult::applied) {
        std::cerr
            << "error: QoS class not applied on " << arm
            << " (" << qos_result_name(qos) << ")\n";
        std::exit(1);
    }
}


double median_of(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());

    const std::size_t n = values.size();

    return (n % 2 == 1)
        ? values[n / 2]
        : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}


// Reads the capture log and keeps the JSON payloads. Each line is
// "<capture_wall_time_ns>\t<json>", the format convert_capture consumes.
bool load_messages(
    const std::string& path,
    std::size_t wanted,
    std::vector<std::string>& json_out,
    std::vector<std::uint64_t>& timestamps_out
)
{
    std::ifstream input(path);

    if (!input) {
        std::cerr << "error: cannot open " << path << '\n';
        return false;
    }

    json_out.reserve(wanted);
    timestamps_out.reserve(wanted);

    std::string line;

    while (json_out.size() < wanted && std::getline(input, line)) {
        const std::size_t tab = line.find('\t');

        if (tab == std::string::npos) {
            std::cerr
                << "error: line " << (json_out.size() + 1)
                << " has no tab separator\n";
            return false;
        }

        timestamps_out.push_back(
            std::strtoull(line.substr(0, tab).c_str(), nullptr, 10)
        );

        json_out.push_back(line.substr(tab + 1));
    }

    if (json_out.size() < wanted) {
        std::cerr
            << "error: capture has " << json_out.size()
            << " lines, need " << wanted << '\n';
        return false;
    }

    return true;
}


// ---------------------------------------------------------------------
// Arm 1 — parse
// ---------------------------------------------------------------------
double measure_parse(
    const std::vector<std::string>& json,
    const std::vector<std::uint64_t>& timestamps,
    const std::string& symbol,
    std::uint64_t& failures
)
{
    CaptureRecord record{};
    std::uint64_t sink = 0;

    const std::uint64_t begin = now_ns();

    for (std::size_t i = 0; i < json.size(); ++i) {
        const ParseError error = parse_book_ticker(
            json[i],
            timestamps[i],
            symbol,
            record
        );

        if (error != ParseError::none) {
            ++failures;
        }

        sink += static_cast<std::uint64_t>(record.bid_price);
    }

    const std::uint64_t end = now_ns();

    // parse_book_ticker is compiled in parser.cpp and there is no LTO, so
    // these calls cannot be removed. The sink keeps the loop live if that
    // ever changes.
    asm volatile("" :: "r"(sink) : "memory");

    return static_cast<double>(end - begin) /
           static_cast<double>(json.size());
}


// ---------------------------------------------------------------------
// Arm 2 — copy (the pre-parsed path)
// ---------------------------------------------------------------------
double measure_copy(const std::vector<CaptureRecord>& source)
{
    CaptureRecord record{};
    std::uint64_t sink = 0;

    const std::uint64_t begin = now_ns();

    for (std::size_t i = 0; i < source.size(); ++i) {
        record = source[i];
        // Makes all 56 bytes of record observable. Without it the compiler
        // reduces the assignment to a load of bid_price, the one field the
        // sink reads, and the arm times a single load. It emits no
        // instructions.
        asm volatile("" :: "r"(&record) : "memory");
        sink += static_cast<std::uint64_t>(record.bid_price);
    }

    const std::uint64_t end = now_ns();

    asm volatile("" :: "r"(sink) : "memory");

    return static_cast<double>(end - begin) /
           static_cast<double>(source.size());
}


// ---------------------------------------------------------------------
// Arm 3 — handoff through the real queue
// ---------------------------------------------------------------------
//
// Single-threaded push-then-pop rather than two threads. That is
// deliberate and it is a *lower* bound: with one thread the cache line
// is never transferred between cores, so this measures the instruction
// cost of a push and a pop without the coherence traffic a real handoff
// pays. A1b's two-thread figure (~30 ns/handoff) is the number that
// includes transfer.
//
// The lower bound is the right comparison here: if parse cost dominates
// even the cheapest possible handoff, it dominates the real one too.
double measure_handoff(const std::vector<Record>& source)
{
    SpscRingBuffer<Record, kRingCapacity> queue;

    Record popped{};
    std::uint64_t sink = 0;

    const std::uint64_t begin = now_ns();

    for (std::size_t i = 0; i < source.size(); ++i) {
        if (!queue.try_push(source[i])) {
            std::cerr << "error: single-threaded push should never fail\n";
            std::exit(1);
        }

        if (!queue.try_pop(popped)) {
            std::cerr << "error: pop after push should never fail\n";
            std::exit(1);
        }

        sink += popped.sequence;
    }

    const std::uint64_t end = now_ns();

    asm volatile("" :: "r"(sink) : "memory");

    return static_cast<double>(end - begin) /
           static_cast<double>(source.size());
}


struct Options {
    std::string git_commit;
    bool dirty = false;
    std::string log_path;
    std::string symbol;
};


bool parse_options(int argc, char* argv[], Options& out)
{
    if (argc != 5) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0]
            << " <git-commit-40-hex> <dirty:0|1> <capture.log> <SYMBOL>\n";
        return false;
    }

    const std::string commit = argv[1];

    if (commit.size() != 40 ||
        commit.find_first_not_of("0123456789abcdef") != std::string::npos) {
        std::cerr << "error: git commit must be 40 lowercase hex characters\n";
        return false;
    }

    const std::string dirty_text = argv[2];

    if (dirty_text != "0" && dirty_text != "1") {
        std::cerr << "error: dirty flag must be either 0 or 1\n";
        return false;
    }

    out.git_commit = commit;
    out.dirty = (dirty_text == "1");
    out.log_path = argv[3];
    out.symbol = argv[4];

    return true;
}

} // namespace


int main(int argc, char* argv[])
{
    Options options;

    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    if (!verify_provenance(options.git_commit, options.dirty)) {
        return 2;
    }

    require_qos("main", request_user_interactive_qos());

    std::vector<std::string> json;
    std::vector<std::uint64_t> timestamps;

    if (!load_messages(options.log_path, kMessages, json, timestamps)) {
        return 1;
    }

    // Build the pre-parsed inputs once, outside every measured window,
    // from the same messages the parse arm consumes. Using the same
    // source keeps the three arms comparable.
    std::vector<CaptureRecord> captures(kMessages);
    std::vector<Record> records(kMessages);

    for (std::size_t i = 0; i < kMessages; ++i) {
        const ParseError error = parse_book_ticker(
            json[i],
            timestamps[i],
            options.symbol,
            captures[i]
        );

        if (error != ParseError::none) {
            // Numeric rather than named: parse_error_name lives in
            // convert_capture.cpp, not the header, and copying its
            // thirteen-case switch here to name a failure that should not
            // occur on a capture the converter accepts is not worth the
            // duplication. The enum is in parser.hpp.
            std::cerr
                << "error: message " << i
                << " failed to parse (ParseError code "
                << static_cast<int>(error)
                << ", see parser.hpp)\n";
            return 1;
        }

        records[i].sequence = i;
        records[i].replay_intended_send_ns = 0;
        records[i].capture = captures[i];
        records[i].symbol_id = 1;
    }

    std::vector<double> parse_rounds;
    std::vector<double> copy_rounds;
    std::vector<double> handoff_rounds;

    std::uint64_t parse_failures = 0;

    // Interleave rather than run each arm to completion, so thermal drift
    // on a fanless M2 spreads across the arms instead of loading onto
    // whichever ran last.
    for (std::size_t round = 0; round < kRounds; ++round) {
        parse_rounds.push_back(
            measure_parse(json, timestamps, options.symbol, parse_failures)
        );
        copy_rounds.push_back(measure_copy(captures));
        handoff_rounds.push_back(measure_handoff(records));
    }

    if (parse_failures != 0) {
        std::cerr
            << "error: " << parse_failures
            << " parse failures during measurement\n";
        return 1;
    }

    const double parse_ns = median_of(parse_rounds);
    const double copy_ns = median_of(copy_rounds);
    const double handoff_ns = median_of(handoff_rounds);

    std::cout
        << "experiment: b3_parse_cost\n"
        << "git_commit: " << options.git_commit << '\n'
        << "git_dirty: " << (options.dirty ? "yes" : "no") << '\n'
        << "capture: " << options.log_path << '\n'
        << "symbol: " << options.symbol << '\n'
        << "messages_per_round: " << kMessages << '\n'
        << "rounds: " << kRounds << '\n'
        << "ring_capacity: " << kRingCapacity << '\n'
        << "qos_class: user_interactive\n"
        << '\n'
        << "parse_ns_per_message: " << parse_ns << '\n'
        << "copy_ns_per_message: " << copy_ns << '\n'
        << "handoff_ns_per_message: " << handoff_ns << '\n'
        << '\n'
        << "parse_over_handoff: " << (parse_ns / handoff_ns) << '\n'
        << "parse_over_copy: " << (parse_ns / copy_ns) << '\n'
        << '\n'
        << "# The handoff arm is single-threaded, so it pays no cross-core\n"
        << "# coherence traffic and is a LOWER bound on a real handoff.\n"
        << "# A1b's two-thread figure is ~30 ns. The lower bound is the\n"
        << "# right comparison: if parse dominates the cheapest possible\n"
        << "# handoff, it dominates the real one.\n"
        << "#\n"
        << "# This is a schema-specific key scanner, not a general JSON\n"
        << "# parser. It assumes a known Binance bookTicker layout. A\n"
        << "# generic JSON library would be considerably slower, so this\n"
        << "# figure must not be quoted as the cost of 'JSON parsing'.\n";

    return 0;
}
