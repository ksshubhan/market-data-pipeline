#include "mutex_queue.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
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


using WakeQueue = MutexQueue<std::uint64_t, 4>;

// A fifth of the runner's 10 s per-run limit, so a failing run exits on
// its own FAIL line rather than the runner's timeout, and at least five
// orders of magnitude above the ~10 us spin and ~1.3 us park/wake it has
// to cover. That margin is a judgement; the controls runner's 100-run
// unmutated baseline is the evidence that it produces no false FAIL.
constexpr auto kDeadline = std::chrono::seconds(2);


// Returns once the consumer is blocked in wait(), or false at the
// deadline. wait_nonempty() increments parks_ under the mutex in the same
// critical section as its wait() call, and parks() takes that mutex, so
// reading 1 means the consumer has released the lock inside wait() --
// which releases and blocks as one step -- and stays there until
// notified. A spurious wakeup finds the predicate false and waits again.
bool wait_for_park(const WakeQueue& queue)
{
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;

    while (queue.parks() < 1) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}


// For failures while the consumer may still be blocked. It cannot be
// joined (that hangs), destroyed while joinable (std::terminate, exit
// 134), or rescued by close() when close()'s notify is what is under
// test. check() writes to std::cerr, which is unit-buffered, so the FAIL
// line is out before _Exit skips the destructors.
[[noreturn]] void fail_with_blocked_consumer(const char* message)
{
    check(false, message);
    std::_Exit(1);
}

} // namespace


int main()
{
    {
        MutexQueue<std::uint64_t, 4> queue;
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

        // Exercise wrapped slot indices.
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

    // W1: the consumer is confirmed blocked before the push, so the push
    // is the only thing that can wake it. The blocks after W2 push or
    // close immediately; their consumer parked first in 0 of 200 runs on
    // 19 Sep, so a deleted notify_one passes them. These two exist so
    // that it cannot.
    {
        WakeQueue queue;

        std::promise<bool> woke;
        std::future<bool> woke_result = woke.get_future();

        std::thread consumer([&queue, &woke] {
            woke.set_value(queue.wait_nonempty());
        });

        if (!wait_for_park(queue)) {
            fail_with_blocked_consumer(
                "consumer did not park before the data push"
            );
        }

        if (!queue.try_push(456)) {
            fail_with_blocked_consumer(
                "push into queue with parked consumer failed"
            );
        }

        if (woke_result.wait_for(kDeadline) != std::future_status::ready) {
            fail_with_blocked_consumer(
                "parked consumer was not woken by push"
            );
        }

        consumer.join();

        if (!check(
                woke_result.get(),
                "parked consumer woken by push should report data"
            )) {
            return 1;
        }
    }

    // W2: as W1, with close() as the only thing that can wake it.
    {
        WakeQueue queue;

        std::promise<bool> woke;
        std::future<bool> woke_result = woke.get_future();

        std::thread consumer([&queue, &woke] {
            woke.set_value(queue.wait_nonempty());
        });

        if (!wait_for_park(queue)) {
            fail_with_blocked_consumer("consumer did not park before close");
        }

        queue.close();

        if (woke_result.wait_for(kDeadline) != std::future_status::ready) {
            fail_with_blocked_consumer(
                "parked consumer was not woken by close"
            );
        }

        consumer.join();

        if (!check(
                !woke_result.get(),
                "parked consumer woken by close should report completion"
            )) {
            return 1;
        }
    }

    {
        MutexQueue<std::uint64_t, 4> queue;

        bool wait_result = false;

        std::thread consumer([&] {
            wait_result = queue.wait_nonempty();
        });

        if (!check(queue.try_push(123), "push used to wake consumer failed")) {
            consumer.join();
            return 1;
        }

        consumer.join();

        if (!check(wait_result, "wait_nonempty should wake for available data")) {
            return 1;
        }

        std::uint64_t value = 0;

        if (!check(
                queue.try_pop(value) && value == 123,
                "woken consumer should find queued value"
            )) {
            return 1;
        }
    }

    {
        MutexQueue<std::uint64_t, 4> queue;

        bool wait_result = true;

        std::thread consumer([&] {
            wait_result = queue.wait_nonempty();
        });

        queue.close();
        consumer.join();

        if (!check(
                !wait_result,
                "wait_nonempty should return false when closed and empty"
            )) {
            return 1;
        }
    }

    {
        MutexQueue<std::uint64_t, 4> queue;

        if (!check(queue.try_push(10), "push before close should succeed")) {
            return 1;
        }

        if (!check(queue.try_push(20), "second push before close should succeed")) {
            return 1;
        }

        queue.close();

        if (!check(
                queue.wait_nonempty(),
                "closed queue with remaining data should report nonempty"
            )) {
            return 1;
        }

        std::uint64_t value = 0;

        if (!check(
                queue.try_pop(value) && value == 10,
                "first queued value should survive close"
            )) {
            return 1;
        }

        if (!check(
                queue.try_pop(value) && value == 20,
                "second queued value should survive close"
            )) {
            return 1;
        }

        if (!check(
                !queue.wait_nonempty(),
                "closed and drained queue should report completion"
            )) {
            return 1;
        }
    }

    std::cout << "mutex queue tests passed\n";
    return 0;
}
