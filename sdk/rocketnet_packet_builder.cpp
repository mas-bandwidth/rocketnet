/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_packet_builder.h"

#include <string.h>

#include "rocketnet_platform.h"

// Layer 6's packet builder (docs/CHANNELS.md 9.3). tla/RNChannel.tla's
// WritePacket is the packet, tla/RNSendBudget.tla's Send, Wait and Drop are
// the budget's answers; each function names the action it implements.

static void rn_builder_count( rocketnet_counters_t * counters, int counter )
{
    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &counters->values[counter], 1 );
}

bool rn_packet_builder_init( struct rn_packet_builder_t * builder, struct rn_region_t * region, struct rn_window_ack_t * ack, struct rocketnet_send_budget_t * budget, int max_packets_per_update, int payload_capacity )
{
    if ( !builder )
        return false;

    memset( builder, 0, sizeof( *builder ) );

    if ( !region || payload_capacity < 1 || payload_capacity > ROCKETNET_MESSAGE_SINGLE_BYTES )
        return false;

    builder->region = region;
    builder->ack = ack;
    builder->budget = budget;
    builder->payload_capacity = payload_capacity;
    builder->max_packets_per_update = max_packets_per_update < 1 ? 1 : ( max_packets_per_update > ROCKETNET_PACKET_BUILDER_MAX_PACKETS ? ROCKETNET_PACKET_BUILDER_MAX_PACKETS : max_packets_per_update );
    builder->num_channels = region->num_channels;

    return true;
}

// one item as the record the writer takes

static void rn_builder_record( const struct rn_packet_item_t * item, struct rocketnet_message_record_t * record )
{
    memset( record, 0, sizeof( *record ) );
    record->code = item->code;
    record->data = item->data;
    record->bytes = (int) item->bytes;

    // only the fields the record's code carries (13.2): a field the code does
    // not carry is zero, so the record is valid and packs one way.
    switch ( item->code )
    {
        case ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED:
            record->channel = item->channel;
            record->ordering = item->ordering;
            record->sequencing = item->sequencing;
            break;

        case ROCKETNET_MESSAGE_CODE_RELIABLE:
            record->number = item->number == ROCKETNET_PACKET_NUMBER_NONE ? 0 : (uint32_t) ( item->number & ROCKETNET_MESSAGE_U24_MASK );
            break;

        case ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED:
            record->number = item->number == ROCKETNET_PACKET_NUMBER_NONE ? 0 : (uint32_t) ( item->number & ROCKETNET_MESSAGE_U24_MASK );
            record->channel = item->channel;
            record->ordering = item->ordering;
            break;

        case ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED:
            record->number = item->number == ROCKETNET_PACKET_NUMBER_NONE ? 0 : (uint32_t) ( item->number & ROCKETNET_MESSAGE_U24_MASK );
            record->channel = item->channel;
            record->ordering = item->ordering;
            record->sequencing = item->sequencing;
            break;

        default:
            break;                                              // code 0 carries no field but its length
    }
}

// the next item of this class, round robin across channels from the cursor
// (17.3 item 4). returns the item's index, or -1 when the class is spent.

static int rn_builder_next_in_class( const struct rn_packet_builder_t * builder, const struct rn_packet_item_t * items, int num_items, int send_class )
{
    const uint32_t channels = builder->num_channels > 0 ? builder->num_channels : 1;

    for ( uint32_t step = 0; step < channels; step++ )
    {
        const uint32_t channel = ( builder->round_robin + 1 + step ) % channels;

        for ( int i = 0; i < num_items; i++ )
        {
            if ( items[i].taken || items[i].send_class != (uint8_t) send_class || items[i].channel != channel )
                continue;
            if ( items[i].bytes < 1 || !items[i].data )
                continue;
            return i;
        }
    }

    return -1;
}

int rn_packet_builder_write( struct rn_packet_builder_t * builder, struct rn_packet_item_t * items, int num_items, uint8_t ** payloads, int num_payloads )
{
    if ( !builder || builder->max_packets_per_update == 0 )
        return 0;

    builder->packets = 0;
    builder->unreliable_dropped = 0;

    if ( !items || num_items < 1 || !payloads || num_payloads < 1 )
        return 0;

    const int limit = builder->max_packets_per_update < num_payloads ? builder->max_packets_per_update : num_payloads;

    for ( int p = 0; p < limit; p++ )
    {
        if ( !payloads[p] )
            break;

        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payloads[p], builder->payload_capacity );

        uint64_t numbers[ROCKETNET_PACKET_BUILDER_MAX_NUMBERS];
        uint32_t count = 0;
        uint32_t records = 0;
        int packet_class = -1;
        int taken[ROCKETNET_PACKET_BUILDER_MAX_NUMBERS];

        // 9.3: fill in the budget's class order. Within a class, channels take
        // turns one item each (round robin), so one chatty channel cannot
        // starve the others.
        for ( int send_class = ROCKETNET_SEND_CLASS_RETRANSMIT; send_class <= ROCKETNET_SEND_CLASS_UNRELIABLE; send_class++ )
        {
            for ( ;; )
            {
                const int i = rn_builder_next_in_class( builder, items, num_items, send_class );
                if ( i < 0 )
                    break;

                struct rocketnet_message_record_t record;
                rn_builder_record( &items[i], &record );

                // a record that does not fit ends this packet; it is never
                // split across packets (9.3).
                if ( !rocketnet_message_writer_add( &writer, &record ) )
                {
                    items[i].taken = 0;
                    goto packet_done;
                }

                items[i].taken = 1;
                if ( records < ROCKETNET_PACKET_BUILDER_MAX_NUMBERS )
                    taken[records] = i;
                records++;

                if ( packet_class < 0 )
                    packet_class = send_class;

                // I3: an unreliable record never takes a reliable number.
                if ( items[i].number != ROCKETNET_PACKET_NUMBER_NONE && count < ROCKETNET_PACKET_BUILDER_MAX_NUMBERS )
                    numbers[count++] = items[i].number;

                builder->round_robin = items[i].channel;
            }
        }

    packet_done:

        const int payload_bytes = rocketnet_message_writer_finish( &writer );
        if ( payload_bytes <= 0 )
            break;                                              // an empty plan writes nothing (9.3)

        // R6 and RNSendBudget: the packet is admitted at the class of the
        // first item it carries, at the most wire bytes its payload can take
        // once the transport adds its ack block. settle refunds the rest.
        const int admitted = rocketnet_send_budget_max_wire_bytes( payload_bytes );
        int decision = ROCKETNET_SEND_BUDGET_SEND;

        if ( builder->budget && packet_class >= 0 )
            decision = rocketnet_send_budget_admit( builder->budget, packet_class, admitted );

        if ( decision != ROCKETNET_SEND_BUDGET_SEND )
        {
            // The packet is not written. Its items stay as they were: a
            // reliable one waits for the next update and is never dropped, an
            // unreliable one is dropped and counted. Either way nothing after
            // it spends the bucket this update.
            for ( uint32_t r = 0; r < records && r < ROCKETNET_PACKET_BUILDER_MAX_NUMBERS; r++ )
                items[taken[r]].taken = 0;

            if ( packet_class == ROCKETNET_SEND_CLASS_UNRELIABLE )
            {
                for ( uint32_t r = 0; r < records && r < ROCKETNET_PACKET_BUILDER_MAX_NUMBERS; r++ )
                {
                    builder->unreliable_dropped++;
                    rn_builder_count( builder->region->counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
                }
            }

            break;
        }

        auto * slot = &builder->packet[p];

        slot->count = count;
        for ( uint32_t n = 0; n < count; n++ )
            slot->numbers[n] = numbers[n];
        slot->records = records;
        slot->payload_bytes = (uint32_t) payload_bytes;
        slot->admitted_wire_bytes = builder->budget ? admitted : 0;

        builder->packets++;
    }

    return (int) builder->packets;
}

void rn_packet_builder_commit( struct rn_packet_builder_t * builder, int packet_index, uint64_t packet_id, double now )
{
    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets || !builder->ack )
        return;

    const uint32_t count = builder->packet[packet_index].count;
    if ( count == 0 )
        return;                                                 // a pure-unreliable packet takes no sent-map entry

    // RNChannel!WritePacket's smap': keyed by the transport's own packet id.
    rn_window_ack_commit_packet( builder->ack, packet_id, builder->packet[packet_index].numbers, count, now );
}

void rn_packet_builder_packet_failed( struct rn_packet_builder_t * builder, int packet_index, double now )
{
    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets || !builder->ack )
        return;

    const uint32_t count = builder->packet[packet_index].count;
    if ( count == 0 )
        return;

    rn_window_ack_packet_failed( builder->ack, builder->packet[packet_index].numbers, count, now );
}

bool rn_packet_builder_settle( struct rn_packet_builder_t * builder, int packet_index, int actual_wire_bytes )
{
    if ( !builder || !builder->budget || packet_index < 0 || packet_index >= (int) builder->packets )
        return false;

    const int admitted = builder->packet[packet_index].admitted_wire_bytes;
    if ( admitted <= 0 )
        return false;

    return rocketnet_send_budget_settle( builder->budget, admitted, actual_wire_bytes );
}

const uint64_t * rn_packet_builder_numbers( const struct rn_packet_builder_t * builder, int packet_index, uint32_t * out_count )
{
    if ( out_count )
        *out_count = 0;

    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets )
        return NULL;

    if ( out_count )
        *out_count = builder->packet[packet_index].count;

    return builder->packet[packet_index].numbers;
}
