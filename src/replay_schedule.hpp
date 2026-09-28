// replay_schedule.hpp: ReplaySchedule, the intended send times a replay
// runs against, and the two functions that build one.
//
// A schedule is one offset from t0 per record, plus counts of the capture
// clock's backwards steps. Building one is pure arithmetic: no clock, no
// queue, no threads. build_fixed_rate_schedule paces records at a chosen
// rate and is what harness_b and measure_pacing_floor use.
// build_replay_schedule replays captured gaps; only the tests call it.
//
// Related: replay_schedule.cpp, replay_producer.hpp (runs a schedule),
// record.hpp (CaptureRecord), test_replay_schedule.cpp,
// tools/inspect_interarrival.py (replicates build_replay_schedule).

#pragma once

#include "record.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>


// Built before the measurement window opens and fixed from then on. If
// the producer instead worked out each send time when it reached the
// record, a stall would slide the schedule along with it and never show
// up as latency: coordinated omission.
struct ReplaySchedule {
    // Offsets from t0 rather than absolute times, so the schedule depends
    // only on its inputs and is identical across runs.
    std::vector<std::uint64_t> intended_offset_ns;

    // Capture timestamps come from Python's time.time_ns(), NTP-disciplined
    // wall time that can step backwards mid-capture. The steps are counted
    // rather than hidden: run_replay copies backwards_steps into
    // ReplayStats, though no C++ code prints either count.
    // tools/inspect_capture.py reports both for a capture file, and both
    // committed captures have none.
    std::uint64_t backwards_steps = 0;
    std::uint64_t clamped_ns = 0;

    std::uint64_t span_ns = 0;
};


// Replays the captured inter-arrival gaps, each divided by `compression`
// and truncated after the clamp:
//
//     gap[n] = (capture[n] >= capture[n-1]) ? capture[n] - capture[n-1] : 0
//     offset[n] = offset[n-1] + gap[n]
//
// The clamp is an explicit comparison, never max(0, b - a). The
// timestamps are uint64_t, so b - a wraps instead of going negative and
// the max does nothing. With that clamp the gaps sum, mod 2^64, to
// capture[n] - capture[0]: the schedule steps back by the size of each
// correction, so the record after one is already overdue when the
// producer reaches it. An offset lands near 2^64, about 585 years ahead,
// only for a record timestamped before the first. The backwards-step case
// in test_replay_schedule.cpp fails on that version.
//
// The offsets accumulate clamped gaps and are never computed as
// (capture[n] - capture[0]) / compression, which would put back every
// gap the clamp removed. The two forms are guaranteed to agree only at
// compression 1.0 with no backwards steps: above 1.0 each gap is
// truncated on its own, so the sum of the quotients can fall short of the
// quotient of the sum.
//
// `compression` must be positive and finite, or the call aborts; the .cpp
// gives the reasoning, which is run_replay's too. 1.0 replays at captured
// pace. B2's analysis fixed its factor at 38,791, and the burst-replay arm
// that would call this with it was not run.
//
// An empty slice is not an error and returns an empty schedule.
ReplaySchedule build_replay_schedule(
    std::span<const CaptureRecord> slice,
    double compression = 1.0
);


// A fixed offered rate, which B1's load sweep uses: the captured gaps are
// ignored and records are paced at rate_hz, so latency can be plotted
// against offered load.
//
// offset[i] is computed from i rather than accumulated, so rounding
// cannot drift across harness_b's two million records.
//
// `rate_hz` must be positive and finite, or the call aborts. The fallback
// this replaced returned `count` offsets of zero, a schedule that sends
// every record at t0, and the caller would have labelled that run with the
// rate it asked for.
//
// `count == 0` is not an error and returns an empty schedule.
ReplaySchedule build_fixed_rate_schedule(
    std::size_t count,
    double rate_hz
);