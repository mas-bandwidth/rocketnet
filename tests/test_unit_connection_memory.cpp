/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Per-connection message memory and the kick on exhaustion
// (proofs/tla/RNConnectionMemory.tla, proofs/tla/README-RNConnectionMemory.md,
// proofs/tla/RNConnectionMemory.tla). A server's regions are carved from one block at
// create, one per client slot; a client's one region is the same arena with
// one slot. Exhaustion is a sticky region state with the reason
// MESSAGE_MEMORY_EXHAUSTED, and the kick decision is a pure function of the
// region. No sockets and no clock: nothing here reads time.

#include "test_unit_registry.h"

#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_internal.h"
#include "rocketnet_fragment.h"
#include "rocketnet_memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

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
    check( rocketnet_init( NULL, cm_malloc, cm_free ) == ROCKETNET_STATUS_OK );
}

static void cm_restore()
{
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static uint64_t cm_calls()
{
    return cm_mallocs + cm_frees;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// sends of one message size (a store chunk or a span) until the region
// refuses; returns the count taken

static int cm_fill( rn_region_t * region, int bytes, uint32_t * taken, int max_taken )
{
    int n = 0;

    for ( ;; )
    {
        const uint32_t d = rn_region_send_take( region, bytes );
        if ( d == RN_DESCRIPTOR_NONE )
            break;
        if ( taken && n < max_taken )
        {
            taken[n] = d;
        }
        n++;
        check( n < 1000000 );
    }

    return n;
}

#if ROCKETNET_DEV
static bool cm_inside( const void * block, uint64_t block_bytes, const void * p, uint64_t bytes )
{
    const uintptr_t base = (uintptr_t) block;
    const uintptr_t address = (uintptr_t) p;
    return address >= base && bytes <= block_bytes && (uint64_t) ( address - base ) <= block_bytes - bytes;
}
#endif // #if ROCKETNET_DEV

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

    // the pools are lazy (proofs/tla/RNLazyPool.tla): their state is the free list,
    // the free count and the watermark; the bytes of a descriptor or chunk
    // link at or past the watermark are not state and are not compared

    if ( a->descriptors_free_top != b->descriptors_free_top || a->descriptors_free != b->descriptors_free || a->descriptors_fresh != b->descriptors_fresh )
        return false;
    if ( memcmp( a->channel_send, b->channel_send, sizeof( rn_channel_send_t ) * a->num_channels ) != 0 )
        return false;
    if ( memcmp( a->channel_receive, b->channel_receive, sizeof( rn_channel_receive_t ) * a->num_channels ) != 0 )
        return false;
    if ( memcmp( a->send_numbers, b->send_numbers, sizeof( rn_send_number_t ) * a->window ) != 0 )
        return false;
    if ( memcmp( a->receive_descriptors, b->receive_descriptors, sizeof( rn_receive_descriptor_t ) * a->window ) != 0 )
        return false;
    if ( memcmp( a->received_bits, b->received_bits, a->window / 8 ) != 0 )
        return false;
    if ( memcmp( a->held_index, b->held_index, sizeof( rn_held_entry_t ) * a->held_entries ) != 0 )
        return false;
    if ( memcmp( a->send_descriptors, b->send_descriptors, sizeof( rn_send_descriptor_t ) * a->descriptors_fresh ) != 0 )
        return false;
    if ( memcmp( a->sent_packets, b->sent_packets, sizeof( rn_sent_packet_t ) * a->packets ) != 0 )
        return false;
    if ( memcmp( a->sent_index, b->sent_index, sizeof( rn_sent_index_t ) * a->sent_index_entries ) != 0 )
        return false;
    if ( memcmp( a->sent_refs, b->sent_refs, sizeof( uint32_t ) * a->sent_ref_entries ) != 0 )
        return false;
    if ( memcmp( a->deliveries, b->deliveries, sizeof( rn_delivery_t ) * a->delivery_entries ) != 0 )
        return false;
    if ( memcmp( a->receipts, b->receipts, sizeof( rn_receipt_t ) * a->receipt_entries ) != 0 )
        return false;
    if ( memcmp( a->resend_fifo, b->resend_fifo, sizeof( rn_resend_link_t ) * a->window ) != 0 )
        return false;

    for ( uint32_t h = 0; h < a->holds; h++ )
    {
        const rn_sequenced_hold_t * x = &a->sequenced_messages_held[h];
        const rn_sequenced_hold_t * y = &b->sequenced_messages_held[h];
        if ( x->bytes != y->bytes || x->ordering != y->ordering || x->sequencing != y->sequencing || x->channel != y->channel || x->used != y->used || x->code != y->code )
            return false;
    }

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const rn_store_class_t * x = &a->store.classes[c];
        const rn_store_class_t * y = &b->store.classes[c];
        if ( x->num_chunks != y->num_chunks || x->free_head != y->free_head || x->free_count != y->free_count || x->chunk_bytes != y->chunk_bytes || x->fresh != y->fresh )
            return false;
        if ( memcmp( x->meta, y->meta, sizeof( uint32_t ) * x->fresh ) != 0 )
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

static void test_memory_isolation_between_slots()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    // the arena lives in a zeroed block of the test's own, so the byte
    // compare of B below reads only defined bytes: a region's memory is
    // written as it is used, and bytes nothing wrote have no value to compare

    const uint64_t arena_bytes = rn_arena_required_bytes( NULL, 2 );
    check( arena_bytes > 0 );

    uint8_t * raw = (uint8_t *) calloc( 1, (size_t) arena_bytes + ROCKETNET_ALLOCATION_ALIGNMENT );
    check( raw );

    uint8_t * block = (uint8_t *) ( ( (uintptr_t) raw + ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
    rn_arena_t * arena = rn_arena_create_in_block( block, arena_bytes, NULL, 2, &counters );
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
    uint8_t * b_memory = (uint8_t *) malloc( (size_t) b->bytes );
    check( b_memory );
    memcpy( b_memory, b->base, (size_t) b->bytes );

    // a sends until refused, then refuses again

    const int sent = cm_fill( a, 10, NULL, 0 );
    check( sent > 0 );
    check( rn_region_exhausted( a ) );
    check( rn_region_send_alloc( a, 10 ) == RN_DESCRIPTOR_NONE );

    // nothing of B moved

    check( memcmp( &b_before, b, sizeof( *b ) ) == 0 );
    check( memcmp( b_memory, b->base, (size_t) b->bytes ) == 0 );
    check( !rn_region_exhausted( b ) );
    check( rn_connection_memory_decide( b ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // B goes on: frees, sends, and is still whole

    for ( int i = 0; i < 8; i++ )
    {
        rn_region_send_free( b, b_taken[i] );
    }
    check( b->descriptors_free == b->queue );
    check( rn_region_send_alloc( b, 1000 ) != RN_DESCRIPTOR_NONE );
    check( !rn_region_exhausted( b ) );

    // A's exhaustion is a kick for A only: the exhaustion step counts once, and
    // the refusal above (a send after exhaustion) also counts, so it is 2

    check( rn_connection_memory_decide( a ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 2 );

    free( b_memory );
    rn_arena_destroy( arena );
    free( raw );
}

// RNConnectionMemory Exhaust and Kick: exhaustion is sticky with the reason
// MESSAGE_MEMORY_EXHAUSTED, and the kick decision for that region says so on
// every update until the region is reset; a flood is kicked by name.

#if ROCKETNET_DEV
static void test_exhaustion_is_a_kick_with_its_reason()
{
    check( ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED != ROCKETNET_DISCONNECT_REASON_NONE );
    check( strcmp( rocketnet_counter_name( ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ), "MESSAGE_MEMORY_EXHAUSTED" ) == 0 );

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    // the client shape: one region

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    check( !rn_region_exhausted( region ) );
    check( rn_region_exhausted_reason( region ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // a refusal for the caller's own argument is not exhaustion

    check( rn_region_send_alloc( region, 0 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_send_alloc( region, (int) region->max_message_bytes + 1 ) == RN_DESCRIPTOR_NONE );
    check( !rn_region_exhausted( region ) );

    // a flood of maximal messages: the store refuses, and that is exhaustion

    const int sent = cm_fill( region, (int) region->max_message_bytes, NULL, 0 );
    check( sent >= 1 );
    check( rn_region_exhausted( region ) );
    check( rn_region_exhausted_reason( region ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL] == 1 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );

    // sticky: a later refusal does not clear it; each refused send counts the
    // exhaustion counter (the send above plus the explicit exhaust call both
    // see an already-exhausted region, so only the send above counts again)

    check( rn_region_send_take( region, (int) region->max_message_bytes ) == RN_DESCRIPTOR_NONE );
    rn_region_exhaust( region );
    check( rn_region_exhausted( region ) );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 2 );
    for ( int update = 0; update < 3; update++ )
    {
        check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    }

    // the descriptor pool running out is the same outcome, with its own counter

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit capacity
    config.send_queue_messages = 16;

    rn_arena_t * small = rn_arena_create( NULL, &config, 1, &counters );
    check( small );

    rn_region_t * q = rn_arena_region( small, 0 );
    check( cm_fill( q, 10, NULL, 0 ) == 16 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == 1 );
    check( rn_connection_memory_decide( q ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 3 );

    // the kick resets the region for the slot's next client

    rn_region_reset( region );
    check( !rn_region_exhausted( region ) );
    check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_NONE );

    rn_arena_destroy( small );
    rn_arena_destroy( arena );
}
#endif // #if ROCKETNET_DEV

// RNConnectionMemory Reuse and ReusedSlotStartsClean: a slot's region, used
// to exhaustion and reset, equals a freshly carved region of the same config
// in every byte of its bookkeeping, and holds nothing of its previous client.

static void test_reused_slot_starts_clean()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 2, &counters );
    rn_arena_t * fresh_arena = rn_arena_create( NULL, NULL, 1, &counters );
    check( arena && fresh_arena );

    rn_region_t * a = rn_arena_region( arena, 0 );
    const rn_region_t * fresh = rn_arena_region( fresh_arena, 0 );

    check( cm_bookkeeping_equal( a, fresh ) );

    // the previous client: mixed sends, a span's fragment acked, tables written

    uint32_t taken[64];
    const int n = cm_fill( a, 300, taken, 64 );
    check( n > 0 );

    const uint32_t span = rn_region_send_take( a, 4000 );
    if ( span != RN_DESCRIPTOR_NONE )
    {
        rn_region_send_free_fragment( a, span );
    }
    for ( int i = 0; i < n && i < 64; i += 2 )
    {
        rn_region_send_free( a, taken[i] );
    }
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
    a->sequenced_messages_held[0].used = 1;
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

#if ROCKETNET_DEV
static void test_user_block_is_used_and_nothing_else()
{
    cm_install();

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    const int slots = 3;
    const uint64_t required = rn_arena_required_bytes( NULL, slots );
    check( required > 0 );
    check( required >= rocketnet_message_connection_bytes( NULL ) * (uint64_t) slots );
    check( rn_arena_required_bytes( NULL, 0 ) == 0 );

    uint8_t * raw = (uint8_t *) malloc( (size_t) required + 2 * ROCKETNET_ALLOCATION_ALIGNMENT );
    check( raw );

    uint8_t * block = (uint8_t *) ( ( (uintptr_t) raw + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );

    const uint64_t before = cm_calls();

    // short by one byte, misaligned, and missing: refused whole, nothing taken

    check( rn_arena_create_in_block( block, required - 1, NULL, slots, &counters ) == NULL );
    check( rn_arena_create_in_block( block + 1, required, NULL, slots, &counters ) == NULL );
    check( rn_arena_create_in_block( NULL, required, NULL, slots, &counters ) == NULL );

    rn_arena_t * arena = rn_arena_create_in_block( block, required, NULL, slots, &counters );
    check( arena );
    check( (void *) arena == (void *) block );
    check( rn_arena_bytes( arena ) == required );
    check( rn_arena_num_regions( arena ) == slots );

    for ( int s = 0; s < slots; s++ )
    {
        rn_region_t * region = rn_arena_region( arena, s );
        check( cm_inside( block, required, region, sizeof( *region ) ) );
        check( cm_inside( block, required, region->base, region->bytes ) );

        std::vector<uint32_t> taken( region->queue );

        const int n = cm_fill( region, 200 + 300 * s, taken.data(), (int) taken.size() );
        check( n > 0 && (size_t) n <= taken.size() );

        for ( size_t i = 0; i < (size_t) n; i++ )
        {
            const rn_send_descriptor_t * d = &region->send_descriptors[taken[i]];
            check( cm_inside( block, required, d, sizeof( *d ) ) );

            const uint8_t * data = rn_region_send_write_pointer( region, taken[i] );
            check( cm_inside( block, required, data, d->bytes ) );
            check( rn_region_contains( region, data, d->bytes ) );
        }

        check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
        rn_region_reset( region );
        check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    rn_arena_destroy( arena );
    check( cm_calls() == before );

    free( raw );
    cm_restore();
}
#endif // #if ROCKETNET_DEV

// R2: a counting allocator sees create's one allocation and then nothing
// across sends, acks, exhaustion, the kick decision, the reset and reuse,
// for a server-shaped arena and a client-shaped one; destroy is the one free.

static void test_connection_memory_no_allocation_after_create()
{
    cm_install();

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    const uint64_t mallocs_before = cm_mallocs;
    rn_arena_t * server = rn_arena_create( NULL, NULL, 4, &counters );
    rn_arena_t * client = rn_arena_create( NULL, NULL, 1, &counters );
    check( server && client );
    check( cm_mallocs == mallocs_before + 2 );

    const uint64_t after_create = cm_calls();

    rn_region_t * regions[5];
    for ( int s = 0; s < 4; s++ )
    {
        regions[s] = rn_arena_region( server, s );
    }
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
                    rn_region_send_free_fragment( region, taken[i] );
                else if ( i % 2 == 0 )
                    rn_region_send_free( region, taken[i] );
            }

            const int reason = rn_connection_memory_decide( region );
            check( reason == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
            rn_region_reset( region );
            check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_NONE );
        }
    }

    check( cm_calls() == after_create );

    const uint64_t frees_before = cm_frees;
    rn_arena_destroy( server );
    rn_arena_destroy( client );
    check( cm_frees == frees_before + 2 );

    cm_restore();
}

// RNConnectionMemory ExhaustionOnlyLeadsToDisconnect: once a region is
// exhausted, every send is refused until the region is reset (the model's
// Reuse). The old code allowed a free + alloc to succeed after exhaustion.

static void test_exhausted_region_refuses_sends_until_reset()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    // use a small queue to exhaust quickly (min is 16)

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit capacity
    config.send_queue_messages = 16;

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    // fill the descriptor pool completely

    uint32_t descriptors[16];

    for ( int i = 0; i < 16; i++ )
    {
        descriptors[i] = rn_region_send_alloc( region, 10 );
        check( descriptors[i] != RN_DESCRIPTOR_NONE );
    }

    check( region->descriptors_free == 0 );

    // next alloc exhausts the region (queue full)

    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_exhausted( region ) );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == 1 );

    // free one descriptor - but region is still exhausted

    rn_region_send_free( region, descriptors[3] );
    check( region->descriptors_free == 1 );

    // send is refused because region is exhausted (sticky flag); the refusal is
    // counted under MESSAGE_MEMORY_EXHAUSTED and takes nothing from either pool

    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( region->descriptors_free == 1 );  // nothing taken
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 2 );

    // kick decision says disconnect

    check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );

    // reset the region (Reuse in the model)

    rn_region_reset( region );
    check( !rn_region_exhausted( region ) );
    check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // now send succeeds

    const uint32_t d = rn_region_send_alloc( region, 10 );
    check( d != RN_DESCRIPTOR_NONE );
    check( region->descriptors_free == 15 );

    rn_region_send_free( region, d );
    check( region->descriptors_free == 16 );

    rn_arena_destroy( arena );
}

// The receive descriptor take is guaranteed room by the window: accept rejects
// a number outside [R, R+W) before indexing, so the W-entry descriptor array
// never runs out (rn_window_receive_accept). No receive path ever lacks room,
// so there is no receive no-room outcome to wire. What a receive can do is
// arrive on an already-exhausted region; this pins that it is refused -- the
// connection's exhaustion already kicked it -- and the decision stays exhaustion.

static void test_receive_without_room_exhausts_the_region()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    // exhaust the region through reliable sends (the store runs out)

    const int n = cm_fill( region, (int) region->max_message_bytes, NULL, 0 );
    check( n > 0 );
    check( rn_region_exhausted( region ) );

    // the shipped receive path over this region (layer 3 and 4 over the codec)

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit capacity

    rn_ordered_t ordered;
    check( rn_ordered_init( &ordered, region, &config, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );

    // a valid unreliable record

    uint8_t payload[2048];
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );

    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
    record.data = (const uint8_t *) "hi";
    record.bytes = 2;
    check( rocketnet_message_writer_add( &writer, &record ) );

    const int payload_bytes = rocketnet_message_writer_finish( &writer );
    check( payload_bytes > 0 );

    const uint64_t exhausted_before = counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED];
    const int rc = rn_ordered_receive( &ordered, payload, payload_bytes );
    check( rc == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &ordered ) == 0 );            // refused: nothing is delivered
    check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == exhausted_before );

    rn_arena_destroy( arena );
}

// Reassembly geometry: an out-of-range argument (connections and slots at
// least 1, slots at most ROCKETNET_MAX_FRAGMENTS, payload between
// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES and ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES,
// delivered at least 1) or an over-budget geometry is refused before any
// allocation (sdk/rocketnet_fragment.cpp rocketnet_reassembly_memory_required,
// rocketnet_reassembly_memory_allowed, rocketnet_reassembly_set_create).

static void test_reassembly_geometry_refuses_bad_arguments_and_budget()
{
    const int MP = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    // a bad argument yields required 0 and allowed false

    const int bad[][4] = {
        { 0, 1, MP, 1 },
        { 1, 0, MP, 1 },
        { 1, ROCKETNET_MAX_FRAGMENTS + 1, MP, 1 },
        { 1, 1, MP - 1, 1 },
        { 1, 1, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, 1 },
        { 1, 1, MP, 0 },
    };
    for ( size_t i = 0; i < sizeof( bad ) / sizeof( bad[0] ); i++ )
    {
        check( rocketnet_reassembly_memory_required( bad[i][0], bad[i][1], bad[i][2], bad[i][3] ) == 0 );
        check( !rocketnet_reassembly_memory_allowed( bad[i][0], bad[i][1], bad[i][2], bad[i][3] ) );
    }

    // valid geometries are accepted

    check( rocketnet_reassembly_memory_required( 4, 8, 16384, 32 ) != 0 );
    check( rocketnet_reassembly_memory_allowed( 4, 8, 16384, 32 ) );
    check( rocketnet_reassembly_memory_required( 1, 1, MP, 1 ) != 0 );
    check( rocketnet_reassembly_memory_allowed( 1, 1, MP, 1 ) );

    // an over-budget geometry: required is non-zero and over the budget,
    // allowed is false, and create refuses it before any allocation

    const uint64_t required = rocketnet_reassembly_memory_required(
        1024, ROCKETNET_MAX_FRAGMENTS, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 1024 );
    const uint64_t budget = (uint64_t) ROCKETNET_REASSEMBLY_MEMORY_MIB * 1024 * 1024;
    check( required != 0 );
    check( required > budget );
    check( !rocketnet_reassembly_memory_allowed(
        1024, ROCKETNET_MAX_FRAGMENTS, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 1024 ) );

    cm_install();

    const uint64_t before = cm_calls();
    check( rocketnet_reassembly_set_create( NULL, 1024, ROCKETNET_MAX_FRAGMENTS, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 1024 ) == NULL );
    check( cm_calls() == before );
    cm_restore();
}

// RNConnectionMemory Use requires ~exhausted for every way a send takes room,
// the large-message span path too (rn_region_send_alloc_span). A region
// exhausted by its descriptor table, with one descriptor then returned, still
// refuses a span: nothing is taken, and each refusal counts
// MESSAGE_MEMORY_EXHAUSTED exactly as rn_region_send_alloc does.

static void test_exhausted_region_refuses_spans()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit capacity
    config.send_queue_messages = 16;
    check( config.max_message_bytes >= (int) RN_SPAN_MIN_BYTES );

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    uint32_t descriptors[16];

    for ( int i = 0; i < 16; i++ )
    {
        descriptors[i] = rn_region_send_alloc( region, 10 );
        check( descriptors[i] != RN_DESCRIPTOR_NONE );
    }

    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_exhausted( region ) );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 1 );

    // room returns (an ack), and the span ring is empty: a span would fit

    rn_region_send_free( region, descriptors[3] );
    check( region->descriptors_free == 1 );
    check( region->span_first == RN_DESCRIPTOR_NONE );

    check( rn_region_send_alloc_span( region, (int) RN_SPAN_MIN_BYTES ) == RN_DESCRIPTOR_NONE );
    check( region->descriptors_free == 1 );                         // nothing taken
    check( region->span_first == RN_DESCRIPTOR_NONE );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 2 );

    check( rn_region_send_alloc_span( region, (int) RN_SPAN_MIN_BYTES ) == RN_DESCRIPTOR_NONE );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == 3 );
    check( rn_connection_memory_decide( region ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );

    rn_arena_destroy( arena );
}

// RNConnectionMemory Reuse: the reset clears the sticky state, and the span
// path takes room again.

static void test_span_granted_after_reset()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit capacity
    config.send_queue_messages = 16;

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    for ( int i = 0; i < 16; i++ )
    {
        check( rn_region_send_alloc( region, 10 ) != RN_DESCRIPTOR_NONE );
    }
    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_exhausted( region ) );
    check( rn_region_send_alloc_span( region, (int) RN_SPAN_MIN_BYTES ) == RN_DESCRIPTOR_NONE );

    rn_region_reset( region );
    check( !rn_region_exhausted( region ) );

    const uint64_t before = counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED];
    const uint32_t d = rn_region_send_alloc_span( region, (int) RN_SPAN_MIN_BYTES );
    check( d != RN_DESCRIPTOR_NONE );
    check( region->descriptors_free == 15 );
    check( region->span_first == d );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == before );

    rn_region_send_free( region, d );
    check( region->descriptors_free == 16 );
    check( region->span_first == RN_DESCRIPTOR_NONE );

    rn_arena_destroy( arena );
}

// Generation-tagged slots: a descriptor's generation bumps on every take and
// survives its free, the free stack is exact LIFO, and a chunk slot's
// generation bumps on every take of that chunk.

#if ROCKETNET_DEV
static void test_generation_tagged_slots_and_lifo_stack()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0;
    config.send_queue_messages = 16;

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    const uint32_t a = rn_region_send_alloc( region, 10 );
    const uint32_t b = rn_region_send_alloc( region, 10 );
    const uint32_t c = rn_region_send_alloc( region, 10 );
    check( a != RN_DESCRIPTOR_NONE && b != RN_DESCRIPTOR_NONE && c != RN_DESCRIPTOR_NONE );
    check( region->send_descriptors[a].generation == 1 );

    const uint32_t chunk_a = region->send_descriptors[a].chunk;
    check( rn_send_store_chunk_generation( &region->store, chunk_a ) == 1 );

    // exact LIFO: free b then a; the next takes are a then b

    rn_region_send_free( region, b );
    rn_region_send_free( region, a );
    check( region->descriptors_free_top == 2 );
    check( rn_region_send_alloc( region, 10 ) == a );
    check( region->send_descriptors[a].generation == 2 );
    check( rn_send_store_chunk_generation( &region->store, region->send_descriptors[a].chunk ) >= 1 );
    check( rn_region_send_alloc( region, 10 ) == b );
    check( region->send_descriptors[b].generation == 2 );

    // a chunk's generation survives its free, and a second free of its
    // handle is refused

    const uint32_t live_chunk = region->send_descriptors[a].chunk;
    const uint32_t gen_before = rn_send_store_chunk_generation( &region->store, live_chunk );
    rn_region_send_free( region, a );
    check( rn_send_store_chunk_generation( &region->store, live_chunk ) == gen_before );
    check( !rn_send_store_free_checked( &region->store, live_chunk ) );

    const uint32_t a2 = rn_region_send_alloc( region, 10 );
    check( a2 == a );
    check( region->send_descriptors[a2].chunk == live_chunk );     // the lowest free chunk
    check( rn_send_store_chunk_generation( &region->store, live_chunk ) == gen_before + 1 );

    rn_arena_destroy( arena );
}
#endif // #if ROCKETNET_DEV

// Live spans are linked in allocation order: the oldest live span pins the
// reclaim floor while interior spans churn, holes are never overwritten, and
// the floor advances only when the oldest span is freed.

static void test_span_links_pin_oldest_live()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0;
    config.send_queue_messages = 16;

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );
    check( region->span_bytes >= 4 * 4096 );

    const int span_bytes = 2048;
    const uint32_t s0 = rn_region_send_alloc_span( region, span_bytes );
    const uint32_t s1 = rn_region_send_alloc_span( region, span_bytes );
    const uint32_t s2 = rn_region_send_alloc_span( region, span_bytes );
    check( s0 != RN_DESCRIPTOR_NONE && s1 != RN_DESCRIPTOR_NONE && s2 != RN_DESCRIPTOR_NONE );
    check( region->span_first == s0 && region->span_last == s2 );
    check( region->send_descriptors[s0].span_prev == RN_DESCRIPTOR_NONE && region->send_descriptors[s0].span_next == s1 );
    check( region->send_descriptors[s1].span_prev == s0 && region->send_descriptors[s1].span_next == s2 );
    check( region->send_descriptors[s2].span_prev == s1 && region->send_descriptors[s2].span_next == RN_DESCRIPTOR_NONE );

    const uint32_t floor = region->span_head;
    check( floor == region->send_descriptors[s0].span_start );

    // interior churn: free and retake the middle span; the floor stays pinned

    rn_region_send_free( region, s1 );
    check( region->span_first == s0 && region->span_head == floor );
    check( region->send_descriptors[s0].span_next == s2 && region->send_descriptors[s2].span_prev == s0 );

    const uint32_t s1b = rn_region_send_alloc_span( region, span_bytes );
    check( s1b != RN_DESCRIPTOR_NONE );
    check( region->span_first == s0 && region->span_head == floor );
    check( region->send_descriptors[s2].span_next == s1b && region->send_descriptors[s1b].span_prev == s2 );
    check( region->span_last == s1b );

    // freeing the oldest advances the floor to the next-oldest live span

    rn_region_send_free( region, s0 );
    check( region->span_first == s2 );
    check( region->span_head == region->send_descriptors[s2].span_start );

    rn_region_send_free( region, s2 );
    rn_region_send_free( region, s1b );
    check( region->span_first == RN_DESCRIPTOR_NONE && region->span_head == 0 && region->span_tail == 0 );

    rn_arena_destroy( arena );
}

// A send descriptor's generation wraps to 1, never to 0: a resend link
// stamped 0 is never unlinked by the ack while its descriptor is in use, so
// the number would be resent until the connection timed out
// (proofs/tla/RNLazyPool.tla HeldGenerationPositive).

static void test_message_descriptor_generation_never_zero()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0;
    config.send_queue_messages = 16;

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    const uint32_t d = rn_region_send_take( region, 10 );
    check( d != RN_DESCRIPTOR_NONE );
    rn_region_send_free( region, d );

    region->send_descriptors[d].generation = UINT32_MAX;

    check( rn_region_send_take( region, 10 ) == d );
    check( region->send_descriptors[d].generation == 1 );

    rn_arena_destroy( arena );
}

// A chunk's generation tag wraps to 1, never to 0: 0 is never a live name
// (proofs/tla/RNSendStoreLayout.tla HeldGenerationPositive).

#if ROCKETNET_DEV
static void test_store_chunk_generation_never_zero()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );

    const uint32_t d = rn_region_send_take( region, 10 );
    check( d != RN_DESCRIPTOR_NONE );
    const uint32_t chunk = region->send_descriptors[d].chunk;
    check( rn_send_store_chunk_generation( &region->store, chunk ) == 1 );
    rn_region_send_free( region, d );

    // the freed chunk at the last generation: its next take wraps past 0

    rn_store_class_t * cls = &region->store.classes[rn_send_store_chunk_class( chunk )];
    cls->meta[chunk & 0x3FFFFFFFu] = RN_META_GENERATION;

    const uint32_t again = rn_region_send_take( region, 10 );
    check( again != RN_DESCRIPTOR_NONE );
    check( region->send_descriptors[again].chunk == chunk );
    check( rn_send_store_chunk_generation( &region->store, chunk ) == 1 );

    rn_region_send_free( region, again );
    check( rn_region_send_take( region, 10 ) != RN_DESCRIPTOR_NONE );
    check( rn_send_store_chunk_generation( &region->store, chunk ) == 2 );

    rn_arena_destroy( arena );
}
#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_memory_isolation_between_slots );
REGISTER_UNIT_TEST( test_reassembly_geometry_refuses_bad_arguments_and_budget );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_exhaustion_is_a_kick_with_its_reason );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_reused_slot_starts_clean );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_user_block_is_used_and_nothing_else );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_connection_memory_no_allocation_after_create );
REGISTER_UNIT_TEST( test_exhausted_region_refuses_sends_until_reset );
REGISTER_UNIT_TEST( test_receive_without_room_exhausts_the_region );
REGISTER_UNIT_TEST( test_exhausted_region_refuses_spans );
REGISTER_UNIT_TEST( test_span_granted_after_reset );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_generation_tagged_slots_and_lifo_stack );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_span_links_pin_oldest_live );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_store_chunk_generation_never_zero );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_message_descriptor_generation_never_zero );
