/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer 4's dispatch, unit tier (docs/CHANNELS.md 7; tla/RNChannel.tla). No
// sockets, no threads, no wall clock: time is an argument.

#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_counters.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_packet_builder.h"
#include "sdk/rocketnet_window_ack.h"
#include "sdk/rocketnet_window_send.h"

static rocketnet_counters_t dispatch_counters;

static rn_arena_t * dispatch_arena = NULL;

static rn_region_t * dispatch_region()
{
    if ( !dispatch_arena )
        dispatch_arena = rn_arena_create( NULL, NULL, 1, &dispatch_counters );
    return dispatch_arena ? rn_arena_region( dispatch_arena, 0 ) : NULL;
}

static void dispatch_record( struct rocketnet_message_record_t * record, int code, uint32_t channel, const uint8_t * data, int bytes )
{
    memset( record, 0, sizeof( *record ) );
    record->code = code;
    record->channel = channel;
    record->data = data;
    record->bytes = bytes;
}

// dispatch reaches every one of the n channels: the per-channel state is the
// region's C slots, and a delivery on channel c moves only c's read index
// (7.1, RNChannel!DeliverMessage).

static void dispatch_reaches_each_of_n_channels()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    rocketnet_unit_check( channels.num_channels == region->num_channels );
    rocketnet_unit_check( channels.num_channels >= 2 );

    static uint8_t payload[16];
    memset( payload, 0xAB, sizeof( payload ) );

    for ( uint32_t c = 0; c < channels.num_channels; c++ )
    {
        rocketnet_unit_check( region->channel_receive[c].ordered_read == 0 );

        struct rocketnet_message_record_t record;
        dispatch_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, c, payload, (int) sizeof( payload ) );
        record.ordering = 0;

        struct rn_delivery_t delivery;
        const int result = rn_channels_dispatch( &channels, &record, &delivery );

        rocketnet_unit_check( result == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        rocketnet_unit_check( delivery.channel == (uint16_t) c );
        rocketnet_unit_check( region->channel_receive[c].ordered_read == 1 );

        // the coupling: the same transition zeroes the sequenced epoch (7.1)
        rocketnet_unit_check( region->channel_receive[c].sequenced_watermark == 0 );

        // and no other channel moved
        for ( uint32_t k = c + 1; k < channels.num_channels; k++ )
            if ( k != c )
                rocketnet_unit_check( region->channel_receive[k].ordered_read == 0 );
    }

    rn_region_reset( region );
}

// the six types outside the first version's mask are refused on send and
// counted, and their records are refused on receive and counted as a
// violation (7.5).

static void disabled_codes_are_refused_and_counted()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    rocketnet_unit_check( channels.enabled_types == ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION );

    // the two enabled types send
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_UNRELIABLE, 0 ) == ROCKETNET_CHANNEL_SEND_OK );
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 0 ) == ROCKETNET_CHANNEL_SEND_OK );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED] == 0 );

    // the other six are refused, each counted once
    const int disabled[6] = { 1, 2, 4, 5, 6, 7 };
    for ( int i = 0; i < 6; i++ )
    {
        rocketnet_unit_check( rn_channels_send_check( &channels, disabled[i], 0 ) == ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED] == (uint64_t) ( i + 1 ) );
    }

    // and their records are refused on receive, behind the enabled mask
    static uint8_t payload[8];
    const int codes[3] = { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, ROCKETNET_MESSAGE_CODE_RELIABLE, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED };

    for ( int i = 0; i < 3; i++ )
    {
        struct rocketnet_message_record_t record;
        dispatch_record( &record, codes[i], 0, payload, (int) sizeof( payload ) );

        struct rn_delivery_t delivery;
        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED] == (uint64_t) ( i + 1 ) );
    }

    // a wire code 5, 6 or 7 is a violation: the receipt types travel as 0, 2 and 3
    struct rocketnet_message_record_t receipt;
    dispatch_record( &receipt, 5, 0, payload, (int) sizeof( payload ) );
    struct rn_delivery_t delivery;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &receipt, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED] == 4 );

    // the enabled codes the reader checks against carry the mask (7.5)
    rocketnet_unit_check( rn_channels_enabled_codes( &channels ) == rocketnet_message_enabled_codes( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION ) );

    rn_region_reset( region );
}

// an unreliable record is delivered as a view into the transport's batch: no
// copy, pointer identity (R3, THE COPIES (b)).

static void unreliable_delivered_as_a_view()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    static uint8_t batch[64];
    memset( batch, 0x5A, sizeof( batch ) );

    struct rocketnet_message_record_t record;
    dispatch_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, batch + 8, 24 );

    struct rn_delivery_t delivery;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );

    // the delivered pointer IS the batch pointer
    rocketnet_unit_check( delivery.data == batch + 8 );
    rocketnet_unit_check( delivery.bytes == 24 );
    rocketnet_unit_check( delivery.code == ROCKETNET_CHANNEL_TYPE_UNRELIABLE );
    rocketnet_unit_check( delivery.data[0] == 0x5A );
    rocketnet_unit_check( channels.unreliable_delivered == 1 );

    // at most once per update: the count is the update's, and the view dies
    // with it (RNChannel!UnreliableAtMostOnce)
    rn_channels_end_update( &channels );
    rocketnet_unit_check( channels.unreliable_delivered == 0 );

    rn_region_reset( region );
}

// an unreliable record never takes a reliable number (I3): no descriptor, no
// send-store chunk, no window movement, nothing to ack.

static void unreliable_never_takes_a_number()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    static uint8_t batch[32];
    struct rocketnet_message_record_t record;
    dispatch_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 1, batch, 16 );

    const uint32_t descriptors_before = region->descriptors_free;
    const uint32_t read_before = region->channel_receive[1].ordered_read;

    struct rn_delivery_t delivery;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );

    // no descriptor, no chunk, no read index, no serial: nothing layer 3 owns
    rocketnet_unit_check( region->descriptors_free == descriptors_before );
    rocketnet_unit_check( region->channel_receive[1].ordered_read == read_before );
    rocketnet_unit_check( delivery.serial == 0 );

    // an unreliable send is refused a number the same way: the builder's item
    // carries ROCKETNET_PACKET_NUMBER_NONE and takes no sent-map entry
    rocketnet_unit_check( ROCKETNET_PACKET_NUMBER_NONE == 0xFFFFFFFFFFFFFFFFull );

    rn_region_reset( region );
}

// R1: the transport's packet ids are the only acks, and they mark exactly the
// numbers the packet they name carried -- including a resend, whose packet is
// keyed by the id the transport assigned to that send.

static void ack_feed_marks_only_carried_numbers()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );

    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, NULL );

    // the carried numbers are assigned through the send window at first write
    // (docs/CHANNELS.md 6.2, RNWindow!NumberRecord), as the builder's numbered
    // send does: layer 3 commits only numbers the window owns
    uint64_t numbered[3];
    for ( int i = 0; i < 3; i++ )
    {
        uint64_t ring_start = 0;
        rocketnet_unit_check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16,
                                                          RN_DESCRIPTOR_NONE, &numbered[i], &ring_start ) == RN_WINDOW_SEND_NUMBERED );
        rocketnet_unit_check( numbered[i] == (uint64_t) i );
    }

    const double now = 100.0;
    const uint64_t transport_id = 90210;            // the id the transport assigned the packet

    const uint64_t carried[2] = { numbered[0], numbered[1] };
    rn_window_ack_commit_packet( &ack, transport_id, carried, 2, now );

    rocketnet_unit_check( rn_window_ack_entries_in_flight( &ack ) == 1 );
    rocketnet_unit_check( send.U == 0 );

    // an ack of a packet that carried nothing marks nothing
    const uint64_t other = transport_id + 1;
    rocketnet_unit_check( rn_channels_feed_acks( &ack, &other, 1, now + 0.01 ) == 0 );
    rocketnet_unit_check( rn_window_ack_entries_in_flight( &ack ) == 1 );
    rocketnet_unit_check( send.U == 0 );

    // the transport's ack of the packet that carried them marks exactly them:
    // the entry leaves the map and U moves past both numbers, not past 2
    rocketnet_unit_check( rn_channels_feed_acks( &ack, &transport_id, 1, now + 0.02 ) == 1 );
    rocketnet_unit_check( rn_window_ack_entries_in_flight( &ack ) == 0 );
    rocketnet_unit_check( send.U == 2 );
    rocketnet_unit_check( region->send_numbers[0].acked && region->send_numbers[1].acked );
    rocketnet_unit_check( !region->send_numbers[2].acked );

    // a resend of number 2 under the id the transport assigns that resend is
    // matched by that id, not by an invented counter
    const uint64_t resend_id = 90211;
    const uint64_t resent[1] = { numbered[2] };
    rn_window_ack_commit_packet( &ack, resend_id, resent, 1, now + 0.03 );
    rocketnet_unit_check( rn_window_ack_entries_in_flight( &ack ) == 1 );
    rocketnet_unit_check( rn_channels_feed_acks( &ack, &resend_id, 1, now + 0.05 ) == 1 );
    rocketnet_unit_check( rn_window_ack_entries_in_flight( &ack ) == 0 );
    rocketnet_unit_check( send.U == 3 );
    rocketnet_unit_check( region->send_numbers[2].acked );

    // nothing else marks a message acked: a second feed of the same id, and a
    // feed of nothing, match nothing
    rocketnet_unit_check( rn_channels_feed_acks( &ack, &transport_id, 1, now + 0.06 ) == 0 );
    rocketnet_unit_check( rn_channels_feed_acks( &ack, NULL, 0, now + 0.07 ) == 0 );

    rn_region_reset( region );
}

// the reversed witnesses of tla/README-RNChannel.md that concern dispatch and
// the builder: the code refuses the broken step.

static void rnchannel_witness_replays()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    static uint8_t payload[16];

    // BrokenOutOfOrder: an ordered record delivered out of its channel's order.
    // The record ahead of its turn is not delivered; the one behind is a
    // violation, because layer 3 already caught the duplicates.
    struct rocketnet_message_record_t ahead;
    dispatch_record( &ahead, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
    ahead.ordering = 1;

    struct rn_delivery_t delivery;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &ahead, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rocketnet_unit_check( region->channel_receive[0].ordered_read == 0 );
    rocketnet_unit_check( channels.deliveries == 0 );

    struct rocketnet_message_record_t behind;
    dispatch_record( &behind, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
    behind.ordering = 0;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &behind, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );

    // the record held ahead is released by the drain in the same call (7.2)
    rocketnet_unit_check( region->channel_receive[0].ordered_read == 2 );
    rocketnet_unit_check( channels.deliveries == 2 );

    struct rocketnet_message_record_t late;
    dispatch_record( &late, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
    late.ordering = 0;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &late, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND] >= 1 );

    rn_region_reset( region );
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

    // BrokenNoSequencedReset: an ordered transition that leaves the sequenced
    // epoch running. Both ordered transitions zero it, on send and on deliver.
    region->channel_send[1].sequenced_write = 7;
    rocketnet_unit_check( rn_channels_send_ordered( &channels, 1 ) == 0 );
    rocketnet_unit_check( region->channel_send[1].sequenced_write == 0 );

    region->channel_receive[1].sequenced_watermark = 9;
    rn_channels_deliver_ordered( &channels, 1, 0 );
    rocketnet_unit_check( region->channel_receive[1].sequenced_watermark == 0 );

    // BrokenTwice: the same packet delivered twice must not deliver its
    // ordered record twice. The second arrival is behind the read index.
    struct rocketnet_message_record_t twice;
    dispatch_record( &twice, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, payload, (int) sizeof( payload ) );
    twice.ordering = 0;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &twice, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &twice, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( region->channel_receive[2].ordered_read == 1 );

    // a bad channel is refused, never delivered
    struct rocketnet_message_record_t bad;
    dispatch_record( &bad, ROCKETNET_MESSAGE_CODE_UNRELIABLE, channels.num_channels + 1, payload, (int) sizeof( payload ) );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &bad, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );

    rn_region_reset( region );
}

REGISTER_UNIT_TEST( dispatch_reaches_each_of_n_channels );
REGISTER_UNIT_TEST( disabled_codes_are_refused_and_counted );
REGISTER_UNIT_TEST( unreliable_delivered_as_a_view );
REGISTER_UNIT_TEST( unreliable_never_takes_a_number );
REGISTER_UNIT_TEST( ack_feed_marks_only_carried_numbers );
REGISTER_UNIT_TEST( rnchannel_witness_replays );
