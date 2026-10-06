/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_allocator.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

// the section 14 working figures, pinned: a struct that grows changes the
// formula with it, and the formula in docs/CHANNELS.md is changed in the
// same commit

static_assert( sizeof(rn_channel_send_t) == 32, "rn_channel_send_t is 32 bytes (CHANNELS.md 7.1)" );
static_assert( sizeof(rn_channel_receive_t) == 16, "rn_channel_receive_t is 16 bytes (CHANNELS.md 7.1)" );
static_assert( sizeof(rn_send_number_t) == 32, "the sender's per-number entry is 32 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_receive_descriptor_t) == 32, "the receiver's descriptor is 32 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_held_entry_t) == 16, "a held-ordered index entry is 16 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_send_descriptor_t) == 64, "a send descriptor is 64 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_sent_packet_t) == 32, "a sent-packet entry is 32 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_sent_index_entry_t) == 16, "a sent-packet index entry is 16 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_delivery_t) == 24, "a delivered entry is 24 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_sequenced_hold_t) == 1208, "a sequenced hold is 1208 bytes (CHANNELS.md 14)" );
static_assert( sizeof(rn_receipt_t) == 16, "a receipt is 16 bytes (CHANNELS.md 14)" );
static_assert( ROCKETNET_MESSAGE_FRAGMENT_BYTES % 64 == 0, "the block stride keeps 1152-class chunks 64-aligned" );

#define RN_STORE_CLASS_SHIFT                    30
#define RN_STORE_INDEX_MASK                     0x3FFFFFFFu

// a free chunk carries this at byte 8, so a chunk freed twice is caught in
// builds with asserts. it is cleared on alloc; a live message would have to
// write these exact 8 bytes there to trip a false double free.
#define RN_STORE_FREE_MAGIC                     0xF4EEC4A1F4EEC4A1ULL

static const int rn_store_chunk_bytes[RN_SEND_STORE_NUM_CLASSES] = { 128, 512, ROCKETNET_MESSAGE_FRAGMENT_BYTES };

static uint64_t rn_align( uint64_t value )
{
    return ( value + RN_REGION_ALIGNMENT - 1 ) & ~(uint64_t) ( RN_REGION_ALIGNMENT - 1 );
}

// ----------------------------------------------------------------
//                            the layout
// ----------------------------------------------------------------

// docs/CHANNELS.md section 14: validation, then the formula, part by part.
// Every part is carved at a 64-byte aligned offset, so the exact region is
// the formula plus at most 63 bytes a part; at the defaults the padding is 0.

bool rn_region_layout( const struct rocketnet_message_config_t * config, struct rn_region_layout_t * layout )
{
    rocketnet_assert( layout );

    struct rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }

    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_message_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "message", config, fields, num_fields ) )
        return false;

    const int F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;

    // the cross rules of section 14, refused and never clamped
    if ( ( config->window_messages & ( config->window_messages - 1 ) ) != 0 )
    {
        rocketnet_error( "message config window_messages %d is not a power of two", config->window_messages );
        return false;
    }
    if ( config->receive_ring_bytes % 16 != 0 )
    {
        rocketnet_error( "message config receive_ring_bytes %d is not a multiple of 16", config->receive_ring_bytes );
        return false;
    }
    if ( config->max_message_bytes > config->receive_ring_bytes )
    {
        rocketnet_error( "message config max_message_bytes %d is above receive_ring_bytes %d", config->max_message_bytes, config->receive_ring_bytes );
        return false;
    }
    const int max_fragments = ( config->max_message_bytes + F - 1 ) / F;
    if ( max_fragments > config->window_messages )
    {
        rocketnet_error( "message config max_message_bytes %d takes %d fragments, more than window_messages %d", config->max_message_bytes, max_fragments, config->window_messages );
        return false;
    }

    // the rto clamp (section 6.4) needs min <= initial <= max
    if ( config->rto_min_ms > config->rto_max_ms || config->rto_initial_ms < config->rto_min_ms || config->rto_initial_ms > config->rto_max_ms )
    {
        rocketnet_error( "message config rto needs rto_min_ms %d <= rto_initial_ms %d <= rto_max_ms %d", config->rto_min_ms, config->rto_initial_ms, config->rto_max_ms );
        return false;
    }

    memset( layout, 0, sizeof(*layout) );
    layout->num_channels = config->num_channels;
    layout->window_messages = config->window_messages;
    layout->receive_ring_bytes = config->receive_ring_bytes;
    layout->max_message_bytes = config->max_message_bytes;
    layout->send_buffer_bytes = config->send_buffer_bytes;
    layout->send_queue_messages = config->send_queue_messages;
    layout->packets_in_flight = config->packets_in_flight;
    layout->unreliable_deliveries = config->unreliable_deliveries;
    layout->sequenced_holds = config->sequenced_holds;
    layout->receipt_ring = config->receipt_ring;

    const uint64_t C = (uint64_t) config->num_channels;
    const uint64_t W = (uint64_t) config->window_messages;
    const uint64_t P = (uint64_t) config->packets_in_flight;

    const uint64_t entries[RN_REGION_NUM_PARTS] =
    {
        C,                                                          // RN_REGION_CHANNEL_SEND
        C,                                                          // RN_REGION_CHANNEL_RECEIVE
        W,                                                          // RN_REGION_SEND_NUMBERS
        W,                                                          // RN_REGION_RECEIVE_DESCRIPTORS
        W,                                                          // RN_REGION_RECEIVED_BITS (bits)
        2 * W,                                                      // RN_REGION_HELD_INDEX
        (uint64_t) config->receive_ring_bytes,                      // RN_REGION_RECEIVE_RING
        (uint64_t) config->send_queue_messages,                     // RN_REGION_SEND_DESCRIPTORS
        (uint64_t) config->send_buffer_bytes,                       // RN_REGION_SEND_STORE
        P,                                                          // RN_REGION_SENT_PACKETS
        2 * P,                                                      // RN_REGION_SENT_INDEX
        16 * P,                                                     // RN_REGION_SENT_REFS
        W + (uint64_t) config->unreliable_deliveries,               // RN_REGION_DELIVERIES
        (uint64_t) config->sequenced_holds,                         // RN_REGION_SEQUENCED_HOLDS
        (uint64_t) config->receipt_ring,                            // RN_REGION_RECEIPTS
    };

    const uint64_t entry_bytes[RN_REGION_NUM_PARTS] =
    {
        sizeof(rn_channel_send_t),
        sizeof(rn_channel_receive_t),
        sizeof(rn_send_number_t),
        sizeof(rn_receive_descriptor_t),
        0,                                                          // bits, below
        sizeof(rn_held_entry_t),
        1,
        sizeof(rn_send_descriptor_t),
        1,
        sizeof(rn_sent_packet_t),
        sizeof(rn_sent_index_entry_t),
        sizeof(uint32_t),
        sizeof(rn_delivery_t),
        sizeof(rn_sequenced_hold_t),
        sizeof(rn_receipt_t),
    };

    uint64_t end = 0;
    for ( int i = 0; i < RN_REGION_NUM_PARTS; i++ )
    {
        layout->part_entries[i] = entries[i];
        layout->part_bytes[i] = i == RN_REGION_RECEIVED_BITS ? W / 8 : entries[i] * entry_bytes[i];
        layout->part_offset[i] = rn_align( end );
        end = layout->part_offset[i] + layout->part_bytes[i];
        layout->formula_bytes += layout->part_bytes[i];
    }
    layout->region_bytes = rn_align( end );

    // the send store: three classes, a third of S each by bytes, 64-aligned.
    // The 1152 class keeps one u32 link per chunk at the head of its own
    // third -- its free list and a block's chain -- so the links cost no
    // byte outside S and the formula stands.
    const uint64_t third = ( (uint64_t) config->send_buffer_bytes / 3 ) & ~(uint64_t) ( RN_REGION_ALIGNMENT - 1 );
    for ( int c = 0; c < RN_SEND_STORE_NUM_CLASSES; c++ )
        layout->store_chunk_bytes[c] = rn_store_chunk_bytes[c];
    layout->store_chunks[0] = (int) ( third / 128 );
    layout->store_chunks[1] = (int) ( third / 512 );
    uint64_t blocks = third / (uint64_t) F;
    while ( blocks > 0 && rn_align( 4 * blocks ) + blocks * (uint64_t) F > third )
        blocks--;
    layout->store_chunks[2] = (int) blocks;
    layout->store_class_offset[0] = 0;
    layout->store_class_offset[1] = third;
    layout->store_links_offset = 2 * third;
    layout->store_class_offset[2] = 2 * third + rn_align( 4 * blocks );

    if ( layout->store_chunks[2] < max_fragments )
    {
        rocketnet_error( "message config send_buffer_bytes %d leaves %d chunks of %d bytes, and max_message_bytes %d needs %d",
                         config->send_buffer_bytes, layout->store_chunks[2], F, config->max_message_bytes, max_fragments );
        return false;
    }

    return true;
}

uint64_t rocketnet_message_connection_bytes( const struct rocketnet_message_config_t * config )
{
    struct rn_region_layout_t layout;
    if ( !rn_region_layout( config, &layout ) )
        return 0;
    return layout.region_bytes;
}

// ----------------------------------------------------------------
//                            the send store
// ----------------------------------------------------------------

// docs/CHANNELS.md section 4, item 2. Pure: no clock, no allocation; the one
// side effect outside the store is the refusal counter. Modelled by
// RNSendStore (docs/CHANNELS.md 15.2): NoDoubleAllocate, NoLeak and
// RefuseOnlyWhenFull, with alloc_block's all-or-none check and the
// double-free guard as the two rules its reversed witnesses remove.

static void rn_send_store_refused( struct rn_send_store_t * store )
{
    if ( store->counters )
        rocketnet_platform_atomic_add_u64( &store->counters[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL], 1 );
}

static uint8_t * rn_send_store_chunk_at( const struct rn_send_store_t * store, int c, uint32_t index )
{
    return store->chunk_base[c] + (uint64_t) index * (uint64_t) store->chunk_bytes[c];
}

static void rn_send_store_mark_free( uint8_t * chunk )
{
    const uint64_t magic = RN_STORE_FREE_MAGIC;
    memcpy( chunk + 8, &magic, sizeof(magic) );
}

static bool rn_send_store_marked_free( const uint8_t * chunk )
{
    uint64_t magic;
    memcpy( &magic, chunk + 8, sizeof(magic) );
    return magic == RN_STORE_FREE_MAGIC;
}

static void rn_send_store_mark_used( uint8_t * chunk )
{
    memset( chunk + 8, 0, sizeof(uint64_t) );
}

static uint32_t rn_send_store_next_free( const struct rn_send_store_t * store, int c, uint32_t index )
{
    if ( c == 2 )
        return store->links[index];
    uint32_t next;
    memcpy( &next, rn_send_store_chunk_at( store, c, index ), sizeof(next) );
    return next;
}

static void rn_send_store_push_free( struct rn_send_store_t * store, int c, uint32_t index )
{
    uint8_t * chunk = rn_send_store_chunk_at( store, c, index );
    rocketnet_assert( !rn_send_store_marked_free( chunk ) );       // freed twice
    if ( c == 2 )
        store->links[index] = store->free_head[c];
    else
        memcpy( chunk, &store->free_head[c], sizeof(uint32_t) );
    rn_send_store_mark_free( chunk );
    store->free_head[c] = index;
    store->free_count[c]++;
    rocketnet_assert( store->free_count[c] <= store->chunks[c] );
}

static void rn_send_store_init( struct rn_send_store_t * store, const struct rn_region_layout_t * layout, uint8_t * base, uint64_t * counters )
{
    memset( store, 0, sizeof(*store) );
    store->counters = counters;
    store->max_message_bytes = layout->max_message_bytes;
    store->links = (uint32_t*) ( base + layout->store_links_offset );
    for ( int c = 0; c < RN_SEND_STORE_NUM_CLASSES; c++ )
    {
        store->chunk_base[c] = base + layout->store_class_offset[c];
        store->chunks[c] = layout->store_chunks[c];
        store->chunk_bytes[c] = layout->store_chunk_bytes[c];
        store->free_head[c] = RN_SEND_STORE_NONE;
        store->free_count[c] = 0;

        // pushed in reverse, so the list hands out chunks in address order
        for ( int i = store->chunks[c] - 1; i >= 0; i-- )
        {
            rn_send_store_mark_used( rn_send_store_chunk_at( store, c, (uint32_t) i ) );
            rn_send_store_push_free( store, c, (uint32_t) i );
        }
    }
}

static uint32_t rn_send_store_pop_free( struct rn_send_store_t * store, int c )
{
    const uint32_t index = store->free_head[c];
    rocketnet_assert( index != RN_SEND_STORE_NONE );
    store->free_head[c] = rn_send_store_next_free( store, c, index );
    store->free_count[c]--;
    uint8_t * chunk = rn_send_store_chunk_at( store, c, index );
    rocketnet_assert( rn_send_store_marked_free( chunk ) );
    rn_send_store_mark_used( chunk );
    if ( c == 2 )
        store->links[index] = RN_SEND_STORE_NONE;
    return index;
}

uint32_t rn_send_store_alloc( struct rn_send_store_t * store, int bytes )
{
    rocketnet_assert( store );
    if ( bytes < 1 || bytes > ROCKETNET_MESSAGE_FRAGMENT_BYTES )
        return RN_SEND_STORE_NONE;
    const int c = bytes <= 128 ? 0 : ( bytes <= 512 ? 1 : 2 );
    if ( store->free_count[c] == 0 )
    {
        rn_send_store_refused( store );
        return RN_SEND_STORE_NONE;
    }
    return ( (uint32_t) c << RN_STORE_CLASS_SHIFT ) | rn_send_store_pop_free( store, c );
}

uint32_t rn_send_store_alloc_block( struct rn_send_store_t * store, int bytes )
{
    rocketnet_assert( store );
    if ( bytes <= ROCKETNET_MESSAGE_FRAGMENT_BYTES || bytes > store->max_message_bytes )
        return RN_SEND_STORE_NONE;
    const int k = ( bytes + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES;

    // all or none: decided before anything is taken
    if ( store->free_count[2] < k )
    {
        rn_send_store_refused( store );
        return RN_SEND_STORE_NONE;
    }

    const uint32_t first = rn_send_store_pop_free( store, 2 );
    uint32_t last = first;
    for ( int i = 1; i < k; i++ )
    {
        const uint32_t next = rn_send_store_pop_free( store, 2 );
        store->links[last] = next;
        last = next;
    }
    store->links[last] = RN_SEND_STORE_NONE;
    return ( 2u << RN_STORE_CLASS_SHIFT ) | first;
}

void rn_send_store_free( struct rn_send_store_t * store, uint32_t chunk )
{
    rocketnet_assert( store );
    rocketnet_assert( chunk != RN_SEND_STORE_NONE );
    const int c = (int) ( chunk >> RN_STORE_CLASS_SHIFT );
    uint32_t index = chunk & RN_STORE_INDEX_MASK;
    rocketnet_assert( c < RN_SEND_STORE_NUM_CLASSES );
    rocketnet_assert( index < (uint32_t) store->chunks[c] );
    if ( c != 2 )
    {
        rn_send_store_push_free( store, c, index );
        return;
    }
    // a whole block from its first chunk: O(k)
    while ( index != RN_SEND_STORE_NONE )
    {
        const uint32_t next = store->links[index];
        rocketnet_assert( next == RN_SEND_STORE_NONE || next < (uint32_t) store->chunks[2] );
        rn_send_store_push_free( store, 2, index );
        index = next;
    }
}

uint8_t * rn_send_store_chunk_data( const struct rn_send_store_t * store, uint32_t chunk )
{
    rocketnet_assert( store );
    const int c = (int) ( chunk >> RN_STORE_CLASS_SHIFT );
    const uint32_t index = chunk & RN_STORE_INDEX_MASK;
    rocketnet_assert( c < RN_SEND_STORE_NUM_CLASSES && index < (uint32_t) store->chunks[c] );
    return rn_send_store_chunk_at( store, c, index );
}

int rn_send_store_chunk_bytes( const struct rn_send_store_t * store, uint32_t chunk )
{
    rocketnet_assert( store );
    return store->chunk_bytes[chunk >> RN_STORE_CLASS_SHIFT];
}

uint32_t rn_send_store_block_next( const struct rn_send_store_t * store, uint32_t chunk )
{
    rocketnet_assert( store );
    if ( ( chunk >> RN_STORE_CLASS_SHIFT ) != 2 )
        return RN_SEND_STORE_NONE;
    const uint32_t next = store->links[chunk & RN_STORE_INDEX_MASK];
    return next == RN_SEND_STORE_NONE ? RN_SEND_STORE_NONE : ( ( 2u << RN_STORE_CLASS_SHIFT ) | next );
}

// ----------------------------------------------------------------
//                            the region
// ----------------------------------------------------------------

// Carved at connect (docs/CHANNELS.md section 4), in memory allocated at
// create. The fixed tables are zeroed so a reconnect starts from nothing the
// last connection left; the ring and the store's data are not, because no
// layer reads them before writing them.

void rn_region_carve( struct rn_region_t * region, const struct rn_region_layout_t * layout, void * memory, uint64_t * counters )
{
    rocketnet_assert( region );
    rocketnet_assert( layout );
    rocketnet_assert( memory );
    rocketnet_assert( ( (uintptr_t) memory ) % RN_REGION_ALIGNMENT == 0 );

    uint8_t * base = (uint8_t*) memory;
    region->base = base;
    region->layout = *layout;

    for ( int i = 0; i < RN_REGION_NUM_PARTS; i++ )
    {
        if ( i == RN_REGION_RECEIVE_RING || i == RN_REGION_SEND_STORE )
            continue;
        memset( base + layout->part_offset[i], 0, (size_t) layout->part_bytes[i] );
    }

    region->channel_send = (rn_channel_send_t*) ( base + layout->part_offset[RN_REGION_CHANNEL_SEND] );
    region->channel_receive = (rn_channel_receive_t*) ( base + layout->part_offset[RN_REGION_CHANNEL_RECEIVE] );
    region->send_numbers = (rn_send_number_t*) ( base + layout->part_offset[RN_REGION_SEND_NUMBERS] );
    region->receive_descriptors = (rn_receive_descriptor_t*) ( base + layout->part_offset[RN_REGION_RECEIVE_DESCRIPTORS] );
    region->received_bits = (uint64_t*) ( base + layout->part_offset[RN_REGION_RECEIVED_BITS] );
    region->held_index = (rn_held_entry_t*) ( base + layout->part_offset[RN_REGION_HELD_INDEX] );
    region->receive_ring = base + layout->part_offset[RN_REGION_RECEIVE_RING];
    region->send_descriptors = (rn_send_descriptor_t*) ( base + layout->part_offset[RN_REGION_SEND_DESCRIPTORS] );
    region->sent_packets = (rn_sent_packet_t*) ( base + layout->part_offset[RN_REGION_SENT_PACKETS] );
    region->sent_index = (rn_sent_index_entry_t*) ( base + layout->part_offset[RN_REGION_SENT_INDEX] );
    region->sent_refs = (uint32_t*) ( base + layout->part_offset[RN_REGION_SENT_REFS] );
    region->deliveries = (rn_delivery_t*) ( base + layout->part_offset[RN_REGION_DELIVERIES] );
    region->sequenced_holds = (rn_sequenced_hold_t*) ( base + layout->part_offset[RN_REGION_SEQUENCED_HOLDS] );
    region->receipts = (rn_receipt_t*) ( base + layout->part_offset[RN_REGION_RECEIPTS] );

    rn_send_store_init( &region->store, layout, base + layout->part_offset[RN_REGION_SEND_STORE], counters );

    // the descriptor pool, handed out in index order
    const uint32_t Q = (uint32_t) layout->send_queue_messages;
    for ( uint32_t i = 0; i < Q; i++ )
        region->send_descriptors[i].next = i + 1 < Q ? i + 1 : RN_SEND_STORE_NONE;
    region->descriptor_free_head = 0;
    region->descriptor_free_count = (int) Q;
}

uint32_t rn_region_alloc_descriptor( struct rn_region_t * region )
{
    rocketnet_assert( region );
    const uint32_t d = region->descriptor_free_head;
    if ( d == RN_SEND_STORE_NONE )
    {
        if ( region->store.counters )
            rocketnet_platform_atomic_add_u64( &region->store.counters[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL], 1 );
        return RN_SEND_STORE_NONE;
    }
    region->descriptor_free_head = region->send_descriptors[d].next;
    region->send_descriptors[d].next = RN_SEND_STORE_NONE;
    region->descriptor_free_count--;
    return d;
}

void rn_region_free_descriptor( struct rn_region_t * region, uint32_t descriptor )
{
    rocketnet_assert( region );
    rocketnet_assert( descriptor < (uint32_t) region->layout.send_queue_messages );
    region->send_descriptors[descriptor].next = region->descriptor_free_head;
    region->descriptor_free_head = descriptor;
    region->descriptor_free_count++;
    rocketnet_assert( region->descriptor_free_count <= region->layout.send_queue_messages );
}

bool rn_region_contains( const struct rn_region_t * region, const void * p, uint64_t bytes )
{
    rocketnet_assert( region );
    const uintptr_t begin = (uintptr_t) region->base;
    const uintptr_t at = (uintptr_t) p;
    if ( at < begin )
        return false;
    const uint64_t offset = (uint64_t) ( at - begin );
    return offset <= region->layout.region_bytes && bytes <= region->layout.region_bytes - offset;
}

// ----------------------------------------------------------------
//                            the arena
// ----------------------------------------------------------------

bool rn_arena_create( struct rn_arena_t * arena, void * context, const struct rocketnet_message_config_t * config, int num_regions )
{
    rocketnet_assert( arena );
    memset( arena, 0, sizeof(*arena) );
    if ( num_regions < 1 )
    {
        rocketnet_error( "message arena needs at least one region, not %d", num_regions );
        return false;
    }
    if ( !rn_region_layout( config, &arena->layout ) )
        return false;
    const uint64_t total = arena->layout.region_bytes * (uint64_t) num_regions;
    if ( total / (uint64_t) num_regions != arena->layout.region_bytes || total > (uint64_t) SIZE_MAX )
    {
        rocketnet_error( "message arena of %d regions of %" PRIu64 " bytes overflows", num_regions, arena->layout.region_bytes );
        return false;
    }
    arena->memory = (uint8_t*) rocketnet_malloc( context, (size_t) total );
    if ( !arena->memory )
    {
        rocketnet_error( "message arena could not allocate %" PRIu64 " bytes", total );
        return false;
    }
    arena->context = context;
    arena->num_regions = num_regions;
    return true;
}

void rn_arena_destroy( struct rn_arena_t * arena )
{
    rocketnet_assert( arena );
    rocketnet_free( arena->context, arena->memory );
    arena->memory = NULL;
    arena->num_regions = 0;
}

void * rn_arena_region_memory( const struct rn_arena_t * arena, int region_index )
{
    rocketnet_assert( arena );
    rocketnet_assert( arena->memory );
    rocketnet_assert( region_index >= 0 && region_index < arena->num_regions );
    return arena->memory + (uint64_t) region_index * arena->layout.region_bytes;
}

// ----------------------------------------------------------------
//                       the sizing, as text
// ----------------------------------------------------------------

#define RN_SIZING_TEXT_BYTES 4096

static void rn_sizing_append( char * text, int * length, const char * format, ... )
{
    if ( *length >= RN_SIZING_TEXT_BYTES - 1 )
        return;
    va_list args;
    va_start( args, format );
    const int n = vsnprintf( text + *length, (size_t) ( RN_SIZING_TEXT_BYTES - *length ), format, args );
    va_end( args );
    if ( n > 0 )
        *length += n < RN_SIZING_TEXT_BYTES - *length ? n : RN_SIZING_TEXT_BYTES - 1 - *length;
}

static int rn_sizing_text( const struct rocketnet_message_config_t * config, int num_connections, char * text )
{
    struct rn_region_layout_t layout;
    if ( num_connections < 1 || !rn_region_layout( config, &layout ) )
        return 0;

    const uint64_t * b = layout.part_bytes;
    const uint64_t total = layout.region_bytes * (uint64_t) num_connections;
    int length = 0;
    text[0] = '\0';
    rn_sizing_append( text, &length, "message connection sizing (docs/CHANNELS.md section 14)\n" );
    rn_sizing_append( text, &length, "  connection_bytes = 48C + 96.125W + B + 64Q + S + 128P + 24(W + Du) + 1208Ru + 16Rq\n" );
    rn_sizing_append( text, &length, "  C=%d W=%d B=%d S=%d Q=%d P=%d Du=%d Ru=%d Rq=%d\n",
                      layout.num_channels, layout.window_messages, layout.receive_ring_bytes, layout.send_buffer_bytes,
                      layout.send_queue_messages, layout.packets_in_flight, layout.unreliable_deliveries,
                      layout.sequenced_holds, layout.receipt_ring );
    rn_sizing_append( text, &length, "  channel state              48*C          %12" PRIu64 "\n", b[RN_REGION_CHANNEL_SEND] + b[RN_REGION_CHANNEL_RECEIVE] );
    rn_sizing_append( text, &length, "  sender per-number table    32*W          %12" PRIu64 "\n", b[RN_REGION_SEND_NUMBERS] );
    rn_sizing_append( text, &length, "  receiver descriptors       32*W          %12" PRIu64 "\n", b[RN_REGION_RECEIVE_DESCRIPTORS] );
    rn_sizing_append( text, &length, "  received bitset            W/8           %12" PRIu64 "\n", b[RN_REGION_RECEIVED_BITS] );
    rn_sizing_append( text, &length, "  held-ordered index         16*2W         %12" PRIu64 "\n", b[RN_REGION_HELD_INDEX] );
    rn_sizing_append( text, &length, "  receive ring               B             %12" PRIu64 "\n", b[RN_REGION_RECEIVE_RING] );
    rn_sizing_append( text, &length, "  send descriptors           64*Q          %12" PRIu64 "\n", b[RN_REGION_SEND_DESCRIPTORS] );
    rn_sizing_append( text, &length, "  send store                 S             %12" PRIu64 "   128 x %d, 512 x %d, 1152 x %d\n",
                      b[RN_REGION_SEND_STORE], layout.store_chunks[0], layout.store_chunks[1], layout.store_chunks[2] );
    rn_sizing_append( text, &length, "  sent packets, index, refs  128*P         %12" PRIu64 "\n", b[RN_REGION_SENT_PACKETS] + b[RN_REGION_SENT_INDEX] + b[RN_REGION_SENT_REFS] );
    rn_sizing_append( text, &length, "  delivered entries          24*(W + Du)   %12" PRIu64 "\n", b[RN_REGION_DELIVERIES] );
    rn_sizing_append( text, &length, "  sequenced holds            1208*Ru       %12" PRIu64 "\n", b[RN_REGION_SEQUENCED_HOLDS] );
    rn_sizing_append( text, &length, "  receipt ring               16*Rq         %12" PRIu64 "\n", b[RN_REGION_RECEIPTS] );
    rn_sizing_append( text, &length, "  formula total                            %12" PRIu64 "\n", layout.formula_bytes );
    rn_sizing_append( text, &length, "  alignment padding                        %12" PRIu64 "\n", layout.region_bytes - layout.formula_bytes );
    rn_sizing_append( text, &length, "  region bytes (exact)                     %12" PRIu64 "\n", layout.region_bytes );
    rn_sizing_append( text, &length, "  x %d connections                         %12" PRIu64 "   %.1f MiB\n",
                      num_connections, total, (double) total / ( 1024.0 * 1024.0 ) );
    return length;
}

int rocketnet_message_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections, char * buffer, int buffer_bytes )
{
    char text[RN_SIZING_TEXT_BYTES];
    const int length = rn_sizing_text( config, num_connections, text );
    if ( buffer && buffer_bytes > 0 )
    {
        const int copy = length < buffer_bytes - 1 ? length : buffer_bytes - 1;
        memcpy( buffer, text, (size_t) copy );
        buffer[copy] = '\0';
    }
    return length;
}

void rocketnet_message_print_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections )
{
    char text[RN_SIZING_TEXT_BYTES];
    const int length = rn_sizing_text( config, num_connections, text );
    int start = 0;
    for ( int i = 0; i < length; i++ )
    {
        if ( text[i] != '\n' )
            continue;
        text[i] = '\0';
        rocketnet_printf( ROCKETNET_LOG_LEVEL_INFO, "%s", text + start );
        start = i + 1;
    }
}
