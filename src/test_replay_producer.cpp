// Unit tests for the replay producer (§7.3a, §7.7a, §6.5a).
//
// Why this file exists: before it, replay_producer.hpp was included by
// exactly one translation unit, measure_pacing_floor.cpp, which drives a
// null sink that accepts every push. The rejection path had therefore
// never executed. That path is where §6.5a's entire correctness oracle
// gets its precondition — sequence numbers are assigned *before* the push
// attempt, so an abandoned record still consumes one and leaves a hole in
// the consumer-observed stream. If that were wrong the oracle would be
// vacuous rather than failing, which is the worst way for it to be wrong.
//
// The queue here is a stub with an explicit list of which push attempts
// fail. That is deliberate over a "reject every Nth" rule: it lets each
// test state exactly which records are abandoned, including the awkward
// cases at the two ends of the run and the consecutive-drop case, none of
// which a modulo rule reaches without contortion.
//
// No timing claim is made by anything in this file. StubQueue allocates
// and copies on every accepted push. It is a correctness fixture.

#include "record.hpp"
#include "replay_producer.hpp"
#include "replay_schedule.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>


namespace {

int failures = 0;


void check(const char* test_name, bool condition, const char* what)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << test_name << ": " << what << "\n";
    }
}


void check_u64(
    const char* test_name,
    const char* what,
    std::uint64_t actual,
    std::uint64_t expected
)
{
    if (actual != expected) {
        ++failures;
        std::cerr << "FAIL " << test_name << ": " << what
                  << ": expected " << expected
                  << ", got " << actual << "\n";
    }
}


// A queue satisfying only the part of the interface run_replay uses:
// try_push and full_rejections. Rejections are scripted by index of the
// push attempt, so every test says plainly which records it abandons.
class StubQueue {
public:
    explicit StubQueue(std::vector<bool> reject_attempt)
        : reject_attempt_(std::move(reject_attempt))
    {
        accepted_.reserve(reject_attempt_.size());
    }

    bool try_push(const Record& value)
    {
        const std::size_t attempt = attempts_++;

        if (attempt < reject_attempt_.size() && reject_attempt_[attempt]) {
            ++full_rejections_;
            return false;
        }

        accepted_.push_back(value);
        return true;
    }

    std::uint64_t full_rejections() const noexcept
    {
        return full_rejections_;
    }

    const std::vector<Record>& accepted() const noexcept
    {
        return accepted_;
    }

private:
    std::vector<bool> reject_attempt_;
    std::size_t attempts_{0};
    std::uint64_t full_rejections_{0};
    std::vector<Record> accepted_;
};


// Synthetic capture records with identical timestamps, so every gap is
// zero and build_replay_schedule produces an all-zero offset vector. The
// run then proceeds as fast as the machine allows, which keeps these
// tests sub-millisecond. Pacing is measure_pacing_floor's subject, not
// this file's; one test below uses a real rate to exercise spin_until.
std::vector<CaptureRecord> make_slice(std::size_t count)
{
    std::vector<CaptureRecord> slice(count);

    for (std::size_t i = 0; i < count; ++i) {
        slice[i].capture_wall_time_ns = 1'787'000'000'000'000'000ull;
        slice[i].event_time_ms = 1'787'000'000'000ull + i;
        slice[i].transaction_time_ms = 1'787'000'000'000ull + i;

        // Distinct per record so a payload mix-up is detectable.
        slice[i].bid_price = static_cast<std::int64_t>(100'000 + i);
        slice[i].ask_price = static_cast<std::int64_t>(200'000 + i);
        slice[i].bid_qty = static_cast<std::int64_t>(300'000 + i);
        slice[i].ask_qty = static_cast<std::int64_t>(400'000 + i);
    }

    return slice;
}


// The reconciliation §6.5a asks for, written out in full.
//
// Note what the full form exposes: §6.5a describes summing "consumer-
// observed gap widths", which reaches only the gaps *between* delivered
// records. Records abandoned before the first delivery, or after the
// last, leave no interior gap at all and are invisible to that sum. The
// boundary terms below are what make the identity actually hold, and the
// leading/trailing tests would fail without them.
std::uint64_t reconcile_gaps(
    const std::vector<Record>& delivered,
    std::uint64_t first_sequence,
    std::uint64_t slice_length
)
{
    if (delivered.empty()) {
        return slice_length;
    }

    const std::uint64_t leading =
        delivered.front().sequence - first_sequence;

    const std::uint64_t trailing =
        (first_sequence + slice_length - 1) - delivered.back().sequence;

    std::uint64_t interior = 0;

    for (std::size_t i = 1; i < delivered.size(); ++i) {
        interior +=
            delivered[i].sequence - delivered[i - 1].sequence - 1;
    }

    return leading + interior + trailing;
}


bool sequences_strictly_increasing(const std::vector<Record>& delivered)
{
    for (std::size_t i = 1; i < delivered.size(); ++i) {
        if (delivered[i].sequence <= delivered[i - 1].sequence) {
            return false;
        }
    }

    return true;
}


ReplayStats drive(
    StubQueue& queue,
    const std::vector<CaptureRecord>& slice,
    const ReplaySchedule& schedule,
    std::uint64_t slice_start,
    std::uint64_t first_sequence,
    std::vector<std::uint32_t>& lag_ns
)
{
    prepare_lag_buffer(lag_ns, slice.size());

    return run_replay(
        queue,
        std::span<const CaptureRecord>(slice.data(), slice.size()),
        schedule,
        7,
        slice_start,
        first_sequence,
        lag_ns
    );
}


// Every push accepted. Establishes the baseline the drop tests deviate
// from, and checks the fields the producer owns (§7.3a).
void test_all_accepted()
{
    const char* name = "producer/all_accepted";

    const std::size_t count = 64;

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule schedule = build_replay_schedule(slice);

    StubQueue queue(std::vector<bool>(count, false));
    std::vector<std::uint32_t> lag_ns;

    const ReplayStats stats =
        drive(queue, slice, schedule, 0, 0, lag_ns);

    check_u64(name, "pushed", stats.pushed, count);
    check_u64(name, "dropped_records", stats.dropped_records, 0);
    check_u64(name, "full_rejections", stats.full_rejections, 0);
    check_u64(name, "slice_length", stats.slice_length, count);

    const std::vector<Record>& delivered = queue.accepted();

    check_u64(
        name,
        "delivered count",
        static_cast<std::uint64_t>(delivered.size()),
        count
    );

    bool dense = true;
    bool payload_ok = true;
    bool intended_ok = true;
    bool symbol_ok = true;
    bool reserved_zeroed = true;

    const std::uint8_t zeroes[6] = {0, 0, 0, 0, 0, 0};

    for (std::size_t i = 0; i < delivered.size(); ++i) {
        if (delivered[i].sequence != i) {
            dense = false;
        }

        if (std::memcmp(
                &delivered[i].capture,
                &slice[i],
                sizeof(CaptureRecord)) != 0) {
            payload_ok = false;
        }

        if (delivered[i].replay_intended_send_ns !=
            stats.t0_ns + schedule.intended_offset_ns[i]) {
            intended_ok = false;
        }

        if (delivered[i].symbol_id != 7) {
            symbol_ok = false;
        }

        if (std::memcmp(delivered[i].reserved, zeroes, 6) != 0) {
            reserved_zeroed = false;
        }
    }

    check(name, dense, "sequences were not dense from first_sequence");
    check(name, payload_ok, "capture payload did not match the slice");
    check(name, intended_ok,
          "replay_intended_send_ns was not t0 + schedule offset");
    check(name, symbol_ok, "symbol_id was not stamped by the producer");

    // §7.3: the tail bytes are explicit and zeroed so nothing memcmps or
    // hashes a Record over indeterminate padding.
    check(name, reserved_zeroed, "Record::reserved was not zeroed");
}


// Rejections landing on the last attempt, so the run ends with an
// abandoned record. Interior gaps alone under-count here.
void test_trailing_drop_reconciles()
{
    const char* name = "producer/trailing_drop";

    const std::size_t count = 99;

    std::vector<bool> reject(count, false);
    std::uint64_t expected_drops = 0;

    for (std::size_t i = 2; i < count; i += 3) {
        reject[i] = true;
        ++expected_drops;
    }

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule schedule = build_replay_schedule(slice);

    StubQueue queue(reject);
    std::vector<std::uint32_t> lag_ns;

    const ReplayStats stats =
        drive(queue, slice, schedule, 0, 0, lag_ns);

    check_u64(name, "dropped_records", stats.dropped_records,
              expected_drops);
    check_u64(name, "pushed", stats.pushed, count - expected_drops);
    check_u64(name, "pushed + dropped", stats.pushed +
              stats.dropped_records, count);

    // §7.7a: under drop-newest with no retry, every rejection is a drop.
    // The two counters are separate by design and must agree here and
    // only here — harness A retries, so its rejections are not drops.
    check_u64(name, "full_rejections == dropped_records",
              stats.full_rejections, stats.dropped_records);

    const std::vector<Record>& delivered = queue.accepted();

    check(name, sequences_strictly_increasing(delivered),
          "delivered sequences were not strictly increasing");

    check_u64(
        name,
        "reconciled gap total",
        reconcile_gaps(delivered, stats.first_sequence,
                       stats.slice_length),
        stats.dropped_records
    );

    // The last attempt was rejected, so there is a trailing hole and the
    // interior-only sum is short by exactly one.
    check(
        name,
        delivered.back().sequence != count - 1,
        "expected the final record to have been abandoned"
    );
}


// Leading drops and a run of consecutive drops. The consecutive case is
// the one that shows gap *width* is not always one: §6.5a's "each drop
// produces a gap of width exactly 1" holds per abandoned record, but two
// adjacent abandonments merge into a single gap of width two. The
// invariant is the sum, not the individual widths.
void test_leading_and_consecutive_drops_reconcile()
{
    const char* name = "producer/leading_and_consecutive";

    const std::size_t count = 80;

    std::vector<bool> reject(count, false);
    reject[0] = true;
    reject[1] = true;
    reject[2] = true;
    reject[40] = true;
    reject[41] = true;

    const std::uint64_t expected_drops = 5;

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule schedule = build_replay_schedule(slice);

    StubQueue queue(reject);
    std::vector<std::uint32_t> lag_ns;

    const ReplayStats stats =
        drive(queue, slice, schedule, 0, 0, lag_ns);

    check_u64(name, "dropped_records", stats.dropped_records,
              expected_drops);
    check_u64(name, "full_rejections == dropped_records",
              stats.full_rejections, stats.dropped_records);

    const std::vector<Record>& delivered = queue.accepted();

    check(name, sequences_strictly_increasing(delivered),
          "delivered sequences were not strictly increasing");

    check_u64(
        name,
        "reconciled gap total",
        reconcile_gaps(delivered, stats.first_sequence,
                       stats.slice_length),
        stats.dropped_records
    );

    check_u64(name, "first delivered sequence",
              delivered.front().sequence, 3);

    bool found_width_two = false;

    for (std::size_t i = 1; i < delivered.size(); ++i) {
        if (delivered[i].sequence - delivered[i - 1].sequence - 1 == 2) {
            found_width_two = true;
        }
    }

    check(name, found_width_two,
          "consecutive drops did not produce a gap of width two");
}


void test_all_rejected()
{
    const char* name = "producer/all_rejected";

    const std::size_t count = 32;

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule schedule = build_replay_schedule(slice);

    StubQueue queue(std::vector<bool>(count, true));
    std::vector<std::uint32_t> lag_ns;

    const ReplayStats stats =
        drive(queue, slice, schedule, 0, 0, lag_ns);

    check_u64(name, "pushed", stats.pushed, 0);
    check_u64(name, "dropped_records", stats.dropped_records, count);
    check_u64(name, "full_rejections", stats.full_rejections, count);
    check(name, queue.accepted().empty(), "something was delivered");

    check_u64(
        name,
        "reconciled gap total",
        reconcile_gaps(queue.accepted(), stats.first_sequence,
                       stats.slice_length),
        stats.dropped_records
    );
}


// §7.3a's deciding argument for a producer-owned counter: six hours of
// capture is far short of harness C's stress run, so the file is replayed
// repeatedly. A sequence baked in at parse time would reset to zero at
// every lap — a backwards jump at exactly the boundary where the oracle
// should be looking for real failures.
void test_sequence_continues_across_laps()
{
    const char* name = "producer/lap_continuity";

    const std::size_t count = 40;

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule schedule = build_replay_schedule(slice);

    std::vector<std::uint32_t> lag_ns;

    StubQueue lap1(std::vector<bool>(count, false));
    const ReplayStats stats1 =
        drive(lap1, slice, schedule, 0, 0, lag_ns);

    const std::uint64_t next =
        stats1.first_sequence + stats1.slice_length;

    StubQueue lap2(std::vector<bool>(count, false));
    const ReplayStats stats2 =
        drive(lap2, slice, schedule, 0, next, lag_ns);

    check_u64(name, "lap 2 recorded first_sequence",
              stats2.first_sequence, next);

    check_u64(name, "lap 2 pushed", stats2.pushed, count);
    check_u64(name, "lap 2 dropped_records", stats2.dropped_records, 0);

    check_u64(name, "lap 2 first sequence",
              lap2.accepted().front().sequence, count);

    check_u64(name, "lap 2 last sequence",
              lap2.accepted().back().sequence, 2 * count - 1);

    check(name, sequences_strictly_increasing(lap2.accepted()),
          "lap 2 sequences were not strictly increasing");

    // The join across the lap boundary is the point: no reset, no repeat.
    check(
        name,
        lap2.accepted().front().sequence ==
            lap1.accepted().back().sequence + 1,
        "sequence did not continue across the lap boundary"
    );
}


// §7.3a's traceability mapping, on a slice that starts partway into the
// file and a sequence that does not start at zero.
//
// NOTE, and it is a defect in the plan rather than in the code: §7.3a
// gives the slice form as
//
//     capture_index = slice_start + (sequence % slice_length)
//
// which is only correct when first_sequence is zero or an exact multiple
// of slice_length. On a second lap, or any run whose sequence origin is
// offset, it silently returns the wrong record. The form that holds in
// general is
//
//     capture_index = slice_start + ((sequence - first_sequence) % slice_length)
//
// and ReplayStats already records first_sequence, so the struct
// anticipates a term the documented formula does not use.
void test_capture_index_mapping()
{
    const char* name = "producer/capture_index_mapping";

    const std::size_t file_records = 200;
    const std::size_t slice_start = 60;
    const std::size_t slice_length = 50;
    const std::uint64_t first_sequence = 1'000'003;

    const std::vector<CaptureRecord> file = make_slice(file_records);

    const std::vector<CaptureRecord> slice(
        file.begin() + static_cast<std::ptrdiff_t>(slice_start),
        file.begin() + static_cast<std::ptrdiff_t>(
            slice_start + slice_length)
    );

    const ReplaySchedule schedule = build_replay_schedule(slice);

    std::vector<bool> reject(slice_length, false);
    reject[5] = true;
    reject[6] = true;
    reject[49] = true;

    StubQueue queue(reject);
    std::vector<std::uint32_t> lag_ns;

    const ReplayStats stats = drive(
        queue, slice, schedule,
        slice_start, first_sequence, lag_ns
    );

    check_u64(name, "dropped_records", stats.dropped_records, 3);
    check_u64(name, "slice_start", stats.slice_start, slice_start);
    check_u64(name, "first_sequence", stats.first_sequence,
              first_sequence);

    bool mapping_ok = true;

    for (const Record& record : queue.accepted()) {
        const std::uint64_t index =
            stats.slice_start +
            ((record.sequence - stats.first_sequence) %
             stats.slice_length);

        if (std::memcmp(
                &record.capture,
                &file[index],
                sizeof(CaptureRecord)) != 0) {
            mapping_ok = false;
        }
    }

    check(name, mapping_ok,
          "capture_index mapping did not recover the source record");

    // The form §7.3a actually prints, shown to be wrong on this input.
    // If this ever starts agreeing, the test inputs have drifted into the
    // special case and the check has stopped meaning anything.
    const std::uint64_t documented =
        stats.slice_start +
        (queue.accepted().front().sequence % stats.slice_length);

    const std::uint64_t corrected =
        stats.slice_start +
        ((queue.accepted().front().sequence - stats.first_sequence) %
         stats.slice_length);

    check(
        name,
        documented != corrected,
        "the documented and corrected mappings agreed; inputs no longer "
        "exercise the offset case"
    );
}


// Exercises spin_until and the lag statistics on a real schedule.
// 20,000 records at 5 MHz is a 4 ms run, well inside the ~50M/s pacing
// floor measured by measure_pacing_floor, so the producer should keep up.
void test_paced_run_lag_statistics()
{
    const char* name = "producer/paced_lag";

    const std::size_t count = 20'000;

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule schedule =
        build_fixed_rate_schedule(count, 5'000'000.0);

    StubQueue queue(std::vector<bool>(count, false));
    std::vector<std::uint32_t> lag_ns;

    const ReplayStats stats =
        drive(queue, slice, schedule, 0, 0, lag_ns);

    check_u64(name, "pushed", stats.pushed, count);

    check(name, stats.p50_lag_ns <= stats.p99_lag_ns,
          "p50 lag exceeded p99 lag");

    check(name, stats.p99_lag_ns <= stats.max_lag_ns,
          "p99 lag exceeded max lag");

    check(name, stats.finished_ns > stats.t0_ns,
          "run did not advance the monotonic clock");

    // Intended send times must come from the precomputed schedule, never
    // from the clock at push time (§6.4). If they were computed at push
    // time the producer's own stalls would slide the schedule with them
    // and coordinated omission would be back.
    bool schedule_driven = true;

    for (std::size_t i = 0; i < queue.accepted().size(); ++i) {
        if (queue.accepted()[i].replay_intended_send_ns !=
            stats.t0_ns + schedule.intended_offset_ns[i]) {
            schedule_driven = false;
        }
    }

    check(name, schedule_driven,
          "intended send times did not come from the fixed schedule");
}


// The preconditions abort, so they cannot be checked in-process directly.
// They are checked in a forked child instead.
//
// The first attempt registered three separate ctest entries with
// WILL_FAIL. That does not work and the reason is worth keeping: WILL_FAIL
// inverts a non-zero *exit code*, but a process killed by a signal is
// classified by ctest as "Subprocess aborted" — an exception, and
// exceptions fail regardless of the property. std::abort raises SIGABRT,
// so it lands in the one category WILL_FAIL cannot reach.
//
// Forking is better than a property anyway. The parent captures the
// child's stderr and matches the diagnostic, so this verifies *which*
// precondition fired rather than merely that the process died — a check
// that any abort satisfies would pass if the three guards were swapped.
struct ChildOutcome {
    bool aborted = false;
    std::string diagnostic;
};


ChildOutcome run_in_child(void (*body)())
{
    ChildOutcome outcome;

    int pipe_fds[2];

    if (pipe(pipe_fds) != 0) {
        std::cerr << "pipe() failed\n";
        return outcome;
    }

    const pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "fork() failed\n";
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return outcome;
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);

        body();

        // Only reached if the precondition failed to fire.
        _exit(0);
    }

    close(pipe_fds[1]);

    char buffer[512];
    ssize_t n = 0;

    while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        outcome.diagnostic.append(buffer, static_cast<std::size_t>(n));
    }

    close(pipe_fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    outcome.aborted =
        WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;

    return outcome;
}


void expect_abort(
    const char* what,
    void (*body)(),
    const char* expected_substring
)
{
    const ChildOutcome outcome = run_in_child(body);

    // Both conditions in one check: the guard must fire *and* it must be
    // the right guard. Reported once, with the child's own stderr, so a
    // failure says what actually happened instead of only what did not.
    const bool ok =
        outcome.aborted &&
        outcome.diagnostic.find(expected_substring) != std::string::npos;

    check("producer/preconditions", ok, what);

    if (!ok) {
        std::cerr << "  expected substring: " << expected_substring << "\n";
        std::cerr << "  child aborted: " << (outcome.aborted ? "yes" : "no")
                  << "\n";
        std::cerr << "  child stderr: " << outcome.diagnostic << "\n";
    }
}


void body_empty_slice()
{
    const ReplaySchedule schedule = build_fixed_rate_schedule(0, 1.0);

    StubQueue queue(std::vector<bool>{});
    std::vector<std::uint32_t> lag_ns;

    run_replay(
        queue,
        std::span<const CaptureRecord>(),
        schedule,
        7, 0, 0,
        lag_ns
    );
}


void body_schedule_mismatch()
{
    const std::size_t count = 16;

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule shorter =
        build_fixed_rate_schedule(count - 1, 1'000'000.0);

    StubQueue queue(std::vector<bool>(count, false));
    std::vector<std::uint32_t> lag_ns;
    prepare_lag_buffer(lag_ns, count);

    run_replay(
        queue,
        std::span<const CaptureRecord>(slice.data(), slice.size()),
        shorter,
        7, 0, 0,
        lag_ns
    );
}


void body_short_lag()
{
    const std::size_t count = 16;

    const std::vector<CaptureRecord> slice = make_slice(count);
    const ReplaySchedule schedule = build_replay_schedule(slice);

    StubQueue queue(std::vector<bool>(count, false));

    std::vector<std::uint32_t> short_buffer;
    prepare_lag_buffer(short_buffer, count - 1);

    run_replay(
        queue,
        std::span<const CaptureRecord>(slice.data(), slice.size()),
        schedule,
        7, 0, 0,
        short_buffer
    );
}


// §8.0b: confirming a check is armed rather than trusting that it is.
void test_preconditions_are_armed()
{
    expect_abort(
        "empty slice did not abort with the expected diagnostic",
        body_empty_slice,
        "slice must not be empty"
    );

    expect_abort(
        "schedule mismatch did not abort with the expected diagnostic",
        body_schedule_mismatch,
        "schedule length must equal slice length"
    );

    expect_abort(
        "short lag buffer did not abort with the expected diagnostic",
        body_short_lag,
        "lag buffer must be at least slice length"
    );
}

} // namespace


int main()
{
    test_all_accepted();
    test_trailing_drop_reconciles();
    test_leading_and_consecutive_drops_reconcile();
    test_all_rejected();
    test_sequence_continues_across_laps();
    test_capture_index_mapping();
    test_paced_run_lag_statistics();
    test_preconditions_are_armed();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "test_replay_producer: all checks passed\n";
    return 0;
}