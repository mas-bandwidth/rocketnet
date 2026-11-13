/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_RECEIVE_PREFILTER_H
#define ROCKETNET_SDK_ROCKETNET_RECEIVE_PREFILTER_H

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"
#include "rocketnet_address.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"

#include <atomic>
#if ROCKETNET_DEV
#include <string>
#include <stdio.h>
#include <string.h>
#endif // #if ROCKETNET_DEV

// The worker-side prefilter: the stateless first rungs of the server's
// receive ladder, run by a transport's own receive worker before a datagram
// takes a slot in the receive bank, so garbage cannot evict a connected
// player's packets.
//
// It is pure. It reads the datagram and the server's packet filter value and
// nothing else. The filter value is fixed for the server's life: it is the
// config's packet_filter, which the transport receives at create in
// rocketnet_server_transport_io_config_t.transport->packet_filter. A
// transport copies those ROCKETNET_FILTER_VALUE_BYTES bytes into its own
// state at create and passes them to every call. No published copy, no lock, no
// main-thread state: any number of workers may call it at once.
//
// A datagram it accepts still runs the whole ladder on main, which repeats
// these checks; the worker's checks are a strict prefix of main's, so a
// datagram refused here is one main would refuse on the same rung, one
// accepted here is never refused by those rungs on main, and a refused one
// is counted exactly as main would have counted it
// (rocketnet_receive_drop_counters; the unit test
// test_unit_receive_prefilter checks the two against each other).
//
// What it refuses, in main's order (sdk/rocketnet_server_protocol.cpp,
// rocketnet_server_protocol_process_received):
//   1. the prefilter header: too short, too large, wrong epoch, wrong value;
//   2. a reserved type (9 and up);
//   3. a type only a server sends (denied, challenge, cookie);
//   4. a server-inbound type whose size its type forbids (the request's exact
//      size, the response's exact size, the session types' wire floor).
// What it cannot refuse, by design: anything that needs a key or a table
// (signatures, tokens, replays, unknown sources). Those still reach main.

enum rocketnet_receive_drop_t
{
    ROCKETNET_RECEIVE_ACCEPT = 0,

    // returned by rocketnet_receive_prefilter
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

    // never returned by the prefilter: counted by a transport for a datagram
    // it received but will never hand to main because it is stopping (a
    // receive that completed after stop began, or one left in a receive bank
    // main will not take). Feeds ROCKETNET_COUNTER_RECEIVE_DROPPED_STOPPED.
    ROCKETNET_RECEIVE_DROP_STOPPED,

    ROCKETNET_RECEIVE_NUM_DROPS
};

// Returns ROCKETNET_RECEIVE_ACCEPT or the one reason main would refuse the
// datagram on its stateless rungs. packet_bytes is the datagram's original
// length (a truncated datagram keeps its length, as the driver interface
// requires), and packet_data holds at least min(packet_bytes,
// ROCKETNET_PREFILTER_BYTES) readable bytes. Reads no other state.
static inline enum rocketnet_receive_drop_t rocketnet_receive_prefilter( const uint8_t * packet_data, int packet_bytes,
                                                                         const uint8_t * packet_filter )
{
    const int packet_type = rocketnet_prefilter_check( packet_data, packet_bytes, packet_filter );
    switch ( packet_type )
    {
        case ROCKETNET_PREFILTER_TOO_SHORT:   return ROCKETNET_RECEIVE_DROP_TOO_SHORT;
        case ROCKETNET_PREFILTER_TOO_LARGE:   return ROCKETNET_RECEIVE_DROP_TOO_LARGE;
        case ROCKETNET_PREFILTER_WRONG_EPOCH: return ROCKETNET_RECEIVE_DROP_WRONG_EPOCH;
        case ROCKETNET_PREFILTER_WRONG_VALUE: return ROCKETNET_RECEIVE_DROP_WRONG_VALUE;
        default:                              break;
    }

    if ( packet_type >= ROCKETNET_FIRST_RESERVED_PACKET_TYPE )
    {
        return ROCKETNET_RECEIVE_DROP_RESERVED_TYPE;
    }

    switch ( packet_type )
    {
        case ROCKETNET_CONNECTION_REQUEST_PACKET:
            return packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES ? ROCKETNET_RECEIVE_ACCEPT : ROCKETNET_RECEIVE_DROP_REQUEST_WRONG_SIZE;
        case ROCKETNET_CONNECTION_DENIED_PACKET:
        case ROCKETNET_CONNECTION_CHALLENGE_PACKET:
        case ROCKETNET_CONNECTION_COOKIE_PACKET:
            return ROCKETNET_RECEIVE_DROP_WRONG_DIRECTION;
        case ROCKETNET_CONNECTION_RESPONSE_PACKET:
            return packet_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES ? ROCKETNET_RECEIVE_ACCEPT : ROCKETNET_RECEIVE_DROP_RESPONSE_WRONG_SIZE;
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET:
            return packet_bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES ? ROCKETNET_RECEIVE_ACCEPT : ROCKETNET_RECEIVE_DROP_KEEP_ALIVE_WRONG_SIZE;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:
            return packet_bytes >= ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES ? ROCKETNET_RECEIVE_ACCEPT : ROCKETNET_RECEIVE_DROP_PAYLOAD_WRONG_SIZE;
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET:
            return packet_bytes >= ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES ? ROCKETNET_RECEIVE_ACCEPT : ROCKETNET_RECEIVE_DROP_DISCONNECT_WRONG_SIZE;
        case ROCKETNET_CONNECTION_ACK_PACKET:
            return packet_bytes >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES ? ROCKETNET_RECEIVE_ACCEPT : ROCKETNET_RECEIVE_DROP_ACK_WRONG_SIZE;
        default:
            break;
    }

    // every type below the reserved range has a case above
    rocketnet_assert( 0 );
    return ROCKETNET_RECEIVE_DROP_RESERVED_TYPE;
}

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
        case ROCKETNET_RECEIVE_DROP_STOPPED:
            counters[0] = ROCKETNET_COUNTER_RECEIVE_DROPPED_STOPPED;
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

#if ROCKETNET_DEV
// Development library only, nothing in the production library: the first few
// datagrams the prefilter refused (the server's own ladder and a transport's
// worker prefilter alike), kept so a functional "clean run" case that finds a
// refusal counter moved can name the datagrams (source, length and first 16
// bytes) in its report (tests/counters_expect.h). Written from any thread,
// read by the report; the first ROCKETNET_RECEIVE_DROP_SAMPLES only.

#define ROCKETNET_RECEIVE_DROP_SAMPLES 4

struct rocketnet_receive_drop_sample_t
{
    int reason;
    char from[64];
    int bytes;
    uint8_t first[16];
    std::atomic<bool> ready;
};

inline rocketnet_receive_drop_sample_t * rocketnet_receive_drop_samples()
{
    static rocketnet_receive_drop_sample_t samples[ROCKETNET_RECEIVE_DROP_SAMPLES];
    return samples;
}

inline void rocketnet_receive_drop_sample_record( int reason, const struct rocketnet_address_t * from, const uint8_t * data, int bytes )
{
    static std::atomic<int> next{ 0 };
    const int at = next.fetch_add( 1 );
    if ( at >= ROCKETNET_RECEIVE_DROP_SAMPLES )
    {
        return;
    }

    rocketnet_receive_drop_sample_t * sample = &rocketnet_receive_drop_samples()[at];
    sample->reason = reason;
    sample->from[0] = '\0';
    if ( from )
    {
        rocketnet_address_to_string( from, sample->from );
    }
    sample->bytes = bytes;
    memset( sample->first, 0, sizeof( sample->first ) );
    if ( data && bytes > 0 )
    {
        memcpy( sample->first, data, (size_t) ( bytes < 16 ? bytes : 16 ) );
    }
    sample->ready.store( true, std::memory_order_release );
}

// the samples as text lines for a report, empty when none was taken

inline std::string rocketnet_receive_drop_samples_text()
{
    std::string text;
    for ( int i = 0; i < ROCKETNET_RECEIVE_DROP_SAMPLES; i++ )
    {
        const rocketnet_receive_drop_sample_t * sample = &rocketnet_receive_drop_samples()[i];
        if ( !sample->ready.load( std::memory_order_acquire ) )
        {
            continue;
        }
        char line[160];
        int at = snprintf( line, sizeof( line ), "  refused datagram %d: reason %d from %s length %d first16 ", i, sample->reason, sample->from, sample->bytes );
        for ( int b = 0; b < 16 && at > 0 && at + 3 < (int) sizeof( line ); b++ )
        {
            at += snprintf( line + at, sizeof( line ) - (size_t) at, "%02x", sample->first[b] );
        }
        text += line;
        text += "\n";
    }
    return text;
}
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_RECEIVE_PREFILTER_H
