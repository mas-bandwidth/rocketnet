/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNReceipts ReceiveAck, EmitAck and SweepLoss: a reliable receipt is emitted
// once, after every distinct number is transport-acked, and never as LOSS.

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"

#include <stdlib.h>
#include <string.h>

struct reliable_receipt_transport_t
{
    alignas( 16 ) uint8_t frame[16 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t sent_payloads[16][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    int sent_bytes[16];
    uint64_t sent_packets[16];
    int sent_count;
    uint64_t next_packet;
    uint64_t ack_packet;
    uint64_t ack_batch;
};

static void reliable_link_update( void * context, double )
{
    reliable_receipt_transport_t * t = (reliable_receipt_transport_t *) context;
    t->ack_batch = t->ack_packet;
    t->ack_packet = 0;
}

static int reliable_link_payloads( void *, const int ** slots, uint8_t * const ** data, const int ** bytes )
{
    *slots = NULL;
    *data = NULL;
    *bytes = NULL;
    return 0;
}

static int reliable_link_acked( void * context, const int ** slots, const uint64_t ** packets )
{
    reliable_receipt_transport_t * t = (reliable_receipt_transport_t *) context;
    *slots = NULL;
    *packets = &t->ack_batch;
    return t->ack_batch ? 1 : 0;
}

static uint8_t * reliable_link_start( void * context, int, int bytes, rocketnet_packet_context_t * packet )
{
    reliable_receipt_transport_t * t = (reliable_receipt_transport_t *) context;
    if ( bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return NULL;
    memset( packet, 0, sizeof( *packet ) );
    packet->frame = t->frame;
    packet->payload_index = 16;
    packet->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    packet->packet_id = ++t->next_packet;
    return t->frame;
}

static int reliable_link_finish( void * context, rocketnet_packet_context_t * packet, int bytes )
{
    reliable_receipt_transport_t * t = (reliable_receipt_transport_t *) context;
    rocketnet_unit_check( t->sent_count < 16 && bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    const int slot = t->sent_count++;
    t->sent_packets[slot] = packet->packet_id;
    t->sent_bytes[slot] = bytes;
    memcpy( t->sent_payloads[slot], packet->frame + packet->payload_index, (size_t) bytes );
    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes );
}

static void reliable_link_abort( void *, rocketnet_packet_context_t * ) {}
static void reliable_link_send( void * ) {}
static uint64_t reliable_link_connection( void *, int ) { return 1; }
static void reliable_link_disconnect( void *, int ) {}
static int reliable_link_state( void * ) { return ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; }

static rn_message_link_t reliable_link( reliable_receipt_transport_t * t )
{
    rn_message_link_t link = {};
    link.transport = t;
    link.update = reliable_link_update;
    link.payloads = reliable_link_payloads;
    link.acked = reliable_link_acked;
    link.start_packet = reliable_link_start;
    link.finish_packet = reliable_link_finish;
    link.abort_packet = reliable_link_abort;
    link.send_packets = reliable_link_send;
    link.connection = reliable_link_connection;
    link.disconnect = reliable_link_disconnect;
    link.state = reliable_link_state;
    return link;
}

struct reliable_receipt_fixture_t
{
    reliable_receipt_transport_t transport;
    rocketnet_message_client_t * client;
    uint32_t serial;
};

static uint64_t reliable_receipt_allocations;

static void reliable_fixture_create( reliable_receipt_fixture_t * f, int type, int bytes, uint64_t * allocations_after_create = NULL )
{
    memset( f, 0, sizeof( *f ) );
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 1024;
    config.receive_message_buffer_bytes = 262144;
    config.max_message_bytes = 262144;
    config.send_message_buffer_bytes = 800000;
    rn_message_link_t link = reliable_link( &f->transport );
    f->client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, &config, NULL, 0,
                                                                          ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    rocketnet_unit_check( f->client != NULL );
    if ( allocations_after_create )
        *allocations_after_create = reliable_receipt_allocations;
    rocketnet_unit_check( rocketnet_message_client_update( f->client, 1.0 ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_client_send_packets( f->client );                            // the connection's CONFIG goes first and is acked (CHANNELS.md 10.3)
    f->transport.ack_packet = f->transport.sent_packets[0];
    rocketnet_unit_check( rocketnet_message_client_update( f->client, 1.0 ) == ROCKETNET_UPDATE_OK );
    f->transport.sent_count = 0;
    uint8_t * payload = rocketnet_message_client_start( f->client, 0, type, bytes );
    rocketnet_unit_check( payload != NULL );
    const rn_message_connection_t * connection = &f->client->object.connections[0];
    rocketnet_unit_check( payload == rn_region_send_write_pointer( connection->region, connection->pending_descriptor ) );
    memset( payload, 0xA7, (size_t) bytes );
    f->serial = rocketnet_message_client_finish( f->client, bytes );
    rocketnet_unit_check( f->serial != 0 );
    rocketnet_message_client_send_packets( f->client );
    rocketnet_unit_check( f->transport.sent_count > 0 );
    for ( int i = 0; i < f->transport.sent_count; i++ )
    {
        rocketnet_message_reader_t reader;
        rocketnet_message_record_t record;
        rocketnet_unit_check( rocketnet_message_reader_begin( &reader, f->transport.sent_payloads[i], f->transport.sent_bytes[i],
                                                              &connection->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
        rocketnet_unit_check( record.code == ( type == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ?
                                                ROCKETNET_MESSAGE_CODE_RELIABLE : ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED ) );
    }
}

static void reliable_fixture_ack( reliable_receipt_fixture_t * f, int packet, double now )
{
    f->transport.ack_packet = f->transport.sent_packets[packet];
    rocketnet_unit_check( rocketnet_message_client_update( f->client, now ) == ROCKETNET_UPDATE_OK );
}

static int reliable_fixture_receipts( reliable_receipt_fixture_t * f, rocketnet_message_receipt_t * out )
{
    return rocketnet_message_client_receipts( f->client, out, 16 );
}

static void reliable_fixture_destroy( reliable_receipt_fixture_t * f )
{
    rn_message_object_destroy( &f->client->object );
}

static void test_reliable_receipt_acked_never_loss()
{
    rocketnet_unit_check( ROCKETNET_CHANNEL_RECEIPT_BASE( ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT ) ==
                          ROCKETNET_CHANNEL_TYPE_RELIABLE );
    rocketnet_unit_check( ROCKETNET_CHANNEL_RECEIPT_BASE( ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT ) ==
                          ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED );
    reliable_receipt_fixture_t f;
    reliable_fixture_create( &f, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 3 );
    rocketnet_unit_check( f.transport.sent_count == 1 );
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    reliable_fixture_ack( &f, 0, 1.01 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 1 );
    rocketnet_unit_check( out[0].serial == f.serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    reliable_fixture_ack( &f, 0, 1.02 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    rn_window_ack_receipt_sweep( &f.client->object.connections[0].ordered.ack, 10.0 );
    rocketnet_unit_check( f.client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    reliable_fixture_destroy( &f );
}

static void test_ordered_receipt_after_all_chunks()
{
    reliable_receipt_fixture_t f;
    reliable_fixture_create( &f, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17 );
    rocketnet_unit_check( f.transport.sent_count == 3 );
    rocketnet_message_receipt_t out[16];
    reliable_fixture_ack( &f, 2, 1.01 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    reliable_fixture_ack( &f, 0, 1.02 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    reliable_fixture_ack( &f, 1, 1.03 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 1 );
    rocketnet_unit_check( out[0].serial == f.serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    reliable_fixture_destroy( &f );
}

static void test_reliable_receipt_waits_for_every_number()
{
    reliable_receipt_fixture_t f;
    reliable_fixture_create( &f, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 1412 );
    rocketnet_unit_check( f.transport.sent_count == 2 );
    rocketnet_message_receipt_t out[16];
    reliable_fixture_ack( &f, 1, 1.01 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    reliable_fixture_ack( &f, 1, 1.02 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    // The first packet's ACK is lost. Its number is resent with a new
    // transport packet id, and only that ACK completes the receipt.
    rocketnet_unit_check( rocketnet_message_client_update( f.client, 1.50 ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_client_send_packets( f.client );
    rocketnet_unit_check( f.transport.sent_count >= 3 );
    rocketnet_message_reader_t first_reader, retry_reader;
    rocketnet_message_record_t first_record, retry_record;
    const rn_message_connection_t * connection = &f.client->object.connections[0];
    rocketnet_unit_check( rocketnet_message_reader_begin( &first_reader, f.transport.sent_payloads[0], f.transport.sent_bytes[0],
                                                          &connection->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rocketnet_unit_check( rocketnet_message_reader_begin( &retry_reader, f.transport.sent_payloads[2], f.transport.sent_bytes[2],
                                                          &connection->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rocketnet_unit_check( rocketnet_message_reader_next( &first_reader, &first_record ) &&
                          rocketnet_message_reader_next( &retry_reader, &retry_record ) );
    rocketnet_unit_check( retry_record.code == ROCKETNET_MESSAGE_CODE_RELIABLE &&
                          retry_record.number == first_record.number && retry_record.fragment == first_record.fragment );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    reliable_fixture_ack( &f, 2, 1.51 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 1 );
    rocketnet_unit_check( out[0].serial == f.serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    reliable_fixture_destroy( &f );
}

static void test_rnreceipts_reliable_witness_replays()
{
    // BrokenLastIndexAck and BrokenReliableLoss: neither a last-index ACK nor
    // a resend delay sweep completes or loses a still-incomplete reliable block.
    reliable_receipt_fixture_t f;
    reliable_fixture_create( &f, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 1412 );
    rocketnet_unit_check( f.transport.sent_count == 2 );
    rocketnet_message_receipt_t out[16];
    reliable_fixture_ack( &f, 1, 1.01 );
    rn_window_ack_receipt_sweep( &f.client->object.connections[0].ordered.ack, 10.0 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 0 );
    rocketnet_unit_check( f.client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    reliable_fixture_ack( &f, 0, 1.02 );
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 1 );
    rocketnet_unit_check( out[0].serial == f.serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    reliable_fixture_destroy( &f );
}

static void * reliable_receipt_malloc( void *, size_t bytes )
{
    reliable_receipt_allocations++;
    return malloc( bytes );
}

static void reliable_receipt_free( void *, void * pointer )
{
    free( pointer );
}

static void test_no_allocation_after_create_reliable_receipts()
{
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, reliable_receipt_malloc, reliable_receipt_free ) == ROCKETNET_STATUS_OK );

    reliable_receipt_fixture_t f;
    uint64_t after_create = 0;
    reliable_fixture_create( &f, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 3, &after_create );
    reliable_fixture_ack( &f, 0, 1.01 );
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( reliable_fixture_receipts( &f, out ) == 1 );
    rocketnet_unit_check( reliable_receipt_allocations == after_create );
    reliable_fixture_destroy( &f );

    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_reliable_receipt_acked_never_loss );
REGISTER_UNIT_TEST( test_ordered_receipt_after_all_chunks );
REGISTER_UNIT_TEST( test_reliable_receipt_waits_for_every_number );
REGISTER_UNIT_TEST( test_rnreceipts_reliable_witness_replays );
REGISTER_UNIT_TEST( test_no_allocation_after_create_reliable_receipts );
