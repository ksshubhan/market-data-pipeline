#include "mutex_queue.hpp"

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