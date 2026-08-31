#include "record.hpp"
#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

constexpr std::size_t kCapacity = 2;
constexpr std::uint64_t kIterations = 100'000'000;

using BrokenQueue = SpscRingBuffer<
    Record,
    kCapacity,
    128,
    SpscMemoryOrder::C1BrokenRelaxedPublication
>;

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

    std::atomic<bool> start{false};
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