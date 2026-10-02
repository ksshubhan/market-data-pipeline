// Harness A: queue microbenchmarks with no market data.
//
// Usage: harness_a <git-commit-40-hex> <dirty:0|1>
//                  <experiment:a1|a2|a2b|a3b|a4|a4b>
// The commit and dirty flag are checked against git before anything runs;
// see provenance.hpp.
//
// Each experiment compares a few arms, meaning variants that differ in
// one thing only:
//   a1   memory ordering: relaxed / acquire-release / seq_cst on a bare
//        atomic, and acquire-release / seq_cst in the real queue
//   a2   padding between the queue's two index blocks: 64 / 128 / 256
//   a2b  the same question without the queue: two atomics 16 / 64 / 128 /
//        256 bytes apart
//   a3b  stride between adjacent ring slots: 80 / 128 / 256 bytes
//   a4   cached vs uncached opposite index, in the real queue
//   a4b  the same, with the producer timed alone against a ring it can
//        never fill
//
// Every arm runs kRounds times, in a freshly shuffled order each round so
// thermal drift is spread across arms rather than loaded onto whichever
// runs last. Timing is batched: one clock read before a trial and one
// after, divided by the operations completed, because the 41.67 ns clock
// cannot time a single ~30 ns handoff. Output is a provenance header then
// one CSV row per trial, on stdout; redirect it to a file in results/.
//
// macOS only: uses clock_gettime_nsec_np and the QoS thread hint. See
// ARCHITECTURE.md.

#include "record.hpp"
#include "spsc_ring_buffer.hpp"
#include "measurement_thread.hpp"
#include "false_sharing.hpp"
#include "provenance.hpp"

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
#include <memory>
#include <string>
#include <ctime>

namespace {

// A1's atomic ping-pong splits the iterations into pairs, one per thread,
// so the count must be even.
constexpr std::uint64_t kIterations = 10'000'000;
static_assert(kIterations % 2 == 0);

constexpr std::size_t kCapacity = 1024;
// Enough trials per arm for a stable median. At 10 rounds A1's
// queue_seq_cst arm scattered across 22.6-27.0 M/s with no explanation
// from rejection count, round order or shuffle position.
constexpr std::size_t kRounds = 20;

enum class Experiment {
    // A1a atomic-only orderings plus A1b queue orderings.
    A1,

    // A2: producer/consumer index separation, 64 vs 128 vs 256 bytes.
    A2,

    // A2b: isolated false-sharing microbenchmark. Same separation
    // question as A2 with the queue removed entirely.
    A2b,

    // A3b: slot-level false sharing, again with the queue removed.
    A3b,

    // A4: cached vs uncached opposite index, in the real queue.
    A4,

    // A4b: the same question with the retry loop removed by construction.
    A4b
};

enum class Arm {
    AtomicRelaxed,
    AtomicAcquireRelease,
    AtomicSeqCst,
    QueueAcquireRelease,
    QueueSeqCst,

    // A2. Acquire-release throughout; only the separation differs, so the
    // slot array is byte-identical across the three arms and there is no
    // working-set confound. The 64-byte arm was the one under test, on the
    // expectation that with 128-byte lines (hw.cachelinesize) both blocks
    // would share one line. A2b later measured the coherence granule at 64
    // bytes, so they do not: at 64 there is no false sharing to measure.
    QueueSeparation64,
    QueueSeparation128,
    QueueSeparation256,

    // A2b. No queue: two threads, two atomics, nothing but release
    // stores. SharedLine16 is the positive control — 16 bytes apart is
    // inside any plausible coherence granule, so if it is not markedly
    // slower than the others the benchmark is not provoking false
    // sharing at all and none of the arms mean anything.
    SharedLine16,
    FalseSharing64,
    FalseSharing128,
    FalseSharing256,

    // A3b. Stride between adjacent ring slots. Record stays 80 bytes in
    // every arm; the slot wrapper is what is over-aligned.
    SlotStride80,
    SlotStride128,
    SlotStride256,

    // A4. Both acquire-release at the default separation; the only
    // difference is whether the opposite index is cached.
    QueueCachedIndex,
    QueueUncachedIndex,

    // A4b. Same two variants, measured producer-only against a ring
    // larger than the run.
    ProducerBoundCached,
    ProducerBoundUncached
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
    case Arm::SharedLine16:
        return "shared_line_16";
    case Arm::FalseSharing64:
        return "false_sharing_64";
    case Arm::FalseSharing128:
        return "false_sharing_128";
    case Arm::FalseSharing256:
        return "false_sharing_256";
    case Arm::SlotStride80:
        return "slot_stride_80";
    case Arm::SlotStride128:
        return "slot_stride_128";
    case Arm::SlotStride256:
        return "slot_stride_256";
    case Arm::QueueCachedIndex:
        return "queue_cached_index";
    case Arm::QueueUncachedIndex:
        return "queue_uncached_index";
    case Arm::ProducerBoundCached:
        return "producer_bound_cached";
    case Arm::ProducerBoundUncached:
        return "producer_bound_uncached";
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

// A4 arms. CachedIndexQueue is the same instantiation as
// AcquireReleaseQueue; naming it separately keeps the results file
// self-describing about which experiment produced the row.
using CachedIndexQueue = SpscRingBuffer<
    Record,
    kCapacity,
    128,
    SpscMemoryOrder::AcquireRelease,
    SpscIndexCaching::Cached
>;

using UncachedIndexQueue = SpscRingBuffer<
    Record,
    kCapacity,
    128,
    SpscMemoryOrder::AcquireRelease,
    SpscIndexCaching::Uncached
>;

// The cached indices are retained in the uncached arm, so the two arms
// must have identical layout. If they did not, A4 would vary two things
// at once.
static_assert(sizeof(CachedIndexQueue) == sizeof(UncachedIndexQueue));


// A4b. A4 could not answer the question: its arms differed 9x in
// full_rejections, and within each arm throughput rose with rejections,
// so its throughput difference measured the balance between producer and
// consumer rather than the cached index.
//
// A4b removes the retry loop by construction rather than gating on it.
// The ring holds more slots than the run pushes, so try_push can never
// return false and full_rejections is zero in both arms by arithmetic,
// not by luck.
//
// That also isolates the variable exactly. The cached producer's
// fullness test (tail - cached_head == Capacity) is never true, so it
// never enters the slow path and performs zero cross-core loads for the
// whole run. The uncached producer performs one acquire load of the
// consumer's index per push. The difference is 0 against
// kA4bIterations cross-core loads with nothing else varying.
//
// Only the producer thread is timed. Since it never rejects, no property
// of the consumer's speed can enter the number — but the consumer is
// running concurrently and storing head_ throughout, so the contention
// the uncached load pays for is real.
//
// Cost of the approach: at 1048576 slots the ring is 80 MB, so it is
// DRAM-resident and the payload store is dearer than at capacity 1024.
// That is a constant added to both arms. It dilutes the relative effect;
// it cannot confound it. The larger run buys a measured window of tens of
// milliseconds rather than two, which a shorter first version showed was
// too noisy to use.
constexpr std::size_t kA4bCapacity = 1048576;
constexpr std::uint64_t kA4bIterations = 1000000;

static_assert(
    kA4bIterations < kA4bCapacity,
    "A4b requires a ring larger than the run so the producer cannot fill it"
);

using ProducerBoundCachedQueue = SpscRingBuffer<
    Record,
    kA4bCapacity,
    128,
    SpscMemoryOrder::AcquireRelease,
    SpscIndexCaching::Cached
>;

using ProducerBoundUncachedQueue = SpscRingBuffer<
    Record,
    kA4bCapacity,
    128,
    SpscMemoryOrder::AcquireRelease,
    SpscIndexCaching::Uncached
>;

static_assert(
    sizeof(ProducerBoundCachedQueue) ==
    sizeof(ProducerBoundUncachedQueue)
);

struct TrialResult {
    std::size_t round;
    Arm arm;
    double seconds;
    std::uint64_t full_rejections;

    // Operations completed in this trial. A1, A2 and A4 arms complete
    // kIterations handoffs; A2b and A3b arms complete twice the per-thread
    // count; A4b arms complete kA4bIterations pushes. Kept explicit so the
    // throughput column is never divided by the wrong denominator.
    std::uint64_t operations;
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

// Provenance is supplied by the caller and written into the results file,
// so the file itself says which build produced it and the environment dump
// committed alongside can be matched to it by commit rather than by
// timestamp. main checks both against git before anything runs
// (provenance.hpp).
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
    case Experiment::A2b:
        return "a2b";
    case Experiment::A3b:
        return "a3b";
    case Experiment::A4:
        return "a4";
    case Experiment::A4b:
        return "a4b";
    }

    std::abort();
}

bool parse_provenance(int argc, char* argv[], Provenance& out)
{
    if (argc != 4) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0]
            << " <git-commit-40-hex> <dirty:0|1>"
            << " <experiment:a1|a2|a2b|a3b|a4|a4b>\n";

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
    } else if (experiment_text == "a2b") {
        out.experiment = Experiment::A2b;
    } else if (experiment_text == "a3b") {
        out.experiment = Experiment::A3b;
    } else if (experiment_text == "a4") {
        out.experiment = Experiment::A4;
    } else if (experiment_text == "a4b") {
        out.experiment = Experiment::A4b;
    } else {
        std::cerr
            << "error: experiment must be a1, a2, a2b, a3b, a4"
               " or a4b\n";
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
// the P-core bias is a stated part of the measurement conditions, so a
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
                // Harness A policy: retry the same record, so every trial
                // completes exactly kIterations handoffs and nothing is
                // lost. Rejections are still counted by the queue.
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

// A2b: the separation question with the queue taken out.
//
// A2 could not answer it. Its three arms differed by up to 60x in
// full_rejections, so "completed handoffs per second" was partly
// measuring how far the producer outran the consumer. Here there
// is no queue, no retry loop and no capacity, so the two threads cannot
// get out of balance: each performs exactly the same number of release
// stores to its own atomic and nothing else.
//
// Release stores, not relaxed, because that is what the queue does — the
// producer publishes tail_ with a release store on every push and the
// consumer publishes head_ the same way on every pop. Those unconditional
// stores are what would false-share, not the conditional cross-loads.
//
// SeparatedCounters and separation_store_loop live in
// false_sharing.hpp so that check_a2b_assembly.cpp disassembles the same
// code this harness runs.
struct SeparationRunResult {
    std::uint64_t stores_completed;
    double seconds;
    std::ptrdiff_t observed_separation;
    QosResult qos;
};

constexpr std::uint64_t kSeparationStoresPerThread = 20'000'000;

template <std::size_t Separation>
SeparationRunResult run_separation_once()
{
    SeparatedCounters<Separation> counters;

    // Verify the layout rather than assuming alignas worked.
    const std::ptrdiff_t observed =
        reinterpret_cast<const std::uint8_t*>(&counters.second) -
        reinterpret_cast<const std::uint8_t*>(&counters.first);

    if (observed != static_cast<std::ptrdiff_t>(Separation) ||
        reinterpret_cast<std::uintptr_t>(&counters.first) % 256 != 0) {
        std::cerr << "separation layout incorrect\n";
        std::abort();
    }

    std::atomic<bool> start{false};
    std::atomic<int> ready{0};

    // Both writers record their own finish time and the later one is
    // taken. The two threads are symmetric, so unlike run_once there is no
    // thread that finishes last by construction: reading the clock in only
    // one of them would understate elapsed time whenever the other ran on
    // past it.
    std::uint64_t first_end_ns = 0;
    std::uint64_t second_end_ns = 0;

    QosResult first_qos = QosResult::not_attempted;
    QosResult second_qos = QosResult::not_attempted;

    std::thread writer_second([&] {
        second_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        separation_store_loop(
            counters.second,
            kSeparationStoresPerThread
        );

        second_end_ns = now_ns();
    });

    std::thread writer_first([&] {
        first_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        separation_store_loop(
            counters.first,
            kSeparationStoresPerThread
        );

        first_end_ns = now_ns();
    });

    while (ready.load(std::memory_order_acquire) != 2) {
    }

    const std::uint64_t begin_ns = now_ns();

    start.store(true, std::memory_order_release);

    writer_first.join();
    writer_second.join();

    if (counters.first.load(std::memory_order_relaxed) !=
            kSeparationStoresPerThread ||
        counters.second.load(std::memory_order_relaxed) !=
            kSeparationStoresPerThread) {
        std::cerr << "separation counter final value incorrect\n";
        std::abort();
    }

    const std::uint64_t end_ns =
        first_end_ns > second_end_ns ? first_end_ns : second_end_ns;

    const double elapsed_seconds =
        static_cast<double>(end_ns - begin_ns) / 1'000'000'000.0;

    return SeparationRunResult{
        2 * kSeparationStoresPerThread,
        elapsed_seconds,
        observed,
        combine_qos(first_qos, second_qos)
    };
}

// A4b runner. Unlike run_once this times the producer thread alone and
// treats any rejection as a hard failure rather than a diagnostic: with
// kA4bIterations < kA4bCapacity a rejection is arithmetically impossible,
// so one occurring means the arm is not what it claims to be.
template <typename Queue>
RunResult run_producer_bound_once()
{
    // Heap rather than stack: at kA4bCapacity the queue is about 84 MB,
    // far larger than any default stack.
    auto queue = std::make_unique<Queue>();

    std::atomic<bool> start{false};
    std::atomic<int> ready{0};

    std::uint64_t pushes_completed = 0;
    std::uint64_t pops_completed = 0;

    std::uint64_t producer_begin_ns = 0;
    std::uint64_t producer_end_ns = 0;

    QosResult producer_qos = QosResult::not_attempted;
    QosResult consumer_qos = QosResult::not_attempted;

    Record input{};

    std::thread consumer([&] {
        consumer_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        Record output{};

        while (!start.load(std::memory_order_acquire)) {
        }

        for (std::uint64_t expected = 0;
             expected < kA4bIterations;) {
            if (!queue->try_pop(output)) {
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
    });

    std::thread producer([&] {
        producer_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        // The producer times itself. It cannot be blocked by the
        // consumer, so this interval is kA4bIterations pushes and
        // nothing else.
        producer_begin_ns = now_ns();

        for (std::uint64_t sequence = 0;
             sequence < kA4bIterations;
             ++sequence) {

            input.sequence = sequence;

            if (!queue->try_push(input)) {
                std::cerr
                    << "A4b producer was rejected, which the capacity "
                       "makes impossible\n";
                std::abort();
            }

            ++pushes_completed;
        }

        producer_end_ns = now_ns();
    });

    while (ready.load(std::memory_order_acquire) != 2) {
    }

    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    const double elapsed_seconds =
        static_cast<double>(producer_end_ns - producer_begin_ns) /
        1'000'000'000.0;

    return RunResult{
        pushes_completed,
        pops_completed,
        queue->full_rejections(),
        elapsed_seconds,
        combine_qos(producer_qos, consumer_qos)
    };
}

constexpr std::uint64_t kSlotOpsPerThread = 5'000'000;

template <std::size_t Alignment>
SeparationRunResult run_slot_once()
{
    // Two adjacent slots in a 256-aligned pair, so the geometry is the
    // same on every run. The writer touches only slots[1] and the reader
    // only slots[0], so they never access the same object.
    SlotPair<Alignment> pair{};

    const std::ptrdiff_t observed =
        reinterpret_cast<const std::uint8_t*>(&pair.slots[1]) -
        reinterpret_cast<const std::uint8_t*>(&pair.slots[0]);

    if (observed !=
            static_cast<std::ptrdiff_t>(sizeof(AlignedSlot<Alignment>)) ||
        reinterpret_cast<std::uintptr_t>(&pair.slots[0]) % 256 != 0) {
        std::cerr << "slot layout incorrect\n";
        std::abort();
    }

    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::atomic<std::uint64_t> reader_sink{0};

    std::uint64_t writer_end_ns = 0;
    std::uint64_t reader_end_ns = 0;

    QosResult writer_qos = QosResult::not_attempted;
    QosResult reader_qos = QosResult::not_attempted;

    std::thread reader([&] {
        reader_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        const std::uint64_t sink =
            slot_read_loop(pair.slots[0], kSlotOpsPerThread);

        reader_end_ns = now_ns();

        reader_sink.store(sink, std::memory_order_relaxed);
    });

    std::thread writer([&] {
        writer_qos = request_user_interactive_qos();
        ready.fetch_add(1, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
        }

        slot_write_loop(pair.slots[1], kSlotOpsPerThread);

        writer_end_ns = now_ns();
    });

    while (ready.load(std::memory_order_acquire) != 2) {
    }

    const std::uint64_t begin_ns = now_ns();

    start.store(true, std::memory_order_release);

    writer.join();
    reader.join();

    // The writer's last iteration is observable, so the slot it wrote
    // must hold the final sequence. This catches a sunk or elided store
    // loop; the disassembly check catches a partially elided one.
    if (pair.slots[1].record.sequence != kSlotOpsPerThread - 1) {
        std::cerr << "slot writer final value incorrect\n";
        std::abort();
    }

    const std::uint64_t end_ns =
        writer_end_ns > reader_end_ns ? writer_end_ns : reader_end_ns;

    const double elapsed_seconds =
        static_cast<double>(end_ns - begin_ns) / 1'000'000'000.0;

    return SeparationRunResult{
        2 * kSlotOpsPerThread,
        elapsed_seconds,
        observed,
        combine_qos(writer_qos, reader_qos)
    };
}

// A1a: a strict ping-pong on one atomic counter, with no payload. Each
// thread waits for the other's value, so every handoff pays a full
// cross-core round trip; the three arms differ only in the memory order
// of the load and store.
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

    if (!verify_provenance(provenance.git_commit, provenance.dirty)) {
        return 1;
    }

    // Fixed, so the shuffled order of arms is the same on every run and
    // recorded in the output.
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

    case Experiment::A2b:
        arms = {
            Arm::SharedLine16,
            Arm::FalseSharing64,
            Arm::FalseSharing128,
            Arm::FalseSharing256
        };
        break;

    case Experiment::A3b:
        arms = {
            Arm::SlotStride80,
            Arm::SlotStride128,
            Arm::SlotStride256
        };
        break;

    case Experiment::A4:
        arms = {
            Arm::QueueCachedIndex,
            Arm::QueueUncachedIndex
        };
        break;

    case Experiment::A4b:
        arms = {
            Arm::ProducerBoundCached,
            Arm::ProducerBoundUncached
        };
        break;
    }

    std::vector<TrialResult> results;
    results.reserve(kRounds * arms.size());

    std::cout
        << "git_commit: " << provenance.git_commit << '\n'
        << "git_dirty: " << (provenance.dirty ? "yes" : "no") << '\n'
        << "utc_timestamp: " << utc_timestamp() << '\n'
        << "iterations_per_trial: "
        << (provenance.experiment == Experiment::A2b
                ? 2 * kSeparationStoresPerThread
                : provenance.experiment == Experiment::A3b
                    ? 2 * kSlotOpsPerThread
                    : provenance.experiment == Experiment::A4b
                        ? kA4bIterations
                        : kIterations) << '\n'
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
        << "sizeof_slot_stride_80: "
        << sizeof(AlignedSlot<8>) << '\n'
        << "sizeof_slot_stride_128: "
        << sizeof(AlignedSlot<128>) << '\n'
        << "sizeof_slot_stride_256: "
        << sizeof(AlignedSlot<256>) << '\n'
        << "sizeof_queue_cached_index: "
        << sizeof(CachedIndexQueue) << '\n'
        << "sizeof_queue_uncached_index: "
        << sizeof(UncachedIndexQueue) << '\n'
        << "a4b_capacity: " << kA4bCapacity << '\n'
        << "a4b_iterations: " << kA4bIterations << '\n'
        << "sizeof_a4b_queue: "
        << sizeof(ProducerBoundCachedQueue) << '\n'
        << "shuffle_seed: " << kShuffleSeed << '\n';

    // A P-core bias hint, not pinning. Every trial verifies the class was
    // actually applied and aborts the run otherwise, so a complete results
    // file means every trial in it ran under the hint.
    std::cout
        << "qos_class: user_interactive\n";

    std::cout
        << "round,arm,seconds,completed_operations_per_second,"
           "full_rejections\n";

    for (std::size_t round = 0; round < kRounds; ++round) {
        std::shuffle(arms.begin(), arms.end(), rng);

        for (const Arm arm : arms) {
            TrialResult trial{
                round,
                arm,
                0.0,
                0,
                kIterations
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

            case Arm::ProducerBoundCached: {
                const RunResult result =
                    run_producer_bound_once<
                        ProducerBoundCachedQueue
                    >();

                if (result.pushes_completed != kA4bIterations ||
                    result.pops_completed != kA4bIterations ||
                    result.full_rejections != 0) {
                    std::cerr
                        << "invalid producer_bound_cached run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = kA4bIterations;
                break;
            }

            case Arm::ProducerBoundUncached: {
                const RunResult result =
                    run_producer_bound_once<
                        ProducerBoundUncachedQueue
                    >();

                if (result.pushes_completed != kA4bIterations ||
                    result.pops_completed != kA4bIterations ||
                    result.full_rejections != 0) {
                    std::cerr
                        << "invalid producer_bound_uncached run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = kA4bIterations;
                break;
            }

            case Arm::QueueCachedIndex: {
                const RunResult result =
                    run_once<CachedIndexQueue>();

                if (result.pushes_completed != kIterations ||
                    result.pops_completed != kIterations) {
                    std::cerr
                        << "invalid queue_cached_index run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.full_rejections = result.full_rejections;
                break;
            }

            case Arm::QueueUncachedIndex: {
                const RunResult result =
                    run_once<UncachedIndexQueue>();

                if (result.pushes_completed != kIterations ||
                    result.pops_completed != kIterations) {
                    std::cerr
                        << "invalid queue_uncached_index run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.full_rejections = result.full_rejections;
                break;
            }

            case Arm::SlotStride80: {
                const SeparationRunResult result = run_slot_once<8>();

                if (result.stores_completed != 2 * kSlotOpsPerThread) {
                    std::cerr << "invalid slot_stride_80 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = result.stores_completed;
                break;
            }

            case Arm::SlotStride128: {
                const SeparationRunResult result = run_slot_once<128>();

                if (result.stores_completed != 2 * kSlotOpsPerThread) {
                    std::cerr << "invalid slot_stride_128 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = result.stores_completed;
                break;
            }

            case Arm::SlotStride256: {
                const SeparationRunResult result = run_slot_once<256>();

                if (result.stores_completed != 2 * kSlotOpsPerThread) {
                    std::cerr << "invalid slot_stride_256 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = result.stores_completed;
                break;
            }

            case Arm::SharedLine16: {
                const SeparationRunResult result =
                    run_separation_once<16>();

                if (result.stores_completed !=
                    2 * kSeparationStoresPerThread) {
                    std::cerr << "invalid shared_line_16 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = result.stores_completed;
                break;
            }

            case Arm::FalseSharing64: {
                const SeparationRunResult result =
                    run_separation_once<64>();

                if (result.stores_completed !=
                    2 * kSeparationStoresPerThread) {
                    std::cerr << "invalid false_sharing_64 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = result.stores_completed;
                break;
            }

            case Arm::FalseSharing128: {
                const SeparationRunResult result =
                    run_separation_once<128>();

                if (result.stores_completed !=
                    2 * kSeparationStoresPerThread) {
                    std::cerr << "invalid false_sharing_128 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = result.stores_completed;
                break;
            }

            case Arm::FalseSharing256: {
                const SeparationRunResult result =
                    run_separation_once<256>();

                if (result.stores_completed !=
                    2 * kSeparationStoresPerThread) {
                    std::cerr << "invalid false_sharing_256 run\n";
                    return 1;
                }

                if (!check_qos(arm_name(arm), result.qos)) {
                    return 1;
                }

                trial.seconds = result.seconds;
                trial.operations = result.stores_completed;
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
                static_cast<double>(trial.operations) / trial.seconds;

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