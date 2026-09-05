#include "replay_schedule.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>


namespace {

// Same mechanism as replay_producer.hpp's preconditions, and
// deliberately the same shape so the two read alike.
//
// assert() is not available: every configured build defines NDEBUG, and
// §8.0b records what that cost the first time — an entire test file
// compiling to nothing and exiting 0 for three days. Anything that must
// hold at -O2 aborts on its own terms.
//
// A status flag on ReplaySchedule was considered and rejected for the
// reason the producer already gives: a flag is only loud if every caller
// reads it, which is the exact property that failed for assert.
//
// Aborting from a function that is otherwise pure arithmetic — no clock,
// no I/O, no threads — is a real cost and worth naming rather than
// glossing. It is paid because there is nothing to recover to. A
// non-positive rate or compression factor is a typo in a harness driver,
// not a runtime condition a sweep could legitimately meet and skip, and
// what was returned in its place was not obviously broken: it is a
// schedule the caller runs and then labels with the rate it asked for
// rather than the one it got.
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


// The property that must hold, written so that its negation catches NaN
// and both infinities as well as zero and negatives.
//
// Testing `x <= 0.0` would let NaN through, because every comparison
// against NaN is false. Testing `x > 0.0` alone would let +infinity
// through, and an infinite divisor collapses every gap to zero — the
// same all-at-t0 schedule a zero rate produces, reached from the other
// direction.
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
    // Checked before anything else, including the empty-slice case. The
    // precondition is a statement about the call, not about whether the
    // call happened to have any work to do.
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
    if (!is_positive_finite(rate_hz)) {
        fail("rate_hz must be positive and finite", rate_hz);
    }

    ReplaySchedule schedule;

    schedule.intended_offset_ns.resize(count);

    // count == 0 is legal and unambiguous — no records, no offsets — and
    // it is checked separately from the rate on purpose. The previous
    // form welded the two into one condition, so a caller error and a
    // legal degenerate case returned the same object.
    if (count == 0) {
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