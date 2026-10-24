/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

#include <string.h>

#include "rocketnet_platform.h"

// Codes 1 and 4 follow RNChannelTypes!Dispatch and !Drain. A code-4 hold
// uses its receive descriptor and ring span; the high bit distinguishes that
// descriptor index from a code-1 Ru hold without changing channel state.
static const uint32_t RN_SEQUENCED_RELIABLE_HOLD = 0x80000000u;

// tla/RNChannelTypes.tla OpenPacket
static bool rn_sequenced_reliable_hold( uint32_t hold )
{
    return hold != RN_DESCRIPTOR_NONE && ( hold & RN_SEQUENCED_RELIABLE_HOLD ) != 0;
}

static rn_receive_descriptor_t * rn_sequenced_descriptor( rn_channels_t * channels, const rocketnet_message_record_t * record, uint32_t * slot )
{
    rn_region_t * region = channels->region;
    *slot = record->number & ( region->window - 1 );
    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[*slot];
    if ( !descriptor->live || ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) != record->number ||
         descriptor->code != ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        return NULL;
    return descriptor;
}

static void rn_sequenced_release_number( rn_channels_t * channels, uint64_t number )
{
    rn_region_t * region = channels->region;
    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[number & ( region->window - 1 )];
    if ( descriptor->live && ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) == ( number & ROCKETNET_MESSAGE_U24_MASK ) )
        descriptor->live = 0;
}

static void rn_sequenced_release_hold( rn_channels_t * channels, rn_channel_receive_t * rx )
{
    const uint32_t hold = rx->held_sequenced;
    if ( hold == RN_DESCRIPTOR_NONE )
        return;
    rn_region_t * region = channels->region;
    if ( rn_sequenced_reliable_hold( hold ) )
    {
        const rn_receive_descriptor_t * descriptor = &region->receive_descriptors[hold & ~RN_SEQUENCED_RELIABLE_HOLD];
        const uint32_t count = descriptor->block_count ? descriptor->block_count : 1;
        for ( uint32_t i = 0; i < count; i++ )
            rn_sequenced_release_number( channels, (uint64_t) rx->pad + i );
    }
    else
        region->sequenced_messages_held[hold].used = 0;
    rx->held_sequenced = RN_DESCRIPTOR_NONE;
}

static void rn_sequenced_count( rn_channels_t * channels, int counter )
{
    if ( channels->region->counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &channels->region->counters->values[counter], 1 );
}

static bool rn_sequenced_room( rn_channels_t * channels, bool reliable )
{
    // The Du portion is shared with code 0; both are unreliable deliveries.
    if ( ( !reliable && channels->unreliable_delivered >= channels->region->delivery_entries - channels->region->window ) ||
         channels->deliveries >= channels->delivery_capacity )
    {
        if ( reliable )
            rn_region_exhaust( channels->region );
        else
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
        return false;
    }
    return true;
}

static rn_delivery_t * rn_sequenced_delivery( rn_channels_t * channels, uint32_t channel, const uint8_t * data, int bytes,
                                               int code, uint32_t descriptor )
{
    rn_delivery_t * delivery = &channels->region->deliveries[channels->deliveries++];
    memset( delivery, 0, sizeof( *delivery ) );
    delivery->data = data;
    delivery->bytes = bytes;
    delivery->channel = (uint16_t) channel;
    delivery->code = (uint8_t) code;
    delivery->descriptor = descriptor;
    if ( code == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED )
        channels->unreliable_delivered++;
    return delivery;
}

// tla/RNChannelTypes.tla Dispatch
// RNChannelTypes!Dispatch for a code-4 block. The first fragment to arrive is
// the anchor; rx->pad holds its first number, and only one block per channel
// can be the newest sequenced state. Each fragment is copied once to its own
// offset because its receive batch expires before the block can complete.
static int rn_sequenced_block( rn_channels_t * channels, const rocketnet_message_record_t * record,
                               rn_channel_receive_t * rx, uint32_t delta, uint32_t slot, rn_delivery_t * out )
{
    rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = record->fragments + 1;
    const uint32_t index = record->fragment;
    const uint32_t max_count = ( region->max_message_bytes + F - 1 ) / F;
    const bool last = index + 1 == count;
    const int64_t base = (int64_t) record->ring - (int64_t) index * ( F / 16 );
    const uint64_t end = base < 0 ? UINT64_MAX : (uint64_t) base * 16 + (uint64_t) ( count - 1 ) * F +
        ( last ? ( ( (uint64_t) record->bytes + 15 ) & ~(uint64_t) 15 ) : 16 );
    if ( record->fragments == 0 || count > max_count || count > region->window || index >= count ||
         ( last ? record->bytes < 1 || (uint32_t) record->bytes > F : (uint32_t) record->bytes != F ) ||
         base < 0 || end > region->ring_bytes )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t first_number = ( record->number - index ) & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t current = rx->held_sequenced;
    rn_receive_descriptor_t * anchor;
    if ( current == RN_DESCRIPTOR_NONE )
    {
        rx->held_sequenced = slot | RN_SEQUENCED_RELIABLE_HOLD;
        rx->pad = first_number;
        anchor = &region->receive_descriptors[slot];
        anchor->block_count = (uint16_t) count;
        anchor->block_have = 0;
        anchor->ring = (uint32_t) base;
    }
    else
    {
        if ( !rn_sequenced_reliable_hold( current ) )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }
        anchor = &region->receive_descriptors[current & ~RN_SEQUENCED_RELIABLE_HOLD];
        if ( !anchor->live || anchor->block_count != count || anchor->block_have >= count ||
             rx->pad != first_number || anchor->ring != (uint32_t) base ||
             anchor->ordering != record->ordering || anchor->sequencing != record->sequencing )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }
    }

    memcpy( region->receive_ring + (uint64_t) record->ring * 16, record->data, (size_t) record->bytes );
    anchor->block_have++;
    if ( anchor->block_have < count )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;

    // Every chunk is present; the first descriptor releases the entire block
    // when its delivered view expires, just as for code 3.
    const uint32_t first_slot = first_number & ( region->window - 1 );
    const uint32_t final_number = ( first_number + count - 1 ) & ROCKETNET_MESSAGE_U24_MASK;
    rn_receive_descriptor_t * first = &region->receive_descriptors[first_slot];
    rn_receive_descriptor_t * final = &region->receive_descriptors[final_number & ( region->window - 1 )];
    const uint32_t total = ( count - 1 ) * F + final->bytes;
    if ( !first->live || ( first->number & ROCKETNET_MESSAGE_U24_MASK ) != first_number ||
         !final->live || ( final->number & ROCKETNET_MESSAGE_U24_MASK ) != final_number ||
         total > region->max_message_bytes )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }
    if ( anchor != first )
        anchor->block_count = 0;
    first->block_count = (uint16_t) count;
    first->block_have = (uint16_t) count;
    first->ring = (uint32_t) base;
    first->bytes = total;
    rx->held_sequenced = first_slot | RN_SEQUENCED_RELIABLE_HOLD;
    if ( delta != 0 )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;
    if ( !rn_sequenced_room( channels, true ) ||
         !rn_channels_deliver_sequenced( channels, record->channel, record->ordering, record->sequencing ) )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    *out = *rn_sequenced_delivery( channels, record->channel, region->receive_ring + (uint64_t) base * 16,
                                    (int) total, ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED, first_slot );
    rx->held_sequenced = RN_DESCRIPTOR_NONE;
    return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
}

// tla/RNChannelTypes.tla Dispatch
// RNChannelTypes!Dispatch: code 1 does not consult the reliable receive window.
int rn_channels_dispatch_sequenced( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    const bool reliable = record->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
    // the mask is checked once, by wire code, in rn_channels_dispatch

    rn_region_t * region = channels->region;
    if ( record->channel >= channels->num_channels )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    uint32_t number_slot = RN_DESCRIPTOR_NONE;
    if ( reliable && !rn_sequenced_descriptor( channels, record, &number_slot ) )
    {
        // the slot is live for another number (check 10, SLOT_COLLISION)
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    rn_channel_receive_t * rx = &region->channel_receive[record->channel];
    const uint32_t ordering = record->ordering & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t sequencing = record->sequencing & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t delta = ( ordering - rx->ordered_read ) & ROCKETNET_MESSAGE_U24_MASK;

    if ( delta >= ( 1u << 23 ) )
    {
        // RNChannelTypes!Dispatch: the transport already accepted and ACKs a
        // stale code-4 packet. Only its application view is discarded (I12).
        if ( reliable )
            rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        return ROCKETNET_CHANNEL_DISPATCH_STALE;
    }
    if ( delta >= region->window )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( delta == 0 && ( !reliable || !record->block ) )
    {
        // A newer in-turn state can overtake a code-4 block still collecting
        // in this epoch. Release its numbered ring hold now; otherwise the
        // missing fragments could leave a live descriptor indefinitely.
        const uint32_t current = rx->held_sequenced;
        if ( rn_sequenced_reliable_hold( current ) )
        {
            const rn_receive_descriptor_t * held = &region->receive_descriptors[current & ~RN_SEQUENCED_RELIABLE_HOLD];
            if ( held->ordering == ordering )
            {
                const uint32_t held_gap = ( sequencing - held->sequencing ) & ROCKETNET_MESSAGE_U24_MASK;
                if ( held_gap == 0 || held_gap >= ( 1u << 23 ) )
                {
                    if ( reliable )
                        rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );
                    rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
                    return ROCKETNET_CHANNEL_DISPATCH_STALE;
                }
                rn_sequenced_release_hold( channels, rx );
                rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
            }
        }
        const uint32_t sequence_gap = ( sequencing - rx->sequenced_watermark ) & ROCKETNET_MESSAGE_U24_MASK;
        if ( sequence_gap >= ( 1u << 23 ) )
        {
            if ( reliable )
                rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
            return ROCKETNET_CHANNEL_DISPATCH_STALE;
        }
        if ( !rn_sequenced_room( channels, reliable ) )
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        if ( !rn_channels_deliver_sequenced( channels, record->channel, ordering, sequencing ) )
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        *out = *rn_sequenced_delivery( channels, record->channel, record->data, record->bytes,
                                       record->code, reliable ? number_slot : RN_DESCRIPTOR_NONE );
        return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
    }

    if ( delta == 0 )
    {
        const uint32_t sequence_gap = ( sequencing - rx->sequenced_watermark ) & ROCKETNET_MESSAGE_U24_MASK;
        if ( sequence_gap >= ( 1u << 23 ) )
        {
            rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
            return ROCKETNET_CHANNEL_DISPATCH_STALE;
        }
    }

    // RNChannelTypes!HoldNewest: only one future state per channel is retained.
    // Code 1 uses Ru; code 4 uses its numbered ring span, including blocks.
    if ( !reliable && record->bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
    {
        // A fragmented unreliable state is legal. It cannot fit the fixed
        // future-state slot, so drop it without treating the sender as invalid.
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t current = rx->held_sequenced;
    if ( current != RN_DESCRIPTOR_NONE )
    {
        const bool held_reliable = rn_sequenced_reliable_hold( current );
        const uint32_t held_slot = current & ~RN_SEQUENCED_RELIABLE_HOLD;
        const uint32_t held_ordering = held_reliable ? region->receive_descriptors[held_slot].ordering : region->sequenced_messages_held[current].ordering;
        const uint32_t held_sequencing = held_reliable ? region->receive_descriptors[held_slot].sequencing : region->sequenced_messages_held[current].sequencing;
        const uint32_t gap = ( ordering - held_ordering ) & ROCKETNET_MESSAGE_U24_MASK;
        const uint32_t sequence_gap = ( sequencing - held_sequencing ) & ROCKETNET_MESSAGE_U24_MASK;
        const bool continuation = reliable && record->block && held_reliable && gap == 0 && sequence_gap == 0 &&
            region->receive_descriptors[held_slot].block_count > 0;
        if ( gap >= ( 1u << 23 ) || ( gap == 0 && ( sequence_gap >= ( 1u << 23 ) || ( sequence_gap == 0 && !continuation ) ) ) )
        {
            if ( reliable )
                rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
            return ROCKETNET_CHANNEL_DISPATCH_STALE;
        }
        if ( !continuation )
        {
            rn_sequenced_release_hold( channels, rx );
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
        }
    }

    if ( reliable )
    {
        if ( record->block )
            return rn_sequenced_block( channels, record, rx, delta, number_slot, out );
        const uint64_t offset = (uint64_t) record->ring * 16;
        const uint64_t span = ( (uint64_t) record->bytes + 15 ) & ~(uint64_t) 15;
        if ( !rn_region_ring_span_valid( region, offset, span ) )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }
        // THE COPIES (b): this future state outlives the receive batch.
        memcpy( region->receive_ring + offset, record->data, (size_t) record->bytes );
        rx->held_sequenced = number_slot | RN_SEQUENCED_RELIABLE_HOLD;
        rx->pad = (uint32_t) region->receive_descriptors[number_slot].number;
        return ROCKETNET_CHANNEL_DISPATCH_HELD;
    }

    uint32_t slot = RN_DESCRIPTOR_NONE;
    for ( uint32_t i = 0; i < region->holds; i++ )
        if ( !region->sequenced_messages_held[i].used )
        {
            slot = i;
            break;
        }
    if ( slot == RN_DESCRIPTOR_NONE )
    {
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }
    rx->held_sequenced = slot;

    rn_sequenced_hold_t * hold = &region->sequenced_messages_held[slot];
    hold->bytes = (uint32_t) record->bytes;
    hold->ordering = ordering;
    hold->sequencing = sequencing;
    hold->channel = (uint16_t) record->channel;
    hold->code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
    hold->used = 1;
    memcpy( region->sequenced_message_payloads[slot].data, record->data, (size_t) record->bytes );
    return ROCKETNET_CHANNEL_DISPATCH_HELD;
}

// tla/RNChannelTypes.tla Drain
// RNChannelTypes!Drain: the ordered transition has already opened the epoch.
bool rn_channels_drain_sequenced( struct rn_channels_t * channels, uint32_t channel )
{
    if ( !channels || channel >= channels->num_channels )
        return false;
    rn_channel_receive_t * rx = &channels->region->channel_receive[channel];
    const uint32_t slot = rx->held_sequenced;
    if ( slot == RN_DESCRIPTOR_NONE )
        return false;
    const bool reliable = rn_sequenced_reliable_hold( slot );
    rn_receive_descriptor_t * descriptor = reliable ?
        &channels->region->receive_descriptors[slot & ~RN_SEQUENCED_RELIABLE_HOLD] : NULL;
    rn_sequenced_hold_t * hold = reliable ? NULL : &channels->region->sequenced_messages_held[slot];
    const uint32_t ordering = reliable ? descriptor->ordering : hold->ordering;
    const uint32_t sequencing = reliable ? descriptor->sequencing : hold->sequencing;
    const uint32_t gap = ( ordering - rx->ordered_read ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( gap != 0 )
    {
        if ( gap >= ( 1u << 23 ) )
        {
            rn_sequenced_release_hold( channels, rx );
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        }
        return false;
    }

    if ( reliable && descriptor->block_count > 0 && descriptor->block_have < descriptor->block_count )
        return false; // the ordered epoch opened while this block is incomplete

    rx->held_sequenced = RN_DESCRIPTOR_NONE;
    if ( !rn_sequenced_room( channels, reliable ) || !rn_channels_deliver_sequenced( channels, channel, ordering, sequencing ) )
    {
        rx->held_sequenced = slot;
        rn_sequenced_release_hold( channels, rx );
        return false;
    }
    if ( reliable )
    {
        const uint32_t number_slot = slot & ~RN_SEQUENCED_RELIABLE_HOLD;
        rn_sequenced_delivery( channels, channel, channels->region->receive_ring + (uint64_t) descriptor->ring * 16,
                               (int) descriptor->bytes, ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED, number_slot );
    }
    else
    {
        rn_sequenced_delivery( channels, channel, channels->region->sequenced_message_payloads[slot].data, (int) hold->bytes,
                               ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED, RN_DESCRIPTOR_NONE );
        hold->used = 2; // delivered view stays live until RNChannel!EndUpdate
    }
    return true;
}

void rn_channels_end_update_sequenced( struct rn_channels_t * channels )
{
    if ( !channels )
        return;
    // RNChannel!EndUpdate: only slots whose views expired return to the pool.
    // The scan walks compact metadata without pulling payload cache lines in.
    for ( uint32_t i = 0; i < channels->region->holds; i++ )
        if ( channels->region->sequenced_messages_held[i].used == 2 )
            channels->region->sequenced_messages_held[i].used = 0;
}
