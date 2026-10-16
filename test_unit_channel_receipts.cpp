/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNReceipts Send: code 5 must be accepted while later receipt types remain gated.

#include "test_unit_registry.h"
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
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
    config.receipt_ring = ring;
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

static void unreliable_receipt_acked_or_loss_exactly_once()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, NULL );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_UNRELIABLE_WITH_RECEIPT, 0 ) == ROCKETNET_CHANNEL_SEND_OK );
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT, 0 ) == ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED );
    rocketnet_unit_check( rn_channels_send_check( &channels, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT, 0 ) == ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED );
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
    rocketnet_unit_check( out[0].serial == 11 && out[0].status == ROCKETNET_RECEIPT_ACKED );
    rocketnet_unit_check( out[1].serial == 12 && out[1].status == ROCKETNET_RECEIPT_ACKED );
    rn_window_ack_receipt_begin_update( &f.ack );
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 0 );
    receipt_fixture_destroy( &f );
}

static void unreliable_receipt_loss_at_deadline_sweep()
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
    rocketnet_unit_check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_LOSS );
    receipt_fixture_destroy( &f );
}

static void unreliable_receipt_loss_on_sent_map_eviction()
{
    receipt_fixture_t f;
    receipt_fixture_init( &f );
    rn_window_ack_receipt_begin_update( &f.ack );
    // The seventeenth live packet forces the first out of the P=16 sent map.
    for ( uint32_t serial = 1; serial <= 17; serial++ )
        rn_window_ack_commit_receipt_packet( &f.ack, 600 + serial, &serial, 1, 1.0 );
    rocketnet_message_receipt_t out[16];
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 1 );
    rocketnet_unit_check( out[0].serial == 1 && out[0].status == ROCKETNET_RECEIPT_LOSS );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED] == 1 );
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 601, 1.01 ) == 0 );
    rocketnet_unit_check( rn_window_ack_on_ack( &f.ack, 602, 1.01 ) == 1 );
    rocketnet_unit_check( receipt_list( &f, out, 16 ) == 2 );
    rocketnet_unit_check( out[1].serial == 2 && out[1].status == ROCKETNET_RECEIPT_ACKED );
    receipt_fixture_destroy( &f );
}

static void receipt_ring_full_drops_oldest_and_counts()
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
    rocketnet_unit_check( out[0].serial == 2 && out[15].serial == 17 );
    rocketnet_unit_check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL] == 1 );
    receipt_fixture_destroy( &f );
}

struct receipt_transport_t
{
    alignas( 16 ) uint8_t frame[16 + ROCKETNET_MAX_PAYLOAD_BYTES];
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
    if ( bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return NULL;
    memset( packet, 0, sizeof( *packet ) );
    packet->frame = t->frame;
    packet->payload_index = 16;
    packet->payload_capacity = ROCKETNET_MAX_PAYLOAD_BYTES;
    packet->packet_id = ++t->next_packet;
    return t->frame;
}

static int receipt_link_finish( void * context, rocketnet_packet_context_t * packet, int bytes )
{
    receipt_transport_t * t = (receipt_transport_t *) context;
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

static void receipts_listed_per_update_by_the_c_api()
{
    receipt_transport_t transport;
    memset( &transport, 0, sizeof( transport ) );
    rn_message_link_t link = receipt_link( &transport );
    rocketnet_message_client_t * client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, NULL, NULL, 0,
                                                                                                     ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, 0.0 );
    rocketnet_unit_check( client != NULL );
    uint8_t * payload = rocketnet_message_client_start( client, 0, ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT, 3 );
    rocketnet_unit_check( payload != NULL );
    rn_message_connection_t * connection = &client->object.connections[0];
    rocketnet_unit_check( payload == rn_region_send_write_pointer( connection->region, connection->pending_descriptor ) );
    payload[0] = 1; payload[1] = 2; payload[2] = 3;
    const uint32_t serial = rocketnet_message_client_finish( client, 3 );
    rocketnet_unit_check( serial != 0 );
    rocketnet_message_client_send_packets( client );
    rocketnet_unit_check( transport.sent_packet != 0 );
    rocketnet_message_receipt_t out[4];
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 0 );
    transport.ack_packet = transport.sent_packet;
    rocketnet_unit_check( rocketnet_message_client_update( client, 0.01 ) == ROCKETNET_MESSAGE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 1 );
    rocketnet_unit_check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_ACKED );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 1 );
    rocketnet_unit_check( rocketnet_message_client_update( client, 0.02 ) == ROCKETNET_MESSAGE_OK );
    rocketnet_unit_check( rocketnet_message_client_receipts( client, out, 4 ) == 0 );
    rn_message_object_destroy( &client->object );
}

static void rnreceipts_code5_witness_replays()
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
    rocketnet_unit_check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_LOSS );
    receipt_fixture_destroy( &f );
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

static void no_allocation_after_create()
{
    rocketnet_term();
    rocketnet_unit_check( rocketnet_allocator( receipt_malloc, receipt_free ) );
    rocketnet_unit_check( rocketnet_init() );
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
    rocketnet_unit_check( rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function ) );
    rocketnet_unit_check( rocketnet_init() );
}

REGISTER_UNIT_TEST( unreliable_receipt_acked_or_loss_exactly_once );
REGISTER_UNIT_TEST( unreliable_receipt_loss_at_deadline_sweep );
REGISTER_UNIT_TEST( unreliable_receipt_loss_on_sent_map_eviction );
REGISTER_UNIT_TEST( receipt_ring_full_drops_oldest_and_counts );
REGISTER_UNIT_TEST( receipts_listed_per_update_by_the_c_api );
REGISTER_UNIT_TEST( rnreceipts_code5_witness_replays );
REGISTER_UNIT_TEST( no_allocation_after_create );
