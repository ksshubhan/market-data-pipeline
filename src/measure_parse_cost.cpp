// measure_parse_cost — B3 (§6.5).
//
// The question B3 asks is "how does parse cost compare to handoff cost?"
// This answers it directly rather than through an end-to-end harness B
// arm, and the reason is worth stating rather than hiding.
//
// §6.5 specifies B3 as a `--parse-in-ingest` flag on the pipeline,
// comparing a pre-parsed replay against one that parses in the ingest
// thread. Two problems with that shape, both discovered after B1 ran:
//
//   1. §8.0c found that above p99 the latency distribution is dominated
//      by a ~12 us scheduler floor. A parse costs ~1-3 us, so in an
//      end-to-end arm it would be legible only at p50 and swamped
//      everywhere else. The comparison would be a p50 comparison
//      wearing a distribution's clothing.
//
//   2. The producer would have to hold the raw log lines for the slice
//      in memory — roughly 400 MB for 2,000,000 messages — beside the
//      734 MiB dataset. §6.4b already treats memory pressure between the
//      sample buffer and the dataset as a real constraint.
//
// Batched timing in harness A's style answers the same question at far
// higher resolution for a fraction of the code, and §6.2's rule points
// the same way: batch where the question is about a mean, time
// per-message where the question is about a distribution. "What does
// parsing cost relative to a handoff" is a question about a mean.
//
// Three arms, interleaved per §5:
//
//   parse     parse_book_ticker over real captured JSON, the same
//             function the offline converter calls. §6.5 requires this:
//             two implementations would make the comparison worthless.
//
//   copy      the pre-parsed path — a 56-byte CaptureRecord assignment,
//             which is what the producer does per record when the data
//             has already been converted.
//
//   handoff   a full try_push/try_pop pair through the real SPSC ring,
//             so the comparison is against this project's own measured
//             handoff rather than against a number quoted from A1b under
//             different conditions.
//
// Reported as ns per message and as the ratio. The honest framing, which
// the output repeats: this is a **schema-specific key scanner**, not a
// general JSON parser. It assumes a known Binance bookTicker layout, and
// a generic JSON library would be considerably slower. Quoting this as
// "JSON parsing costs X" would overclaim in the flattering direction.

#include "measurement_thread.hpp"
#include "parser.hpp"
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


constexpr std::size_t kRounds = 20;

// Enough messages that a round is comfortably longer than a clock tick
// (~41.667 ns) many times over, and few enough that the working set does
// not become the thing being measured. 200,000 real messages is ~40 MB
// of JSON, which fits well inside DRAM streaming without touching the
// 734 MiB dataset.
constexpr std::size_t kMessages = 200'000;

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

    // Without a sink the whole loop is dead at -O2 and the arm measures
    // an empty loop. Same trap as §6.4b's pre-touch.
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
            // Numeric rather than named: parse_error_name lives inside
            // convert_capture.cpp rather than the header, and duplicating
            // a twenty-case switch here to name a failure that cannot
            // occur on an already-validated dataset would be the kind of
            // accretion §7.0 warns about. Cross-reference the enum in
            // parser.hpp if this ever fires.
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

    // §5: interleave rather than run each arm to completion, so thermal
    // drift on a fanless M2 spreads across conditions instead of loading
    // onto whichever ran last.
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
