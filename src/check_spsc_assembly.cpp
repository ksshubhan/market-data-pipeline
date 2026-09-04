#include "record.hpp"
#include "spsc_ring_buffer.hpp"

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