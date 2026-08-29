#pragma once

#include <array>
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

        const std::uint64_t size = tail_ - head_;

        if (size == Capacity) {
            ++full_rejections_;
            return false;
        }

        const bool was_empty = (size == 0);

        buffer_[tail_ & kMask] = value;
        ++tail_;

        lock.unlock();

        if (was_empty) {
            not_empty_.notify_one();
        }

        return true;
    }

    bool try_pop(T& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tail_ - head_ == 0) {
            return false;
        }

        value = buffer_[head_ & kMask];
        ++head_;

        return true;
    }

    // Baseline-only blocking entry point.
    // Returns false only when the queue is closed and empty.
    bool wait_nonempty()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        not_empty_.wait(lock, [this] {
            return tail_ - head_ != 0 || closed_;
        });

        return tail_ - head_ != 0;
    }

    // Wakes a blocked consumer when the producer is finished.
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
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
    static constexpr std::uint64_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};

    // Monotonic counters. Protected by mutex_.
    std::uint64_t head_{0};
    std::uint64_t tail_{0};

    // Incremented by the producer whenever try_push() rejects on full.
    std::uint64_t full_rejections_{0};

    // Set once the producer has finished.
    bool closed_{false};

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
};