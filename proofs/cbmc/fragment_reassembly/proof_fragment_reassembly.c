/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Bounded model-checking proof of the fragment readers on the real sdk
    code. The readers are sdk/rocketnet_fragment.cpp, included below (never
    copied). This harness supersedes proof_fragment_bounds.c, which copies
    the reassembly function; the copy's divergences from the real function
    are named in results.md.

    The checker is ESBMC, built as C++17: sdk/rocketnet_fragment.cpp is C++
    that the CBMC front end does not parse (results.md). proof.mk passes
    esbmc_cprover.h with --include-file, which maps the __CPROVER_ names
    used here onto ESBMC, and compiles this file through a one-line C++
    wrapper. Checks: ESBMC's default bounds, pointer and division checks,
    --overflow-check (signed overflow), --ub-shift-check, and the unwinding
    assertions; harness_memory_sizing adds --unsigned-overflow-check.

    Bounds. Built with no bound macros, every harness is at the full
    production bound: fragment type data up to the header plus
    ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES and one byte past it, and
    max_payload_bytes up to ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES.
    PROOF_MAX_FRAGMENT, PROOF_MAX_PAYLOAD and the sizing macros lower them.
    proof.mk passes the largest bounds that finish; results.md records the
    bound each harness proved and the full bounds that did not finish.
*/

#include <stdlib.h>

#ifndef PROOF_MAX_FRAGMENT
#define PROOF_MAX_FRAGMENT ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES
#endif

#ifndef PROOF_MAX_PAYLOAD
#define PROOF_MAX_PAYLOAD ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES
#endif

#ifndef PROOF_DELIVERED_CAPACITY
#define PROOF_DELIVERED_CAPACITY 64
#endif

/* PROOF_SLOT_CASE splits harness_process_fragment by the slot's state
   before the call and the fragment that arrives, so each run is a smaller
   problem. 0 is every reachable state and every fragment. 1 an empty slot;
   2 an occupied slot whose stride is not yet known; with the stride fixed,
   3 a fragment of illegal size or another payload id, 4 a non-final
   fragment of the slot's payload, 5 the final fragment of the slot's
   payload. Cases 1 to 5 together are case 0. */

#ifndef PROOF_SLOT_CASE
#define PROOF_SLOT_CASE 0
#endif

/* The memcpy model. The reader's copies run up to a whole payload
   (ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES bytes at completion), far past
   what unrolling a byte loop can reach, and no property here depends on
   the bytes copied: only on where they go. So under the checker every
   memcpy, in the reader and in the inline helpers it calls, is this model:
   a bounds-checked write of the destination's first and last byte from
   the source's first and last byte. Objects are contiguous, so both ends
   inside one object is the whole span inside it; the checker's bounds and
   pointer checks fire on either end outside its object. The bytes between
   the ends are not modelled. memset stays the library's own: the reader
   only clears fixed-size slot records with it. */

#include <string.h>
#include <stdint.h>

#ifdef __CPROVER__

/* The slot arena holds two regions: the payload, max_payload_bytes from its
   start, and the staging buffer after it. A span that touches the arena
   lies wholly inside one of them: the checker's object bounds alone would
   let a placement run from the payload into staging. */

static const uint8_t * proof_arena = NULL;
static size_t proof_payload_region = 0;
static size_t proof_staging_region = 0;

static int proof_span_in_region( const uint8_t * p, size_t bytes )
{
    if ( proof_arena == NULL || !__CPROVER_same_object( p, proof_arena ) )
        return 1;
    const size_t offset = (size_t) ( p - proof_arena );
    if ( offset == proof_payload_region )
        return bytes <= proof_staging_region;
    return offset < proof_payload_region && bytes <= proof_payload_region - offset;
}

static void * proof_memcpy( void * destination, const void * source, size_t bytes )
{
    if ( bytes == 0 )
        return destination;
    uint8_t * d = (uint8_t *) destination;
    const uint8_t * s = (const uint8_t *) source;
    __CPROVER_assert( proof_span_in_region( d, bytes ), "a copy into the slot arena stays inside the payload region or the staging buffer" );
    __CPROVER_assert( proof_span_in_region( s, bytes ), "a copy out of the slot arena stays inside the payload region or the staging buffer" );
    d[0] = s[0];
    d[bytes - 1] = s[bytes - 1];
    return destination;
}
#define memcpy proof_memcpy

/* the sdk's own assertions are claims too: with NDEBUG unset each
   rocketnet_assert is a checked property */
#define ROCKETNET_ASSERTS 1
#define rn_assert_function( condition, function, file, line ) __CPROVER_assert( 0, condition )

#endif

#include "rocketnet_fragment.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned nondet_uint( void ) { return 0; }
static uint8_t nondet_uint8( void ) { return 0; }
static uint64_t nondet_uint64( void ) { return 0; }
#else
int nondet_int( void );
unsigned nondet_uint( void );
uint8_t nondet_uint8( void );
uint64_t nondet_uint64( void );
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

    /* the header is 6 bytes: the id little-endian, the index byte, the
       count byte plus one */
    __CPROVER_assert( payload_id == ( (uint32_t) data[0] | ( (uint32_t) data[1] << 8 ) | ( (uint32_t) data[2] << 16 ) | ( (uint32_t) data[3] << 24 ) ),
                      "payload id is the first four bytes, little-endian" );
    __CPROVER_assert( fragment_index == data[4], "fragment index is the fifth byte" );
    __CPROVER_assert( fragment_count == data[5] + 1, "fragment count is the sixth byte plus one" );

    int count = 0;
    int stride = 0;
    int payload_bytes = nondet_int();

    int rc = rocketnet_fragment_plan( payload_bytes, &count, &stride );

    const int needs_fragments = payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES && payload_bytes <= ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES;
    __CPROVER_assert( rc == needs_fragments, "the plan exists exactly for payloads above one packet and inside the fragment cap" );

    if ( rc == 1 )
    {
        __CPROVER_assert( count >= 2 && count <= ROCKETNET_MAX_FRAGMENTS, "plan count is at least two and at most the fragment cap" );
        __CPROVER_assert( stride >= 1 && stride <= ROCKETNET_MAX_FRAGMENT_BYTES, "plan stride is within the fragment stride cap" );
        __CPROVER_assert( payload_bytes - stride * ( count - 1 ) >= 1, "plan leaves a non-empty final fragment" );
        __CPROVER_assert( payload_bytes - stride * ( count - 1 ) <= stride, "plan final fragment fits the stride" );
        __CPROVER_assert( stride <= ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES, "every planned fragment fits the receiver's staging" );
    }
}

/* ---------------- harness: process_fragment on the real code ---------------- */

static int popcount8( uint8_t b )
{
    int v = b;
    v = ( v & 0x55 ) + ( ( v >> 1 ) & 0x55 );
    v = ( v & 0x33 ) + ( ( v >> 2 ) & 0x33 );
    return ( v & 0x0f ) + ( ( v >> 4 ) & 0x0f );
}

static int bit_of( const struct rocketnet_reassembly_slot_t * slot, int index )
{
    return ( slot->received[index / 8] >> ( index % 8 ) ) & 1;
}

/* The reachable slot states: what rocketnet_reassembly_set_create (all
   zero) and every rocketnet_reassembly_process_fragment call leave. The
   harness assumes it of the slot before the call and asserts it after, so
   one call from any reachable state is covered, not only from an empty
   slot. */

static int slot_ok( const struct rocketnet_reassembly_slot_t * slot, int max_payload_bytes )
{
    int bits = 0;
    int high_bits = 0;
    for ( int i = 0; i < ROCKETNET_MAX_FRAGMENTS / 8; i++ )
    {
        bits += popcount8( slot->received[i] );
    }

    if ( slot->payload_id == 0 )
    {
        return slot->fragment_count == 0 && slot->fragment_stride == 0 && slot->final_bytes == 0 && slot->final_staged == 0 && slot->num_received == 0 && bits == 0;
    }

    const int count = slot->fragment_count;
    if ( count < 2 || count > ROCKETNET_MAX_FRAGMENTS )
        return 0;

    for ( int i = 0; i < ROCKETNET_MAX_FRAGMENTS / 8; i++ )
    {
        /* bits at or above the count are never set */
        const int low = i * 8;
        const int mask = count <= low ? 0xff : ( count >= low + 8 ? 0 : ( 0xff << ( count - low ) ) & 0xff );
        if ( slot->received[i] & mask )
            high_bits++;
    }

    const int stride = slot->fragment_stride;
    const int final_bytes = slot->final_bytes;
    const int final_seen = bit_of( slot, count - 1 );

    if ( high_bits != 0 || slot->num_received != bits || bits >= count )
        return 0;
    if ( stride < 0 || stride > ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES )
        return 0;
    if ( final_seen ? !( final_bytes >= 1 && final_bytes <= ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES ) : final_bytes != -1 )
        return 0;
    if ( slot->final_staged != ( stride == 0 && final_seen ) )
        return 0;
    if ( stride == 0 )
    {
        /* no non-final fragment yet: at most the final is held */
        if ( bits != final_seen )
            return 0;
    }
    else
    {
        if ( bits == final_seen )
            return 0;   /* a stride is fixed only by a non-final arrival */
        if ( final_seen && final_bytes > stride )
            return 0;
        if ( (int64_t) stride * ( count - 1 ) + ( final_seen ? final_bytes : 1 ) > (int64_t) max_payload_bytes )
            return 0;
    }
    return 1;
}

static void nondet_slot( struct rocketnet_reassembly_slot_t * slot, int max_payload_bytes )
{
    slot->payload_id = nondet_uint();
    slot->fragment_count = nondet_int();
    slot->fragment_stride = nondet_int();
    slot->final_bytes = nondet_int();
    slot->final_staged = nondet_int();
    slot->num_received = nondet_int();
    for ( int i = 0; i < ROCKETNET_MAX_FRAGMENTS / 8; i++ )
        slot->received[i] = nondet_uint8();
    __CPROVER_assume( slot_ok( slot, max_payload_bytes ) );
}

static int slot_same( const struct rocketnet_reassembly_slot_t * slot,
                      const struct rocketnet_reassembly_slot_t * before )
{
    int same = slot->payload_id == before->payload_id && slot->fragment_count == before->fragment_count && slot->fragment_stride == before->fragment_stride && slot->final_bytes == before->final_bytes && slot->final_staged == before->final_staged && slot->num_received == before->num_received;
    for ( int i = 0; i < ROCKETNET_MAX_FRAGMENTS / 8; i++ )
    {
        if ( slot->received[i] != before->received[i] )
            same = 0;
    }
    return same;
}

void harness_process_fragment( void )
{
    /* one connection and one slot: the slot and arena index arithmetic
       reduces to slot 0, and every input byte, size and prior slot state is
       symbolic */

    const int max_payload = nondet_int();
    __CPROVER_assume( max_payload >= 1 && max_payload <= PROOF_MAX_PAYLOAD );

    /* the slot record and the per-connection reference ride in one
       allocation, as rocketnet_reassembly_set_create lays them out */
    struct rocketnet_reassembly_slot_t * slot = (struct rocketnet_reassembly_slot_t *) malloc( sizeof( struct rocketnet_reassembly_slot_t ) + sizeof( uint32_t ) );
    __CPROVER_assume( slot != NULL );
    nondet_slot( slot, max_payload );
#if PROOF_SLOT_CASE == 1
    __CPROVER_assume( slot->payload_id == 0 );
#elif PROOF_SLOT_CASE == 2
    __CPROVER_assume( slot->payload_id != 0 && slot->fragment_stride == 0 );
#elif PROOF_SLOT_CASE >= 3
    __CPROVER_assume( slot->payload_id != 0 && slot->fragment_stride != 0 );
#endif
    uint32_t * newest_completed = (uint32_t *) ( (uint8_t *) slot + sizeof( struct rocketnet_reassembly_slot_t ) );
    *newest_completed = nondet_uint();

    /* the slot's arena: max payload plus the staging buffer, at the
       receive-side maximum fragment, exactly as create sizes it */
    const size_t slot_arena_bytes = (size_t) max_payload + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES;
    uint8_t * arena = (uint8_t *) malloc( slot_arena_bytes );
    __CPROVER_assume( arena != NULL );
#ifdef __CPROVER__
    proof_arena = arena;
    proof_payload_region = (size_t) max_payload;
    proof_staging_region = ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES;
#endif

    /* the delivered arena: at least one maximum payload, the caller's byte
       bound, with an arbitrary part already used this update */
    const int delivered_capacity = nondet_int();
    __CPROVER_assume( delivered_capacity >= 1 && delivered_capacity <= PROOF_DELIVERED_CAPACITY );
    const uint64_t delivered_bytes = nondet_uint64();
    __CPROVER_assume( delivered_bytes >= (uint64_t) max_payload && delivered_bytes <= (uint64_t) PROOF_DELIVERED_CAPACITY * ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED( PROOF_MAX_PAYLOAD ) );
    uint8_t * delivered = (uint8_t *) malloc( (size_t) delivered_bytes );
    __CPROVER_assume( delivered != NULL );
    const int num_delivered = nondet_int();
    __CPROVER_assume( num_delivered >= 0 && num_delivered <= delivered_capacity );
    const uint64_t delivered_used = nondet_uint64();
    __CPROVER_assume( delivered_used <= delivered_bytes );

    /* the fragment: an object of exactly type_data_bytes symbolic bytes,
       from empty to one byte past the largest the reader accepts */
    const int type_data_bytes = nondet_int();
    __CPROVER_assume( type_data_bytes >= 0 && type_data_bytes <= ROCKETNET_FRAGMENT_HEADER_BYTES + PROOF_MAX_FRAGMENT + 1 );
    uint8_t * type_data = (uint8_t *) malloc( (size_t) type_data_bytes );
    __CPROVER_assume( type_data != NULL );

    uint64_t counters[12];
    for ( int i = 0; i < 12; i++ )
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

    struct rocketnet_reassembly_set_t set = {};
    set.num_connections = 1;
    set.num_slots = 1;
    set.max_payload_bytes = max_payload;
    set.slot_arena_bytes = slot_arena_bytes;
    set.slots = slot;
    set.newest_completed = newest_completed;
    set.arena = arena;
    set.delivered_capacity = delivered_capacity;
    set.num_delivered = num_delivered;
    set.delivered_bytes = (size_t) delivered_bytes;
    set.delivered_used = (size_t) delivered_used;
    set.delivered = delivered;

    const struct rocketnet_reassembly_slot_t before = *slot;
    const uint32_t before_reference = *newest_completed;

    /* the header fields as the reader will see them */
    const int in_size = type_data_bytes >= ROCKETNET_FRAGMENT_HEADER_BYTES + 1 && type_data_bytes <= ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES;
    uint32_t payload_id = 0;
    int fragment_index = 0;
    int fragment_count = 1;
    int fragment_bytes = 0;
    if ( in_size )
    {
        payload_id = (uint32_t) type_data[0] | ( (uint32_t) type_data[1] << 8 ) | ( (uint32_t) type_data[2] << 16 ) | ( (uint32_t) type_data[3] << 24 );
        fragment_index = type_data[4];
        fragment_count = type_data[5] + 1;
        fragment_bytes = type_data_bytes - ROCKETNET_FRAGMENT_HEADER_BYTES;
    }
    const int is_final = fragment_index == fragment_count - 1;
#if PROOF_SLOT_CASE == 3
    __CPROVER_assume( !in_size || payload_id != before.payload_id );
#elif PROOF_SLOT_CASE == 4
    __CPROVER_assume( in_size && payload_id == before.payload_id && !is_final );
#elif PROOF_SLOT_CASE == 5
    __CPROVER_assume( in_size && payload_id == before.payload_id && is_final );
#endif

    int rc = rocketnet_reassembly_process_fragment( &set, 0, type_data, type_data_bytes, counters, &table );

    __CPROVER_assert( slot_ok( slot, max_payload ), "the slot stays in a reachable state" );
    __CPROVER_assert( counters[table.accepted] == (uint64_t) rc, "accepted fires exactly on acceptance" );

    if ( !in_size || payload_id == 0 || fragment_count == 1 || fragment_index >= fragment_count )
    {
        __CPROVER_assert( rc == 0, "illegal size, zero id, count of one or index out of range is refused" );
        __CPROVER_assert( slot_same( slot, &before ), "that refusal leaves the slot" );
    }

    {
        uint64_t refusals = 0;
        for ( int i = 0; i <= 7; i++ )
            refusals += counters[i];
        __CPROVER_assert( refusals == (uint64_t) ( 1 - rc ), "a refusal fires exactly one refusal counter, an acceptance none" );
    }

    if ( in_size && payload_id != 0 )
    {
        const int same = payload_id == before.payload_id;
        if ( payload_id - before_reference > ROCKETNET_REASSEMBLY_REACH && before_reference - payload_id > ROCKETNET_REASSEMBLY_REACH )
            __CPROVER_assert( rc == 0 && counters[table.stale] == 1, "an id beyond reach of the newest completed is refused stale" );
        if ( before.payload_id != 0 && !same && before.payload_id - payload_id <= ROCKETNET_REASSEMBLY_BEHIND )
            __CPROVER_assert( rc == 0 && counters[table.stale] == 1, "an id just behind the occupant is refused stale" );
        if ( same && fragment_count != before.fragment_count )
            __CPROVER_assert( rc == 0, "a count that differs from the pinned count is refused" );
        if ( same && before.fragment_stride != 0 && !is_final && fragment_bytes != before.fragment_stride )
            __CPROVER_assert( rc == 0, "a non-final fragment off the stride is refused" );
        if ( same && before.fragment_stride != 0 && is_final && fragment_bytes > before.fragment_stride )
            __CPROVER_assert( rc == 0, "a final fragment over the stride is refused" );
        if ( same && fragment_index < fragment_count && bit_of( &before, fragment_index ) )
            __CPROVER_assert( rc == 0, "a fragment index already held is refused" );
    }

    if ( rc == 0 )
    {
        __CPROVER_assert( slot_same( slot, &before ) || slot->payload_id == 0, "a refusal leaves the slot or clears it" );
        __CPROVER_assert( set.completed_payload == NULL && set.completed_bytes == 0, "a refusal completes nothing" );
        __CPROVER_assert( *newest_completed == before_reference, "a refusal leaves the stale rule's reference" );
    }

    if ( rc == 1 )
    {
        __CPROVER_assert( in_size && payload_id != 0 && fragment_count >= 2 && fragment_index < fragment_count, "only a well-formed fragment is accepted" );
        if ( counters[table.reassembled] == 0 )
        {
            __CPROVER_assert( *newest_completed == before_reference, "an acceptance short of completion leaves the reference" );
            /* still assembling: where this fragment landed is inside the
               slot's payload region, or the staging buffer for an early final */
            __CPROVER_assert( slot->payload_id == payload_id && slot->fragment_count == fragment_count,
                              "the accepted fragment's payload holds the slot" );
            __CPROVER_assert( bit_of( slot, fragment_index ), "the accepted index is marked" );
            if ( slot->fragment_stride != 0 )
            {
                const int64_t at = is_final ? (int64_t) slot->fragment_stride * ( fragment_count - 1 )
                                            : (int64_t) slot->fragment_stride * fragment_index;
                __CPROVER_assert( at + fragment_bytes <= (int64_t) max_payload, "the placed fragment stays inside the payload region" );
            }
            else
            {
                __CPROVER_assert( is_final && slot->final_staged, "only a final waits in staging" );
            }
        }
        else
        {
            __CPROVER_assert( counters[table.reassembled] == 1, "one completion per call" );
            __CPROVER_assert( slot->payload_id == 0, "completion clears the slot" );
            __CPROVER_assert( *newest_completed == before_reference || *newest_completed == payload_id,
                              "completion moves the reference only to the completed id" );
            __CPROVER_assert( before.payload_id == payload_id && before.num_received == before.fragment_count - 1,
                              "only the last missing fragment completes a payload" );
            if ( set.completed_payload != NULL )
            {
                __CPROVER_assert( set.completed_bytes >= 1 && set.completed_bytes <= max_payload, "completed payload fits the maximum" );
                __CPROVER_assert( set.completed_payload >= delivered && (uint64_t) ( set.completed_payload - delivered ) + (uint64_t) set.completed_bytes <= delivered_bytes,
                                  "the completed payload's span is inside the delivered arena" );
                __CPROVER_assert( set.delivered_used <= set.delivered_bytes && set.num_delivered <= delivered_capacity,
                                  "the delivered arena stays inside its bounds" );
                __CPROVER_assert( counters[table.batch_full] == 0, "a delivered completion is not batch full" );
            }
            else
            {
                __CPROVER_assert( counters[table.batch_full] == 1 && set.num_delivered == num_delivered && set.delivered_used == delivered_used,
                                  "a completion that does not fit counts batch full and packs nothing" );
            }
        }
    }
}

/* ---------------- harness: memory sizing, no unsigned overflow ---------------- */

/* PROOF_SIZING_FULL leaves every argument unconstrained: every value the
   int and uint64 types carry. Without it the arguments range over what the
   transports can pass: up to PROOF_SIZING_MAX_CONNECTIONS connections
   (ROCKETNET_MAX_CLIENTS by default; the client passes one), up to
   PROOF_SIZING_MAX_CAPACITY completions (by default the server's drain,
   ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE: both delivered-capacity
   functions return at most their drain, and the client's drain is
   smaller), a delivered byte bound up to that capacity at the aligned
   largest payload (the most rocketnet_server_reassembly_delivered_bytes
   returns), and every slot count and payload size, documented or not. */

#ifndef PROOF_SIZING_MAX_CONNECTIONS
#define PROOF_SIZING_MAX_CONNECTIONS ROCKETNET_MAX_CLIENTS
#endif

#ifndef PROOF_SIZING_MAX_CAPACITY
#define PROOF_SIZING_MAX_CAPACITY ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE
#endif

void harness_memory_sizing( void )
{
    const int num_connections = nondet_int();
    const int num_slots = nondet_int();
    const int max_payload = nondet_int();
    const int delivered_capacity = nondet_int();
    const uint64_t bytes = nondet_uint64();
#ifndef PROOF_SIZING_FULL
    __CPROVER_assume( num_connections <= PROOF_SIZING_MAX_CONNECTIONS );
    __CPROVER_assume( delivered_capacity <= PROOF_SIZING_MAX_CAPACITY );
    __CPROVER_assume( bytes <= (uint64_t) PROOF_SIZING_MAX_CAPACITY * ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED( ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES ) );
#endif

    const uint64_t required = rocketnet_reassembly_memory_required( num_connections, num_slots, max_payload, delivered_capacity );
    const uint64_t required_d = rocketnet_reassembly_memory_required_delivered( num_connections, num_slots, max_payload, delivered_capacity, bytes );
    const bool allowed = rocketnet_reassembly_memory_allowed( num_connections, num_slots, max_payload, delivered_capacity );
    const bool allowed_d = rocketnet_reassembly_memory_allowed_delivered( num_connections, num_slots, max_payload, delivered_capacity, bytes );

    const int documented = num_connections >= 1 && num_slots >= 1 && num_slots <= ROCKETNET_MAX_FRAGMENTS && max_payload >= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES && max_payload <= ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES && delivered_capacity >= 1;

    __CPROVER_assert( documented || ( required == 0 && required_d == 0 && !allowed && !allowed_d ), "invalid geometry is refused" );
    __CPROVER_assert( bytes >= (uint64_t) max_payload || required_d == 0, "a delivered bound below one payload is refused" );

    /* a nonzero answer covers every component without wrapping: the slot
       arenas alone, and the delivered bytes alone, fit under it. A nonzero
       answer implies the documented geometry, under which these products
       stay below 2^59 */
    if ( required_d != 0 )
    {
        const uint64_t slots = (uint64_t) num_connections * (uint64_t) num_slots;
        __CPROVER_assert( required_d >= slots * ( (uint64_t) max_payload + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES ), "delivered required bytes cover the slot arenas" );
        __CPROVER_assert( required_d > bytes, "delivered required bytes cover the delivered arena" );
    }
    if ( required != 0 )
    {
        const uint64_t slots = (uint64_t) num_connections * (uint64_t) num_slots;
        __CPROVER_assert( required >= slots * ( (uint64_t) max_payload + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES ), "required bytes cover the slot arenas" );
        __CPROVER_assert( required > (uint64_t) delivered_capacity * (uint64_t) ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED( max_payload ), "required bytes cover the full delivered arena" );
    }

    const uint64_t budget = (uint64_t) ROCKETNET_REASSEMBLY_MEMORY_MIB * 1024 * 1024;
    __CPROVER_assert( allowed == ( required != 0 && required <= budget ), "allowed is a nonzero requirement inside the budget" );
    __CPROVER_assert( allowed_d == ( required_d != 0 && required_d <= budget ), "delivered allowed is a nonzero requirement inside the budget" );
}
