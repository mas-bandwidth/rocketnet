/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_BUFFER_HANDOFF_H
#define ROCKETNET_SDK_ROCKETNET_BUFFER_HANDOFF_H

#include "rocketnet_internal.h"

// N preallocated buffers crossing between one producer thread and one consumer
// thread (docs/decisions/BUFFER-HANDOFF-DECISION-20261021.md, proofs/tla/RNBufferHandoff.tla).
// Only buffer indices move, and only through the functions below, which the
// caller runs with its handoff mutex held. Each is a few integer moves: no
// copy, no clear, no transport call and no wake happen under the mutex.
//
// Every buffer is owned by exactly one of: the producer (the index it holds),
// the ready queue, the consumer (the index it holds), or the free queue. The
// producer writes only the buffer it holds and the consumer reads only the
// buffer it holds, so neither touches a buffer the other may be using.
//
// The game thread side only ever try-acquires the mutex: a busy mutex defers
// the exchange to its next call, it never waits. The worker side may acquire.
//
// The same structure runs both directions of the client transport: send
// (producer: the game thread filling payload rings; consumer: the I/O thread
// sending them) and receive (producer: the I/O thread reading datagrams;
// consumer: update processing them). The portable server's TX banks
// (sdk/rocketnet_driver_builtin.cpp) are the same pattern.

template <int NumBuffers> struct rocketnet_buffer_handoff_t
{
    int free_index[size_t( NumBuffers )];
    int free_head;
    int free_count;
    int ready_index[size_t( NumBuffers )];
    int ready_head;
    int ready_count;

    // the producer holds a filled buffer it could not publish because no
    // buffer was free; the consumer's next release reports it, so the caller
    // wakes the producer after it drops the mutex

    bool owed;
};

// every buffer but the producer's first one starts free

template <int NumBuffers> inline void rocketnet_buffer_handoff_init( rocketnet_buffer_handoff_t<NumBuffers> * handoff, int producer )
{
    static_assert( NumBuffers >= 3, "a producer buffer, a consumer buffer and one in between" );
    rocketnet_assert( producer >= 0 && producer < NumBuffers );

    handoff->free_head = 0;
    handoff->free_count = 0;

    for ( int i = 0; i < NumBuffers; i++ )
    {
        if ( i != producer )
            handoff->free_index[handoff->free_count++] = i;
    }

    handoff->ready_head = 0;
    handoff->ready_count = 0;
    handoff->owed = false;
}

// RNBufferHandoff ProducerExchange: the producer gives its buffer to the ready queue
// and takes a free one in the same exchange. Returns the producer's next
// buffer, or -1 when none is free or max_ready buffers are already ready: the
// producer then keeps its buffer, adds to it while it has room, and the
// publish is owed. A max_ready of 1 is the triple buffer: the consumer always
// takes everything published since its last take in one buffer. Mutex held.

template <int NumBuffers> inline int rocketnet_buffer_handoff_publish( rocketnet_buffer_handoff_t<NumBuffers> * handoff, int producer, int max_ready )
{
    rocketnet_assert( producer >= 0 && producer < NumBuffers );
    rocketnet_assert( max_ready >= 1 && max_ready < NumBuffers );

    if ( handoff->free_count == 0 || handoff->ready_count >= max_ready )
    {
        handoff->owed = true;
        return -1;
    }

    rocketnet_assert( handoff->ready_count < NumBuffers );

    handoff->ready_index[( handoff->ready_head + handoff->ready_count ) % NumBuffers] = producer;
    handoff->ready_count++;

    const int next = handoff->free_index[handoff->free_head];

    handoff->free_head = ( handoff->free_head + 1 ) % NumBuffers;
    handoff->free_count--;
    handoff->owed = false;

    return next;
}

// RNBufferHandoff ConsumerExchange, its take: the consumer takes the oldest ready buffer, or -1
// when none is ready. Mutex held.

template <int NumBuffers> inline int rocketnet_buffer_handoff_take( rocketnet_buffer_handoff_t<NumBuffers> * handoff )
{
    if ( handoff->ready_count == 0 )
        return -1;

    const int index = handoff->ready_index[handoff->ready_head];

    handoff->ready_head = ( handoff->ready_head + 1 ) % NumBuffers;
    handoff->ready_count--;

    return index;
}

// RNBufferHandoff ConsumerExchange, its release: the consumer returns a buffer it has finished with
// to the free queue. Mutex held.

template <int NumBuffers> inline void rocketnet_buffer_handoff_release( rocketnet_buffer_handoff_t<NumBuffers> * handoff, int index )
{
    rocketnet_assert( index >= 0 && index < NumBuffers );
    rocketnet_assert( handoff->free_count < NumBuffers );
    handoff->free_index[( handoff->free_head + handoff->free_count ) % NumBuffers] = index;
    handoff->free_count++;
}

// RNBufferHandoff ConsumerExchange, its owed flag: the consumer reads and clears the owed flag at the end
// of its exchange. True when the producer held back a publish: the caller
// wakes the producer after dropping the mutex, never under it. Mutex held.

template <int NumBuffers> inline bool rocketnet_buffer_handoff_owed( rocketnet_buffer_handoff_t<NumBuffers> * handoff )
{
    const bool owed = handoff->owed;
    handoff->owed = false;
    return owed;
}

#endif // ROCKETNET_SDK_ROCKETNET_BUFFER_HANDOFF_H
