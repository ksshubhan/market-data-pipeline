// spsc_ring_buffer.hpp: SpscRingBuffer, the bounded single-producer,
// single-consumer queue the project measures.
//
// try_push copies a T into the next slot and publishes it with a release
// store of the tail; try_pop reads the slot after an acquire load of the
// tail and frees it with a release store of the head. Both are wait-free:
// each call is a fixed sequence of loads and stores, with no
// compare-and-swap and no retry loop. When the queue is full, try_push
// returns false and counts a rejection; what happens to the record is the
// caller's decision.
//
// Template parameters select the variants the harnesses compare: block
// alignment, memory order, and whether each side caches the other's
// index.
//
// Related: harness_a.cpp (queue microbenchmarks), harness_b.cpp (against
// mutex_queue.hpp), harness_c.cpp (stress test), c1_relaxed_publication.cpp
// (the broken-ordering control), measure_parse_cost.cpp,
// check_spsc_assembly.cpp, test_spsc_ring_buffer.cpp.

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
// is also two loads. Neither can loop.
enum class SpscIndexCaching {
    Cached,
    Uncached
};

// Alignment defaults to 128 bytes, the line size M2 reports
// (hw.cachelinesize). The measured coherence granule is 64 bytes, so 128
// keeps the two threads' write sets apart with room to spare.
template <
    typename T,
    std::size_t Capacity,
    std::size_t Alignment = 128,
    SpscMemoryOrder Order = SpscMemoryOrder::AcquireRelease,
    SpscIndexCaching Caching = SpscIndexCaching::Cached
>
class SpscRingBuffer {
    // Needed on its own: 0 passes the power-of-two test below, since
    // 0 & (0 - 1) is 0.
    static_assert(Capacity > 0);
    static_assert(
        (Capacity & (Capacity - 1)) == 0,
        "SpscRingBuffer capacity must be a power of two"
    );

    static_assert(
        Alignment != 0 && (Alignment & (Alignment - 1)) == 0,
        "Alignment must be a power of two"
    );

    // Slots are overwritten by plain assignment and never constructed or
    // destroyed per push, so a push cannot allocate or throw.
    static_assert(std::is_trivially_copyable_v<T>);

    // The wait-free claim needs real atomic loads and stores on the
    // indices, not a lock the library supplies.
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    // alignas on a member holds only if the object itself is placed at
    // that alignment, which not every allocation path guarantees, and a
    // misplaced block would build false sharing into every measurement.
    // So it is checked against this instantiation's own Alignment. abort
    // rather than assert, because both CMake presets define NDEBUG.
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

        // The counters only increase and are compared by difference,
        // never with <, so the test is exact across the uint64_t wrap and
        // full means Capacity slots in use, with none sacrificed.
        if constexpr (Caching == SpscIndexCaching::Cached) {
            if (tail - producer_.cached_head == Capacity) {
                // One re-read, not a loop. Waiting here for space would
                // make the producer's progress depend on the consumer.
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

        // Publishes the slot: a consumer whose acquire load sees tail + 1
        // also sees the value written above.
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

        // Frees the slot. Release keeps the read above from moving after
        // this store, so the producer cannot see the slot as free and
        // overwrite it while it is still being read.
        consumer_.head.store(head + 1, kPublishStoreOrder);

        return true;
    }

    // A plain counter: only the producer writes it, and it is read on the
    // producer thread or after both threads are joined.
    std::uint64_t full_rejections() const noexcept
    {
        return producer_.full_rejections;
    }

    static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

private:
    // SeqCst makes every access seq_cst, std::atomic's default, so the
    // acquire/release arm can be compared with the naive one.

    // A thread's own index is written only by that thread, so reading it
    // back needs no ordering.
    static constexpr std::memory_order kOwnLoadOrder =
        Order == SpscMemoryOrder::SeqCst
            ? std::memory_order_seq_cst
            : std::memory_order_relaxed;

    // Pairs with the consumer's release of head, so the consumer's read
    // of a slot happens before the producer reuses it.
    static constexpr std::memory_order kProducerCrossLoadOrder =
        Order == SpscMemoryOrder::SeqCst
            ? std::memory_order_seq_cst
            : std::memory_order_acquire;

    // Pairs with the producer's release of tail, so the payload write
    // happens before the consumer's read. C1 weakens this load alone.
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

    // Each thread's whole write set gets its own aligned block, and the
    // slots start on a fresh one, so neither thread's writes share a line
    // with the other's or with slot 0. full_rejections is producer-written
    // and so lives in the producer's block.
    //
    // cached_head and cached_tail stay in the uncached arm even though
    // nothing reads them there. Removing them would change the layout, so
    // the A4 arms would differ in two things at once; harness_a asserts
    // their sizeof equal.
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