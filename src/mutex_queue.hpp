// mutex_queue.hpp: MutexQueue, the baseline B1 compares the SPSC ring
// against. A bounded FIFO guarded by one std::mutex, with a condition
// variable so an idle consumer can sleep instead of spinning forever.
//
// It has the ring's interface: try_push and try_pop, both non-blocking, and
// try_push rejects the newest record when full. harness_b drives both
// queues through the same templated code. wait_nonempty() is the one extra
// entry point: a bounded spin, then a condvar wait, used only by this
// queue's consumer.
//
// Related: spsc_ring_buffer.hpp (the queue it is compared against),
// harness_b.cpp (B1 and the spin sweep), measure_condvar_wakeup.cpp (the
// park/wake and spin-iteration costs quoted below).

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>


// SpinCount is a template parameter so the spin sweep can instantiate
// several values without editing this header. The default is 8192; see
// kSpinCount below.
template <typename T, std::size_t Capacity, int SpinCount = 8192>
class MutexQueue {
    static_assert(Capacity > 0);
    static_assert(
        (Capacity & (Capacity - 1)) == 0,
        "MutexQueue capacity must be a power of two"
    );
    static_assert(std::is_trivially_copyable_v<T>);

public:
    MutexQueue() = default;

    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;

    bool try_push(const T& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t size =
            tail - head_.load(std::memory_order_relaxed);

        if (size == Capacity) {
            ++full_rejections_;
            return false;
        }

        const bool was_empty = (size == 0);

        if (was_empty) {
            // Counted under the lock, so this adds no sharing the mutex
            // does not already impose. A plain uint64_t is enough: it is
            // written here and read in signals(), both under mutex_.
            ++signals_;
        }

        buffer_[tail & kMask] = value;
        tail_.store(tail + 1, std::memory_order_relaxed);

        lock.unlock();

        if (was_empty) {
            not_empty_.notify_one();
        }

        return true;
    }

    bool try_pop(T& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::uint64_t head = head_.load(std::memory_order_relaxed);

        if (tail_.load(std::memory_order_relaxed) - head == 0) {
            return false;
        }

        value = buffer_[head & kMask];
        head_.store(head + 1, std::memory_order_relaxed);

        return true;
    }

    // Baseline-only blocking entry point, deliberately absent from the
    // shared interface: try_pop() stays non-blocking on both queues, or the
    // comparison is no longer like for like.
    //
    // Returns false only when the queue is closed and empty.
    bool wait_nonempty()
    {
        // The bounded spin. Without it the comparison is dismissed in one
        // sentence: a condvar wait on every empty queue means the baseline
        // pays a syscall for gaps routinely shorter than the syscall
        // itself, and beating that is not a result.
        //
        // The spin reads the counters without holding the lock, so they are
        // std::atomic and the reads are relaxed. With plain uint64_t the
        // unlocked read would be a data race and so undefined behaviour;
        // "benign in practice" is exactly the reasoning the C1 control
        // exists to disprove, and ThreadSanitizer would flag it.
        //
        // The mutex still provides all mutual exclusion and ordering; the
        // atomics only make this unlocked read well-defined. Relaxed loads
        // and stores are plain ldr and str on ARM64, so the locked paths
        // are unchanged. A stale read costs one wasted iteration, never a
        // missed wakeup: the predicate is re-evaluated under the lock
        // below.
        for (int i = 0; i < kSpinCount; ++i) {
            if (size_hint() != 0 || closed_hint()) {
                break;
            }

            cpu_relax();
        }

        std::unique_lock<std::mutex> lock(mutex_);

        // Distinguishes "reached the condvar" from "actually blocked":
        // if the predicate already holds, wait() returns without
        // parking and no wake syscall is needed. Only the latter costs
        // the producer a __ulock_wake, so only the latter is counted.
        const bool would_block =
            size_hint() == 0 &&
            !closed_.load(std::memory_order_relaxed);

        if (would_block) {
            ++parks_;
        }

        not_empty_.wait(lock, [this] {
            return size_hint() != 0 ||
                   closed_.load(std::memory_order_relaxed);
        });

        return size_hint() != 0;
    }

    // Wakes a blocked consumer when the producer is finished.
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_.store(true, std::memory_order_relaxed);
        }

        not_empty_.notify_one();
    }

    std::uint64_t full_rejections() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return full_rejections_;
    }


    // Number of empty -> non-empty transitions, i.e. the number of
    // notify_one calls the producer made. Each one where a consumer was
    // actually parked costs the producer a wake syscall on the critical
    // path of its own send schedule.
    std::uint64_t signals() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return signals_;
    }


    // Number of times the consumer exhausted its spin budget and blocked.
    std::uint64_t parks() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return parks_;
    }


    static constexpr int spin_count() noexcept
    {
        return SpinCount;
    }


    static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

private:
    // The spin budget: 8192, chosen from a measured sweep. The number it
    // replaced is kept here because the way the first answer was wrong is
    // worth more than the answer.
    //
    // First attempt, 4 Sep: the ski-rental bound. Spin for exactly the cost
    // of a park and wake and the worst case is twice optimal.
    // measure_condvar_wakeup measured park/wake at ~1296 ns and a contended
    // spin iteration at ~1.29 ns, giving 1004, rounded to 1000.
    //
    // That model was incomplete. It costs the waiter correctly and ignores
    // what blocking costs the signaller: when a consumer is parked, the
    // producer's notify_one becomes a __ulock_wake syscall on the critical
    // path of its own send schedule.
    //
    // The spin sweep measured that directly
    // (results/spin_sweep_20260905_130613.csv). At 1M records/s, spin 1000
    // parked on 646,253 of 2,000,000 messages with producer p99 lag 3208
    // ns; spin 8192 parked 437 times with p99 lag 41 ns, one clock tick.
    // Consumer p99 latency fell from 9917 ns to 416 ns. 65536 parked 8
    // times with the same producer lag, but consumer p99 rose to 1416 ns,
    // and at 500k/s producer p99 lag rose from 41 to 167 ns: going higher
    // costs more than it buys.
    //
    // What 8192 is in time depends on the state of the line being spun on.
    // At the contended ~1.29 ns per iteration it is ~10.6 us. On a quiet
    // queue measure_condvar_wakeup measured ~0.29 ns, so ~2.4 us, which
    // covers the gap between messages only at ~425k/s and above. So at
    // 100k/s and 250k/s the consumer still parks on about half the messages
    // (results/harness_b_spin8192_20260905_131415.csv). Those rows pass the
    // lag gate anyway: the gaps there are long enough to absorb the wake.
    //
    // What the budget does not fix: at 5M/s it stops mattering. Across the
    // sweep parks fall from 69,674 to 3 and producer p99 lag does not move
    // (6075, 5891, 6358 ns). There the cost is the lock itself, not the
    // wait policy, and the baseline's ceiling is real.
    //
    // SpinCount stays a template parameter because 1000 remains a reported
    // configuration, not dead code. A condvar queue that blocks is
    // catastrophic on the tail, and tuning that away is the point of a fair
    // baseline, so both are run and both are reported. Reporting only the
    // tuned arm understates the mechanism the project is about; reporting
    // only the parking arm is a strawman.
    static constexpr int kSpinCount = SpinCount;

    // Unlocked reads, used only to decide whether to take the lock. Atomic,
    // so not a data race; possibly stale, which the locked re-check
    // absorbs. See wait_nonempty().
    std::uint64_t size_hint() const noexcept
    {
        return tail_.load(std::memory_order_relaxed) -
               head_.load(std::memory_order_relaxed);
    }

    bool closed_hint() const noexcept
    {
        return closed_.load(std::memory_order_relaxed);
    }

    static void cpu_relax() noexcept
    {
#if defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
        asm volatile("pause" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

    static constexpr std::uint64_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};

    // Monotonic counters. Written only under mutex_, which supplies the
    // mutual exclusion; atomic solely so wait_nonempty()'s bounded spin
    // can read them without the lock without that read being a data
    // race.
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};

    // Incremented by the producer whenever try_push() rejects on full.
    std::uint64_t full_rejections_{0};

    // Set once the producer has finished. Atomic for the same reason as
    // the counters above.
    std::atomic<bool> closed_{false};

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;

    // Diagnostics for the spin sweep. signals_ is producer-written and
    // parks_ consumer-written, both only under mutex_, and read through
    // accessors that take it.
    std::uint64_t signals_ = 0;
    std::uint64_t parks_ = 0;
};
