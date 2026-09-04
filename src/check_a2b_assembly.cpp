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


int main()
{
    SeparatedCounters<64> counters{};

    store_loop_64(counters, 4);

    return counters.first.load(std::memory_order_relaxed) == 4 ? 0 : 1;
}