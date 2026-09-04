#pragma once

#include "record.hpp"

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


// ---------------------------------------------------------------------
// A3b: slot-level false sharing
// ---------------------------------------------------------------------
//
// §6.5's A3 asks whether payload false sharing bites when the queue is
// near-empty — the producer writing slot n while the consumer is still
// reading slot n-1. A3b asks it without the queue, for the same reason
// A2b did: in harness A the throughput number is dominated by
// producer/consumer imbalance, so a slot-layout difference and a
// scheduling difference are indistinguishable.
//
// Padding is a property of the ring slot, not of Record: Record stays 80
// bytes in every arm and the slot wrapper is what is over-aligned, so
// sizeof(AlignedSlot<8>) is 80, <128> is 128 and <256> is 256.
//
// Both loops copy a whole Record, because that is what the queue does —
// the committed SPSC disassembly shows try_push moving all 80 bytes as
// five quadword accesses. An earlier version of this benchmark assigned
// three fields instead, and the disassembly showed it touching only
// bytes 0-7 and 40-55; with an 80-byte stride the writer's lines and the
// reader's lines then did not overlap at all and the experiment measured
// nothing.
//
// Geometry with the 64-byte coherence granule measured in A2b, given a
// line-aligned base:
//
//   stride  80: reader spans bytes   0-79  (lines 0,1)
//               writer spans bytes  80-159 (lines 1,2)  -> shares line 1
//   stride 128: reader spans bytes   0-127 (lines 0,1)
//               writer spans bytes 128-255 (lines 2,3)  -> no sharing
//
// There is deliberately no same-slot positive control. Two threads
// accessing one non-atomic Record concurrently is a data race and
// therefore undefined, and §6.5 already refuses to report numbers from
// undefined programs. A2b is the control: same machine, same loop shape,
// 3.56x when the accesses share a line.
template <std::size_t Alignment>
struct alignas(Alignment) AlignedSlot {
    Record record;
};


// The pair is always 256-aligned so the first slot starts on a line
// boundary in every arm and the geometry above is deterministic. Without
// this, AlignedSlot<8> would leave the array at an arbitrary offset
// modulo the line size and whether the slots shared a line would depend
// on stack layout.
template <std::size_t Alignment>
struct alignas(256) SlotPair {
    AlignedSlot<Alignment> slots[2];
};


// The writer touches only slots[1] and the reader only slots[0], so the
// two threads never access the same object and the program is race-free.
// All contention is at the cache line.
//
// Both loops carry an empty asm with a memory clobber. Without it the
// compiler may hoist the read out of the loop or sink all but the last
// write, since from a single thread's view nothing else touches the
// memory. Taking the address in the read loop also forces the whole
// 80-byte copy to be materialised rather than narrowed to the fields the
// sink happens to use. The clobber costs the same in every arm.
template <std::size_t Alignment>
inline void slot_write_loop(
    AlignedSlot<Alignment>& slot,
    std::uint64_t count
) noexcept
{
    Record value{};

    for (std::uint64_t i = 0; i < count; ++i) {
        value.sequence = i;

        slot.record = value;

        asm volatile("" :: "r"(&slot) : "memory");
    }
}


template <std::size_t Alignment>
inline std::uint64_t slot_read_loop(
    const AlignedSlot<Alignment>& slot,
    std::uint64_t count
) noexcept
{
    std::uint64_t sink = 0;

    for (std::uint64_t i = 0; i < count; ++i) {
        Record copy = slot.record;

        asm volatile("" :: "r"(&copy) : "memory");

        sink += copy.sequence;
        sink += static_cast<std::uint64_t>(copy.capture.ask_qty);
    }

    return sink;
}