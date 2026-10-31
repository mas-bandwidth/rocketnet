/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// CONFIG, the one announcement (standard/MESSAGES.md m11;
// proofs/tla/RNEndpoint.tla Reset, WriteOk, OnPayload with ConfigCompare, Tick):
// each end queues one CONFIG control record at connection reset, at the head
// of IMMEDIATE, taking the connection's first reliable number; the receiver
// compares it field by field with its own, and a difference ends the
// connection with ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH. The objects run
// over the same in-memory transport pair test_unit_message_objects.cpp uses
// (copied here, renamed mc_): frames handed out at start_packet, opened once
// on the far side, opened ids reported back as the transport's acks. Time is
// only ever an argument: nothing sleeps, nothing opens a socket.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_wire.h"

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// the in-memory transport pair
// ---------------------------------------------------------------------------

#define MC_MAX_SLOTS        4
#define MC_PAYLOAD_INDEX    16
#define MC_MAX_FRAMES       64

struct mc_packet_t
{
    uint64_t id;
    int slot;
    int bytes;
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
};

struct mc_net_t;

struct mc_endpoint_t
{
    mc_net_t * net;
    bool is_server;
    int slot;                                       // a client endpoint's slot on the server

    // send: frames handed out by start_packet, finished into `outbound`
    alignas( 16 ) uint8_t frames[MC_MAX_FRAMES][MC_PAYLOAD_INDEX + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    bool frame_used[MC_MAX_FRAMES];
    uint64_t frame_id[MC_MAX_FRAMES];
    int frame_slot[MC_MAX_FRAMES];

    // receive: the packets on the wire to this endpoint, and the update's batch
    std::vector<mc_packet_t> inbound;
    std::vector<mc_packet_t> batch;
    std::vector<uint8_t *> batch_data;
    std::vector<int> batch_bytes;
    std::vector<int> batch_slot;

    // acks: ids this endpoint opened, reported to the sender at its next update
    std::vector<uint64_t> acks_inbox;               // ids the other end opened of this endpoint's sends
    std::vector<int> acks_inbox_slot;
    std::vector<uint64_t> acked;                    // this update's acked batch
    std::vector<int> acked_slot;

    // what this endpoint finished, for the pointer-identity checks
    std::vector<mc_packet_t> sent_log;

    bool connected;                                 // the client's transport state
    int disconnects;                                // times the object disconnected it
    int finishes;
    double time;
};

struct mc_net_t
{
    mc_endpoint_t * server;
    mc_endpoint_t * clients[MC_MAX_SLOTS];
    uint64_t slot_connection[MC_MAX_SLOTS];         // the server transport's slot owners
    uint64_t next_connection;
    uint64_t next_packet_id;
    bool drop_to_server;
    bool drop_to_client;
    int drop_next_to_server;                        // drop this many packets to the server, then deliver
    int fail_finish;                                // the next this many finish_packet calls fail (return 0, nothing sent)
    bool hold_acks;                                 // opened ids are not reported back while set
    bool keep_log;
};

#if ROCKETNET_DEV
static mc_net_t * mc_net_new()
{
    mc_net_t * net = new mc_net_t();
    memset( net->slot_connection, 0, sizeof( net->slot_connection ) );
    net->server = new mc_endpoint_t();
    net->server->net = net;
    net->server->is_server = true;
    net->server->slot = -1;
    for ( int i = 0; i < MC_MAX_SLOTS; i++ )
    {
        net->clients[i] = NULL;
    }
    net->next_connection = 100;
    net->next_packet_id = 1000;
    net->drop_to_server = false;
    net->drop_to_client = false;
    net->drop_next_to_server = 0;
    net->fail_finish = 0;
    net->hold_acks = false;
    net->keep_log = true;
    return net;
}

static mc_endpoint_t * mc_net_client( mc_net_t * net, int slot )
{
    mc_endpoint_t * client = new mc_endpoint_t();
    client->net = net;
    client->is_server = false;
    client->slot = slot;
    client->connected = false;
    net->clients[slot] = client;

    return client;
}

// the transport's handshake completes: the server's slot takes a new
// connection id and the client is CONNECTED

static void mc_net_connect( mc_net_t * net, int slot )
{
    net->slot_connection[slot] = net->next_connection++;
    if ( net->clients[slot] )
    {
        net->clients[slot]->connected = true;
    }
}

static void mc_net_drop_slot( mc_net_t * net, int slot )
{
    net->slot_connection[slot] = 0;
    if ( net->clients[slot] )
    {
        net->clients[slot]->connected = false;
    }
}

static void mc_net_free( mc_net_t * net )
{
    for ( int i = 0; i < MC_MAX_SLOTS; i++ )
    {
        delete net->clients[i];
    }
    delete net->server;
    delete net;
}

// --- the link functions ---

static void mc_update( void * transport, double time )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    mc_net_t * net = e->net;
    e->time = time;

    // the transport's acked packet ids: what the other end opened since the last update

    e->acked.clear();
    e->acked_slot.clear();

    if ( !net->hold_acks )
    {
        e->acked.swap( e->acks_inbox );
        e->acked_slot.swap( e->acks_inbox_slot );
    }

    // the batch: the last one is gone, the wire's packets are opened once each

    e->batch.clear();
    e->batch_data.clear();
    e->batch_bytes.clear();
    e->batch_slot.clear();
    e->batch.reserve( e->inbound.size() );

    for ( size_t i = 0; i < e->inbound.size(); i++ )
    {
        const mc_packet_t & p = e->inbound[i];
        if ( e->is_server && net->slot_connection[p.slot] == 0 )
            continue;                                           // no connection in the slot: the transport refuses it
        if ( !e->is_server && !e->connected )
            continue;
        e->batch.push_back( p );

        // the opened id goes back to the sender as an ack at its next update

        mc_endpoint_t * sender = e->is_server ? net->clients[p.slot] : net->server;
        if ( sender )
        {
            sender->acks_inbox.push_back( p.id );
            sender->acks_inbox_slot.push_back( p.slot );
        }
    }

    e->inbound.clear();

    for ( size_t i = 0; i < e->batch.size(); i++ )
    {
        e->batch_data.push_back( e->batch[i].data );
        e->batch_bytes.push_back( e->batch[i].bytes );
        e->batch_slot.push_back( e->batch[i].slot );
    }
}

static int mc_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    *client_index = e->is_server ? e->batch_slot.data() : NULL;
    *payload_data = e->batch_data.data();
    *payload_bytes = e->batch_bytes.data();

    return (int) e->batch.size();
}

static int mc_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    *client_index = e->is_server ? e->acked_slot.data() : NULL;
    *packet_ids = e->acked.data();
    return (int) e->acked.size();
}

static uint8_t * mc_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    memset( context, 0, sizeof( *context ) );
    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return NULL;
    if ( e->is_server ? e->net->slot_connection[client_index] == 0 : !e->connected )
        return NULL;

    for ( int f = 0; f < MC_MAX_FRAMES; f++ )
    {
        if ( e->frame_used[f] )
            continue;
        e->frame_used[f] = true;
        e->frame_id[f] = e->net->next_packet_id++;
        e->frame_slot[f] = e->is_server ? client_index : e->slot;
        context->frame = e->frames[f];
        context->payload_index = MC_PAYLOAD_INDEX;
        context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        context->packet_id = e->frame_id[f];
        context->client_index = client_index;
        return e->frames[f];
    }

    return NULL;
}

static int mc_frame_of( mc_endpoint_t * e, const rocketnet_packet_context_t * context )
{
    for ( int f = 0; f < MC_MAX_FRAMES; f++ )
        if ( e->frame_used[f] && context->frame == e->frames[f] )
            return f;
    return -1;
}

static int mc_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    const int f = mc_frame_of( e, context );
    check( f >= 0 );
    check( payload_bytes >= 1 && payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    e->frame_used[f] = false;
    if ( e->net->fail_finish > 0 )
    {
        e->net->fail_finish--;                                  // the transport refused the packet: nothing goes on the wire
        return 0;
    }

    e->finishes++;

    mc_packet_t packet;
    packet.id = e->frame_id[f];
    packet.slot = e->frame_slot[f];
    packet.bytes = payload_bytes;
    memcpy( packet.data, e->frames[f] + MC_PAYLOAD_INDEX, (size_t) payload_bytes );         // the wire: the transport's own business
    if ( e->net->keep_log )
    {
        e->sent_log.push_back( packet );
    }

    mc_net_t * net = e->net;

    if ( e->is_server )
    {
        mc_endpoint_t * to = net->clients[packet.slot];
        if ( to && !net->drop_to_client )
        {
            to->inbound.push_back( packet );
        }
    }
    else
    {
        if ( net->drop_next_to_server > 0 )
            net->drop_next_to_server--;
        else if ( !net->drop_to_server )
            net->server->inbound.push_back( packet );
    }

    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_bytes );
}

static void mc_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    const int f = mc_frame_of( e, context );
    if ( f >= 0 )
    {
        e->frame_used[f] = false;
    }
}

static void mc_send_packets( void * transport )
{
    (void) transport;
}

static uint64_t mc_connection( void * transport, int client_index )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    if ( e->is_server )
        return e->net->slot_connection[client_index];
    return e->connected ? 1 : 0;
}

static void mc_disconnect( void * transport, int client_index )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    e->disconnects++;
    if ( e->is_server )
        mc_net_drop_slot( e->net, client_index );
    else
        e->connected = false;
}

static int mc_state( void * transport )
{
    mc_endpoint_t * e = (mc_endpoint_t *) transport;
    if ( e->is_server )
        return ROCKETNET_SERVER_TRANSPORT_RUNNING;
    return e->connected ? ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED : ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
}

static rn_message_link_t mc_link( mc_endpoint_t * e )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = e;
    link.update = mc_update;
    link.payloads = mc_payloads;
    link.acked = mc_acked;
    link.start_packet = mc_start_packet;
    link.finish_packet = mc_finish_packet;
    link.abort_packet = mc_abort_packet;
    link.send_packets = mc_send_packets;
    link.connection = mc_connection;
    link.disconnect = mc_disconnect;
    link.state = mc_state;
    link.destroy = NULL;                                        // the harness owns the pair
    return link;
}

// ---------------------------------------------------------------------------
// objects on the pair
// ---------------------------------------------------------------------------

static void mc_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->num_channels = 4;
    config->reliable_messages_in_flight = 64;
    config->receive_message_buffer_bytes = 65536;
    config->max_message_bytes = 16384;
    config->send_message_buffer_bytes = 65536;
    config->send_queue_messages = 64;
    config->packets_in_flight = 64;
    config->packets_per_update = 8;
    config->unreliable_messages_per_update = 64;
    config->message_resend_delay_initial_ms = 100;
    config->message_resend_delay_min_ms = 20;
    config->message_resend_delay_max_ms = 400;
    config->reliable_message_timeout_ms = 120000;
}
#endif // #if ROCKETNET_DEV

// the clock every case starts at: never 0.0, so a time field left at zero
// shows up as an immediate timeout instead of passing by accident

#define MC_T0 1000.0

struct mc_pair_t
{
    mc_net_t * net;
    rocketnet_message_server_t * server;
    rocketnet_message_client_t * clients[MC_MAX_SLOTS];
    int num_clients;
    double time;
};

#if ROCKETNET_DEV
static rocketnet_message_server_t * mc_server_create( mc_net_t * net, const rocketnet_message_config_t * config, int slots, int32_t kbps, void * memory = NULL, uint64_t memory_bytes = 0 )
{
    rn_message_link_t link = mc_link( net->server );
    return (rocketnet_message_server_t *) rn_message_object_create( NULL, &link, 1, slots, config, memory, memory_bytes, kbps, MC_T0 );
}

static rocketnet_message_client_t * mc_client_create( mc_net_t * net, int slot, const rocketnet_message_config_t * config, int32_t kbps, void * memory = NULL, uint64_t memory_bytes = 0 )
{
    mc_endpoint_t * e = mc_net_client( net, slot );
    rn_message_link_t link = mc_link( e );
    return (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, config, memory, memory_bytes, kbps, MC_T0 );
}

static void mc_pair_setup( mc_pair_t * pair, int num_clients, const rocketnet_message_config_t * config = NULL,
                           int32_t client_kbps = ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, int32_t server_kbps = ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS )
{
    rocketnet_message_config_t defaults;

    if ( !config )
    {
        mc_config( &defaults );
        config = &defaults;
    }

    memset( pair, 0, sizeof( *pair ) );
    pair->net = mc_net_new();
    pair->num_clients = num_clients;
    pair->server = mc_server_create( pair->net, config, MC_MAX_SLOTS, server_kbps );
    check( pair->server );

    for ( int i = 0; i < num_clients; i++ )
    {
        pair->clients[i] = mc_client_create( pair->net, i, config, client_kbps );
        check( pair->clients[i] );
        mc_net_connect( pair->net, i );
    }

    pair->time = MC_T0;
}

static void mc_pair_teardown( mc_pair_t * pair )
{
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        rocketnet_message_client_destroy( pair->clients[i] );
    }
    rocketnet_message_server_destroy( pair->server );
    mc_net_free( pair->net );
}

static rn_message_connection_t * mc_server_slot( mc_pair_t * pair, int slot )
{
    return rn_message_object_connection( &pair->server->object, slot );
}

static rn_message_connection_t * mc_client_connection( rocketnet_message_client_t * client )
{
    return rn_message_object_connection( &client->object, 0 );
}
#endif // #if ROCKETNET_DEV

// every message the application received this update, released before the next

struct mc_received_t
{
    int slot;
    int channel;
    std::vector<uint8_t> bytes;
};

// copies a received message; a zero-byte message copies nothing, so its data
// pointer never reaches memmove (gcc 13 and 15 -Wnonnull on vector::assign)

#if ROCKETNET_DEV
static void mc_copy_bytes( std::vector<uint8_t> & to, const uint8_t * from, int bytes )
{
    to.resize( (size_t) bytes );
    if ( bytes > 0 )
    {
        memcpy( to.data(), from, (size_t) bytes );
    }
}

static void mc_drain_server( mc_pair_t * pair, std::vector<mc_received_t> * out )
{
    // the server's one receive queue across every slot: each message names
    // its sender's slot and the channel it was delivered on

    const uint8_t * data = NULL;
    int bytes = 0;
    int slot = 0;
    int channel = 0;
    while ( rocketnet_message_server_receive( pair->server, &slot, &channel, &data, &bytes ) )
    {
        check( slot >= 0 && slot < MC_MAX_SLOTS );

        mc_received_t r;
        r.slot = slot;
        r.channel = channel;
        mc_copy_bytes( r.bytes, data, bytes );
        if ( out )
        {
            out->push_back( r );
        }
        rocketnet_message_server_release( pair->server, data );
    }
}

static void mc_drain_client( rocketnet_message_client_t * client, std::vector<mc_received_t> * out )
{
    {
        const uint8_t * data = NULL;
        int bytes = 0;
        int channel = 0;
        while ( rocketnet_message_client_receive( client, &channel, &data, &bytes ) )
        {
            mc_received_t r;
            r.slot = 0;
            r.channel = channel;
            mc_copy_bytes( r.bytes, data, bytes );
            if ( out )
            {
                out->push_back( r );
            }
            rocketnet_message_client_release( client, data );
        }
    }
}

// one step of the whole pair: every object updates, the applications drain and
// release, every object sends

static void mc_step( mc_pair_t * pair, std::vector<mc_received_t> * server_got = NULL, std::vector<mc_received_t> * client_got = NULL, double dt = 0.01 )
{
    pair->time += dt;
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        check( rocketnet_message_client_update( pair->clients[i], pair->time ) == ROCKETNET_UPDATE_OK );
    }
    mc_drain_server( pair, server_got );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        mc_drain_client( pair->clients[i], client_got );
    }
    rocketnet_message_server_send_packets( pair->server );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        rocketnet_message_client_send_packets( pair->clients[i] );
    }
}

static uint8_t mc_pattern( uint32_t id, uint32_t i )
{
    return (uint8_t) ( id * 131 + i * 7 + ( i >> 8 ) * 13 + 5 );
}

static void mc_fill( uint8_t * data, uint32_t id, uint32_t bytes )
{
    for ( uint32_t i = 0; i < bytes; i++ )
    {
        data[i] = mc_pattern( id, i );
    }
    if ( bytes >= 4 )
    {
        memcpy( data, &id, 4 );
    }
}

static bool mc_verify( const std::vector<uint8_t> & data, uint32_t * out_id )
{
    if ( data.size() < 4 )
        return false;

    uint32_t id;
    memcpy( &id, data.data(), 4 );
    for ( uint32_t i = 4; i < data.size(); i++ )
        if ( data[i] != mc_pattern( id, i ) )
            return false;
    *out_id = id;

    return true;
}

static uint64_t mc_counter( const rocketnet_counters_t * counters, int which )
{
    return rocketnet_counter_value( counters, which );
}

// ---------------------------------------------------------------------------
// CONFIG helpers
// ---------------------------------------------------------------------------

// one step with the client updating first, so each end opens the other's
// CONFIG before either transport drops the connection (the harness drops a
// kicked slot at once; a real transport's disconnect is a packet later)

static void mc_step_client_first( mc_pair_t * pair, double dt = 0.01 )
{
    pair->time += dt;
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        check( rocketnet_message_client_update( pair->clients[i], pair->time ) == ROCKETNET_UPDATE_OK );
    }
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    mc_drain_server( pair, NULL );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        mc_drain_client( pair->clients[i], NULL );
    }
    rocketnet_message_server_send_packets( pair->server );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        rocketnet_message_client_send_packets( pair->clients[i] );
    }
}

// the error lines naming a CONFIG mismatch
static int g_mc_mismatch_lines = 0;
static char g_mc_mismatch_line[2][512];

static void mc_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    if ( level != ROCKETNET_LOG_LEVEL_ERROR || !strstr( message, "CONFIG mismatch" ) )
        return;

    if ( g_mc_mismatch_lines < 2 )
    {
        const size_t n = length < sizeof( g_mc_mismatch_line[0] ) - 1 ? length : sizeof( g_mc_mismatch_line[0] ) - 1;
        memcpy( g_mc_mismatch_line[g_mc_mismatch_lines], message, n );
        g_mc_mismatch_line[g_mc_mismatch_lines][n] = '\0';
    }

    g_mc_mismatch_lines++;
}

// ---------------------------------------------------------------------------
// the cases
// ---------------------------------------------------------------------------

// RNEndpoint OnPayload with ConfigCompare: two builds whose message configs
// differ in one wire field (num_channels 4 at the server, 3 at the client)
// refuse each other. Each end logs one error line naming the field and both
// values; the server kicks the slot and the client disconnects itself, both
// with CONFIG_MISMATCH. Red at a base without CONFIG: both stay connected.

static void test_message_config_mismatch_ends_both_connections()
{
    rocketnet_message_config_t server_config;
    rocketnet_message_config_t client_config;
    mc_config( &server_config );
    mc_config( &client_config );
    client_config.num_channels = 3;

    mc_pair_t pair;
    memset( &pair, 0, sizeof( pair ) );
    pair.net = mc_net_new();
    pair.num_clients = 1;
    pair.server = mc_server_create( pair.net, &server_config, MC_MAX_SLOTS, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
    check( pair.server );
    pair.clients[0] = mc_client_create( pair.net, 0, &client_config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( pair.clients[0] );
    mc_net_connect( pair.net, 0 );
    pair.time = MC_T0;

    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    g_mc_mismatch_lines = 0;
    rocketnet_log_function( mc_capture, NULL );

    for ( int i = 0; i < 5; i++ )
    {
        mc_step_client_first( &pair );
    }

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    check( rocketnet_message_client_disconnect_reason( pair.clients[0] ) == ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH );
    check( pair.net->clients[0]->disconnects == 1 );           // the client disconnected itself
    check( pair.net->server->disconnects == 1 );               // the server kicked the slot
    check( !pair.net->clients[0]->connected );
    check( pair.net->slot_connection[0] == 0 );

    // one line from each end, naming the field and both values

    check( g_mc_mismatch_lines == 2 );

    for ( int i = 0; i < 2 && i < g_mc_mismatch_lines; i++ )
    {
        check( strstr( g_mc_mismatch_line[i], "num_channels" ) != NULL );
        check( strstr( g_mc_mismatch_line[i], " 4 " ) != NULL || strstr( g_mc_mismatch_line[i], " 4" ) != NULL );
        check( strstr( g_mc_mismatch_line[i], " 3 " ) != NULL || strstr( g_mc_mismatch_line[i], " 3" ) != NULL );
    }

    mc_pair_teardown( &pair );
}

// The records one finished packet carried, read back with the receiving
// connection's limits.

static int mc_records( const mc_packet_t & packet, const rocketnet_message_limits_t * limits, rocketnet_message_record_t * out, int max )
{
    rocketnet_message_reader_t reader;
    if ( rocketnet_message_reader_begin( &reader, packet.data, packet.bytes, limits ) != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return -1;

    int count = 0;
    rocketnet_message_record_t record;
    while ( count < max && rocketnet_message_reader_next( &reader, &record ) )
    {
        out[count++] = record;
    }

    return count;
}

// RNEndpoint WriteOk: an endpoint's first packet carries CONFIG first, code 2
// with CONTROL set, the connection's first reliable number, and the 13.4 body
// of the sender's config.

static void mc_check_first_record_is_config( const mc_endpoint_t * sender, const rocketnet_message_limits_t * limits, const rocketnet_message_config_t * config )
{
    check( !sender->sent_log.empty() );
    if ( sender->sent_log.empty() )
        return;

    rocketnet_message_record_t records[16];
    const int count = mc_records( sender->sent_log[0], limits, records, 16 );
    check( count >= 1 );
    if ( count < 1 )
        return;
    check( records[0].code == ROCKETNET_MESSAGE_CODE_RELIABLE );
    check( records[0].control );
    check( !records[0].block );
    check( records[0].number == 0 );
    check( records[0].bytes == ROCKETNET_MESSAGE_CONFIG_BYTES );

    rocketnet_message_config_body_t body;
    check( rocketnet_message_read_config_body( records[0].data, records[0].bytes, &body ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( body.version == ROCKETNET_MESSAGE_CONFIG_VERSION );
    check( body.enabled == ( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION & 0xFFu ) );
    check( body.num_channels == (uint32_t) config->num_channels );
    check( body.reliable_messages_in_flight == (uint32_t) config->reliable_messages_in_flight );
    check( body.receive_message_buffer_bytes == (uint32_t) config->receive_message_buffer_bytes );
    check( body.max_message_bytes == (uint32_t) config->max_message_bytes );
    check( body.fragment_bytes == ROCKETNET_MESSAGE_FRAGMENT_BYTES );
}

// the CONFIG records a sender finished, first sends and resends

static int mc_config_sends( const mc_endpoint_t * sender, const rocketnet_message_limits_t * limits )
{
    int sends = 0;

    for ( size_t p = 0; p < sender->sent_log.size(); p++ )
    {
        rocketnet_message_record_t records[16];
        const int count = mc_records( sender->sent_log[p], limits, records, 16 );
        for ( int i = 0; i < count; i++ )
            if ( records[i].control )
                sends++;
    }

    return sends;
}

// RNEndpoint Reset, WriteOk and OnPayload: equal configs connect and both
// ends verify. CONFIG goes first even when the application queued a message
// before the first packet, and that message's record takes reliable number 1.

static void test_message_config_equal_connects_and_verifies()
{
    rocketnet_message_config_t config;
    mc_config( &config );

    mc_pair_t pair;
    mc_pair_setup( &pair, 1, &config );

    // queued before the client's first packet: it travels behind CONFIG

    uint8_t data[64];
    mc_fill( data, 7, sizeof( data ) );
    check( rocketnet_message_client_send( pair.clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, data, sizeof( data ) ) != 0 );

    std::vector<mc_received_t> server_got;
    for ( int i = 0; i < 10; i++ )
    {
        mc_step( &pair, &server_got );
    }

    rn_message_connection_t * server_slot = mc_server_slot( &pair, 0 );
    rn_message_connection_t * client = mc_client_connection( pair.clients[0] );
    check( server_slot->ordered.config_received );
    check( client->ordered.config_received );
    check( rocketnet_message_client_disconnect_reason( pair.clients[0] ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( pair.net->clients[0]->disconnects == 0 );
    check( pair.net->server->disconnects == 0 );

    // each end's first record is its CONFIG (read with the other end's limits)

    mc_check_first_record_is_config( pair.net->clients[0], &server_slot->ordered.limits, &config );
    mc_check_first_record_is_config( pair.net->server, &client->ordered.limits, &config );

    // the application's message arrived once, intact, numbered after CONFIG

    check( server_got.size() == 1 );

    uint32_t id = 0;
    check( server_got.size() == 1 && mc_verify( server_got[0].bytes, &id ) && id == 7 );

    bool found = false;
    for ( size_t p = 0; p < pair.net->clients[0]->sent_log.size() && !found; p++ )
    {
        rocketnet_message_record_t records[16];
        const int count = mc_records( pair.net->clients[0]->sent_log[p], &server_slot->ordered.limits, records, 16 );
        for ( int i = 0; i < count; i++ )
            if ( records[i].code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED )
            {
                check( records[i].number == 1 );
                found = true;
                break;
            }
    }

    check( found );

    // CONFIG is never an application delivery

    check( mc_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) == 0 );

    mc_pair_teardown( &pair );
}

// RNEndpoint ConfigReliable (the reversed witness ConfigVerifiedDespite-
// FirstLoss): the first packet each way is lost; CONFIG is resent until acked
// like any reliable record, and both ends still verify.

static void test_message_config_first_loss_still_verifies()
{
    rocketnet_message_config_t config;
    mc_config( &config );

    mc_pair_t pair;
    mc_pair_setup( &pair, 1, &config );

    pair.net->drop_next_to_server = 1;                          // the client's first packet: its CONFIG
    pair.net->drop_to_client = true;                            // the server's first packet: its CONFIG
    mc_step( &pair );

    rn_message_connection_t * server_slot = mc_server_slot( &pair, 0 );
    rn_message_connection_t * client = mc_client_connection( pair.clients[0] );
    check( !server_slot->ordered.config_received );
    check( !client->ordered.config_received );
    pair.net->drop_to_client = false;

    for ( int i = 0; i < 100; i++ )
    {
        mc_step( &pair );
    }

    check( server_slot->ordered.config_received );
    check( client->ordered.config_received );
    check( rocketnet_message_client_disconnect_reason( pair.clients[0] ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( pair.net->clients[0]->disconnects == 0 );
    check( pair.net->server->disconnects == 0 );

    // each CONFIG went out at least twice: the lost send and its resend

    check( mc_config_sends( pair.net->clients[0], &server_slot->ordered.limits ) >= 2 );
    check( mc_config_sends( pair.net->server, &client->ordered.limits ) >= 2 );
    check( mc_counter( rocketnet_message_client_counters( pair.clients[0] ), ROCKETNET_COUNTER_MESSAGE_RESENT ) >= 1 );
    check( mc_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_RESENT ) >= 1 );

    // every CONFIG resend is acked: nothing reliable is left unacked

    check( client->ordered.send.U >= client->ordered.send.next );
    check( server_slot->ordered.send.U >= server_slot->ordered.send.next );

    mc_pair_teardown( &pair );
}

// RNEndpoint Tick, the CONFIG half: the server's packets never reach the
// client, while the client's reach the server and are acked. The client has
// nothing reliable unacked, yet it is still waiting for the server's CONFIG, so
// reliable_timeout after it connected it ends the connection as
// RELIABLE_TIMEOUT. Red at a base without CONFIG: nothing ever times out.

static void test_message_config_never_received_is_reliable_timeout()
{
    rocketnet_message_config_t config;
    mc_config( &config );
    config.reliable_message_timeout_ms = 1000;

    mc_pair_t pair;
    mc_pair_setup( &pair, 1, &config );
    pair.net->drop_to_client = true;

    rn_message_connection_t * client = mc_client_connection( pair.clients[0] );

    // 0.9 s: still waiting, still connected, and the client's own CONFIG acked

    for ( int i = 0; i < 9; i++ )
    {
        mc_step( &pair, NULL, NULL, 0.1 );
    }
    check( !client->ordered.config_received );
    check( client->ordered.send.U >= client->ordered.send.next );
    check( client->ordered.send.next >= 1 );
    check( rocketnet_message_client_disconnect_reason( pair.clients[0] ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( pair.net->clients[0]->disconnects == 0 );

    // past reliable_timeout: the client ends it. The server's CONFIG went out
    // in the same update the client's wait began, so both ends reach
    // reliable_timeout in the same update, and each checks it in its update
    // (rn_endpoint_tick). The client updates first here so that its own
    // CONFIG wait, not the server's kick, ends its connection.

    for ( int i = 0; i < 5; i++ )
    {
        mc_step_client_first( &pair, 0.1 );
    }
    check( rocketnet_message_client_disconnect_reason( pair.clients[0] ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( pair.net->clients[0]->disconnects == 1 );

    // the server's CONFIG was never acked: it times out too (RNEndpoint Tick)

    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );

    mc_pair_teardown( &pair );
}
#endif // #if ROCKETNET_DEV

// STANDARD.md labels the CONFIG defaults vector; this case regenerates it
// from the library's own defaults (standard/MESSAGES.md m11) and pins it byte for
// byte: enabled 0xFF, all eight types of the first version's mask (7.5).
// Red if the default mask or the default config drifts from the standard.

static void test_config_vector_is_library_default()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    check( config.message_resend_delay_initial_ms == 2000 );
    check( config.message_resend_delay_min_ms == 0 );
    check( config.message_resend_delay_max_ms == 2000 );

    // the body rn_ordered_announce_config writes for a NULL/default config

    struct rocketnet_message_config_body_t body;
    body.version = ROCKETNET_MESSAGE_CONFIG_VERSION;
    body.enabled = ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION & 0xFFu;
    body.num_channels = (uint32_t) config.num_channels;
    body.reliable_messages_in_flight = (uint32_t) config.reliable_messages_in_flight;
    body.receive_message_buffer_bytes = (uint32_t) config.receive_message_buffer_bytes;
    body.max_message_bytes = (uint32_t) config.max_message_bytes;
    body.fragment_bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;

    // the first version's mask is all eight types: the intended default

    check( body.enabled == 0xFF );

    uint8_t config_bytes[ROCKETNET_MESSAGE_CONFIG_BYTES];
    rocketnet_message_write_config_body( config_bytes, &body );

    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.data = config_bytes;
    record.bytes = ROCKETNET_MESSAGE_CONFIG_BYTES;
    record.control = 1;

    static const uint8_t expected[] = { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
                                        0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    check( rocketnet_message_writer_add( &writer, &record ) == 1 );
    check( rocketnet_message_writer_finish( &writer ) == (int) sizeof( expected ) );
    check( memcmp( payload, expected, sizeof( expected ) ) == 0 );
}

// The sent-packet map against the window (standard/MESSAGES.md m9.1; proofs/tla/RNSentMap.tla NoLiveEviction): under the
// library's default window and map (the receive ring and the store raised
// to hold the burst), a client with no send budget writes 1000 reliable
// messages of 800 bytes, one to a packet, while the transport holds every
// ack: about a window of packets inside one round trip. Then the acks land. Every
// packet's entry is still in the map when its ack arrives: nothing is
// evicted, nothing is resent, and the sender's U reaches next. Red with the
// old default of 256 entries: the burst evicts its own entries and their acks
// are ignored.

#if ROCKETNET_DEV
static void test_message_config_sent_map_holds_a_window_burst()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = 1 << 20;  // the receive ring and the store hold the whole unacked
    config.send_message_buffer_bytes = 1 << 22;     // burst; W and P stay the defaults

    mc_pair_t pair;
    mc_pair_setup( &pair, 1, &config, -1 );           // below 0: no client send budget (11.5)

    const int messages = 1000;                      // with CONFIG, 1001 of the window's 1024 numbers
    const int bytes = 800;                          // two never share a ROCKETNET_MAX_PACKET_PAYLOAD_BYTES packet
    check( 2 * bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    check( messages < config.reliable_messages_in_flight );

    uint8_t data[bytes];

    for ( int i = 0; i < messages; i++ )
    {
        mc_fill( data, (uint32_t) i, (uint32_t) bytes );
        check( rocketnet_message_client_send( pair.clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, data, bytes ) != 0 );
    }

    // the burst: every first send leaves before any ack lands

    std::vector<mc_received_t> server_got;
    pair.net->hold_acks = true;
    for ( int i = 0; i < 6; i++ )
    {
        mc_step( &pair, &server_got, NULL, 0.001 );
    }
    check( server_got.size() == (size_t) messages );

    rn_message_connection_t * client = mc_client_connection( pair.clients[0] );
    check( client->ordered.send.next >= (uint64_t) messages );

    // the acks land, well inside the 2 s initial resend delay

    pair.net->hold_acks = false;
    for ( int i = 0; i < 4; i++ )
    {
        mc_step( &pair, &server_got, NULL, 0.001 );
    }

    const rocketnet_counters_t * counters = rocketnet_message_client_counters( pair.clients[0] );
    check( mc_counter( counters, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 0 );
    check( mc_counter( counters, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 0 );
    check( client->ordered.send.U == client->ordered.send.next );
    check( server_got.size() == (size_t) messages );
    check( rocketnet_message_client_disconnect_reason( pair.clients[0] ) == ROCKETNET_DISCONNECT_REASON_NONE );

    mc_pair_teardown( &pair );
}
#endif // #if ROCKETNET_DEV

// The fuzz seed f8 0d 00 30 (tests/fuzz/corpus_config/regress_config_f80d0030)
// applies "0" to message_resend_delay_max_ms (range 10..60000, zero is not
// "use the default"). The harness expects validation to refuse it exactly as
// it refuses any out-of-range INT; this pins that the library does, by setting
// the struct field directly, and that the refusal never clamps the value.

static void test_message_config_resend_delay_max_zero_refused()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_message_config_fields( &num_fields );
    check( rocketnet_config_validate( "client", &config, fields, num_fields ) );
    config.message_resend_delay_max_ms = 0;
    check( !rocketnet_config_validate( "client", &config, fields, num_fields ) );
    check( config.message_resend_delay_max_ms == 0 );
    config.message_resend_delay_max_ms = 9;
    check( !rocketnet_config_validate( "client", &config, fields, num_fields ) );
    check( config.message_resend_delay_max_ms == 9 );
    config.message_resend_delay_max_ms = 60001;
    check( !rocketnet_config_validate( "client", &config, fields, num_fields ) );
    check( config.message_resend_delay_max_ms == 60001 );
    config.message_resend_delay_max_ms = 2000;
    check( rocketnet_config_validate( "client", &config, fields, num_fields ) );
}

// The message table's range is the table's own; the message config's contract
// is the range and the cross-field resend rules together (the initial delay
// must sit inside the min..max the operator chose). A value the table's range
// accepts can still break the cross rule: a min above the initial delay is
// refused by the cross rule alone.

static void test_message_config_resend_delay_cross_rule_refused()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_message_config_fields( &num_fields );

    // 60000 is inside the table's range for the min (0..60000), so the table
    // accepts it; the cross rule does not, because the initial delay (2000)
    // would sit below the floor

    config.message_resend_delay_min_ms = 60000;
    check( rocketnet_config_validate( "client", &config, fields, num_fields ) );
    check( !rocketnet_message_validate_config( &config ) );
    check( config.message_resend_delay_min_ms == 60000 );
    config.message_resend_delay_min_ms = 0;
    check( rocketnet_config_validate( "client", &config, fields, num_fields ) );
    check( rocketnet_message_validate_config( &config ) );
}

// The resend reserve is sized for the send rate over the longest resend delay,
// and its receive-reserve span bytes SR must fit in 32 bits. Memory is the
// application's to choose, so this is no budget: it is the one bound the
// region layout cannot represent past, and validation refuses a config past
// it exactly as create does, with the same status, so a validate never
// reports OK for a config a create refuses. With the defaults and
// message_resend_delay_max_ms 60000, 565169 kbps puts SR at 4294960512 bytes
// and 565170 kbps at 4294968128, just past UINT32_MAX. Nothing is allocated:
// the accepted config is only sized, and the refused create fails before it
// allocates.

static void test_message_config_reserve_past_32_bits_refused_by_validate_and_create()
{
    const int last_kbps = 565169;
    const int past_kbps = 565170;

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_max_ms = 60000;

    int status = -1;
    config.client_send_kbps = last_kbps;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 1 );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_message_connection_bytes( &config ) > (uint64_t) UINT32_MAX );

    status = -1;
    config.client_send_kbps = past_kbps;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    check( rocketnet_message_connection_bytes( &config ) == 0 );
    check( rocketnet_message_client_required_memory( &config ) == 0 );

    // the public client at the same rate: create refuses with the status the
    // validate reported, and required_memory gives no size

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    memset( client_config.packet_filter, 0x5A, sizeof( client_config.packet_filter ) );   // not the published test filter, which a production build refuses first
    client_config.message_resend_delay_max_ms = 60000;
    client_config.send_kbps = past_kbps;

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &create_status ) == NULL );
    check( (int) create_status == status );
    check( rocketnet_client_required_memory( &client_config ) == 0 );
}

#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_message_config_mismatch_ends_both_connections );
REGISTER_UNIT_TEST( test_message_config_equal_connects_and_verifies );
REGISTER_UNIT_TEST( test_message_config_first_loss_still_verifies );
REGISTER_UNIT_TEST( test_message_config_never_received_is_reliable_timeout );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_config_vector_is_library_default );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_message_config_sent_map_holds_a_window_burst );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_message_config_resend_delay_max_zero_refused );
REGISTER_UNIT_TEST( test_message_config_resend_delay_cross_rule_refused );
REGISTER_UNIT_TEST( test_message_config_reserve_past_32_bits_refused_by_validate_and_create );
