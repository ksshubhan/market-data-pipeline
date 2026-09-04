#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>


template <typename T, std::size_t Capacity>
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


    static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

private:
    // PROVISIONAL, and marked so deliberately (§4, decided 4 Sep).
    //
    // The mechanism cannot be wrong; only this constant can. It is tuned
    // in Step 12 once B1 shows the distribution of empty-queue intervals
    // under load. Choosing it now would mean writing a number with
    // nothing behind it, and §6.3 already set the house style against
    // that by deriving the 70 ns calibration threshold from a model
    // rather than picking it.
    //
    // Until that tuning happens the word "tuned" in §10's CV line is not
    // earned.
    static constexpr int kSpinCount = 200;

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
};