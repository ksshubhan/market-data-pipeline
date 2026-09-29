// test_replay_producer.cpp: tests for run_replay, the loop that plays a
// capture slice into a queue on a fixed schedule.
//
// The queue is StubQueue, which rejects exactly the push attempts a test
// lists. The cases cover every push accepted, every push rejected, drops
// ending the run, drops starting it and two in a row, a second run whose
// sequence continues from the first, the capture_index mapping on a slice
// with a non-zero sequence origin, and a run on a real fixed-rate
// schedule. The last case runs each of run_replay's three preconditions
// in a forked child and checks it aborts with its own message.
//
// Failures are counted rather than returned on, so a single run reports
// every broken case; the exit status is 1 if any check failed. No check
// depends on speed, and StubQueue copies every accepted record into a
// vector.
//
// Related: replay_producer.hpp (the code under test), replay_schedule.hpp
// (the schedules), record.hpp (Record), test_child_process.hpp
// (run_in_child).

#include "record.hpp"
#include "replay_producer.hpp"
#include "replay_schedule.hpp"
#include "test_child_process.hpp"

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


// Implements only what run_replay calls: try_push and full_rejections.
// Rejections are listed by push attempt, and the attempt index is the
// record index because run_replay pushes each record once and never
// retries. A list rather than a reject-every-Nth rule, so a test can
// abandon two records in a row, which no every-Nth rule with N > 1 can.
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


// Every record gets the same capture_wall_time_ns, the field
// build_replay_schedule takes its gaps from, so the schedule is all zeros
// and no record waits. Only the paced case gives spin_until a deadline to
// wait for.
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


// The sequence numbers missing from the delivered stream, counted before
// the first delivery, between deliveries and after the last. Their sum
// equals dropped_records because run_replay assigns a sequence before the
// push, so an abandoned record leaves a hole. Drops at either end leave no
// hole between delivered records, so the two boundary terms need
// first_sequence and slice_length from the stats.
//
// The sum alone does not catch a producer that assigns only on success:
// the delivered stream is then dense, the trailing term becomes
// slice_length - pushed, and that equals dropped_records. The leading
// case's first-sequence and width checks, and the mapping check, do.
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
        // Non-zero: Record's value-initialisation leaves symbol_id 0, so a
        // producer that skipped the stamp would pass a check against 0.
        7,
        slice_start,
        first_sequence,
        lag_ns
    );
}


// Every push accepted: the baseline the drop cases deviate from. Checks
// every field run_replay writes into a Record.
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

    // The tail bytes are a member rather than padding, and record.hpp
    // zeroes them, so they never hold an indeterminate value; run_replay
    // must leave them zero.
    check(name, reserved_zeroed, "Record::reserved was not zeroed");
}


// Every third attempt rejected, the last included, so the run ends on an
// abandoned record. The gaps between delivered records alone would sum to
// one short here.
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

    // run_replay abandons a rejected record instead of retrying, so every
    // rejection is a drop and the two counters must agree. They are
    // separate fields because a caller that retries has rejections without
    // drops.
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

    // Guards the input: if the last record were delivered, the trailing
    // term would be zero and this case would not exercise it.
    check(
        name,
        delivered.back().sequence != count - 1,
        "expected the final record to have been abandoned"
    );
}


// Three drops at the start and two in a row mid-run. Each abandoned record
// adds one to the sum, but two adjacent ones leave a single gap of width
// two, so the invariant is the sum, not the individual widths.
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


// Two runs over the same slice, the second starting where the first
// stopped. run_replay numbers records from the first_sequence it is given,
// not from their position in the slice, so a caller that starts each run
// at the previous run's first_sequence + slice_length gets one increasing
// sequence across the laps.
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


// The mapping from a delivered sequence back to its capture record, on a
// slice that starts partway into the file and a sequence origin that is
// not zero:
//
//     capture_index = slice_start + ((sequence - first_sequence) % slice_length)
//
// Without the "- first_sequence" term the mapping is right only when
// first_sequence is a multiple of slice_length, and otherwise returns the
// wrong record with nothing to show it.
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

    // The form without "- first_sequence" must disagree with the correct
    // one here. If it ever agrees, the inputs have drifted into the case
    // where the two coincide, and the mapping check above could no longer
    // tell them apart.
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


// A real fixed-rate schedule, so spin_until has later deadlines to wait
// for: 20,000 records at 5 MHz, a 4 ms run. Besides the push count and
// the clock advancing, the checks are the order of the lag percentiles
// and the source of the intended send times; none depends on the producer
// keeping up.
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

    // Intended send times must come from the schedule fixed before the run,
    // never from the clock when a record is reached: then a producer stall
    // would slide the schedule with it and never show as latency
    // (coordinated omission).
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


// The preconditions abort, so each is run in a forked child by
// run_in_child; test_child_process.hpp says why a fork rather than ctest's
// WILL_FAIL.

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


// Runs each precondition to confirm it fires, rather than trusting that it
// does.
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