/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once
#include "rocketnet_internal.h"

// Driver-owned staging generations. The caller holds its publication mutex
// for every operation. Producers write only the acquired buffer, after joining
// before publish; the worker retires only after copying its final packet.
// RNDriverSendStaging.Publish: retain producer storage until a replacement
// is free; a later flush retries after the worker retires a generation.
template<int NumBuffers> struct rocketnet_send_handoff_t
{
    bool free[NumBuffers];
    int queued[NumBuffers];
    int head = 0;
    int count = 0;
    int filling = -1;

    rocketnet_send_handoff_t()
    {
        static_assert(NumBuffers >= 2, "producer and consumer storage");
        for (int i = 0; i < NumBuffers; ++i) free[i] = true;
    }
    int acquire()
    {
        if (filling >= 0) return filling;
        for (int i = 0; i < NumBuffers; ++i)
            if (free[i]) { free[i] = false; return filling = i; }
        return -1;
    }
    bool publish()
    {
        rocketnet_assert(filling >= 0 && count < NumBuffers);
        if (count == NumBuffers - 1) return false;
        queued[(head + count) % NumBuffers] = filling;
        ++count;
        filling = -1;
        return true;
    }
    int front() const { return count ? queued[head] : -1; }
    void retire()
    {
        rocketnet_assert(count > 0);
        free[queued[head]] = true;
        head = (head + 1) % NumBuffers;
        --count;
    }
};
