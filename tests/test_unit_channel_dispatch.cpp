/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer 4's dispatch, unit tier (docs/CHANNELS.md 7; tla/RNChannel.tla). No
// sockets, no threads, no wall clock: time is an argument.

#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_packet_builder.h"
#include "../sdk/rocketnet_window_ack.h"
#include "../sdk/rocketnet_window_send.h"

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

static void test_dispatch_reaches_each_of_n_channels()
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

// Every type outside the current send mask is refused and counted. Later
// type cards may add bits without weakening refusal for the remaining types.

static void test_disabled_codes_are_refused_and_counted()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    rocketnet_unit_check( channels.enabled_types == ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION );

    uint64_t refused = 0;
    for ( int type = 0; type < ROCKETNET_CHANNEL_NUM_TYPES; type++ )
    {
        const bool enabled = ( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION & ( 1u << type ) ) != 0;
        rocketnet_unit_check( rn_channels_send_check( &channels, type, 0 ) ==
                              ( enabled ? ROCKETNET_CHANNEL_SEND_OK : ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED ) );
        if ( !enabled )
            refused++;
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED] == refused );
    }
    rocketnet_unit_check( ( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION & ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT ) ) != 0 );
    rocketnet_unit_check( ( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION & ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT ) ) != 0 );

    // Wire codes still outside the enabled mask are refused on receive.
    static uint8_t payload[8];
    const int codes[3] = { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, ROCKETNET_MESSAGE_CODE_RELIABLE, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED };
    uint64_t refused_wire = 0;
    for ( int i = 0; i < 3; i++ )
    {
        if ( rn_channels_enabled_codes( &channels ) & ( 1u << codes[i] ) )
            continue;
        struct rocketnet_message_record_t record;
        dispatch_record( &record, codes[i], 0, payload, (int) sizeof( payload ) );

        struct rn_delivery_t delivery;
        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        refused_wire++;
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED] == refused_wire );
    }

    // a wire code 5, 6 or 7 is a violation: the receipt types travel as 0, 2 and 3
    struct rocketnet_message_record_t receipt;
    dispatch_record( &receipt, 5, 0, payload, (int) sizeof( payload ) );
    struct rn_delivery_t delivery;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &receipt, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED] == refused_wire + 1 );

    // the enabled codes the reader checks against carry the mask (7.5)
    rocketnet_unit_check( rn_channels_enabled_codes( &channels ) == rocketnet_message_enabled_codes( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION ) );

    rn_region_reset( region );
}

// an unreliable record is delivered as a view into the transport's batch: no
// copy, pointer identity (R3, THE COPIES (b)).

static void test_unreliable_delivered_as_a_view()
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

// the unreliable delivery budget Du (6.3 step 4): once an update has delivered
// its Du records the rest are dropped and counted, which unreliable semantics
// allow, so it is neither a delivery nor a violation. The next update starts
// the count again.

static void test_unreliable_delivery_budget_drops_and_counts()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );
    memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint32_t budget = region->delivery_entries - region->window;      // Du
    rocketnet_unit_check( budget >= 2 );

    static uint8_t batch[8] = { 'a', 'b', 'c', 0, 0, 0, 0, 0 };
    struct rocketnet_message_record_t record;
    dispatch_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, batch, 3 );

    struct rn_delivery_t delivery;
    for ( uint32_t i = 0; i < budget; i++ )
        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( channels.unreliable_delivered == budget );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL] == 0 );

    for ( int i = 0; i < 3; i++ )
        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( channels.unreliable_delivered == budget );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL] == 3 );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );
    rocketnet_unit_check( channels.error == ROCKETNET_DISCONNECT_REASON_NONE );

    // the budget is per update
    rn_channels_end_update( &channels );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL] == 3 );

    rn_region_reset( region );
}

// an unreliable record never takes a reliable number (I3): no descriptor, no
// send-store chunk, no window movement, nothing to ack.

static void test_unreliable_never_takes_a_number()
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

static void test_ack_feed_marks_only_carried_numbers()
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

static void test_rnchannel_witness_replays()
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

// The bad-ring, collision and double-ordering refusals of
// rn_channels_dispatch_ordered (sdk/rocketnet_channel.cpp), each pinned by a
// refusal: a second record in a live descriptor slot under another number
// (PROTOCOL_VIOLATION), an out-of-ring span (BAD_RING), two messages sharing
// one ordering index (PROTOCOL_VIOLATION), and a block holding an in-turn
// ordering (PROTOCOL_VIOLATION).

static void test_ordered_dispatch_refuses_collisions_and_bad_rings()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    static uint8_t payload[16];
    memset( payload, 0xAB, sizeof( payload ) );

    // (a) a number in a slot another number holds: the same descriptor slot, a
    // different number, is a protocol violation.
    {
        rn_region_reset( region );
        memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

        rn_channels_t channels;
        rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

        struct rocketnet_message_record_t held;
        dispatch_record( &held, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
        held.ordering = 1;
        held.number = 1;

        struct rn_delivery_t delivery;
        rocketnet_unit_check( rn_channels_dispatch( &channels, &held, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );

        struct rocketnet_message_record_t collision;
        dispatch_record( &collision, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
        collision.ordering = 2;
        collision.number = 1 + region->window;
        collision.ring = 4;

        rocketnet_unit_check( rn_channels_dispatch( &channels, &collision, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
        rocketnet_unit_check( channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        rocketnet_unit_check( region->channel_receive[0].ordered_read == 0 );
    }

    // (b) an out-of-ring span is a bad ring.
    {
        rn_region_reset( region );
        memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

        rn_channels_t channels;
        rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

        struct rocketnet_message_record_t record;
        dispatch_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
        record.ordering = 1;
        record.number = 1;
        record.ring = (uint32_t) ( region->ring_bytes / 16 );

        struct rn_delivery_t delivery;
        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_RING] == 1 );
        rocketnet_unit_check( channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        rocketnet_unit_check( region->channel_receive[0].ordered_read == 0 );
    }

    // (c) two messages with one ordering index are refused.
    {
        rn_region_reset( region );
        memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

        rn_channels_t channels;
        rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

        struct rocketnet_message_record_t held;
        dispatch_record( &held, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
        held.ordering = 1;
        held.number = 1;

        struct rn_delivery_t delivery;
        rocketnet_unit_check( rn_channels_dispatch( &channels, &held, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );

        struct rocketnet_message_record_t twice;
        dispatch_record( &twice, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
        twice.ordering = 1;
        twice.number = 2;
        twice.ring = 4;

        rocketnet_unit_check( rn_channels_dispatch( &channels, &twice, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
        rocketnet_unit_check( channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        rocketnet_unit_check( region->channel_receive[0].ordered_read == 0 );
    }

    // (d) a block collecting chunks holds its ordering: an in-turn small record
    // on that ordering is refused.
    {
        const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        static uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
        memset( chunk, 0xCD, sizeof( chunk ) );

        rn_region_reset( region );
        memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

        rn_channels_t channels;
        rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

        struct rocketnet_message_record_t block;
        memset( &block, 0, sizeof( block ) );
        block.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        block.block = 1;
        block.channel = 0;
        block.number = 0;
        block.ordering = 0;
        block.fragment = 0;
        block.fragments = 1;
        block.ring = 0;
        block.data = chunk;
        block.bytes = (int) F;

        struct rn_delivery_t delivery;
        rocketnet_unit_check( rn_channels_dispatch( &channels, &block, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );

        struct rocketnet_message_record_t small;
        dispatch_record( &small, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, payload, (int) sizeof( payload ) );
        small.ordering = 0;
        small.number = 5;

        rocketnet_unit_check( rn_channels_dispatch( &channels, &small, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
        rocketnet_unit_check( channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        rocketnet_unit_check( region->channel_receive[0].ordered_read == 0 );
    }

    rn_region_reset( region );
}

// The first version's mask enables all eight types; a caller's narrower mask
// leaves out what it does not enable, counted once per refused send (7.5).

static void test_default_mask_enables_all_eight_types()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    rocketnet_unit_check( channels.enabled_types == (uint32_t) 0xFF );

    for ( int type = 0; type < ROCKETNET_CHANNEL_NUM_TYPES; type++ )
        rocketnet_unit_check( rn_channels_send_check( &channels, type, 0 ) ==
                              ROCKETNET_CHANNEL_SEND_OK );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED] == 0 );

    rn_channels_t narrow;
    rocketnet_unit_check( rn_channels_init( &narrow, region, region->num_channels,
                                           1u << ROCKETNET_CHANNEL_TYPE_UNRELIABLE ) );
    rocketnet_unit_check( rn_channels_send_check( &narrow, ROCKETNET_CHANNEL_TYPE_RELIABLE, 0 ) ==
                          ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED] == 1 );

    rn_region_reset( region );
}

// A block whose numbers cross 2^24 is released whole at the end of the update:
// every number of the update's deliveries is free after it (tla/RNChannel.tla
// EndUpdate). Fragments two and three of a block starting at 0xFFFFFE carry the
// wire numbers 0 and 1, and a stale descriptor there refuses the next record in
// the same slot as MESSAGE_SLOT_COLLISION.

static void test_channel_release_block_across_u24_wrap()
{
    rn_region_t * region = dispatch_region();
    rocketnet_unit_check( region != NULL );

    rn_region_reset( region );
    memset( &dispatch_counters, 0, sizeof( dispatch_counters ) );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    const uint32_t window = region->window;

    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t first_number = 0xFFFFFE;
    static uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    memset( chunk, 0x5C, sizeof( chunk ) );

    struct rn_delivery_t delivery;
    for ( uint32_t i = 0; i < 4; i++ )
    {
        struct rocketnet_message_record_t block;
        memset( &block, 0, sizeof( block ) );
        block.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        block.block = 1;
        block.channel = 0;
        block.number = ( first_number + i ) & 0xFFFFFFu;
        block.ordering = 0;
        block.fragment = i;
        block.fragments = 3;
        block.ring = i * ( F / 16 );
        block.data = chunk;
        block.bytes = (int) F;

        const int result = rn_channels_dispatch( &channels, &block, &delivery );
        rocketnet_unit_check( result == ( i + 1 == 4 ? ROCKETNET_CHANNEL_DISPATCH_DELIVERED : ROCKETNET_CHANNEL_DISPATCH_HELD ) );
    }
    rocketnet_unit_check( channels.deliveries == 1 );

    rn_channels_end_update( &channels );
    rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION] == 0 );

    // the wrapped numbers 0 and 1 are free with the rest of the block: a new
    // number one window later, in each of their slots, is not a collision
    rocketnet_unit_check( !region->receive_descriptors[0].live );
    rocketnet_unit_check( !region->receive_descriptors[1].live );

    for ( uint32_t i = 0; i < 2; i++ )
    {
        struct rocketnet_message_record_t record;
        dispatch_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, chunk, (int) sizeof( chunk ) );
        record.number = window + i;
        record.ordering = 1 + i;

        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION] == 0 );
    }

    rn_channels_end_update( &channels );

    // and the numbers the block's wrapped fragments carried dispatch anew
    for ( uint32_t i = 0; i < 2; i++ )
    {
        struct rocketnet_message_record_t record;
        dispatch_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, chunk, (int) sizeof( chunk ) );
        record.number = i;
        record.ordering = 3 + i;

        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        rocketnet_unit_check( dispatch_counters.values[ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION] == 0 );
    }

    rn_region_reset( region );
}

REGISTER_UNIT_TEST( test_channel_release_block_across_u24_wrap );
REGISTER_UNIT_TEST( test_dispatch_reaches_each_of_n_channels );
REGISTER_UNIT_TEST( test_disabled_codes_are_refused_and_counted );
REGISTER_UNIT_TEST( test_unreliable_delivered_as_a_view );
REGISTER_UNIT_TEST( test_unreliable_never_takes_a_number );
REGISTER_UNIT_TEST( test_ack_feed_marks_only_carried_numbers );
REGISTER_UNIT_TEST( test_rnchannel_witness_replays );
REGISTER_UNIT_TEST( test_unreliable_delivery_budget_drops_and_counts );
REGISTER_UNIT_TEST( test_default_mask_enables_all_eight_types );
REGISTER_UNIT_TEST( test_ordered_dispatch_refuses_collisions_and_bad_rings );

static rn_arena_t * schedule_arena( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->client_send_kbps = 0;
    config->server_send_kbps = 0;
    return rn_arena_create( NULL, config, 1, NULL );
}

static bool schedule_enqueue( rn_channels_t * channels, uint32_t channel, uint32_t bytes )
{
    uint8_t * data = rn_channels_start( channels, channel, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, bytes );
    if ( !data )
        return false;
    memset( data, 0x5a, bytes );
    return rn_channels_finish( channels, bytes );
}

static uint32_t schedule_walk( const rn_channels_t * channels, uint32_t channel, uint32_t * out, uint32_t max_out )
{
    // The flat FIFO walk: live descriptors of this channel in enqueue_ticket order.
    const rn_region_t * region = channels->region;
    uint32_t n = 0;
    uint32_t after = 0;
    while ( n < max_out )
    {
        uint32_t best = RN_DESCRIPTOR_NONE;
        uint32_t best_ticket = 0;
        for ( uint32_t d = 0; d < region->descriptors_fresh; d++ )
        {
            const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
            if ( !rn_descriptor_schedule_live( descriptor ) || descriptor->channel != channel || descriptor->enqueue_ticket <= after )
                continue;
            if ( best == RN_DESCRIPTOR_NONE || descriptor->enqueue_ticket < best_ticket )
            {
                best = d;
                best_ticket = descriptor->enqueue_ticket;
            }
        }
        if ( best == RN_DESCRIPTOR_NONE )
            break;
        after = best_ticket;
        out[n++] = best;
    }
    return n;
}

static void test_channel_queue_fill_cancel_and_reuse()
{
    rocketnet_message_config_t config;
    rn_arena_t * arena = schedule_arena( &config );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    uint32_t filled = 0;
    while ( schedule_enqueue( &channels, 0, 16 ) )
        filled++;
    rocketnet_unit_check( filled == region->queue );
    rocketnet_unit_check( rn_region_exhausted( region ) );
    rn_arena_destroy( arena );

    arena = schedule_arena( &config );
    region = rn_arena_region( arena, 0 );
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    for ( int i = 0; i < 4; i++ )
        rocketnet_unit_check( schedule_enqueue( &channels, 0, 16 ) );
    uint32_t live[4];
    rocketnet_unit_check( schedule_walk( &channels, 0, live, 4 ) == 4 );
    const uint32_t generation = region->send_descriptors[live[1]].generation;
    const uint32_t epoch = region->schedule_epoch;
    uint32_t oracle[4];
    for ( int i = 0; i < 4; i++ )
        oracle[i] = region->send_descriptors[live[i]].ordering;
    rocketnet_unit_check( rn_channels_cancel_queued( &channels, 0, live[1], generation + 1, epoch ) == false );
    rocketnet_unit_check( rn_channels_cancel_queued( &channels, 0, live[1], generation, epoch + 1 ) == false );
    rocketnet_unit_check( rn_channels_cancel_queued( &channels, 0, live[1], generation, epoch ) == true );
    rn_channels_compact_queues( &channels );
    uint32_t after[4];
    const uint32_t n = schedule_walk( &channels, 0, after, 4 );
    rocketnet_unit_check( n == 3 );
    rocketnet_unit_check( region->send_descriptors[after[0]].ordering == oracle[0] );
    rocketnet_unit_check( region->send_descriptors[after[1]].ordering == oracle[2] );
    rocketnet_unit_check( region->send_descriptors[after[2]].ordering == oracle[3] );
    uint8_t * reused = rn_channels_start( &channels, 1, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 16 );
    rocketnet_unit_check( reused != NULL );
    const uint32_t reused_index = channels.pending;
    const uint32_t reused_generation = region->send_descriptors[reused_index].generation;
    if ( reused_index == live[1] )
        rocketnet_unit_check( reused_generation != generation );
    rocketnet_unit_check( rn_channels_finish( &channels, 16 ) );
    rocketnet_unit_check( rn_channels_cancel_queued( &channels, 1, reused_index, reused_generation + 1, epoch ) == false );
    rn_arena_destroy( arena );
}

static void test_channel_config_front_ready_rotation_and_work()
{
    rocketnet_message_config_t config;
    rn_arena_t * arena = schedule_arena( &config );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    config.packets_per_update = 1;
    rn_ordered_t ordered;
    rocketnet_unit_check( rn_ordered_init( &ordered, region, &config, NULL, 1, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    rn_channels_t * channels = &ordered.channels;
    channels->payload_capacity = 40;
    rocketnet_unit_check( schedule_enqueue( channels, 0, 16 ) );
    const uint32_t before = channels->schedule_work;
    rocketnet_unit_check( rn_ordered_announce_config( &ordered, &config ) );
    const rn_send_descriptor_t * head = &region->send_descriptors[region->channel_send[0].queue_head];
    rocketnet_unit_check( ( head->flags & RN_SEND_FLAG_CONTROL ) != 0 );
    uint32_t front_order[2];
    rocketnet_unit_check( schedule_walk( channels, 0, front_order, 2 ) == 2 );
    rocketnet_unit_check( front_order[0] == region->channel_send[0].queue_head );
    rocketnet_unit_check( head->enqueue_ticket < region->send_descriptors[front_order[1]].enqueue_ticket );
    rocketnet_unit_check( channels->schedule_work == before + 2 );
    rocketnet_unit_check( channels->schedule_work < region->queue );
    rocketnet_unit_check( channels->ready_head == 0 );

    rocketnet_unit_check( schedule_enqueue( channels, 0, 16 ) );
    rocketnet_unit_check( schedule_enqueue( channels, 1, 16 ) );
    rocketnet_unit_check( schedule_enqueue( channels, 1, 16 ) );
    const uint32_t ticket0 = region->channel_send[0].ready_ticket;
    const uint32_t ticket1 = region->channel_send[1].ready_ticket;
    rocketnet_unit_check( ticket0 != 0 && ticket1 != 0 && ticket0 != ticket1 );
    rocketnet_unit_check( channels->ready_head == 0 && channels->ready_tail == 1 );
    rocketnet_unit_check( region->channel_send[0].ready_ticket < region->channel_send[1].ready_ticket );
    alignas( 16 ) uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * payloads[1] = { frame };
    rn_ordered_write( &ordered, 1000.0, payloads, 1 );
    rocketnet_unit_check( channels->ready_head == 1 );
    rocketnet_unit_check( channels->ready_tail == 0 );
    rocketnet_unit_check( region->channel_send[0].ready_ticket != region->channel_send[1].ready_ticket );
    rocketnet_unit_check( region->channel_send[0].ready_ticket != 0 && region->channel_send[1].ready_ticket != 0 );
    rn_arena_destroy( arena );

    arena = schedule_arena( &config );
    region = rn_arena_region( arena, 0 );
    rocketnet_unit_check( rn_channels_init( &ordered.channels, region, region->num_channels, 0 ) );
    channels = &ordered.channels;
    region->channel_send[0].ordered_write = 0xFFFFFEu;
    rocketnet_unit_check( schedule_enqueue( channels, 0, 16 ) );
    rocketnet_unit_check( schedule_enqueue( channels, 0, 16 ) );
    uint32_t wrapped[2];
    rocketnet_unit_check( schedule_walk( channels, 0, wrapped, 2 ) == 2 );
    rocketnet_unit_check( region->send_descriptors[wrapped[0]].ordering == 0xFFFFFEu );
    rocketnet_unit_check( region->send_descriptors[wrapped[1]].ordering == 0xFFFFFFu );
    rocketnet_unit_check( ( region->send_descriptors[wrapped[0]].ordering & 0xFFFFFFu ) == 0xFFFFFEu );
    rocketnet_unit_check( ( region->send_descriptors[wrapped[1]].ordering & 0xFFFFFFu ) == 0xFFFFFFu );
    rocketnet_unit_check( region->send_descriptors[wrapped[0]].enqueue_ticket < region->send_descriptors[wrapped[1]].enqueue_ticket );
    // ordered_write is the full counter. The wire value is its low 24 bits (rn_u24).
    rocketnet_unit_check( region->channel_send[0].ordered_write == 0x1000000u );
    rocketnet_unit_check( schedule_enqueue( channels, 0, 16 ) );
    uint32_t wrapped_next[3];
    rocketnet_unit_check( schedule_walk( channels, 0, wrapped_next, 3 ) == 3 );
    rocketnet_unit_check( region->send_descriptors[wrapped_next[2]].ordering == 0x1000000u );
    rocketnet_unit_check( ( region->send_descriptors[wrapped_next[2]].ordering & 0xFFFFFFu ) == 0 );
    rocketnet_unit_check( region->channel_send[0].ordered_write == 0x1000001u );
    rn_arena_destroy( arena );

    arena = schedule_arena( &config );
    region = rn_arena_region( arena, 0 );
    rocketnet_unit_check( rn_channels_init( &ordered.channels, region, region->num_channels, 0 ) );
    channels = &ordered.channels;
    const uint32_t queued = 80;
    for ( uint32_t i = 0; i < queued; i++ )
        rocketnet_unit_check( schedule_enqueue( channels, 0, 16 ) );
    uint32_t all[80];
    rocketnet_unit_check( schedule_walk( channels, 0, all, 80 ) == queued );
    channels->schedule_work = 0;
    for ( uint32_t i = 1; i <= 64; i++ )
        rocketnet_unit_check( rn_channels_cancel_queued( channels, 0, all[i], region->send_descriptors[all[i]].generation, region->schedule_epoch ) );
    rocketnet_unit_check( channels->schedule_work == 0 );
    rocketnet_unit_check( rn_channels_cancel_queued( channels, 0, all[65], region->send_descriptors[all[65]].generation, region->schedule_epoch ) );
    rocketnet_unit_check( channels->schedule_work == queued );
    rocketnet_unit_check( channels->schedule_work < region->num_channels * region->queue );
    rocketnet_unit_check( channels->schedule_work < region->queue * region->queue );
    rn_arena_destroy( arena );
}

REGISTER_UNIT_TEST( test_channel_queue_fill_cancel_and_reuse );
REGISTER_UNIT_TEST( test_channel_config_front_ready_rotation_and_work );
