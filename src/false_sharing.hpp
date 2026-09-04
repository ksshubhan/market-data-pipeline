#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>


// A2b primitives, factored out of harness_a.cpp so that
// check_a2b_assembly.cpp disassembles the code the harness actually runs
// rather than a second copy of it. A disassembly of a duplicate proves
// nothing about the binary that produced the numbers.

// Two atomics a fixed distance apart. The struct is always 256-aligned so
// the first counter sits on a line boundary in every arm; only the
// distance to the second varies.
template <std::size_t Separation>
struct alignas(256) SeparatedCounters {
    static_assert(
        Separation >= 16,
        "Separation must leave room for the first counter"
    );

    std::atomic<std::uint64_t> first{0};
    std::uint8_t padding[Separation - sizeof(std::atomic<std::uint64_t>)]{};
    std::atomic<std::uint64_t> second{0};
};


// The measured loop. Release stores, not relaxed, because that is what
// the queue does: the producer publishes tail_ with a release store on
// every push and the consumer publishes head_ the same way on every pop.
// Those unconditional stores are what would false-share; the conditional
// cross-loads are rare by design.
//
// The final value is checked by the caller, so a compiler that merged or
// elided stores would still have to leave the last one — which is why the
// disassembly check exists rather than the counter check alone.
inline void separation_store_loop(
    std::atomic<std::uint64_t>& counter,
    std::uint64_t count
) noexcept
{
    for (std::uint64_t i = 0; i < count; ++i) {
        counter.store(i + 1, std::memory_order_release);
    }
}