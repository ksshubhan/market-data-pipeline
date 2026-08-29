#include "record.hpp"
#include "spsc_ring_buffer.hpp"

using Queue = SpscRingBuffer<Record, 1024>;

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

int main()
{
    Queue queue;

    Record input{};
    Record output{};

    input.sequence = 1;

    if (!push_once(queue, input)) {
        return 1;
    }

    if (!pop_once(queue, output)) {
        return 1;
    }

    return output.sequence == 1 ? 0 : 1;
}