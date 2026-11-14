/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_RECEIVE_HANDOFF_H
#define ROCKETNET_SDK_ROCKETNET_RECEIVE_HANDOFF_H

#include "rocketnet_internal.h"

#include <stdint.h>

// The receive handoff between a transport's worker thread, which banks
// datagrams, and the main thread, which reads them in its receive call: the
// gso, xdp, rio and iocp servers' queues and the client's I/O thread and
// legacy receive worker (proofs/tla/RNReceiveHandoff.tla, bound to the gso
// server's code by the trace check tests/tla_trace_receive_handoff.cpp, to
// the xdp server's by tests/tla_trace_receive_handoff_xdp.cpp and to the
// client I/O core's by tests/tla_trace_client_receive_handoff.cpp). Three
// preallocated buffers; only indices
// and counts move, and only through the functions below, which the caller
// runs with its handoff mutex held. The worker acquires the mutex; main only
// ever tries it.
//
// Main reads the buffer the worker is filling, up to the count the worker last
// committed, so a datagram the worker banked and committed before main's call
// began is returned by that call when the call took the mutex, began with no
// datagrams left from a cut call and its capacity did not cut it
// (DeliveredByNextCall): then a receive adds no update of latency. The worker appends to its buffer outside the mutex and
// commits its count at the end of each pass; main reads below the committed
// count outside the mutex. Neither ever touches the other's slots: the worker
// writes only at or past the committed count, main reads only below it, and
// the mutex orders the worker's slot writes before main's reads.
//
// The buffers' roles:
//
//   worker  the buffer the worker appends to.
//   main    the buffer main reads, up to main_limit. While main and the worker
//           share a buffer, main == worker.
//   tail    the buffer the worker left that main still reads (tail_cursor up
//           to tail_end, the worker's final count) or has finished and still
//           views until its next call.
//   free    the buffer nobody holds: there is one exactly while main and the
//           worker share a buffer, otherwise -1.
//
// The worker moves to the free buffer at a commit when main has started on
// its buffer (main == worker). Main, at the start of a call, when the worker
// has left main's buffer and main has finished its tail, frees the tail,
// makes its own buffer the tail and starts on the worker's buffer. A worker
// whose buffer is full and cannot move keeps it and stops reading (the
// queue's bounded backpressure, unchanged).
//
// A datagram the worker banked but had not yet committed when main's call
// took the mutex is returned by main's next call under the same conditions:
// the window is one worker pass. A busy mutex leaves main reading what it
// already holds, up to its earlier limit; the first later call that takes the
// mutex catches up, within its capacity and after any backlog.

#define ROCKETNET_RECEIVE_HANDOFF_BUFFERS 3

struct rocketnet_receive_handoff_t
{
    // under the handoff mutex

    int worker;
    int main;
    int tail;
    int free;
    uint32_t committed[ROCKETNET_RECEIVE_HANDOFF_BUFFERS];

    // main's alone

    uint32_t main_cursor;
    uint32_t main_limit;
    uint32_t tail_cursor;
    uint32_t tail_end;
};

// proofs/tla/RNReceiveHandoff.tla Init: main and the worker share buffer 0,
// buffer 1 is free and buffer 2 is main's finished tail.
inline void rocketnet_receive_handoff_init( struct rocketnet_receive_handoff_t * handoff )
{
    handoff->worker = 0;
    handoff->main = 0;
    handoff->tail = 2;
    handoff->free = 1;
    for ( int i = 0; i < ROCKETNET_RECEIVE_HANDOFF_BUFFERS; i++ )
    {
        handoff->committed[i] = 0;
    }
    handoff->main_cursor = 0;
    handoff->main_limit = 0;
    handoff->tail_cursor = 0;
    handoff->tail_end = 0;
}

// the initial handoff, for a member initializer
inline struct rocketnet_receive_handoff_t rocketnet_receive_handoff_initial()
{
    struct rocketnet_receive_handoff_t handoff;
    rocketnet_receive_handoff_init( &handoff );
    return handoff;
}

// proofs/tla/RNReceiveHandoff.tla Commit: the worker, at the end of a pass,
// makes its count visible and, when main has started on its buffer, moves to
// the free one. Returns the buffer the worker fills next when it moved (empty:
// main reset it before freeing it), or -1 when it keeps its buffer. Mutex held.
inline int rocketnet_receive_handoff_commit( struct rocketnet_receive_handoff_t * handoff, uint32_t count )
{
    rocketnet_assert( count >= handoff->committed[handoff->worker] );

    handoff->committed[handoff->worker] = count;

    if ( handoff->main != handoff->worker )
    {
        return -1;
    }

    rocketnet_assert( handoff->free >= 0 );

    handoff->worker = handoff->free;
    handoff->free = -1;
    handoff->committed[handoff->worker] = 0;

    return handoff->worker;
}

// proofs/tla/RNReceiveHandoff.tla Call: main, at the start of a receive call
// whose try-acquire succeeded. When the worker has left main's buffer and main
// has finished its tail, the tail is freed (the caller reset it before), main's
// buffer becomes the tail with the worker's final count, and main starts on the
// worker's buffer. Then main's limit is its buffer's committed count. Returns
// the buffer that just became the tail, whose count and the worker's tallies
// in it are now final, or -1. Mutex held.
inline int rocketnet_receive_handoff_begin( struct rocketnet_receive_handoff_t * handoff )
{
    int left = -1;

    if ( handoff->main != handoff->worker && handoff->tail_cursor >= handoff->tail_end )
    {
        rocketnet_assert( handoff->free < 0 );

        handoff->free = handoff->tail;
        handoff->tail = handoff->main;
        handoff->tail_end = handoff->committed[handoff->main];
        handoff->tail_cursor = handoff->main_cursor;
        handoff->main = handoff->worker;
        handoff->main_cursor = 0;
        left = handoff->tail;
    }

    handoff->main_limit = handoff->committed[handoff->main];

    return left;
}

// main: the finished tail, which main may reset before its next begin (its
// view has expired at the start of the call). Main's alone.
inline bool rocketnet_receive_handoff_tail_finished( const struct rocketnet_receive_handoff_t * handoff )
{
    return handoff->tail_cursor >= handoff->tail_end;
}

#endif // ROCKETNET_SDK_ROCKETNET_RECEIVE_HANDOFF_H
