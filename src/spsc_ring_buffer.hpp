#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <type_traits>

enum class SpscMemoryOrder {
    AcquireRelease,
    SeqCst,

    // C1 only: intentionally invalid C++.
    // Removes the consumer acquire from producer publication.
    C1BrokenRelaxedPublication
};

// A4. The producer caches its last-seen consumer index and only re-reads
// the shared one when the cached value says the queue is full; the
// consumer does the same with the producer's index. Uncached re-reads the
// opposite index on every call instead, turning a rare cross-core acquire
// load into one per operation.
//
// Both variants are wait-free: the uncached path is in fact shorter, two
// loads and a compare with no branch back. The cached path's worst case
// is also two loads (§2). Neither can loop.
enum class SpscIndexCaching {
    Cached,
    Uncached
};

template <
    typename T,
    std::size_t Capacity,
    std::size_t Alignment = 128,
    SpscMemoryOrder Order = SpscMemoryOrder::AcquireRelease,
    SpscIndexCaching Caching = SpscIndexCaching::Cached
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
            producer_.tail.load(kOwnLoadOrder);

        if constexpr (Caching == SpscIndexCaching::Cached) {
            if (tail - producer_.cached_head == Capacity) {
                producer_.cached_head =
                    consumer_.head.load(kProducerCrossLoadOrder);

                if (tail - producer_.cached_head == Capacity) {
                    ++producer_.full_rejections;
                    return false;
                }
            }
        } else {
            // A4 uncached arm: the consumer's index is re-read on every
            // push, so the producer takes a cross-core acquire load per
            // operation rather than only when it believes it is full.
            const std::uint64_t head =
                consumer_.head.load(kProducerCrossLoadOrder);

            if (tail - head == Capacity) {
                ++producer_.full_rejections;
                return false;
            }
        }

        buffer_[tail & kMask] = value;

        producer_.tail.store(tail + 1, kPublishStoreOrder);

        return true;
    }

    bool try_pop(T& value)
    {
        const std::uint64_t head =
            consumer_.head.load(kOwnLoadOrder);

        if constexpr (Caching == SpscIndexCaching::Cached) {
            if (consumer_.cached_tail - head == 0) {
                consumer_.cached_tail =
                    producer_.tail.load(kConsumerCrossLoadOrder);

                if (consumer_.cached_tail - head == 0) {
                    return false;
                }
            }
        } else {
            const std::uint64_t tail =
                producer_.tail.load(kConsumerCrossLoadOrder);

            if (tail - head == 0) {
                return false;
            }
        }

        value = buffer_[head & kMask];

        consumer_.head.store(head + 1, kPublishStoreOrder);

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
    static constexpr std::memory_order kOwnLoadOrder =
        Order == SpscMemoryOrder::SeqCst
            ? std::memory_order_seq_cst
            : std::memory_order_relaxed;

    static constexpr std::memory_order kProducerCrossLoadOrder =
        Order == SpscMemoryOrder::SeqCst
            ? std::memory_order_seq_cst
            : std::memory_order_acquire;

    static constexpr std::memory_order kConsumerCrossLoadOrder =
        Order == SpscMemoryOrder::SeqCst
            ? std::memory_order_seq_cst
            : Order == SpscMemoryOrder::C1BrokenRelaxedPublication
                ? std::memory_order_relaxed
                : std::memory_order_acquire;

    static constexpr std::memory_order kPublishStoreOrder =
        Order == SpscMemoryOrder::SeqCst
            ? std::memory_order_seq_cst
            : std::memory_order_release;

    static constexpr std::uint64_t kMask = Capacity - 1;

    // cached_head and cached_tail are retained in the uncached arm even
    // though nothing reads them. Removing them would shrink the control
    // block and change the layout, so the A4 arms would differ in two
    // variables at once. sizeof is asserted equal across arms in the
    // harness for the same reason.
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