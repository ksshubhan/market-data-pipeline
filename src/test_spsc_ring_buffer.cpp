#include "spsc_ring_buffer.hpp"
#include "record.hpp"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>


namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    return true;
}

// A4: the uncached arm must behave identically. Same FIFO order, same
// full-capacity behaviour, same rejection accounting - the only
// difference is when the opposite index is read.
bool test_uncached_queue()
{
    using Queue = SpscRingBuffer<
        std::uint64_t,
        4,
        128,
        SpscMemoryOrder::AcquireRelease,
        SpscIndexCaching::Uncached
    >;

    Queue queue;

    for (std::uint64_t value : {10, 20, 30, 40}) {
        if (!check(queue.try_push(value), "uncached push failed")) {
            return false;
        }
    }

    if (!check(
            !queue.try_push(50),
            "uncached full queue accepted extra item"
        )) {
        return false;
    }

    if (!check(
            queue.full_rejections() == 1,
            "uncached full rejection count incorrect"
        )) {
        return false;
    }

    std::uint64_t value = 0;

    for (std::uint64_t expected : {10, 20, 30, 40}) {
        if (!check(
                queue.try_pop(value) && value == expected,
                "uncached FIFO mismatch"
            )) {
            return false;
        }
    }

    if (!check(
            !queue.try_pop(value),
            "uncached empty queue returned an item"
        )) {
        return false;
    }

    // Wrap the masked indices.
    if (!check(
            queue.try_push(60) && queue.try_push(70),
            "uncached wrapped push failed"
        )) {
        return false;
    }

    if (!check(
            queue.try_pop(value) && value == 60,
            "uncached wrapped FIFO failed"
        )) {
        return false;
    }

    if (!check(
            queue.try_pop(value) && value == 70,
            "uncached wrapped FIFO failed"
        )) {
        return false;
    }

    return true;
}


bool test_uncached_two_thread()
{
    constexpr std::uint64_t kCount = 1'000'000;

    using Queue = SpscRingBuffer<
        std::uint64_t,
        1024,
        128,
        SpscMemoryOrder::AcquireRelease,
        SpscIndexCaching::Uncached
    >;

    Queue queue;

    std::atomic<bool> failed{false};

    std::thread consumer([&] {
        for (std::uint64_t expected = 0; expected < kCount; ++expected) {
            std::uint64_t value = 0;

            while (!queue.try_pop(value)) {
            }

            if (value != expected) {
                std::cerr
                    << "FAIL: uncached two-thread handoff corrupted FIFO"
                       " order at index "
                    << expected
                    << ", observed "
                    << value
                    << '\n';

                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    std::thread producer([&] {
        for (std::uint64_t value = 0; value < kCount; ++value) {
            while (!queue.try_push(value)) {
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
            }
        }
    });

    producer.join();
    consumer.join();

    return !failed.load(std::memory_order_acquire);
}


bool test_seq_cst_queue()
{
    using Queue = SpscRingBuffer<
        std::uint64_t,
        4,
        128,
        SpscMemoryOrder::SeqCst
    >;

    Queue queue;

    if (!check(queue.try_push(10), "seq_cst push 10 failed")) {
        return false;
    }

    if (!check(queue.try_push(20), "seq_cst push 20 failed")) {
        return false;
    }

    if (!check(queue.try_push(30), "seq_cst push 30 failed")) {
        return false;
    }

    if (!check(queue.try_push(40), "seq_cst push 40 failed")) {
        return false;
    }

    if (!check(
            !queue.try_push(50),
            "seq_cst full queue accepted extra item"
        )) {
        return false;
    }

    if (!check(
            queue.full_rejections() == 1,
            "seq_cst full rejection count incorrect"
        )) {
        return false;
    }

    std::uint64_t value = 0;

    if (!check(
            queue.try_pop(value) && value == 10,
            "seq_cst FIFO mismatch for 10"
        )) {
        return false;
    }

    if (!check(
            queue.try_pop(value) && value == 20,
            "seq_cst FIFO mismatch for 20"
        )) {
        return false;
    }

    if (!check(
            queue.try_pop(value) && value == 30,
            "seq_cst FIFO mismatch for 30"
        )) {
        return false;
    }

    if (!check(
            queue.try_pop(value) && value == 40,
            "seq_cst FIFO mismatch for 40"
        )) {
        return false;
    }

    if (!check(
            !queue.try_pop(value),
            "seq_cst empty queue returned an item"
        )) {
        return false;
    }

    return true;
}

} // namespace


int main()
{
    {
        SpscRingBuffer<std::uint64_t, 4> queue;
        std::uint64_t value = 0;

        if (!check(!queue.try_pop(value), "new queue should be empty")) {
            return 1;
        }

        if (!check(queue.capacity() == 4, "capacity should be 4")) {
            return 1;
        }

        if (!check(queue.try_push(10), "push 10 should succeed")) {
            return 1;
        }

        if (!check(queue.try_push(20), "push 20 should succeed")) {
            return 1;
        }

        if (!check(queue.try_push(30), "push 30 should succeed")) {
            return 1;
        }

        if (!check(queue.try_push(40), "push 40 should succeed")) {
            return 1;
        }

        if (!check(
                !queue.try_push(50),
                "fifth push into capacity-4 queue should be rejected"
            )) {
            return 1;
        }

        if (!check(
                queue.full_rejections() == 1,
                "full_rejections should be exactly 1"
            )) {
            return 1;
        }

        for (std::uint64_t expected : {10, 20, 30, 40}) {
            if (!check(queue.try_pop(value), "pop should succeed")) {
                return 1;
            }

            if (!check(value == expected, "FIFO order is incorrect")) {
                return 1;
            }
        }

        if (!check(!queue.try_pop(value), "queue should be empty after drain")) {
            return 1;
        }

        // Force the masked slot indices to wrap.
        if (!check(queue.try_push(60), "wrapped push 60 should succeed")) {
            return 1;
        }

        if (!check(queue.try_push(70), "wrapped push 70 should succeed")) {
            return 1;
        }

        if (!check(queue.try_pop(value) && value == 60, "wrapped FIFO failed")) {
            return 1;
        }

        if (!check(queue.try_pop(value) && value == 70, "wrapped FIFO failed")) {
            return 1;
        }
    }

    {
        SpscRingBuffer<Record, 4> queue;

        Record input{};
        input.sequence = 123;
        input.replay_intended_send_ns = 456;

        input.capture.capture_wall_time_ns = 1000;
        input.capture.event_time_ms = 2000;
        input.capture.transaction_time_ms = 3000;
        input.capture.bid_price = 4000;
        input.capture.ask_price = 5000;
        input.capture.bid_qty = 6000;
        input.capture.ask_qty = 7000;

        input.symbol_id = 1;

        if (!check(
                queue.try_push(input),
                "Record push should succeed"
            )) {
            return 1;
        }

        Record output{};

        if (!check(
                queue.try_pop(output),
                "Record pop should succeed"
            )) {
            return 1;
        }

        if (!check(output.sequence == input.sequence, "Record sequence corrupted")) {
            return 1;
        }

        if (!check(
                output.replay_intended_send_ns == input.replay_intended_send_ns,
                "Record intended-send timestamp corrupted"
            )) {
            return 1;
        }

        if (!check(
                output.capture.capture_wall_time_ns ==
                    input.capture.capture_wall_time_ns,
                "Record capture timestamp corrupted"
            )) {
            return 1;
        }

        if (!check(
                output.capture.event_time_ms == input.capture.event_time_ms,
                "Record event timestamp corrupted"
            )) {
            return 1;
        }

        if (!check(
                output.capture.transaction_time_ms ==
                    input.capture.transaction_time_ms,
                "Record transaction timestamp corrupted"
            )) {
            return 1;
        }

        if (!check(
                output.capture.bid_price == input.capture.bid_price &&
                output.capture.ask_price == input.capture.ask_price &&
                output.capture.bid_qty == input.capture.bid_qty &&
                output.capture.ask_qty == input.capture.ask_qty,
                "Record market-data fields corrupted"
            )) {
            return 1;
        }

        if (!check(
                output.symbol_id == input.symbol_id,
                "Record symbol_id corrupted"
            )) {
            return 1;
        }
    }


    {
        constexpr std::uint64_t kCount = 1'000'000;

        SpscRingBuffer<std::uint64_t, 1024> queue;

        // A correctness failure must terminate this test, not deadlock it.
        // If the consumer returns early on a mismatch and the producer has
        // no way to learn that, the producer spins for ever on a full
        // queue and producer.join() never returns — so the test's failure
        // mode becomes a hang rather than a report.
        std::atomic<bool> failed{false};

        std::thread consumer([&] {
            for (std::uint64_t expected = 0; expected < kCount; ++expected) {
                std::uint64_t value = 0;

                while (!queue.try_pop(value)) {
                    // Test harness retries on empty. The producer cannot
                    // exit early while the consumer is still running, so
                    // this loop needs no escape hatch.
                }

                if (value != expected) {
                    std::cerr
                        << "FAIL: two-thread SPSC handoff corrupted FIFO"
                           " order at index "
                        << expected
                        << ", observed "
                        << value
                        << '\n';

                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });

        std::thread producer([&] {
            for (std::uint64_t value = 0; value < kCount; ++value) {
                while (!queue.try_push(value)) {
                    // Test harness retries on full.
                    if (failed.load(std::memory_order_acquire)) {
                        return;
                    }
                }
            }
        });

        producer.join();
        consumer.join();

        if (failed.load(std::memory_order_acquire)) {
            return 1;
        }
    }

    if (!test_seq_cst_queue()) {
        return 1;
    }

    if (!test_uncached_queue()) {
        return 1;
    }

    if (!test_uncached_two_thread()) {
        return 1;
    }

    std::cout << "spsc ring buffer tests passed\n";
    return 0;
}