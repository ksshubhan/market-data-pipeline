#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <type_traits>


template <
    typename T,
    std::size_t Capacity,
    std::size_t Alignment = 128
>
class SpscRingBuffer {
    static_assert(Capacity > 0);
    static_assert(
        (Capacity & (Capacity - 1)) == 0,
        "SpscRingBuffer capacity must be a power of two"
    );

    static_assert(
        Alignment != 0 && (Alignment & (Alignment - 1)) == 0,
        "Alignment must be a power of two"
    );

    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    SpscRingBuffer()
    {
        const auto is_aligned = [](const void* address) {
            return reinterpret_cast<std::uintptr_t>(address) % Alignment == 0;
        };

        if (!is_aligned(&producer_) ||
            !is_aligned(&consumer_) ||
            !is_aligned(buffer_.data())) {
            std::abort();
        }
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    bool try_push(const T& value)
    {
        const std::uint64_t tail =
            producer_.tail.load(std::memory_order_relaxed);

        if (tail - producer_.cached_head == Capacity) {
            producer_.cached_head =
                consumer_.head.load(std::memory_order_acquire);

            if (tail - producer_.cached_head == Capacity) {
                ++producer_.full_rejections;
                return false;
            }
        }

        buffer_[tail & kMask] = value;

        producer_.tail.store(
            tail + 1,
            std::memory_order_release
        );

        return true;
    }

    bool try_pop(T& value)
    {
        const std::uint64_t head =
            consumer_.head.load(std::memory_order_relaxed);

        if (consumer_.cached_tail - head == 0) {
            consumer_.cached_tail =
                producer_.tail.load(std::memory_order_acquire);

            if (consumer_.cached_tail - head == 0) {
                return false;
            }
        }

        value = buffer_[head & kMask];

        consumer_.head.store(
            head + 1,
            std::memory_order_release
        );

        return true;
    }

    std::uint64_t full_rejections() const noexcept
    {
        return producer_.full_rejections;
    }

    static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    struct alignas(Alignment) ProducerState {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t cached_head{0};
        std::uint64_t full_rejections{0};
    };

    struct alignas(Alignment) ConsumerState {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t cached_tail{0};
    };

    ProducerState producer_{};
    ConsumerState consumer_{};

    alignas(Alignment) std::array<T, Capacity> buffer_{};
};