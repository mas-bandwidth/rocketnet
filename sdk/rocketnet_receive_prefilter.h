/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_RECEIVE_PREFILTER_H
#define ROCKETNET_SDK_ROCKETNET_RECEIVE_PREFILTER_H

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"

#include <atomic>

// The receive drop reasons a server transport counts itself, before a
// datagram reaches main: each reason names the counters main's ladder adds
// for the same datagram (rocketnet_receive_drop_counters), so a drop made on a
// transport's own thread reads exactly as main would have counted it. A
// transport's workers count into rocketnet_receive_drop_tally_t tallies, one
// writer each, and its receive_drop_totals operation (sdk/rocketnet_driver.h)
// sums them for the server to fold into its counters.

enum rocketnet_receive_drop_t
{
    ROCKETNET_RECEIVE_ACCEPT = 0,

    // the stateless rungs of main's ladder, in its order
    // (sdk/rocketnet_server_protocol.cpp, rocketnet_server_protocol_process_received)
    ROCKETNET_RECEIVE_DROP_TOO_SHORT,
    ROCKETNET_RECEIVE_DROP_TOO_LARGE,
    ROCKETNET_RECEIVE_DROP_WRONG_EPOCH,
    ROCKETNET_RECEIVE_DROP_WRONG_VALUE,
    ROCKETNET_RECEIVE_DROP_RESERVED_TYPE,
    ROCKETNET_RECEIVE_DROP_WRONG_DIRECTION,
    ROCKETNET_RECEIVE_DROP_REQUEST_WRONG_SIZE,
    ROCKETNET_RECEIVE_DROP_RESPONSE_WRONG_SIZE,
    ROCKETNET_RECEIVE_DROP_KEEP_ALIVE_WRONG_SIZE,
    ROCKETNET_RECEIVE_DROP_PAYLOAD_WRONG_SIZE,
    ROCKETNET_RECEIVE_DROP_DISCONNECT_WRONG_SIZE,
    ROCKETNET_RECEIVE_DROP_ACK_WRONG_SIZE,

    // never returned by the prefilter: counted by the transport itself when
    // a datagram that passed (or was never offered to) the prefilter finds
    // no room in its receive bank, buffer or posted slice. Feeds
    // ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL.
    ROCKETNET_RECEIVE_DROP_BANK_FULL,

    // never returned by the prefilter: counted by the transport itself for a
    // receive it completed but cannot read as a datagram (an error
    // completion, a source address it cannot read). Feeds
    // ROCKETNET_COUNTER_RECEIVE_FAILED.
    ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED,

    ROCKETNET_RECEIVE_NUM_DROPS
};

// The server counters one drop adds, written to counters[] in order; returns
// how many (at most ROCKETNET_RECEIVE_DROP_MAX_COUNTERS). They are exactly
// what main's ladder adds for the same datagram: PACKETS_RECEIVED, then the
// type's RECEIVED where main counts one before its size check, then the
// reason. BANK_FULL is RECEIVE_DROPPED_FULL alone and RECEIVE_FAILED is
// RECEIVE_FAILED alone: the datagram was never received by the protocol.
// ACCEPT adds nothing.
#define ROCKETNET_RECEIVE_DROP_MAX_COUNTERS 3

static inline int rocketnet_receive_drop_counters( enum rocketnet_receive_drop_t drop, int * counters )
{
    switch ( drop )
    {
        case ROCKETNET_RECEIVE_ACCEPT:
            return 0;
        case ROCKETNET_RECEIVE_DROP_BANK_FULL:
            counters[0] = ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL;
            return 1;
        case ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED:
            counters[0] = ROCKETNET_COUNTER_RECEIVE_FAILED;
            return 1;
        default:
            break;
    }

    counters[0] = ROCKETNET_COUNTER_PACKETS_RECEIVED;

    switch ( drop )
    {
        case ROCKETNET_RECEIVE_DROP_TOO_SHORT:       counters[1] = ROCKETNET_COUNTER_PREFILTER_TOO_SHORT; return 2;
        case ROCKETNET_RECEIVE_DROP_TOO_LARGE:       counters[1] = ROCKETNET_COUNTER_PREFILTER_TOO_LARGE; return 2;
        case ROCKETNET_RECEIVE_DROP_WRONG_EPOCH:     counters[1] = ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH; return 2;
        case ROCKETNET_RECEIVE_DROP_WRONG_VALUE:     counters[1] = ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE; return 2;
        case ROCKETNET_RECEIVE_DROP_RESERVED_TYPE:   counters[1] = ROCKETNET_COUNTER_RESERVED_TYPE; return 2;
        case ROCKETNET_RECEIVE_DROP_WRONG_DIRECTION: counters[1] = ROCKETNET_COUNTER_WRONG_DIRECTION; return 2;
        case ROCKETNET_RECEIVE_DROP_REQUEST_WRONG_SIZE:
            counters[1] = ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED;
            counters[2] = ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE;
            return 3;
        case ROCKETNET_RECEIVE_DROP_RESPONSE_WRONG_SIZE:
            counters[1] = ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED;
            counters[2] = ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE;
            return 3;
        case ROCKETNET_RECEIVE_DROP_KEEP_ALIVE_WRONG_SIZE:
            counters[1] = ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED;
            counters[2] = ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE;
            return 3;
        case ROCKETNET_RECEIVE_DROP_PAYLOAD_WRONG_SIZE:
            counters[1] = ROCKETNET_COUNTER_PAYLOAD_RECEIVED;
            counters[2] = ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE;
            return 3;
        case ROCKETNET_RECEIVE_DROP_DISCONNECT_WRONG_SIZE:
            counters[1] = ROCKETNET_COUNTER_DISCONNECT_RECEIVED;
            counters[2] = ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE;
            return 3;
        case ROCKETNET_RECEIVE_DROP_ACK_WRONG_SIZE:
            counters[1] = ROCKETNET_COUNTER_ACK_RECEIVED;
            counters[2] = ROCKETNET_COUNTER_ACK_WRONG_SIZE;
            return 3;
        default:
            break;
    }

    rocketnet_assert( 0 );
    return 1;
}

// One worker's drop tally. Exactly one thread writes a tally (the worker
// that owns it); any thread may read it. The writer adds with a relaxed load
// and store, no read-modify-write, because it is the only writer; readers
// load relaxed. A transport keeps one tally per receive worker (or one per
// worker and one more for a path main itself runs), each on its own cache
// lines, and sums them in its receive_drop_totals operation.
// The values fill whole cache lines, the slots past the last reason held at
// zero, so the compiler adds no padding (MSVC C4324 refuses implicit
// padding from an alignment).
#define ROCKETNET_RECEIVE_DROP_TALLY_SLOTS ( ( ( ROCKETNET_RECEIVE_NUM_DROPS + 7 ) / 8 ) * 8 )

struct alignas( 64 ) rocketnet_receive_drop_tally_t
{
    std::atomic<uint64_t> values[ROCKETNET_RECEIVE_DROP_TALLY_SLOTS];
};

static_assert( sizeof( std::atomic<uint64_t> ) == sizeof( uint64_t ), "eight values fill one cache line" );
static_assert( sizeof( struct rocketnet_receive_drop_tally_t ) % 64 == 0, "a tally is whole cache lines" );

static inline void rocketnet_receive_drop_tally_reset( struct rocketnet_receive_drop_tally_t * tally )
{
    for ( int i = 0; i < ROCKETNET_RECEIVE_DROP_TALLY_SLOTS; ++i )
    {
        tally->values[i].store( 0, std::memory_order_relaxed );
    }
}

// the owning worker only; ACCEPT is not a drop and is not counted
static inline void rocketnet_receive_drop_tally_count( struct rocketnet_receive_drop_tally_t * tally, enum rocketnet_receive_drop_t drop,
                                                       uint64_t datagrams = 1 )
{
    rocketnet_assert( drop > ROCKETNET_RECEIVE_ACCEPT && drop < ROCKETNET_RECEIVE_NUM_DROPS );
    std::atomic<uint64_t> & value = tally->values[drop];
    value.store( value.load( std::memory_order_relaxed ) + datagrams, std::memory_order_relaxed );
}

// any thread: adds each of num_tallies tallies into totals[ROCKETNET_RECEIVE_NUM_DROPS]
static inline void rocketnet_receive_drop_tally_sum( const struct rocketnet_receive_drop_tally_t * tallies, int num_tallies, uint64_t * totals )
{
    for ( int t = 0; t < num_tallies; ++t )
    {
        for ( int i = 0; i < ROCKETNET_RECEIVE_NUM_DROPS; ++i )
        {
            totals[i] += tallies[t].values[i].load( std::memory_order_relaxed );
        }
    }
}

#endif // ROCKETNET_SDK_ROCKETNET_RECEIVE_PREFILTER_H
