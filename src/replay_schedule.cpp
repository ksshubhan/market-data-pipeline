// replay_schedule.cpp: builds the intended-send schedule a replay runs
// against, either from captured inter-arrival gaps or at a fixed rate.
//
// Pure arithmetic: no clock, no I/O, no threads. Each function returns a
// ReplaySchedule of offsets from t0, one per record, plus counts of the
// capture clock's backwards steps. replay_schedule.hpp carries the
// reasoning behind both forms.
//
// Related: replay_schedule.hpp, replay_producer.hpp (runs the schedule),
// test_replay_schedule.cpp, tools/inspect_interarrival.py (replicates
// build_replay_schedule to predict B2's compressed schedule).

#include "replay_schedule.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>


namespace {

// Preconditions abort with a message, the same policy as
// replay_producer.hpp's. assert() would not do: both CMake presets build
// RelWithDebInfo, which defines NDEBUG. A status flag on ReplaySchedule
// would be loud only if every caller read it. A bad rate or compression
// is a mistake in the driver, and the silent fallbacks this replaced
// returned a schedule the caller would run and then label with the rate
// or compression it asked for, not the one it got. The cost is that the
// tests must run each case in a forked child to observe the abort.
[[noreturn]] void fail(const char* message, double value) noexcept
{
    std::fprintf(
        stderr,
        "replay_schedule: precondition failed: %s (got %g)\n",
        message,
        value
    );

    std::fflush(stderr);
    std::abort();
}


// Written so that its negation rejects NaN and both infinities as well
// as zero and negatives. `x <= 0.0` would let NaN through, since every
// comparison with NaN is false; `x > 0.0` alone would let +infinity
// through, and dividing by it makes every gap zero, a schedule that
// sends every record at t0.
bool is_positive_finite(double value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

} // namespace


ReplaySchedule build_replay_schedule(
    std::span<const CaptureRecord> slice,
    double compression
)
{
    // Checked before the empty-slice case: the precondition is about the
    // call, not about whether the call has any work to do.
    if (!is_positive_finite(compression)) {
        fail("compression must be positive and finite", compression);
    }

    ReplaySchedule schedule;

    schedule.intended_offset_ns.resize(slice.size());

    if (slice.empty()) {
        return schedule;
    }

    schedule.intended_offset_ns[0] = 0;

    std::uint64_t offset = 0;

    for (std::size_t n = 1; n < slice.size(); ++n) {
        const std::uint64_t previous = slice[n - 1].capture_wall_time_ns;
        const std::uint64_t current = slice[n].capture_wall_time_ns;

        std::uint64_t gap = 0;

        // An explicit comparison, not max(0, current - previous): the
        // operands are unsigned, so a backwards step would wrap to near
        // 2^64 and pass straight through. replay_schedule.hpp gives what
        // that does to the schedule. clamped_ns is in capture
        // nanoseconds, before compression.
        if (current >= previous) {
            gap = current - previous;
        } else {
            ++schedule.backwards_steps;
            schedule.clamped_ns += previous - current;
        }

        // Divided and truncated per gap, before it is added, so any gap
        // shorter than `compression` nanoseconds becomes exactly 0 and
        // its record shares the previous record's offset.
        // tools/inspect_interarrival.py reproduces this truncation.
        if (compression != 1.0) {
            gap = static_cast<std::uint64_t>(
                static_cast<double>(gap) / compression
            );
        }

        // Cumulative. An endpoint subtraction would re-absorb every gap
        // the clamp above removed.
        offset += gap;

        schedule.intended_offset_ns[n] = offset;
    }

    schedule.span_ns = offset;

    return schedule;
}


ReplaySchedule build_fixed_rate_schedule(
    std::size_t count,
    double rate_hz
)
{
    if (!is_positive_finite(rate_hz)) {
        fail("rate_hz must be positive and finite", rate_hz);
    }

    ReplaySchedule schedule;

    schedule.intended_offset_ns.resize(count);

    // count == 0 is legal and returns an empty schedule, but only after
    // the rate is checked, so a bad rate aborts even with no records.
    if (count == 0) {
        return schedule;
    }

    const double period_ns = 1'000'000'000.0 / rate_hz;

    for (std::size_t i = 0; i < count; ++i) {
        // Computed from i, not accumulated. Adding a rounded period each
        // step would drift by up to half a nanosecond per record, a
        // millisecond over two million. Every rate harness_b and
        // measure_pacing_floor use has a whole-nanosecond period, so for
        // them the two forms agree.
        schedule.intended_offset_ns[i] = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(i) * period_ns)
        );
    }

    schedule.span_ns = schedule.intended_offset_ns[count - 1];

    return schedule;
}