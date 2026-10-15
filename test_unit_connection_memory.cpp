/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Per-connection message memory and the kick on exhaustion
// (tla/RNConnectionMemory.tla, tla/README-RNConnectionMemory.md,
// docs/CHANNELS.md 4). A server's regions are carved from one block at
// create, one per client slot; a client's one region is the same arena with
// one slot. Exhaustion is a sticky region state with the reason
// MESSAGE_MEMORY_EXHAUSTED, and the kick decision is a pure function of the
// region. No sockets and no clock: nothing here reads time.

#include "test_unit_registry.h"

#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// The counting allocator: rocketnet_allocator accepts a replacement only on an
// uninitialized library with nothing live, so the worker's init is stopped,
// the counter installed and the library started again; the restore reverses it.
// ---------------------------------------------------------------------------

static uint64_t cm_mallocs = 0;
static uint64_t cm_frees = 0;

static void * cm_malloc( void * context, size_t bytes )
{
    (void) context;
    cm_mallocs++;
    return malloc( bytes );
}

static void cm_free( void * context, void * p )
{
    (void) context;
    cm_frees++;
    free( p );
}

static void cm_install()
{
    rocketnet_term();
    check( rocketnet_allocator( cm_malloc, cm_free ) );
    check( rocketnet_init() );
}

static void cm_restore()
{
    rocketnet_term();
    check( rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function ) );
    check( rocketnet_init() );
}

static uint64_t cm_calls()
{
    return cm_mallocs + cm_frees;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// sends of one small message until the region refuses; returns the count taken
static int cm_fill( rn_region_t * region, int bytes, uint32_t * taken, int max_taken )
{
    int n = 0;
    for ( ;; )
    {
        const uint32_t d = rn_region_send_alloc( region, bytes );
        if ( d == RN_DESCRIPTOR_NONE )
            break;
        if ( taken && n < max_taken )
            taken[n] = d;
        n++;
        check( n < 1000000 );
    }
    return n;
}

static bool cm_inside( const void * block, uint64_t block_bytes, const void * p, uint64_t bytes )
{
    const uintptr_t base = (uintptr_t) block;
    const uintptr_t address = (uintptr_t) p;
    return address >= base && bytes <= block_bytes && (uint64_t) ( address - base ) <= block_bytes - bytes;
}

// The region's bookkeeping, every table carve resets and every scalar the
// send paths move, compared byte for byte between two regions of the same
// config. carves (the connections a region has carried) is the one field a
// reuse is meant to change, and the ring's and the chunks' payload bytes are
// left alone by design (every reader writes them first).
static bool cm_bookkeeping_equal( const rn_region_t * a, const rn_region_t * b )
{
    if ( a->bytes != b->bytes || a->num_channels != b->num_channels || a->window != b->window || a->queue != b->queue )
        return false;
    if ( rn_region_exhausted( a ) != rn_region_exhausted( b ) )
        return false;
    if ( rn_connection_memory_decide( a ) != rn_connection_memory_decide( b ) )
        return false;
    if ( a->descriptors_free_head != b->descriptors_free_head || a->descriptors_free != b->descriptors_free )
        return false;
    if ( memcmp( a->channel_send, b->channel_send, sizeof(rn_channel_send_t) * a->num_channels ) != 0 ) return false;
    if ( memcmp( a->channel_receive, b->channel_receive, sizeof(rn_channel_receive_t) * a->num_channels ) != 0 ) return false;
    if ( memcmp( a->send_numbers, b->send_numbers, sizeof(rn_send_number_t) * a->window ) != 0 ) return false;
    if ( memcmp( a->receive_descriptors, b->receive_descriptors, sizeof(rn_receive_descriptor_t) * a->window ) != 0 ) return false;
    if ( memcmp( a->received_bits, b->received_bits, a->window / 8 ) != 0 ) return false;
    if ( memcmp( a->held_index, b->held_index, sizeof(rn_held_entry_t) * a->held_entries ) != 0 ) return false;
    if ( memcmp( a->send_descriptors, b->send_descriptors, sizeof(rn_send_descriptor_t) * a->queue ) != 0 ) return false;
    if ( memcmp( a->sent_packets, b->sent_packets, sizeof(rn_sent_packet_t) * a->packets ) != 0 ) return false;
    if ( memcmp( a->sent_index, b->sent_index, sizeof(rn_sent_index_t) * a->sent_index_entries ) != 0 ) return false;
    if ( memcmp( a->sent_refs, b->sent_refs, sizeof(uint32_t) * a->sent_ref_entries ) != 0 ) return false;
    if ( memcmp( a->deliveries, b->deliveries, sizeof(rn_delivery_t) * a->delivery_entries ) != 0 ) return false;
    if ( memcmp( a->receipts, b->receipts, sizeof(rn_receipt_t) * a->receipt_entries ) != 0 ) return false;
    if ( memcmp( a->resend_fifo, b->resend_fifo, sizeof(rn_resend_link_t) * a->window ) != 0 ) return false;
    for ( uint32_t h = 0; h < a->holds; h++ )
    {
        const rn_sequenced_hold_t * x = &a->sequenced_holds[h];
        const rn_sequenced_hold_t * y = &b->sequenced_holds[h];
        if ( x->bytes != y->bytes || x->ordering != y->ordering || x->sequencing != y->sequencing || x->channel != y->channel || x->used != y->used || x->code != y->code )
            return false;
    }
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const rn_store_class_t * x = &a->store.classes[c];
        const rn_store_class_t * y = &b->store.classes[c];
        if ( x->num_chunks != y->num_chunks || x->free_head != y->free_head || x->free_count != y->free_count || x->chunk_bytes != y->chunk_bytes )
            return false;
        if ( memcmp( x->link, y->link, sizeof(uint32_t) * x->num_chunks ) != 0 )
            return false;
        // the class's place in its region, the same offset in both
        if ( x->data - a->store_memory != y->data - b->store_memory )
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// the tests
// ---------------------------------------------------------------------------

// RNConnectionMemory NoCrossSlotEffect: two regions from one arena (the
// server shape). Exhausting A changes nothing of B: not its memory, not its
// region state, not its kick decision, and B still sends after.
static void memory_isolation_between_slots()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 2, &counters );
    check( arena );
    rn_region_t * a = rn_arena_region( arena, 0 );
    rn_region_t * b = rn_arena_region( arena, 1 );

    // B carries a connection with some sends in flight
    uint32_t b_taken[8];
    for ( int i = 0; i < 8; i++ )
    {
        b_taken[i] = rn_region_send_alloc( b, 100 + i );
        check( b_taken[i] != RN_DESCRIPTOR_NONE );
    }

    const rn_region_t b_before = *b;
    uint8_t * b_memory = (uint8_t*) malloc( (size_t) b->bytes );
    check( b_memory );
    memcpy( b_memory, b->base, (size_t) b->bytes );

    // A sends until refused, then refuses again
    const int sent = cm_fill( a, 10, NULL, 0 );
    check( sent > 0 );
    check( rn_region_exhausted( a ) );
    check( rn_region_send_alloc( a, 10 ) == RN_DESCRIPTOR_NONE );

    // nothing of B moved
    check( memcmp( &b_before, b, sizeof(*b) ) == 0 );
    check( memcmp( b_memory, b->base, (size_t) b->bytes ) == 0 );
    check( !rn_region_exhausted( b ) );
    check( rn_connection_memory_decide( b ) == ROCKETNET_MESSAGE_ERROR_NONE );

    // B goes on: frees, sends, and is still whole
    for ( int i = 0; i < 8; i++ )
        rn_region_send_free( b, b_taken[i] );
    check( b->descriptors_free == b->queue );
    check( rn_region_send_alloc( b, 1000 ) != RN_DESCRIPTOR_NONE );
    check( !rn_region_exhausted( b ) );

    // A's exhaustion is a kick for A only, counted once
    check( rn_connection_memory_decide( a ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );

    free( b_memory );
    rn_arena_destroy( arena );
}

// RNConnectionMemory Exhaust and Kick: exhaustion is sticky with the reason
// MESSAGE_MEMORY_EXHAUSTED, and the kick decision for that region says so on
// every update until the region is reset; a flood is kicked by name.
static void exhaustion_is_a_kick_with_its_reason()
{
    check( ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED != ROCKETNET_MESSAGE_ERROR_NONE );
    check( strcmp( rocketnet_counter_name( ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ), "MESSAGE_MEMORY_EXHAUSTED" ) == 0 );

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    // the client shape: one region
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );

    check( !rn_region_exhausted( region ) );
    check( rn_region_exhausted_reason( region ) == ROCKETNET_MESSAGE_ERROR_NONE );
    check( rn_connection_memory_decide( region ) == ROCKETNET_MESSAGE_ERROR_NONE );

    // a refusal for the caller's own argument is not exhaustion
    check( rn_region_send_alloc( region, 0 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_send_alloc( region, (int) region->max_message_bytes + 1 ) == RN_DESCRIPTOR_NONE );
    check( !rn_region_exhausted( region ) );

    // a flood of maximal messages: the store refuses, and that is exhaustion
    const int sent = cm_fill( region, (int) region->max_message_bytes, NULL, 0 );
    check( sent >= 1 );
    check( rn_region_exhausted( region ) );
    check( rn_region_exhausted_reason( region ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL] == 1 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );

    // sticky: a later refusal does not clear it or count it again
    // (now counts QUEUE_FULL via guard, since reuse of existing counter for post-exhaust send)
    check( rn_region_send_alloc( region, (int) region->max_message_bytes ) == RN_DESCRIPTOR_NONE );
    rn_region_exhaust( region );
    check( rn_region_exhausted( region ) );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == 1 );
    for ( int update = 0; update < 3; update++ )
        check( rn_connection_memory_decide( region ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );

    // the descriptor pool running out is the same outcome, with its own counter
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.send_queue_messages = 16;
    rn_arena_t * small = rn_arena_create( NULL, &config, 1, &counters );
    check( small );
    rn_region_t * q = rn_arena_region( small, 0 );
    check( cm_fill( q, 10, NULL, 0 ) == 16 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == 2 );
    check( rn_connection_memory_decide( q ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 2 );

    // the kick resets the region for the slot's next client
    rn_region_reset( region );
    check( !rn_region_exhausted( region ) );
    check( rn_connection_memory_decide( region ) == ROCKETNET_MESSAGE_ERROR_NONE );

    rn_arena_destroy( small );
    rn_arena_destroy( arena );
}

// RNConnectionMemory Reuse and ReusedSlotStartsClean: a slot's region, used
// to exhaustion and reset, equals a freshly carved region of the same config
// in every byte of its bookkeeping, and holds nothing of its previous client.
static void reused_slot_starts_clean()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 2, &counters );
    rn_arena_t * fresh_arena = rn_arena_create( NULL, NULL, 1, &counters );
    check( arena && fresh_arena );
    rn_region_t * a = rn_arena_region( arena, 0 );
    const rn_region_t * fresh = rn_arena_region( fresh_arena, 0 );

    check( cm_bookkeeping_equal( a, fresh ) );

    // the previous client: mixed sends, a block's fragment acked, tables written
    uint32_t taken[64];
    const int n = cm_fill( a, 300, taken, 64 );
    check( n > 0 );
    const uint32_t block = rn_region_send_alloc( a, 4000 );
    if ( block != RN_DESCRIPTOR_NONE )
        rn_region_send_free_fragment( a, block, 1 );
    for ( int i = 0; i < n && i < 64; i += 2 )
        rn_region_send_free( a, taken[i] );
    a->channel_send[0].ordered_write = 7;
    a->channel_receive[1].ordered_read = 9;
    a->send_numbers[3].descriptor = 5;
    a->receive_descriptors[4].live = 1;
    a->received_bits[0] = 0xFF;
    a->held_index[2].used = 1;
    a->sent_packets[1].packet_id = 77;
    a->sent_index[0].used = 1;
    a->sent_refs[0] = 3;
    a->deliveries[0].bytes = 12;
    a->sequenced_holds[0].used = 1;
    a->receipts[0].serial = 4;
    a->resend_fifo[0].next = 2;
    rn_region_exhaust( a );
    check( rn_region_exhausted( a ) );
    check( !cm_bookkeeping_equal( a, fresh ) );

    // the kick, then the reset before reuse
    rn_region_reset( a );
    check( cm_bookkeeping_equal( a, fresh ) );
    check( a->carves == 2 );

    // and the reused slot behaves as fresh: the same descriptor order
    const uint32_t first_a = rn_region_send_alloc( a, 10 );
    rn_arena_t * fresh_arena2 = rn_arena_create( NULL, NULL, 1, &counters );
    check( fresh_arena2 );
    const uint32_t first_fresh = rn_region_send_alloc( rn_arena_region( fresh_arena2, 0 ), 10 );
    check( first_a == first_fresh );

    rn_arena_destroy( fresh_arena2 );
    rn_arena_destroy( fresh_arena );
    rn_arena_destroy( arena );
}

// The caller may supply the block: create takes nothing from the allocator,
// every region and every record lies inside the block, a short or misaligned
// block is refused (never truncated), and destroy frees nothing.
static void user_block_is_used_and_nothing_else()
{
    cm_install();

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    const int slots = 3;
    const uint64_t required = rn_arena_required_bytes( NULL, slots );
    check( required > 0 );
    check( required >= rocketnet_message_connection_bytes( NULL ) * (uint64_t) slots );
    check( rn_arena_required_bytes( NULL, 0 ) == 0 );

    uint8_t * raw = (uint8_t*) malloc( (size_t) required + 2 * ROCKETNET_ALLOCATION_ALIGNMENT );
    check( raw );
    uint8_t * block = (uint8_t*) ( ( (uintptr_t) raw + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );

    const uint64_t before = cm_calls();

    // short by one byte, misaligned, and missing: refused whole, nothing taken
    check( rn_arena_create_in_block( block, required - 1, NULL, slots, &counters ) == NULL );
    check( rn_arena_create_in_block( block + 1, required, NULL, slots, &counters ) == NULL );
    check( rn_arena_create_in_block( NULL, required, NULL, slots, &counters ) == NULL );

    rn_arena_t * arena = rn_arena_create_in_block( block, required, NULL, slots, &counters );
    check( arena );
    check( (void*) arena == (void*) block );
    check( rn_arena_bytes( arena ) == required );
    check( rn_arena_num_regions( arena ) == slots );

    for ( int s = 0; s < slots; s++ )
    {
        rn_region_t * region = rn_arena_region( arena, s );
        check( cm_inside( block, required, region, sizeof(*region) ) );
        check( cm_inside( block, required, region->base, region->bytes ) );

        uint32_t taken[2048];
        const int n = cm_fill( region, 200 + 300 * s, taken, 2048 );
        check( n > 0 && n <= 2048 );
        for ( int i = 0; i < n; i++ )
        {
            const rn_send_descriptor_t * d = &region->send_descriptors[taken[i]];
            check( cm_inside( block, required, d, sizeof(*d) ) );
            for ( uint32_t chunk = d->chunk; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &region->store, chunk ) )
            {
                const uint8_t * data = rn_send_store_chunk_data( &region->store, chunk );
                check( cm_inside( block, required, data, region->store.classes[rn_send_store_chunk_class( chunk )].chunk_bytes ) );
                check( rn_region_contains( region, data, 1 ) );
            }
        }
        check( rn_connection_memory_decide( region ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
        rn_region_reset( region );
        check( rn_connection_memory_decide( region ) == ROCKETNET_MESSAGE_ERROR_NONE );
    }

    rn_arena_destroy( arena );
    check( cm_calls() == before );

    free( raw );
    cm_restore();
}

// R2: a counting allocator sees create's one allocation and then nothing
// across sends, acks, exhaustion, the kick decision, the reset and reuse,
// for a server-shaped arena and a client-shaped one; destroy is the one free.
static void no_allocation_after_create()
{
    cm_install();

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    const uint64_t mallocs_before = cm_mallocs;
    rn_arena_t * server = rn_arena_create( NULL, NULL, 4, &counters );
    rn_arena_t * client = rn_arena_create( NULL, NULL, 1, &counters );
    check( server && client );
    check( cm_mallocs == mallocs_before + 2 );

    const uint64_t after_create = cm_calls();

    rn_region_t * regions[5];
    for ( int s = 0; s < 4; s++ )
        regions[s] = rn_arena_region( server, s );
    regions[4] = rn_arena_region( client, 0 );

    for ( int round = 0; round < 6; round++ )
    {
        for ( int r = 0; r < 5; r++ )
        {
            rn_region_t * region = regions[r];
            uint32_t taken[1024];
            const int bytes = ( round + r ) % 3 == 0 ? 60 : ( ( round + r ) % 3 == 1 ? 400 : 2500 );
            const int n = cm_fill( region, bytes, taken, 1024 );
            for ( int i = 0; i < n && i < 1024; i++ )
            {
                if ( region->send_descriptors[taken[i]].fragments > 1 )
                    rn_region_send_free_fragment( region, taken[i], 0 );
                else if ( i % 2 == 0 )
                    rn_region_send_free( region, taken[i] );
            }
            const int reason = rn_connection_memory_decide( region );
            check( reason == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
            rn_region_reset( region );
            check( rn_connection_memory_decide( region ) == ROCKETNET_MESSAGE_ERROR_NONE );
        }
    }

    check( cm_calls() == after_create );

    const uint64_t frees_before = cm_frees;
    rn_arena_destroy( server );
    rn_arena_destroy( client );
    check( cm_frees == frees_before + 2 );

    cm_restore();
}

// RNConnectionMemory ExhaustionOnlyLeadsToDisconnect: after exhaust (descriptor
// pool), a free returns room to pools but send still refuses (guard); only
// reset (model Reuse) grants again. Counts reuse existing queue counter.
static void exhausted_region_refuses_sends_until_reset()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.send_queue_messages = 16;

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    uint32_t d[16];
    for ( int i = 0; i < 16; i++ )
    {
        d[i] = rn_region_send_alloc( region, 10 );
        check( d[i] != RN_DESCRIPTOR_NONE );
    }
    check( region->descriptors_free == 0 );
    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_exhausted( region ) );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == 1 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );

    rn_region_send_free( region, d[5] );
    check( region->descriptors_free == 1 );
    // guard: free gave room but exhausted refuses, counts again, takes none
    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_exhausted( region ) );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == 2 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );

    // reset (the model's after Kick) clears
    rn_region_reset( region );
    check( !rn_region_exhausted( region ) );
    check( rn_connection_memory_decide( region ) == ROCKETNET_MESSAGE_ERROR_NONE );
    const uint32_t d2 = rn_region_send_alloc( region, 10 );
    check( d2 != RN_DESCRIPTOR_NONE );

    rn_arena_destroy( arena );
}

REGISTER_UNIT_TEST( memory_isolation_between_slots );
REGISTER_UNIT_TEST( exhaustion_is_a_kick_with_its_reason );
REGISTER_UNIT_TEST( reused_slot_starts_clean );
REGISTER_UNIT_TEST( user_block_is_used_and_nothing_else );
REGISTER_UNIT_TEST( no_allocation_after_create );
REGISTER_UNIT_TEST( exhausted_region_refuses_sends_until_reset );
