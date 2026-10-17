/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#include <atomic>
#include <stdint.h>
#include "rocketnet.h"

// The two send rings' state machine (docs/CLIENT-IO.md). Frames fill one ring
// while the other is held by the I/O path; a ring is reused only after SENT.

enum { ROCKETNET_CLIENT_RING_FILLING = 0, ROCKETNET_CLIENT_RING_HANDED_OFF = 1,
       ROCKETNET_CLIENT_RING_SENDING = 2, ROCKETNET_CLIENT_RING_SENT = 3 };
enum { ROCKETNET_CLIENT_RINGS_HANDED_OFF = 0, ROCKETNET_CLIENT_RINGS_EMPTY = 1, ROCKETNET_CLIENT_RINGS_LATE = 2 };
struct rocketnet_client_send_rings_t
{
    std::atomic<int> state[2];
    uint64_t committed[2];      // written by the main thread before the release store of HANDED_OFF
    int fill;                   // main thread only: the ring game threads fill
};

// RNClientIo initial; main thread.
inline void rocketnet_client_send_rings_init( rocketnet_client_send_rings_t * r )
{
    r->state[0].store( ROCKETNET_CLIENT_RING_FILLING, std::memory_order_relaxed );
    r->state[1].store( ROCKETNET_CLIENT_RING_FILLING, std::memory_order_relaxed );
    r->committed[0] = 0;
    r->committed[1] = 0;
    r->fill = 0;
}

// RNClientIo Reopen; main thread.
inline bool rocketnet_client_send_rings_reopen( rocketnet_client_send_rings_t * r, int ring )
{
    if ( r->state[ring].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_RING_SENT )
    {
        r->committed[ring] = 0;
        r->state[ring].store( ROCKETNET_CLIENT_RING_FILLING, std::memory_order_relaxed );
        return true;
    }
    return false;
}

// RNClientIo HandOff; main thread.
inline int rocketnet_client_send_rings_hand_off( rocketnet_client_send_rings_t * r, uint64_t committed )
{
    if ( committed == 0 )
        return ROCKETNET_CLIENT_RINGS_EMPTY;
    int other = r->fill ^ 1;
    if ( r->state[other].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_RING_SENT )
    {
        r->committed[other] = 0;
        r->state[other].store( ROCKETNET_CLIENT_RING_FILLING, std::memory_order_relaxed );
    }
    if ( r->state[other].load( std::memory_order_acquire ) != ROCKETNET_CLIENT_RING_FILLING )
        return ROCKETNET_CLIENT_RINGS_LATE;
    r->committed[r->fill] = committed;
    r->state[r->fill].store( ROCKETNET_CLIENT_RING_HANDED_OFF, std::memory_order_release );
    r->fill = other;
    return ROCKETNET_CLIENT_RINGS_HANDED_OFF;
}

// RNClientIo IoSend (first); I/O thread.
inline int rocketnet_client_send_rings_take( rocketnet_client_send_rings_t * r )
{
    for ( int ring = 0; ring < 2; ring++ )
    {
        int expected = ROCKETNET_CLIENT_RING_HANDED_OFF;
        if ( r->state[ring].compare_exchange_strong( expected, ROCKETNET_CLIENT_RING_SENDING,
                                                     std::memory_order_acquire ) )
            return ring;
    }
    return -1;
}

// RNClientIo IoFinishRing; I/O thread.
inline void rocketnet_client_send_rings_finish( rocketnet_client_send_rings_t * r, int ring )
{
    rocketnet_assert( r->state[ring].load( std::memory_order_relaxed ) == ROCKETNET_CLIENT_RING_SENDING );
    r->state[ring].store( ROCKETNET_CLIENT_RING_SENT, std::memory_order_release );
}

// In-flight query; either thread.
inline bool rocketnet_client_send_rings_in_flight( const rocketnet_client_send_rings_t * r )
{
    return r->state[0].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_RING_HANDED_OFF
        || r->state[0].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_RING_SENDING
        || r->state[1].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_RING_HANDED_OFF
        || r->state[1].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_RING_SENDING;
}
