#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>


// SpinCount is a template parameter so the constant can be swept as a
// diagnostic without editing the header between runs. The default is the
// derived 1000 documented at kSpinCount below; harness_b's spin-sweep
// mode instantiates other values to test whether producer lag in the
// baseline arm is caused by wake syscalls.
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
            // does not already impose. Read after join, which supplies
            // the happens-before edge, so a plain uint64_t is correct
            // here for the same reason §6.5b gives for full_rejections.
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
    // shared interface (§4). try_pop() stays non-blocking on both arms,
    // or the parity argument collapses.
    //
    // Returns false only when the queue is closed and empty.
    bool wait_nonempty()
    {
        // §4's bounded spin. Without it an interviewer dismisses the
        // comparison in one sentence: a condvar wait on every empty queue
        // means the baseline pays a syscall for intervals that are
        // routinely shorter than the syscall itself, and beating that is
        // not a result.
        //
        // The spin reads the counters without holding the lock, so the
        // members are std::atomic and the reads are relaxed. Plain
        // uint64_t would be a data race and therefore undefined —
        // "benign in practice" is exactly the reasoning §2 rejects and
        // C1 exists to disprove, and TSan would flag it against §6.6's
        // clean-arms claim.
        //
        // The mutex still provides all mutual exclusion and ordering;
        // the atomics only make this unlocked read well-defined. Relaxed
        // loads are plain ldr on ARM64, so the locked paths are
        // unchanged. A stale read costs one wasted iteration, never a
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
    // §4's spin budget. Default 8192, measured — and the number it
    // replaces is kept here because the way the first answer was wrong
    // is worth more than the answer.
    //
    // First attempt, 4 Sep: derive it from the ski-rental bound. Spinning
    // is worth doing only while it costs less than blocking, so spin for
    // exactly the cost of a park and wake and the worst case is twice
    // optimal. measure_condvar_wakeup measured park/wake at ~1296 ns and
    // a spin iteration at ~1.29 ns, giving 1004, rounded to 1000.
    //
    // That model was incomplete. It costs the *waiter* correctly and
    // ignores what blocking costs the *signaller*: when a consumer is
    // parked, the producer's notify_one becomes a __ulock_wake syscall on
    // the critical path of its own send schedule. In a queue whose
    // producer must never be delayed, that term dominates the one the
    // model optimised.
    //
    // The spin sweep measured it directly (results/spin_sweep_*.csv).
    // At 1M records/s, spin 1000 parked on 646,253 of 2,000,000 messages
    // and the producer's p99 lag was 3208 ns; spin 8192 parked 437 times
    // and p99 lag was 41 ns. A 78x reduction in producer lag, tracking a
    // 1479x reduction in parks. Consumer p99 latency fell with it, from
    // 9917 ns to 416 ns.
    //
    // 8192 iterations is ~10.6 us of spinning, which exceeds the
    // inter-arrival gap at every offered rate at or above ~95k/s — the
    // whole of B1's sweep. That is the justification: a bound covering
    // the sweep, not a fitted crossover. 65536 was also measured and
    // changes nothing (parks 8 vs 437 at 1M, p99 lag identical), so 8192
    // is on the flat part of the curve rather than at its edge.
    //
    // What this does NOT fix, and §4 should not pretend otherwise: above
    // ~2.5M/s the spin budget stops mattering entirely. At 5M/s, parks
    // fall from 69,674 to 3 across the same sweep and producer p99 lag
    // does not move (5891 vs 6358 ns). There the cost is the lock itself,
    // not the wait policy, and the baseline's ceiling is real.
    //
    // SpinCount stays a template parameter because 1000 remains a
    // reportable configuration, not dead code. A condvar queue that
    // blocks is what §3 describes as catastrophic on the tail, and
    // tuning it away is exactly what §4 requires — so both are run and
    // both are reported. Reporting only the tuned arm understates the
    // mechanism the project is about; reporting only the parking arm is
    // the strawman §4 forbids.
    static constexpr int kSpinCount = SpinCount;

    // Racy reads used only to decide whether to take the lock. See
    // wait_nonempty().
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

    // Diagnostics for §4's spin tuning. Producer-written and
    // consumer-written respectively, both only under the mutex, both
    // read only after join.
    std::uint64_t signals_ = 0;
    std::uint64_t parks_ = 0;
};
