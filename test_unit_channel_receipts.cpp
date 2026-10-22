/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNReceipts Send: all three receipt types are accepted by the send mask.

#include "test_unit_registry.h"
#include "test/plan_rss.h"
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_message_object.h"
#include "sdk/rocketnet_send_budget.h"
#include "sdk/rocketnet_window_send.h"
#include "sdk/rocketnet_window_ack.h"

#include <stdlib.h>
#include <string.h>

struct receipt_fixture_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_window_send_t send;
    rn_window_ack_t ack;
};

static void receipt_fixture_init( receipt_fixture_t * f, int ring = 16 )
{
    memset( f, 0, sizeof( *f ) );
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.max_message_receipts = ring;
    config.packets_in_flight = 16;
    f->arena = rn_arena_create( NULL, &config, 1, &f->counters );
    rocketnet_unit_check( f->arena != NULL );
    f->region = rn_arena_region( f->arena, 0 );
    rn_window_send_init( &f->send, f->region, 0 );
    rn_window_ack_init( &f->ack, &f->send, f->region, &config );
}

static void receipt_fixture_destroy( receipt_fixture_t * f )
{
    rn_arena_destroy( f->arena );
}

static int receipt_list( receipt_fixture_t * f, rocketnet_message_receipt_t * out, int capacity )
{
    rn_window_ack_receipt_end_update( &f->ack );
    return rn_window_ack_receipts( &f->ack, out, capacity );
}

static void test_receipts_report_available_without_overwriting_capacity()
{
    receipt_fixture_t f;
    receipt_fixture_init( &f );
    rn_window_ack_receipt_begin_update( &f.ack );
    const uint32_t serials[3] = { 11, 12, 13 };
    rn_window_ack_commit_receipt_packet( &f.ack, 101, serials, 3, 1000.0 );
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 101, 1000.01 ) == 1 );
    rocketnet_message_receipt_t out[3];
    memset( out, 0, sizeof( out ) );
    rocketnet_unit_check( receipt_list( &f, out, 1 ) == 3 );
    rocketnet_unit_check( out[0].serial == 11 && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rocketnet_unit_check( out[1].serial == 0 && out[2].serial == 0 );
    rocketnet_unit_check( receipt_list( &f, NULL, 0 ) == 3 );
    rocketnet_unit_check( receipt_list( &f, out, 0 ) == 3 );
    rocketnet_unit_check( receipt_list( &f, out, -1 ) == 0 );
    rocketnet_unit_check( receipt_list( &f, NULL, 1 ) == 0 );
    rocketnet_unit_check( out[1].serial == 0 && out[2].serial == 0 );
    rocketnet_unit_check( receipt_list( &f, out, 3 ) == 3 );
    for ( int i = 0; i < 3; i++ )
        rocketnet_unit_check( out[i].serial == serials[i] && out[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rn_window_ack_receipt_begin_update( &f.ack );
    rocketnet_unit_check( receipt_list( &f, NULL, 0 ) == 0 );
    receipt_fixture_destroy( &f );
}
REGISTER_UNIT_TEST( test_receipts_report_available_without_overwriting_capacity );

static void test_unreliable_receipt_acked_or_loss_exactly_once()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, NULL );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_UNRELIABLE_WITH_RECEIPT, 0 ) == ROCKETNET_CHANNEL_SEND_OK );
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT, 0 ) == ROCKETNET_CHANNEL_SEND_OK );
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT, 0 ) == ROCKETNET_CHANNEL_SEND_OK );
    rn_arena_destroy( arena );

    receipt_fixture_t f;
    receipt_fixture_init( &f );
    rn_window_ack_receipt_begin_update( &f.ack );
    const uint32_t serials[2] = { 11, 12 };
    rn_window_ack_commit_receipt_packet( &f.ack, 101, serials, 2, 1.0 ); // RNReceipts WritePacket
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 101, 1.01 ) == 1 ); // ReceiveAck
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 101, 1.02 ) == 0 );
    rn_window_ack_receipt_sweep( &f.ack, 10.0 );
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 2 );
    rocketnet_unit_check( out[0].serial == 11 && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rocketnet_unit_check( out[1].serial == 12 && out[1].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 2 );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    rn_window_ack_receipt_begin_update( &f.ack );
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 0 );
    receipt_fixture_destroy( &f );
}

static void test_unreliable_receipt_loss_at_deadline_sweep()
{
    receipt_fixture_t f;
    receipt_fixture_init( &f );
    rn_window_ack_receipt_begin_update( &f.ack );
    const uint32_t serial = 21;
    rn_window_ack_commit_receipt_packet( &f.ack, 201, &serial, 1, 1.0 );
    rn_window_ack_receipt_sweep( &f.ack, 1.01 );
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 0 );
    rn_window_ack_receipt_sweep( &f.ack, 10.0 ); // RNReceipts SweepLoss
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 201, 10.01 ) == 1 ); // late ACK stutters
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 1 );
    rocketnet_unit_check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 0 );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 1 );
    receipt_fixture_destroy( &f );
}

static void test_unreliable_receipt_loss_on_sent_map_eviction()
{
    receipt_fixture_t f;
    receipt_fixture_init( &f );
    rn_window_ack_receipt_begin_update( &f.ack );
    // The seventeenth live packet forces the first out of the P=16 sent map.
    for ( uint32_t serial = 1; serial <= 17; serial++ )
        rn_window_ack_commit_receipt_packet( &f.ack, 600 + serial, &serial, 1, 1.0 );
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 1 );
    rocketnet_unit_check( out[0].serial == 1 && out[0].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED] == 1 );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 1 );
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 601, 1.01 ) == 0 );
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 602, 1.01 ) == 1 );
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 2 );
    rocketnet_unit_check( out[1].serial == 2 && out[1].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 1 );
    receipt_fixture_destroy( &f );
}

static void test_receipt_ring_full_drops_oldest_and_counts()
{
    receipt_fixture_t f;
    receipt_fixture_init( &f, 16 );
    rn_window_ack_receipt_begin_update( &f.ack );
    for ( uint32_t i = 1; i <= 17; i++ )
    {
        rn_window_ack_commit_receipt_packet( &f.ack, 300 + i, &i, 1, 1.0 + i * 0.01 );
        rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 300 + i, 1.01 + i * 0.01 ) == 1 );
    }
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 16 );
    rocketnet_unit_check( receipt_list( &f, NULL, 0 ) == 16 );
    rocketnet_unit_check( receipt_list( &f, out, 1 ) == 16 );
    rocketnet_unit_check( out[0].serial == 2 && out[15].serial == 17 );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL] == 1 );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 17 );
    receipt_fixture_destroy( &f );
}

struct receipt_transport_t
{
    alignas( 16 ) uint8_t frame[16 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t sent_payloads[4][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    int sent_bytes[4];
    uint64_t sent_packets[4];
    int sent_count;
    uint64_t next_packet;
    uint64_t sent_packet;
    uint64_t ack_packet;
    uint64_t ack_batch;
};

static void receipt_link_update( void * context, double )
{
    receipt_transport_t * t = (receipt_transport_t *) context;
    t->ack_batch = t->ack_packet;
    t->ack_packet = 0;
}

static int receipt_link_payloads( void *, const int ** slots, uint8_t * const ** data, const int ** bytes )
{
    *slots = NULL;
    *data = NULL;
    *bytes = NULL;
    return 0;
}

static int receipt_link_acked( void * context, const int ** slots, const uint64_t ** packets )
{
    receipt_transport_t * t = (receipt_transport_t *) context;
    *slots = NULL;
    *packets = &t->ack_batch;
    return t->ack_batch ? 1 : 0;
}

static uint8_t * receipt_link_start( void * context, int, int bytes, rocketnet_packet_context_t * packet )
{
    receipt_transport_t * t = (receipt_transport_t *) context;
    if ( bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return NULL;
    memset( packet, 0, sizeof( *packet ) );
    packet->frame = t->frame;
    packet->payload_index = 16;
    packet->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    packet->packet_id = ++t->next_packet;
    return t->frame;
}

static int receipt_link_finish( void * context, rocketnet_packet_context_t * packet, int bytes )
{
    receipt_transport_t * t = (receipt_transport_t *) context;
    rocketnet_unit_check( t->sent_count < 4 && bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    const int slot = t->sent_count++;
    t->sent_packets[slot] = packet->packet_id;
    t->sent_bytes[slot] = bytes;
    memcpy( t->sent_payloads[slot], packet->frame + packet->payload_index, (size_t) bytes );
    t->sent_packet = packet->packet_id;
    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes );
}

static void receipt_link_abort( void *, rocketnet_packet_context_t * ) {}
static void receipt_link_send( void * ) {}
static uint64_t receipt_link_connection( void *, int ) { return 1; }
static void receipt_link_disconnect( void *, int ) {}
static int receipt_link_state( void * ) { return ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; }

static rn_message_link_t receipt_link( receipt_transport_t * t )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = t;
    link.update = receipt_link_update;
    link.payloads = receipt_link_payloads;
    link.acked = receipt_link_acked;
    link.start_packet = receipt_link_start;
    link.finish_packet = receipt_link_finish;
    link.abort_packet = receipt_link_abort;
    link.send_packets = receipt_link_send;
    link.connection = receipt_link_connection;
    link.disconnect = receipt_link_disconnect;
    link.state = receipt_link_state;
    return link;
}

static void test_receipts_listed_per_update_by_the_c_api()
{
    receipt_transport_t transport;
    memset( &transport, 0, sizeof( transport ) );
    rn_message_link_t link = receipt_link( &transport );
    rocketnet_message_client_t * client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, NULL, NULL, 0,
                                                                                                     ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    rocketnet_unit_check( client != NULL );
    rocketnet_unit_check( rocketnet_message_client_update( client, 0.005 ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_client_send_packets( client );                               // the connection's CONFIG goes first and is acked (CHANNELS.md 10.3)
    transport.ack_packet = transport.sent_packet;
    rocketnet_unit_check( rocketnet_message_client_update( client, 0.005 ) == ROCKETNET_UPDATE_OK );
    transport.sent_count = 0;
    uint8_t * payload = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 3 );
    rocketnet_unit_check( payload != NULL );
    rn_message_connection_t * connection = &client->object.connections[0];
    rocketnet_unit_check( !rn_region_contains( connection->region, payload, 3 ) );     // written in place in the packet buffer (docs/OUTGOING-BUILD.md)
    payload[0] = 1; payload[1] = 2; payload[2] = 3;
    const uint32_t serial = rocketnet_message_client_finish( client, 3 );
    rocketnet_unit_check( serial != 0 );
    rocketnet_message_client_send_packets( client );
    rocketnet_unit_check( transport.sent_packet != 0 );
    rocketnet_message_receipt_t out[4];
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 0 );
    transport.ack_packet = transport.sent_packet;
    rocketnet_unit_check( rocketnet_message_client_update( client, 0.01 ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 1 );
    rocketnet_unit_check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rocketnet_unit_check( client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 1 );
    rocketnet_unit_check( client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 1 );
    rocketnet_unit_check( rocketnet_message_client_update( client, 0.02 ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 0 );
    rn_message_object_destroy( &client->object );
}

static void test_rnreceipts_code5_witness_replays()
{
    // BrokenTwoOutcomes: LOSS at the deadline, then a late packet ACK.
    // The serial's sent-map slot is a tombstone, so only LOSS is observable.
    receipt_fixture_t f;
    receipt_fixture_init( &f );
    rn_window_ack_receipt_begin_update( &f.ack );
    const uint32_t serial = 71;
    rn_window_ack_commit_receipt_packet( &f.ack, 401, &serial, 1, 1.0 );
    rn_window_ack_receipt_sweep( &f.ack, 10.0 );
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 401, 10.1 ) == 1 );
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 401, 10.2 ) == 0 );
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 1 );
    rocketnet_unit_check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    receipt_fixture_destroy( &f );
}

static void test_promoted_type5_block_replays_last_index_and_reliable_loss_witnesses()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 1024;
    config.receive_message_buffer_bytes = 262144;
    config.max_message_bytes = 262144;
    config.send_message_buffer_bytes = 800000;

    receipt_transport_t transport;
    memset( &transport, 0, sizeof( transport ) );
    rn_message_link_t link = receipt_link( &transport );
    rocketnet_message_client_t * client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, &config, NULL, 0,
                                                                                                     ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    rocketnet_unit_check( client != NULL );
    rn_message_connection_t * connection = &client->object.connections[0];
    rocketnet_unit_check( rocketnet_message_client_update( client, 1.0 ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_client_send_packets( client );                               // the connection's CONFIG goes first and is acked (CHANNELS.md 10.3)
    transport.ack_packet = transport.sent_packet;
    rocketnet_unit_check( rocketnet_message_client_update( client, 1.0 ) == ROCKETNET_UPDATE_OK );
    transport.sent_count = 0;
    const int large = (int) client->object.unreliable_limit + 1;
    uint8_t * payload = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, large );
    rocketnet_unit_check( payload != NULL );
    const uint32_t d = connection->pending_descriptor;
    rocketnet_unit_check( d == connection->ordered.channels.pending );
    rocketnet_unit_check( payload == rn_region_send_write_pointer( connection->region, d ) );
    memset( payload, 0xA5, (size_t) large );
    const uint32_t serial = rocketnet_message_client_finish( client, large );
    rocketnet_unit_check( serial != 0 );
    const rn_send_descriptor_t * descriptor = &connection->region->send_descriptors[d];
    rocketnet_unit_check( descriptor->reliability == ROCKETNET_RELIABILITY_RELIABLE );
    rocketnet_unit_check( ( descriptor->flags & RN_SEND_FLAG_RECEIPT_PROMOTED ) != 0 );
    rocketnet_unit_check( descriptor->fragments == 2 && descriptor->serial == serial );

    rocketnet_message_client_send_packets( client );
    rocketnet_unit_check( transport.sent_count == 2 );
    for ( int i = 0; i < 2; i++ )
    {
        rocketnet_message_reader_t reader;
        rocketnet_message_record_t record;
        rocketnet_unit_check( rocketnet_message_reader_begin( &reader, transport.sent_payloads[i], transport.sent_bytes[i], &connection->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
        rocketnet_unit_check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE && record.block );
    }

    // BrokenReliableLoss: an elapsed resend delay does not turn a reliable block into LOSS.
    rn_window_ack_receipt_sweep( &connection->ordered.ack, 10.0 );
    rocketnet_message_receipt_t out[4];
    rn_window_ack_receipt_end_update( &connection->ordered.ack );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 0 );

    // BrokenLastIndexAck: the last fragment alone cannot complete the block.
    transport.ack_packet = transport.sent_packets[1];
    rocketnet_unit_check( rocketnet_message_client_update( client, 1.01 ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 0 );
    transport.ack_packet = transport.sent_packets[0];
    rocketnet_unit_check( rocketnet_message_client_update( client, 1.02 ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 1 );
    rocketnet_unit_check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rocketnet_unit_check( client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 1 );
    rocketnet_unit_check( client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    transport.ack_packet = transport.sent_packets[1];
    rocketnet_unit_check( rocketnet_message_client_update( client, 1.03 ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 0 );

    // A large reservation finished short retains the single-packet code-5 semantics.
    payload = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, large );
    rocketnet_unit_check( payload != NULL );
    payload[0] = 7;
    const uint32_t short_serial = rocketnet_message_client_finish( client, 1 );
    rocketnet_unit_check( short_serial != 0 );
    rocketnet_message_client_send_packets( client );
    rocketnet_unit_check( transport.sent_count == 3 );
    rocketnet_message_reader_t reader;
    rocketnet_message_record_t record;
    rocketnet_unit_check( rocketnet_message_reader_begin( &reader, transport.sent_payloads[2], transport.sent_bytes[2], &connection->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) && record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE );
    transport.ack_packet = transport.sent_packets[2];
    rocketnet_unit_check( rocketnet_message_client_update( client, 1.04 ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 1 );
    rocketnet_unit_check( out[0].serial == short_serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rocketnet_unit_check( client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 2 );
    rocketnet_unit_check( client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    rn_message_object_destroy( &client->object );
}

static uint64_t receipt_allocations;

static void * receipt_malloc( void *, size_t bytes )
{
    receipt_allocations++;
    return malloc( bytes );
}

static void receipt_free( void *, void * pointer )
{
    free( pointer );
}

static void test_receipts_no_allocation_after_create()
{
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, plan_rss_chain( receipt_malloc, receipt_free ), plan_rss_free ) == ROCKETNET_STATUS_OK );
    receipt_fixture_t f;
    receipt_fixture_init( &f );
    const uint64_t snapshot = receipt_allocations;
    rn_window_ack_receipt_begin_update( &f.ack );
    for ( uint32_t i = 1; i <= 32; i++ )
    {
        rn_window_ack_commit_receipt_packet( &f.ack, 500 + i, &i, 1, 1.0 + i * 0.01 );
        if ( i & 1 )
            rn_window_ack_on_ack( &f.ack, 500 + i, 1.01 + i * 0.01 );
    }
    rn_window_ack_receipt_sweep( &f.ack, 10.0 );
    rocketnet_message_receipt_t out[16];
    receipt_list( &f, out, 16 );
    rocketnet_unit_check( receipt_allocations == snapshot );
    receipt_fixture_destroy( &f );
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_unreliable_receipt_acked_or_loss_exactly_once );
REGISTER_UNIT_TEST( test_unreliable_receipt_loss_at_deadline_sweep );
REGISTER_UNIT_TEST( test_unreliable_receipt_loss_on_sent_map_eviction );
REGISTER_UNIT_TEST( test_receipt_ring_full_drops_oldest_and_counts );
REGISTER_UNIT_TEST( test_receipts_listed_per_update_by_the_c_api );
REGISTER_UNIT_TEST( test_rnreceipts_code5_witness_replays );
REGISTER_UNIT_TEST( test_promoted_type5_block_replays_last_index_and_reliable_loss_witnesses );
REGISTER_UNIT_TEST( test_receipts_no_allocation_after_create );
