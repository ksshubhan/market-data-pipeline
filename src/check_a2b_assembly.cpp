#include "false_sharing.hpp"

#include <cstdint>


// A2b reports roughly 0.30 ns per release store per thread, about one
// store per cycle on this core. That is plausible for a store hitting L1
// in exclusive state, but fast enough that the loop must be shown to
// contain one stlr per iteration rather than something coalesced,
// unrolled into fewer stores, or sunk out of the loop entirely.
//
// The counter check in the harness is not sufficient on its own: a
// compiler that dropped intermediate stores would still leave the final
// value correct.
//
// noinline so the loop survives as a standalone symbol to disassemble.
// The three separations are instantiated because the loop body is the
// same code in each arm; if they differ, the arms are not comparable.

__attribute__((noinline))
void store_loop_64(SeparatedCounters<64>& counters, std::uint64_t count)
{
    separation_store_loop(counters.first, count);
}

__attribute__((noinline))
void store_loop_128(SeparatedCounters<128>& counters, std::uint64_t count)
{
    separation_store_loop(counters.first, count);
}

__attribute__((noinline))
void store_loop_256(SeparatedCounters<256>& counters, std::uint64_t count)
{
    separation_store_loop(counters.first, count);
}

__attribute__((noinline))
void store_loop_16(SeparatedCounters<16>& counters, std::uint64_t count)
{
    separation_store_loop(counters.first, count);
}


// A3b's loops carry an empty asm with a memory clobber, so they cannot
// be elided outright, and the harness checks the writer's final value.
// They are disassembled anyway for the same reason as above: the checks
// catch a loop that vanished, not one that was partially unrolled or
// vectorised into fewer, wider accesses. The three strides must also
// produce the same loop body, or the arms are not comparable.

__attribute__((noinline))
void slot_write_80(AlignedSlot<8>& slot, std::uint64_t count)
{
    slot_write_loop(slot, count);
}

__attribute__((noinline))
void slot_write_128(AlignedSlot<128>& slot, std::uint64_t count)
{
    slot_write_loop(slot, count);
}

__attribute__((noinline))
std::uint64_t slot_read_80(const AlignedSlot<8>& slot, std::uint64_t count)
{
    return slot_read_loop(slot, count);
}

__attribute__((noinline))
std::uint64_t slot_read_128(
    const AlignedSlot<128>& slot,
    std::uint64_t count
)
{
    return slot_read_loop(slot, count);
}


int main()
{
    SeparatedCounters<64> counters{};

    store_loop_64(counters, 4);

    if (counters.first.load(std::memory_order_relaxed) != 4) {
        return 1;
    }

    SlotPair<8> narrow_pair{};
    SlotPair<128> wide_pair{};

    AlignedSlot<8>& narrow = narrow_pair.slots[1];
    AlignedSlot<128>& wide = wide_pair.slots[1];

    slot_write_80(narrow, 4);
    slot_write_128(wide, 4);

    if (narrow.record.sequence != 3 || wide.record.sequence != 3) {
        return 1;
    }

    return (slot_read_80(narrow, 4) + slot_read_128(wide, 4)) == 0
        ? 1
        : 0;
}