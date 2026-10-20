/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once
#include "rocketnet_internal.h"

// Driver-owned staging generations. The caller holds its publication mutex
// for every operation. Producers write only the acquired buffer, after joining
// before publish; the worker retires only after copying its final packet.
// A full queue never overwrites a queued buffer: acquire returns -1, and
// reopen hands back the newest queued buffer unreset (never the front), whose
// packets stay, sent ahead of the ones staged into it next
// (rocketnet_server_socket_acquire_send_buffer, rocketnet_server_socket_linux.cpp).
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
    void publish()
    {
        rocketnet_assert(filling >= 0 && count < NumBuffers);
        queued[(head + count) % NumBuffers] = filling;
        ++count;
        filling = -1;
    }
    // The producer asks for storage while every generation is queued (two
    // flushes inside one worker latency). The newest queued generation is not
    // the front, so the worker has not started it: hand it back to the
    // producer, which stages after its packets until the next publish. -1
    // when only the front is queued. The caller tries acquire first.
    int reopen()
    {
        if (filling >= 0) return filling;
        if (count < 2) return -1;
        --count;
        return filling = queued[(head + count) % NumBuffers];
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
