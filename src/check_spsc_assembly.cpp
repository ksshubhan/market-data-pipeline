// A program built to be disassembled, not to measure anything. It wraps
// SpscRingBuffer's try_push and try_pop, for both the cached and the
// uncached index arms, in standalone functions so each appears in the
// binary under its own symbol.
//
// tools/make_spsc_evidence.py reads an llvm-objdump listing of this
// binary, written by the README's command, and checks that the four
// functions hold eight acquire or release instructions in total,
// that no line matches its read-modify-write pattern, and where each
// function's acquire load sits relative to its first branch. It
// writes evidence/spsc_arm64_disassembly_20260908.txt. Rename a
// function only together with that script. Running the program is a
// sanity check: one record through each arm, exit 0 if it arrives
// intact. See ARCHITECTURE.md.

#include "record.hpp"
#include "spsc_ring_buffer.hpp"

// The default configuration: 128-byte alignment, acquire/release
// ordering, cached indices.
using Queue = SpscRingBuffer<Record, 1024>;

// A4: the uncached arm should show an unconditional cross-core acquire
// load of the opposite index on every call, where the cached arm only
// reaches that load when it believes the queue is full or empty.
using UncachedQueue = SpscRingBuffer<
    Record,
    1024,
    128,
    SpscMemoryOrder::AcquireRelease,
    SpscIndexCaching::Uncached
>;

// noinline so each call survives as a standalone symbol to disassemble.
__attribute__((noinline))
bool push_once(Queue& queue, const Record& record)
{
    return queue.try_push(record);
}

__attribute__((noinline))
bool pop_once(Queue& queue, Record& record)
{
    return queue.try_pop(record);
}

__attribute__((noinline))
bool push_once_uncached(UncachedQueue& queue, const Record& record)
{
    return queue.try_push(record);
}

__attribute__((noinline))
bool pop_once_uncached(UncachedQueue& queue, Record& record)
{
    return queue.try_pop(record);
}

int main()
{
    Queue queue;
    UncachedQueue uncached;

    Record input{};
    Record output{};

    input.sequence = 1;

    if (!push_once(queue, input)) {
        return 1;
    }

    if (!pop_once(queue, output)) {
        return 1;
    }

    if (output.sequence != 1) {
        return 1;
    }

    Record uncached_out{};

    if (!push_once_uncached(uncached, input)) {
        return 1;
    }

    if (!pop_once_uncached(uncached, uncached_out)) {
        return 1;
    }

    return uncached_out.sequence == 1 ? 0 : 1;
}