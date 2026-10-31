/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_allocator.h"
#include "rocketnet_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stddef.h>

#include <sodium.h>

// Layer 1, the region: proofs/tla/RNConnectionMemory.tla, sized per docs/LOAD.md Table 3, the
// send store checked by the section 4 tests.

// ----------------------------------------------------------------
//          the entry sizes, pinned to docs/LOAD.md Table 3's figures
// ----------------------------------------------------------------

static_assert( sizeof( rn_channel_send_t ) == 40, "channel send state is 40 bytes (14)" );
static_assert( sizeof( rn_channel_receive_t ) == 24, "channel receive state is 24 bytes (14)" );
static_assert( sizeof( rn_send_number_t ) == 32, "the sender's per-number entry is 32 bytes (14)" );
static_assert( sizeof( rn_receive_descriptor_t ) == 32, "the receiver's descriptor is 32 bytes (14)" );
static_assert( sizeof( rn_held_entry_t ) == 16, "a held-ordered index entry is 16 bytes (14)" );
static_assert( sizeof( rn_send_descriptor_t ) == 72, "a send descriptor is 72 bytes (14)" );
static_assert( sizeof( rn_sent_packet_t ) == 32, "a sent-packet entry is 32 bytes (14)" );
static_assert( sizeof( rn_sent_index_t ) == 16, "a sent-packet index entry is 16 bytes (14)" );
static_assert( sizeof( rn_delivery_t ) == 24, "a delivery is 24 bytes (14)" );
static_assert( sizeof( rn_sequenced_hold_t ) == 16, "sequenced hold metadata is 16 hot bytes (14)" );
static_assert( sizeof( rn_sequenced_payload_t ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, "a sequenced payload has one cold payload capacity (14)" );
static_assert( sizeof( rn_sequenced_hold_t ) + sizeof( rn_sequenced_payload_t ) == 1432, "separate hot and cold arrays retain the 1432Ru hold term (14)" );
static_assert( sizeof( rn_sequenced_hold_t ) + sizeof( rn_sequenced_payload_t ) + 2 * sizeof( uint32_t ) == 1440, "a hold with its free-stack and delivered-list indices is the 1440Ru region term (14)" );
static_assert( sizeof( rn_receipt_t ) == 16, "a receipt is 16 bytes (14)" );
static_assert( sizeof( rn_resend_link_t ) == 12, "a resend FIFO link is 12 bytes (14)" );
static_assert( sizeof( ( (rn_send_descriptor_t *) 0 )->reserved ) >= 2, "a send descriptor keeps two spare bytes (14)" );
static_assert( sizeof( rn_store_class_t ) >= 32, "a store class header stays a small fixed record" );
static_assert( RN_STORE_LARGE_BYTES % 16 == 0, "the block stride keeps ring placements 16-aligned (4)" );

#if ROCKETNET_DEV
static thread_local rn_span_cost_t rn_span_cost;
void rn_span_cost_reset() { memset( &rn_span_cost, 0, sizeof( rn_span_cost ) ); }
rn_span_cost_t rn_span_cost_read() { return rn_span_cost; }
#define RN_SPAN_COST_VISIT() ( rn_span_cost.edge_visits++ )
#else
#define RN_SPAN_COST_VISIT() ( (void) 0 )
#endif // #if ROCKETNET_DEV

static uint64_t rn_align64( uint64_t value )
{
    return ( value + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uint64_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 );
}

static void rn_count( rocketnet_counters_t * counters, int counter )
{
    // an object's regions share its counters, and different connections may send from different threads (16.1), so a refusal adds atomically

    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &counters->values[counter], 1 );
}

// ----------------------------------------------------------------
//                            the layout
// ----------------------------------------------------------------

static const char * const rn_part_names[RN_NUM_PARTS] = {
    "channel send state",
    "channel receive state",
    "sender per-number table",
    "sender first sends",
    "receiver descriptors",
    "received bitset",
    "held-ordered index",
    "receive ring",
    "send descriptors",
    "send store",
    "sent-packet ring",
    "sent-packet index",
    "sent-packet references",
    "delivered entries",
    "sequenced holds",
    "receipt ring",
    "resend FIFO",
    "unreliable ring",
    "send spans",
};

static const char * const rn_part_formulas[RN_NUM_PARTS] = {
    "40C",
    "24C",
    "32W",
    "8W",
    "32W",
    "W/8",
    "16*2W",
    "B",
    "72Q",
    "S",
    "32P",
    "16*2P",
    "4*16P",
    "24(W+Du)",
    "1440Ru",
    "16Rq",
    "12W",
    "8*2Q",
    "SR",
};

#if ROCKETNET_DEV
const char * rn_region_part_name( int part )
{
    return part >= 0 && part < RN_NUM_PARTS ? rn_part_names[part] : NULL;
}
#endif // #if ROCKETNET_DEV

// Section 14: each part is its formula's term exactly, at a 64-aligned offset
// (4: ROCKETNET_ALLOCATION_ALIGNMENT), so the ring base is 16-aligned and the
// exact region is the formula plus at most 63 bytes per part.

bool rn_region_layout( const rocketnet_message_config_t * config, rn_region_layout_t * layout )
{
    rocketnet_assert( layout );

    rocketnet_message_config_t defaults;

    if ( !config )
    {
        rocketnet_message_default_config( &defaults );

        config = &defaults;
    }

    if ( !rocketnet_message_validate_config( config ) )
        return false;

    const uint64_t C = (uint64_t) config->num_channels;
    const uint64_t W = (uint64_t) config->reliable_messages_in_flight;
    const uint64_t B = (uint64_t) config->receive_message_buffer_bytes;

    // Local create-time payload reserve. The documented typical payloads are
    // 128, 512 and 1024 bytes for the fixed pools; descriptors allow 128-byte
    // records. Tiny-record floods retain bounded exhaustion semantics.

    const int kbps = config->client_send_kbps > config->server_send_kbps ? config->client_send_kbps : config->server_send_kbps;
    const uint64_t reserve = kbps > 0 ? ( (uint64_t) kbps * 125 * (uint64_t) config->message_resend_delay_max_ms + 999 ) / 1000 : 0;
    const uint64_t Q = (uint64_t) config->send_queue_messages + ( reserve + 127 ) / 128;
    const uint64_t SR = B + rn_align64( reserve + ( ( reserve + 1152 ) / 1153 ) * 15 );
    const uint32_t chunk_bytes[RN_STORE_NUM_CLASSES] = { RN_STORE_SMALL_BYTES, RN_STORE_MEDIUM_BYTES, RN_STORE_LARGE_BYTES };
    const uint32_t typical_payload[RN_STORE_NUM_CLASSES] = { 128, 512, 1024 };
    uint32_t chunks[RN_STORE_NUM_CLASSES];
    uint64_t S = 0;

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const uint64_t payload_bytes = typical_payload[c];
        const uint64_t count = rn_send_store_chunks_for( (uint64_t) config->send_message_buffer_bytes, c ) + ( reserve + payload_bytes - 1 ) / payload_bytes;

        // a chunk's handle is its class in the top two bits and its index in
        // the low 30, and a class holds at most RN_HANDLE_INDEX chunks; a
        // config past that is refused whole

        if ( count > RN_HANDLE_INDEX )
        {
            rocketnet_error( "message config refused: the resend reserve of %" PRIu64 " bytes (send rate %d kbps for message_resend_delay_max_ms %d) needs %" PRIu64 " send store chunks of %u bytes, more than the %u a class holds",
                             reserve, kbps, config->message_resend_delay_max_ms, count, chunk_bytes[c], RN_HANDLE_INDEX );
            return false;
        }

        chunks[c] = (uint32_t) count;
        S += rn_align64( count * ( chunk_bytes[c] + sizeof( uint32_t ) ) );
    }

    // keep all original store headroom, including its alignment slack

    if ( S < (uint64_t) config->send_message_buffer_bytes )
        S = (uint64_t) config->send_message_buffer_bytes;
    if ( Q >= UINT32_MAX || SR > UINT32_MAX )
    {
        rocketnet_error( "message config refused: the resend reserve of %" PRIu64 " bytes (send rate %d kbps for message_resend_delay_max_ms %d) needs %" PRIu64 " send descriptors (below %u) and %" PRIu64 " receive reserve bytes (at most %u)",
                         reserve, kbps, config->message_resend_delay_max_ms, Q, UINT32_MAX, SR, UINT32_MAX );
        return false;
    }

    const uint64_t P = (uint64_t) config->packets_in_flight;
    const uint64_t Du = (uint64_t) config->unreliable_messages_per_update;
    const uint64_t Ru = (uint64_t) config->sequenced_messages_held;
    const uint64_t Rq = (uint64_t) config->max_message_receipts;

    memset( layout, 0, sizeof( *layout ) );
    layout->bytes[RN_PART_CHANNEL_SEND] = sizeof( rn_channel_send_t ) * C;
    layout->bytes[RN_PART_CHANNEL_RECEIVE] = sizeof( rn_channel_receive_t ) * C;
    layout->bytes[RN_PART_SEND_NUMBERS] = sizeof( rn_send_number_t ) * W;
    layout->bytes[RN_PART_SEND_FIRST_SENDS] = sizeof( double ) * W;
    layout->bytes[RN_PART_RECEIVE_DESCRIPTORS] = sizeof( rn_receive_descriptor_t ) * W;
    layout->bytes[RN_PART_RECEIVED_BITS] = W / 8;                                   // W is a power of two, at least 64: whole u64 words
    layout->bytes[RN_PART_HELD_INDEX] = sizeof( rn_held_entry_t ) * 2 * W;
    layout->bytes[RN_PART_RECEIVE_RING] = B;
    layout->bytes[RN_PART_SEND_DESCRIPTORS] = sizeof( rn_send_descriptor_t ) * Q;
    layout->bytes[RN_PART_SEND_STORE] = S;
    layout->bytes[RN_PART_SENT_PACKETS] = sizeof( rn_sent_packet_t ) * P;
    layout->bytes[RN_PART_SENT_INDEX] = sizeof( rn_sent_index_t ) * 2 * P;
    layout->bytes[RN_PART_SENT_REFS] = sizeof( uint32_t ) * 16 * P;
    layout->bytes[RN_PART_DELIVERIES] = sizeof( rn_delivery_t ) * ( W + Du );
    layout->bytes[RN_PART_SEQUENCED_HOLDS] = ( sizeof( rn_sequenced_hold_t ) + sizeof( rn_sequenced_payload_t ) + 2 * sizeof( uint32_t ) ) * Ru;
    layout->bytes[RN_PART_RECEIPTS] = sizeof( rn_receipt_t ) * Rq;
    layout->bytes[RN_PART_RESEND_FIFO] = sizeof( rn_resend_link_t ) * W;
    layout->bytes[RN_PART_UNRELIABLE_RING] = sizeof( rn_unreliable_entry_t ) * 2 * Q;
    layout->bytes[RN_PART_SEND_SPANS] = SR;                                         // local send reserve; receive-ring wire size stays B

    uint64_t offset = 0;

    for ( int part = 0; part < RN_NUM_PARTS; part++ )
    {
        layout->offset[part] = offset;
        layout->formula_bytes += layout->bytes[part];
        offset = rn_align64( offset + layout->bytes[part] );
    }

    layout->region_bytes = offset;

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        layout->store_chunks[c] = chunks[c];

    return true;
}

// ----------------------------------------------------------------
//                          the send store
// ----------------------------------------------------------------


static const uint32_t rn_class_bytes[RN_STORE_NUM_CLASSES] = { RN_STORE_SMALL_BYTES, RN_STORE_MEDIUM_BYTES, RN_STORE_LARGE_BYTES };

static uint64_t rn_send_store_third( uint64_t store_bytes )
{
    // each class a third of send_message_buffer_bytes by bytes (4), 64-aligned

    return ( store_bytes / RN_STORE_NUM_CLASSES ) & ~(uint64_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 );
}

uint32_t rn_send_store_chunks_for( uint64_t store_bytes, int class_index )
{
    rocketnet_assert( class_index >= 0 && class_index < RN_STORE_NUM_CLASSES );

    // each chunk with its u32 of meta, both inside the class's third

    return (uint32_t) ( rn_send_store_third( store_bytes ) / ( rn_class_bytes[class_index] + sizeof( uint32_t ) ) );
}

int rn_send_store_class_for( int bytes )
{
    // the smallest class that fits, and only that class: no fallback to a larger one (4)

    if ( bytes <= RN_STORE_SMALL_BYTES )
        return RN_STORE_SMALL;
    if ( bytes <= RN_STORE_MEDIUM_BYTES )
        return RN_STORE_MEDIUM;

    return RN_STORE_LARGE;
}

// proofs/tla/RNSendStoreLayout.tla Carve: nothing taken, no meta written

static void rn_send_store_init_counts( rn_send_store_t * store, uint8_t * memory, const uint32_t * counts )
{
    uint64_t offset = 0;

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        rn_store_class_t * cls = &store->classes[c];
        cls->chunk_bytes = rn_class_bytes[c];
        cls->num_chunks = counts[c];
        cls->data = memory + offset;
        cls->meta = (uint32_t *) ( cls->data + (uint64_t) cls->num_chunks * cls->chunk_bytes );
        cls->free_head = 0;
        cls->free_count = cls->num_chunks;
        cls->fresh = 0;
        offset += rn_align64( (uint64_t) cls->num_chunks * ( cls->chunk_bytes + sizeof( uint32_t ) ) );
    }
}

// a chunk is free when it was never taken (at or past the watermark, whose
// meta is never read) or its meta's live bit is clear

static bool rn_store_chunk_is_free( const rn_store_class_t * cls, uint32_t i )
{
    return i >= cls->fresh || ( cls->meta[i] & RN_META_LIVE ) == 0;
}

// proofs/tla/RNSendStoreLayout.tla Alloc's guard and Refuse: the lowest free
// chunk, scanned from free_head, which no free chunk lies below; the scan
// meets at most the held chunks between free_head and fresh, then fresh itself

bool rn_send_store_find( const rn_send_store_t * store, int bytes, uint32_t * out_index )
{
    rocketnet_assert( store && out_index );

    if ( bytes <= 0 || bytes > RN_STORE_LARGE_BYTES )
        return false;

    const rn_store_class_t * cls = &store->classes[rn_send_store_class_for( bytes )];
    if ( cls->free_count == 0 )
        return false;

    for ( uint32_t i = cls->free_head; i < cls->num_chunks; i++ )
    {
        if ( rn_store_chunk_is_free( cls, i ) )
        {
            *out_index = i;
            return true;
        }
    }

    return false;
}

// proofs/tla/RNSendStoreLayout.tla Alloc: the chunk is marked live under its
// next generation, and the watermark and the cursor move past it
// (proofs/tla/RNSendStore.tla Alloc)

uint32_t rn_send_store_take( rn_send_store_t * store, int bytes, uint32_t index )
{
    rocketnet_assert( store && bytes > 0 && bytes <= RN_STORE_LARGE_BYTES );

    const int c = rn_send_store_class_for( bytes );
    rn_store_class_t * cls = &store->classes[c];
    rocketnet_assert( index < cls->num_chunks && index <= cls->fresh && rn_store_chunk_is_free( cls, index ) );

    uint32_t generation = 1;

    if ( index < cls->fresh )
    {
        // 0 is never a live name (proofs/tla/RNSendStoreLayout.tla HeldGenerationPositive): the tag wraps to 1

        generation = ( cls->meta[index] + 1 ) & RN_META_GENERATION;
        if ( generation == 0 )
            generation = 1;
    }
    else
    {
        cls->fresh = index + 1;
    }

    cls->meta[index] = RN_META_LIVE | generation;
    cls->free_count--;
    cls->free_head = index + 1;

    return ( (uint32_t) c << 30 ) | index;
}

// proofs/tla/RNSendStoreLayout.tla Free: a held chunk's live bit cleared, its
// generation kept, and the cursor lowered to it. A handle that names no held
// chunk (RN_CHUNK_NONE, a class past the last, a chunk never taken, a chunk
// already free) is refused with nothing changed (proofs/tla/RNSendStore.tla
// Free and FreeAgain).

bool rn_send_store_free_checked( rn_send_store_t * store, uint32_t handle )
{
    rocketnet_assert( store );

    if ( handle == RN_CHUNK_NONE )
        return false;

    const int c = rn_send_store_chunk_class( handle );
    if ( c >= RN_STORE_NUM_CLASSES )
        return false;

    rn_store_class_t * cls = &store->classes[c];
    const uint32_t i = handle & RN_HANDLE_INDEX;
    if ( i >= cls->fresh )                  // never taken: its meta is unwritten, and it is not held
        return false;
    if ( ( cls->meta[i] & RN_META_LIVE ) == 0 )
        return false;

    cls->meta[i] &= RN_META_GENERATION;
    cls->free_count++;

    if ( i < cls->free_head )
        cls->free_head = i;

    return true;
}

void rn_send_store_free( rn_send_store_t * store, uint32_t handle )
{
    const bool freed = rn_send_store_free_checked( store, handle );
    rocketnet_assert( freed );
    (void) freed;
}

uint8_t * rn_send_store_chunk_data( const rn_send_store_t * store, uint32_t handle )
{
    rocketnet_assert( store );

    // the same rule as rn_send_store_free_checked: a handle the store did not
    // issue -- a class at or past RN_STORE_NUM_CLASSES (class 3, which includes
    // RN_CHUNK_NONE) or an index the store has not issued -- reads nothing. A
    // refusal, not an assert, so a debug build answers alike.

    const int c = rn_send_store_chunk_class( handle );
    if ( c >= RN_STORE_NUM_CLASSES )
        return NULL;

    const rn_store_class_t * cls = &store->classes[c];
    const uint32_t i = handle & RN_HANDLE_INDEX;
    if ( i >= cls->fresh )                 // never taken: its data is unwritten
        return NULL;

    return cls->data + (uint64_t) i * cls->chunk_bytes;
}

#if ROCKETNET_DEV
uint32_t rn_send_store_chunk_generation( const rn_send_store_t * store, uint32_t handle )
{
    rocketnet_assert( store );

    const int c = rn_send_store_chunk_class( handle );
    if ( c >= RN_STORE_NUM_CLASSES )
        return 0;

    const rn_store_class_t * cls = &store->classes[c];
    const uint32_t i = handle & RN_HANDLE_INDEX;
    if ( i >= cls->fresh )
        return 0;

    return cls->meta[i] & RN_META_GENERATION;
}

uint32_t rn_send_store_free_chunks( const rn_send_store_t * store, int class_index )
{
    rocketnet_assert( store && class_index >= 0 && class_index < RN_STORE_NUM_CLASSES );
    return store->classes[class_index].free_count;
}

uint32_t rn_send_store_total_chunks( const rn_send_store_t * store, int class_index )
{
    rocketnet_assert( store && class_index >= 0 && class_index < RN_STORE_NUM_CLASSES );
    return store->classes[class_index].num_chunks;
}
#endif // #if ROCKETNET_DEV

// ----------------------------------------------------------------
//                            the region
// ----------------------------------------------------------------

static void rn_region_point( rn_region_t * region, uint8_t * base, const rn_region_layout_t * layout, const rocketnet_message_config_t * config, rocketnet_counters_t * counters )
{
    memset( region, 0, sizeof( *region ) );
    region->base = base;
    region->bytes = layout->region_bytes;
    region->counters = counters;

    region->num_channels = (uint32_t) config->num_channels;
    region->window = (uint32_t) config->reliable_messages_in_flight;
    region->ring_bytes = (uint32_t) config->receive_message_buffer_bytes;
    region->queue = (uint32_t) ( layout->bytes[RN_PART_SEND_DESCRIPTORS] / sizeof( rn_send_descriptor_t ) );
    region->packets = (uint32_t) config->packets_in_flight;
    region->held_entries = 2 * region->window;
    region->sent_index_entries = 2 * region->packets;
    region->sent_ref_entries = 16 * region->packets;
    region->delivery_entries = region->window + (uint32_t) config->unreliable_messages_per_update;
    region->holds = (uint32_t) config->sequenced_messages_held;
    region->receipt_entries = (uint32_t) config->max_message_receipts;
    region->max_message_bytes = (uint32_t) config->max_message_bytes;

    // every typed carve-out at its part's offset, nothing outside the region

    region->channel_send = (rn_channel_send_t *) ( base + layout->offset[RN_PART_CHANNEL_SEND] );
    region->channel_receive = (rn_channel_receive_t *) ( base + layout->offset[RN_PART_CHANNEL_RECEIVE] );
    region->send_numbers = (rn_send_number_t *) ( base + layout->offset[RN_PART_SEND_NUMBERS] );
    region->send_first_sends = (double *) ( base + layout->offset[RN_PART_SEND_FIRST_SENDS] );
    region->receive_descriptors = (rn_receive_descriptor_t *) ( base + layout->offset[RN_PART_RECEIVE_DESCRIPTORS] );
    region->received_bits = (uint64_t *) ( base + layout->offset[RN_PART_RECEIVED_BITS] );
    region->held_index = (rn_held_entry_t *) ( base + layout->offset[RN_PART_HELD_INDEX] );
    region->receive_ring = base + layout->offset[RN_PART_RECEIVE_RING];
    region->send_descriptors = (rn_send_descriptor_t *) ( base + layout->offset[RN_PART_SEND_DESCRIPTORS] );
    region->sent_packets = (rn_sent_packet_t *) ( base + layout->offset[RN_PART_SENT_PACKETS] );
    region->sent_index = (rn_sent_index_t *) ( base + layout->offset[RN_PART_SENT_INDEX] );
    region->sent_refs = (uint32_t *) ( base + layout->offset[RN_PART_SENT_REFS] );
    region->deliveries = (rn_delivery_t *) ( base + layout->offset[RN_PART_DELIVERIES] );
    region->sequenced_messages_held = (rn_sequenced_hold_t *) ( base + layout->offset[RN_PART_SEQUENCED_HOLDS] );

    // both arrays remain in the existing, exactly planned hold part. Metadata is densely packed; payloads are not touched by allocation/expiry scans.

    region->sequenced_message_payloads = (rn_sequenced_payload_t *) ( region->sequenced_messages_held + region->holds );

    // the free stack and the delivered list follow the payloads in the same part, one index per hold, so a take and a return are both O(1)

    region->sequenced_hold_free_stack = (uint32_t *) ( region->sequenced_message_payloads + region->holds );
    region->sequenced_holds_delivered = region->sequenced_hold_free_stack + region->holds;
    region->receipts = (rn_receipt_t *) ( base + layout->offset[RN_PART_RECEIPTS] );
    region->resend_fifo = (rn_resend_link_t *) ( base + layout->offset[RN_PART_RESEND_FIFO] );
    region->unreliable_ring = (rn_unreliable_entry_t *) ( base + layout->offset[RN_PART_UNRELIABLE_RING] );
    region->unreliable_ring_entries = (uint32_t) ( 2 * layout->bytes[RN_PART_SEND_DESCRIPTORS] / sizeof( rn_send_descriptor_t ) );
    region->send_spans = base + layout->offset[RN_PART_SEND_SPANS];
    region->span_bytes = (uint32_t) layout->bytes[RN_PART_SEND_SPANS];

    // the store's memory is S at its offset; its class tables live in the region struct

    region->store_memory = base + layout->offset[RN_PART_SEND_STORE];
    region->store_bytes = layout->bytes[RN_PART_SEND_STORE];
    rn_send_store_init_counts( &region->store, region->store_memory, layout->store_chunks );
}

    // proofs/tla/RNConnectionMemory.tla, Reuse: the slate is carved clean
    // proofs/tla/RNSendStoreLayout.tla Carve: both send pools empty, both watermarks at zero
// proofs/tla/RNSequencedHoldPool.tla Carve: the hold free stack full, lowest index on top
void rn_region_carve( rn_region_t * region )
{
    rocketnet_assert( region && region->base );

    // the fixed tables to their empty state (4.1). O(tables), once per
    // connection, never on a send or receive path. The ring and the store's
    // chunks keep their bytes: every reader of them writes first.

    memset( region->channel_send, 0, sizeof( rn_channel_send_t ) * region->num_channels );
    memset( region->channel_receive, 0, sizeof( rn_channel_receive_t ) * region->num_channels );

    for ( uint32_t c = 0; c < region->num_channels; c++ )
    {
        region->channel_send[c].queue_head = RN_DESCRIPTOR_NONE;
        region->channel_send[c].queue_tail = RN_DESCRIPTOR_NONE;
        region->channel_send[c].ready_prev = RN_DESCRIPTOR_NONE;
        region->channel_send[c].ready_next = RN_DESCRIPTOR_NONE;
        region->channel_receive[c].held_sequenced = RN_DESCRIPTOR_NONE;
        region->channel_receive[c].block_in_turn = RN_DESCRIPTOR_NONE;
    }

    memset( region->send_numbers, 0, sizeof( rn_send_number_t ) * region->window );

    // send_first_sends is not cleared: an entry is read only after its
    // number first send writes it (rn_window_ack_record_first_send), and
    // numbering sets last_send to RN_SEND_NUMBER_NEVER_SENT

    for ( uint32_t n = 0; n < region->window; n++ )
    {
        region->send_numbers[n].descriptor = RN_DESCRIPTOR_NONE;
        region->resend_fifo[n].prev = 0;
        region->resend_fifo[n].next = 0;
        region->resend_fifo[n].generation = 0;
    }

    memset( region->receive_descriptors, 0, sizeof( rn_receive_descriptor_t ) * region->window );
    memset( region->received_bits, 0, region->window / 8 );
    memset( region->held_index, 0, sizeof( rn_held_entry_t ) * region->held_entries );
    region->held_count = 0;

    // the send descriptor pool: a LIFO free stack in the descriptors'
    // free_slot array, lazy (proofs/tla/RNLazyPool.tla Reset):
    // nothing taken, no descriptor written. A take initializes its descriptor
    // whole, preserving its generation tag and any live stack slot it holds.
    // Clear schedule tickets on the descriptors this connection took before
    // the fresh range is forgotten. A later take off the free stack still
    // bumps generation. A carve restarts the epoch at 1 and empties the
    // stack, so a reused slot's queue bookkeeping matches a slot carved once.

    for ( uint32_t i = 0; i < region->descriptors_fresh; i++ )
    {
        region->send_descriptors[i].enqueue_ticket = 0;
        region->send_descriptors[i].schedule_flags = 0;
    }

    region->descriptors_free_top = 0;
    region->descriptors_free = region->queue;
    region->descriptors_fresh = 0;

    // the store's classes, emptied in place (lazy, as the descriptors)

    uint32_t store_chunks[RN_STORE_NUM_CLASSES];

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        store_chunks[c] = region->store.classes[c].num_chunks;

    rn_send_store_init_counts( &region->store, region->store_memory, store_chunks );

    // the send spans: empty, nothing live (proofs/tla/RNSendSpans.tla, Reset)

    region->span_head = 0;
    region->span_tail = 0;
    region->span_first = RN_DESCRIPTOR_NONE;
    region->span_last = RN_DESCRIPTOR_NONE;

    memset( region->sent_packets, 0, sizeof( rn_sent_packet_t ) * region->packets );
    memset( region->sent_index, 0, sizeof( rn_sent_index_t ) * region->sent_index_entries );
    memset( region->sent_refs, 0, sizeof( uint32_t ) * region->sent_ref_entries );
    memset( region->deliveries, 0, sizeof( rn_delivery_t ) * region->delivery_entries );

    for ( uint32_t h = 0; h < region->holds; h++ )
    {
        rn_sequenced_hold_t * hold = &region->sequenced_messages_held[h];
        hold->bytes = 0;
        hold->ordering = 0;
        hold->sequencing = 0;
        hold->channel = 0;
        hold->used = 0;
        hold->code = 0;

        // the free stack starts full, lowest index on top, as the scan found it

        region->sequenced_hold_free_stack[h] = region->holds - 1 - h;
    }

    region->sequenced_hold_free_count = region->holds;
    region->sequenced_holds_delivered_count = 0;
    memset( region->receipts, 0, sizeof( rn_receipt_t ) * region->receipt_entries );

    region->exhausted = 0;
    region->exhausted_reason = ROCKETNET_DISCONNECT_REASON_NONE;
    region->send_room_armed = 1;

    region->schedule_ticket_next = 1;
    region->schedule_ticket_wrapped = 0;

    region->carves++;
}

void rn_region_reset( rn_region_t * region )
{
    // proofs/tla/RNConnectionMemory.tla, Connect after Reuse: the slot's region is
    // carved again before its next client, so ReusedSlotStartsClean holds --
    // used room, the exhausted state and every record of the previous client
    // are gone. The server resets lazily: a slot whose connection ended is
    // freed in O(1) and its region is carved here only when the slot is bound
    // to its next connection (rn_message_connection_release,
    // sdk/rocketnet_message_server.cpp; proofs/tla/RNSlots.tla ResetFree, Observe),
    // never for the whole population in the update after a mass disconnect.
    // The region holds message bytes only; session keys are wiped by the
    // transport when the slot ends.

    rn_region_carve( region );
}

void rn_region_exhaust( rn_region_t * region )
{
    // proofs/tla/RNConnectionMemory.tla, Exhaust: sticky with its one reason
    // (ExhaustedReasonNamed), counted once per exhausted connection

    rocketnet_assert( region );

    if ( region->exhausted )
        return;

    region->exhausted = 1;
    region->exhausted_reason = ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED;
    rn_count( region->counters, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED );
}

bool rn_region_exhausted( const rn_region_t * region )
{
    rocketnet_assert( region );
    return region->exhausted != 0;
}

#if ROCKETNET_DEV
int rn_region_exhausted_reason( const rn_region_t * region )
{
    rocketnet_assert( region );
    return region->exhausted ? region->exhausted_reason : ROCKETNET_DISCONNECT_REASON_NONE;
}
#endif // #if ROCKETNET_DEV

int rn_connection_memory_decide( const rn_region_t * region )
{
    // proofs/tla/RNConnectionMemory.tla, Kick's guard: an exhausted connection's only
    // step is the disconnect (ExhaustionOnlyLeadsToDisconnect), and the
    // decision is this region's alone (NoCrossSlotEffect)

    rocketnet_assert( region );

    return region->exhausted ? ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED : ROCKETNET_DISCONNECT_REASON_NONE;
}

// proofs/tla/RNLazyPool.tla Take and proofs/tla/RNSendStoreLayout.tla Alloc:
// a descriptor off the free stack (the descriptors' free_slot array, its top
// at descriptors_free_top - 1), or the first one never taken since the carve.
// The caller checked descriptors_free. The take initializes the descriptor
// whole, preserving its generation tag (bumped here) and, when the descriptor
// sits below the stack's new top, the stack entry its free_slot holds for
// that depth.

static uint32_t rn_region_descriptor_take( rn_region_t * region )
{
    rocketnet_assert( region->descriptors_free > 0 );

    uint32_t d;
    bool fresh_take = false;

    if ( region->descriptors_free_top > 0 )
    {
        const uint32_t top = region->descriptors_free_top - 1;
        d = region->send_descriptors[top].free_slot;
        region->descriptors_free_top = top;
        rocketnet_assert( d < region->descriptors_fresh );
        rocketnet_assert( !region->send_descriptors[d].in_use );
    }
    else
    {
        d = region->descriptors_fresh++;
        rocketnet_assert( d < region->queue );

        fresh_take = true;
    }

    region->descriptors_free--;

    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    // 0 is never a live name (proofs/tla/RNLazyPool.tla HeldGenerationPositive): a
    // resend link stamped 0 is skipped by the ack while the descriptor is in use

    const uint32_t generation = fresh_take || descriptor->generation == UINT32_MAX ? 1 : descriptor->generation + 1;
    const bool keep_free_slot = !fresh_take && d < region->descriptors_free_top;
    const uint32_t free_slot = keep_free_slot ? descriptor->free_slot : 0;
    memset( descriptor, 0, sizeof( *descriptor ) );
    descriptor->generation = generation;

    if ( keep_free_slot )
        descriptor->free_slot = free_slot;

    return d;
}

// proofs/tla/RNLazyPool.tla Release and proofs/tla/RNSendStoreLayout.tla Free:
// push the descriptor's index on the free stack, in the free_slot of the
// descriptor at the stack's depth (below descriptors_fresh, since the stack
// holds fewer entries than the descriptors taken).

static void rn_region_descriptor_release( rn_region_t * region, uint32_t d )
{
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    descriptor->in_use = 0;
    descriptor->chunk = RN_CHUNK_NONE;

    const uint32_t top = region->descriptors_free_top;
    region->send_descriptors[top].free_slot = d;
    region->descriptors_free_top = top + 1;
    region->descriptors_free++;
}

int rn_region_send_fit_store( const rn_region_t * region, int bytes, uint32_t * out_first )
{
    // proofs/tla/RNConnectionMemory.tla, Use's guard: the decision rn_region_send_alloc acts on, read alone by the room query

    rocketnet_assert( region );

    // an argument outside the config, or a message that takes a span, is the
    // caller's refusal (MESSAGE_SEND_REFUSED, layer 8), not exhaustion

    if ( bytes <= 0 || bytes > RN_STORE_LARGE_BYTES || (uint32_t) bytes > region->max_message_bytes )
        return RN_SEND_FIT_REFUSED;

    // sticky exhaustion: once exhausted, every send is refused until reset
    // (proofs/tla/RNConnectionMemory.tla, Exhaust sets the sticky flag; Use requires ~exhausted)

    if ( region->exhausted )
        return RN_SEND_FIT_EXHAUSTED;

    if ( region->descriptors_free == 0 )
        return RN_SEND_FIT_QUEUE_FULL;

    uint32_t first = 0;
    if ( !rn_send_store_find( &region->store, bytes, &first ) )
        return RN_SEND_FIT_STORE_FULL;

    if ( out_first )
        *out_first = first;

    return RN_SEND_FIT_OK;
}

// A refused allocation's effects, by its decision. Exhaustion is a counted
// refusal, never growth (R1, 4), and nothing is taken from either pool when
// one of them refuses. It is also the end of the connection: the region is
// exhausted (proofs/tla/RNConnectionMemory.tla, Exhaust) and its owner kicks it on
// the next decision. A send refused while the region stays exhausted is
// counted under MESSAGE_MEMORY_EXHAUSTED, the counter that names this refusal,
// on every refused send, not just at the transition to exhausted
// (proofs/tla/RNSendStoreLayout.tla Refuse).

static void rn_region_send_refuse( rn_region_t * region, int fit )
{
    switch ( fit )
    {
        case RN_SEND_FIT_EXHAUSTED:
            rn_count( region->counters, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED );
            break;
        case RN_SEND_FIT_QUEUE_FULL:
            rn_count( region->counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL );

            rn_region_exhaust( region );
            break;
        case RN_SEND_FIT_STORE_FULL:
            rn_count( region->counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );

            rn_region_exhaust( region );
            break;
        default:
            break;                                              // RN_SEND_FIT_REFUSED: the caller's to count
    }
}

uint32_t rn_region_send_alloc( rn_region_t * region, int bytes )
{
    // proofs/tla/RNSendStoreLayout.tla Alloc (room found) or Refuse (none)
    // proofs/tla/RNConnectionMemory.tla, Use (room found) or Exhaust (none)
    // ExhaustionOnlyLeadsToDisconnect: once exhausted, no further Use; the
    // steps left are Release (an ack returning room, which nothing takes
    // again), Kick (disconnect) and Reuse (reset).

    rocketnet_assert( region );

    uint32_t first = 0;
    const int fit = rn_region_send_fit_store( region, bytes, &first );
    if ( fit != RN_SEND_FIT_OK )
    {
        rn_region_send_refuse( region, fit );
        return RN_DESCRIPTOR_NONE;
    }

    const uint32_t chunk = rn_send_store_take( &region->store, bytes, first );   // the chunk the decision found: one scan

    const uint32_t d = rn_region_descriptor_take( region );
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    descriptor->chunk = chunk;
    descriptor->bytes = (uint32_t) bytes;
    descriptor->fragments = 1;
    descriptor->in_use = 1;

    return d;
}

// ----------------------------------------------------------------
//                          the send spans
// ----------------------------------------------------------------

// The spans are one ring of SR bytes with RNRing's placement rules for the
// sender's side (proofs/tla/RNRing.tla, Pos and PlaceSingle): the next span goes at
// the tail; when it would cross the end of the ring it goes to the start,
// leaving the skipped bytes as padding; it is admitted only when it ends at or
// before the oldest live span's start (the floor). The live spans are a list,
// oldest first, so the floor is the first node's start. A span is freed when
// its message is fully acked, in any order; the floor moves only when the
// oldest one goes, and the bytes of a younger freed span wait for it.
//
// This representation (offsets in 0..SR-1, head/tail, the first..last list)
// is modelled in proofs/tla/RNSendSpans.tla, checked by proofs/tla/MCRNSendSpans.cfg with
// the reversed witnesses MCRNSendSpansBrokenFloorAtNewest.cfg and
// MCRNSendSpansBrokenNoPadding.cfg (proofs/tla/CASES.tsv group rnsendspans).

static uint64_t rn_span_round16( uint64_t bytes )
{
    return ( bytes + 15 ) & ~(uint64_t) 15;
}

// proofs/tla/RNSendSpans.tla, Place (PadOnWrap is the wrap branch below)

static bool rn_span_place( const rn_region_t * region, uint64_t len16, uint32_t * out_start )
{
    const uint64_t SR = region->span_bytes;
    if ( len16 == 0 || len16 > SR )
        return false;

    if ( region->span_first == RN_DESCRIPTOR_NONE )
    {
        *out_start = 0;                                     // nothing live: the ring starts over
        return true;
    }

    const uint64_t head = region->span_head;
    const uint64_t tail = region->span_tail;
    if ( tail > head )
    {
        if ( len16 <= SR - tail )
        {
            *out_start = (uint32_t) tail;
            return true;
        }

        if ( len16 <= head )                                // wrap: pad to the end, place at the start
        {
            *out_start = 0;
            return true;
        }

        return false;
    }

    // the tail has wrapped behind the head (or met it: full)

    if ( len16 <= head - tail )
    {
        *out_start = (uint32_t) tail;
        return true;
    }

    return false;
}

// Live spans are a list in allocation order, oldest first, linked by
// descriptor index through span_prev/span_next inside the fixed Q-descriptor
// array (docs/SEQUENCE-BUFFERS.md, approved index links); span_first and
// span_last are its ends, and span_head is the reclaim floor (the oldest live
// span's start). A freed interior span's bytes stay retained behind the
// floor; they are never overwritten while any older span lives. Every link
// and unlink is O(1).

// proofs/tla/RNSendSpans.tla, Alloc (FloorAtOldest: head moves only for the first span)

static void rn_span_link( rn_region_t * region, uint32_t d, uint32_t start, uint64_t len16 )
{
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    descriptor->flags |= RN_SEND_FLAG_SPAN;
    descriptor->span_start = start;
    descriptor->span_prev = region->span_last;
    descriptor->span_next = RN_DESCRIPTOR_NONE;

    if ( region->span_first == RN_DESCRIPTOR_NONE )
    {
        region->span_first = d;
        region->span_head = start;
    }
    else
    {
        region->send_descriptors[region->span_last].span_next = d;
    }

    region->span_last = d;

    const uint64_t end = (uint64_t) start + len16;
    region->span_tail = end == region->span_bytes ? 0 : (uint32_t) end;
}

// proofs/tla/RNSendSpans.tla, Free (any order; head follows the oldest, EmptyIsZero)

static void rn_span_unlink( rn_region_t * region, uint32_t d )
{
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    rocketnet_assert( descriptor->flags & RN_SEND_FLAG_SPAN );

    if ( !( descriptor->flags & RN_SEND_FLAG_SPAN ) )
        return;

    RN_SPAN_COST_VISIT();

    const uint32_t prev = descriptor->span_prev;
    const uint32_t next = descriptor->span_next;

    descriptor->flags &= (uint8_t) ~RN_SEND_FLAG_SPAN;
    descriptor->span_prev = RN_DESCRIPTOR_NONE;
    descriptor->span_next = RN_DESCRIPTOR_NONE;

    if ( prev != RN_DESCRIPTOR_NONE )
        region->send_descriptors[prev].span_next = next;
    else
        region->span_first = next;

    if ( next != RN_DESCRIPTOR_NONE )
        region->send_descriptors[next].span_prev = prev;
    else
        region->span_last = prev;

    if ( region->span_first == RN_DESCRIPTOR_NONE )
    {
        region->span_head = 0;                              // empty: the floor is the start
        region->span_tail = 0;
    }
    else if ( prev == RN_DESCRIPTOR_NONE )
    {
        RN_SPAN_COST_VISIT();

        region->span_head = region->send_descriptors[next].span_start;
    }
}

int rn_region_send_fit_span( const rn_region_t * region, int bytes, uint32_t * out_start )
{
    // proofs/tla/RNConnectionMemory.tla, UseSpan's guard: the decision rn_region_send_alloc_span acts on, read alone by the room query

    rocketnet_assert( region );

    if ( bytes < (int) RN_SPAN_MIN_BYTES || (uint32_t) bytes > region->max_message_bytes )
        return RN_SEND_FIT_REFUSED;

    // sticky exhaustion, the same guard as rn_region_send_fit_store: Use (and
    // UseSpan) in proofs/tla/RNConnectionMemory.tla require ~exhausted, so an
    // exhausted region takes no span until rn_region_reset, even when acks
    // have returned room

    if ( region->exhausted )
        return RN_SEND_FIT_EXHAUSTED;

    if ( region->descriptors_free == 0 )
        return RN_SEND_FIT_QUEUE_FULL;

    uint32_t start = 0;
    if ( !rn_span_place( region, rn_span_round16( (uint64_t) bytes ), &start ) )
        return RN_SEND_FIT_STORE_FULL;

    if ( out_start )
        *out_start = start;

    return RN_SEND_FIT_OK;
}

int rn_region_send_fit( const rn_region_t * region, int bytes )
{
    return rn_region_send_is_span( bytes ) ? rn_region_send_fit_span( region, bytes, NULL ) : rn_region_send_fit_store( region, bytes, NULL );
}

uint32_t rn_region_send_take( rn_region_t * region, int bytes )
{
    return rn_region_send_is_span( bytes ) ? rn_region_send_alloc_span( region, bytes ) : rn_region_send_alloc( region, bytes );
}

uint32_t rn_region_send_alloc_span( rn_region_t * region, int bytes )
{
    // proofs/tla/RNSendStoreLayout.tla AllocSpan: the descriptor take.
    // proofs/tla/RNConnectionMemory.tla, UseSpan (room found) or Exhaust (none); the span
    // is proofs/tla/RNRing.tla's PlaceSingle on the sender's side, and its PlaceBlock
    // for a send of more than one fragment: one contiguous span, both bounds
    // checked once, fragment i at start + i * F (rn_region_send_data).
    // proofs/tla/RNSendSpans.tla Exhaust: no descriptor or no place for the span,
    // so rn_region_send_refuse exhausts the region.

    rocketnet_assert( region );

    uint32_t start = 0;
    const int fit = rn_region_send_fit_span( region, bytes, &start );
    if ( fit != RN_SEND_FIT_OK )
    {
        rn_region_send_refuse( region, fit );
        return RN_DESCRIPTOR_NONE;
    }

    const uint64_t len16 = rn_span_round16( (uint64_t) bytes );

    const uint32_t d = rn_region_descriptor_take( region );
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    descriptor->chunk = RN_CHUNK_NONE;
    descriptor->bytes = (uint32_t) bytes;
    descriptor->fragments = (uint16_t) ( ( bytes + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES );
    descriptor->in_use = 1;

    rn_span_link( region, d, start, len16 );

    return d;
}

// proofs/tla/RNRing.tla PlaceSingle

const uint8_t * rn_region_send_data( const rn_region_t * region, uint32_t d, uint32_t fragment, uint32_t * out_bytes )
{
    rocketnet_assert( region );

    if ( out_bytes )
        *out_bytes = 0;
    if ( d >= region->descriptors_fresh )   // never taken since the carve: not in use, and unwritten
        return NULL;

    const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    if ( !descriptor->in_use )
        return NULL;

    const uint32_t F = RN_STORE_LARGE_BYTES;
    const bool block = descriptor->fragments > 1;
    if ( ( !block && fragment != 0 ) || ( block && fragment >= descriptor->fragments ) )
        return NULL;

    const uint8_t * data = NULL;

    if ( descriptor->flags & RN_SEND_FLAG_SPAN )
    {
        data = region->send_spans + descriptor->span_start + (uint64_t) fragment * F;
    }
    else
    {
        if ( descriptor->chunk == RN_CHUNK_NONE || block )
            return NULL;

        data = rn_send_store_chunk_data( &region->store, descriptor->chunk );
    }

    if ( out_bytes )
    {
        const uint32_t offset = fragment * F;
        *out_bytes = block ? ( descriptor->bytes - offset < F ? descriptor->bytes - offset : F ) : descriptor->bytes;
    }

    return data;
}

uint8_t * rn_region_send_write_pointer( rn_region_t * region, uint32_t d )
{
    rocketnet_assert( region );

    if ( d >= region->descriptors_fresh || !region->send_descriptors[d].in_use )
        return NULL;

    const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    if ( descriptor->flags & RN_SEND_FLAG_SPAN )
        return region->send_spans + descriptor->span_start;
    if ( descriptor->chunk == RN_CHUNK_NONE )
        return NULL;

    return rn_send_store_chunk_data( &region->store, descriptor->chunk );
}

void rn_region_send_free( rn_region_t * region, uint32_t d )
{
    // proofs/tla/RNConnectionMemory.tla, Release: allowed on an exhausted region (an
    // ack or an abort between Exhaust and Kick), it only returns room, and
    // both allocators refuse to take it again until rn_region_reset.
    // proofs/tla/RNSendStoreLayout.tla Free: the chunk, then the descriptor
    // pushed on the free stack.

    rocketnet_assert( region );
    rocketnet_assert( d < region->descriptors_fresh );

    if ( d >= region->descriptors_fresh )
        return;

    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    rocketnet_assert( descriptor->in_use );

    if ( !descriptor->in_use )
        return;
    if ( descriptor->flags & RN_SEND_FLAG_SPAN )
        rn_span_unlink( region, d );
    if ( descriptor->chunk != RN_CHUNK_NONE )
        rn_send_store_free( &region->store, descriptor->chunk );

    rn_region_descriptor_release( region, d );
}

void rn_region_send_free_fragment( rn_region_t * region, uint32_t d )
{
    // proofs/tla/RNConnectionMemory.tla, Release: a send's room returns whole
    // on its last distinct fragment ack. A store send is one fragment; a span
    // holds all of its fragments until the last.

    rocketnet_assert( region );
    rocketnet_assert( d < region->descriptors_fresh );

    if ( d >= region->descriptors_fresh )
        return;

    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    rocketnet_assert( descriptor->in_use );

    if ( !descriptor->in_use )
        return;

    descriptor->fragments_acked++;
    if ( descriptor->fragments_acked >= descriptor->fragments )
        rn_region_send_free( region, d );
}

// ----------------------------------------------------------------
//                      the send room watch
// ----------------------------------------------------------------

static uint32_t rn_permille( uint64_t used, uint64_t total )
{
    return total > 0 ? (uint32_t) ( used * 1000 / total ) : 0;
}

// the span bytes from the oldest live span's start to the tail, the padding
// a wrap leaves included: the bytes no new span can take

static uint64_t rn_span_used( const rn_region_t * region )
{
    if ( region->span_first == RN_DESCRIPTOR_NONE )
        return 0;

    const uint64_t head = region->span_head;
    const uint64_t tail = region->span_tail;
    return tail > head ? tail - head : region->span_bytes - head + tail;
}

void rn_region_send_room( const rn_region_t * region, rn_send_room_t * room )
{
    rocketnet_assert( region && room );

    memset( room, 0, sizeof( *room ) );

    room->messages = region->queue;
    room->used_messages = region->queue - region->descriptors_free;

    uint32_t permille = rn_permille( room->used_messages, room->messages );

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const rn_store_class_t * cls = &region->store.classes[c];
        const uint32_t used = cls->num_chunks - cls->free_count;
        room->store_used_bytes += (uint64_t) used * cls->chunk_bytes;
        room->store_bytes += (uint64_t) cls->num_chunks * cls->chunk_bytes;

        const uint32_t class_permille = rn_permille( used, cls->num_chunks );
        if ( class_permille > permille )
            permille = class_permille;
    }

    room->span_used_bytes = rn_span_used( region );
    room->span_bytes = region->span_bytes;

    const uint32_t span_permille = rn_permille( room->span_used_bytes, room->span_bytes );
    if ( span_permille > permille )
        permille = span_permille;

    room->permille = permille;
}

int rn_region_send_fits( const rn_region_t * region, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages )
{
    rocketnet_assert( region );

    uint64_t free_room = 0;
    uint64_t pool = 0;

    if ( bytes >= 1 && (uint32_t) bytes <= region->max_message_bytes )
    {
        if ( rn_region_send_is_span( bytes ) )
        {
            free_room = region->span_bytes - rn_span_used( region );
            pool = region->span_bytes;
        }
        else
        {
            const rn_store_class_t * cls = &region->store.classes[rn_send_store_class_for( bytes )];
            free_room = (uint64_t) cls->free_count * cls->chunk_bytes;
            pool = (uint64_t) cls->num_chunks * cls->chunk_bytes;
        }
    }

    if ( free_bytes )
        *free_bytes = free_room;
    if ( pool_bytes )
        *pool_bytes = pool;
    if ( free_messages )
        *free_messages = region->descriptors_free;

    return rn_region_send_fit( region, bytes ) == RN_SEND_FIT_OK;      // the allocation's own decision
}

int rn_region_send_room_watch( rn_region_t * region )
{
    rocketnet_assert( region );

    rn_send_room_t room;
    rn_region_send_room( region, &room );

    if ( room.permille < RN_SEND_ROOM_REARM_PERMILLE )
    {
        region->send_room_armed = 1;
        return RN_SEND_ROOM_NONE;
    }

    if ( room.permille >= RN_SEND_ROOM_WARN_PERMILLE && region->send_room_armed )
    {
        region->send_room_armed = 0;
        return RN_SEND_ROOM_CROSSED;
    }

    return RN_SEND_ROOM_NONE;
}

const int rn_send_room_warnings = ROCKETNET_ASSERTS ? 1 : 0;

#if ROCKETNET_ASSERTS

static void rn_region_send_room_log( const rn_region_t * region, int is_server, int index, const char * what )
{
    rn_send_room_t room;
    rn_region_send_room( region, &room );

    char who[32];

    if ( is_server )
        snprintf( who, sizeof( who ), "server slot %d", index );
    else
        snprintf( who, sizeof( who ), "client" );

    rocketnet_warn( "%s send room %s (%u.%u%% of the fullest pool): store %" PRIu64 " of %" PRIu64 " bytes, spans %" PRIu64 " of %" PRIu64 " bytes, "
                    "messages %u of %u; sends outrun acks, raise send_message_buffer_bytes, receive_message_buffer_bytes or send_queue_messages, or send less",
                    who, what, room.permille / 10, room.permille % 10, room.store_used_bytes, room.store_bytes, room.span_used_bytes, room.span_bytes,
                    room.used_messages, room.messages );
}

void rn_region_send_room_note( rn_region_t * region, int is_server, int index )
{
    if ( rn_region_send_room_watch( region ) == RN_SEND_ROOM_CROSSED )
        rn_region_send_room_log( region, is_server, index, "over 75% full" );
}

void rn_region_send_room_exhausted( const rn_region_t * region, int is_server, int index )
{
    rn_region_send_room_log( region, is_server, index, "exhausted, the connection ends (MESSAGE_MEMORY_EXHAUSTED)" );
}

#else // #if ROCKETNET_ASSERTS

void rn_region_send_room_note( rn_region_t * region, int is_server, int index )
{
    (void) region;

    (void) is_server;

    (void) index;
}

void rn_region_send_room_exhausted( const rn_region_t * region, int is_server, int index )
{
    (void) region;

    (void) is_server;

    (void) index;
}

#endif // #if ROCKETNET_ASSERTS

#if ROCKETNET_DEV
bool rn_region_contains( const rn_region_t * region, const void * p, uint64_t bytes )
{
    rocketnet_assert( region );
    const uintptr_t address = (uintptr_t) p;
    const uintptr_t base = (uintptr_t) region->base;
    if ( address < base || bytes > region->bytes )
        return false;

    return (uint64_t) ( address - base ) <= region->bytes - bytes;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNRing.tla SpanInsideRing

bool rn_region_ring_span_valid( const rn_region_t * region, uint64_t offset, uint64_t bytes )
{
    // placement is the sender's; the receiver only checks bounds (4.3, 6.3), so a sender that breaks them is a violation, never a write outside the ring

    rocketnet_assert( region );

    if ( bytes == 0 || offset % 16 != 0 || offset >= region->ring_bytes )
        return false;

    return bytes <= region->ring_bytes - offset;
}

// ----------------------------------------------------------------
//                            the arena
// ----------------------------------------------------------------

struct rn_arena_t
{
    void * context;
    uint64_t bytes;
    int num_regions;
    int owns_memory;                    // 1: the create's own allocation, freed at destroy; 0: the caller's block
    rn_region_layout_t layout;
    rn_region_t * regions;
    uint8_t * memory;
};

// the arena's one block: its header, its region structs, then num_regions
// regions back to back, each 64-aligned (4). false, with a log line, for a
// refused config or count, or a size that does not fit in memory.

static bool rn_arena_plan( const rocketnet_message_config_t * config, int num_regions, rn_region_layout_t * layout, uint64_t * header, uint64_t * bytes )
{
    if ( num_regions < 1 || num_regions > ( 1 << 20 ) )
    {
        rocketnet_error( "message arena of %d regions refused (1 to %d)", num_regions, 1 << 20 );
        return false;
    }

    if ( !rn_region_layout( config, layout ) )
        return false;

    *header = rn_align64( sizeof( rn_arena_t ) ) + rn_align64( sizeof( rn_region_t ) * (uint64_t) num_regions );
    if ( layout->region_bytes > ( UINT64_MAX - *header ) / (uint64_t) num_regions || *header + layout->region_bytes * (uint64_t) num_regions > (uint64_t) SIZE_MAX )
    {
        rocketnet_error( "message arena of %d regions of %" PRIu64 " bytes is too large", num_regions, layout->region_bytes );
        return false;
    }

    *bytes = *header + layout->region_bytes * (uint64_t) num_regions;

    return true;
}

// lays the arena out in memory and carves every region: no allocation

static rn_arena_t * rn_arena_build( uint8_t * memory, void * context, int owns_memory, const rocketnet_message_config_t * config, int num_regions, const rn_region_layout_t * layout, uint64_t header, uint64_t bytes, rocketnet_counters_t * counters )
{
    rn_arena_t * arena = (rn_arena_t *) memory;
    memset( arena, 0, sizeof( *arena ) );
    arena->context = context;
    arena->bytes = bytes;
    arena->num_regions = num_regions;
    arena->owns_memory = owns_memory;
    arena->layout = *layout;
    arena->regions = (rn_region_t *) ( memory + rn_align64( sizeof( rn_arena_t ) ) );
    arena->memory = memory + header;

    for ( int i = 0; i < num_regions; i++ )
    {
        rn_region_point( &arena->regions[i], arena->memory + layout->region_bytes * (uint64_t) i, layout, config, counters );

        rn_region_carve( &arena->regions[i] );
    }

    return arena;
}

    // proofs/tla/RNConnectionMemory.tla, Init: every slot is Free with a carved region
rn_arena_t * rn_arena_create( void * context, const rocketnet_message_config_t * config, int num_regions, rocketnet_counters_t * counters )
{
    rocketnet_message_config_t defaults;

    if ( !config )
    {
        rocketnet_message_default_config( &defaults );

        config = &defaults;
    }

    rn_region_layout_t layout;
    uint64_t header = 0;
    uint64_t bytes = 0;
    if ( !rn_arena_plan( config, num_regions, &layout, &header, &bytes ) )
        return NULL;

    // the create path's one allocation (R2): nothing after it allocates

    uint8_t * memory = (uint8_t *) rocketnet_malloc( context, (size_t) bytes );
    if ( !memory )
    {
        rocketnet_error( "message arena allocation of %" PRIu64 " bytes failed", bytes );
        return NULL;
    }

    return rn_arena_build( memory, context, 1, config, num_regions, &layout, header, bytes, counters );
}

uint64_t rn_arena_required_bytes( const rocketnet_message_config_t * config, int num_regions )
{
    rocketnet_message_config_t defaults;

    if ( !config )
    {
        rocketnet_message_default_config( &defaults );

        config = &defaults;
    }

    rn_region_layout_t layout;
    uint64_t header = 0;
    uint64_t bytes = 0;
    if ( !rn_arena_plan( config, num_regions, &layout, &header, &bytes ) )
        return 0;

    return bytes;
}

    // proofs/tla/RNConnectionMemory.tla, Init: every slot is Free with a carved region
rn_arena_t * rn_arena_create_in_block( void * block, uint64_t block_bytes, const rocketnet_message_config_t * config, int num_regions, rocketnet_counters_t * counters )
{
    rocketnet_message_config_t defaults;

    if ( !config )
    {
        rocketnet_message_default_config( &defaults );

        config = &defaults;
    }

    rn_region_layout_t layout;
    uint64_t header = 0;
    uint64_t bytes = 0;
    if ( !rn_arena_plan( config, num_regions, &layout, &header, &bytes ) )
        return NULL;

    // the caller's block is checked, never truncated: the configuration's exact size and the regions' 64-byte alignment, or nothing

    if ( !block || ( (uintptr_t) block & ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) ) != 0 )
    {
        rocketnet_error( "message arena block refused: NULL or not %d-byte aligned", ROCKETNET_ALLOCATION_ALIGNMENT );
        return NULL;
    }

    if ( block_bytes < bytes )
    {
        rocketnet_error( "message arena block of %" PRIu64 " bytes refused: %d regions need %" PRIu64, block_bytes, num_regions, bytes );
        return NULL;
    }

    return rn_arena_build( (uint8_t *) block, NULL, 0, config, num_regions, &layout, header, bytes, counters );
}

void rn_arena_destroy( rn_arena_t * arena )
{
    if ( !arena )
        return;

    // the regions hold message bytes, never keys: nothing is wiped. The OS
    // zero-fills a page before another process gets it, and wiping would
    // touch every page of a plan that can reach gigabytes, most of them never
    // used. A caller's block stays the caller's, as it is.

    if ( arena->owns_memory )
        rocketnet_free( arena->context, arena );
}

#if ROCKETNET_DEV
int rn_arena_num_regions( const rn_arena_t * arena )
{
    rocketnet_assert( arena );
    return arena->num_regions;
}
#endif // #if ROCKETNET_DEV

rn_region_t * rn_arena_region( rn_arena_t * arena, int index )
{
    rocketnet_assert( arena );

    if ( index < 0 || index >= arena->num_regions )
        return NULL;

    return &arena->regions[index];
}

#if ROCKETNET_DEV
uint64_t rn_arena_bytes( const rn_arena_t * arena )
{
    rocketnet_assert( arena );
    return arena->bytes;
}
#endif // #if ROCKETNET_DEV

// ----------------------------------------------------------------
//                     the sizing, before connect
// ----------------------------------------------------------------

uint64_t rocketnet_message_connection_bytes( const struct rocketnet_message_config_t * config )
{
    rn_region_layout_t layout;
    if ( !rn_region_layout( config, &layout ) )
        return 0;

    return layout.region_bytes;
}

struct rn_text_t
{
    char * buffer;
    int size;
    int length;
};

#if defined( __GNUC__ ) || defined( __clang__ )
__attribute__( ( format( printf, 2, 3 ) ) )
#endif // #if defined( __GNUC__ ) || defined( __clang__ )
static void
rn_text_append( rn_text_t * text, const char * format, ... )
{
    // snprintf's contract over many appends: write what fits, always terminate, count the full length. Integers and strings only, so nothing here allocates.

    char * at = NULL;
    size_t room = 0;

    if ( text->buffer && text->length < text->size )
    {
        at = text->buffer + text->length;
        room = (size_t) ( text->size - text->length );
    }

    va_list args;
    va_start( args, format );

    const int written = vsnprintf( at, room, format, args );

    va_end( args );

    if ( written > 0 )
        text->length += written;
}

int rocketnet_message_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections, char * buffer, int buffer_size )
{
    if ( buffer && buffer_size > 0 )
        buffer[0] = '\0';
    if ( buffer_size < 0 )
        buffer_size = 0;

    rocketnet_message_config_t defaults;

    if ( !config )
    {
        rocketnet_message_default_config( &defaults );

        config = &defaults;
    }

    rn_region_layout_t layout;
    if ( !rn_region_layout( config, &layout ) )
        return 0;
    if ( num_connections < 0 )
        num_connections = 0;

    rn_text_t text = { buffer, buffer_size, 0 };
    rn_text_append( &text, "message connection sizing (docs/LOAD.md Table 3)\n" );
    rn_text_append( &text, "connection_bytes = 64C + 116.125W + B + 88Q + S + 128P + 24(W + Du) + 1440Ru + 16Rq + SR, Q/S/SR include maximum resend-delay payload reserve (typical pool payloads 128/512/1024 bytes)\n" );
    rn_text_append( &text, "C=%d W=%d B=%d S=%d Q=%d P=%d Du=%d Ru=%d Rq=%d\n",
                    config->num_channels, config->reliable_messages_in_flight, config->receive_message_buffer_bytes, config->send_message_buffer_bytes,
                    config->send_queue_messages, config->packets_in_flight, config->unreliable_messages_per_update,
                    config->sequenced_messages_held, config->max_message_receipts );

    for ( int part = 0; part < RN_NUM_PARTS; part++ )
        rn_text_append( &text, "  %-24s %-9s %" PRIu64 "\n", rn_part_names[part], rn_part_formulas[part], layout.bytes[part] );

    rn_text_append( &text, "store classes %u x %d, %u x %d, %u x %d\n",
                    layout.store_chunks[RN_STORE_SMALL], RN_STORE_SMALL_BYTES,
                    layout.store_chunks[RN_STORE_MEDIUM], RN_STORE_MEDIUM_BYTES,
                    layout.store_chunks[RN_STORE_LARGE], RN_STORE_LARGE_BYTES );
    rn_text_append( &text, "formula bytes %" PRIu64 "\n", layout.formula_bytes );
    rn_text_append( &text, "alignment padding %" PRIu64 "\n", layout.region_bytes - layout.formula_bytes );
    rn_text_append( &text, "region bytes %" PRIu64 "\n", layout.region_bytes );
    rn_text_append( &text, "%d connections %" PRIu64 " bytes\n", num_connections, layout.region_bytes * (uint64_t) num_connections );

    return text.length;
}

uint64_t rocketnet_message_print_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections )
{
    char text[4096];
    if ( rocketnet_message_connection_sizing( config, num_connections, text, (int) sizeof( text ) ) == 0 )
        return 0;

    // one log line per text line

    char * line = text;
    while ( *line )
    {
        char * end = strchr( line, '\n' );
        if ( end )
            *end = '\0';

        rocketnet_info( "%s", line );

        if ( !end )
            break;

        line = end + 1;
    }

    return rocketnet_message_connection_bytes( config );
}
