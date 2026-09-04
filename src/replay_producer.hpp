#pragma once

#include "record.hpp"
#include "replay_schedule.hpp"

#include <time.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>


// The replay producer: reads a capture slice and pushes it into a queue
// at a controlled rate, standing in for the exchange.
//
// It measures nothing about latency — that is harness B's job. Its work
// is to be a believable, controllable source of load, and to record
// enough about its own behaviour that harness B can tell when it failed
// to be one.
//
// A template on the queue type rather than a runtime interface, because
// B1 drives both the SPSC ring and the mutex baseline through this loop.
// A virtual try_push would cost an indirect call two million times per
// datapoint — but worse, it would tax the two arms *unequally*.
// SpscRingBuffer::try_push is about a dozen instructions and inlines
// completely, so a virtual call would be a large fraction of its cost;
// MutexQueue::try_push takes a lock, so the same call is a small fraction
// of its cost. Virtual dispatch would narrow the measured gap between the
// arms, and the gap is the result. §4 requires the baseline be fair, and
// fairness runs both directions.
//
// Header-only follows from that: a template's definition must be visible
// where it is instantiated.


struct ReplayStats {
    std::uint64_t pushed = 0;

    // §7.7a: the *caller's* counter, not the queue's. A rejection becomes
    // a dropped record only because this producer abandons it.
    std::uint64_t dropped_records = 0;

    std::uint64_t full_rejections = 0;

    std::uint64_t backwards_steps = 0;

    // actual_send_ns - intended_send_ns. §6.4's first validity gate.
    std::uint64_t max_lag_ns = 0;
    std::uint64_t p99_lag_ns = 0;
    std::uint64_t p50_lag_ns = 0;

    std::uint64_t t0_ns = 0;
    std::uint64_t finished_ns = 0;

    // §7.3a: without these the mapping from a consumer-observed sequence
    // back to a source record is unreconstructible after the fact.
    std::uint64_t slice_start = 0;
    std::uint64_t slice_length = 0;

    std::uint64_t first_sequence = 0;
};


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


// Pacing has to be a spin on the clock.
//
// At 100k msg/s the period is 10 µs. nanosleep has millisecond
// granularity and unpredictable wake latency, so sleeping cannot hold the
// schedule. Spinning burns a core, and on a four-P-core machine that core
// competes with the consumer. There is no way around it on this hardware
// and it is a stated limitation rather than a hidden one.
//
// Two consequences, both pre-committed: the producer's own spinning is a
// cost §6.4's lag gate will measure, so a datapoint it cannot pace is
// rejected rather than quietly wrong; and there is a floor on achievable
// offered rate, which measure_pacing_floor exists to find before the
// sweep is designed.
inline void spin_until(std::uint64_t deadline_ns) noexcept
{
    while (replay_now_ns() < deadline_ns) {
#if defined(__aarch64__)
        asm volatile("isb" ::: "memory");
#endif
    }
}


namespace replay_detail {

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


// Pre-touch the lag buffer before the clock starts. §6.4b warns about
// demand paging on the consumer's output buffer; the producer's lag
// buffer has the identical problem, and a page fault here lands inside
// the measured window. The write must have an observable effect or it is
// dead code at -O2.
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
    ReplayStats stats;

    stats.backwards_steps = schedule.backwards_steps;
    stats.slice_start = slice_start;
    stats.slice_length = slice.size();
    stats.first_sequence = first_sequence;

    if (slice.empty() ||
        schedule.intended_offset_ns.size() != slice.size()) {
        return stats;
    }

    // §7.3: symbol_id and reserved are invariant for a run, so they are
    // set once on the producer's stack Record. Value-initialising a fresh
    // Record each iteration would zero 80 bytes and then overwrite 72 of
    // them, inside the measured window.
    Record record{};
    record.symbol_id = symbol_id;

    std::uint64_t sequence = first_sequence;

    const std::uint64_t t0 = replay_now_ns();
    stats.t0_ns = t0;

    for (std::size_t i = 0; i < slice.size(); ++i) {
        const std::uint64_t intended =
            t0 + schedule.intended_offset_ns[i];

        spin_until(intended);

        const std::uint64_t actual = replay_now_ns();

        // §7.3a: assigned *before* the push attempt. A rejected record
        // still consumes a sequence number, so each abandoned record
        // leaves a consumer-observed gap of width exactly one and the sum
        // of gap widths reconciles exactly with dropped_records. Assign
        // on successful push instead and the consumer sees a dense
        // sequence, drops become invisible, and §6.5a's oracle is
        // vacuous.
        record.sequence = sequence++;
        record.replay_intended_send_ns = intended;

        // One 56-byte member assignment, not seven field copies (§7.3).
        record.capture = slice[i];

        if (queue.try_push(record)) {
            ++stats.pushed;
        } else {
            // Drop-newest: abandon and move on. Retrying is harness A's
            // policy (§7.7a), and retrying here would make the zero-drop
            // validity gate meaningless.
            ++stats.dropped_records;
        }

        const std::uint64_t lag = actual - intended;

        lag_ns[i] = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(lag, 0xffffffffull)
        );
    }

    stats.finished_ns = replay_now_ns();
    stats.full_rejections = queue.full_rejections();

    // Percentiles computed after the run, never during it (§6.4b).
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