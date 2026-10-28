/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

#include <string.h>

#include "rocketnet_platform.h"

// Layer 4's dispatch (docs/CHANNELS.md 7). The spec is tla/RNChannel.tla; each
// function names the action it implements. Nothing here allocates, locks or
// reads a clock (owner rules R2 and R7). Channel and ready queues name a
// descriptor generation and the region's schedule epoch (tla/RNChannelQueues.tla).

static void rn_channels_count( rocketnet_counters_t * counters, int counter )
{
    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &counters->values[counter], 1 );
}

static inline uint32_t rn_u24( uint32_t value )
{
    return value & ROCKETNET_MESSAGE_U24_MASK;
}

static inline uint64_t rn_round16( uint64_t bytes )
{
    return ( bytes + 15 ) & ~(uint64_t) 15;
}

// The dispatch half of m4 checks 7 and 8: a block's shape bounds (count at
// least two, inside W and max_message_bytes, index inside the count, every
// fragment but the last exactly F) and its base units. The layer 2 reader works
// on wire limits before a region exists, so it checks the same bounds apart; the
// caller checks the block's span and the fragment's own ring span and counts its
// own counter.

bool rn_block_shape_valid( const rn_channels_t * channels, const struct rocketnet_message_record_t * record, int64_t * out_base_units )
{
    const rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = record->fragments + 1;
    const uint32_t index = record->fragment;
    const uint32_t max_count = ( region->max_message_bytes + F - 1 ) / F;
    const bool last = index + 1 == count;

    if ( count < 2 || count > max_count || count > region->window || index >= count ||
         ( last ? ( record->bytes < 1 || (uint32_t) record->bytes > F ) : (uint32_t) record->bytes != F ) )
        return false;

    *out_base_units = (int64_t) record->ring - (int64_t) index * ( F / 16 );

    return true;
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
// A code-2 block collecting its fragments is keyed by (RN_HELD_RELIABLE_BLOCK,
// first number), a channel no channel index reaches.
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

rn_receive_descriptor_t * rn_channels_descriptor( rn_channels_t * channels, const struct rocketnet_message_record_t * record, uint32_t * out_slot )
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

    // A fresh binding starts with empty flat queues: no descriptor stamped
    // under an earlier binding of this region is a member of this binding's
    // channel or unreliable sequences, and no channel is ready. This is the
    // flat-storage form of the carved queue heads rn_region_carve resets;
    // a binding made without a carve gets the same empty start.

    memset( region->channel_send, 0, sizeof( rn_channel_send_t ) * num_channels );

    for ( uint32_t c = 0; c < num_channels; c++ )
    {
        region->channel_send[c].queue_head = RN_DESCRIPTOR_NONE;
        region->channel_send[c].queue_tail = RN_DESCRIPTOR_NONE;
        region->channel_send[c].ready_prev = RN_DESCRIPTOR_NONE;
        region->channel_send[c].ready_next = RN_DESCRIPTOR_NONE;
    }

    for ( uint32_t d = 0; d < region->descriptors_fresh; d++ )
    {
        region->send_descriptors[d].enqueue_ticket = 0;
        region->send_descriptors[d].schedule_flags = 0;
    }

    channels->enabled_types = enabled_types != 0 ? enabled_types : ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION;
    channels->enabled_codes = rocketnet_message_enabled_codes( channels->enabled_types );
    channels->delivery_capacity = region->delivery_entries;
    channels->payload_capacity = ROCKETNET_MESSAGE_SINGLE_BYTES;
    channels->error = ROCKETNET_DISCONNECT_REASON_NONE;
    channels->pending = RN_DESCRIPTOR_NONE;
    channels->ready_head = RN_DESCRIPTOR_NONE;
    channels->ready_tail = RN_DESCRIPTOR_NONE;
    channels->ready_ticket_next = 1;
    channels->schedule_work = 0;

    return true;
}

bool rn_channels_set_payload_capacity( struct rn_channels_t * channels, uint32_t payload_capacity )
{
    // a chunk's record (the longest header, then F bytes) and the form byte must fit (8)

    const uint32_t least = ROCKETNET_MESSAGE_FRAGMENT_BYTES + ROCKETNET_MESSAGE_RECORD_MAX_HEADER_BYTES + 1;
    if ( !channels || payload_capacity < least || payload_capacity > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
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

int rn_channels_send_allowed( const struct rn_channels_t * channels, int type, uint32_t channel )
{
    if ( !channels || type < 0 || type >= ROCKETNET_CHANNEL_NUM_TYPES || channel >= channels->num_channels )
        return ROCKETNET_CHANNEL_SEND_BAD_ARGUMENT;

    if ( ( channels->enabled_types & ( 1u << type ) ) == 0 )
        return ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED;

    return ROCKETNET_CHANNEL_SEND_OK;
}

int rn_channels_send_check( struct rn_channels_t * channels, int type, uint32_t channel )
{
    const int result = rn_channels_send_allowed( channels, type, channel );

    // 7.5: a send with a reliability outside the mask is refused and
    // counted, never queued.

    if ( result == ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED )
        rn_channels_count( channels->region->counters, ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED );

    return result;
}

// tla/RNChannel.tla Enqueue

uint32_t rn_channels_send_ordered( struct rn_channels_t * channels, uint32_t channel )
{
    if ( !channels || channel >= channels->num_channels )
        return 0;

    // RNChannel!Enqueue: ordered_write' = ordered_write + 1, and
    // sequenced_write' = 0 on the same channel. The reset runs although no
    // sequenced type is enabled (7.1), so enabling a sequenced type changes
    // no ordered trace.

    rn_channel_send_t * send = channels->region->channel_send + channel;
    const uint32_t ordering = send->ordered_write;
    send->ordered_write = ordering + 1;
    send->sequenced_write = 0;

    return ordering;
}

// tla/RNChannel.tla Enqueue, GhostSequencedWrite: a sequenced send moves sequenced_write past zero on its channel

uint32_t rn_channels_send_sequenced( struct rn_channels_t * channels, uint32_t channel, uint32_t * out_ordering )
{
    if ( !channels || channel >= channels->num_channels )
        return 0;

    // RNChannel!Enqueue: ordering = ordered_write, sequenced_write' = @ + 1

    rn_channel_send_t * send = channels->region->channel_send + channel;

    if ( out_ordering )
        *out_ordering = send->ordered_write;

    const uint32_t sequencing = send->sequenced_write;
    send->sequenced_write = sequencing + 1;

    return sequencing;
}

// tla/RNChannel.tla DeliverMessage

void rn_channels_deliver_ordered( struct rn_channels_t * channels, uint32_t channel, uint32_t ordering )
{
    if ( !channels || channel >= channels->num_channels )
        return;

    // RNChannel!DeliverMessage: ordered_read' = ordering + 1, and sequenced_watermark' = 0. Both steps, one function, one path (7.1).

    rn_channel_receive_t * receive = channels->region->channel_receive + channel;
    receive->ordered_read = ordering + 1;
    receive->sequenced_watermark = 0;
}

// tla/RNChannel.tla DeliverMessage, GhostSequencedRead: a sequenced delivery moves sequenced_watermark past zero on its channel

bool rn_channels_deliver_sequenced( struct rn_channels_t * channels, uint32_t channel, uint32_t ordering, uint32_t sequencing )
{
    if ( !channels || channel >= channels->num_channels )
        return false;

    // RNChannel!DeliverMessage, the sequenced guard: only in this epoch

    const rn_channel_receive_t * receive = channels->region->channel_receive + channel;
    const uint32_t sequence_gap = ( sequencing - receive->sequenced_watermark ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( ( ordering & ROCKETNET_MESSAGE_U24_MASK ) != ( receive->ordered_read & ROCKETNET_MESSAGE_U24_MASK ) ||
         sequence_gap >= ( 1u << 23 ) )
        return false;

    rn_channel_receive_t * slot = channels->region->channel_receive + channel;
    slot->sequenced_watermark = ( sequencing + 1 ) & ROCKETNET_MESSAGE_U24_MASK;

    return true;
}

// The counter a reader or window violation (ROCKETNET_MESSAGE_VIOLATION_*,
// sdk/rocketnet_message.h) moves besides MESSAGE_PROTOCOL_VIOLATION: every
// check has its own counter, the same ones the dispatch moves, and a violation
// with no counter of its own (an unknown value) returns
// MESSAGE_PROTOCOL_VIOLATION, which rn_channels_count_violation counts once.
static_assert( ROCKETNET_MESSAGE_NUM_VIOLATIONS == 13, "a new message violation is mapped in rn_violation_counter" );

// Each violation is tied to the counter it moves; the function is constexpr so
// the tie is checked at compile time.

static constexpr int rn_violation_counter( int violation )
{
    return violation == ROCKETNET_MESSAGE_VIOLATION_BAD_FORM ? ROCKETNET_COUNTER_MESSAGE_BAD_FORM : violation == ROCKETNET_MESSAGE_VIOLATION_TRUNCATED           ? ROCKETNET_COUNTER_MESSAGE_TRUNCATED
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS             ? ROCKETNET_COUNTER_MESSAGE_BAD_FLAGS
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH            ? ROCKETNET_COUNTER_MESSAGE_BAD_LENGTH
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL           ? ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED      ? ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK             ? ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_BAD_RING              ? ROCKETNET_COUNTER_MESSAGE_BAD_RING
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE              ? ROCKETNET_COUNTER_MESSAGE_BAD_SIZE
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL           ? ROCKETNET_COUNTER_MESSAGE_BAD_CONTROL
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW ? ROCKETNET_COUNTER_MESSAGE_NUMBER_OUTSIDE_WINDOW
                                                                                                : violation == ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION        ? ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION
                                                                                                                                                                 : ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION;
}

static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_FORM ) == ROCKETNET_COUNTER_MESSAGE_BAD_FORM,
               "BAD_FORM moves MESSAGE_BAD_FORM" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_TRUNCATED ) == ROCKETNET_COUNTER_MESSAGE_TRUNCATED,
               "TRUNCATED moves MESSAGE_TRUNCATED" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS ) == ROCKETNET_COUNTER_MESSAGE_BAD_FLAGS,
               "BAD_FLAGS moves MESSAGE_BAD_FLAGS" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH ) == ROCKETNET_COUNTER_MESSAGE_BAD_LENGTH,
               "BAD_LENGTH moves MESSAGE_BAD_LENGTH" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL ) == ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL,
               "BAD_CHANNEL moves MESSAGE_BAD_CHANNEL" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED ) == ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED,
               "TYPE_NOT_ENABLED moves MESSAGE_TYPE_NOT_ENABLED" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK ) == ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK,
               "BAD_BLOCK moves MESSAGE_BAD_BLOCK" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_RING ) == ROCKETNET_COUNTER_MESSAGE_BAD_RING,
               "BAD_RING moves MESSAGE_BAD_RING" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE ) == ROCKETNET_COUNTER_MESSAGE_BAD_SIZE,
               "BAD_SIZE moves MESSAGE_BAD_SIZE" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL ) == ROCKETNET_COUNTER_MESSAGE_BAD_CONTROL,
               "BAD_CONTROL moves MESSAGE_BAD_CONTROL" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW ) == ROCKETNET_COUNTER_MESSAGE_NUMBER_OUTSIDE_WINDOW,
               "NUMBER_OUTSIDE_WINDOW moves MESSAGE_NUMBER_OUTSIDE_WINDOW" );
static_assert( rn_violation_counter( ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION ) == ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION,
               "SLOT_COLLISION moves MESSAGE_SLOT_COLLISION" );

void rn_channels_count_violation( struct rn_channels_t * channels, int counter )
{
    if ( !channels )
        return;

    channels->violations++;

    if ( counter != ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION )
        rn_channels_count( channels->region->counters, counter );

    // 17.3 item 11: a wire violation ends the connection. The owner reads
    // rn_ordered_decide and disconnects the other side of the connection; counted once per connection.

    if ( channels->error == ROCKETNET_DISCONNECT_REASON_NONE )
    {
        channels->error = ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION;
        rn_channels_count( channels->region->counters, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
    }
}

// tla/RNChannel.tla OpenPacket, OpenHead: dispatches each record of the packet the transport opened.
// tla/RNReceipts.tla OpenPacket: the far end dispatches that packet, marking it opened.

int rn_channels_dispatch( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out || record->bytes < 1 || !record->data )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    // The receipt variants travel as their base codes, so a wire code 5, 6 or
    // 7 is a violation and a receipt type is dispatched under its base
    // (7.2's last line).

    const int code = record->code;

    // 7.5: an application record whose wire code the mask does not admit is
    // refused here, once for every code (rocketnet_message_enabled_codes,
    // sdk/rocketnet_message.cpp:289, maps
    // a receipt type to the base code it travels as). A control record is
    // the layer's own and is never refused by the mask, as in the reader.

    if ( code >= 0 && code < ROCKETNET_MESSAGE_NUM_CODES && !record->control && ( channels->enabled_codes & ( 1u << code ) ) == 0 )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

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
            // A wire code outside 0..4: 5, 6 and 7 are receipt types, which travel as 0, 2 and 3, so their arrival is a violation (13.2)

            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }
}

// ---------------------------------------------------------------
//                  RELIABLE_ORDERED, the receive side
// ---------------------------------------------------------------

// tla/RNChannel.tla DeliverMessage

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
        // RNChannelTypes!Drain: a current-epoch sequenced hold is released before the ordered successor, then the next epoch is considered
        rn_channels_drain_sequenced( channels, channel );

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

// tla/RNChannel.tla DeliverMessage

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
        // the slot is live for another number (check 10, SLOT_COLLISION)
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( delta == 0 )
    {
        // A block still collecting chunks holds this ordering index: two messages with one ordering index, as on the held path below
        if ( rn_held_find( region, rn_held_key( record->channel, ordering ) ) != RN_DESCRIPTOR_NONE )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_DUPLICATE_ORDERING );
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
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_DUPLICATE_ORDERING );     // two messages with one ordering index
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

// A block's anchor (CHANNELS.md 8, "No start packet"): any chunk opens the
// block's state on its own. The first one seen is the anchor: its descriptor
// holds the count, the chunks in, and the block's first number, and the held
// index finds it by key. Every later chunk must agree with it on the count,
// the first number and the base, so the block's completion is one count and
// never a rescan of its descriptors. The chunk is copied to its own offset,
// ring + index * F (THE COPIES (b): a chunk must outlive the batch).
// RNBlocks!Receive; by arrival is MCRNBlocksBrokenPlaceByArrival.cfg.
// tla/RNChannelTypes.tla ArriveFragment: one block fragment arrives at its
// index and is placed, and the block is complete only when all have arrived.

rn_receive_descriptor_t * rn_channels_block_collect( rn_channels_t * channels, const struct rocketnet_message_record_t * record,
                                                     uint32_t key_channel, uint32_t key_index, uint32_t slot,
                                                     uint32_t first_number, int64_t base_units, uint32_t * out_held )
{
    rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = record->fragments + 1;
    const uint64_t key = rn_held_key( key_channel, key_index );
    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[slot];
    rn_receive_descriptor_t * anchor = descriptor;

    uint32_t held = rn_held_find( region, key );
    if ( held == RN_DESCRIPTOR_NONE )
    {
        if ( !rn_held_insert( region, key, slot, RN_HELD_COLLECTING ) )
        {
            rn_region_exhaust( region );
            return NULL;
        }

        held = rn_held_find( region, key );
        descriptor->block_count = (uint16_t) count;
        descriptor->block_have = 0;
        descriptor->sequencing = first_number;
    }
    else
    {
        anchor = &region->receive_descriptors[region->held_index[held].number_slot];

        // the block's count, first number and base must agree with the first chunk seen, or this chunk belongs to something else

        const uint32_t anchor_index = rn_u24( (uint32_t) anchor->number - first_number );
        if ( region->held_index[held].used != RN_HELD_COLLECTING || !anchor->live ||
             anchor->block_count != count || anchor->sequencing != first_number || anchor_index >= count ||
             (int64_t) anchor->ring - (int64_t) anchor_index * (int64_t) ( F / 16 ) != base_units )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
            return NULL;
        }
    }

    memcpy( region->receive_ring + (uint64_t) record->ring * 16, record->data, (size_t) record->bytes );

    anchor->block_have++;
    *out_held = held;

    return anchor;
}

// Every chunk is in: the first number's descriptor takes the whole block (the
// update's end releases every number of it from there) and the anchor gives
// it up. A block that waits for its turn stays in the held index, complete; one
// delivered now leaves it.

void rn_channels_block_settle( rn_channels_t * channels, uint32_t held, rn_receive_descriptor_t * anchor,
                               uint32_t first_slot, uint32_t count, uint32_t total, uint32_t base_units, bool keep )
{
    rn_region_t * region = channels->region;
    rn_receive_descriptor_t * first = &region->receive_descriptors[first_slot];
    if ( anchor != first )
        anchor->block_count = 0;

    first->block_count = (uint16_t) count;                      // released together at the update's end
    first->block_have = (uint16_t) count;
    first->bytes = total;
    first->ring = base_units;

    if ( keep )
    {
        region->held_index[held].number_slot = first_slot;
        region->held_index[held].used = RN_HELD_COMPLETE;
    }
    else
    {
        rn_held_erase( region, held );
    }
}

// A chunk of a large message (CHANNELS.md 8): placed by index, counted, and the
// whole message dispatched as one only when every chunk is in. tla/RNBlocks.tla:
// RNBlocks!Receive (the anchor by b = n - index, the copy to ring + index * F)
// and RNBlocks!Deliver (complete at its turn: one view, one ordering slot).

// tla/RNChannel.tla DeliverMessage

static int rn_channels_dispatch_chunk( rn_channels_t * channels, const struct rocketnet_message_record_t * record, rn_delivery_t * out, uint32_t delta )
{
    rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t window = region->window;
    const uint32_t count = record->fragments + 1;
    const uint32_t index = record->fragment;

    // a count at least two, inside W and max_message_bytes, an index inside it, and every fragment but the last exactly F (checks 7 and 8, the shape half)

    int64_t base_units = 0;
    if ( !rn_block_shape_valid( channels, record, &base_units ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    // the fragment's own ring span lies inside the ring (code 2's check, unified)

    if ( !rn_region_ring_span_valid( region, (uint64_t) record->ring * 16,
                                     rn_round16( (uint64_t) record->bytes ) ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    // the block's span: its base is this chunk's ring offset less index * F, and
    // it ends inside the ring (check 8). A chunk that is not the last knows the
    // end only to within the last chunk's one byte.

    const bool last = index + 1 == count;
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
        // the slot is live for another number (check 10, SLOT_COLLISION)
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t first_number = rn_u24( record->number - index );
    const uint32_t first_slot = first_number & ( window - 1 );

    // placed by index, not by arrival, and counted in the block's anchor, found by (channel, ordering)

    uint32_t held = RN_DESCRIPTOR_NONE;
    rn_receive_descriptor_t * anchor = rn_channels_block_collect( channels, record, record->channel, record->ordering, slot,
                                                                  first_number, base_units, &held );
    if ( !anchor )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    if ( anchor->block_have < count )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;                 // never a partial delivery

    // every chunk is in. The block is one contiguous message in the ring.

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

    rn_channels_block_settle( channels, held, anchor, first_slot, count, total, (uint32_t) base_units, true );

    if ( delta != 0 )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;                 // complete, waiting for its turn

    // Its turn: delivered whole, in place, a view of the ring (no reassembly
    // buffer, no second copy), then the coupling and the drain.
    // RNBlocks!Deliver (DeliveredEqualsSent, NoPartialDelivery,
    // OneOrderingSlotPerBlock).

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

// tla/RNChannel.tla ReceiveAck, ReceiveAckAny: feeds the acked packet ids the transport delivered into the window

uint32_t rn_channels_feed_acks( struct rn_window_ack_t * ack, const uint64_t * packet_ids, uint32_t count, double now )
{
    // RNChannel!ReceiveAck, R1's one path: the transport's packet ids into layer 3's sent map, keyed by the id the transport assigned

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

        // descriptors hold the 24-bit wire number, so compare every fragment across the 2^24 wrap the same way

        if ( descriptor->live && rn_u24( (uint32_t) descriptor->number ) == rn_u24( (uint32_t) ( number + i ) ) )
        {
            descriptor->live = 0;
            descriptor->block_count = 0;
            descriptor->block_have = 0;
        }
    }
}

// tla/RNChannel.tla EndUpdate

void rn_channels_end_update( struct rn_channels_t * channels )
{
    if ( !channels )
        return;

    // RNChannel!EndUpdate: the batch's views are gone and the update's deliveries expire, so their numbers stop being live, and the per-update counts start over

    for ( uint32_t i = 0; i < channels->deliveries; i++ )
    {
        const uint32_t slot = channels->region->deliveries[i].descriptor;
        if ( slot != RN_DESCRIPTOR_NONE )
            rn_channels_release( channels, slot );
    }

    rn_channels_end_update_sequenced( channels );

    channels->unreliable_delivered = 0;
    channels->deliveries = 0;
    channels->violations = 0;
}

// ---------------------------------------------------------------
//                 the send API (RNChannel!Enqueue)
// ---------------------------------------------------------------

// tla/RNChannelQueues.tla Enqueue, Front, Cancel, StaleCancel, Compact,
// ReadyJoin, Rotate and Reset. Ready membership is one ticket per channel. A push
// of a channel that already holds a ticket is a no-op, so a channel cannot
// sit on the list twice. Removing a ticket that is not the current one leaves
// the list alone.

static uint32_t rn_channel_live_count( const rn_channel_send_t * send )
{
    return (uint32_t) send->queued[0] + send->queued[1] + send->queued[2] + send->queued[3];
}

// The channel FIFO is the flat send_descriptors array ordered by
// enqueue_ticket (tla/RNChannelQueues.tla's sequence): the head is the live
// reliable descriptor of this channel with the smallest ticket, and the next
// entry is the smallest ticket after a given one. No descriptor points at
// another descriptor.

static bool rn_channel_queue_member( const rn_send_descriptor_t * descriptor, uint32_t channel )
{
    return descriptor->in_use && descriptor->channel == channel &&
           descriptor->reliability >= ROCKETNET_CHANNEL_TYPE_RELIABLE &&
           descriptor->reliability <= ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED &&
           rn_descriptor_schedule_live( descriptor );
}

static uint32_t rn_channel_queue_next_live( const rn_region_t * region, uint32_t channel, uint32_t after_ticket )
{
    uint32_t best = RN_DESCRIPTOR_NONE;
    uint32_t best_ticket = 0;

    for ( uint32_t d = 0; d < region->descriptors_fresh; d++ )
    {
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        if ( !rn_channel_queue_member( descriptor, channel ) || !rn_descriptor_schedule_live( descriptor ) )
            continue;
        if ( descriptor->enqueue_ticket <= after_ticket )
            continue;

        if ( best == RN_DESCRIPTOR_NONE || descriptor->enqueue_ticket < best_ticket )
        {
            best = d;
            best_ticket = descriptor->enqueue_ticket;
        }
    }

    return best;
}

static void rn_channel_queue_sync( rn_region_t * region, uint32_t channel )
{
    rn_channel_send_t * send = region->channel_send + channel;
    send->queue_head = rn_channel_queue_next_live( region, channel, 0 );

    uint32_t tail = RN_DESCRIPTOR_NONE;
    uint32_t tail_ticket = 0;

    for ( uint32_t d = 0; d < region->descriptors_fresh; d++ )
    {
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        if ( !rn_channel_queue_member( descriptor, channel ) || !rn_descriptor_schedule_live( descriptor ) )
            continue;

        if ( tail == RN_DESCRIPTOR_NONE || descriptor->enqueue_ticket > tail_ticket )
        {
            tail = d;
            tail_ticket = descriptor->enqueue_ticket;
        }
    }

    send->queue_tail = tail;
}

static uint32_t rn_channel_head_generation( const rn_channels_t * channels, uint32_t channel )
{
    const rn_region_t * region = channels->region;
    const uint32_t d = rn_channel_queue_next_live( region, channel, 0 );
    return d == RN_DESCRIPTOR_NONE ? 0 : region->send_descriptors[d].generation;
}

// The ready list is the flat channel_send array ordered by ready_ticket:
// the next ready channel is the smallest ready ticket after a given one.

static uint32_t rn_ready_next_after( const rn_channels_t * channels, uint32_t after_ticket )
{
    const rn_channel_send_t * send = channels->region->channel_send;
    uint32_t best = RN_DESCRIPTOR_NONE;
    uint32_t best_ticket = 0;

    for ( uint32_t channel = 0; channel < channels->num_channels; channel++ )
    {
        if ( send[channel].ready_ticket == 0 || send[channel].ready_ticket <= after_ticket )
            continue;

        if ( best == RN_DESCRIPTOR_NONE || send[channel].ready_ticket < best_ticket )
        {
            best = channel;
            best_ticket = send[channel].ready_ticket;
        }
    }

    return best;
}

static void rn_ready_sync( rn_channels_t * channels )
{
    rn_channel_send_t * send = channels->region->channel_send;

    channels->ready_head = rn_ready_next_after( channels, 0 );

    uint32_t tail = RN_DESCRIPTOR_NONE;
    uint32_t tail_ticket = 0;

    for ( uint32_t channel = 0; channel < channels->num_channels; channel++ )
    {
        if ( send[channel].ready_ticket != 0 && ( tail == RN_DESCRIPTOR_NONE || send[channel].ready_ticket > tail_ticket ) )
        {
            tail = channel;
            tail_ticket = send[channel].ready_ticket;
        }
    }

    channels->ready_tail = tail;
}

static void rn_ready_rebase_tickets( rn_channels_t * channels )
{
    rn_channel_send_t * send = channels->region->channel_send;
    uint32_t ticket = 1;
    uint32_t after = 0;
    uint32_t channel;
    while ( ( channel = rn_ready_next_after( channels, after ) ) != RN_DESCRIPTOR_NONE )
    {
        after = send[channel].ready_ticket;
        send[channel].ready_ticket = ticket++;
        channels->schedule_work++;

        if ( ticket == 0 )
            ticket = 1;
    }

    channels->ready_ticket_next = ticket == 0 ? 1 : ticket;
    rn_ready_sync( channels );
}

static void rn_ready_link( rn_channels_t * channels, uint32_t channel, bool front )
{
    rn_channel_send_t * send = channels->region->channel_send;
    if ( send[channel].ready_ticket != 0 )
        return;

    if ( front )
    {
        // the front of the flat ready sequence takes ticket 1; every channel already ready keeps its order behind it at tickets 2..n+1
        uint32_t order[ROCKETNET_MAX_CHANNELS];
        uint32_t count = 0;
        uint32_t after = 0;
        uint32_t ready_channel;
        while ( ( ready_channel = rn_ready_next_after( channels, after ) ) != RN_DESCRIPTOR_NONE && count < ROCKETNET_MAX_CHANNELS )
        {
            after = send[ready_channel].ready_ticket;
            order[count++] = ready_channel;
        }

        for ( uint32_t i = 0; i < count; i++ )
            send[order[i]].ready_ticket = i + 2;

        channels->ready_ticket_next = count + 2;
        send[channel].ready_ticket = 1;
    }
    else
    {
        if ( channels->ready_ticket_next == 0 )
            rn_ready_rebase_tickets( channels );

        const uint32_t ticket = channels->ready_ticket_next++;

        if ( channels->ready_ticket_next == 0 )
            channels->ready_ticket_next = 1;

        send[channel].ready_ticket = ticket;
    }

    send[channel].ready_generation = rn_channel_head_generation( channels, channel );

    rn_ready_sync( channels );
}

// tla/RNChannel.tla Enqueue

static void rn_ready_push( rn_channels_t * channels, uint32_t channel )
{
    rn_ready_link( channels, channel, false );
}

static bool rn_ready_remove_named( rn_channels_t * channels, uint32_t channel, uint32_t ticket, uint32_t generation )
{
    rn_channel_send_t * send = channels->region->channel_send;
    if ( ticket == 0 || send[channel].ready_ticket != ticket || send[channel].ready_generation != generation )
        return false;

    send[channel].ready_ticket = 0;
    send[channel].ready_generation = 0;

    rn_ready_sync( channels );

    return true;
}

// tla/RNEndpoint.tla Reset: CONFIG's channel takes the first turn (10.3)

static void rn_ready_push_front( rn_channels_t * channels, uint32_t channel )
{
    rn_ready_link( channels, channel, true );
}

// Rebuild one channel's flat FIFO. Live descriptors keep
// their ticket order and receive a fresh increasing ticket range, so a CONFIG
// spliced at the head holds the smallest ticket. One pass over this channel's
// entries, never per emitted record. Restamping parks each live descriptor
// under a sentinel channel while it is processed, so a fresh ticket (issued
// ahead of every live one) is never selected again in the same pass.

static void rn_channel_compact_one( rn_channels_t * channels, uint32_t channel )
{
    rn_region_t * region = channels->region;
    rn_channel_send_t * send = region->channel_send + channel;
    const uint16_t parked_channel = 0xFFFFu;
    rocketnet_assert( channel < parked_channel );

    for ( uint32_t d = 0; d < region->descriptors_fresh; d++ )
    {
        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        if ( !rn_channel_queue_member( descriptor, channel ) )
            continue;

        channels->schedule_work++;
    }

    uint32_t after = 0;
    uint32_t d;
    while ( ( d = rn_channel_queue_next_live( region, channel, after ) ) != RN_DESCRIPTOR_NONE )
    {
        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        after = descriptor->enqueue_ticket;
        descriptor->channel = parked_channel;
        rn_descriptor_enqueue_stamp( region, descriptor );
    }

    for ( uint32_t parked = 0; parked < region->descriptors_fresh; parked++ )
    {
        if ( region->send_descriptors[parked].channel == parked_channel )
            region->send_descriptors[parked].channel = (uint16_t) channel;
    }

    rn_channel_queue_sync( region, channel );

    if ( rn_channel_live_count( send ) == 0 && ( send->ready_ticket != 0 || channels->ready_head == channel ) )
    {
        if ( send->ready_ticket == 0 )
        {
            send->ready_ticket = 1;
            send->ready_generation = 0;
        }

        rn_ready_remove_named( channels, channel, send->ready_ticket, send->ready_generation );
    }
}

// tla/RNChannel.tla RefuseSend

uint8_t * rn_channels_start( struct rn_channels_t * channels, uint32_t channel, int type, uint32_t bytes )
{
    if ( !channels || channels->pending != RN_DESCRIPTOR_NONE )
        return NULL;                                            // one start at a time, like the transport's start_packet

    rn_region_t * region = channels->region;

    const int receipt_base = rn_channels_reliable_receipt_base( type );

    // RNChannel!RefuseSend: a type outside the mask is refused and counted (7.5)

    if ( rn_channels_send_check( channels, type, channel ) != ROCKETNET_CHANNEL_SEND_OK ||
         ( type != ROCKETNET_CHANNEL_TYPE_RELIABLE && type != ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED &&
           type != ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED && receipt_base < 0 ) )
        return NULL;

    // an exhausted connection sends nothing more: the owner kicks it (R5)

    if ( rn_region_exhausted( region ) || bytes < 1 || bytes > region->max_message_bytes )
        return NULL;

    // the room is the region's, one descriptor and its bytes; no room exhausts it

    const uint32_t d = rn_region_send_take( region, (int) bytes );
    if ( d == RN_DESCRIPTOR_NONE )
        return NULL;

    channels->pending = d;
    channels->pending_bytes = bytes;
    channels->pending_channel = channel;

    // RNReceipts EffectiveKind: public 6/7 use the existing reliable code
    // 2/3 numbering and resend path; the flag requests one ACKED outcome.

    region->send_descriptors[d].reliability = (uint8_t) ( receipt_base >= 0 ? receipt_base : type );

    if ( receipt_base >= 0 )
        region->send_descriptors[d].flags |= RN_SEND_FLAG_RECEIPT_RELIABLE;

    return rn_region_send_write_pointer( region, d );
}

// tla/RNChannel.tla Enqueue

bool rn_channels_finish( struct rn_channels_t * channels, uint32_t bytes )
{
    if ( !channels || channels->pending == RN_DESCRIPTOR_NONE )
        return false;

    rn_region_t * region = channels->region;
    const uint32_t d = channels->pending;
    const uint32_t channel = channels->pending_channel;

    if ( bytes < 1 || bytes > channels->pending_bytes )
        return false;                                           // the start stays pending

    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    const uint8_t type = descriptor->reliability;
    if ( type != ROCKETNET_CHANNEL_TYPE_RELIABLE && type != ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED &&
         type != ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED )
        return false;

    channels->pending = RN_DESCRIPTOR_NONE;

    // tla/RNChannelTypes.tla Enqueue: assigns the ordering and sequencing counters as the message joins the channel queue.
    // Code 2 has no ordering index. Code 3 keeps its
    // coupled per-channel counter transition. A record fitting one packet
    // takes one number; a larger block takes one number per chunk
    // (RNBlocks!NumberMessage, Count: k = ceil(bytes / F)).

    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;

    descriptor->bytes = bytes;

    // A code-4 record's header is 15 bytes, 16 with the WIDE channel field
    // on a channel of 256 or more (rocketnet_message_flags_header_bytes,
    // sdk/rocketnet_message.cpp:52); the form byte follows it.

    struct rocketnet_message_record_t sequenced_shape;
    memset( &sequenced_shape, 0, sizeof( sequenced_shape ) );
    sequenced_shape.code = ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
    sequenced_shape.channel = channel;

    const uint32_t small_limit = type == ROCKETNET_CHANNEL_TYPE_RELIABLE ? channels->payload_capacity - 7 : type == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED ? channels->payload_capacity - 1 - (uint32_t) rocketnet_message_record_header_bytes( &sequenced_shape )
                                                                                                                                                              : rn_channels_small_limit( channels );
    descriptor->fragments = (uint16_t) ( bytes <= small_limit ? 1 : ( bytes + F - 1 ) / F );
    descriptor->channel = (uint16_t) channel;
    descriptor->priority = ROCKETNET_PRIORITY_MEDIUM;

    if ( type == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED )
        descriptor->ordering = rn_channels_send_ordered( channels, channel );
    else if ( type == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED )
        descriptor->sequencing = rn_channels_send_sequenced( channels, channel, &descriptor->ordering );
    else
        descriptor->ordering = 0;

    rn_descriptor_enqueue_stamp( region, descriptor );

    // the channel's flat FIFO: first writes are in ordering order (9.2).
    // The ticket is distinct from the reliable number, which is assigned later.

    rn_channel_send_t * send = region->channel_send + channel;
    const bool was_empty = rn_channel_live_count( send ) == 0;
    send->queued[descriptor->priority]++;

    rn_channel_queue_sync( region, channel );

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
//        CONFIG, the one announcement (CHANNELS.md 10.3, 13.4)
// ---------------------------------------------------------------

// tla/RNEndpoint.tla Reset: relQ = <<CONFIG>>, so CONFIG is the first record
// numbered (ConfigReliable: it takes the first reliable number and is resent
// until acked), and WriteOk: the first packet carries it, from the head of
// IMMEDIATE.

bool rn_ordered_announce_config( struct rn_ordered_t * ordered, const struct rocketnet_message_config_t * config )
{
    if ( !ordered || !ordered->channels.region || ordered->config_announced )
        return false;

    rocketnet_message_config_t defaults;

    if ( !config )
    {
        rocketnet_message_default_config( &defaults );

        config = &defaults;
    }

    rn_channels_t * channels = &ordered->channels;
    rn_region_t * region = channels->region;

    // the body of 13.4: the wire-relevant configuration and the enabled mask

    struct rocketnet_message_config_body_t * body = &ordered->config;
    memset( body, 0, sizeof( *body ) );
    body->version = ROCKETNET_MESSAGE_CONFIG_VERSION;
    body->enabled = channels->enabled_types & 0xFFu;
    body->num_channels = (uint32_t) config->num_channels;
    body->reliable_messages_in_flight = (uint32_t) config->reliable_messages_in_flight;
    body->receive_message_buffer_bytes = (uint32_t) config->receive_message_buffer_bytes;
    body->max_message_bytes = (uint32_t) config->max_message_bytes;
    body->fragment_bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;

    if ( rn_region_exhausted( region ) )
        return false;

    const uint32_t d = rn_region_send_alloc( region, ROCKETNET_MESSAGE_CONFIG_BYTES );
    if ( d == RN_DESCRIPTOR_NONE )
        return false;                                           // no room: the region is exhausted, the owner kicks (R5)

    rocketnet_message_write_config_body( rn_region_send_write_pointer( region, d ), body );

    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    descriptor->reliability = ROCKETNET_CHANNEL_TYPE_RELIABLE;  // code 2, CONTROL set at write (rn_ordered_item)
    descriptor->flags |= RN_SEND_FLAG_CONTROL;
    descriptor->bytes = ROCKETNET_MESSAGE_CONFIG_BYTES;
    descriptor->fragments = 1;
    descriptor->channel = 0;
    descriptor->priority = ROCKETNET_PRIORITY_IMMEDIATE;
    descriptor->ordering = 0;

    // the head of channel 0's FIFO, and channel 0 at the head of the turns.
    // Rebase this channel only, once: CONFIG receives the smallest ticket
    // and the messages already queued keep their order behind it.

    rn_channel_send_t * send = region->channel_send;

    descriptor->schedule_flags = (uint16_t) RN_SCHEDULE_QUEUED;
    descriptor->enqueue_ticket = 1;
    descriptor->queue_epoch = region->schedule_epoch;
    send->queued[descriptor->priority]++;

    rn_channel_compact_one( channels, 0 );

    if ( send->ready_ticket != 0 )
        rn_ready_remove_named( channels, 0, send->ready_ticket, send->ready_generation );

    rn_ready_push_front( channels, 0 );

    ordered->config_announced = 1;

    return true;
}

// tla/RNEndpoint.tla OnPayload, the CONFIG half, with ConfigCompare: every
// field of 13.4 compared; the first that differs ends the connection.

int rn_ordered_take_config( struct rn_ordered_t * ordered, const struct rocketnet_message_record_t * record )
{
    if ( !ordered || !record )
        return ROCKETNET_DISCONNECT_REASON_NONE;

    rn_channels_t * channels = &ordered->channels;

    struct rocketnet_message_config_body_t remote;
    memset( &remote, 0, sizeof( remote ) );

    const int body_violation = rocketnet_message_read_config_body( record->data, record->bytes, &remote );
    if ( body_violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        rn_channels_count_violation( channels, rn_violation_counter( body_violation ) );
        return channels->error;
    }

    const struct rocketnet_message_config_body_t * own = &ordered->config;
    struct field_t
    {
        const char * name;
        uint32_t own, remote;
    };
    const field_t fields[] = {
        { "version", own->version, remote.version },
        { "enabled", own->enabled, remote.enabled },
        { "num_channels", own->num_channels, remote.num_channels },
        { "reliable_messages_in_flight", own->reliable_messages_in_flight, remote.reliable_messages_in_flight },
        { "receive_message_buffer_bytes", own->receive_message_buffer_bytes, remote.receive_message_buffer_bytes },
        { "max_message_bytes", own->max_message_bytes, remote.max_message_bytes },
        { "fragment_bytes", own->fragment_bytes, remote.fragment_bytes },
    };
    for ( size_t i = 0; i < sizeof( fields ) / sizeof( fields[0] ); i++ )
    {
        if ( fields[i].own == fields[i].remote )
            continue;

        if ( channels->error == ROCKETNET_DISCONNECT_REASON_NONE )
        {
            rocketnet_error( "message CONFIG mismatch: %s is %u here and %u at the remote endpoint", fields[i].name, fields[i].own, fields[i].remote );

            channels->error = ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH;
        }

        return channels->error;
    }

    ordered->config_received = 1;

    return ROCKETNET_DISCONNECT_REASON_NONE;
}

// ---------------------------------------------------------------
//        the ordered path of one connection, ends to end
// ---------------------------------------------------------------

bool rn_ordered_init( struct rn_ordered_t * ordered, struct rn_region_t * region, const struct rocketnet_message_config_t * config,
                      struct rocketnet_send_budget_t * budget, int packets_per_update, int payload_capacity )
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

    if ( !rn_packet_builder_init( &ordered->builder, region, &ordered->ack, budget, packets_per_update, payload_capacity ) )
    {
        memset( ordered, 0, sizeof( *ordered ) );
        return false;
    }

    ordered->budget = budget;
    ordered->limits.num_channels = (int) region->num_channels;
    ordered->limits.enabled_codes = ordered->channels.enabled_codes;
    ordered->limits.reliable_messages_in_flight = (int) region->window;
    ordered->limits.max_message_bytes = (int) region->max_message_bytes;
    ordered->limits.receive_message_buffer_bytes = (int) region->ring_bytes;

    return true;
}

// One number as the builder's item: the bytes at the start of its region
// record (THE COPIES (a): the frame is written from there), with the fields its
// record carries. The ring offset is the receive ring offset the window gave it.

// tla/RNChannel.tla WritePacket, WriteAny: turns one unacked number into the builder's item for a packet write.
// tla/RNChannelTypes.tla WriteAny: the same packet write, any message and either attempt.
// tla/RNBlocks.tla SendRecord: a fragment's item carries its index, k - 1 and its own ring offset

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
    item->sequencing = rn_u24( descriptor->sequencing );
    item->descriptor = slot->descriptor;
    item->channel = descriptor->channel;
    item->code = descriptor->reliability;        // RNChannelTypes!WritePacket keeps code 2 on every resend
    item->control = ( descriptor->flags & RN_SEND_FLAG_CONTROL ) ? 1 : 0;   // RNEndpoint WriteOk: CONFIG keeps CONTROL on every resend
    item->send_class = (uint8_t) send_class;
    item->priority = descriptor->priority < ROCKETNET_PACKET_BUILDER_LEVELS ? descriptor->priority : ROCKETNET_PRIORITY_MEDIUM;
    item->ring = (uint32_t) ( ( slot->ring_start % region->ring_bytes ) / 16 );

    if ( descriptor->fragments > 1 )
    {
        // RNBlocks!SendRecord: the fragment carries its index, k - 1 and its own ring offset, p + index * F
        item->block = 1;                                        // one chunk, in its own packet (8)
        item->alone = 1;
        item->fragment = slot->fragment;
        item->fragments = (uint32_t) descriptor->fragments - 1;
    }

    return true;
}

// The update's items as rn_ordered_write plans them: how many, the bytes
// planned and the room the update's packets have.

struct rn_ordered_plan_t
{
    rn_packet_item_t * items;
    int count;
    uint64_t planned;
    uint64_t room;
    uint32_t capacity;
};

// One resend FIFO's walk for rn_ordered_write, oldest first
// (RNWindow!WriteOldest): a number never sent is due now (a new message's, a
// chunk's, a packet the transport refused); a sent one is due when its class's
// delay has passed since its last send, the same delay every time
// (tla/RNResendTiming.tla Walk, no back-off). The walk stops at the first number not
// yet due. Returns false when more is due than this update can write. On
// return *last_unsent is the last never-sent number at the FIFO's head (where
// new numbers of this class join), or RN_RESEND_NONE.

static bool rn_ordered_walk( rn_ordered_t * ordered, bool chunk, double now, rn_ordered_plan_t * plan, uint32_t * last_unsent )
{
    rn_region_t * region = ordered->channels.region;
    rn_window_ack_t * ack = &ordered->ack;
    const double delay = rn_window_ack_class_delay( ack, chunk );
    uint32_t cursor = rn_window_ack_resend_first( ack, chunk );

    *last_unsent = RN_RESEND_NONE;

    while ( cursor != RN_RESEND_NONE )
    {
        const rn_send_number_t * slot = &region->send_numbers[cursor];
        const uint32_t next_slot = rn_window_ack_resend_next( ack, chunk, cursor );

        ack->walk_visits++;

        if ( slot->acked )
        {
            cursor = next_slot;
            continue;
        }

        const bool unsent = rn_send_number_unsent_due( slot->last_send );
        if ( !unsent && !( now >= slot->last_send + delay ) )
            return true;

        if ( plan->count >= ROCKETNET_ORDERED_MAX_ITEMS || plan->planned >= plan->room )
            return false;                                       // more is due than this update can write

        if ( unsent )
            *last_unsent = cursor;

        rn_packet_item_t * item = &plan->items[plan->count];
        if ( rn_ordered_item( ordered, slot, unsent ? ROCKETNET_SEND_CLASS_ORDERED_PROGRESS : ROCKETNET_SEND_CLASS_RETRANSMIT, item ) )
        {
            item->resend = unsent ? 0 : 1;
            plan->planned += item->alone ? ( plan->capacity - 1 ) : (uint64_t) item->bytes + ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES;
            plan->count++;
        }

        cursor = next_slot;
    }

    return true;
}

// tla/RNChannel.tla NumberRecord: numbers the channel's head message late, only while n < U' + W
// tla/RNBlocks.tla NumberMessage: gives a block k consecutive numbers and one contiguous span through rn_window_send_first_write_block

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

    // an exhausted or ended connection does nothing but be kicked (R5), and a reliable message past its timeout ends it (6.4 "Giving up")

    if ( rn_region_exhausted( region ) || channels->error != ROCKETNET_DISCONNECT_REASON_NONE || rn_window_ack_check_timeout( ack, now ) )
        return 0;

    if ( ordered->budget )
        rocketnet_send_budget_begin_update( ordered->budget, now );

    const int packet_limit = builder->packets_per_update < num_payloads ? builder->packets_per_update : num_payloads;
    const uint32_t capacity = channels->payload_capacity;
    const uint64_t room = (uint64_t) packet_limit * ( capacity - 1 );

    rn_packet_item_t items[ROCKETNET_ORDERED_MAX_ITEMS];
    rn_ordered_plan_t plan;
    plan.items = items;
    plan.count = 0;
    plan.planned = 0;
    plan.room = room;
    plan.capacity = capacity;

    // 1. The two resend FIFOs (6.4 "Resend"): single-packet records at
    // min( resend_delay, resend_interval ), then the chunks of blocks at the resend delay alone
    // (rn_window_ack_chunk_resend_delay), retransmits first (R6). Each walk
    // stops at its own first number not yet due, so a chunk that is not due
    // never holds back a small record that is (RNResendTiming!
    // SmallNeverHeldByChunk) and the work is O(due).

    ack->walk_visits = 0;

    uint32_t last_unsent_small = RN_RESEND_NONE;
    uint32_t last_unsent_chunk = RN_RESEND_NONE;
    const bool walked = rn_ordered_walk( ordered, false, now, &plan, &last_unsent_small ) &&
                        rn_ordered_walk( ordered, true, now, &plan, &last_unsent_chunk );

    // 2. New messages, numbered late (6.2) and whole (8), channels taking turns
    // one message each (17.3 item 4), while the update has room. The channel's
    // FIFO head is numbered first, so first writes are in ordering order (9.2).
    // A message the window cannot yet admit stops numbering until acks move U'
    // (RNWindow!NumberRecord, RNRing!PlaceSingle and PlaceBlock); a stall never
    // blocks what is already numbered, or the unreliable records.

    while ( walked && plan.planned < room && plan.count < ROCKETNET_ORDERED_MAX_ITEMS && channels->ready_head != RN_DESCRIPTOR_NONE )
    {
        const uint32_t channel = channels->ready_head;
        rn_channel_send_t * send = region->channel_send + channel;
        if ( send->queue_head == RN_DESCRIPTOR_NONE )
        {
            rn_channel_compact_one( channels, channel );
            continue;
        }

        const uint32_t d = send->queue_head;
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];

        uint64_t first = 0;
        uint64_t ring_start = 0;
        uint32_t numbers = 0;

        // RNChannelTypes!WritePacket: code 2 uses the same late, stable
        // reliable number and transport-ACK resend table as code 3.
        // RNBlocks!NumberMessage: a block's k consecutive numbers and one
        // contiguous span, fragment i at p + i * F, admitted once.

        if ( descriptor->fragments > 1 )
            numbers = rn_window_send_first_write_block( &ordered->send, descriptor->reliability, descriptor->bytes, d, &first, &ring_start );
        else if ( rn_window_send_first_write( &ordered->send, descriptor->reliability, descriptor->bytes, d, &first, &ring_start ) == RN_WINDOW_SEND_NUMBERED )
            numbers = 1;

        if ( numbers == 0 )
            break;                                              // stalled: the window counted it

        // Rotate off the ready list before the dequeue, while the membership
        // still names the generation captured at join (tla/RNChannelQueues.tla Rotate).

        const uint32_t ready_ticket = send->ready_ticket;
        const uint32_t ready_generation = send->ready_generation;
        if ( !rn_ready_remove_named( channels, channel, ready_ticket, ready_generation ) && send->ready_ticket != 0 )
            rn_ready_remove_named( channels, channel, send->ready_ticket, send->ready_generation );

        // dequeue: the descriptor leaves the flat FIFO by clearing its ticket

        if ( region->send_descriptors[d].priority < 4 && send->queued[region->send_descriptors[d].priority] > 0 )
            send->queued[region->send_descriptors[d].priority]--;

        region->send_descriptors[d].enqueue_ticket = 0;
        region->send_descriptors[d].schedule_flags = 0;
        rn_channel_queue_sync( region, channel );

        // channels take turns: this one rejoins the back if it has more

        if ( send->queue_head != RN_DESCRIPTOR_NONE )
            rn_ready_push( channels, channel );

        // every number of the message joins the resend set, due now; the ones
        // this update has room for are also planned into packets now
        // (in its class's FIFO: a block's numbers are chunks)

        uint32_t * last_unsent = rn_window_ack_slot_is_chunk( ack, (uint32_t) ( first & ( region->window - 1 ) ) ) ? &last_unsent_chunk : &last_unsent_small;

        for ( uint32_t i = 0; i < numbers; i++ )
        {
            const uint32_t slot_index = (uint32_t) ( ( first + i ) & ( region->window - 1 ) );
            rn_window_ack_park( ack, *last_unsent, slot_index );

            *last_unsent = slot_index;

            if ( plan.count < ROCKETNET_ORDERED_MAX_ITEMS && plan.planned < room &&
                 rn_ordered_item( ordered, &region->send_numbers[slot_index], ROCKETNET_SEND_CLASS_NEW_RELIABLE, &items[plan.count] ) )
            {
                plan.planned += items[plan.count].alone ? ( capacity - 1 ) : (uint64_t) items[plan.count].bytes + ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES;
                plan.count++;
            }
        }
    }

    if ( plan.count == 0 )
        return 0;

    // 3. Packets (9.3), under the send budget (R6)
    // 4. A resend is counted when the packet carrying it is sent, at
    // rn_packet_builder_commit, never here: a refused packet's resends are
    // counted by the packet that carries them out. The delay never backs off
    // (6.4, RNResendTiming).

    return rn_packet_builder_write( builder, items, plan.count, payloads, num_payloads );
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

// Reserve all deliveries this record can release before accepting its number.
// A partial packet may already have delivered an earlier record; its retry
// deduplicates that prefix while the refused record remains unmarked.

static uint32_t rn_receive_delivery_reservation( const rn_channels_t * channels, const rocketnet_message_record_t * record )
{
    if ( record->control )
        return 0;

    uint32_t needed = 1;

    if ( record->code != ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED || record->channel >= channels->num_channels )
        return needed;

    const rn_region_t * region = channels->region;
    const rn_channel_receive_t * rx = &region->channel_receive[record->channel];
    if ( rn_u24( record->ordering ) != rx->ordered_read )
        return needed;

    // at most one sequenced hold can be released by this channel's drain

    if ( rx->held_sequenced != RN_DESCRIPTOR_NONE )
        needed++;

    uint32_t ordering = rn_u24( record->ordering + 1 );
    for ( uint32_t i = 0; i < region->window; ++i, ordering = rn_u24( ordering + 1 ) )
    {
        const uint32_t held = rn_held_find( region, rn_held_key( record->channel, ordering ) );
        if ( held == RN_DESCRIPTOR_NONE || region->held_index[held].used != RN_HELD_COMPLETE )
            break;

        needed++;
    }

    return needed;
}

int rn_ordered_receive( struct rn_ordered_t * ordered, const uint8_t * payload, int bytes )
{
    if ( !ordered || !payload )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_FORM;

    rn_channels_t * channels = &ordered->channels;

    // an ended or exhausted connection takes nothing more

    if ( channels->error != ROCKETNET_DISCONNECT_REASON_NONE || rn_region_exhausted( channels->region ) )
        return ROCKETNET_MESSAGE_VIOLATION_NONE;

    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &ordered->limits );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        rn_channels_count_violation( channels, rn_violation_counter( violation ) );
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
            const uint32_t needed = rn_receive_delivery_reservation( channels, &record );
            if ( channels->deliveries > channels->delivery_capacity || needed > channels->delivery_capacity - channels->deliveries )
                return ROCKETNET_CHANNEL_RECEIVE_DEFERRED;

            uint32_t slot = RN_DESCRIPTOR_NONE;
            const int accepted = rn_window_receive_accept( &ordered->receive, &record, &slot );
            if ( accepted == RN_WINDOW_RECEIVE_DUPLICATE )
                continue;                                       // counted by layer 3; normal after a resend

            if ( accepted == RN_WINDOW_RECEIVE_VIOLATION )
            {
                rn_channels_count_violation( channels, rn_violation_counter( ordered->receive.violation ) );
                return ordered->receive.violation;
            }
        }

        // layer 4: the dispatch

        rn_delivery_t delivery;
        rn_channels_dispatch( channels, &record, &delivery );

        // RNEndpoint OnPayload: a new CONFIG is compared with this end's own
        // (10.3); its number is already released by the dispatch

        if ( record.control && record.code == ROCKETNET_MESSAGE_CODE_RELIABLE )
            rn_ordered_take_config( ordered, &record );
        if ( channels->error != ROCKETNET_DISCONNECT_REASON_NONE )
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
        return ROCKETNET_DISCONNECT_REASON_NONE;

    // tla/RNConnectionMemory.tla, Kick: exhaustion first; then the other side of the connection broke
    // the wire (17.3 item 11); then a reliable message went unacked too long

    const int memory = rn_connection_memory_decide( ordered->channels.region );
    if ( memory != ROCKETNET_DISCONNECT_REASON_NONE )
        return memory;
    if ( ordered->channels.error != ROCKETNET_DISCONNECT_REASON_NONE )
        return ordered->channels.error;

    return ordered->ack.error;
}
