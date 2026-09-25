// Negative control for the SPSC ring buffer's publication ordering.
// Intentionally invalid C++: not a benchmark and not a ctest entry.
//
// Instantiates SpscRingBuffer with C1BrokenRelaxedPublication, which makes
// the consumer's load of the producer's index relaxed instead of acquire.
// The producer's release store is unchanged, but a release that no acquire
// reads synchronises with nothing, so the consumer's copy of a slot races
// with the producer's write of it: a data race on a non-atomic Record.
// ThreadSanitizer reports that race; on ARM64 a native run can also observe
// it as a corrupted record. Slot reuse (the consumer's release of its index,
// the producer's acquire of it) is left intact, so only publication breaks.
//
// Output: exit 0 and a note on stdout if all kIterations records arrive
// intact; otherwise exit 1 and, on stderr, the expected and observed value
// of every field of the first bad record. tools/classify_c1_runs.awk parses
// that stderr format, so change it only together with the script.
//
// See ARCHITECTURE.md for where this sits among the harnesses.

#include "record.hpp"
#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

// Two slots: each is overwritten every other record, so the slot the
// consumer reads for record N last held record N - 2. A stale read shows
// up as fields from N - 2, which is what print_failure reports alongside.
constexpr std::size_t kCapacity = 2;

// An upper bound, not a target: the consumer stops at the first bad record,
// so a run reaches kIterations only when it observes no corruption at all.
constexpr std::uint64_t kIterations = 100'000'000;

using BrokenQueue = SpscRingBuffer<
    Record,
    kCapacity,
    128,
    SpscMemoryOrder::C1BrokenRelaxedPublication
>;

// Every field is sequence * 10 plus a per-field constant (sequence itself
// is exact), so each observed field names the record it came from. That
// lets a failure be classified as a whole stale record or a mix of two,
// not merely "wrong".
Record make_record(std::uint64_t sequence)
{
    Record record{};

    record.sequence = sequence;
    record.replay_intended_send_ns = sequence * 10 + 100;

    record.capture.capture_wall_time_ns = sequence * 10 + 200;
    record.capture.event_time_ms = sequence * 10 + 300;
    record.capture.transaction_time_ms = sequence * 10 + 400;

    record.capture.bid_price =
        static_cast<std::int64_t>(sequence * 10 + 1);
    record.capture.ask_price =
        static_cast<std::int64_t>(sequence * 10 + 2);
    record.capture.bid_qty =
        static_cast<std::int64_t>(sequence * 10 + 3);
    record.capture.ask_qty =
        static_cast<std::int64_t>(sequence * 10 + 4);

    record.symbol_id = 1;

    // Record{} already zero-initialises reserved[6].

    return record;
}

bool matches_expected(
    const Record& observed,
    std::uint64_t expected_sequence
)
{
    const Record expected = make_record(expected_sequence);

    return
        observed.sequence == expected.sequence &&
        observed.replay_intended_send_ns ==
            expected.replay_intended_send_ns &&
        observed.capture.capture_wall_time_ns ==
            expected.capture.capture_wall_time_ns &&
        observed.capture.event_time_ms ==
            expected.capture.event_time_ms &&
        observed.capture.transaction_time_ms ==
            expected.capture.transaction_time_ms &&
        observed.capture.bid_price ==
            expected.capture.bid_price &&
        observed.capture.ask_price ==
            expected.capture.ask_price &&
        observed.capture.bid_qty ==
            expected.capture.bid_qty &&
        observed.capture.ask_qty ==
            expected.capture.ask_qty &&
        observed.symbol_id == expected.symbol_id;
}

void print_failure(
    const Record& observed,
    std::uint64_t expected_sequence
)
{
    const Record expected = make_record(expected_sequence);

    std::cerr << "C1 observable corruption detected\n";
    std::cerr << "expected_sequence: "
              << expected_sequence << '\n';

    std::cerr << "observed_sequence: "
              << observed.sequence << '\n';

    if (expected_sequence >= kCapacity) {
        std::cerr << "expected_sequence_minus_capacity: "
                  << expected_sequence - kCapacity << '\n';
    }

    std::cerr
        << "field,expected,observed\n"
        << "sequence,"
        << expected.sequence << ','
        << observed.sequence << '\n'

        << "replay_intended_send_ns,"
        << expected.replay_intended_send_ns << ','
        << observed.replay_intended_send_ns << '\n'

        << "capture_wall_time_ns,"
        << expected.capture.capture_wall_time_ns << ','
        << observed.capture.capture_wall_time_ns << '\n'

        << "event_time_ms,"
        << expected.capture.event_time_ms << ','
        << observed.capture.event_time_ms << '\n'

        << "transaction_time_ms,"
        << expected.capture.transaction_time_ms << ','
        << observed.capture.transaction_time_ms << '\n'

        << "bid_price,"
        << expected.capture.bid_price << ','
        << observed.capture.bid_price << '\n'

        << "ask_price,"
        << expected.capture.ask_price << ','
        << observed.capture.ask_price << '\n'

        << "bid_qty,"
        << expected.capture.bid_qty << ','
        << observed.capture.bid_qty << '\n'

        << "ask_qty,"
        << expected.capture.ask_qty << ','
        << observed.capture.ask_qty << '\n'

        << "symbol_id,"
        << expected.symbol_id << ','
        << observed.symbol_id << '\n';
}

} // namespace

int main()
{
    BrokenQueue queue;

    // Released by main once both threads exist, so neither begins early.
    // Pairs with the acquire load at the top of each thread.
    std::atomic<bool> start{false};

    // The harness's own flags are correctly ordered; only the queue under test
    // is broken. failed lets whichever thread is still spinning stop once the
    // consumer has reported a bad record.
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
        }

        for (
            std::uint64_t sequence = 0;
            sequence < kIterations;
            ++sequence
        ) {
            const Record record = make_record(sequence);

            // Retry rather than drop: every sequence number is delivered
            // exactly once, so any mismatch is ordering corruption, never a
            // legitimate gap.
            while (!queue.try_push(record)) {
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
            }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
        }

        std::uint64_t expected_sequence = 0;

        while (expected_sequence < kIterations) {
            Record observed{};

            if (!queue.try_pop(observed)) {
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }

                continue;
            }

            if (!matches_expected(
                    observed,
                    expected_sequence
                )) {
                print_failure(
                    observed,
                    expected_sequence
                );

                failed.store(
                    true,
                    std::memory_order_release
                );

                return;
            }

            ++expected_sequence;
        }
    });

    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    if (failed.load(std::memory_order_acquire)) {
        return 1;
    }

    std::cout
        << "C1: no observable corruption detected in this "
           "finite native run.\n"
        << "The implementation remains intentionally invalid C++.\n";

    return 0;
}