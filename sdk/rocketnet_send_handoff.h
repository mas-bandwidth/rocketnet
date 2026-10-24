/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once
#include "rocketnet_internal.h"

// Transport-owned staging generations. The caller holds its publication mutex
// for every operation. Producers write only the acquired buffer, after joining
// before publish; the worker retires only after copying its final packet.
// tla/RNDriverSendStaging.tla Publish: retain producer storage until a replacement
// is free; a later flush retries after the worker retires a generation.
// reopen remains the defensive recovery for a preexisting fully queued state:
// it hands back the newest generation, never the front.
template<int NumBuffers> struct rocketnet_send_handoff_t
{
    bool free[size_t(NumBuffers)];
    int queued[size_t(NumBuffers)];
    int head = 0;
    int count = 0;
    int filling = -1;

    rocketnet_send_handoff_t()
    {
        static_assert(NumBuffers >= 2, "producer and consumer storage");
        for (int i = 0; i < NumBuffers; ++i) free[i] = true;
    }
    // tla/SendHandoff.tla Acquire: reserves a free generation as the filling one.
    // tla/SendHandoff.tla Finish: fills the next packet slot of the filling
    // generation, counted as the next packet for it.
    int acquire()
    {
        if (filling >= 0) return filling;
        for (int i = 0; i < NumBuffers; ++i)
            if (free[i]) { free[i] = false; return filling = i; }
        return -1;
    }
    // tla/SendHandoff.tla Publish: appends the filling generation to the queue.
    bool publish()
    {
        rocketnet_assert(filling >= 0 && count < NumBuffers);
        if (count == NumBuffers - 1) return false;
        queued[(head + count) % NumBuffers] = filling;
        ++count;
        filling = -1;
        return true;
    }
    // tla/SendHandoff.tla Reopen: reopen takes the newest queued generation, never
    // the front. The producer asks for storage while every generation is
    // queued (two flushes inside one worker latency). The newest queued
    // generation is not the front, so the worker has not started it: hand it
    // back to the producer, which stages after its packets until the next
    // publish. -1 when only the front is queued. The caller tries acquire
    // first.
    // tla/SendHandoff.tla StaleFinish: a finish through a pointer from before
    // the reopen, below finish_base, is refused.
    int reopen()
    {
        if (filling >= 0) return filling;
        if (count < 2) return -1;
        --count;
        return filling = queued[(head + count) % NumBuffers];
    }
    // tla/SendHandoff.tla TakeFront: the worker reads the front generation.
    int front() const { return count ? queued[head] : -1; }
    // tla/SendHandoff.tla Retire: frees the copied front and advances the queue.
    void retire()
    {
        rocketnet_assert(count > 0);
        free[queued[head]] = true;
        head = (head + 1) % NumBuffers;
        --count;
    }
};
