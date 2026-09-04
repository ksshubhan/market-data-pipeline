#include "replay_producer.hpp"
#include "replay_schedule.hpp"
#include "measurement_thread.hpp"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>


// How fast can the producer issue, with nothing attached?
//
// Pacing is a spin on the clock (see replay_producer.hpp), so there is a
// ceiling on offered rate set by how quickly the producer can read the
// clock, stamp a Record and move on. Above that ceiling the requested
// rate is fiction: the schedule says send every 2 µs, the producer needs
// 3 µs, and every message ships late.
//
// Measuring it before harness B is written bounds the sweep. Without this
// number the first B run would put rates on the x-axis that the producer
// physically cannot deliver, §6.4's lag gate would reject them, and the
// time would be spent discovering a property of the producer rather than
// of the queue. It is also the figure §6.5's B2 compression analysis is
// parameterised around and currently leaves unmeasured — "the fraction of
// original gaps that compress below the producer's pacing floor" cannot
// be computed until the floor is known.
//
// There is no queue here on purpose. A queue would add its own cost and
// the answer would be "producer plus queue", which is not the quantity
// the sweep needs to be bounded by.

namespace {

// Stands in for the queue: same call shape, no work, so the loop measures
// pacing and record assembly and nothing else.
struct NullSink {
    std::uint64_t accepted = 0;

    bool try_push(const Record& record) noexcept
    {
        // Consume the record so the assembly above cannot be optimised
        // away. §6.4b's rule about observable side effects applies to any
        // loop whose result is discarded.
        asm volatile("" :: "r"(&record) : "memory");
        ++accepted;
        return true;
    }

    std::uint64_t full_rejections() const noexcept { return 0; }
};


std::string utc_timestamp()
{
    const std::time_t now = std::time(nullptr);

    std::tm utc{};
    gmtime_r(&now, &utc);

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);

    return std::string(buffer);
}


bool parse_provenance(
    int argc,
    char* argv[],
    std::string& commit,
    bool& dirty
)
{
    if (argc != 3) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0]
            << " <git-commit-40-hex> <dirty:0|1>\n";

        return false;
    }

    commit = argv[1];

    if (commit.size() != 40 ||
        commit.find_first_not_of("0123456789abcdef") !=
            std::string::npos) {
        std::cerr
            << "error: git commit must be 40 lowercase hex characters\n";

        return false;
    }

    const std::string dirty_text = argv[2];

    if (dirty_text != "0" && dirty_text != "1") {
        std::cerr << "error: dirty flag must be either 0 or 1\n";
        return false;
    }

    dirty = (dirty_text == "1");

    return true;
}

} // namespace


int main(int argc, char* argv[])
{
    std::string commit;
    bool dirty = false;

    if (!parse_provenance(argc, argv, commit, dirty)) {
        return 1;
    }

    // §5: the same P-core bias hint the A harness uses, verified rather
    // than requested, since a pacing figure measured on an E-core would
    // understate the ceiling.
    const QosResult qos = request_user_interactive_qos();

    if (qos != QosResult::applied) {
        std::cerr
            << "QoS not applied: "
            << qos_result_name(qos)
            << '\n';

        return 1;
    }

    // Two hundred thousand records per trial: long enough that startup is
    // negligible, short enough that the whole sweep takes seconds. The
    // pacing ceiling is a property of the loop, not of the run length.
    constexpr std::size_t kRecords = 200'000;

    // The sweep must run past the point where the producer stops
    // keeping up, or it does not locate a floor — it just confirms the
    // producer is fast enough for whatever range was guessed. The top
    // rates here are deliberately absurd: at 100 MHz the period is 10 ns,
    // which is below the cost of reading the clock once, so the producer
    // cannot possibly hold that schedule and the achieved-rate column
    // must fall away from the requested one.
    const double rates[] = {
        10'000.0,
        50'000.0,
        100'000.0,
        250'000.0,
        500'000.0,
        1'000'000.0,
        2'000'000.0,
        5'000'000.0,
        10'000'000.0,
        20'000'000.0,
        50'000'000.0,
        100'000'000.0
    };

    std::cout << std::setprecision(17);

    std::cout
        << "git_commit: " << commit << '\n'
        << "git_dirty: " << (dirty ? "yes" : "no") << '\n'
        << "utc_timestamp: " << utc_timestamp() << '\n'
        << "records_per_trial: " << kRecords << '\n'
        << "qos_class: user_interactive\n";

    std::cout
        << "requested_rate_hz,achieved_rate_hz,ratio,"
           "p50_lag_ns,p99_lag_ns,max_lag_ns,sustained\n";

    std::vector<CaptureRecord> slice(kRecords);
    std::vector<std::uint32_t> lag_ns;

    prepare_lag_buffer(lag_ns, kRecords);

    for (const double rate : rates) {
        const ReplaySchedule schedule =
            build_fixed_rate_schedule(kRecords, rate);

        NullSink sink;

        const ReplayStats stats = run_replay(
            sink,
            std::span<const CaptureRecord>(slice),
            schedule,
            0,
            0,
            0,
            lag_ns
        );

        const double elapsed_seconds =
            static_cast<double>(stats.finished_ns - stats.t0_ns) /
            1'000'000'000.0;

        const double achieved =
            static_cast<double>(kRecords) / elapsed_seconds;

        // Below 0.99 the producer is not delivering the rate on the
        // x-axis, which is the same criterion §6.4's lag gate applies
        // per-datapoint. Flagged here so the floor is readable at a
        // glance rather than inferred from the ratio column.
        const bool sustained = (achieved / rate) >= 0.99;

        std::cout
            << rate << ','
            << achieved << ','
            << (achieved / rate) << ','
            << stats.p50_lag_ns << ','
            << stats.p99_lag_ns << ','
            << stats.max_lag_ns << ','
            << (sustained ? "yes" : "no") << '\n';

        if (stats.pushed != kRecords) {
            std::cerr << "null sink did not accept every record\n";
            return 1;
        }
    }

    return 0;
}