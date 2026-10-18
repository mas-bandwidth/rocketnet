/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the fragment readers on the real sdk code. The readers
    are sdk/rocketnet_fragment.cpp, included below (never copied). This
    harness supersedes proof_fragment_bounds.c, which copied the reassembly
    function; the copy's divergences from the real function are named in
    results.md.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions

    Bounds. PROOF_TYPE_BUF is the symbolic fragment buffer size (header plus
    fragment bytes); PROOF_MAX_PAYLOAD is the upper bound on max_payload_bytes.
    The full production bounds are the header plus ROCKETNET_MAX_RECEIVE_
    FRAGMENT_BYTES bytes and a maximum payload of
    ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES; the instance bounds below are what
    this run proves, and results.md records the gap.
*/

#ifndef PROOF_TYPE_BUF
#define PROOF_TYPE_BUF ( ROCKETNET_FRAGMENT_HEADER_BYTES + 8 )
#endif

#ifndef PROOF_MAX_PAYLOAD
#define PROOF_MAX_PAYLOAD 24
#endif

#define PROOF_STAGING ( PROOF_TYPE_BUF - ROCKETNET_FRAGMENT_HEADER_BYTES )
#define PROOF_ARENA ( PROOF_MAX_PAYLOAD + PROOF_STAGING )

#include "rocketnet_fragment.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned nondet_uint( void ) { return 0; }
static uint8_t nondet_uint8( void ) { return 0; }
#else
int nondet_int( void );
unsigned nondet_uint( void );
uint8_t nondet_uint8( void );
#endif

void rocketnet_error( const char * format, ... )
{
    (void) format;
}

void rocketnet_spam( const char * format, ... )
{
    (void) format;
}

/* ---------------- harness: header reader and split plan ---------------- */

void harness_header_and_plan( void )
{
    uint8_t data[ROCKETNET_FRAGMENT_HEADER_BYTES];
    for ( int i = 0; i < ROCKETNET_FRAGMENT_HEADER_BYTES; i++ )
        data[i] = nondet_uint8();

    uint32_t payload_id;
    int fragment_index;
    int fragment_count;
    rocketnet_read_fragment_header( data, &payload_id, &fragment_index, &fragment_count );

    /* header reads (index from a byte, count from a byte plus one), inside
       the 6-byte header, so no read runs past the object. */
    __CPROVER_assert( fragment_index >= 0 && fragment_index <= 255, "fragment index is a byte" );
    __CPROVER_assert( fragment_count >= 1 && fragment_count <= 256, "fragment count is a byte plus one" );

    int count;
    int stride;
    int payload_bytes = nondet_int();
    __CPROVER_assume( payload_bytes >= 0 && payload_bytes <= ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 100 );

    int rc = rocketnet_fragment_plan( payload_bytes, &count, &stride );

    if ( rc == 1 )
    {
        __CPROVER_assert( count >= 2 && count <= ROCKETNET_MAX_FRAGMENTS, "plan count is at least two and at most the fragment cap" );
        __CPROVER_assert( stride >= 1 && stride <= ROCKETNET_MAX_FRAGMENT_BYTES, "plan stride is within the fragment stride cap" );
        __CPROVER_assert( payload_bytes - stride * ( count - 1 ) >= 1, "plan leaves a non-empty final fragment" );
        __CPROVER_assert( payload_bytes - stride * ( count - 1 ) <= stride, "plan final fragment fits the stride" );
    }
}

/* ---------------- harness: process_fragment on the real code ---------------- */

static int slot_unchanged( const struct rocketnet_reassembly_slot_t * slot,
                           const struct rocketnet_reassembly_slot_t * before )
{
    int same = slot->payload_id == before->payload_id
            && slot->fragment_count == before->fragment_count
            && slot->fragment_stride == before->fragment_stride
            && slot->final_bytes == before->final_bytes
            && slot->final_staged == before->final_staged
            && slot->num_received == before->num_received;
    for ( int i = 0; i < ROCKETNET_MAX_FRAGMENTS / 8; i++ )
    {
        if ( slot->received[i] != before->received[i] )
            same = 0;
    }
    return same;
}

void harness_process_fragment( void )
{
    struct rocketnet_reassembly_slot_t slot;
    memset( &slot, 0, sizeof( slot ) );

    uint8_t arena[PROOF_ARENA];
    uint8_t delivered[PROOF_MAX_PAYLOAD];
    uint8_t type_data[PROOF_TYPE_BUF];
    for ( int i = 0; i < PROOF_TYPE_BUF; i++ )
        type_data[i] = nondet_uint8();

    int type_data_bytes = nondet_int();
    __CPROVER_assume( type_data_bytes >= 0 && type_data_bytes <= PROOF_TYPE_BUF );

    int max_payload = nondet_int();
    __CPROVER_assume( max_payload >= 1 && max_payload <= PROOF_MAX_PAYLOAD );

    uint64_t counters[16];
    for ( int i = 0; i < 16; i++ )
        counters[i] = 0;
    struct rocketnet_reassembly_counters_t table;
    table.zero_payload_id = 0;
    table.stale = 1;
    table.count_invalid = 2;
    table.index_out_of_range = 3;
    table.count_mismatch = 4;
    table.size_mismatch = 5;
    table.payload_too_large = 6;
    table.duplicate_index = 7;
    table.accepted = 8;
    table.reassembled = 9;
    table.abandoned = 10;
    table.batch_full = 11;

    struct rocketnet_reassembly_set_t set;
    memset( &set, 0, sizeof( set ) );
    set.num_connections = 1;
    set.num_slots = 1;
    set.max_payload_bytes = max_payload;
    set.slot_arena_bytes = PROOF_ARENA;
    set.slots = &slot;
    set.arena = arena;
    set.delivered_capacity = 1;
    set.delivered_bytes = PROOF_MAX_PAYLOAD;
    set.delivered = delivered;

    struct rocketnet_reassembly_slot_t before = slot;

    int rc = rocketnet_reassembly_process_fragment( &set, 0, type_data, type_data_bytes, counters, &table );

    const int in_size = type_data_bytes >= ROCKETNET_FRAGMENT_HEADER_BYTES + 1
                     && type_data_bytes <= ROCKETNET_FRAGMENT_HEADER_BYTES + PROOF_STAGING;
    uint32_t payload_id = 0;
    int fragment_index = 0;
    int fragment_count = 1;
    int fragment_bytes = 0;
    if ( in_size )
    {
        const uint8_t * p = type_data;
        payload_id = rocketnet_read_uint32( &p );
        fragment_index = rocketnet_read_uint8( &p );
        fragment_count = rocketnet_read_uint8( &p ) + 1;
        fragment_bytes = type_data_bytes - ROCKETNET_FRAGMENT_HEADER_BYTES;
    }

    if ( !in_size || payload_id == 0 )
    {
        __CPROVER_assert( rc == 0, "illegal size or zero payload id is refused" );
        __CPROVER_assert( slot_unchanged( &slot, &before ), "refusal leaves the slot" );
    }

    if ( rc == 1 && in_size && !( fragment_index == fragment_count - 1 ) )
    {
        __CPROVER_assert( slot.fragment_stride >= 1, "a non-final accept fixes a stride" );
        __CPROVER_assert( (long long) fragment_index * slot.fragment_stride + fragment_bytes <= max_payload,
            "placed fragment stays inside the payload maximum" );
    }

    if ( rc == 1 && counters[table.reassembled] == 1 )
    {
        __CPROVER_assert( slot.payload_id == 0, "completion clears the slot" );
        __CPROVER_assert( set.completed_bytes >= 1 && set.completed_bytes <= max_payload,
            "completed payload fits the maximum" );
    }
}

/* ---------------- harness: memory sizing, no unsigned overflow ---------------- */

#if 0
#define PROOF_MAX_CLIENTS 8
#define PROOF_DELIVERED_CAP 64
#else
#define PROOF_MAX_CLIENTS 8
#define PROOF_DELIVERED_CAP 64
#endif

void harness_memory_sizing( void )
{
    int num_connections = nondet_int();
    __CPROVER_assume( num_connections >= 1 && num_connections <= PROOF_MAX_CLIENTS );
    int num_slots = nondet_int();
    __CPROVER_assume( num_slots >= 1 && num_slots <= ROCKETNET_MAX_FRAGMENTS );
    int max_payload = nondet_int();
    __CPROVER_assume( max_payload >= ROCKETNET_MAX_PAYLOAD_BYTES
                      && max_payload <= ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES );
    int delivered_capacity = nondet_int();
    __CPROVER_assume( delivered_capacity >= 1 && delivered_capacity <= PROOF_DELIVERED_CAP );
    uint64_t bytes = nondet_uint();
    __CPROVER_assume( bytes >= (uint64_t) max_payload && bytes <= (uint64_t) max_payload * 8 );

    uint64_t required = rocketnet_reassembly_memory_required( num_connections, num_slots, max_payload, delivered_capacity );
    __CPROVER_assert( required <= SIZE_MAX, "required bytes do not overflow" );

    uint64_t required_d = rocketnet_reassembly_memory_required_delivered(
        num_connections, num_slots, max_payload, delivered_capacity, bytes );
    __CPROVER_assert( required_d <= SIZE_MAX, "delivered required bytes do not overflow" );

    bool allowed = rocketnet_reassembly_memory_allowed( num_connections, num_slots, max_payload, delivered_capacity );
    bool allowed_d = rocketnet_reassembly_memory_allowed_delivered(
        num_connections, num_slots, max_payload, delivered_capacity, bytes );
    __CPROVER_assert( !allowed || required <= SIZE_MAX, "allowed geometry does not overflow" );
    __CPROVER_assert( !allowed_d || required_d <= SIZE_MAX, "delivered allowed geometry does not overflow" );
}
