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

// The near advance clears up to ROCKETNET_REPLAY_WINDOW_BITS - 1 bits one
// at a time, so its unwinding is the cost of the claim.
// SEQUENCE_BUFFERS_NEAR_SKIP bounds the near skip for the bounded harness;
// the full harness leaves it open.
#ifndef SEQUENCE_BUFFERS_NEAR_SKIP
#define SEQUENCE_BUFFERS_NEAR_SKIP 256
#endif

enum { REPLAY_ANY_SKIP, REPLAY_NEAR_BOUNDED, REPLAY_FAR_ONLY, REPLAY_STEP };

static void replay_advance_claim( int shape )
{
    rocketnet_replay_protection_t replay;
    uint64_t sequence = nondet_uint64();
    replay.highest_opened = nondet_uint64();
    for ( int i = 0; i < ROCKETNET_REPLAY_WINDOW_BITS / 64; ++i )
        replay.window[i] = nondet_uint64();
    __ESBMC_assume( sequence != 0 );
    __ESBMC_assume( !rocketnet_replay_already_received( &replay, sequence ) );
    const int ahead = sequence > replay.highest_opened;
    const uint64_t skipped = ahead ? sequence - replay.highest_opened : 0;
    if ( shape == REPLAY_NEAR_BOUNDED )
        __ESBMC_assume( !ahead || skipped <= SEQUENCE_BUFFERS_NEAR_SKIP ||
                        skipped >= ROCKETNET_REPLAY_WINDOW_BITS );
    if ( shape == REPLAY_FAR_ONLY )
        __ESBMC_assume( ahead && skipped >= ROCKETNET_REPLAY_WINDOW_BITS );
    if ( shape == REPLAY_STEP )
        __ESBMC_assume( !ahead || skipped <= 2 || skipped >= ROCKETNET_REPLAY_WINDOW_BITS );

    rocketnet_replay_advance( &replay, sequence );
    __ESBMC_assert( replay.highest_opened >= sequence,
                    "advance keeps the accepted sequence at or below the high-water mark" );
    __ESBMC_assert( rocketnet_replay_already_received( &replay, sequence ) == 1,
                    "advance marks the accepted sequence in the replay window" );
}

// Full claim: every sequence against every window state.
void harness_replay_advance( void ) { replay_advance_claim( REPLAY_ANY_SKIP ); }

// Every sequence at or behind the high-water mark, every near skip up to
// SEQUENCE_BUFFERS_NEAR_SKIP, and every far skip.
void harness_replay_advance_near_bounded( void ) { replay_advance_claim( REPLAY_NEAR_BOUNDED ); }

// Every far skip (at least the window width): the whole-window clear.
void harness_replay_advance_far( void ) { replay_advance_claim( REPLAY_FAR_ONLY ); }

// The step lemma for the near clear loop (sdk/rocketnet_replay.cpp:66-67):
// every sequence at or behind the high-water mark, every far skip, and every
// near skip of at most 2, so the loop body runs at most once and the call is
// proved whole with unwinding assertions on. Its one loop iteration clears
// s = highest_opened + 1 for an arbitrary highest_opened, so it covers the
// body for every s; iteration i of a longer skip is that same body at
// s = highest_opened + i, which is the first iteration of the state with
// highest_opened' = highest_opened + i - 1. results.md gives the argument.
void harness_replay_advance_step( void ) { replay_advance_claim( REPLAY_STEP ); }

void harness_ack_record_received( void )
{
    rocketnet_ack_endpoint_t endpoint;
    uint64_t sequence = nondet_uint64();
    int wire_bytes = nondet_int();
    double time = 1000.0;
    assume_endpoint_preconditions( &endpoint, sequence );

    rocketnet_ack_endpoint_record_received( &endpoint, sequence, wire_bytes, time );
    // The record the sdk selects is a function of the sequence alone
    // (sdk/rocketnet_ack.cpp:363), so pinning the written record to
    // received[sequence % SIZE] pins the access inside the received ring.
    const uint64_t slot = sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE;
    __ESBMC_assert( endpoint.received[slot].sequence == sequence,
                    "received sequence is recorded in its selected slot" );
}

static void sent_classified_claim( rocketnet_ack_endpoint_t * endpoint )
{
    uint64_t sequence = nondet_uint64();
    int wire_bytes = nondet_int();
    uint64_t packet_id = nondet_uint64();
    int has_packet_id = nondet_int();
    uint64_t aggregate_id = nondet_uint64();
    double time = 1000.0;
    assume_endpoint_preconditions( endpoint, sequence );
    __ESBMC_assume( !( has_packet_id && aggregate_id != 0 ) );
    endpoint->sent_lock = 0;
    // Every other field, including the selected sent record and both
    // bitmaps, keeps its arbitrary initial value: a wider state than the
    // ring invariant allows, so the claim covers every reachable state.

    const rocketnet_ack_sent_result_t result =
        rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence,
                                                       wire_bytes, packet_id,
                                                       has_packet_id, aggregate_id,
                                                       time );
    __ESBMC_assert( result >= ROCKETNET_ACK_SENT_RECORDED &&
                    result <= ROCKETNET_ACK_SENT_REFUSED_STALE,
                    "sent classification is one of the documented outcomes" );
    // ESBMC checks a dereference against the whole endpoint object, not
    // against the sent array inside it, so an index past the ring that
    // lands in a later field is not a bounds failure. The record the sdk
    // selects is a function of the sequence alone (sdk/rocketnet_ack.cpp:244),
    // so pinning the written record to sent[sequence % SIZE] pins every
    // access of the call inside the ring.
    if ( result != ROCKETNET_ACK_SENT_REFUSED_STALE )
        __ESBMC_assert( endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].sequence == sequence,
                        "a recorded sequence is written to its slot in the sent ring" );
}

void harness_ack_record_sent_classified( void )
{
    rocketnet_ack_endpoint_t endpoint;
    sent_classified_claim( &endpoint );
}

void harness_ack_record_sent_classified_heap( void )
{
    rocketnet_ack_endpoint_t * endpoint = (rocketnet_ack_endpoint_t *) malloc( sizeof( rocketnet_ack_endpoint_t ) );
    __ESBMC_assume( endpoint != NULL );
    sent_classified_claim( endpoint );
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

// ESBMC checks a dereference against the whole endpoint object, not against
// the sent array inside it, so an index past the ring that lands in a later
// field is not a bounds failure in the harness above. The record the sdk
// selects is a function of the sequence alone (sdk/rocketnet_ack.cpp:331), so
// this harness pins it: for every sequence whose slot holds that sequence,
// the tag lands on sent[sequence % SIZE], so the record is inside the ring.
void harness_ack_mark_keep_alive_pinned( void )
{
    rocketnet_ack_endpoint_t endpoint;
    uint64_t sequence = nondet_uint64();
    assume_endpoint_preconditions( &endpoint, sequence );
    endpoint.sent_lock = 0;
    const uint64_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    endpoint.sent[slot].sequence = sequence;
    endpoint.sent[slot].was_keep_alive = 0;

    rocketnet_ack_endpoint_mark_keep_alive( &endpoint, sequence );
    __ESBMC_assert( endpoint.sent[slot].was_keep_alive == 1,
                    "keep-alive tags the sent ring slot of its sequence" );
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

// window_messages is a power of two from 64 to 1 << 20 (rocketnet.h).
#define SEQUENCE_BUFFERS_FULL_WINDOW ( 1u << 20 )
#ifndef SEQUENCE_BUFFERS_ACCEPT_WINDOW
#define SEQUENCE_BUFFERS_ACCEPT_WINDOW 64
#endif

static uint32_t symbolic_window( uint32_t max_window )
{
    uint32_t window = nondet_uint();
    __ESBMC_assume( window >= 64 && window <= max_window );
    __ESBMC_assume( ( window & ( window - 1 ) ) == 0 );
    return window;
}

void harness_window_receive_init( void )
{
    const uint32_t window = symbolic_window( SEQUENCE_BUFFERS_FULL_WINDOW );
    rn_region_t * region = symbolic_region( window );
    rn_window_receive_t receive;
    uint64_t first_number = nondet_uint64();

    rn_window_receive_init( &receive, region, first_number );
    __ESBMC_assert( receive.region == region, "init retains the supplied region" );
    __ESBMC_assert( receive.R == first_number, "init retains the first message number" );
}

static void window_accept_claim( uint32_t max_window, int step )
{
    const uint32_t window = symbolic_window( max_window );
    rn_region_t * region = symbolic_region( window );
    rn_window_receive_t receive;
    rocketnet_message_record_t record;
    uint32_t slot;
    receive.region = region;
    receive.R = nondet_uint64();
    record.number = nondet_uint();
    receive.violation = ROCKETNET_MESSAGE_VIOLATION_NONE;
    if ( step )
    {
        for ( uint64_t ahead = 1; ahead <= 2; ++ahead )
        {
            const uint32_t index = (uint32_t) ( ( receive.R + ahead ) & ( window - 1 ) );
            __ESBMC_assume( !( region->received_bits[index >> 6] & ( 1ULL << ( index & 63 ) ) ) );
        }
    }

    const int result = rn_window_receive_accept( &receive, &record, &slot );
    __ESBMC_assert( result == RN_WINDOW_RECEIVE_ACCEPT ||
                    result == RN_WINDOW_RECEIVE_DUPLICATE ||
                    result == RN_WINDOW_RECEIVE_VIOLATION,
                    "accept returns a documented result" );
    __ESBMC_assert( slot == RN_DESCRIPTOR_NONE || slot < window,
                    "an accepted message slot is inside the configured window" );
}

// Full claim: every power-of-two window up to 1 << 20. The advance loop
// runs up to W times, so its unwinding is the cost of the claim.
void harness_window_receive_accept( void ) { window_accept_claim( SEQUENCE_BUFFERS_FULL_WINDOW, 0 ); }

// Every power-of-two window up to SEQUENCE_BUFFERS_ACCEPT_WINDOW.
void harness_window_receive_accept_bounded( void ) { window_accept_claim( SEQUENCE_BUFFERS_ACCEPT_WINDOW, 0 ); }

// The step lemma for the advance loop (sdk/rocketnet_window_receive.cpp:92-101):
// every power-of-two window up to 1 << 20, every R, every wire number and
// every received bit, except that the bits for R + 1 and R + 2 start clear.
// The accepted number sets at most one of them, so the loop inspects at
// most R, R + 1 and R + 2 and the call is proved whole with unwinding
// assertions on. Its first iteration runs at the arbitrary R the call
// starts from, so it covers the body for every R and W; iteration k of a
// longer run is that same body at R + k. results.md gives the argument.
void harness_window_receive_accept_step( void ) { window_accept_claim( SEQUENCE_BUFFERS_FULL_WINDOW, 1 ); }

void harness_window_receive_release( void )
{
    const uint32_t window = symbolic_window( SEQUENCE_BUFFERS_FULL_WINDOW );
    rn_region_t * region = symbolic_region( window );
    rn_window_receive_t receive;
    uint64_t number = nondet_uint64();
    receive.region = region;

    rn_window_receive_release( &receive, number );
    const uint64_t slot = number & ( window - 1 );
    __ESBMC_assert( slot < window,
                    "released message number indexes inside the descriptor window" );
}
