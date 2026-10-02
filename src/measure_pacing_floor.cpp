// measure_pacing_floor: the replay producer's ceiling, the highest offered
// rate it can hold with no queue attached.
//
// Usage: measure_pacing_floor <git-commit-40-hex> <dirty:0|1>
// The commit and dirty flag are checked against git before anything runs;
// see provenance.hpp.
// Prints provenance and one CSV row per requested rate on stdout; the
// committed run is results/pacing_floor_20260928.txt. macOS only: it exits
// 1 unless the QoS class is applied, and elsewhere it never is.
// Related: replay_producer.hpp (run_replay and the clock spin that paces
// it), replay_schedule.hpp (build_fixed_rate_schedule), harness_b.cpp (the
// sweep this bounds), tools/inspect_interarrival.py (which uses the floor).
//
// Pacing is a spin on the clock, so the producer has a ceiling set by how
// fast it can read the clock and assemble a Record. Above it the requested
// rate is fiction: every message ships late. Knowing the ceiling keeps
// harness B's x-axis to rates the producer can deliver, and gives B2's
// compression analysis the smallest gap the producer can honour. The
// committed run puts it at about 20 ns per record, roughly 50M records/s.
//
// There is no queue on purpose. With one, the answer would be "producer
// plus queue", which is not what the sweep needs to be bounded by.

#include "replay_producer.hpp"
#include "replay_schedule.hpp"
#include "measurement_thread.hpp"
#include "provenance.hpp"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>


namespace {

// Stands in for a queue: the try_push and full_rejections that run_replay
// calls, and no work, so a trial times pacing and Record assembly only.
struct NullSink {
    std::uint64_t accepted = 0;

    bool try_push(const Record& record) noexcept
    {
        // Without an observable use of the record, the compiler may delete
        // the stores in run_replay that build it, and the trial would time
        // an empty loop.
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

    if (!verify_provenance(commit, dirty)) {
        return 1;
    }

    // Bias toward the P-cores, and read back rather than only requested: a
    // ceiling measured on an E-core would understate it. A bias, not
    // pinning; macOS has no pinning.
    const QosResult qos = request_user_interactive_qos();

    if (qos != QosResult::applied) {
        std::cerr
            << "QoS not applied: "
            << qos_result_name(qos)
            << '\n';

        return 1;
    }

    // 200,000 records per trial: long enough that startup is negligible,
    // and the ceiling is a property of the loop, not the run length. The
    // sweep takes about 28 s, 20 of them in the 10k/s trial.
    constexpr std::size_t kRecords = 200'000;

    // The sweep has to run past the ceiling, or it only confirms the
    // producer is fast enough for the range guessed. At 100M/s the period
    // is 10 ns, below the ~19.9 ns timer calibration measured between
    // back-to-back clock reads, so the top rate cannot be held.
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

    // 17 significant digits, so every double in the CSV round-trips
    // exactly.
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

    // Zeroed records: nothing reads them, and copying one costs the same
    // whatever it holds.
    std::vector<CaptureRecord> slice(kRecords);
    std::vector<std::uint32_t> lag_ns;

    prepare_lag_buffer(lag_ns, kRecords);

    for (const double rate : rates) {
        const ReplaySchedule schedule =
            build_fixed_rate_schedule(kRecords, rate);

        NullSink sink;

        // symbol_id, slice_start and first_sequence are 0: they describe a
        // captured slice, and there is none here.
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

        // The schedule's first send is at offset 0 and its last at N-1
        // periods, so this divides N records by N-1 intervals and reads
        // high by N/(N-1), 5 ppm at 200,000 records. That is why sustained
        // ratios sit just above 1.
        const double achieved =
            static_cast<double>(kRecords) / elapsed_seconds;

        // 0.99 is this program's own flag for reading the table, not
        // harness B's lag gate, which rejects a datapoint whose p99 lag
        // exceeds one period. The two disagree near the ceiling: in the
        // committed run 50M/s averages 0.997 and reads yes while its p99
        // lag is about 600 periods.
        const bool sustained = (achieved / rate) >= 0.99;

        std::cout
            << rate << ','
            << achieved << ','
            << (achieved / rate) << ','
            << stats.p50_lag_ns << ','
            << stats.p99_lag_ns << ','
            << stats.max_lag_ns << ','
            << (sustained ? "yes" : "no") << '\n';

        // The null sink accepts everything, so this checks run_replay's own
        // count, not the sink.
        if (stats.pushed != kRecords) {
            std::cerr << "null sink did not accept every record\n";
            return 1;
        }
    }

    return 0;
}