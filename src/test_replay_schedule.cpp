#include "replay_schedule.hpp"
#include "test_child_process.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>


// Pure arithmetic, no threads and no clock. That is the point of having
// split the schedule out of the producer: the trap these tests exist for
// could otherwise only be inferred from timing behaviour.
//
// No assert() anywhere — every configured build defines NDEBUG.

namespace {

std::uint64_t g_failures = 0;

void check(const char* test_name, bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL [" << test_name << "] " << what << '\n';
        ++g_failures;
    }
}

void check_u64(
    const char* test_name,
    const char* what,
    std::uint64_t observed,
    std::uint64_t expected
)
{
    if (observed != expected) {
        std::cerr
            << "FAIL [" << test_name << "] " << what
            << ": expected " << expected
            << ", got " << observed
            << '\n';

        ++g_failures;
    }
}


std::vector<CaptureRecord> from_timestamps(
    const std::vector<std::uint64_t>& timestamps
)
{
    std::vector<CaptureRecord> records(timestamps.size());

    for (std::size_t i = 0; i < timestamps.size(); ++i) {
        records[i].capture_wall_time_ns = timestamps[i];
    }

    return records;
}


void test_monotonic_matches_endpoint_form()
{
    const char* name = "schedule/monotonic";

    const std::vector<CaptureRecord> records =
        from_timestamps({1000, 1500, 3000, 3001, 9000});

    const ReplaySchedule schedule = build_replay_schedule(records);

    check_u64(name, "offset[0]", schedule.intended_offset_ns[0], 0);
    check_u64(name, "offset[1]", schedule.intended_offset_ns[1], 500);
    check_u64(name, "offset[2]", schedule.intended_offset_ns[2], 2000);
    check_u64(name, "offset[3]", schedule.intended_offset_ns[3], 2001);
    check_u64(name, "offset[4]", schedule.intended_offset_ns[4], 8000);

    check_u64(name, "backwards_steps", schedule.backwards_steps, 0);
    check_u64(name, "clamped_ns", schedule.clamped_ns, 0);
    check_u64(name, "span_ns", schedule.span_ns, 8000);

    // With no backwards steps the cumulative and endpoint forms agree,
    // which is the fact §6.1a says to assert at load time rather than
    // assume.
    const std::uint64_t endpoint_span =
        records.back().capture_wall_time_ns -
        records.front().capture_wall_time_ns;

    check_u64(name, "endpoint equivalence", schedule.span_ns, endpoint_span);
}


// The test this file exists for.
//
// A backwards step is clamped to zero and counted. Rewriting the clamp as
// max(0, current - previous) makes this test fail loudly: the operands
// are unsigned, so the subtraction wraps and offset[2] becomes a value
// near 2^64 instead of 1000.
void test_backwards_step_is_clamped_not_wrapped()
{
    const char* name = "schedule/backwards_step";

    // The third timestamp steps back by 300 ns, as an NTP correction
    // mid-capture would.
    const std::vector<CaptureRecord> records =
        from_timestamps({10'000, 11'000, 10'700, 12'000});

    const ReplaySchedule schedule = build_replay_schedule(records);

    check_u64(name, "offset[0]", schedule.intended_offset_ns[0], 0);
    check_u64(name, "offset[1]", schedule.intended_offset_ns[1], 1000);

    // Clamped to zero, so the offset does not advance across the step.
    check_u64(name, "offset[2]", schedule.intended_offset_ns[2], 1000);

    // And then continues normally: 12000 - 10700 = 1300.
    check_u64(name, "offset[3]", schedule.intended_offset_ns[3], 2300);

    check_u64(name, "backwards_steps", schedule.backwards_steps, 1);
    check_u64(name, "clamped_ns", schedule.clamped_ns, 300);

    // The schedule must never go backwards. This is the check that
    // names the real failure mode of max(0, current - previous): the
    // wrapped gap is near 2^64, and adding it wraps the accumulator back
    // round, so the schedule silently steps *backwards* rather than
    // jumping forwards. A producer reading that offset believes the
    // message is already overdue and sends it immediately, which destroys
    // pacing with no visible symptom. (The forward jump people expect
    // happens in the endpoint form, not the cumulative one.)
    bool monotonic = true;

    for (std::size_t i = 1; i < schedule.intended_offset_ns.size(); ++i) {
        if (schedule.intended_offset_ns[i] <
            schedule.intended_offset_ns[i - 1]) {
            monotonic = false;
        }
    }

    check(name, monotonic, "schedule went backwards across a clamped step");

    // The endpoint form gives 12000 - 10000 = 2000, the cumulative form
    // gives 2300. They are not equivalent once a step has been clamped,
    // which is why §6.1a forbids the endpoint form.
    const std::uint64_t endpoint_span =
        records.back().capture_wall_time_ns -
        records.front().capture_wall_time_ns;

    check(
        name,
        schedule.span_ns != endpoint_span,
        "cumulative and endpoint forms agreed despite a clamped step"
    );
}


// A backwards step from a very large timestamp. If the subtraction were
// performed before the comparison, the intermediate would wrap and the
// resulting offset would be astronomically large.
void test_backwards_step_near_uint64_max()
{
    const char* name = "schedule/backwards_near_max";

    const std::uint64_t big = std::numeric_limits<std::uint64_t>::max() - 5;

    const std::vector<CaptureRecord> records =
        from_timestamps({big - 1000, big, big - 500, big + 1});

    const ReplaySchedule schedule = build_replay_schedule(records);

    check_u64(name, "offset[1]", schedule.intended_offset_ns[1], 1000);
    check_u64(name, "offset[2]", schedule.intended_offset_ns[2], 1000);
    check_u64(name, "offset[3]", schedule.intended_offset_ns[3], 1501);

    check_u64(name, "backwards_steps", schedule.backwards_steps, 1);
    check_u64(name, "clamped_ns", schedule.clamped_ns, 500);
}


void test_repeated_timestamps_are_not_backwards()
{
    const char* name = "schedule/equal_timestamps";

    // Equal timestamps are a zero gap, not a backwards step. The capture
    // clock has finite resolution, so runs of equal values are expected.
    const std::vector<CaptureRecord> records =
        from_timestamps({500, 500, 500, 700});

    const ReplaySchedule schedule = build_replay_schedule(records);

    check_u64(name, "backwards_steps", schedule.backwards_steps, 0);
    check_u64(name, "offset[2]", schedule.intended_offset_ns[2], 0);
    check_u64(name, "offset[3]", schedule.intended_offset_ns[3], 200);
}


void test_compression_scales_and_preserves_order()
{
    const char* name = "schedule/compression";

    const std::vector<CaptureRecord> records =
        from_timestamps({0, 1000, 3000, 3400});

    const ReplaySchedule schedule = build_replay_schedule(records, 10.0);

    check_u64(name, "offset[1]", schedule.intended_offset_ns[1], 100);
    check_u64(name, "offset[2]", schedule.intended_offset_ns[2], 300);
    check_u64(name, "offset[3]", schedule.intended_offset_ns[3], 340);

    bool monotonic = true;

    for (std::size_t i = 1; i < schedule.intended_offset_ns.size(); ++i) {
        if (schedule.intended_offset_ns[i] <
            schedule.intended_offset_ns[i - 1]) {
            monotonic = false;
        }
    }

    check(name, monotonic, "compression broke monotonicity");
}


void test_fixed_rate_has_no_drift()
{
    const char* name = "schedule/fixed_rate";

    constexpr std::size_t kCount = 2'000'000;
    constexpr double kRate = 100'000.0;

    const ReplaySchedule schedule =
        build_fixed_rate_schedule(kCount, kRate);

    check_u64(name, "offset[0]", schedule.intended_offset_ns[0], 0);
    check_u64(name, "offset[1]", schedule.intended_offset_ns[1], 10'000);
    check_u64(name, "offset[100]", schedule.intended_offset_ns[100],
        1'000'000);

    // The slice length fixed in §6.4b at the rate B1 sweeps around: two
    // million records at 100k/s is exactly twenty seconds. Accumulating
    // the period instead of computing from i would drift here.
    check_u64(
        name,
        "final offset",
        schedule.intended_offset_ns[kCount - 1],
        19'999'990'000ull
    );

    check_u64(name, "backwards_steps", schedule.backwards_steps, 0);

    // A rate whose period is not a whole number of nanoseconds.
    const ReplaySchedule awkward =
        build_fixed_rate_schedule(1'000'001, 333'333.0);

    bool monotonic = true;

    for (std::size_t i = 1; i < awkward.intended_offset_ns.size(); ++i) {
        if (awkward.intended_offset_ns[i] <
            awkward.intended_offset_ns[i - 1]) {
            monotonic = false;
        }
    }

    check(name, monotonic, "awkward rate broke monotonicity");

    // 1,000,000 periods of 3000.003 ns is 3,000,003,000 ns to the nearest
    // nanosecond. An accumulated schedule would be off by microseconds.
    check_u64(
        name,
        "awkward final offset",
        awkward.intended_offset_ns[1'000'000],
        3'000'003'000ull
    );
}


void test_degenerate_inputs()
{
    const char* name = "schedule/degenerate";

    const std::vector<CaptureRecord> empty;
    const ReplaySchedule empty_schedule = build_replay_schedule(empty);

    check(name, empty_schedule.intended_offset_ns.empty(),
        "empty slice produced offsets");
    check_u64(name, "empty span", empty_schedule.span_ns, 0);

    const std::vector<CaptureRecord> single = from_timestamps({12345});
    const ReplaySchedule single_schedule = build_replay_schedule(single);

    check_u64(name, "single size",
        single_schedule.intended_offset_ns.size(), 1);
    check_u64(name, "single offset",
        single_schedule.intended_offset_ns[0], 0);
    check_u64(name, "single span", single_schedule.span_ns, 0);

    const ReplaySchedule zero_count = build_fixed_rate_schedule(0, 1000.0);

    check(name, zero_count.intended_offset_ns.empty(),
        "zero count produced offsets");

    // A zero count is legal and must stay legal. It used to share a
    // condition with the rate check, and separating them is half of what
    // the precondition work below is for.
    check_u64(name, "zero count span", zero_count.span_ns, 0);

    // Compression of exactly 1.0 is the documented no-op and must not be
    // caught by the positive-and-finite guard.
    const std::vector<CaptureRecord> pair = from_timestamps({1000, 4000});
    const ReplaySchedule unit = build_replay_schedule(pair, 1.0);

    check_u64(name, "unit compression gap",
        unit.intended_offset_ns[1], 3000);
}


// The preconditions abort, so each body runs in a forked child and the
// parent matches the diagnostic. Matching the text rather than merely
// observing a death is what makes this verify *which* guard fired: a
// check satisfied by any abort would pass with the two guards swapped.
void expect_abort(
    const char* what,
    void (*body)(),
    const char* expected_substring
)
{
    const ChildOutcome outcome = run_in_child(body);

    const bool ok =
        outcome.aborted &&
        outcome.diagnostic.find(expected_substring) != std::string::npos;

    check("schedule/preconditions", ok, what);

    if (!ok) {
        std::cerr << "  expected substring: " << expected_substring << "\n";
        std::cerr << "  child aborted: " << (outcome.aborted ? "yes" : "no")
                  << "\n";
        std::cerr << "  child stderr: " << outcome.diagnostic << "\n";
    }
}


void body_zero_rate()
{
    build_fixed_rate_schedule(10, 0.0);
}


void body_negative_rate()
{
    build_fixed_rate_schedule(10, -1000.0);
}


// Infinity divides a period to zero, producing the same all-at-t0
// schedule a zero rate does. `rate_hz > 0.0` alone would let it through.
void body_infinite_rate()
{
    build_fixed_rate_schedule(
        10,
        std::numeric_limits<double>::infinity()
    );
}


// Every comparison against NaN is false, so a guard written as
// `rate_hz <= 0.0` would pass NaN straight into llround.
void body_nan_rate()
{
    build_fixed_rate_schedule(
        10,
        std::numeric_limits<double>::quiet_NaN()
    );
}


void body_zero_compression()
{
    const std::vector<CaptureRecord> records =
        from_timestamps({1000, 2000, 3000});

    build_replay_schedule(records, 0.0);
}


void body_negative_compression()
{
    const std::vector<CaptureRecord> records =
        from_timestamps({1000, 2000, 3000});

    build_replay_schedule(records, -38791.0);
}


void body_nan_compression()
{
    const std::vector<CaptureRecord> records =
        from_timestamps({1000, 2000, 3000});

    build_replay_schedule(
        records,
        std::numeric_limits<double>::quiet_NaN()
    );
}


// The argument is wrong whether or not there is work to do, so the guard
// fires ahead of the empty-slice early return rather than behind it.
void body_bad_compression_empty_slice()
{
    const std::vector<CaptureRecord> empty;

    build_replay_schedule(empty, 0.0);
}


void test_preconditions_abort()
{
    expect_abort("zero rate", body_zero_rate, "rate_hz must be positive");
    expect_abort("negative rate", body_negative_rate,
        "rate_hz must be positive");
    expect_abort("infinite rate", body_infinite_rate,
        "rate_hz must be positive");
    expect_abort("NaN rate", body_nan_rate, "rate_hz must be positive");

    expect_abort("zero compression", body_zero_compression,
        "compression must be positive");
    expect_abort("negative compression", body_negative_compression,
        "compression must be positive");
    expect_abort("NaN compression", body_nan_compression,
        "compression must be positive");
    expect_abort("bad compression on an empty slice",
        body_bad_compression_empty_slice,
        "compression must be positive");
}

} // namespace


int main()
{
    test_monotonic_matches_endpoint_form();
    test_backwards_step_is_clamped_not_wrapped();
    test_backwards_step_near_uint64_max();
    test_repeated_timestamps_are_not_backwards();
    test_compression_scales_and_preserves_order();
    test_fixed_rate_has_no_drift();
    test_degenerate_inputs();
    test_preconditions_abort();

    if (g_failures != 0) {
        std::cerr
            << "replay schedule tests FAILED: "
            << g_failures
            << " check(s) failed\n";

        return 1;
    }

    std::cout << "replay schedule tests passed\n";

    return 0;
}