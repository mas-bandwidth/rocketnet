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

    return true;
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
    rn_channels_count( channels->region->counters, counter );
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
            if ( channels->unreliable_delivered >= channels->delivery_capacity )
            {
                rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
                return ROCKETNET_CHANNEL_DISPATCH_NONE;
            }

            if ( record->channel >= channels->num_channels )
            {
                rn_channels_count_violation( channels, ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL );
                return ROCKETNET_CHANNEL_DISPATCH_NONE;
            }

            memset( out, 0, sizeof( *out ) );
            out->data = record->data;                       // a view, pointer identity preserved
            out->bytes = record->bytes;
            out->channel = (uint16_t) record->channel;
            out->code = ROCKETNET_CHANNEL_TYPE_UNRELIABLE;
            out->serial = 0;

            channels->unreliable_delivered++;
            channels->deliveries++;

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

int rn_channels_dispatch_ordered( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    if ( record->channel >= channels->num_channels )
    {
        rn_channels_count_violation( channels, ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const rn_channel_receive_t * receive = channels->region->channel_receive + record->channel;
    const uint32_t ordering = record->ordering;

    if ( ordering < receive->ordered_read )
    {
        // 7.2: a reliable ordered record behind the read index can only be a
        // duplicate, and layer 3 already caught those.
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( ordering > receive->ordered_read )
    {
        // Held ahead of its turn: the ordered card copies it into the ring at
        // its offset and inserts (channel, ordering) into the held index.
        // Until then nothing holds it, and the record is not delivered.
        return ROCKETNET_CHANNEL_DISPATCH_HELD;
    }

    // Its turn: delivered as a view into the batch (the record arrived this
    // update), then the coupling runs and the drain follows (7.2).
    memset( out, 0, sizeof( *out ) );
    out->data = record->data;
    out->bytes = record->bytes;
    out->channel = (uint16_t) record->channel;
    out->code = ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED;
    out->serial = 0;

    rn_channels_deliver_ordered( channels, record->channel, ordering );

    channels->deliveries++;

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

void rn_channels_end_update( struct rn_channels_t * channels )
{
    if ( !channels )
        return;

    // RNChannel!EndUpdate: the batch's views are gone, so the per-update
    // counts start over.
    channels->unreliable_delivered = 0;
    channels->deliveries = 0;
    channels->violations = 0;
}
