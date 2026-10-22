/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

#include <string.h>

static uint64_t rn_reliable_round16( uint64_t bytes )
{
    return ( bytes + 15 ) & ~(uint64_t) 15;
}

// RNChannelTypes!Dispatch and RNChannel!OpenPacket: each accepted number owns
// one chunk until all numbers of the block are live. The first number anchors
// the one application delivery, independent of fragment arrival order.
static int rn_channels_dispatch_reliable_chunk( rn_channels_t * channels, const rocketnet_message_record_t * record, rn_delivery_t * out )
{
    rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = record->fragments + 1;
    const uint32_t index = record->fragment;
    const uint32_t max_count = ( region->max_message_bytes + F - 1 ) / F;
    const bool last = index + 1 == count;

    if ( count < 2 || count > max_count || count > region->window || index >= count ||
         ( last ? ( record->bytes < 1 || (uint32_t) record->bytes > F ) : (uint32_t) record->bytes != F ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t slot = record->number & ( region->window - 1 );
    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[slot];
    if ( !descriptor->live || (uint32_t) ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) != record->number ||
         descriptor->number < index )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const int64_t base_units = (int64_t) record->ring - (int64_t) index * ( F / 16 );
    const uint64_t span_end = base_units < 0 ? UINT64_MAX :
        (uint64_t) base_units * 16 + (uint64_t) ( count - 1 ) * F + ( last ? rn_reliable_round16( (uint64_t) record->bytes ) : 16 );
    if ( base_units < 0 || span_end > region->ring_bytes ||
         !rn_region_ring_span_valid( region, (uint64_t) record->ring * 16, rn_reliable_round16( (uint64_t) record->bytes ) ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint64_t first_number = descriptor->number - index;
    descriptor->block_count = (uint16_t) count;
    descriptor->block_have = 1;
    memcpy( region->receive_ring + (uint64_t) record->ring * 16, record->data, (size_t) record->bytes );

    // The transport batch is replaced next update; a partial block therefore
    // has its sole receive-side copy in the ring. No ordering cursor gates it.
    uint32_t last_bytes = 0;
    for ( uint32_t i = 0; i < count; i++ )
    {
        const rn_receive_descriptor_t * part = &region->receive_descriptors[( first_number + i ) & ( region->window - 1 )];
        if ( !part->live || part->number != first_number + i )
            return ROCKETNET_CHANNEL_DISPATCH_HELD;
        if ( part->code != ROCKETNET_MESSAGE_CODE_RELIABLE || part->block_count != count ||
             part->ring != (uint32_t) base_units + i * ( F / 16 ) ||
             ( i + 1 < count ? part->bytes != F : part->bytes < 1 || part->bytes > F ) )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }
        if ( i + 1 == count )
            last_bytes = part->bytes;
    }

    const uint32_t total = ( count - 1 ) * F + last_bytes;
    if ( total > region->max_message_bytes ||
         !rn_region_ring_span_valid( region, (uint64_t) base_units * 16, rn_reliable_round16( total ) ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }
    if ( channels->deliveries >= channels->delivery_capacity )
    {
        rn_region_exhaust( region );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    rn_receive_descriptor_t * first = &region->receive_descriptors[first_number & ( region->window - 1 )];
    first->block_count = (uint16_t) count;       // EndUpdate releases every number in this block
    first->bytes = total;
    first->ring = (uint32_t) base_units;

    rn_delivery_t * delivery = &region->deliveries[channels->deliveries++];
    delivery->data = region->receive_ring + (uint64_t) base_units * 16;
    delivery->bytes = (int32_t) total;
    delivery->channel = 0;
    delivery->code = ROCKETNET_CHANNEL_TYPE_RELIABLE;
    delivery->pad = 0;
    delivery->serial = 0;
    delivery->descriptor = (uint32_t) ( first_number & ( region->window - 1 ) );
    *out = *delivery;
    return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
}

// RNChannelTypes!Dispatch: layer 3 accepts a reliable number once, so code 2
// delivers without consulting an ordering cursor. The view of a small record
// remains in the transport's batch until the update ends.
int rn_channels_dispatch_reliable( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    if ( record->control )
    {
        // Layer 3 already accepted its number, but a control record is never
        // an application view and has no delivery-table entry to free it.
        rn_region_t * region = channels->region;
        rn_receive_descriptor_t * descriptor = &region->receive_descriptors[record->number & ( region->window - 1 )];
        if ( descriptor->live && (uint32_t) ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) == record->number )
            descriptor->live = 0;
        return ROCKETNET_CHANNEL_DISPATCH_CONTROL;
    }

    if ( record->block )
        return rn_channels_dispatch_reliable_chunk( channels, record, out );

    rn_region_t * region = channels->region;
    const uint32_t slot = record->number & ( region->window - 1 );
    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[slot];
    if ( !descriptor->live || (uint32_t) ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) != record->number )
    {
        // the slot is live for another number (check 10, SLOT_COLLISION)
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }
    if ( channels->deliveries >= channels->delivery_capacity )
    {
        rn_region_exhaust( region );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    rn_delivery_t * delivery = &region->deliveries[channels->deliveries++];
    delivery->data = record->data;
    delivery->bytes = record->bytes;
    delivery->channel = 0;
    delivery->code = ROCKETNET_CHANNEL_TYPE_RELIABLE;
    delivery->pad = 0;
    delivery->serial = 0;
    delivery->descriptor = slot;
    *out = *delivery;
    return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
}
