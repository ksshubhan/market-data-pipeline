// measure_condvar_wakeup — derives §4's bounded-spin constant.
//
// §4 leaves MutexQueue::kSpinCount at a provisional 200 and says to tune
// it in Step 12 "once B1 shows the distribution of empty-queue intervals
// under load". That method is circular: the tuned baseline is what
// produces B1, so tuning from B1 means running the sweep twice with the
// baseline changing underneath, and the resulting constant depends on
// which offered rates happened to be swept.
//
// This derives it instead.
//
// Spinning on an empty queue is worth doing only while it costs less than
// blocking would. Once the consumer has spun for as long as a park and
// wake would have taken, it should have parked: it is now paying the spin
// *and* will still pay the block. Spin for exactly the cost of blocking
// and the worst case is twice the optimal offline choice — the standard
// ski-rental bound. So the constant is
//
//     kSpinCount = park_wake_cost_ns / spin_iteration_cost_ns
//
// and both terms are measurable. That is the same house style §6.3 used
// to derive the 70 ns calibration threshold from the single-boundary
// model rather than picking a round number.
//
// Three measurements, and the third is the one with a caveat:
//
//   1. Condvar ping-pong. Strict alternation through mutex +
//      condition_variable, so each iteration parks and wakes both
//      threads. Gives round-trip cost including two mutex acquisitions.
//
//   2. Atomic ping-pong. The identical alternation on an atomic, no
//      parking. This is the control: subtracting it removes the
//      cross-core coherence round trip and the loop overhead that both
//      arms pay, leaving the park/wake cost alone. A1a measured this
//      shape at 36.9 ns/handoff, so it doubles as a cross-check — if
//      this arm reports something far from that, the harness is wrong
//      before any conclusion is drawn from arm 1.
//
//   3. Spin-iteration cost. Not a bare `yield`: the real loop body in
//      wait_nonempty() is two relaxed loads, a comparison, a third
//      relaxed load and a yield. Measured twice — uncontended, and with
//      a background thread writing the loaded cache line, because under
//      load the producer is writing tail_ and those loads miss. The
//      contended figure is the realistic one and is what the constant is
//      derived from; the uncontended figure bounds it from below.
//
// Batched timing throughout for measurement 3, per §6.2: a single spin
// iteration is far below the 41.667 ns tick, so it is timed across
// millions of iterations and divided. Measurements 1 and 2 are per-round
// rather than per-iteration for the same reason.

#include "measurement_thread.hpp"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


namespace {

// Matching harness_a: same clock, same call path, per §6.4b's clock
// discipline. clock_gettime_nsec_np(CLOCK_UPTIME_RAW) is what §6.3
// calibrated; mach_absolute_time reads the same counter but not through
// the same code.
std::uint64_t now_ns() noexcept
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    // Non-Apple fallback exists only so the logic can be exercised off
    // the measurement machine, matching replay_producer.hpp. §5's rule
    // stands: all headline numbers come from bare macOS.
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}


constexpr std::size_t kRounds = 20;

// A condvar round trip is expected in the microseconds, so 20,000
// iterations is ~0.1 s per round and 2 s across all rounds.
constexpr std::uint64_t kCondvarIterations = 20'000;

// The atomic arm is ~40 ns per handoff, so it needs more iterations to
// fill a round comparable in duration.
constexpr std::uint64_t kAtomicIterations = 2'000'000;

// Sub-tick per iteration, so batched hard.
constexpr std::uint64_t kSpinIterations = 20'000'000;


// §8.0b: a trial whose QoS did not apply is not the trial being reported.
// Same rule as harness_a — abort rather than silently report an
// unmitigated run as a mitigated one.
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
    std::sort(values.begin(), values.end());

    const std::size_t n = values.size();

    if (n == 0) {
        return 0.0;
    }

    return (n % 2 == 1)
        ? values[n / 2]
        : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}


struct RoundStats {
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
};


RoundStats summarise(std::vector<double> values)
{
    RoundStats stats;

    if (values.empty()) {
        return stats;
    }

    stats.median = median_of(values);
    stats.min = *std::min_element(values.begin(), values.end());
    stats.max = *std::max_element(values.begin(), values.end());

    return stats;
}


// ---------------------------------------------------------------------
// 1. Condvar ping-pong
// ---------------------------------------------------------------------
//
// Strict alternation on `turn`. Each side waits for its turn, flips it,
// notifies, and waits again. Because neither side can proceed until the
// other has acted, both genuinely park on most iterations.
//
// "Most", not "all", and the caveat is stated rather than assumed away:
// if a notify lands before the other thread reaches wait(), that
// iteration re-checks the predicate and returns without parking, which
// makes the measured cost an *under*-estimate of a true park/wake. The
// derived spin count is therefore conservative — it spins for less than
// the true blocking cost, so it parks slightly sooner than the
// ski-rental optimum. That direction is the safe one.
struct CondvarPingPong {
    std::mutex mutex;
    std::condition_variable cv;
    int turn = 0;
    bool done = false;
};


void condvar_side(CondvarPingPong& state, int my_turn, std::uint64_t iterations)
{
    for (std::uint64_t i = 0; i < iterations; ++i) {
        std::unique_lock<std::mutex> lock(state.mutex);

        state.cv.wait(lock, [&] {
            return state.turn == my_turn || state.done;
        });

        if (state.done) {
            return;
        }

        state.turn = 1 - my_turn;

        lock.unlock();
        state.cv.notify_one();
    }
}


double measure_condvar_round(std::uint64_t iterations)
{
    CondvarPingPong state;

    std::thread other([&] {
        require_qos("condvar responder", request_user_interactive_qos());
        condvar_side(state, 1, iterations);
    });

    const std::uint64_t begin = now_ns();

    condvar_side(state, 0, iterations);

    const std::uint64_t end = now_ns();

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.done = true;
    }

    state.cv.notify_all();
    other.join();

    // Each iteration of the initiating side is one full round trip: this
    // thread wakes the other, the other wakes this one back.
    return static_cast<double>(end - begin) /
           static_cast<double>(iterations);
}


// ---------------------------------------------------------------------
// 2. Atomic ping-pong — the control
// ---------------------------------------------------------------------
//
// The same alternation with no blocking primitive, so the difference
// against arm 1 is the cost of parking and waking rather than the cost of
// getting a value from one core to another. Acquire/release because
// relaxed here would be a different experiment; the payload is the turn
// variable itself so there is no separate non-atomic object and no data
// race either way, but matching the queue's ordering keeps the control
// comparable to A1a.
void atomic_side(
    std::atomic<int>& turn,
    int my_turn,
    std::uint64_t iterations
) noexcept
{
    for (std::uint64_t i = 0; i < iterations; ++i) {
        while (turn.load(std::memory_order_acquire) != my_turn) {
            // All three branches, matching MutexQueue::cpu_relax. The
            // aarch64-only version livelocks on a single-core host,
            // which is not the measurement machine but is a real bug.
#if defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
            asm volatile("pause" ::: "memory");
#else
            asm volatile("" ::: "memory");
#endif
        }

        turn.store(1 - my_turn, std::memory_order_release);
    }
}


double measure_atomic_round(std::uint64_t iterations)
{
    std::atomic<int> turn{0};

    std::thread other([&] {
        require_qos("atomic responder", request_user_interactive_qos());
        atomic_side(turn, 1, iterations);
    });

    const std::uint64_t begin = now_ns();

    atomic_side(turn, 0, iterations);

    const std::uint64_t end = now_ns();

    other.join();

    return static_cast<double>(end - begin) /
           static_cast<double>(iterations);
}


// ---------------------------------------------------------------------
// 3. Spin-iteration cost
// ---------------------------------------------------------------------
//
// Replicates wait_nonempty()'s loop body exactly: size_hint() is two
// relaxed loads and a subtraction, closed_hint() is a third, then
// cpu_relax(). Measuring a bare `yield` instead would understate the
// iteration and therefore overstate how many iterations fit in the
// blocking budget.
//
// The accumulator is sunk through an asm barrier so the loop survives
// -O2. §6.4b's warning about dead pre-touch loops applies identically
// here: a spin loop whose result is discarded is deleted, and the
// measurement would then report the cost of an empty loop.
struct SpinTargets {
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
    std::atomic<bool> closed{false};
};


double measure_spin_round(
    SpinTargets& targets,
    std::uint64_t iterations
) noexcept
{
    std::uint64_t accumulator = 0;

    const std::uint64_t begin = now_ns();

    for (std::uint64_t i = 0; i < iterations; ++i) {
        const std::uint64_t size =
            targets.tail.load(std::memory_order_relaxed) -
            targets.head.load(std::memory_order_relaxed);

        const bool closed = targets.closed.load(std::memory_order_relaxed);

        accumulator += size + (closed ? 1u : 0u);

#if defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
        asm volatile("pause" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

    const std::uint64_t end = now_ns();

    asm volatile("" :: "r"(accumulator) : "memory");

    return static_cast<double>(end - begin) /
           static_cast<double>(iterations);
}


// ---------------------------------------------------------------------
// Provenance — same convention as harness_a and convert_capture (§7.6):
// supplied by the caller, never queried at runtime, so the results
// describe the build that produced them.
// ---------------------------------------------------------------------
struct Provenance {
    std::string git_commit;
    bool dirty = false;
};


bool parse_provenance(int argc, char* argv[], Provenance& out)
{
    if (argc != 3) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0] << " <git-commit-40-hex> <dirty:0|1>\n";
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

    return true;
}

} // namespace


int main(int argc, char* argv[])
{
    Provenance provenance;

    if (!parse_provenance(argc, argv, provenance)) {
        return 2;
    }

    require_qos("main", request_user_interactive_qos());

    std::vector<double> condvar_rounds;
    std::vector<double> atomic_rounds;
    std::vector<double> spin_quiet_rounds;
    std::vector<double> spin_contended_rounds;

    condvar_rounds.reserve(kRounds);
    atomic_rounds.reserve(kRounds);
    spin_quiet_rounds.reserve(kRounds);
    spin_contended_rounds.reserve(kRounds);

    // §5: interleave the arms rather than running each to completion, so
    // thermal drift on a fanless M2 spreads across conditions instead of
    // loading onto whichever ran last.
    for (std::size_t round = 0; round < kRounds; ++round) {
        condvar_rounds.push_back(measure_condvar_round(kCondvarIterations));
        atomic_rounds.push_back(measure_atomic_round(kAtomicIterations));

        {
            SpinTargets quiet;
            spin_quiet_rounds.push_back(
                measure_spin_round(quiet, kSpinIterations)
            );
        }

        {
            SpinTargets contended;
            std::atomic<bool> stop{false};

            // A background writer on the same line the spin loop reads,
            // standing in for the producer advancing tail_ under load.
            std::thread writer([&] {
                std::uint64_t n = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    contended.tail.store(++n, std::memory_order_relaxed);
                }
            });

            spin_contended_rounds.push_back(
                measure_spin_round(contended, kSpinIterations)
            );

            stop.store(true, std::memory_order_relaxed);
            writer.join();
        }
    }

    const RoundStats condvar = summarise(condvar_rounds);
    const RoundStats atomic_arm = summarise(atomic_rounds);
    const RoundStats spin_quiet = summarise(spin_quiet_rounds);
    const RoundStats spin_contended = summarise(spin_contended_rounds);

    // Round trip minus the coherence round trip the control also pays,
    // halved because one round trip is two park/wake events.
    const double park_wake_ns =
        (condvar.median - atomic_arm.median) / 2.0;

    const double derived_contended =
        spin_contended.median > 0.0
            ? park_wake_ns / spin_contended.median
            : 0.0;

    const double derived_quiet =
        spin_quiet.median > 0.0
            ? park_wake_ns / spin_quiet.median
            : 0.0;

    std::cout
        << "experiment: condvar_wakeup\n"
        << "git_commit: " << provenance.git_commit << '\n'
        << "git_dirty: " << (provenance.dirty ? "yes" : "no") << '\n'
        << "rounds: " << kRounds << '\n'
        << "condvar_iterations_per_round: " << kCondvarIterations << '\n'
        << "atomic_iterations_per_round: " << kAtomicIterations << '\n'
        << "spin_iterations_per_round: " << kSpinIterations << '\n'
        << "qos_class: user_interactive\n"
        << '\n'
        << "condvar_round_trip_ns_median: " << condvar.median << '\n'
        << "condvar_round_trip_ns_min: " << condvar.min << '\n'
        << "condvar_round_trip_ns_max: " << condvar.max << '\n'
        << "atomic_round_trip_ns_median: " << atomic_arm.median << '\n'
        << "atomic_round_trip_ns_min: " << atomic_arm.min << '\n'
        << "atomic_round_trip_ns_max: " << atomic_arm.max << '\n'
        << "spin_iteration_ns_quiet_median: " << spin_quiet.median << '\n'
        << "spin_iteration_ns_contended_median: "
        << spin_contended.median << '\n'
        << '\n'
        << "park_wake_ns: " << park_wake_ns << '\n'
        << "derived_spin_count_contended: " << derived_contended << '\n'
        << "derived_spin_count_quiet: " << derived_quiet << '\n'
        << '\n'
        << "# The contended figure is the one to adopt: under load the\n"
        << "# producer is writing tail_, so wait_nonempty's loads miss.\n"
        << "# A1a measured an atomic ping-pong at 36.9 ns/handoff; if\n"
        << "# atomic_round_trip_ns_median is far from twice that, this\n"
        << "# harness is wrong and arm 1 should not be trusted either.\n";

    return 0;
}
