/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

#include <string.h>

#include "rocketnet_platform.h"

// Layer 4's dispatch (docs/CHANNELS.md 7). The spec is tla/RNChannel.tla; each
// function names the action it implements. Nothing here allocates, locks or
// reads a clock (owner rules R2 and R7).

static void rn_channels_count( rocketnet_counters_t * counters, int counter )
{
    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &counters->values[counter], 1 );
}

static inline uint32_t rn_u24( uint32_t value )
{
    return value & ROCKETNET_MESSAGE_U24_MASK;
}

static inline uint64_t rn_round16( uint64_t bytes )
{
    return ( bytes + 15 ) & ~(uint64_t) 15;
}

// The delivery table (CHANNELS.md 10.1): every delivery of the update, this
// record's or one released from a hold, appended in order. The caller checked
// there is room.

static rn_delivery_t * rn_channels_append_delivery( rn_channels_t * channels )
{
    rocketnet_assert( channels->deliveries < channels->delivery_capacity );
    rn_delivery_t * delivery = channels->region->deliveries + channels->deliveries;
    memset( delivery, 0, sizeof( *delivery ) );
    delivery->descriptor = RN_DESCRIPTOR_NONE;
    channels->deliveries++;
    return delivery;
}

// A reliable delivery has room by construction: at most W numbers are live in
// an update (6.3 step 4). A table that is full anyway is the region failing its
// own bound, so the connection is exhausted rather than a message dropped (R5).

static rn_delivery_t * rn_channels_reliable_delivery( rn_channels_t * channels )
{
    if ( channels->deliveries >= channels->delivery_capacity )
    {
        rn_region_exhaust( channels->region );
        return NULL;
    }
    return rn_channels_append_delivery( channels );
}

// ---------------------------------------------------------------
//          the held-ordered index (6.1): 2W open-addressed entries
// ---------------------------------------------------------------

// Keyed by (channel, ordering). A held message is `used` 1 (complete: in the
// ring, ready for its turn); a block still collecting fragments is `used` 2.
// At most W entries are live (each holds a live number), so the table is never
// full. Backward-shift deletion keeps probes short without tombstones.

#define RN_HELD_COMPLETE        1
#define RN_HELD_COLLECTING      2

static inline uint64_t rn_held_key( uint32_t channel, uint32_t ordering )
{
    return ( (uint64_t) channel << 32 ) | rn_u24( ordering );
}

static inline uint32_t rn_held_home( uint64_t key, uint32_t entries )
{
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
    return (uint32_t) ( h & ( entries - 1 ) );
}

static uint32_t rn_held_find( const rn_region_t * region, uint64_t key )
{
    const uint32_t entries = region->held_entries;
    uint32_t i = rn_held_home( key, entries );
    for ( uint32_t step = 0; step < entries; step++ )
    {
        const rn_held_entry_t * entry = &region->held_index[i];
        if ( !entry->used )
            return RN_DESCRIPTOR_NONE;
        if ( entry->key == key )
            return i;
        i = ( i + 1 ) & ( entries - 1 );
    }
    return RN_DESCRIPTOR_NONE;
}

static bool rn_held_insert( rn_region_t * region, uint64_t key, uint32_t number_slot, uint32_t used )
{
    const uint32_t entries = region->held_entries;
    uint32_t i = rn_held_home( key, entries );
    for ( uint32_t step = 0; step < entries; step++ )
    {
        rn_held_entry_t * entry = &region->held_index[i];
        if ( !entry->used )
        {
            entry->key = key;
            entry->number_slot = number_slot;
            entry->used = used;
            return true;
        }
        i = ( i + 1 ) & ( entries - 1 );
    }
    return false;
}

static void rn_held_erase( rn_region_t * region, uint32_t index )
{
    const uint32_t entries = region->held_entries;
    const uint32_t mask = entries - 1;
    uint32_t i = index;
    uint32_t j = index;
    for ( ;; )
    {
        j = ( j + 1 ) & mask;
        if ( !region->held_index[j].used )
            break;
        const uint32_t home = rn_held_home( region->held_index[j].key, entries );
        // the entry at j may move to the hole at i unless its home lies cyclically in (i, j]
        const bool stays = i <= j ? ( i < home && home <= j ) : ( i < home || home <= j );
        if ( stays )
            continue;
        region->held_index[i] = region->held_index[j];
        i = j;
    }
    region->held_index[i].key = 0;
    region->held_index[i].number_slot = 0;
    region->held_index[i].used = 0;
}

// The receiver's descriptor of a record's number. The window accepted the
// record already (rn_window_receive_accept: live, fields set). A record that
// reaches dispatch without that is filled in here from the record, so dispatch
// stands alone. NULL when the slot is live under a different number.

static rn_receive_descriptor_t * rn_channels_descriptor( rn_channels_t * channels, const struct rocketnet_message_record_t * record, uint32_t * out_slot )
{
    rn_region_t * region = channels->region;
    const uint32_t slot = record->number & ( region->window - 1 );
    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[slot];
    if ( descriptor->live )
    {
        if ( rn_u24( (uint32_t) descriptor->number ) != record->number )
            return NULL;
    }
    else
    {
        descriptor->number = record->number;
        descriptor->ordering = record->ordering;
        descriptor->sequencing = record->sequencing;
        descriptor->ring = record->ring;
        descriptor->bytes = (uint32_t) record->bytes;
        descriptor->channel = (uint16_t) record->channel;
        descriptor->code = (uint8_t) record->code;
        descriptor->block_have = 0;
        descriptor->block_count = 0;
        descriptor->live = 1;
    }
    *out_slot = slot;
    return descriptor;
}

static int rn_channels_dispatch_chunk( rn_channels_t * channels, const struct rocketnet_message_record_t * record, rn_delivery_t * out, uint32_t delta );

bool rn_channels_init( struct rn_channels_t * channels, struct rn_region_t * region, uint32_t num_channels, uint32_t enabled_types )
{
    if ( !channels )
        return false;

    memset( channels, 0, sizeof( *channels ) );

    if ( !region || num_channels < 1 || num_channels > region->num_channels )
        return false;

    channels->region = region;
    channels->num_channels = num_channels;
    channels->enabled_types = enabled_types != 0 ? enabled_types : ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION;
    channels->enabled_codes = rocketnet_message_enabled_codes( channels->enabled_types );
    channels->delivery_capacity = region->delivery_entries;
    channels->payload_capacity = ROCKETNET_MESSAGE_SINGLE_BYTES;
    channels->error = ROCKETNET_MESSAGE_ERROR_NONE;
    channels->pending = RN_DESCRIPTOR_NONE;
    channels->ready_head = RN_DESCRIPTOR_NONE;
    channels->ready_tail = RN_DESCRIPTOR_NONE;

    return true;
}

bool rn_channels_set_payload_capacity( struct rn_channels_t * channels, uint32_t payload_capacity )
{
    // a chunk's record (the longest header, then F bytes) and the form byte must fit (8)
    const uint32_t least = ROCKETNET_MESSAGE_FRAGMENT_BYTES + ROCKETNET_MESSAGE_RECORD_MAX_HEADER_BYTES + 1;
    if ( !channels || payload_capacity < least || payload_capacity > ROCKETNET_MAX_PAYLOAD_BYTES )
        return false;
    channels->payload_capacity = payload_capacity;
    return true;
}

uint32_t rn_channels_small_limit( const struct rn_channels_t * channels )
{
    if ( !channels || channels->payload_capacity < ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES + 2 )
        return 0;
    return channels->payload_capacity - 1 - ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES;
}

uint32_t rn_channels_enabled_codes( const struct rn_channels_t * channels )
{
    return channels ? channels->enabled_codes : 0;
}

int rn_channels_send_check( struct rn_channels_t * channels, int type, uint32_t channel )
{
    if ( !channels || type < 0 || type >= ROCKETNET_CHANNEL_NUM_TYPES || channel >= channels->num_channels )
        return ROCKETNET_CHANNEL_SEND_BAD_ARGUMENT;

    if ( ( channels->enabled_types & ( 1u << type ) ) == 0 )
    {
        // 7.5: a send with a reliability outside the mask is refused and
        // counted, never queued.
        rn_channels_count( channels->region->counters, ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED );
        return ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED;
    }

    return ROCKETNET_CHANNEL_SEND_OK;
}

uint32_t rn_channels_send_ordered( struct rn_channels_t * channels, uint32_t channel )
{
    if ( !channels || channel >= channels->num_channels )
        return 0;

    // RNChannel!Enqueue: ordered_write' = ordered_write + 1, and
    // sequenced_write' = 0 on the same channel. The reset runs although no
    // sequenced type is enabled (7.1), so a later sequenced card changes no
    // ordered trace.
    rn_channel_send_t * send = channels->region->channel_send + channel;
    const uint32_t ordering = send->ordered_write;
    send->ordered_write = ordering + 1;
    send->sequenced_write = 0;

    return ordering;
}

uint32_t rn_channels_send_sequenced( struct rn_channels_t * channels, uint32_t channel, uint32_t * out_ordering )
{
    if ( !channels || channel >= channels->num_channels )
        return 0;

    // RNChannel!Enqueue: ordering = ordered_write, sequenced_write' = @ + 1.
    rn_channel_send_t * send = channels->region->channel_send + channel;
    if ( out_ordering )
        *out_ordering = send->ordered_write;

    const uint32_t sequencing = send->sequenced_write;
    send->sequenced_write = sequencing + 1;

    return sequencing;
}

void rn_channels_deliver_ordered( struct rn_channels_t * channels, uint32_t channel, uint32_t ordering )
{
    if ( !channels || channel >= channels->num_channels )
        return;

    // RNChannel!DeliverMessage: ordered_read' = ordering + 1, and
    // sequenced_watermark' = 0. Both steps, one function, one path (7.1).
    rn_channel_receive_t * receive = channels->region->channel_receive + channel;
    receive->ordered_read = ordering + 1;
    receive->sequenced_watermark = 0;
}

bool rn_channels_deliver_sequenced( struct rn_channels_t * channels, uint32_t channel, uint32_t ordering, uint32_t sequencing )
{
    if ( !channels || channel >= channels->num_channels )
        return false;

    // RNChannel!DeliverMessage, the sequenced guard: only in this epoch.
    const rn_channel_receive_t * receive = channels->region->channel_receive + channel;
    if ( ordering != receive->ordered_read || sequencing < receive->sequenced_watermark )
        return false;

    rn_channel_receive_t * slot = channels->region->channel_receive + channel;
    slot->sequenced_watermark = sequencing + 1;

    return true;
}

void rn_channels_count_violation( struct rn_channels_t * channels, int counter )
{
    if ( !channels )
        return;
    channels->violations++;
    if ( counter != ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION )
        rn_channels_count( channels->region->counters, counter );

    // 17.3 item 11: a wire violation ends the connection. The owner reads
    // rn_ordered_decide and disconnects the peer; counted once per connection.
    if ( channels->error == ROCKETNET_MESSAGE_ERROR_NONE )
    {
        channels->error = ROCKETNET_MESSAGE_ERROR_PROTOCOL_VIOLATION;
        rn_channels_count( channels->region->counters, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
    }
}

int rn_channels_dispatch( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out || record->bytes < 1 || !record->data )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    // The receipt variants travel as their base codes, so a wire code 5, 6 or
    // 7 is a violation and a receipt type is dispatched under its base
    // (7.2's last line).
    const int code = record->code;

    switch ( code )
    {
        case ROCKETNET_MESSAGE_CODE_UNRELIABLE:
        {
            // RNChannel!OpenPacket then DeliverMessage: at most once, a view
            // into the transport's batch, no number taken (I3) and no window
            // check (I13). The batch is replaced every update, and this view
            // dies with it, so nothing is copied (THE COPIES (b)).
            // the unreliable budget Du: the rest are dropped and counted (6.3
            // step 4). Unreliable semantics allow it, so it is not a violation.
            if ( channels->unreliable_delivered >= channels->region->delivery_entries - channels->region->window ||
                 channels->deliveries >= channels->delivery_capacity )
            {
                rn_channels_count( channels->region->counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
                return ROCKETNET_CHANNEL_DISPATCH_NONE;
            }

            if ( record->channel >= channels->num_channels )
            {
                rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );
                return ROCKETNET_CHANNEL_DISPATCH_NONE;
            }

            rn_delivery_t * delivery = rn_channels_append_delivery( channels );
            delivery->data = record->data;                  // a view, pointer identity preserved
            delivery->bytes = record->bytes;
            delivery->channel = (uint16_t) record->channel;
            delivery->code = ROCKETNET_CHANNEL_TYPE_UNRELIABLE;
            delivery->serial = 0;
            delivery->descriptor = RN_DESCRIPTOR_NONE;
            *out = *delivery;

            channels->unreliable_delivered++;

            return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
        }

        case ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED:
            return rn_channels_dispatch_ordered( channels, record, out );

        case ROCKETNET_MESSAGE_CODE_RELIABLE:
            return rn_channels_dispatch_reliable( channels, record, out );

        case ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED:
        case ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED:
            return rn_channels_dispatch_sequenced( channels, record, out );

        default:
            // A wire code outside 0..4: 5, 6 and 7 are receipt types, which
            // travel as 0, 2 and 3, so their arrival is a violation (13.2).
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }
}

// ---------------------------------------------------------------
//                  RELIABLE_ORDERED, the receive side
// ---------------------------------------------------------------

void rn_channels_drain( struct rn_channels_t * channels, uint32_t channel )
{
    // 7.2 Drain: after an ordered delivery on a channel, deliver the held
    // message whose ordering index is now next, from the ring, and go on.
    // Each step is O(1), and the drain is bounded by the held messages.
    // RNChannel!DeliverMessage, once per message.
    if ( !channels || channel >= channels->num_channels )
        return;

    rn_region_t * region = channels->region;
    rn_channel_receive_t * rx = region->channel_receive + channel;

    for ( ;; )
    {
        const uint32_t held = rn_held_find( region, rn_held_key( channel, rx->ordered_read ) );
        if ( held == RN_DESCRIPTOR_NONE || region->held_index[held].used != RN_HELD_COMPLETE )
            break;                                              // not here yet, or a block still short of fragments

        rn_delivery_t * delivery = rn_channels_reliable_delivery( channels );
        if ( !delivery )
            break;

        const uint32_t slot = region->held_index[held].number_slot;
        const rn_receive_descriptor_t * descriptor = &region->receive_descriptors[slot];

        // the held message is delivered where it lies in the ring: a view, no copy
        delivery->data = region->receive_ring + (uint64_t) descriptor->ring * 16;
        delivery->bytes = (int32_t) descriptor->bytes;
        delivery->channel = (uint16_t) channel;
        delivery->code = ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED;
        delivery->descriptor = slot;

        rn_held_erase( region, held );
        rn_channels_deliver_ordered( channels, channel, rx->ordered_read );
    }
}

int rn_channels_dispatch_ordered( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    rn_region_t * region = channels->region;

    if ( record->channel >= channels->num_channels )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    // The ordering index is 24 bits on the wire: the difference from the
    // channel's read index, modulo 2^24, is the record's place. Zero is its
    // turn, under W is ahead (a sender numbers fewer than W records at once, so
    // no legitimate record is further), and the upper half is behind.
    rn_channel_receive_t * rx = region->channel_receive + record->channel;
    const uint32_t ordering = rn_u24( record->ordering );
    const uint32_t delta = rn_u24( ordering - rx->ordered_read );

    if ( delta >= ( 1u << 23 ) )
    {
        // 7.2: a reliable ordered record behind the read index can only be a
        // duplicate, and layer 3 already caught those.
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( delta >= region->window )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( record->block )
        return rn_channels_dispatch_chunk( channels, record, out, delta );

    uint32_t slot = 0;
    rn_receive_descriptor_t * descriptor = rn_channels_descriptor( channels, record, &slot );
    if ( !descriptor )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( delta == 0 )
    {
        // A block still collecting chunks holds this ordering index: two messages
        // with one ordering index, as on the held path below.
        if ( rn_held_find( region, rn_held_key( record->channel, ordering ) ) != RN_DESCRIPTOR_NONE )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }

        // Its turn: delivered as a view into the batch (the record arrived this
        // update), then the coupling runs and the drain follows (7.2).
        // RNChannel!DeliverMessage.
        rn_delivery_t * delivery = rn_channels_reliable_delivery( channels );
        if ( !delivery )
            return ROCKETNET_CHANNEL_DISPATCH_NONE;

        delivery->data = record->data;
        delivery->bytes = record->bytes;
        delivery->channel = (uint16_t) record->channel;
        delivery->code = ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED;
        delivery->descriptor = slot;                    // live until the update ends (6.3 step 1)
        *out = *delivery;

        rn_channels_deliver_ordered( channels, record->channel, rx->ordered_read );
        rn_channels_drain( channels, record->channel );

        return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
    }

    // Ahead of its turn: copy it into the ring at the offset the sender chose
    // (THE COPIES (b): the batch is replaced every update, this record must
    // outlive it), index it by (channel, ordering), and keep its descriptor live.
    // The receiver only checks the bounds (4.3).
    const uint64_t offset = (uint64_t) record->ring * 16;
    if ( !rn_region_ring_span_valid( region, offset, rn_round16( (uint64_t) record->bytes ) ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint64_t key = rn_held_key( record->channel, ordering );
    if ( rn_held_find( region, key ) != RN_DESCRIPTOR_NONE )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );     // two messages with one ordering index
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( !rn_held_insert( region, key, slot, RN_HELD_COMPLETE ) )
    {
        rn_region_exhaust( region );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    memcpy( region->receive_ring + offset, record->data, (size_t) record->bytes );
    descriptor->ring = record->ring;
    descriptor->bytes = (uint32_t) record->bytes;

    return ROCKETNET_CHANNEL_DISPATCH_HELD;
}

// A chunk of a large message (CHANNELS.md 8): placed by index, counted, and the
// whole message dispatched as one only when every chunk is in.

static int rn_channels_dispatch_chunk( rn_channels_t * channels, const struct rocketnet_message_record_t * record, rn_delivery_t * out, uint32_t delta )
{
    rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t window = region->window;
    const uint32_t count = record->fragments + 1;
    const uint32_t index = record->fragment;
    const uint32_t max_count = ( region->max_message_bytes + F - 1 ) / F;

    // a count at least two, inside W, at most what max_message_bytes allows, and
    // an index inside it: a sender that breaks these is a violation, never memory
    // corruption (6.3)
    if ( record->fragments == 0 || count > max_count || count > window || index >= count )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    // every chunk but the last is exactly F bytes (8)
    const bool last = index + 1 == count;
    if ( last ? ( record->bytes < 1 || (uint32_t) record->bytes > F ) : (uint32_t) record->bytes != F )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    // the block's span: its base is this chunk's ring offset less index * F, and
    // it ends inside the ring (check 8). A chunk that is not the last knows the
    // end only to within the last chunk's one byte.
    const int64_t base_units = (int64_t) record->ring - (int64_t) index * (int64_t) ( F / 16 );
    const uint64_t span_end = (uint64_t) ( base_units < 0 ? 0 : base_units ) * 16 + (uint64_t) ( count - 1 ) * F + ( last ? rn_round16( (uint64_t) record->bytes ) : 16 );
    if ( base_units < 0 || span_end > region->ring_bytes )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    uint32_t slot = 0;
    rn_receive_descriptor_t * descriptor = rn_channels_descriptor( channels, record, &slot );
    if ( !descriptor )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t first_number = rn_u24( record->number - index );
    const uint32_t first_slot = first_number & ( window - 1 );
    const uint64_t key = rn_held_key( record->channel, record->ordering );

    // Any chunk opens the block's state on its own (8, "No start packet"): the
    // first one seen is the block's anchor, its descriptor holds the count, the
    // chunks in, and the block's first number, and the held index finds it by
    // (channel, ordering).
    uint32_t held = rn_held_find( region, key );
    rn_receive_descriptor_t * anchor = descriptor;

    if ( held == RN_DESCRIPTOR_NONE )
    {
        if ( !rn_held_insert( region, key, slot, RN_HELD_COLLECTING ) )
        {
            rn_region_exhaust( region );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }
        held = rn_held_find( region, key );
        descriptor->block_count = (uint16_t) count;
        descriptor->block_have = 0;
        descriptor->sequencing = first_number;
    }
    else
    {
        anchor = &region->receive_descriptors[region->held_index[held].number_slot];

        // the block's count, first number and base must agree with the first
        // chunk seen, or this chunk belongs to something else
        const uint32_t anchor_index = rn_u24( (uint32_t) anchor->number - first_number );
        if ( region->held_index[held].used != RN_HELD_COLLECTING || !anchor->live ||
             anchor->block_count != count || anchor->sequencing != first_number || anchor_index >= count ||
             (int64_t) anchor->ring - (int64_t) anchor_index * (int64_t) ( F / 16 ) != base_units )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }
    }

    // Placed by index, not by arrival: ring + index * F, which is this chunk's
    // own offset (THE COPIES (b): a chunk must outlive the batch).
    memcpy( region->receive_ring + (uint64_t) record->ring * 16, record->data, (size_t) record->bytes );
    anchor->block_have++;

    if ( anchor->block_have < count )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;                 // never a partial delivery

    // Every chunk is in. The block is one contiguous message in the ring.
    rn_receive_descriptor_t * first = &region->receive_descriptors[first_slot];
    const rn_receive_descriptor_t * final_chunk = &region->receive_descriptors[( first_number + count - 1 ) & ( window - 1 )];
    const uint32_t last_bytes = last ? (uint32_t) record->bytes : final_chunk->bytes;
    const uint32_t total = ( count - 1 ) * F + last_bytes;
    if ( !first->live || rn_u24( (uint32_t) first->number ) != first_number || !final_chunk->live ||
         rn_u24( (uint32_t) final_chunk->number ) != rn_u24( first_number + count - 1 ) || total > region->max_message_bytes )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( anchor != first )
        anchor->block_count = 0;
    first->block_count = (uint16_t) count;                      // released together at the update's end
    first->block_have = (uint16_t) count;
    first->bytes = total;
    first->ring = (uint32_t) base_units;

    region->held_index[held].number_slot = first_slot;
    region->held_index[held].used = RN_HELD_COMPLETE;

    if ( delta != 0 )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;                 // complete, waiting for its turn

    // Its turn: delivered whole, in place, a view of the ring (no reassembly
    // buffer, no second copy), then the coupling and the drain.
    rn_delivery_t * delivery = rn_channels_reliable_delivery( channels );
    if ( !delivery )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    rn_channel_receive_t * rx = region->channel_receive + record->channel;
    delivery->data = region->receive_ring + (uint64_t) first->ring * 16;
    delivery->bytes = (int32_t) total;
    delivery->channel = (uint16_t) record->channel;
    delivery->code = ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED;
    delivery->descriptor = first_slot;
    *out = *delivery;

    rn_held_erase( region, held );
    rn_channels_deliver_ordered( channels, record->channel, rx->ordered_read );
    rn_channels_drain( channels, record->channel );

    return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
}

uint32_t rn_channels_feed_acks( struct rn_window_ack_t * ack, const uint64_t * packet_ids, uint32_t count, double now )
{
    // RNChannel!ReceiveAck, R1's one path: the transport's packet ids into
    // layer 3's sent map, keyed by the id the transport assigned.
    if ( !ack || !packet_ids || count == 0 )
        return 0;

    return rn_window_ack_on_acks( ack, packet_ids, count, now );
}

// Frees one delivery's receiver descriptors: the message's number, or a block's
// every fragment's (6.3 step 1: a number is live while held or delivered in the
// current update).

static void rn_channels_release( rn_channels_t * channels, uint32_t slot )
{
    rn_region_t * region = channels->region;
    if ( slot >= region->window )
        return;

    const uint64_t number = region->receive_descriptors[slot].number;
    if ( !region->receive_descriptors[slot].live )
        return;

    const uint32_t count = region->receive_descriptors[slot].block_count > 0 ? region->receive_descriptors[slot].block_count : 1;
    for ( uint32_t i = 0; i < count; i++ )
    {
        rn_receive_descriptor_t * descriptor = &region->receive_descriptors[( number + i ) & ( region->window - 1 )];
        if ( descriptor->live && descriptor->number == number + i )
        {
            descriptor->live = 0;
            descriptor->block_count = 0;
            descriptor->block_have = 0;
        }
    }
}

void rn_channels_end_update( struct rn_channels_t * channels )
{
    if ( !channels )
        return;

    // RNChannel!EndUpdate: the batch's views are gone and the update's
    // deliveries expire, so their numbers stop being live, and the per-update
    // counts start over.
    for ( uint32_t i = 0; i < channels->deliveries; i++ )
    {
        const uint32_t slot = channels->region->deliveries[i].descriptor;
        if ( slot != RN_DESCRIPTOR_NONE )
            rn_channels_release( channels, slot );
    }

    channels->unreliable_delivered = 0;
    channels->deliveries = 0;
    channels->violations = 0;
}

// ---------------------------------------------------------------
//                 the send API (RNChannel!Enqueue)
// ---------------------------------------------------------------

static void rn_ready_push( rn_channels_t * channels, uint32_t channel )
{
    rn_channel_send_t * send = channels->region->channel_send;
    send[channel].ready_prev = channels->ready_tail;
    send[channel].ready_next = RN_DESCRIPTOR_NONE;
    if ( channels->ready_tail != RN_DESCRIPTOR_NONE )
        send[channels->ready_tail].ready_next = channel;
    else
        channels->ready_head = channel;
    channels->ready_tail = channel;
}

static void rn_ready_remove( rn_channels_t * channels, uint32_t channel )
{
    rn_channel_send_t * send = channels->region->channel_send;
    const uint32_t prev = send[channel].ready_prev;
    const uint32_t next = send[channel].ready_next;
    if ( prev != RN_DESCRIPTOR_NONE )
        send[prev].ready_next = next;
    else
        channels->ready_head = next;
    if ( next != RN_DESCRIPTOR_NONE )
        send[next].ready_prev = prev;
    else
        channels->ready_tail = prev;
    send[channel].ready_prev = RN_DESCRIPTOR_NONE;
    send[channel].ready_next = RN_DESCRIPTOR_NONE;
}

uint8_t * rn_channels_start( struct rn_channels_t * channels, uint32_t channel, int type, uint32_t bytes )
{
    if ( !channels || channels->pending != RN_DESCRIPTOR_NONE )
        return NULL;                                            // one start at a time, like the transport's start_packet

    rn_region_t * region = channels->region;

    // RNChannel!RefuseSend: a type outside the mask is refused and counted (7.5)
    if ( rn_channels_send_check( channels, type, channel ) != ROCKETNET_CHANNEL_SEND_OK || type != ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED )
        return NULL;

    // an exhausted connection sends nothing more: the owner kicks it (R5)
    if ( rn_region_exhausted( region ) || bytes < 1 || bytes > region->max_message_bytes )
        return NULL;

    // the room is the region's, one descriptor and its bytes; no room exhausts it
    const uint32_t d = bytes >= RN_SPAN_MIN_BYTES ? rn_region_send_alloc_span( region, (int) bytes ) : rn_region_send_alloc( region, (int) bytes );
    if ( d == RN_DESCRIPTOR_NONE )
        return NULL;

    channels->pending = d;
    channels->pending_bytes = bytes;
    channels->pending_channel = channel;

    return rn_region_send_write_pointer( region, d );
}

bool rn_channels_finish( struct rn_channels_t * channels, uint32_t bytes )
{
    if ( !channels || channels->pending == RN_DESCRIPTOR_NONE )
        return false;

    rn_region_t * region = channels->region;
    const uint32_t d = channels->pending;
    const uint32_t channel = channels->pending_channel;

    if ( bytes < 1 || bytes > channels->pending_bytes )
        return false;                                           // the start stays pending

    channels->pending = RN_DESCRIPTOR_NONE;

    // RNChannel!Enqueue: the ordering index is given at acceptance (7.1). A
    // message whose record fits one packet is SMALL and takes one number; a
    // larger one is a block of ceil(bytes / F) chunks, one number each (8).
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    descriptor->bytes = bytes;
    descriptor->fragments = (uint16_t) ( bytes <= rn_channels_small_limit( channels ) ? 1 : ( bytes + F - 1 ) / F );
    descriptor->channel = (uint16_t) channel;
    descriptor->reliability = ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED;
    descriptor->priority = ROCKETNET_PRIORITY_MEDIUM;
    descriptor->ordering = rn_channels_send_ordered( channels, channel );
    descriptor->next = RN_DESCRIPTOR_NONE;

    // the channel's FIFO: first writes are in ordering order (9.2)
    rn_channel_send_t * send = region->channel_send + channel;
    const bool was_empty = send->queue_head == RN_DESCRIPTOR_NONE;
    if ( was_empty )
        send->queue_head = d;
    else
        region->send_descriptors[send->queue_tail].next = d;
    send->queue_tail = d;
    send->queued[descriptor->priority]++;
    if ( was_empty )
        rn_ready_push( channels, channel );

    return true;
}

bool rn_channels_cancel( struct rn_channels_t * channels )
{
    if ( !channels || channels->pending == RN_DESCRIPTOR_NONE )
        return false;
    rn_region_send_free( channels->region, channels->pending );
    channels->pending = RN_DESCRIPTOR_NONE;
    return true;
}

// ---------------------------------------------------------------
//        the ordered path of one connection, ends to end
// ---------------------------------------------------------------

bool rn_ordered_init( struct rn_ordered_t * ordered, struct rn_region_t * region, const struct rocketnet_message_config_t * config,
                      struct rocketnet_send_budget_t * budget, int max_packets_per_update, int payload_capacity )
{
    if ( !ordered )
        return false;

    memset( ordered, 0, sizeof( *ordered ) );

    if ( !region || payload_capacity < 1 )
        return false;

    if ( !rn_channels_init( &ordered->channels, region, region->num_channels, 0 ) ||
         !rn_channels_set_payload_capacity( &ordered->channels, (uint32_t) payload_capacity ) )
    {
        memset( ordered, 0, sizeof( *ordered ) );
        return false;
    }

    rn_window_send_init( &ordered->send, region, 0 );
    rn_window_ack_init( &ordered->ack, &ordered->send, region, config );
    rn_window_receive_init( &ordered->receive, region, 0 );

    if ( !rn_packet_builder_init( &ordered->builder, region, &ordered->ack, budget, max_packets_per_update, payload_capacity ) )
    {
        memset( ordered, 0, sizeof( *ordered ) );
        return false;
    }

    ordered->budget = budget;
    ordered->limits.num_channels = (int) region->num_channels;
    ordered->limits.enabled_codes = ordered->channels.enabled_codes;
    ordered->limits.window_messages = (int) region->window;
    ordered->limits.max_message_bytes = (int) region->max_message_bytes;
    ordered->limits.receive_ring_bytes = (int) region->ring_bytes;

    return true;
}

// One number as the builder's item: the bytes at the start of its region
// record (THE COPIES (a): the frame is written from there), with the fields its
// record carries. The ring offset is the receive ring offset the window gave it.

static bool rn_ordered_item( const rn_ordered_t * ordered, const rn_send_number_t * slot, int send_class, rn_packet_item_t * item )
{
    const rn_region_t * region = ordered->channels.region;
    if ( slot->descriptor == RN_DESCRIPTOR_NONE || slot->descriptor >= region->queue )
        return false;

    const rn_send_descriptor_t * descriptor = &region->send_descriptors[slot->descriptor];
    if ( !descriptor->in_use )
        return false;

    uint32_t bytes = 0;
    const uint8_t * data = rn_region_send_data( region, slot->descriptor, slot->fragment, &bytes );
    if ( !data || bytes == 0 )
        return false;

    memset( item, 0, sizeof( *item ) );
    item->data = data;
    item->bytes = bytes;
    item->number = slot->number;
    item->ordering = rn_u24( descriptor->ordering );
    item->descriptor = slot->descriptor;
    item->channel = descriptor->channel;
    item->code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    item->send_class = (uint8_t) send_class;
    item->priority = descriptor->priority < ROCKETNET_PACKET_BUILDER_LEVELS ? descriptor->priority : ROCKETNET_PRIORITY_MEDIUM;
    item->ring = (uint32_t) ( ( slot->ring_start % region->ring_bytes ) / 16 );

    if ( descriptor->fragments > 1 )
    {
        item->block = 1;                                        // one chunk, in its own packet (8)
        item->alone = 1;
        item->fragment = slot->fragment;
        item->fragments = (uint32_t) descriptor->fragments - 1;
    }

    return true;
}

// Links a never-sent number into the resend FIFO behind the numbers already
// waiting to be sent, ahead of every number that was sent (the FIFO is ordered
// by last send, and a number never sent is due now): it is due the next update
// it was not written in. RNChannel!NumberRecord's number joins the resend set.

static void rn_ordered_park( rn_window_ack_t * ack, uint32_t after, uint32_t slot )
{
    rn_resend_link_t * fifo = ack->region->resend_fifo;
    if ( after == RN_RESEND_NONE )
    {
        fifo[slot].prev = RN_RESEND_NONE;
        fifo[slot].next = ack->resend_head;
        if ( ack->resend_head != RN_RESEND_NONE )
            fifo[ack->resend_head].prev = slot;
        else
            ack->resend_tail = slot;
        ack->resend_head = slot;
    }
    else
    {
        const uint32_t next = fifo[after].next;
        fifo[slot].prev = after;
        fifo[slot].next = next;
        fifo[after].next = slot;
        if ( next != RN_RESEND_NONE )
            fifo[next].prev = slot;
        else
            ack->resend_tail = slot;
    }
    ack->resend_count++;
}

int rn_ordered_write( struct rn_ordered_t * ordered, double now, uint8_t ** payloads, int num_payloads )
{
    if ( !ordered || !payloads || num_payloads < 1 )
        return 0;

    rn_channels_t * channels = &ordered->channels;
    rn_region_t * region = channels->region;
    rn_window_ack_t * ack = &ordered->ack;
    rn_packet_builder_t * builder = &ordered->builder;

    builder->packets = 0;
    builder->unreliable_dropped = 0;

    // an exhausted or ended connection does nothing but be kicked (R5), and a
    // reliable message past its timeout ends it (6.4 "Giving up")
    if ( rn_region_exhausted( region ) || channels->error != ROCKETNET_MESSAGE_ERROR_NONE || rn_window_ack_check_timeout( ack, now ) )
        return 0;

    if ( ordered->budget )
        rocketnet_send_budget_begin_update( ordered->budget, now );

    const int packet_limit = builder->max_packets_per_update < num_payloads ? builder->max_packets_per_update : num_payloads;
    const uint32_t capacity = channels->payload_capacity;
    const uint64_t room = (uint64_t) packet_limit * ( capacity - 1 );
    uint64_t planned = 0;

    rn_packet_item_t items[ROCKETNET_ORDERED_MAX_ITEMS];
    int count = 0;

    // 1. The resend FIFO, oldest first (RNWindow!WriteOldest): a number never
    // sent is due now (a new message's, a chunk's, a packet the transport
    // refused); a sent one is due when its RTO, backed off, has passed. The walk
    // stops at the first number not yet due.
    const double delay = ack->rto * (double) ack->backoff;
    uint32_t cursor = ack->resend_head;
    uint32_t last_unsent = RN_RESEND_NONE;
    bool walked = true;

    while ( cursor != RN_RESEND_NONE )
    {
        const rn_send_number_t * slot = &region->send_numbers[cursor];
        const uint32_t next = region->resend_fifo[cursor].next;

        if ( slot->acked )
        {
            cursor = next;
            continue;
        }

        const bool unsent = slot->last_send <= 0.0;
        if ( !unsent && !( now >= slot->last_send + delay ) )
            break;

        if ( count >= ROCKETNET_ORDERED_MAX_ITEMS || planned >= room )
        {
            walked = false;                                     // more is due than this update can write
            break;
        }

        if ( unsent )
            last_unsent = cursor;

        if ( rn_ordered_item( ordered, slot, unsent ? ROCKETNET_SEND_CLASS_ORDERED_PROGRESS : ROCKETNET_SEND_CLASS_RETRANSMIT, &items[count] ) )
        {
            items[count].resend = unsent ? 0 : 1;
            planned += items[count].alone ? ( capacity - 1 ) : (uint64_t) items[count].bytes + ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES;
            count++;
        }

        cursor = next;
    }

    if ( walked )
    {
        // never-sent numbers sit at the FIFO's head; the last of them is where
        // new numbers join
        while ( cursor != RN_RESEND_NONE && region->send_numbers[cursor].last_send <= 0.0 )
        {
            last_unsent = cursor;
            cursor = region->resend_fifo[cursor].next;
        }
    }

    // 2. New messages, numbered late (6.2) and whole (8), channels taking turns
    // one message each (17.3 item 4), while the update has room. The channel's
    // FIFO head is numbered first, so first writes are in ordering order (9.2).
    // A message the window cannot yet admit stops numbering until acks move U'
    // (RNWindow!NumberRecord, RNRing!PlaceSingle and PlaceBlock); a stall never
    // blocks what is already numbered, or the unreliable records.
    while ( walked && planned < room && count < ROCKETNET_ORDERED_MAX_ITEMS && channels->ready_head != RN_DESCRIPTOR_NONE )
    {
        const uint32_t channel = channels->ready_head;
        rn_channel_send_t * send = region->channel_send + channel;
        const uint32_t d = send->queue_head;
        rocketnet_assert( d != RN_DESCRIPTOR_NONE );
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];

        uint64_t first = 0;
        uint64_t ring_start = 0;
        uint32_t numbers = 0;

        if ( descriptor->fragments > 1 )
            numbers = rn_window_send_first_write_block( &ordered->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, descriptor->bytes, d, &first, &ring_start );
        else if ( rn_window_send_first_write( &ordered->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, descriptor->bytes, d, &first, &ring_start ) == RN_WINDOW_SEND_NUMBERED )
            numbers = 1;

        if ( numbers == 0 )
            break;                                              // stalled: the window counted it

        // dequeue
        send->queue_head = descriptor->next;
        if ( send->queue_head == RN_DESCRIPTOR_NONE )
            send->queue_tail = RN_DESCRIPTOR_NONE;
        send->queued[region->send_descriptors[d].priority]--;
        region->send_descriptors[d].next = RN_DESCRIPTOR_NONE;

        // channels take turns: this one rejoins the back if it has more
        rn_ready_remove( channels, channel );
        if ( send->queue_head != RN_DESCRIPTOR_NONE )
            rn_ready_push( channels, channel );

        // every number of the message joins the resend set, due now; the ones
        // this update has room for are also planned into packets now
        for ( uint32_t i = 0; i < numbers; i++ )
        {
            const uint32_t slot_index = (uint32_t) ( ( first + i ) & ( region->window - 1 ) );
            rn_ordered_park( ack, last_unsent, slot_index );
            last_unsent = slot_index;

            if ( count < ROCKETNET_ORDERED_MAX_ITEMS && planned < room &&
                 rn_ordered_item( ordered, &region->send_numbers[slot_index], ROCKETNET_SEND_CLASS_NEW_RELIABLE, &items[count] ) )
            {
                planned += items[count].alone ? ( capacity - 1 ) : (uint64_t) items[count].bytes + ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES;
                count++;
            }
        }
    }

    if ( count == 0 )
        return 0;

    // 3. Packets (9.3), under the send budget (R6)
    const int written = rn_packet_builder_write( builder, items, count, payloads, num_payloads );

    // 4. The RTO's back-off: once per update that resent a number whose RTO had
    // passed, up to 8 (6.4); each such resend is counted
    uint32_t expired = 0;
    for ( int i = 0; i < count; i++ )
    {
        if ( items[i].taken && items[i].resend )
        {
            expired++;
            rn_channels_count( region->counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
        }
    }

    if ( expired > 0 && ack->backoff < 8 )
        ack->backoff = ack->backoff * 2 > 8 ? 8 : ack->backoff * 2;

    return written;
}

void rn_ordered_commit( struct rn_ordered_t * ordered, int packet_index, uint64_t packet_id, double now )
{
    if ( ordered )
        rn_packet_builder_commit( &ordered->builder, packet_index, packet_id, now );
}

void rn_ordered_packet_failed( struct rn_ordered_t * ordered, int packet_index, double now )
{
    if ( ordered )
        rn_packet_builder_packet_failed( &ordered->builder, packet_index, now );
}

bool rn_ordered_settle( struct rn_ordered_t * ordered, int packet_index, int actual_wire_bytes )
{
    return ordered && rn_packet_builder_settle( &ordered->builder, packet_index, actual_wire_bytes );
}

uint32_t rn_ordered_acks( struct rn_ordered_t * ordered, const uint64_t * packet_ids, uint32_t count, double now )
{
    if ( !ordered )
        return 0;
    return rn_channels_feed_acks( &ordered->ack, packet_ids, count, now );
}

int rn_ordered_receive( struct rn_ordered_t * ordered, const uint8_t * payload, int bytes )
{
    if ( !ordered || !payload )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_FORM;

    rn_channels_t * channels = &ordered->channels;

    // an ended or exhausted connection takes nothing more
    if ( channels->error != ROCKETNET_MESSAGE_ERROR_NONE || rn_region_exhausted( channels->region ) )
        return ROCKETNET_MESSAGE_VIOLATION_NONE;

    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &ordered->limits );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
        return violation;
    }

    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        // layer 3: a numbered record passes the window first (RNWindow!Accept)
        const bool numbered = ( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE || record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED ||
                                record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED );
        if ( numbered )
        {
            uint32_t slot = RN_DESCRIPTOR_NONE;
            const int accepted = rn_window_receive_accept( &ordered->receive, &record, &slot );
            if ( accepted == RN_WINDOW_RECEIVE_DUPLICATE )
                continue;                                       // counted by layer 3; normal after a resend
            if ( accepted == RN_WINDOW_RECEIVE_VIOLATION )
            {
                rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
                return ordered->receive.violation;
            }
        }

        // layer 4: the dispatch
        rn_delivery_t delivery;
        rn_channels_dispatch( channels, &record, &delivery );
        if ( channels->error != ROCKETNET_MESSAGE_ERROR_NONE )
            return ROCKETNET_CHANNEL_VIOLATION;
        if ( rn_region_exhausted( channels->region ) )
            return ROCKETNET_MESSAGE_VIOLATION_NONE;
    }

    return ROCKETNET_MESSAGE_VIOLATION_NONE;
}

uint32_t rn_ordered_deliveries( const struct rn_ordered_t * ordered )
{
    return ordered ? ordered->channels.deliveries : 0;
}

void rn_ordered_end_update( struct rn_ordered_t * ordered )
{
    if ( ordered )
        rn_channels_end_update( &ordered->channels );
}

int rn_ordered_decide( const struct rn_ordered_t * ordered )
{
    if ( !ordered || !ordered->channels.region )
        return ROCKETNET_MESSAGE_ERROR_NONE;

    // tla/RNConnectionMemory.tla, Kick: exhaustion first; then the peer broke
    // the wire (17.3 item 11); then a reliable message went unacked too long
    const int memory = rn_connection_memory_decide( ordered->channels.region );
    if ( memory != ROCKETNET_MESSAGE_ERROR_NONE )
        return memory;
    if ( ordered->channels.error != ROCKETNET_MESSAGE_ERROR_NONE )
        return ordered->channels.error;
    return ordered->ack.error;
}
