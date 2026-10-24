/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

#include <string.h>

#if defined(ROCKETNET_BLOCK_COST_TEST_HOOKS)
// Unit-library instrumentation only: no production state or per-chunk writes.
static thread_local rn_block_cost_t rn_block_cost;
void rn_channels_block_cost_reset() { memset( &rn_block_cost, 0, sizeof(rn_block_cost) ); }
rn_block_cost_t rn_channels_block_cost_read() { return rn_block_cost; }
#define RN_BLOCK_COST( field ) ( rn_block_cost.field++ )
#else
#define RN_BLOCK_COST( field ) ( (void) 0 )
#endif

static uint64_t rn_reliable_round16( uint64_t bytes )
{
    return ( bytes + 15 ) & ~(uint64_t) 15;
}

// RNChannelTypes!Dispatch and RNChannel!OpenPacket: each accepted number owns
// one chunk until all numbers of the block are live. The first chunk to arrive
// anchors the block and counts the chunks in, as code 3's blocks do
// (rn_channels_block_collect), so the block completes in work linear in its
// chunk count, independent of fragment arrival order. The first number's
// descriptor owns the one application delivery.
static int rn_channels_dispatch_reliable_chunk( rn_channels_t * channels, const rocketnet_message_record_t * record, rn_delivery_t * out )
{
    rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = record->fragments + 1;
    const uint32_t index = record->fragment;
    const bool last = index + 1 == count;

    RN_BLOCK_COST( chunks );
    int64_t base_units = 0;
    if ( !rn_block_shape_valid( channels, record, &base_units ) )
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

    const uint64_t span_end = base_units < 0 ? UINT64_MAX :
        (uint64_t) base_units * 16 + (uint64_t) ( count - 1 ) * F + ( last ? rn_reliable_round16( (uint64_t) record->bytes ) : 16 );
    if ( base_units < 0 || span_end > region->ring_bytes ||
         !rn_region_ring_span_valid( region, (uint64_t) record->ring * 16, rn_reliable_round16( (uint64_t) record->bytes ) ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    // The transport batch is replaced next update; a partial block therefore
    // has its sole receive-side copy in the ring. No ordering cursor gates it:
    // the block is keyed by its first number alone.
    const uint64_t first_number = descriptor->number - index;
    uint32_t held = RN_DESCRIPTOR_NONE;
    RN_BLOCK_COST( descriptor_visits );
    rn_receive_descriptor_t * anchor = rn_channels_block_collect( channels, record, RN_HELD_RELIABLE_BLOCK,
                                                                  (uint32_t) ( first_number & ROCKETNET_MESSAGE_U24_MASK ), slot,
                                                                  (uint32_t) ( first_number & ROCKETNET_MESSAGE_U24_MASK ), base_units, &held );
    if ( !anchor )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    if ( anchor->block_have < count )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;                 // never a partial delivery

    // Every chunk is in: the anchor agreed with each of them on the count, the
    // first number and the base, and each one's size was checked as it came.
    const uint32_t first_slot = (uint32_t) ( first_number & ( region->window - 1 ) );
    const rn_receive_descriptor_t * first = &region->receive_descriptors[first_slot];
    const rn_receive_descriptor_t * final_chunk = &region->receive_descriptors[( first_number + count - 1 ) & ( region->window - 1 )];
    RN_BLOCK_COST( descriptor_visits );
    RN_BLOCK_COST( descriptor_visits );
    const uint32_t total = ( count - 1 ) * F + final_chunk->bytes;
    if ( !first->live || first->number != first_number || !final_chunk->live || final_chunk->number != first_number + count - 1 ||
         total > region->max_message_bytes ||
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

    rn_channels_block_settle( channels, held, anchor, first_slot, count, total, (uint32_t) base_units, false );

    rn_delivery_t * delivery = &region->deliveries[channels->deliveries++];
    delivery->data = region->receive_ring + (uint64_t) base_units * 16;
    delivery->bytes = (int32_t) total;
    delivery->channel = 0;
    delivery->code = ROCKETNET_CHANNEL_TYPE_RELIABLE;
    delivery->pad = 0;
    delivery->serial = 0;
    delivery->descriptor = first_slot;
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
    uint32_t slot = 0;
    rn_receive_descriptor_t * descriptor = rn_channels_descriptor( channels, record, &slot );
    if ( !descriptor )
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
