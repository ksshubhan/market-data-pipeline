#pragma once

#include "record.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>


// The intended-send schedule for a replay run.
//
// §6.4: the schedule is a pure function of record index and is fixed
// before the measurement window opens. If the producer instead computed
// an intended send time at the moment it was about to push, coordinated
// omission would be reintroduced through the back door — the producer
// stalls, the schedule slides with it, and the stall never appears in any
// latency.
//
// This lives apart from the producer because it is pure arithmetic with
// no clock, no queue and no threads, and because the trap in it (see
// build_replay_schedule) is the single thing in Step 11 most likely to be
// silently wrong. Split out, that trap is a three-line unit test; folded
// into the producer it could only be inferred from timing behaviour.
struct ReplaySchedule {
    // Offsets from t0 rather than absolute times, so the schedule stays a
    // pure function of its inputs and is identical across runs.
    std::vector<std::uint64_t> intended_offset_ns;

    // Capture timestamps come from Python's time.time_ns() — wall clock,
    // NTP-disciplined, and therefore able to step backwards mid-capture
    // (§6.1a). Both counters are reported, never silently swallowed.
    std::uint64_t backwards_steps = 0;
    std::uint64_t clamped_ns = 0;

    std::uint64_t span_ns = 0;
};


// Replays the captured inter-arrival gaps, optionally compressed.
//
// §6.1a rule 4, and the reason it is written the way it is:
//
//     gap[n] = (capture[n] >= capture[n-1]) ? capture[n] - capture[n-1] : 0
//     offset[n] = offset[n-1] + gap[n]
//
// Two things about that form are load-bearing.
//
// **The clamp is an explicit comparison, never max(0, b - a).**
// capture_wall_time_ns is uint64_t (§7.3), so the subtraction wraps
// rather than going negative and max(0, ...) is a no-op: the backwards
// step becomes a gap near 2^64 and passes straight through.
//
// What that then does depends on the accumulation, and it is worth being
// precise because the obvious guess is wrong. In the *endpoint* form it
// is the ~585-year forward jump people expect. In the *cumulative* form
// used here it is quieter and worse: adding a gap near 2^64 wraps the
// accumulator back round, so the schedule steps *backwards* by exactly
// the size of the NTP correction. A producer reading that offset believes
// the message is already overdue and sends it immediately — pacing
// destroyed, no visible symptom, no crash. Verified by rewriting the
// clamp the wrong way and watching offset[2] come back as 700 instead of
// 1000.
//
// The bug is invisible on any capture that happens to have no backwards
// steps, which is exactly why it must be written correctly before anyone
// checks.
//
// **The accumulation is cumulative, never an endpoint subtraction.**
// offset[n] = (capture[n] - capture[0]) / C is not equivalent, because it
// silently re-absorbs every gap the clamp removed. The two forms agree
// only when backwards_steps is zero — which is a fact to assert at load
// time, not to assume.
//
// `compression` divides every gap (§6.5 B2's time-compression mechanism).
// 1.0 replays at captured pace. The factor for B2 is still open pending
// the ETHW analysis; the parameter exists now so B2 does not need a
// second schedule builder.
ReplaySchedule build_replay_schedule(
    std::span<const CaptureRecord> slice,
    double compression = 1.0
);


// A fixed offered rate, which is what B1's load sweep uses: the captured
// gaps are ignored entirely and messages are paced at a chosen rate so
// latency can be plotted against offered load (§6.4).
//
// offset[i] is computed from i rather than accumulated, so rounding error
// cannot drift across two million records.
ReplaySchedule build_fixed_rate_schedule(
    std::size_t count,
    double rate_hz
);