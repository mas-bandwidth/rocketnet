/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNChannelTypes!Dispatch, !Drain and !ReceiveAck for code 4.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_unit_registry.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_message_object.h"

#define check rocketnet_unit_check

struct rs_node_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
};

static void rs_init( rs_node_t * node )
{
    memset( node, 0, sizeof( *node ) );
    node->arena = rn_arena_create( NULL, NULL, 1, &node->counters );
    check( node->arena != NULL );
    node->region = rn_arena_region( node->arena, 0 );
    check( rn_ordered_init( &node->ordered, node->region, NULL, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
}

static rocketnet_message_record_t rs_record( int code, uint32_t number, uint32_t ordering, uint32_t sequencing, const uint8_t * data )
{
    rocketnet_message_record_t record = {};
    record.code = code;
    record.number = number;
    record.channel = 0;
    record.ordering = ordering;
    record.sequencing = sequencing;
    record.ring = number * 2;
    record.data = data;
    record.bytes = 1;
    return record;
}

static void test_stale_acked_then_discarded()
{
    rs_node_t receiver;
    rs_init( &receiver );
    const uint8_t fresh = 2, stale = 1;
    rn_delivery_t out;
    rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 1, &fresh );
    uint32_t slot;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( out.data == &fresh && out.descriptor == slot );
    rn_ordered_end_update( &receiver.ordered );
    const uint64_t before = receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE];
    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 1, 0, 0, &stale );
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE] == before + 1 );
    check( receiver.region->receive_descriptors[slot].live == 0 );
    check( receiver.ordered.channels.deliveries == 0 );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_stale_acked_then_discarded );

static void test_shares_the_watermark_with_code_1()
{
    rs_node_t receiver;
    rs_init( &receiver );
    const uint8_t one = 1, four = 4, old = 0;
    rn_delivery_t out;
    rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, &one );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 1, &four );
    uint32_t slot;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( receiver.region->channel_receive[0].sequenced_watermark == 2 );
    record = rs_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, &old );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_STALE );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_shares_the_watermark_with_code_1 );

static void test_reliable_sequenced_c_api_start_finish()
{
    rn_message_link_t link = {};
    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1, NULL, NULL, 0,
                                                               ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    check( object != NULL );
    rocketnet_message_client_t * client = (rocketnet_message_client_t *) object;
    uint8_t * data = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, 3 );
    check( data != NULL );
    if ( data )
    {
        memcpy( data, "rs!", 3 );
        check( rocketnet_message_client_finish( client, 3 ) != 0 );
        const uint32_t d = object->connections[0].region->channel_send[0].queue_tail;    // the head is the connection's CONFIG (CHANNELS.md 10.3)
        check( d != RN_DESCRIPTOR_NONE );
        check( object->connections[0].region->send_descriptors[d].reliability == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED );
    }
    rn_message_object_destroy( object );
}

REGISTER_UNIT_TEST( test_reliable_sequenced_c_api_start_finish );

struct rs_packet_t
{
    uint64_t id;
    int bytes;
    uint8_t bytes_pad[16 - sizeof( uint64_t ) - sizeof( int )];                 // explicit: data starts 16-aligned
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t tail_pad[16 - ROCKETNET_MAX_PACKET_PAYLOAD_BYTES % 16];                     // explicit: the size rounds up to 16
};
static_assert( sizeof( rs_packet_t ) == 16 + ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 15 ) / 16 * 16, "no implicit padding" );
static_assert( alignof( rs_packet_t ) == 16, "data is 16-aligned" );

static void rs_send( rs_node_t * node, int type, const uint8_t * data, uint32_t bytes )
{
    uint8_t * pointer = rn_channels_start( &node->ordered.channels, 0, type, bytes );
    check( pointer != NULL );
    memcpy( pointer, data, bytes );
    check( rn_channels_finish( &node->ordered.channels, bytes ) );
}

static int rs_write( rs_node_t * node, double now, rs_packet_t * packets, int capacity, uint64_t first_id )
{
    alignas( 16 ) uint8_t frames[8][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[8];
    check( capacity > 0 && capacity <= 8 );
    for ( int i = 0; i < capacity; i++ )
        pointers[i] = frames[i];
    const int count = rn_ordered_write( &node->ordered, now, pointers, capacity );
    for ( int i = 0; i < count; i++ )
    {
        packets[i].id = first_id + (uint64_t) i;
        packets[i].bytes = (int) node->ordered.builder.packet[i].payload_bytes;
        check( packets[i].bytes > 0 && packets[i].bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        memcpy( packets[i].data, frames[i], (size_t) packets[i].bytes );
        rn_ordered_commit( &node->ordered, i, packets[i].id, now );
    }
    return count;
}

static void test_newest_delivered_under_loss_and_reorder()
{
    rs_node_t receiver;
    rs_init( &receiver );
    const uint8_t old = 1, newest = 2, ordered = 3;
    rn_delivery_t out;
    rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 0, &old );
    uint32_t slot;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( receiver.region->channel_receive[0].held_sequenced != RN_DESCRIPTOR_NONE );
    check( receiver.region->receive_ring[record.ring * 16] == old );

    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 1, 1, 1, &newest );
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( receiver.region->receive_descriptors[0].live == 0 );

    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 0, 0, &ordered );
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( receiver.ordered.channels.deliveries == 2 );
    check( receiver.region->deliveries[1].code == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED );
    check( receiver.region->deliveries[1].data[0] == newest );
    check( receiver.region->deliveries[1].data >= receiver.region->receive_ring &&
           receiver.region->deliveries[1].data < receiver.region->receive_ring + receiver.region->ring_bytes );
    check( receiver.region->channel_receive[0].sequenced_watermark == 2 );
    rn_ordered_end_update( &receiver.ordered );
    check( receiver.region->receive_descriptors[1].live == 0 );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_newest_delivered_under_loss_and_reorder );

static void test_resent_until_packet_acked()
{
    rs_node_t sender;
    rs_init( &sender );
    const uint8_t state = 9;
    rs_send( &sender, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, &state, 1 );
    check( sender.ordered.send.next == 0 ); // numbering is late
    rs_packet_t first[1], retry[1];
    check( rs_write( &sender, 1.0, first, 1, 11 ) == 1 );
    check( sender.ordered.send.next == 1 );
    check( rs_write( &sender, 10.0, retry, 1, 12 ) == 1 );
    rocketnet_message_reader_t reader;
    rocketnet_message_record_t record;
    check( rocketnet_message_reader_begin( &reader, retry[0].data, retry[0].bytes, &sender.ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rocketnet_message_reader_next( &reader, &record ) );
    check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED && record.number == 0 );
    check( rn_ordered_acks( &sender.ordered, &retry[0].id, 1, 10.01 ) == 1 );
    check( rs_write( &sender, 20.0, retry, 1, 13 ) == 0 );
    rn_arena_destroy( sender.arena );
}

REGISTER_UNIT_TEST( test_resent_until_packet_acked );

static void test_stale_transport_ack_retires_resend_after_discard()
{
    rs_node_t sender, receiver;
    rs_init( &sender );
    rs_init( &receiver );
    const uint8_t old = 1, newest = 2;
    rs_packet_t old_packet[1], new_packet[1], retry[1];
    rs_send( &sender, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, &old, 1 );
    check( rs_write( &sender, 1.0, old_packet, 1, 31 ) == 1 );
    rs_send( &sender, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, &newest, 1 );
    check( rs_write( &sender, 1.01, new_packet, 1, 32 ) == 1 );

    // RNChannelTypes!Dispatch: packet arrival is ACK-worthy even when the
    // application later sees the reliable sequence as stale (study I12).
    check( rn_ordered_receive( &receiver.ordered, new_packet[0].data, new_packet[0].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &receiver.ordered ) == 1 );
    check( receiver.region->deliveries[0].data[0] == newest );
    rn_ordered_end_update( &receiver.ordered );
    const uint64_t stale_before = receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE];
    check( rn_ordered_receive( &receiver.ordered, old_packet[0].data, old_packet[0].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &receiver.ordered ) == 0 );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE] == stale_before + 1 );
    rn_ordered_end_update( &receiver.ordered );

    check( rn_ordered_acks( &sender.ordered, &old_packet[0].id, 1, 1.02 ) == 1 );
    check( rs_write( &sender, 10.0, retry, 1, 33 ) == 1 );
    rocketnet_message_reader_t reader;
    rocketnet_message_record_t record;
    check( rocketnet_message_reader_begin( &reader, retry[0].data, retry[0].bytes, &sender.ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rocketnet_message_reader_next( &reader, &record ) );
    check( record.number == 1 ); // only the newer unacked message resends
    check( rn_ordered_acks( &sender.ordered, &retry[0].id, 1, 10.01 ) == 1 );
    check( rs_write( &sender, 20.0, retry, 1, 34 ) == 0 );
    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_stale_transport_ack_retires_resend_after_discard );

static void test_reliable_sequenced_block_delivers_whole()
{
    rs_node_t sender, receiver;
    rs_init( &sender );
    rs_init( &receiver );
    const uint32_t bytes = 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17;
    uint8_t data[2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17];
    for ( uint32_t i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( i * 7 + 3 );
    rs_send( &sender, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, data, bytes );
    rs_packet_t packets[3];
    check( rs_write( &sender, 1.0, packets, 3, 41 ) == 3 );
    for ( int i = 2; i >= 0; i-- )
    {
        rocketnet_message_reader_t reader;
        rocketnet_message_record_t record;
        check( rocketnet_message_reader_begin( &reader, packets[i].data, packets[i].bytes, &sender.ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( rocketnet_message_reader_next( &reader, &record ) );
        check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED && record.block );
        check( rn_ordered_receive( &receiver.ordered, packets[i].data, packets[i].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( rn_ordered_deliveries( &receiver.ordered ) == ( i == 0 ? 1u : 0u ) );
        if ( i == 0 )
        {
            const rn_delivery_t & delivery = receiver.region->deliveries[0];
            check( delivery.code == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED && delivery.bytes == (int) bytes );
            check( delivery.data >= receiver.region->receive_ring && delivery.data < receiver.region->receive_ring + receiver.region->ring_bytes );
            check( memcmp( delivery.data, data, bytes ) == 0 );
        }
        rn_ordered_end_update( &receiver.ordered );
    }
    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_reliable_sequenced_block_delivers_whole );

static void test_reliable_sequenced_block_ahead_drains_from_ring()
{
    rs_node_t sender, receiver;
    rs_init( &sender );
    rs_init( &receiver );
    const uint8_t ordered = 3;
    uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES + 300];
    for ( uint32_t i = 0; i < sizeof( data ); i++ )
        data[i] = (uint8_t) ( i * 11 + 5 );
    rs_send( &sender, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, &ordered, 1 );
    rs_packet_t packets[3];
    check( rs_write( &sender, 1.0, packets, 1, 61 ) == 1 );
    rs_send( &sender, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, data, (uint32_t) sizeof( data ) );
    check( rs_write( &sender, 1.01, packets + 1, 2, 62 ) == 2 );
    check( rn_ordered_receive( &receiver.ordered, packets[2].data, packets[2].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rn_ordered_end_update( &receiver.ordered );
    check( rn_ordered_receive( &receiver.ordered, packets[1].data, packets[1].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &receiver.ordered ) == 0 );
    rn_ordered_end_update( &receiver.ordered );
    check( rn_ordered_receive( &receiver.ordered, packets[0].data, packets[0].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &receiver.ordered ) == 2 );
    const rn_delivery_t & delivery = receiver.region->deliveries[1];
    check( delivery.code == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED && delivery.bytes == (int) sizeof( data ) );
    check( delivery.data >= receiver.region->receive_ring && delivery.data < receiver.region->receive_ring + receiver.region->ring_bytes );
    check( memcmp( delivery.data, data, sizeof( data ) ) == 0 );
    rn_ordered_end_update( &receiver.ordered );
    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_reliable_sequenced_block_ahead_drains_from_ring );

static void test_newer_state_releases_incomplete_block()
{
    rs_node_t receiver;
    rs_init( &receiver );
    uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES] = {};
    const uint8_t newest = 9;
    rn_delivery_t out;
    uint32_t slot;
    rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 0, chunk );
    record.bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    record.block = 1;
    record.fragment = 0;
    record.fragments = 1;
    record.ring = 0;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( receiver.region->receive_descriptors[0].live != 0 );

    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 2, 0, 1, &newest );
    record.ring = 200;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( out.data == &newest && receiver.region->receive_descriptors[0].live == 0 );
    rn_ordered_end_update( &receiver.ordered );

    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 1, 0, 0, chunk );
    record.bytes = 20;
    record.block = 1;
    record.fragment = 1;
    record.fragments = 1;
    record.ring = ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( receiver.region->receive_descriptors[slot].live == 0 );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_newer_state_releases_incomplete_block );

// RNChannelTypes!Dispatch (I12): a code-4 record stale for its ordered epoch
// is acked by the transport and discarded, and its receive number is released.
// Without the release each stale arrival keeps one descriptor live, and the
// first number W above it collides with that slot, so the window refuses the
// stream after about W numbers. Each epoch here is one fresh code-4 state,
// the ordered record that closes the epoch, then a late code-4 state from the
// closed epoch; more than W + 64 numbers pass through the window.
static void test_stale_numbers_released_across_ordered_epochs()
{
    rs_node_t receiver;
    rs_init( &receiver );
    const uint32_t W = receiver.region->window;
    check( W >= 64 );
    const uint32_t epochs = ( W + 64 ) / 3 + W;
    const uint8_t payload = 5;
    const uint64_t stale_before = receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE];
    uint32_t number = 0;
    uint32_t sequencing = 0;
    for ( uint32_t epoch = 0; epoch < epochs; epoch++ )
    {
        const int codes[3] = { ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                               ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED };
        const int expected[3] = { ROCKETNET_CHANNEL_DISPATCH_DELIVERED, ROCKETNET_CHANNEL_DISPATCH_DELIVERED,
                                  ROCKETNET_CHANNEL_DISPATCH_STALE };
        for ( int i = 0; i < 3; i++ )
        {
            rocketnet_message_record_t record = rs_record( codes[i], number, epoch, codes[i] == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED ? 0 : sequencing++, &payload );
            record.ring = ( number & ( W - 1 ) ) * 2;
            uint32_t slot;
            const int accepted = rn_window_receive_accept( &receiver.ordered.receive, &record, &slot );
            if ( accepted != RN_WINDOW_RECEIVE_ACCEPT )
                printf( "test_stale_numbers_released_across_ordered_epochs: number %u (epoch %u) refused by the window: accept=%d violation=%d\n",
                        number, epoch, accepted, (int) receiver.ordered.receive.violation );
            check( accepted == RN_WINDOW_RECEIVE_ACCEPT );
            rn_delivery_t out;
            check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == expected[i] );
            number++;
        }
        rn_ordered_end_update( &receiver.ordered );
        check( receiver.region->channel_receive[0].ordered_read == epoch + 1 );
    }
    check( number > W + 64 );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE] == stale_before + epochs );
    for ( uint32_t i = 0; i < W; i++ )
        check( receiver.region->receive_descriptors[i].live == 0 );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_stale_numbers_released_across_ordered_epochs );

static void test_reliable_sequenced_drain_refusal_counts_stale()
{
    // The ordered record opens the epoch; the drain refuses the held code-4
    // record's sequencing, releases its number and counts it.
    rs_node_t receiver;
    rs_init( &receiver );
    const uint8_t ahead = 1, ordered = 2;
    rn_delivery_t out;
    uint32_t slot = RN_DESCRIPTOR_NONE, ahead_slot = RN_DESCRIPTOR_NONE;
    rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 0x800000, &ahead );
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &ahead_slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( receiver.region->channel_receive[0].held_sequenced != RN_DESCRIPTOR_NONE );
    const uint64_t stale_before = receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE];
    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 0, 0, &ordered );
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( receiver.ordered.channels.deliveries == 1 );
    check( receiver.region->channel_receive[0].held_sequenced == RN_DESCRIPTOR_NONE );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE] == stale_before + 1 );
    check( receiver.region->receive_descriptors[ahead_slot].live == 0 );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_reliable_sequenced_drain_refusal_counts_stale );

static void test_rnchannel_reliable_sequenced_witness_replays()
{
    // BrokenSequencedBackwards and BrokenHoldAllSequenced are rejected for
    // numbered code 4 as for code 1; the ordered drain resets the watermark.
    test_stale_acked_then_discarded();
    test_stale_transport_ack_retires_resend_after_discard();
    test_newest_delivered_under_loss_and_reorder();
    test_shares_the_watermark_with_code_1();
}

REGISTER_UNIT_TEST( test_rnchannel_reliable_sequenced_witness_replays );

static uint64_t rs_allocations;

static void * rs_counted_malloc( void *, size_t bytes )
{
    rs_allocations++;
    return malloc( bytes );
}

static void rs_counted_free( void *, void * memory )
{
    rs_allocations++;
    free( memory );
}

static void test_reliable_sequenced_no_allocation_after_create()
{
    rocketnet_term();
    check( rocketnet_init( NULL, rs_counted_malloc, rs_counted_free ) == ROCKETNET_STATUS_OK );
    rs_node_t sender, receiver;
    rs_init( &sender );
    rs_init( &receiver );
    const uint64_t created = rs_allocations;
    const uint8_t state = 7;
    rs_send( &sender, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, &state, 1 );
    rs_packet_t packet;
    check( rs_write( &sender, 1.0, &packet, 1, 91 ) == 1 );
    check( rn_ordered_receive( &receiver.ordered, packet.data, packet.bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( receiver.region->deliveries[0].data >= packet.data &&
           receiver.region->deliveries[0].data < packet.data + packet.bytes );
    rn_ordered_end_update( &receiver.ordered );
    check( rn_ordered_acks( &sender.ordered, &packet.id, 1, 1.01 ) == 1 );
    check( rs_allocations == created );
    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_reliable_sequenced_no_allocation_after_create );

// A refused record dispatches nothing, raises its own refusal counter by one,
// sets the sticky protocol violation and counts it once for the channel set.
static void rs_check_refused( rs_node_t * node, const rocketnet_message_record_t * record, int counter )
{
    const uint64_t before = node->counters.values[counter];
    const uint64_t violations_before = node->counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION];
    rn_delivery_t out;
    check( rn_channels_dispatch( &node->ordered.channels, record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node->counters.values[counter] == before + 1 );
    if ( counter != ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION )
        check( node->counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == violations_before + 1 );
    check( node->ordered.channels.deliveries == 0 );
    check( node->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
}

// RNChannelTypes!Dispatch: a sequenced record that names a channel the set
// does not have, an ordering epoch at or beyond the window, a reliable number
// the window never accepted, a ring span outside the receive ring, or a block
// chunk of the wrong size is a wire violation (17.3 item 11). Implemented by
// rn_channels_dispatch_sequenced and rn_sequenced_block in
// sdk/rocketnet_channel_sequenced.cpp; each case uses a fresh node because the
// violation is counted once per channel set.
static void test_hostile_sequenced_records_are_refused()
{
    const uint8_t payload = 7;
    uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES] = {};
    uint32_t slot;

    // (a) BAD_CHANNEL: code 1 on a channel past num_channels.
    {
        rs_node_t node;
        rs_init( &node );
        rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, &payload );
        record.channel = node.ordered.channels.num_channels;
        rs_check_refused( &node, &record, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );
        rn_arena_destroy( node.arena );
    }

    // (b) ORDERING_AHEAD: code 1 whose ordering is window epochs ahead.
    {
        rs_node_t node;
        rs_init( &node );
        rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, &payload );
        record.ordering = node.region->window;
        rs_check_refused( &node, &record, ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD );
        rn_arena_destroy( node.arena );
    }

    // (c) PROTOCOL_VIOLATION: code 4 number 5 that the window never accepted.
    {
        rs_node_t node;
        rs_init( &node );
        rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 5, 0, 0, &payload );
        rs_check_refused( &node, &record, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
        rn_arena_destroy( node.arena );
    }

    // (d) BAD_RING: a future code-4 state whose ring offset is the ring's end.
    {
        rs_node_t node;
        rs_init( &node );
        rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 0, &payload );
        record.ring = (uint32_t) ( node.region->ring_bytes / 16 );
        check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        rs_check_refused( &node, &record, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        rn_arena_destroy( node.arena );
    }

    // (e) BAD_BLOCK: a non-last chunk shorter than ROCKETNET_MESSAGE_FRAGMENT_BYTES.
    {
        rs_node_t node;
        rs_init( &node );
        rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 0, chunk );
        record.block = 1;
        record.fragment = 0;
        record.fragments = 1;
        record.bytes = 100;
        record.ring = 0;
        check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        rs_check_refused( &node, &record, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        rn_arena_destroy( node.arena );
    }
}

REGISTER_UNIT_TEST( test_hostile_sequenced_records_are_refused );

// RNChannelTypes!Dispatch: an in-turn code-4 state whose sequencing equals the
// held block's is not newer, so it is superseded: discarded, its number
// released, and the held block kept. Implemented by the delta == 0 branch of
// rn_channels_dispatch_sequenced in sdk/rocketnet_channel_sequenced.cpp.
static void test_equal_sequencing_at_a_held_block_is_superseded()
{
    rs_node_t receiver;
    rs_init( &receiver );
    uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES] = {};
    const uint8_t equal = 9;
    rn_delivery_t out;
    uint32_t slot;
    rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 0, chunk );
    record.bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    record.block = 1;
    record.fragment = 0;
    record.fragments = 1;
    record.ring = 0;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( receiver.region->receive_descriptors[0].live != 0 );

    const uint64_t superseded_before = receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED];
    record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 2, 0, 0, &equal );
    record.ring = 200;
    check( rn_window_receive_accept( &receiver.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_channels_dispatch( &receiver.ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED] == superseded_before + 1 );
    check( receiver.region->receive_descriptors[slot].live == 0 );
    check( receiver.region->receive_descriptors[0].live != 0 );
    check( receiver.ordered.channels.deliveries == 0 );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_equal_sequencing_at_a_held_block_is_superseded );

// RNChannelTypes!InTurnOneRule (tla/MCRNChannelTypesBlocks.cfg): an in-turn
// code-4 block follows the record rule. It is checked against the watermark
// and the in-turn block only, never against the held future state, and it is
// delivered as soon as its last fragment arrives. The earlier code collected
// it in the future-state slot (tla/MCRNChannelBrokenBlockInTurnHeld.cfg), so
// a held future state discarded it (first receiver) and a future state that
// arrived while it collected released it (second receiver).
static rocketnet_message_record_t rs_fragment( uint32_t number, uint32_t fragment, const uint8_t * data )
{
    rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, number, 0, 0, data );
    record.block = 1;
    record.fragment = fragment;
    record.fragments = 1;
    record.ring = fragment * ( ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16 );
    record.bytes = fragment == 0 ? ROCKETNET_MESSAGE_FRAGMENT_BYTES : 20;
    return record;
}

static int rs_arrive( rs_node_t * node, rocketnet_message_record_t * record, rn_delivery_t * out )
{
    uint32_t slot;
    check( rn_window_receive_accept( &node->ordered.receive, record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    return rn_channels_dispatch( &node->ordered.channels, record, out );
}

static void test_sequenced_in_turn_block_follows_record_rule()
{
    uint8_t block[ROCKETNET_MESSAGE_FRAGMENT_BYTES + 20];
    for ( uint32_t i = 0; i < sizeof( block ); i++ )
        block[i] = (uint8_t) ( i * 13 + 1 );
    const uint8_t future = 7, record_state = 8;
    rn_delivery_t out;

    // a future state is held, then the in-turn block arrives
    {
        rs_node_t receiver;
        rs_init( &receiver );
        rocketnet_message_record_t record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 2, 1, 0, &future );
        record.ring = 200;
        check( rs_arrive( &receiver, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

        record = rs_fragment( 0, 0, block );
        check( rs_arrive( &receiver, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        record = rs_fragment( 1, 1, block + ROCKETNET_MESSAGE_FRAGMENT_BYTES );
        check( rs_arrive( &receiver, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        check( out.code == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED && out.bytes == (int) sizeof( block ) );
        check( memcmp( out.data, block, sizeof( block ) ) == 0 );
        check( receiver.region->channel_receive[0].sequenced_watermark == 1 );

        // an in-turn record in the same state is delivered by the same rule
        record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 3, 0, 1, &record_state );
        record.ring = 210;
        check( rs_arrive( &receiver, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        check( out.data == &record_state );
        check( receiver.region->channel_receive[0].held_sequenced != RN_DESCRIPTOR_NONE );
        check( receiver.region->receive_descriptors[2].live != 0 );
        rn_ordered_end_update( &receiver.ordered );
        rn_arena_destroy( receiver.arena );
    }

    // the in-turn block is collecting when a future state arrives
    {
        rs_node_t receiver;
        rs_init( &receiver );
        const uint64_t superseded_before = receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED];
        rocketnet_message_record_t record = rs_fragment( 0, 0, block );
        check( rs_arrive( &receiver, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

        record = rs_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 2, 1, 0, &future );
        record.ring = 200;
        check( rs_arrive( &receiver, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        check( receiver.region->receive_descriptors[0].live != 0 );

        record = rs_fragment( 1, 1, block + ROCKETNET_MESSAGE_FRAGMENT_BYTES );
        check( rs_arrive( &receiver, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        check( out.bytes == (int) sizeof( block ) && memcmp( out.data, block, sizeof( block ) ) == 0 );
        check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED] == superseded_before );
        check( receiver.region->channel_receive[0].held_sequenced != RN_DESCRIPTOR_NONE );
        rn_ordered_end_update( &receiver.ordered );
        rn_arena_destroy( receiver.arena );
    }
}

REGISTER_UNIT_TEST( test_sequenced_in_turn_block_follows_record_rule );
