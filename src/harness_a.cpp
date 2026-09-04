#include "record.hpp"
#include "spsc_ring_buffer.hpp"
#include "measurement_thread.hpp"

#include <atomic>
#include <time.h>
#include <cstdint>
#include <iostream>
#include <thread>
#include <iomanip>
#include <algorithm>
#include <array>
#include <random>
#include <vector>
#include <cstdlib>
#include <string>
#include <ctime>

namespace {

constexpr std::uint64_t kIterations = 10'000'000;
static_assert(kIterations % 2 == 0);

constexpr std::size_t kCapacity = 1024;
// Raised from 10 for Step 9. The A1 rerun showed queue_seq_cst
// scattering across 22.6-27.0 M/s with no explanation from rejection
// count, round order or shuffle position, so the medians for A2-A4 need
// more trials behind them. 20 rounds costs about 35 seconds per run.
constexpr std::size_t kRounds = 20;

enum class Experiment {
    // A1a atomic-only orderings plus A1b queue orderings.
    A1,

    // A2: producer/consumer index separation, 64 vs 128 vs 256 bytes.
    A2
};

enum class Arm {
    AtomicRelaxed,
    AtomicAcquireRelease,
    AtomicSeqCst,
    QueueAcquireRelease,
    QueueSeqCst,

    // A2. Acquire-release throughout; only the separation differs, so the
    // slot array is byte-identical across the three arms and there is no
    // working-set confound. The 64-byte arm is the one under test: this
    // machine has 128-byte hardware lines (hw.cachelinesize), so padding
    // each index block to 64 leaves both blocks inside one line and the
    // false sharing is real rather than hypothetical.
    QueueSeparation64,
    QueueSeparation128,
    QueueSeparation256
};

const char* arm_name(Arm arm)
{
    switch (arm) {
    case Arm::AtomicRelaxed:
        return "atomic_relaxed";
    case Arm::AtomicAcquireRelease:
        return "atomic_acquire_release";
    case Arm::AtomicSeqCst:
        return "atomic_seq_cst";
    case Arm::QueueAcquireRelease:
        return "queue_acquire_release";
    case Arm::QueueSeqCst:
        return "queue_seq_cst";
    case Arm::QueueSeparation64:
        return "queue_separation_64";
    case Arm::QueueSeparation128:
        return "queue_separation_128";
    case Arm::QueueSeparation256:
        return "queue_separation_256";
    }

    std::abort();
}

using AcquireReleaseQueue = SpscRingBuffer<
    Record,
    kCapacity,
    128,
    SpscMemoryOrder::AcquireRelease
>;

using SeqCstQueue = SpscRingBuffer<
    Record,
    kCapacity,
    128,
    SpscMemoryOrder::SeqCst
>;

// A2 arms. Separation128 is the same instantiation as
// AcquireReleaseQueue; it is named separately so the results file records
// which experiment produced the row.
using Separation64Queue = SpscRingBuffer<
    Record,
    kCapacity,
    64,
    SpscMemoryOrder::AcquireRelease
>;

using Separation128Queue = SpscRingBuffer<
    Record,
    kCapacity,
    128,
    SpscMemoryOrder::AcquireRelease
>;

using Separation256Queue = SpscRingBuffer<
    Record,
    kCapacity,
    256,
    SpscMemoryOrder::AcquireRelease
>;

struct TrialResult {
    std::size_t round;
    Arm arm;
    double seconds;
    std::uint64_t full_rejections;
};

struct AtomicRunResult {
    std::uint64_t handoffs_completed;
    double seconds;
    QosResult qos;
};

struct RunResult {
    std::uint64_t pushes_completed;
    std::uint64_t pops_completed;
    std::uint64_t full_rejections;
    double seconds;
    QosResult qos;
};

// Provenance is supplied by the caller rather than queried at runtime,
// matching convert_capture (§7.6): the results file must describe the
// build that produced it, not the state of the working tree at some later
// moment. Emitting it into the results file itself means the artifact is
// self-describing — the environment dump is still committed alongside,
// but the pairing becomes checkable rather than assumed by timestamp.
struct Provenance {
    std::string git_commit;
    bool dirty;
    Experiment experiment;
};

const char* experiment_name(Experiment experiment)
{
    switch (experiment) {
    case Experiment::A1:
        return "a1";
    case Experiment::A2:
        return "a2";
    }

    std::abort();
}

bool parse_provenance(int argc, char* argv[], Provenance& out)
{
    if (argc != 4) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0]
            << " <git-commit-40-hex> <dirty:0|1> <experiment:a1|a2>\n";

        return false;
    }

    const std::string commit = argv[1];

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

    const std::string experiment_text = argv[3];

    if (experiment_text == "a1") {
        out.experiment = Experiment::A1;
    } else if (experiment_text == "a2") {
        out.experiment = Experiment::A2;
    } else {
        std::cerr << "error: experiment must be a1 or a2\n";
        return false;
    }

    out.git_commit = commit;
    out.dirty = (dirty_text == "1");

    return true;
}

std::string utc_timestamp()
{
    const std::time_t now = std::time(nullptr);

    std::tm utc{};
    gmtime_r(&now, &utc);

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);

    return std::string(buffer);
}

std::uint64_t now_ns() noexcept
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// A trial whose QoS class did not apply is not the trial being reported:
// §5's P-core bias is a stated part of the measurement conditions, so a
// silent fallback would put an unmitigated run in the results file.
bool check_qos(const char* arm, QosResult qos)
{
    if (qos == QosResult::applied) {
        return true;
    }

    std::cerr
        << "QoS not applied for "
        << arm
        << ": "
        << qos_result_name(qos)
        << '\n';

    return false;
}

template <typename Queue>
RunResult run_once()
{
    Queue queue;

    std::atomic<bool> start{false};

    // Both threads apply their QoS class and then announce readiness. The
    // clock does not start until both have done so, so the
    // pthread_set_qos_class_self_np call cannot land inside the measured
    // window.
    std::atomic<int> ready{0};

    std::uint64_t pushes_completed = 0;
    std::uint64_t pops_completed = 0;
    std::uint64_t end_ns = 0;

    QosResult producer_qos = QosResult::not_attempted;
    QosResult consumer_qos = QosResult::not_attempted;

    Record input{};
    input.sequence = 0;

    std::thread consumer([&] {
        consumer_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        Record output{};

        while (!start.load(std::memory_order_acquire)) {
        }

        for (std::uint64_t expected = 0; expected < kIterations;) {
            if (!queue.try_pop(output)) {
                continue;
            }

            if (output.sequence != expected) {
                std::cerr
                    << "sequence mismatch: expected "
                    << expected
                    << ", got "
                    << output.sequence
                    << '\n';

                std::abort();
            }

            ++expected;
            ++pops_completed;
        }

        end_ns = now_ns();
    });

    std::thread producer([&] {
        producer_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        for (std::uint64_t sequence = 0;
             sequence < kIterations;
             ++sequence) {

            input.sequence = sequence;

            while (!queue.try_push(input)) {
                // Harness A policy: retry the same record.
            }

            ++pushes_completed;
        }
    });

    while (ready.load(std::memory_order_acquire) != 2) {
    }

    const std::uint64_t begin_ns = now_ns();

    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    const double elapsed_seconds =
        static_cast<double>(end_ns - begin_ns) / 1'000'000'000.0;

    return RunResult{
        pushes_completed,
        pops_completed,
        queue.full_rejections(),
        elapsed_seconds,
        combine_qos(producer_qos, consumer_qos)
    };
}

template <
    std::memory_order LoadOrder,
    std::memory_order StoreOrder
>
AtomicRunResult run_atomic_once()
{
    std::atomic<std::uint64_t> counter{0};
    std::atomic<bool> start{false};
    std::atomic<int> ready{0};

    std::uint64_t end_ns = 0;

    QosResult producer_qos = QosResult::not_attempted;
    QosResult consumer_qos = QosResult::not_attempted;

    std::thread consumer([&] {
        consumer_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        for (std::uint64_t i = 0; i < kIterations / 2; ++i) {
            const std::uint64_t expected = (2 * i) + 1;

            while (counter.load(LoadOrder) != expected) {
            }

            counter.store(expected + 1, StoreOrder);
        }

        end_ns = now_ns();
    });

    std::thread producer([&] {
        producer_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        for (std::uint64_t i = 0; i < kIterations / 2; ++i) {
            const std::uint64_t expected = 2 * i;

            while (counter.load(LoadOrder) != expected) {
            }

            counter.store(expected + 1, StoreOrder);
        }
    });

    while (ready.load(std::memory_order_acquire) != 2) {
    }

    const std::uint64_t begin_ns = now_ns();

    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    if (counter.load(std::memory_order_relaxed) !=
        kIterations) {
        std::cerr << "atomic counter final value incorrect\n";
        std::abort();
    }

    const double elapsed_seconds =
        static_cast<double>(end_ns - begin_ns) / 1'000'000'000.0;

    return AtomicRunResult{
        kIterations,
        elapsed_seconds,
        combine_qos(producer_qos, consumer_qos)
    };
}

} // namespace

int main(int argc, char* argv[])
{
    Provenance provenance{};

    if (!parse_provenance(argc, argv, provenance)) {
        return 1;
    }

    constexpr std::uint32_t kShuffleSeed = 0xA1A1A1A1u;

    std::cout << std::setprecision(17);

    std::mt19937 rng{kShuffleSeed};

    std::vector<Arm> arms;

    switch (provenance.experiment) {
    case Experiment::A1:
        arms = {
            Arm::AtomicRelaxed,
            Arm::AtomicAcquireRelease,
            Arm::AtomicSeqCst,
            Arm::QueueAcquireRelease,
            Arm::QueueSeqCst
        };
        break;

    case Experiment::A2:
        arms = {
            Arm::QueueSeparation64,
            Arm::QueueSeparation128,
            Arm::QueueSeparation256
        };
        break;
    }

    std::vector<TrialResult> results;
    results.reserve(kRounds * arms.size());

    std::cout
        << "git_commit: " << provenance.git_commit << '\n'
        << "git_dirty: " << (provenance.dirty ? "yes" : "no") << '\n'
        << "utc_timestamp: " << utc_timestamp() << '\n'
        << "iterations_per_trial: " << kIterations << '\n'
        << "rounds: " << kRounds << '\n'
        << "experiment: "
        << experiment_name(provenance.experiment) << '\n'
        << "queue_capacity: " << kCapacity << '\n'
        << "sizeof_queue_separation_64: "
        << sizeof(Separation64Queue) << '\n'
        << "sizeof_queue_separation_128: "
        << sizeof(Separation128Queue) << '\n'
        << "sizeof_queue_separation_256: "
        << sizeof(Separation256Queue) << '\n'
        << "shuffle_seed: " << kShuffleSeed << '\n';

    // §5: a P-core bias hint, not pinning. Every trial verifies the class
    // was actually applied and aborts the run otherwise, so reaching the
    // end of this file means all 50 trials ran under it.
    std::cout
        << "qos_class: user_interactive\n";

    std::cout
        << "round,arm,seconds,completed_handoffs_per_second,"
           "full_rejections\n";

    for (std::size_t round = 0; round < kRounds; ++round) {
        std::shuffle(arms.begin(), arms.end(), rng);

        for (const Arm arm : arms) {
            TrialResult trial{
                round,
                arm,
                0.0,
                0
            };

            switch (arm) {
            case Arm::AtomicRelaxed: {
                const AtomicRunResult result =
                    run_atomic_once<
                        std::memory_order_relaxed,
                        std::memory_order_relaxed
                    >();

                if (result.handoffs_completed != kIterations) {
                    std::cerr << "invalid atomic_relaxed run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                break;
            }

            case Arm::AtomicAcquireRelease: {
                const AtomicRunResult result =
                    run_atomic_once<
                        std::memory_order_acquire,
                        std::memory_order_release
                    >();

                if (result.handoffs_completed != kIterations) {
                    std::cerr
                        << "invalid atomic_acquire_release run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                break;
            }

            case Arm::AtomicSeqCst: {
                const AtomicRunResult result =
                    run_atomic_once<
                        std::memory_order_seq_cst,
                        std::memory_order_seq_cst
                    >();

                if (result.handoffs_completed != kIterations) {
                    std::cerr << "invalid atomic_seq_cst run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                break;
            }

            case Arm::QueueAcquireRelease: {
                const RunResult result =
                    run_once<AcquireReleaseQueue>();

                if (result.pushes_completed != kIterations ||
                    result.pops_completed != kIterations) {
                    std::cerr
                        << "invalid queue_acquire_release run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.full_rejections = result.full_rejections;
                break;
            }

            case Arm::QueueSeqCst: {
                const RunResult result =
                    run_once<SeqCstQueue>();

                if (result.pushes_completed != kIterations ||
                    result.pops_completed != kIterations) {
                    std::cerr << "invalid queue_seq_cst run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.full_rejections = result.full_rejections;
                break;
            }

            case Arm::QueueSeparation64: {
                const RunResult result =
                    run_once<Separation64Queue>();

                if (result.pushes_completed != kIterations ||
                    result.pops_completed != kIterations) {
                    std::cerr << "invalid queue_separation_64 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.full_rejections = result.full_rejections;
                break;
            }

            case Arm::QueueSeparation128: {
                const RunResult result =
                    run_once<Separation128Queue>();

                if (result.pushes_completed != kIterations ||
                    result.pops_completed != kIterations) {
                    std::cerr << "invalid queue_separation_128 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.full_rejections = result.full_rejections;
                break;
            }

            case Arm::QueueSeparation256: {
                const RunResult result =
                    run_once<Separation256Queue>();

                if (result.pushes_completed != kIterations ||
                    result.pops_completed != kIterations) {
                    std::cerr << "invalid queue_separation_256 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.full_rejections = result.full_rejections;
                break;
            }
            }

            const double throughput =
                static_cast<double>(kIterations) / trial.seconds;

            results.push_back(trial);

            std::cout
                << round
                << ','
                << arm_name(arm)
                << ','
                << trial.seconds
                << ','
                << throughput
                << ','
                << trial.full_rejections
                << '\n';
        }
    }

    if (results.size() != kRounds * arms.size()) {
        std::cerr << "unexpected trial count\n";
        return 1;
    }

    return 0;
}