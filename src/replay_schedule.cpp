#include "replay_schedule.hpp"

#include <cmath>


ReplaySchedule build_replay_schedule(
    std::span<const CaptureRecord> slice,
    double compression
)
{
    ReplaySchedule schedule;

    schedule.intended_offset_ns.resize(slice.size());

    if (slice.empty()) {
        return schedule;
    }

    if (!(compression > 0.0)) {
        compression = 1.0;
    }

    schedule.intended_offset_ns[0] = 0;

    std::uint64_t offset = 0;

    for (std::size_t n = 1; n < slice.size(); ++n) {
        const std::uint64_t previous = slice[n - 1].capture_wall_time_ns;
        const std::uint64_t current = slice[n].capture_wall_time_ns;

        std::uint64_t gap = 0;

        // Written as an explicit comparison. max(0, current - previous)
        // would be a no-op here: the operands are unsigned, so a
        // backwards step wraps to a value near 2^64 and the clamp lets it
        // through. See the header for why that matters.
        if (current >= previous) {
            gap = current - previous;
        } else {
            ++schedule.backwards_steps;
            schedule.clamped_ns += previous - current;
        }

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
    ReplaySchedule schedule;

    schedule.intended_offset_ns.resize(count);

    if (count == 0 || !(rate_hz > 0.0)) {
        return schedule;
    }

    const double period_ns = 1'000'000'000.0 / rate_hz;

    for (std::size_t i = 0; i < count; ++i) {
        // From i, not accumulated: at two million records an accumulated
        // rounding error of half a nanosecond per step would drift the
        // tail of the schedule by a millisecond.
        schedule.intended_offset_ns[i] = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(i) * period_ns)
        );
    }

    schedule.span_ns = schedule.intended_offset_ns[count - 1];

    return schedule;
}