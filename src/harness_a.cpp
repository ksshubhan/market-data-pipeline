#include "record.hpp"
#include "spsc_ring_buffer.hpp"

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

namespace {

constexpr std::uint64_t kIterations = 10'000'000;
static_assert(kIterations % 2 == 0);

constexpr std::size_t kCapacity = 1024;
constexpr std::size_t kRounds = 10;

enum class Arm {
    AtomicRelaxed,
    AtomicAcquireRelease,
    AtomicSeqCst,
    QueueAcquireRelease,
    QueueSeqCst
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

struct TrialResult {
    std::size_t round;
    Arm arm;
    double seconds;
    std::uint64_t full_rejections;
};

struct AtomicRunResult {
    std::uint64_t handoffs_completed;
    double seconds;
};

struct RunResult {
    std::uint64_t pushes_completed;
    std::uint64_t pops_completed;
    std::uint64_t full_rejections;
    double seconds;
};

std::uint64_t now_ns() noexcept
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

template <typename Queue>
RunResult run_once()
{
    Queue queue;

    std::atomic<bool> start{false};

    std::uint64_t pushes_completed = 0;
    std::uint64_t pops_completed = 0;
    std::uint64_t end_ns = 0;

    Record input{};
    input.sequence = 0;

    std::thread consumer([&] {
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
        elapsed_seconds
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

    std::uint64_t end_ns = 0;

    std::thread consumer([&] {
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
        while (!start.load(std::memory_order_acquire)) {
        }

        for (std::uint64_t i = 0; i < kIterations / 2; ++i) {
            const std::uint64_t expected = 2 * i;

            while (counter.load(LoadOrder) != expected) {
            }

            counter.store(expected + 1, StoreOrder);
        }
    });

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
        elapsed_seconds
    };
}

} // namespace

int main()
{
    constexpr std::uint32_t kShuffleSeed = 0xA1A1A1A1u;

    std::cout << std::setprecision(17);

    std::mt19937 rng{kShuffleSeed};

    std::array<Arm, 5> arms{
        Arm::AtomicRelaxed,
        Arm::AtomicAcquireRelease,
        Arm::AtomicSeqCst,
        Arm::QueueAcquireRelease,
        Arm::QueueSeqCst
    };

    std::vector<TrialResult> results;
    results.reserve(kRounds * arms.size());

    std::cout
        << "shuffle_seed: "
        << kShuffleSeed
        << '\n';

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
