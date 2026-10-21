/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Broadcast on the message server (docs/CHANNELS.md section 11.4, "Broadcast"
// and the R3 copy rule). A broadcast is one copy per connection: each
// connection's region holds its own copy, the R3 copy this API cannot avoid.
// rocketnet_message_server_broadcast sends the bytes to every connected slot
// exactly as rocketnet_message_server_send would and returns the number of
// slots it was queued for; a slot that refuses (exhausted, not connected) is
// skipped and counted, never retried.
//
// These tests drive three connected clients over an in-memory link built the
// same way test_unit_message_objects.cpp builds its pair: the object's
// transport seam (rn_message_link_t) hands out frames at start_packet, takes
// them at finish_packet, opens each packet once on the far side into a payload
// batch, and reports the opened ids back as acked at the sender's next update.
// Time is only an argument: nothing sleeps, nothing opens a socket.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_connect_token.h"
#include "sdk/rocketnet_message_object.h"
#include "sdk/rocketnet_send_budget.h"

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// the in-memory transport pair
// ---------------------------------------------------------------------------

#define MB_MAX_SLOTS        4
#define MB_PAYLOAD_INDEX    16
#define MB_MAX_FRAMES       64

struct mb_packet_t
{
    uint64_t id;
    int slot;
    int bytes;
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
};

struct mb_net_t;

struct mb_endpoint_t
{
    mb_net_t * net;
    bool is_server;
    int slot;

    alignas( 16 ) uint8_t frames[MB_MAX_FRAMES][MB_PAYLOAD_INDEX + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    bool frame_used[MB_MAX_FRAMES];
    uint64_t frame_id[MB_MAX_FRAMES];
    int frame_slot[MB_MAX_FRAMES];

    std::vector<mb_packet_t> inbound;
    std::vector<mb_packet_t> batch;
    std::vector<uint8_t *> batch_data;
    std::vector<int> batch_bytes;
    std::vector<int> batch_slot;

    std::vector<uint64_t> acks_inbox;
    std::vector<int> acks_inbox_slot;
    std::vector<uint64_t> acked;
    std::vector<int> acked_slot;

    bool connected;
    int disconnects;
    int finishes;
    double time;
};

struct mb_net_t
{
    mb_endpoint_t * server;
    mb_endpoint_t * clients[MB_MAX_SLOTS];
    uint64_t slot_connection[MB_MAX_SLOTS];
    uint64_t next_connection;
    uint64_t next_packet_id;
};

static mb_net_t * mb_net_new()
{
    mb_net_t * net = new mb_net_t();
    memset( net->slot_connection, 0, sizeof( net->slot_connection ) );
    net->server = new mb_endpoint_t();
    net->server->net = net;
    net->server->is_server = true;
    net->server->slot = -1;
    net->server->connected = false;
    net->server->disconnects = 0;
    net->server->finishes = 0;
    net->server->time = 0.0;
    for ( int i = 0; i < MB_MAX_SLOTS; i++ )
        net->clients[i] = NULL;
    net->next_connection = 100;
    net->next_packet_id = 1000;
    return net;
}

static mb_endpoint_t * mb_net_client( mb_net_t * net, int slot )
{
    mb_endpoint_t * client = new mb_endpoint_t();
    client->net = net;
    client->is_server = false;
    client->slot = slot;
    client->connected = false;
    client->disconnects = 0;
    client->finishes = 0;
    client->time = 0.0;
    net->clients[slot] = client;
    return client;
}

static void mb_net_connect( mb_net_t * net, int slot )
{
    net->slot_connection[slot] = net->next_connection++;
    if ( net->clients[slot] )
        net->clients[slot]->connected = true;
}

static void mb_net_drop_slot( mb_net_t * net, int slot )
{
    net->slot_connection[slot] = 0;
    if ( net->clients[slot] )
        net->clients[slot]->connected = false;
}

static void mb_net_free( mb_net_t * net )
{
    for ( int i = 0; i < MB_MAX_SLOTS; i++ )
        delete net->clients[i];
    delete net->server;
    delete net;
}

static void mb_update( void * transport, double time )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    mb_net_t * net = e->net;
    e->time = time;

    e->acked.clear();
    e->acked_slot.clear();
    e->acked.swap( e->acks_inbox );
    e->acked_slot.swap( e->acks_inbox_slot );

    e->batch.clear();
    e->batch_data.clear();
    e->batch_bytes.clear();
    e->batch_slot.clear();
    e->batch.reserve( e->inbound.size() );
    for ( size_t i = 0; i < e->inbound.size(); i++ )
    {
        const mb_packet_t & p = e->inbound[i];
        if ( e->is_server && net->slot_connection[p.slot] == 0 )
            continue;
        if ( !e->is_server && !e->connected )
            continue;
        e->batch.push_back( p );
        mb_endpoint_t * sender = e->is_server ? net->clients[p.slot] : net->server;
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

static int mb_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    *client_index = e->is_server ? e->batch_slot.data() : NULL;
    *payload_data = e->batch_data.data();
    *payload_bytes = e->batch_bytes.data();
    return (int) e->batch.size();
}

static int mb_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    *client_index = e->is_server ? e->acked_slot.data() : NULL;
    *packet_ids = e->acked.data();
    return (int) e->acked.size();
}

static uint8_t * mb_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    memset( context, 0, sizeof( *context ) );
    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return NULL;
    if ( e->is_server ? e->net->slot_connection[client_index] == 0 : !e->connected )
        return NULL;
    for ( int f = 0; f < MB_MAX_FRAMES; f++ )
    {
        if ( e->frame_used[f] )
            continue;
        e->frame_used[f] = true;
        e->frame_id[f] = e->net->next_packet_id++;
        e->frame_slot[f] = e->is_server ? client_index : e->slot;
        context->frame = e->frames[f];
        context->payload_index = MB_PAYLOAD_INDEX;
        context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        context->packet_id = e->frame_id[f];
        context->client_index = client_index;
        return e->frames[f];
    }
    return NULL;
}

static int mb_frame_of( mb_endpoint_t * e, const rocketnet_packet_context_t * context )
{
    for ( int f = 0; f < MB_MAX_FRAMES; f++ )
        if ( e->frame_used[f] && context->frame == e->frames[f] )
            return f;
    return -1;
}

static int mb_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    const int f = mb_frame_of( e, context );
    check( f >= 0 );
    check( payload_bytes >= 1 && payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    e->frame_used[f] = false;
    e->finishes++;

    mb_packet_t packet;
    packet.id = e->frame_id[f];
    packet.slot = e->frame_slot[f];
    packet.bytes = payload_bytes;
    memcpy( packet.data, e->frames[f] + MB_PAYLOAD_INDEX, (size_t) payload_bytes );

    mb_net_t * net = e->net;
    if ( e->is_server )
    {
        mb_endpoint_t * to = net->clients[packet.slot];
        if ( to )
            to->inbound.push_back( packet );
    }
    else
    {
        net->server->inbound.push_back( packet );
    }

    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_bytes );
}

static void mb_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    const int f = mb_frame_of( e, context );
    if ( f >= 0 )
        e->frame_used[f] = false;
}

static void mb_send_packets( void * transport )
{
    (void) transport;
}

static uint64_t mb_connection( void * transport, int client_index )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    if ( e->is_server )
        return e->net->slot_connection[client_index];
    return e->connected ? 1 : 0;
}

static void mb_disconnect( void * transport, int client_index )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    e->disconnects++;
    if ( e->is_server )
        mb_net_drop_slot( e->net, client_index );
    else
        e->connected = false;
}

static int mb_state( void * transport )
{
    mb_endpoint_t * e = (mb_endpoint_t *) transport;
    if ( e->is_server )
        return ROCKETNET_SERVER_TRANSPORT_RUNNING;
    return e->connected ? ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED : ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
}

static rn_message_link_t mb_link( mb_endpoint_t * e )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = e;
    link.update = mb_update;
    link.payloads = mb_payloads;
    link.acked = mb_acked;
    link.start_packet = mb_start_packet;
    link.finish_packet = mb_finish_packet;
    link.abort_packet = mb_abort_packet;
    link.send_packets = mb_send_packets;
    link.connection = mb_connection;
    link.disconnect = mb_disconnect;
    link.state = mb_state;
    link.destroy = NULL;                                        // the harness owns the pair
    return link;
}

// ---------------------------------------------------------------------------
// objects on the pair
// ---------------------------------------------------------------------------

static void mb_config( rocketnet_message_config_t * config )
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

static void mb_small_config( rocketnet_message_config_t * config )
{
    mb_config( config );
    config->max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config->send_message_buffer_bytes = 12288;
    config->send_queue_messages = 16;
}

#define MB_T0 1000.0

struct mb_pair_t
{
    mb_net_t * net;
    rocketnet_message_server_t * server;
    rocketnet_message_client_t * clients[MB_MAX_SLOTS];
    int num_clients;
    double time;
};

static rocketnet_message_server_t * mb_server_create_seam( mb_net_t * net, const rocketnet_message_config_t * config, int slots, uint32_t kbps )
{
    (void) slots;
    rn_message_link_t link = mb_link( net->server );
    return (rocketnet_message_server_t *) rn_message_object_create( NULL, &link, 1, MB_MAX_SLOTS, config, NULL, 0, kbps, MB_T0 );
}

static rocketnet_message_client_t * mb_client_create( mb_net_t * net, int slot, const rocketnet_message_config_t * config, uint32_t kbps )
{
    mb_endpoint_t * e = mb_net_client( net, slot );
    rn_message_link_t link = mb_link( e );
    return (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, config, NULL, 0, kbps, MB_T0 );
}

static void mb_pair_setup( mb_pair_t * pair, int num_clients, const rocketnet_message_config_t * config = NULL,
                           uint32_t client_kbps = ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, uint32_t server_kbps = ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS )
{
    rocketnet_message_config_t defaults;
    if ( !config )
    {
        mb_config( &defaults );
        config = &defaults;
    }
    memset( pair, 0, sizeof( *pair ) );
    pair->net = mb_net_new();
    pair->num_clients = num_clients;
    pair->server = mb_server_create_seam( pair->net, config, MB_MAX_SLOTS, server_kbps );
    check( pair->server );
    for ( int i = 0; i < num_clients; i++ )
    {
        pair->clients[i] = mb_client_create( pair->net, i, config, client_kbps );
        check( pair->clients[i] );
        mb_net_connect( pair->net, i );
    }
    pair->time = MB_T0;
}

static void mb_pair_teardown( mb_pair_t * pair )
{
    for ( int i = 0; i < pair->num_clients; i++ )
        rocketnet_message_client_destroy( pair->clients[i] );
    rocketnet_message_server_destroy( pair->server );
    mb_net_free( pair->net );
}

// every message the application received this update, released before the next

struct mb_received_t
{
    int channel;
    std::vector<uint8_t> bytes;
};

static void mb_drain_client( rocketnet_message_client_t * client, std::vector<mb_received_t> * out )
{
    {
        const uint8_t * data = NULL;
        int bytes = 0;
        int channel = 0;
        while ( rocketnet_message_client_receive( client, &channel, &data, &bytes ) )
        {
            mb_received_t r;
            r.channel = channel;
            r.bytes.assign( data, data + bytes );
            out->push_back( r );
            rocketnet_message_client_release( client, data );
        }
    }
}

// one step of the whole pair: every object updates, each client's application
// drains into its own vector, every object sends

static void mb_step( mb_pair_t * pair, std::vector<mb_received_t> * got = NULL, int steps = 1 )
{
    for ( int s = 0; s < steps; s++ )
    {
        pair->time += 0.01;
        check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
        for ( int i = 0; i < pair->num_clients; i++ )
            check( rocketnet_message_client_update( pair->clients[i], pair->time ) == ROCKETNET_UPDATE_OK );
        if ( got )
            for ( int i = 0; i < pair->num_clients; i++ )
                mb_drain_client( pair->clients[i], &got[i] );
        rocketnet_message_server_send_packets( pair->server );
        for ( int i = 0; i < pair->num_clients; i++ )
            rocketnet_message_client_send_packets( pair->clients[i] );
    }
}

static void mb_fill( uint8_t * data, int bytes, uint32_t id )
{
    for ( int i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( id * 131 + (uint32_t) i * 7 + 5 );
    if ( bytes >= 4 )
        memcpy( data, &id, 4 );
}

// ---------------------------------------------------------------------------
// CHANNELS 11.4 Broadcast: three connected slots each receive the one broadcast
// exactly once, on the channel with the reliability asked; the return value is
// the queued slot count
// ---------------------------------------------------------------------------

static void test_message_server_broadcast_reaches_every_connected()
{
    mb_pair_t pair;
    mb_pair_setup( &pair, 3 );
    mb_step( &pair );                                           // Observe binds all three slots

    uint8_t message[128];
    mb_fill( message, (int) sizeof( message ), 4100 );

    // one broadcast, the copying convenience per connected connection
    check( rocketnet_message_server_broadcast( pair.server, 2, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) == 3 );

    std::vector<mb_received_t> got[MB_MAX_SLOTS];
    mb_step( &pair, got, 6 );

    for ( int i = 0; i < 3; i++ )
    {
        check( got[i].size() == 1 );                            // exactly once
        check( got[i][0].channel == 2 );                        // on the channel asked
        check( got[i][0].bytes.size() == sizeof( message ) );
        check( memcmp( got[i][0].bytes.data(), message, sizeof( message ) ) == 0 );
    }

    // all three slots are still connected after the broadcast
    check( rocketnet_message_server_client_connected( pair.server, 0 ) == 1 );
    check( rocketnet_message_server_client_connected( pair.server, 1 ) == 1 );
    check( rocketnet_message_server_client_connected( pair.server, 2 ) == 1 );

    mb_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// CHANNELS 11.4 Broadcast: a free slot and an exhausted slot are skipped and
// counted; the return value names how many slots were queued
// ---------------------------------------------------------------------------

static void test_message_server_broadcast_skips_free_and_exhausted()
{
    rocketnet_message_config_t config;
    mb_small_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit capacity
    mb_pair_t pair;
    mb_pair_setup( &pair, 3, &config, 0, 0 );
    mb_step( &pair );

    // slot 1 is free: its transport connection is gone before the broadcast
    mb_net_drop_slot( pair.net, 1 );
    mb_step( &pair );
    check( rocketnet_message_server_client_connected( pair.server, 1 ) == 0 );

    // slot 2 is exhausted: queue to it until its region finds no room, which
    // kicks the connection with the named reason
    int started = 0;
    for ( ;; )
    {
        uint8_t * q = rocketnet_message_server_start( pair.server, 2, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 );
        if ( !q )
            break;
        mb_fill( q, 100, (uint32_t) started );
        check( rocketnet_message_server_finish( pair.server, 2, 100 ) != 0 );
        started++;
        check( started <= 1000 );
    }
    check( started > 0 );
    check( rocketnet_message_server_client_connected( pair.server, 2 ) == 0 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 2 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( pair.net->slot_connection[2] == 0 );

    const uint64_t drops = rocketnet_counter_value( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );

    uint8_t message[64];
    mb_fill( message, (int) sizeof( message ), 4200 );

    // only slot 0 is connected; slots 1 (free), 2 (exhausted) and 3 (free) are
    // skipped, each dropped through the same send path that counts it
    check( rocketnet_message_server_broadcast( pair.server, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) == 1 );
    check( rocketnet_counter_value( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) == drops + 3 );

    std::vector<mb_received_t> got[MB_MAX_SLOTS];
    mb_step( &pair, got, 6 );

    check( got[0].size() == 1 );                                // the one connected slot got it once
    check( got[0][0].channel == 1 );
    check( got[0][0].bytes.size() == sizeof( message ) );
    check( memcmp( got[0][0].bytes.data(), message, sizeof( message ) ) == 0 );
    check( got[1].empty() );                                    // the free slot got nothing
    check( got[2].empty() );                                    // the exhausted slot got nothing

    mb_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_message_server_broadcast_reaches_every_connected );
REGISTER_UNIT_TEST( test_message_server_broadcast_skips_free_and_exhausted );

// ---------------------------------------------------------------------------
// The counted allocator, as test_unit_message_objects.cpp carries its mo_*
// pair: a create that refuses before the transport opens a socket allocates
// nothing.
// ---------------------------------------------------------------------------

static uint64_t mcr_mallocs = 0;

static void * mcr_malloc( void * context, size_t bytes )
{
    (void) context;
    mcr_mallocs++;
    return malloc( bytes );
}

static void mcr_free( void * context, void * p )
{
    (void) context;
    free( p );
}

// ---------------------------------------------------------------------------
// The client create refuses an out-of-range send budget or a config whose
// required memory is zero before rocketnet_client_transport_create opens a
// socket, allocating nothing (sdk/rocketnet_message_client.cpp,
// rocketnet_message_client_create).
// ---------------------------------------------------------------------------

static void test_message_client_create_refuses_budget_and_config()
{
    rocketnet_term();
    check( rocketnet_allocator( mcr_malloc, mcr_free ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

    // a config whose required memory is zero: num_channels = 0 fails
    // rocketnet_message_validate_config (1..ROCKETNET_MAX_CHANNELS), so
    // rocketnet_message_client_required_memory returns 0
    rocketnet_message_config_t bad;
    rocketnet_message_default_config( &bad );
    bad.num_channels = 0;
    check( rocketnet_message_client_required_memory( &bad ) == 0 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );

    const uint64_t before = mcr_mallocs;

    // a send budget above the maximum: refused before any allocation
    check( rocketnet_message_client_create( NULL, token, NULL, NULL, NULL, 0, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == NULL );
    // an invalid message config (zero required memory): refused before any allocation
    check( rocketnet_message_client_create( NULL, token, NULL, &bad, NULL, 0, 0 ) == NULL );

    check( mcr_mallocs == before );

    rocketnet_term();
    check( rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// The server create refuses an out-of-range per-client send budget or a config
// whose required memory is zero before rocketnet_server_transport_create opens
// a socket, allocating nothing (sdk/rocketnet_message_server.cpp,
// rocketnet_message_server_create).
// ---------------------------------------------------------------------------

static void test_message_server_create_refuses_budget_and_config()
{
    rocketnet_term();
    check( rocketnet_allocator( mcr_malloc, mcr_free ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

    rocketnet_message_config_t bad;
    rocketnet_message_default_config( &bad );
    bad.num_channels = 0;
    check( rocketnet_message_server_required_memory( NULL, &bad ) == 0 );

    const uint64_t before = mcr_mallocs;

    // a send budget above the maximum: refused before any allocation
    check( rocketnet_message_server_create( NULL, "127.0.0.1:40000", NULL, NULL, NULL, 0, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == NULL );
    // an invalid message config (zero required memory): refused before any allocation
    check( rocketnet_message_server_create( NULL, "127.0.0.1:40000", NULL, &bad, NULL, 0, 0 ) == NULL );

    check( mcr_mallocs == before );

    rocketnet_term();
    check( rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_message_client_create_refuses_budget_and_config );
REGISTER_UNIT_TEST( test_message_server_create_refuses_budget_and_config );
