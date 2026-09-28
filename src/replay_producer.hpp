// replay_producer.hpp: run_replay, the loop that plays a capture slice into
// a queue on a fixed schedule, standing in for the exchange.
//
// For each record it waits for the intended send time, stamps the
// run-owned Record fields, calls try_push once, and abandons the record if
// the queue is full. It returns ReplayStats: delivery and drop counts, and
// the producer's lag against its own schedule. End-to-end latency is
// harness B's to measure, not this file's.
//
// Inputs are the queue, a slice of CaptureRecords, a ReplaySchedule of
// intended offsets, the symbol_id to stamp on every Record, the slice's
// position in the file and first sequence number, and a lag buffer the
// caller has prepared with prepare_lag_buffer. Nothing here maps a symbol
// to an id; harness_b passes 0.
//
// Related: replay_schedule.hpp (the schedule), record.hpp (the Record it
// builds), harness_b.cpp (B1), measure_pacing_floor.cpp (this loop's
// ceiling), test_replay_producer.cpp (drop accounting and preconditions).

#pragma once

#include "record.hpp"
#include "replay_schedule.hpp"

#include <time.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>


struct ReplayStats {
    std::uint64_t pushed = 0;

    // The caller's count, not the queue's: a rejection becomes a dropped
    // record only because run_replay abandons it instead of retrying.
    std::uint64_t dropped_records = 0;

    std::uint64_t full_rejections = 0;

    std::uint64_t backwards_steps = 0;

    // Producer lag: the clock when the producer reached record i, minus
    // that record's intended send time. harness_b gates on p99, which must
    // be at most one offered-rate period; max and p50 are reported only.
    std::uint64_t max_lag_ns = 0;
    std::uint64_t p99_lag_ns = 0;
    std::uint64_t p50_lag_ns = 0;

    std::uint64_t t0_ns = 0;
    std::uint64_t finished_ns = 0;

    // With first_sequence below, these map a consumer-observed sequence
    // back to its source record:
    //   index = slice_start + (sequence - first_sequence) % slice_length
    // Without all three the mapping cannot be rebuilt after the run.
    std::uint64_t slice_start = 0;
    std::uint64_t slice_length = 0;

    std::uint64_t first_sequence = 0;
};


// On macOS this is the call calibrate.cpp measured, so its 41.67 ns step
// applies to every send time and lag here. The Linux branch lets the
// tests build and run in the Linux guest. harness_b and
// measure_pacing_floor exit with an error when the QoS class is not
// applied, and off macOS it never is.
inline std::uint64_t replay_now_ns() noexcept
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


// Pacing is a spin on the clock, not a sleep. The period is 10 µs at
// 100k/s and 50 ns at 20M/s, the top of harness_b's sweep; a sleeping
// thread runs again when the scheduler returns it, and that wake-up was
// not measured here. The spin occupies a core that, on the M2's four
// performance cores, competes with the consumer.
//
// The isb is a speculation barrier, not a spin hint like the `yield` in
// MutexQueue's spin: the next iteration's clock read cannot execute ahead
// of the instructions before it. An early read returns a smaller value,
// so without the barrier the loop could only exit late, never early.
// Whether the clock call already contains a barrier of its own has not
// been checked. The isb's cost is inside the ~20 ns per record that
// measure_pacing_floor measures for this loop, and it stays because that
// floor was measured with it; alternatives were not measured. At the top
// of the sweep the loop is close to its limit: with no queue attached its
// p99 lag at 20M/s is 41 ns of a 50 ns period, and in the committed B1
// runs the SPSC arm fails the lag gate at 20M/s on every pass.
inline void spin_until(std::uint64_t deadline_ns) noexcept
{
    while (replay_now_ns() < deadline_ns) {
#if defined(__aarch64__)
        asm volatile("isb" ::: "memory");
#endif
    }
}


namespace replay_detail {

// Preconditions abort with a message. assert() would not do: both CMake
// presets build RelWithDebInfo, which defines NDEBUG, so an assert
// compiles to nothing. A returned status would be loud only if every
// caller checked it. All three preconditions are setup errors in the
// driver, not conditions a sweep could meet and skip, so there is no
// datapoint to discard, only a bug to report.
[[noreturn]] inline void fail(const char* message) noexcept
{
    std::fprintf(stderr, "replay_producer: precondition failed: %s\n", message);
    std::fflush(stderr);
    std::abort();
}


[[noreturn]] inline void fail_size(
    const char* message,
    std::size_t required,
    std::size_t actual
) noexcept
{
    std::fprintf(
        stderr,
        "replay_producer: precondition failed: %s (need %zu, got %zu)\n",
        message,
        required,
        actual
    );
    std::fflush(stderr);
    std::abort();
}


// The element at index floor(fraction * n), clamped to the last: the
// same convention as harness_b's percentile_of.
inline std::uint64_t percentile(
    std::vector<std::uint32_t>& sorted_scratch,
    double fraction
) noexcept
{
    if (sorted_scratch.empty()) {
        return 0;
    }

    const std::size_t index = std::min(
        sorted_scratch.size() - 1,
        static_cast<std::size_t>(
            fraction * static_cast<double>(sorted_scratch.size())
        )
    );

    return sorted_scratch[index];
}

} // namespace replay_detail


// Writes every element of the lag buffer before the clock starts, so no
// page fault lands inside the measured window; run_replay writes one
// element per record. assign already writes each element. The loop writes
// each again, and the empty asm keeps it: without it GCC 13 and clang 18
// on x86-64 delete the loop at -O2, because the fill after it overwrites
// the same elements.
inline void prepare_lag_buffer(
    std::vector<std::uint32_t>& lag_ns,
    std::size_t count
)
{
    lag_ns.assign(count, 0);

    for (std::size_t i = 0; i < count; ++i) {
        lag_ns[i] = 1;
        asm volatile("" :: "r"(&lag_ns[i]) : "memory");
    }

    std::fill(lag_ns.begin(), lag_ns.end(), 0u);
}


// A template on the queue type rather than a virtual interface, because
// harness_b drives the SPSC ring and the mutex baseline through this loop
// two million times per datapoint. An indirect call adds roughly the same
// cost to either push, so it is a larger fraction of the ring's, a short
// run of loads and stores, than of the mutex's, which takes a lock:
// dispatch would narrow the gap between the arms, and the gap is the
// result. Header-only follows, since a template must be visible where it
// is instantiated.
template <typename Queue>
ReplayStats run_replay(
    Queue& queue,
    std::span<const CaptureRecord> slice,
    const ReplaySchedule& schedule,
    std::uint16_t symbol_id,
    std::uint64_t slice_start,
    std::uint64_t first_sequence,
    std::vector<std::uint32_t>& lag_ns
)
{
    if (slice.empty()) {
        replay_detail::fail("slice must not be empty");
    }

    if (schedule.intended_offset_ns.size() != slice.size()) {
        replay_detail::fail_size(
            "schedule length must equal slice length",
            slice.size(),
            schedule.intended_offset_ns.size()
        );
    }

    // The loop writes lag_ns[i] for every record, and prepare_lag_buffer
    // takes its count in a separate call, so a mismatch would be an
    // out-of-bounds write on the hot path.
    if (lag_ns.size() < slice.size()) {
        replay_detail::fail_size(
            "lag buffer must be at least slice length",
            slice.size(),
            lag_ns.size()
        );
    }

    ReplayStats stats;

    stats.backwards_steps = schedule.backwards_steps;
    stats.slice_start = slice_start;
    stats.slice_length = slice.size();
    stats.first_sequence = first_sequence;

    // symbol_id and reserved are the same for every record in a run, so
    // they are set once here; only sequence, replay_intended_send_ns and
    // capture change per iteration. A fresh Record each iteration would
    // zero all 80 bytes and then overwrite 72 of them, inside the measured
    // window.
    Record record{};
    record.symbol_id = symbol_id;

    std::uint64_t sequence = first_sequence;

    const std::uint64_t t0 = replay_now_ns();
    stats.t0_ns = t0;

    for (std::size_t i = 0; i < slice.size(); ++i) {
        const std::uint64_t intended =
            t0 + schedule.intended_offset_ns[i];

        spin_until(intended);

        // Sampled before the push, deliberately. The lag gate asks whether
        // the producer reached each record's slot on time, which is a
        // question about the rate on the x-axis, not about what the queue
        // then cost. Sampled after the push, every lag would include that
        // push's own cost, and the mutex push costs more: harness_b's
        // mutex arm fails the lag gate from 2.5M/s, where the ring passes
        // up to 10M/s. A push that overruns still shows: it delays the
        // next spin, and each record it made late carries that lateness
        // as its own lag.
        const std::uint64_t actual = replay_now_ns();

        // Assigned before the push attempt, so a rejected record still
        // consumes a sequence number and every drop leaves a hole in what
        // the consumer sees. Consecutive drops merge into one wider gap,
        // and drops before the first delivery or after the last leave no
        // gap between delivered records, so the identity that holds is
        // leading + interior + trailing missing sequences ==
        // dropped_records; test_replay_producer checks it. Assign only on
        // success and the consumer sees a dense sequence with the drops
        // invisible.
        record.sequence = sequence++;
        record.replay_intended_send_ns = intended;

        // One member assignment copies all 56 bytes of the capture.
        record.capture = slice[i];

        if (queue.try_push(record)) {
            ++stats.pushed;
        } else {
            // Drop-newest: abandon, never retry. A retry would make the
            // producer wait for the consumer, and dropped_records could
            // never be non-zero, so the zero-drop gate would pass by
            // construction.
            ++stats.dropped_records;
        }

        // Cannot wrap: spin_until returns only once the clock has reached
        // intended, and actual is read after it from the same monotonic
        // clock.
        const std::uint64_t lag = actual - intended;

        // Saturates at ~4.29 s. A stall that long makes nearly every
        // record scheduled inside it more than a period late: over 429,000
        // at 100k/s, harness_b's lowest rate, where 20,000 are enough to
        // fail the p99 gate on its 2,000,000-record slice. Saturation
        // therefore cannot alter a datapoint that would be reported.
        // Saturated samples are not counted.
        lag_ns[i] = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(lag, 0xffffffffull)
        );
    }

    stats.finished_ns = replay_now_ns();

    // Read on the producer thread, the counter's only writer, so it needs
    // no synchronisation.
    stats.full_rejections = queue.full_rejections();

    // Sorted after finished_ns, so none of this is in the measured window.
    std::vector<std::uint32_t> sorted(
        lag_ns.begin(),
        lag_ns.begin() + static_cast<std::ptrdiff_t>(slice.size())
    );

    std::sort(sorted.begin(), sorted.end());

    stats.max_lag_ns = sorted.back();
    stats.p99_lag_ns = replay_detail::percentile(sorted, 0.99);
    stats.p50_lag_ns = replay_detail::percentile(sorted, 0.50);

    return stats;
}
