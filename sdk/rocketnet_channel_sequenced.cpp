/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

#include <string.h>

#include "rocketnet_platform.h"

// Code 1 follows RNChannelTypes!Dispatch and !Drain. Code 4 stays behind its
// own mask until its reliable-sequenced implementation is enabled.
static const bool rn_channel_unreliable_sequenced_enabled = true;
static const bool rn_channel_reliable_sequenced_enabled = false;

static void rn_sequenced_count( rn_channels_t * channels, int counter )
{
    if ( channels->region->counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &channels->region->counters->values[counter], 1 );
}

static bool rn_sequenced_room( rn_channels_t * channels )
{
    // The Du portion is shared with code 0; both are unreliable deliveries.
    if ( channels->unreliable_delivered >= channels->region->delivery_entries - channels->region->window ||
         channels->deliveries >= channels->delivery_capacity )
    {
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
        return false;
    }
    return true;
}

static rn_delivery_t * rn_sequenced_delivery( rn_channels_t * channels, uint32_t channel, const uint8_t * data, int bytes )
{
    rn_delivery_t * delivery = &channels->region->deliveries[channels->deliveries++];
    memset( delivery, 0, sizeof( *delivery ) );
    delivery->data = data;
    delivery->bytes = bytes;
    delivery->channel = (uint16_t) channel;
    delivery->code = ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED;
    delivery->descriptor = RN_DESCRIPTOR_NONE;
    channels->unreliable_delivered++;
    return delivery;
}

// RNChannelTypes!Dispatch: code 1 does not consult the reliable receive window.
int rn_channels_dispatch_sequenced( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    if ( record->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED && !rn_channel_reliable_sequenced_enabled )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }
    if ( record->code != ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED || !rn_channel_unreliable_sequenced_enabled )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    rn_region_t * region = channels->region;
    if ( record->channel >= channels->num_channels )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    rn_channel_receive_t * rx = &region->channel_receive[record->channel];
    const uint32_t ordering = record->ordering & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t sequencing = record->sequencing & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t delta = ( ordering - rx->ordered_read ) & ROCKETNET_MESSAGE_U24_MASK;

    if ( delta >= ( 1u << 23 ) )
    {
        // A late unreliable state may simply be lost; it is not a violation.
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        return ROCKETNET_CHANNEL_DISPATCH_STALE;
    }
    if ( delta >= region->window )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( delta == 0 )
    {
        const uint32_t sequence_gap = ( sequencing - rx->sequenced_watermark ) & ROCKETNET_MESSAGE_U24_MASK;
        if ( sequence_gap >= ( 1u << 23 ) )
        {
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
            return ROCKETNET_CHANNEL_DISPATCH_STALE;
        }
        if ( !rn_sequenced_room( channels ) )
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        if ( !rn_channels_deliver_sequenced( channels, record->channel, ordering, sequencing ) )
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        *out = *rn_sequenced_delivery( channels, record->channel, record->data, record->bytes );
        return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
    }

    // RNChannelTypes!HoldNewest: only one future state per channel is retained.
    // The copy is necessary because the transport's receive batch dies at the
    // update boundary. The in-turn path above remains a pointer-identical view.
    const uint32_t current = rx->held_sequenced;
    if ( current != RN_DESCRIPTOR_NONE )
    {
        rn_sequenced_hold_t * hold = &region->sequenced_holds[current];
        const uint32_t gap = ( ordering - hold->ordering ) & ROCKETNET_MESSAGE_U24_MASK;
        const uint32_t sequence_gap = ( sequencing - hold->sequencing ) & ROCKETNET_MESSAGE_U24_MASK;
        if ( gap >= ( 1u << 23 ) || ( gap == 0 && ( sequence_gap == 0 || sequence_gap >= ( 1u << 23 ) ) ) )
        {
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
            return ROCKETNET_CHANNEL_DISPATCH_STALE;
        }
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
    }

    if ( record->bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        // A fragmented unreliable state is legal. It cannot fit the fixed
        // future-state slot, so drop it without treating the peer as invalid.
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    uint32_t slot = current;
    if ( slot == RN_DESCRIPTOR_NONE )
    {
        for ( uint32_t i = 0; i < region->holds; i++ )
            if ( !region->sequenced_holds[i].used )
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
    }

    rn_sequenced_hold_t * hold = &region->sequenced_holds[slot];
    hold->bytes = (uint32_t) record->bytes;
    hold->ordering = ordering;
    hold->sequencing = sequencing;
    hold->channel = (uint16_t) record->channel;
    hold->code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
    hold->used = 1;
    memcpy( hold->data, record->data, (size_t) record->bytes );
    return ROCKETNET_CHANNEL_DISPATCH_HELD;
}

// RNChannelTypes!Drain: the ordered transition has already opened the epoch.
bool rn_channels_drain_sequenced( struct rn_channels_t * channels, uint32_t channel )
{
    if ( !channels || channel >= channels->num_channels )
        return false;
    rn_channel_receive_t * rx = &channels->region->channel_receive[channel];
    const uint32_t slot = rx->held_sequenced;
    if ( slot == RN_DESCRIPTOR_NONE )
        return false;
    rn_sequenced_hold_t * hold = &channels->region->sequenced_holds[slot];
    const uint32_t gap = ( hold->ordering - rx->ordered_read ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( gap != 0 )
    {
        if ( gap >= ( 1u << 23 ) )
        {
            hold->used = 0;
            rx->held_sequenced = RN_DESCRIPTOR_NONE;
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        }
        return false;
    }

    rx->held_sequenced = RN_DESCRIPTOR_NONE;
    if ( !rn_sequenced_room( channels ) || !rn_channels_deliver_sequenced( channels, channel, hold->ordering, hold->sequencing ) )
    {
        hold->used = 0;
        return false;
    }
    rn_sequenced_delivery( channels, channel, hold->data, (int) hold->bytes );
    hold->used = 2; // delivered view stays live until RNChannel!EndUpdate
    return true;
}

void rn_channels_end_update_sequenced( struct rn_channels_t * channels )
{
    if ( !channels )
        return;
    // RNChannel!EndUpdate: only slots whose views expired return to the pool.
    for ( uint32_t i = 0; i < channels->region->holds; i++ )
        if ( channels->region->sequenced_holds[i].used == 2 )
            channels->region->sequenced_holds[i].used = 0;
}
