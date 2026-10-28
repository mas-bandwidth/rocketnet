/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SEND_HANDOFF_H
#define ROCKETNET_SDK_ROCKETNET_SEND_HANDOFF_H

#include "rocketnet_internal.h"
#include <atomic>

// Transport-owned staging generations. The caller holds its publication mutex
// for every operation. Producers write only the acquired buffer, after joining
// before publish; the worker retires only after copying its final packet.
// tla/RNDriverSendStaging.tla Publish: retain producer storage until a replacement
// is free; a later flush retries after the worker retires a generation.
// reopen remains the defensive recovery for a preexisting fully queued state:
// it hands back the newest generation, never the front.
template <int NumBuffers> struct rocketnet_send_handoff_t
{
    bool free[size_t( NumBuffers )];
    int queued[size_t( NumBuffers )];
    int head = 0;
    int count = 0;
    int filling = -1;

    rocketnet_send_handoff_t()
    {
        static_assert( NumBuffers >= 2, "producer and consumer storage" );
        for ( int i = 0; i < NumBuffers; ++i )
            free[i] = true;
    }
    // tla/SendHandoff.tla Acquire: reserves a free generation as the filling one.
    int acquire()
    {
        if ( filling >= 0 )
            return filling;
        for ( int i = 0; i < NumBuffers; ++i )
            if ( free[i] )
            {
                free[i] = false;
                return filling = i;
            }
        return -1;
    }
    // tla/SendHandoff.tla Publish: appends the filling generation to the queue.
    bool publish()
    {
        rocketnet_assert( filling >= 0 && count < NumBuffers );
        // tla/SendHandoff.tla Publish needs a filling generation: the model
        // enables it only with one, so a publish without one is refused here
        // instead of queueing -1.
        if ( filling < 0 )
            return false;
        if ( count == NumBuffers - 1 )
            return false;
        queued[( head + count ) % NumBuffers] = filling;
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
    int reopen()
    {
        if ( filling >= 0 )
            return filling;
        if ( count < 2 )
            return -1;
        --count;
        return filling = queued[( head + count ) % NumBuffers];
    }
    // tla/SendHandoff.tla TakeFront: the worker reads the front generation.
    int front() const { return count ? queued[head] : -1; }
    // tla/SendHandoff.tla Retire: frees the copied front and advances the queue.
    void retire()
    {
        rocketnet_assert( count > 0 );
        // tla/SendHandoff.tla Retire needs a queued generation: the model
        // enables it only with one, so a retire on an empty queue is refused
        // here instead of freeing a stale generation.
        if ( count == 0 )
            return;
        free[queued[head]] = true;
        head = ( head + 1 ) % NumBuffers;
        --count;
    }
};

// Claim one staging slot for a packet: the producer counter names the index,
// and a claim past the limit (or a counter that wrapped negative) is refused
// and counted, so the flush names the queue and the count once per flush
// instead of every refused start logging a line of its own.
inline int rocketnet_xdp_send_claim( std::atomic<int> & num_packets, int limit, std::atomic<uint64_t> & refused )
{
    const int index = num_packets.fetch_add( 1 );
    if ( index < 0 || index >= limit )
    {
        refused.fetch_add( 1, std::memory_order_relaxed );
        return -1;
    }

    return index;
}

#endif // #ifndef ROCKETNET_SDK_ROCKETNET_SEND_HANDOFF_H
