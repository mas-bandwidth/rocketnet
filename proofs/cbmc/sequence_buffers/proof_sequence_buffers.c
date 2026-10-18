/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    ESBMC proofs for sequence-indexed replay, acknowledgment and message
    receive buffers. Each harness calls the actual sdk implementation on one
    arbitrary sequence and one arbitrary state satisfying the documented
    per-call invariants. No sdk implementation is mirrored here.
*/

#include "../esbmc_shim.h"

#include <stdint.h>
#include <stdlib.h>

#define NDEBUG
#include "rocketnet_replay.cpp"
#include "rocketnet_platform.h"

// The sequence-buffer readers use the platform atomics only to serialize a
// selected slot and publish a maximum. Model those external primitives as one
// sequential call; the sdk indexing and record writes remain unchanged.
static void proof_spin_lock_u64( volatile uint64_t * value )
{
    __ESBMC_assert( value != NULL, "spin lock receives a valid word" );
    __ESBMC_assume( *value == 0 );
    *value = 1;
}
static void proof_spin_unlock_u64( volatile uint64_t * value )
{
    __ESBMC_assert( value != NULL, "spin unlock receives a valid word" );
    *value = 0;
}
static void proof_atomic_max_u64( volatile uint64_t * value, uint64_t candidate )
{
    __ESBMC_assert( value != NULL, "atomic max receives a valid word" );
    if ( *value < candidate )
        *value = candidate;
}
static uint64_t proof_atomic_add_u64( volatile uint64_t * value, uint64_t add )
{
    __ESBMC_assert( value != NULL, "atomic add receives a valid word" );
    const uint64_t previous = *value;
    *value += add;
    return previous;
}
#define rocketnet_platform_spin_lock_u64 proof_spin_lock_u64
#define rocketnet_platform_spin_unlock_u64 proof_spin_unlock_u64
#define rocketnet_platform_atomic_max_u64 proof_atomic_max_u64
#define rocketnet_platform_atomic_add_u64 proof_atomic_add_u64
#include "rocketnet_ack.cpp"
#include "rocketnet_window_receive.cpp"

static void assume_endpoint_preconditions( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence )
{
    __ESBMC_assume( endpoint != NULL );
    __ESBMC_assume( sequence != 0 );
}

void harness_replay_already_received( void )
{
    rocketnet_replay_protection_t replay;
    uint64_t sequence = nondet_uint64();
    replay.highest_opened = nondet_uint64();
    for ( int i = 0; i < ROCKETNET_REPLAY_WINDOW_BITS / 64; ++i )
        replay.window[i] = nondet_uint64();
    __ESBMC_assume( sequence != 0 );

    const int result = rocketnet_replay_already_received( &replay, sequence );
    __ESBMC_assert( result == 0 || result == 1,
                    "replay lookup returns a boolean for every sequence" );
}

void harness_replay_advance( void )
{
    rocketnet_replay_protection_t replay;
    uint64_t sequence = nondet_uint64();
    replay.highest_opened = nondet_uint64();
    for ( int i = 0; i < ROCKETNET_REPLAY_WINDOW_BITS / 64; ++i )
        replay.window[i] = nondet_uint64();
    __ESBMC_assume( sequence != 0 );
    __ESBMC_assume( !rocketnet_replay_already_received( &replay, sequence ) );

    rocketnet_replay_advance( &replay, sequence );
    __ESBMC_assert( replay.highest_opened >= sequence,
                    "advance keeps the accepted sequence at or below the high-water mark" );
    __ESBMC_assert( rocketnet_replay_already_received( &replay, sequence ) == 1,
                    "advance marks the accepted sequence in the replay window" );
}

void harness_ack_record_received( void )
{
    rocketnet_ack_endpoint_t endpoint;
    uint64_t sequence = nondet_uint64();
    int wire_bytes = nondet_int();
    double time = 1000.0;
    assume_endpoint_preconditions( &endpoint, sequence );

    rocketnet_ack_endpoint_record_received( &endpoint, sequence, wire_bytes, time );
    const uint64_t slot = sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE;
    __ESBMC_assert( slot < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE,
                    "received sequence indexes inside the received ring" );
    __ESBMC_assert( endpoint.received[slot].sequence == sequence,
                    "received sequence is recorded in its selected slot" );
}

void harness_ack_record_sent_classified( void )
{
    rocketnet_ack_endpoint_t endpoint;
    uint64_t sequence = nondet_uint64();
    int wire_bytes = nondet_int();
    uint64_t packet_id = nondet_uint64();
    int has_packet_id = nondet_int();
    uint64_t aggregate_id = nondet_uint64();
    double time = 1000.0;
    assume_endpoint_preconditions( &endpoint, sequence );
    __ESBMC_assume( !( has_packet_id && aggregate_id != 0 ) );
    endpoint.sent_lock = 0;
    endpoint.highest_sequence = nondet_uint64();
    endpoint.largest_acked_sequence = nondet_uint64();
    const uint64_t selected_slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    uint64_t old_sequence = nondet_uint64();
    __ESBMC_assume( old_sequence == 0 ||
                    ( old_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE == selected_slot &&
                      old_sequence <= endpoint.highest_sequence ) );
    endpoint.sent[selected_slot].sequence = old_sequence;
    endpoint.sent[selected_slot].acked = nondet_uchar();
    endpoint.sent_held[selected_slot >> 6] = nondet_uint64();
    endpoint.sent_unacked[selected_slot >> 6] = nondet_uint64();

    const rocketnet_ack_sent_result_t result =
        rocketnet_ack_endpoint_record_sent_classified( &endpoint, sequence,
                                                       wire_bytes, packet_id,
                                                       has_packet_id, aggregate_id,
                                                       time );
    __ESBMC_assert( result >= ROCKETNET_ACK_SENT_RECORDED &&
                    result <= ROCKETNET_ACK_SENT_REFUSED_STALE,
                    "sent classification is one of the documented outcomes" );
    const uint64_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    __ESBMC_assert( slot < ROCKETNET_SENT_PACKET_BUFFER_SIZE,
                    "sent sequence indexes inside the sent ring" );
    __ESBMC_assert( ( slot >> 6 ) < ROCKETNET_SENT_BITMAP_WORDS,
                    "sent sequence indexes inside both sent bitmaps" );
}

void harness_ack_mark_keep_alive( void )
{
    rocketnet_ack_endpoint_t endpoint;
    uint64_t sequence = nondet_uint64();
    assume_endpoint_preconditions( &endpoint, sequence );
    endpoint.sent_lock = 0;
    endpoint.sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].sequence = nondet_uint64();

    rocketnet_ack_endpoint_mark_keep_alive( &endpoint, sequence );
    const uint64_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    __ESBMC_assert( slot < ROCKETNET_SENT_PACKET_BUFFER_SIZE,
                    "keep-alive sequence indexes inside the sent ring" );
}

static rn_region_t * symbolic_region( uint32_t window )
{
    rn_region_t * region = (rn_region_t *) malloc( sizeof( rn_region_t ) );
    __ESBMC_assume( region != NULL );
    region->window = window;
    region->counters = NULL;
    region->received_bits = (uint64_t *) malloc( sizeof( uint64_t ) * ( window / 64 ) );
    region->receive_descriptors =
        (rn_receive_descriptor_t *) malloc( sizeof( rn_receive_descriptor_t ) * window );
    __ESBMC_assume( region->received_bits != NULL );
    __ESBMC_assume( region->receive_descriptors != NULL );
    return region;
}

static uint32_t symbolic_window( void )
{
    uint32_t window = nondet_uint();
    __ESBMC_assume( window >= 64 && window <= ( 1u << 20 ) );
    __ESBMC_assume( ( window & ( window - 1 ) ) == 0 );
    return window;
}

void harness_window_receive_init( void )
{
    const uint32_t window = symbolic_window();
    rn_region_t * region = symbolic_region( window );
    rn_window_receive_t receive;
    uint64_t first_number = nondet_uint64();

    rn_window_receive_init( &receive, region, first_number );
    __ESBMC_assert( receive.region == region, "init retains the supplied region" );
    __ESBMC_assert( receive.R == first_number, "init retains the first message number" );
}

void harness_window_receive_accept( void )
{
    const uint32_t window = symbolic_window();
    rn_region_t * region = symbolic_region( window );
    rn_window_receive_t receive;
    rocketnet_message_record_t record;
    uint32_t slot;
    receive.region = region;
    receive.R = nondet_uint64();
    record.number = nondet_uint();
    receive.violation = ROCKETNET_MESSAGE_VIOLATION_NONE;

    const int result = rn_window_receive_accept( &receive, &record, &slot );
    __ESBMC_assert( result == RN_WINDOW_RECEIVE_ACCEPT ||
                    result == RN_WINDOW_RECEIVE_DUPLICATE ||
                    result == RN_WINDOW_RECEIVE_VIOLATION,
                    "accept returns a documented result" );
    __ESBMC_assert( slot == RN_DESCRIPTOR_NONE || slot < window,
                    "an accepted message slot is inside the configured window" );
}

void harness_window_receive_release( void )
{
    const uint32_t window = symbolic_window();
    rn_region_t * region = symbolic_region( window );
    rn_window_receive_t receive;
    uint64_t number = nondet_uint64();
    receive.region = region;

    rn_window_receive_release( &receive, number );
    const uint64_t slot = number & ( window - 1 );
    __ESBMC_assert( slot < window,
                    "released message number indexes inside the descriptor window" );
}
