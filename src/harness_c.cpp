#include "record.hpp"
#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

constexpr std::size_t kDenseCapacity = 1024;
constexpr std::size_t kDropCapacity = 8;

constexpr std::uint64_t kDenseIterations = 100'000'000;
constexpr std::uint64_t kDropIterations = 10'000'000;
constexpr std::uint64_t kForcedDrops = 32;

using DenseQueue = SpscRingBuffer<
    Record,
    kDenseCapacity,
    128,
    SpscMemoryOrder::AcquireRelease
>;

using DropQueue = SpscRingBuffer<
    Record,
    kDropCapacity,
    128,
    SpscMemoryOrder::AcquireRelease
>;

Record make_record(std::uint64_t sequence)
{
    Record record{};

    record.sequence = sequence;
    record.replay_intended_send_ns =
        sequence * 10 + 100;

    record.capture.capture_wall_time_ns =
        sequence * 10 + 200;
    record.capture.event_time_ms =
        sequence * 10 + 300;
    record.capture.transaction_time_ms =
        sequence * 10 + 400;

    record.capture.bid_price =
        static_cast<std::int64_t>(
            sequence * 10 + 1
        );

    record.capture.ask_price =
        static_cast<std::int64_t>(
            sequence * 10 + 2
        );

    record.capture.bid_qty =
        static_cast<std::int64_t>(
            sequence * 10 + 3
        );

    record.capture.ask_qty =
        static_cast<std::int64_t>(
            sequence * 10 + 4
        );

    record.symbol_id =
        static_cast<std::uint16_t>(
            sequence & 0xffffU
        );

    for (std::size_t i = 0; i < 6; ++i) {
        record.reserved[i] =
            static_cast<std::uint8_t>(
                (sequence + i) & 0xffU
            );
    }

    return record;
}

bool matches_expected(const Record& observed)
{
    const Record expected =
        make_record(observed.sequence);

    if (
        observed.sequence != expected.sequence ||
        observed.replay_intended_send_ns !=
            expected.replay_intended_send_ns ||
        observed.capture.capture_wall_time_ns !=
            expected.capture.capture_wall_time_ns ||
        observed.capture.event_time_ms !=
            expected.capture.event_time_ms ||
        observed.capture.transaction_time_ms !=
            expected.capture.transaction_time_ms ||
        observed.capture.bid_price !=
            expected.capture.bid_price ||
        observed.capture.ask_price !=
            expected.capture.ask_price ||
        observed.capture.bid_qty !=
            expected.capture.bid_qty ||
        observed.capture.ask_qty !=
            expected.capture.ask_qty ||
        observed.symbol_id != expected.symbol_id
    ) {
        return false;
    }

    for (std::size_t i = 0; i < 6; ++i) {
        if (
            observed.reserved[i] !=
            expected.reserved[i]
        ) {
            return false;
        }
    }

    return true;
}

bool run_dense_phase()
{
    DenseQueue queue;

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};

    std::uint64_t pushes_completed = 0;
    std::uint64_t pops_completed = 0;

    std::thread consumer([&] {
        while (!start.load(
            std::memory_order_acquire
        )) {
        }

        for (
            std::uint64_t expected_sequence = 0;
            expected_sequence < kDenseIterations;
            ++expected_sequence
        ) {
            Record observed{};

            while (!queue.try_pop(observed)) {
                if (failed.load(
                    std::memory_order_acquire
                )) {
                    return;
                }
            }

            if (
                observed.sequence !=
                    expected_sequence ||
                !matches_expected(observed)
            ) {
                std::cerr
                    << "Harness C dense failure\n"
                    << "expected_sequence: "
                    << expected_sequence << '\n'
                    << "observed_sequence: "
                    << observed.sequence << '\n';

                failed.store(
                    true,
                    std::memory_order_release
                );

                return;
            }

            ++pops_completed;
        }
    });

    std::thread producer([&] {
        while (!start.load(
            std::memory_order_acquire
        )) {
        }

        for (
            std::uint64_t sequence = 0;
            sequence < kDenseIterations;
            ++sequence
        ) {
            const Record record =
                make_record(sequence);

            while (!queue.try_push(record)) {
                if (failed.load(
                    std::memory_order_acquire
                )) {
                    return;
                }
            }

            ++pushes_completed;
        }
    });

    start.store(
        true,
        std::memory_order_release
    );

    producer.join();
    consumer.join();

    if (failed.load(std::memory_order_acquire)) {
        return false;
    }

    if (
        pushes_completed != kDenseIterations ||
        pops_completed != kDenseIterations
    ) {
        std::cerr
            << "Harness C dense count failure\n"
            << "pushes_completed: "
            << pushes_completed << '\n'
            << "pops_completed: "
            << pops_completed << '\n';

        return false;
    }

    std::cout
        << "dense_phase: passed\n"
        << "logical_records: "
        << kDenseIterations << '\n'
        << "pushes_completed: "
        << pushes_completed << '\n'
        << "pops_completed: "
        << pops_completed << '\n'
        << "full_rejections: "
        << queue.full_rejections() << '\n'
        << "dropped_records: 0\n";

    return true;
}

bool run_drop_phase()
{
    DropQueue queue;

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> consumer_go{false};
    std::atomic<bool> consumer_has_popped{false};
    std::atomic<bool> producer_done{false};

    std::uint64_t pushes_completed = 0;
    std::uint64_t pops_completed = 0;
    std::uint64_t dropped_records = 0;

    std::uint64_t observed_gap_records = 0;
    std::uint64_t next_expected_sequence = 0;

    std::thread consumer([&] {
        while (!start.load(
            std::memory_order_acquire
        )) {
        }

        while (!consumer_go.load(
            std::memory_order_acquire
        )) {
            if (failed.load(
                std::memory_order_acquire
            )) {
                return;
            }
        }

        for (;;) {
            Record observed{};

            if (!queue.try_pop(observed)) {
                if (!producer_done.load(
                        std::memory_order_acquire
                    )) {
                    continue;
                }

                // Recheck after synchronising with producer_done.
                // All successful producer pushes happened before this point.
                if (!queue.try_pop(observed)) {
                    break;
                }
            }

            if (
                observed.sequence <
                next_expected_sequence
            ) {
                std::cerr
                    << "Harness C sequence "
                       "repeat/decrease\n"
                    << "next_expected_sequence: "
                    << next_expected_sequence << '\n'
                    << "observed_sequence: "
                    << observed.sequence << '\n';

                failed.store(
                    true,
                    std::memory_order_release
                );

                return;
            }

            observed_gap_records +=
                observed.sequence -
                next_expected_sequence;

            next_expected_sequence =
                observed.sequence + 1;

            if (!matches_expected(observed)) {
                std::cerr
                    << "Harness C payload "
                       "corruption\n"
                    << "observed_sequence: "
                    << observed.sequence << '\n';

                failed.store(
                    true,
                    std::memory_order_release
                );

                return;
            }

            ++pops_completed;

            consumer_has_popped.store(
                true,
                std::memory_order_release
            );
        }
    });

    std::thread producer([&] {
        while (!start.load(
            std::memory_order_acquire
        )) {
        }

        std::uint64_t sequence = 0;

        // Fill the queue while the consumer is held back.
        for (
            ;
            sequence < kDropCapacity;
            ++sequence
        ) {
            const Record record =
                make_record(sequence);

            if (!queue.try_push(record)) {
                std::cerr
                    << "Harness C could not fill "
                       "initial queue\n";

                failed.store(
                    true,
                    std::memory_order_release
                );

                producer_done.store(
                    true,
                    std::memory_order_release
                );

                return;
            }

            ++pushes_completed;
        }

        // These must be rejected and abandoned.
        for (
            std::uint64_t i = 0;
            i < kForcedDrops;
            ++i, ++sequence
        ) {
            const Record record =
                make_record(sequence);

            if (queue.try_push(record)) {
                std::cerr
                    << "Harness C expected forced "
                       "drop rejection\n";

                failed.store(
                    true,
                    std::memory_order_release
                );

                producer_done.store(
                    true,
                    std::memory_order_release
                );

                return;
            }

            ++dropped_records;
        }

        consumer_go.store(
            true,
            std::memory_order_release
        );

        while (!consumer_has_popped.load(
            std::memory_order_acquire
        )) {
            if (failed.load(
                std::memory_order_acquire
            )) {
                producer_done.store(
                    true,
                    std::memory_order_release
                );

                return;
            }
        }

        for (
            ;
            sequence < kDropIterations;
            ++sequence
        ) {
            const Record record =
                make_record(sequence);

            if (queue.try_push(record)) {
                ++pushes_completed;
            } else {
                ++dropped_records;
            }
        }

        producer_done.store(
            true,
            std::memory_order_release
        );
    });

    start.store(
        true,
        std::memory_order_release
    );

    producer.join();
    consumer.join();

    if (failed.load(std::memory_order_acquire)) {
        return false;
    }

    if (next_expected_sequence < kDropIterations) {
        observed_gap_records +=
            kDropIterations -
            next_expected_sequence;
    }

    const std::uint64_t full_rejections =
        queue.full_rejections();

    if (
        pushes_completed + dropped_records !=
        kDropIterations
    ) {
        std::cerr
            << "Harness C accounting failure: "
               "pushes + drops\n";

        return false;
    }

    if (pops_completed != pushes_completed) {
        std::cerr
            << "Harness C accounting failure: "
               "pops != pushes\n"
            << "pushes_completed: "
            << pushes_completed << '\n'
            << "pops_completed: "
            << pops_completed << '\n';

        return false;
    }

    if (full_rejections != dropped_records) {
        std::cerr
            << "Harness C accounting failure: "
               "rejections != drops\n"
            << "full_rejections: "
            << full_rejections << '\n'
            << "dropped_records: "
            << dropped_records << '\n';

        return false;
    }

    if (observed_gap_records != dropped_records) {
        std::cerr
            << "Harness C gap reconciliation "
               "failure\n"
            << "observed_gap_records: "
            << observed_gap_records << '\n'
            << "dropped_records: "
            << dropped_records << '\n';

        return false;
    }

    if (dropped_records < kForcedDrops) {
        std::cerr
            << "Harness C forced-drop count "
               "incorrect\n";

        return false;
    }

    std::cout
        << "drop_phase: passed\n"
        << "logical_records: "
        << kDropIterations << '\n'
        << "pushes_completed: "
        << pushes_completed << '\n'
        << "pops_completed: "
        << pops_completed << '\n'
        << "full_rejections: "
        << full_rejections << '\n'
        << "dropped_records: "
        << dropped_records << '\n'
        << "observed_gap_records: "
        << observed_gap_records << '\n';

    return true;
}

} // namespace

int main()
{
    if (!run_dense_phase()) {
        return 1;
    }

    if (!run_drop_phase()) {
        return 1;
    }

    std::cout << "Harness C passed\n";

    return 0;
}
