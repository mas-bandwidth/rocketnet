/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public client and server lifecycle (tla/ClientSession.tla,
// tla/ServerSession.tla): a client connects, disconnects and connects again;
// a server starts, stops and starts again. Everything runs in one process
// over the in-memory loopback transport with step-driven clients: no socket,
// no thread, no sleep, and time is an argument starting at 1000.0.

#include "test_unit_registry.h"
#include "sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_packet_builder.h"
#include "test/token_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define LIFECYCLE_PROTOCOL 0x7A11C1C1E5EED001ULL
#define LIFECYCLE_MAX_CLIENTS 4

struct lifecycle_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
};

static double lifecycle_now = 1000.0;
static double lifecycle_clock( void * ) { return lifecycle_now; }

static void lifecycle_identity( lifecycle_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
}

// set nonzero by a test that queues more messages per connection than the
// defaults hold: the window, the send queue and the send buffer then fit
// that many messages, on the server and every client the world creates
static int lifecycle_queue_messages = 0;

static void lifecycle_queue_config( int * reliable_messages_in_flight, int * send_queue_messages, int * send_message_buffer_bytes )
{
    if ( lifecycle_queue_messages == 0 )
        return;
    *reliable_messages_in_flight = lifecycle_queue_messages;
    *send_queue_messages = lifecycle_queue_messages;
    *send_message_buffer_bytes = lifecycle_queue_messages * 256;      // a send-store chunk per message, with room
}

static void lifecycle_server_config( rocketnet_server_config_t * config, const lifecycle_identity_t * identity, int max_clients )
{
    rocketnet_server_default_config( config );
    config->protocol_id = LIFECYCLE_PROTOCOL;
    config->max_clients = max_clients;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
    lifecycle_queue_config( &config->reliable_messages_in_flight, &config->send_queue_messages, &config->send_message_buffer_bytes );
}

static void lifecycle_client_config( rocketnet_client_test_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = lifecycle_clock;
    lifecycle_queue_config( &config->reliable_messages_in_flight, &config->send_queue_messages, &config->send_message_buffer_bytes );
}

// the token string sealed to the server's own key, naming 127.0.0.1:port
static void lifecycle_token( const rocketnet_server_t * server, uint16_t port, const lifecycle_identity_t * identity, uint64_t client_id, int32_t timeout_seconds, char * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rn_test_server_token_string( identity->backend_secret, server,
        LIFECYCLE_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, timeout_seconds, NULL,
        token ) == ROCKETNET_STATUS_OK );
}

struct lifecycle_world_t
{
    lifecycle_identity_t identity;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[LIFECYCLE_MAX_CLIENTS];
    int num_clients;
    double time;
};

static void lifecycle_world_create( lifecycle_world_t * world, int max_clients, int num_clients )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    lifecycle_now = world->time;
    lifecycle_identity( &world->identity );
    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &world->identity, max_clients );
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL );
    check( world->server != NULL );
    check( rocketnet_server_running( world->server ) == 0 );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_running( world->server ) == 1 );
    world->num_clients = num_clients;
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_test_config_t client_config;
        lifecycle_client_config( &client_config );
        world->clients[i] = rocketnet_client_create( NULL, &client_config, NULL );
        check( world->clients[i] != NULL );
        check( rocketnet_client_state( world->clients[i] ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    }
}

static void lifecycle_world_destroy( lifecycle_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
        rocketnet_client_destroy( world->clients[i] );
    rocketnet_server_destroy( world->server );
}

static void lifecycle_pump( lifecycle_world_t * world, double dt = 0.01 )
{
    lifecycle_now = world->time;
    for ( int i = 0; i < world->num_clients; i++ )
    {
        if ( !world->clients[i] )
            continue;
        rocketnet_client_update( world->clients[i], world->time );
        rocketnet_client_send_packets( world->clients[i] );
    }
    if ( rocketnet_server_running( world->server ) )
    {
        rocketnet_server_update( world->server, world->time );
        rocketnet_server_send_packets( world->server );
    }
    world->time += dt;
}

static void lifecycle_connect( lifecycle_world_t * world, int client, uint64_t client_id )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world->server, rocketnet_server_port( world->server ), &world->identity, client_id, 300, token );
    check( rocketnet_client_connect( world->clients[client], token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( world->clients[client] ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( world );
    check( rocketnet_client_state( world->clients[client] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int index = rocketnet_client_index( world->clients[client] );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world->server, index ); i++ )
        lifecycle_pump( world );
    check( rocketnet_server_client_connected( world->server, index ) != 0 );
    check( rocketnet_server_client_id( world->server, index ) == client_id );
}

static void lifecycle_client_send( rocketnet_client_t * client, uint8_t tag, uint8_t value )
{
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out != NULL );
    if ( !out )
        return;
    out[0] = tag; out[1] = value;
    check( rocketnet_client_finish_message( client, 2 ) != 0 );
}

static void lifecycle_server_send( rocketnet_server_t * server, uint64_t index, uint8_t tag, uint8_t value )
{
    uint8_t * out = rocketnet_server_start_message( server, index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out != NULL );
    if ( !out )
        return;
    out[0] = tag; out[1] = value;
    check( rocketnet_server_finish_message( server, 2 ) != 0 );
}

// Pumps until the server holds `want` messages from slot index and the client
// `want` from the server, all tagged `tag` and in order; anything with another
// tag fails the test.
static void lifecycle_exchange( lifecycle_world_t * world, int client, uint8_t tag, int want )
{
    rocketnet_client_t * c = world->clients[client];
    const int index = rocketnet_client_index( c );
    for ( int i = 0; i < want; i++ )
    {
        lifecycle_client_send( c, tag, (uint8_t) i );
        lifecycle_server_send( world->server, index, tag, (uint8_t) i );
    }
    int at_server = 0, at_client = 0;
    for ( int step = 0; step < 400 && ( at_server < want || at_client < want ); step++ )
    {
        lifecycle_pump( world );
        const uint8_t * data = NULL;
        int bytes = 0;
        uint64_t sender = ~0ULL;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
                sender = batch->client_index[i];
                check( sender == (uint64_t) index );
                check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_server );

                at_server++;
            }
        }
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( c ) ) != NULL )
        {
            for ( int i = 0; i < incoming->num_messages; i++ )
            {
                data = incoming->data[i];
                bytes = incoming->bytes[i];
                check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_client );

                at_client++;
            }
        }
    }
    check( at_server == want && at_client == want );
}

static void lifecycle_disconnect( lifecycle_world_t * world, int client )
{
    const int index = rocketnet_client_index( world->clients[client] );
    rocketnet_client_disconnect( world->clients[client] );
    check( rocketnet_client_state( world->clients[client] ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    for ( int i = 0; i < 200 && rocketnet_server_client_connected( world->server, index ); i++ )
        lifecycle_pump( world );
    check( rocketnet_server_client_connected( world->server, index ) == 0 );
}

static void test_client_reconnects_with_a_second_token()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];

    lifecycle_connect( &world, 0, 0x1001 );
    lifecycle_exchange( &world, 0, 0xA1, 3 );
    const uint64_t sent_first = rn_client_stats_of( client ).packets_sent;
    check( sent_first > 0 );
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_index( client ) == -1 );
    check( rocketnet_client_transport_name( client ) == NULL );

    // a second token with another client id: sequences and channels start
    // fresh, so the second exchange's values count from 0 again in order, and
    // nothing tagged by the first connection arrives
    lifecycle_connect( &world, 0, 0x2002 );
    lifecycle_exchange( &world, 0, 0xB2, 3 );
    check( rn_client_stats_of( client ).packets_sent > sent_first );
    check( rn_server_stats_of( world.server ).clients_connected == 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_reconnects_with_a_second_token );

static void test_client_destroy_then_create_and_connect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    lifecycle_connect( &world, 0, 0x1101 );
    lifecycle_exchange( &world, 0, 0xC1, 2 );
    const int index = rocketnet_client_index( world.clients[0] );
    rocketnet_client_destroy( world.clients[0] );
    world.clients[0] = NULL;
    for ( int i = 0; i < 200 && rocketnet_server_client_connected( world.server, index ); i++ )
        lifecycle_pump( &world );
    check( rocketnet_server_client_connected( world.server, index ) == 0 );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    check( world.clients[0] != NULL );
    lifecycle_connect( &world, 0, 0x1102 );
    lifecycle_exchange( &world, 0, 0xC2, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_destroy_then_create_and_connect );

static void test_client_connect_refused_unless_disconnected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1201, 300, token );

    // while connecting: refused, nothing changed
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    const int connecting = rocketnet_client_state( client );
    check( connecting > ROCKETNET_CLIENT_STATE_DISCONNECTED && connecting < ROCKETNET_CLIENT_STATE_CONNECTED );
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_client_state( client ) == connecting );

    // while connected: refused, nothing changed, the connection still carries
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    for ( int i = 0; i < 50; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    lifecycle_exchange( &world, 0, 0xD1, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_connect_refused_unless_disconnected );

static void test_server_start_refused_unless_stopped()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    const uint16_t port = rocketnet_server_port( world.server );
    check( port != 0 );
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_SERVER_NOT_STOPPED );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_server_running( world.server ) == 1 );
    check( rocketnet_server_port( world.server ) == port );
    lifecycle_connect( &world, 0, 0x1301 );
    lifecycle_exchange( &world, 0, 0xE1, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_start_refused_unless_stopped );

static void test_client_error_state_then_disconnect_then_connect()
{
    // denied: a server of one slot, held by client 0; client 1 is refused
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );
    lifecycle_connect( &world, 0, 0x1401 );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1402, 300, token );
    rocketnet_client_t * late = world.clients[1];
    check( rocketnet_client_connect( late, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( late ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    check( rocketnet_client_connect( late, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );

    // free the slot, then disconnect the denied client and connect it again
    lifecycle_disconnect( &world, 0 );
    rocketnet_client_disconnect( late );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_connect( &world, 1, 0x1403 );
    lifecycle_exchange( &world, 1, 0xF1, 2 );
    lifecycle_world_destroy( &world );

    // timed out: the token names an address no server holds, so nothing
    // answers for the token's timeout
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_token( world.server, 1, &world.identity, 0x1404, 1, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 100 && rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
        lifecycle_pump( &world, 0.1 );
    const int ended = rocketnet_client_state( client );
    check( ended == ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT || ended == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( client ) == ended );
    rocketnet_client_disconnect( client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_connect( &world, 0, 0x1405 );
    lifecycle_exchange( &world, 0, 0xF2, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_error_state_then_disconnect_then_connect );

// A send on a connection that is not CONNECTED is dropped and counted, on the
// client and on the server alike (standard/MESSAGES.md m1.5;
// tla/ClientSession.tla DropSend, tla/ServerSession.tla ServerDropSend):
// start message returns NULL and finish message 0, the NOT_CONNECTED_DROPS
// counter moves by exactly 1 per start, the misuse count does not move, and
// nothing of it is ever sent.
static void lifecycle_check_client_send_dropped( rocketnet_client_t * client )
{
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t drops = rn_client_stats_of( client ).not_connected_drops;
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out == NULL );
    if ( out )
    {
        out[0] = 0xEE; out[1] = 0xEE;
    }
    check( rocketnet_client_finish_message( client, 2 ) == 0 );
    check( rn_client_stats_of( client ).not_connected_drops == drops + 1 );
    check( rocketnet_api_misuse_count() == misuse );
}

static void lifecycle_check_server_send_dropped( rocketnet_server_t * server, int index )
{
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t drops = rn_server_stats_of( server ).not_connected_drops;
    uint8_t * out = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out == NULL );
    if ( out )
    {
        out[0] = 0xEE; out[1] = 0xEE;
    }
    check( rocketnet_server_finish_message( server, 2 ) == 0 );
    check( rn_server_stats_of( server ).not_connected_drops == drops + 1 );
    check( rocketnet_api_misuse_count() == misuse );
}

// once CONNECTED and pumped further, nothing arrives at the server from the
// client on channel 0, nor at the client from the server; then the first send
// after CONNECTED is delivered
static void lifecycle_check_nothing_then_first_send( lifecycle_world_t * world, int client, uint8_t tag )
{
    rocketnet_client_t * c = world->clients[client];
    check( rocketnet_client_state( c ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    for ( int step = 0; step < 50; step++ )
    {
        lifecycle_pump( world );
        static const struct rocketnet_message_batch_t * nothing = NULL;
        check( ( ( nothing = rocketnet_server_receive_messages( world->server ) ) == NULL ) );
        check( ( ( nothing = rocketnet_client_receive_messages( c ) ) == NULL ) );
    }
    lifecycle_exchange( world, client, tag, 1 );
}

static void test_client_send_dropped_before_connect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];

    // right after create: DISCONNECTED, no connection object
    check( rn_client_stats_of( client ).not_connected_drops == 0 );
    lifecycle_check_client_send_dropped( client );
    lifecycle_check_client_send_dropped( client );
    check( rn_client_stats_of( client ).not_connected_drops == 2 );

    // an invalid token: the client is in an error state with no connection
    char bad[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    memset( bad, 'A', sizeof( bad ) - 1 );
    bad[sizeof( bad ) - 1] = '\0';
    check( rocketnet_client_connect( client, bad ) == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );
    lifecycle_check_client_send_dropped( client );
    rocketnet_client_disconnect( client );

    lifecycle_connect( &world, 0, 0x1501 );
    check( rn_client_stats_of( client ).not_connected_drops == 3 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA5 );
    check( rn_client_stats_of( client ).not_connected_drops == 3 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_send_dropped_before_connect );

static void test_client_send_dropped_while_connecting()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );
    rocketnet_client_t * client = world.clients[0];

    // a handshake state: the connection object exists, its transport is not
    // CONNECTED, and an unreliable send is dropped the same as a reliable one
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1502, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( client ) < ROCKETNET_CLIENT_STATE_CONNECTED );
    lifecycle_check_client_send_dropped( client );
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 2 ) == NULL );
    check( rocketnet_client_finish_message( client, 2 ) == 0 );
    check( rocketnet_api_misuse_count() == misuse );
    check( rn_client_stats_of( client ).not_connected_drops == 2 );

    // every handshake state the pump passes through drops the same way
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( &world );
        if ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
            lifecycle_check_client_send_dropped( client );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const uint64_t drops = rn_client_stats_of( client ).not_connected_drops;
    check( drops >= 2 );
    const int index = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world.server, index ); i++ )
        lifecycle_pump( &world );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA6 );
    check( rn_client_stats_of( client ).not_connected_drops == drops );

    // an error state with its connection object: the second client is denied
    // by the full server
    rocketnet_client_t * late = world.clients[1];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1503, 300, token );
    check( rocketnet_client_connect( late, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( late ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    lifecycle_check_client_send_dropped( late );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_send_dropped_while_connecting );

static void test_client_send_dropped_after_disconnect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1504 );
    lifecycle_exchange( &world, 0, 0xA7, 2 );
    check( rn_client_stats_of( client ).not_connected_drops == 0 );

    // the client disconnects itself: DISCONNECTED, no connection object
    lifecycle_disconnect( &world, 0 );
    lifecycle_check_client_send_dropped( client );

    // reconnect: the counter is a lifetime counter, kept across connections,
    // and nothing dropped is sent on the new one
    lifecycle_connect( &world, 0, 0x1505 );
    check( rn_client_stats_of( client ).not_connected_drops == 1 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA8 );

    // the server ends the connection: SERVER_DISCONNECTED while its object exists
    rocketnet_server_disconnect_client( world.server, rocketnet_client_index( client ) );
    for ( int i = 0; i < 200 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    lifecycle_check_client_send_dropped( client );
    check( rn_client_stats_of( client ).not_connected_drops == 2 );

    lifecycle_connect( &world, 0, 0x1506 );
    check( rn_client_stats_of( client ).not_connected_drops == 2 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA9 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_send_dropped_after_disconnect );

static void test_server_send_dropped_to_slot_not_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 1 );
    rocketnet_server_t * server = world.server;
    lifecycle_connect( &world, 0, 0x1507 );
    const int index = rocketnet_client_index( world.clients[0] );
    const int free_slot = ( index + 1 ) % 3;
    check( rocketnet_server_client_connected( server, free_slot ) == 0 );
    check( rn_server_stats_of( server ).not_connected_drops == 0 );

    // a free slot below max_clients
    lifecycle_check_server_send_dropped( server, free_slot );

    // an index at or past max_clients stays a counted misuse, and is no drop
    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( server, 3, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( rocketnet_server_finish_message( server, 2 ) == 0 );
    check( rocketnet_server_start_message( server, 1000, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 2 );     // the finish with no start pending is not misuse
    check( rn_server_stats_of( server ).not_connected_drops == 1 );

    // the connected slot still sends; then its client leaves and the slot drops
    lifecycle_exchange( &world, 0, 0xB1, 1 );
    lifecycle_disconnect( &world, 0 );
    lifecycle_check_server_send_dropped( server, index );
    check( rn_server_stats_of( server ).not_connected_drops == 2 );

    // a stopped server: the same drop, the counter kept across the stop
    rocketnet_server_stop( server );
    check( rocketnet_server_running( server ) == 0 );
    check( rn_server_stats_of( server ).not_connected_drops == 2 );
    lifecycle_check_server_send_dropped( server, 0 );
    lifecycle_check_server_send_dropped( server, 2 );
    misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( server, 3, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( rocketnet_server_finish_message( server, 2 ) == 0 );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rn_server_stats_of( server ).not_connected_drops == 4 );

    // start again: nothing dropped is sent to the client that connects next
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1508 );
    check( rn_server_stats_of( server ).not_connected_drops == 4 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xB2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_send_dropped_to_slot_not_connected );

// set_send_kbps follows the same rule: a connection that is not CONNECTED has
// no rate to set. The call returns INVALID_ARGUMENT, changes nothing and is
// not misuse; once CONNECTED the same call is OK.
static void test_server_set_send_kbps_refused_unless_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_server_t * server = world.server;
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_set_send_kbps( server, 0, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, 1, 0 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );

    lifecycle_connect( &world, 0, 0x1509 );
    const int index = rocketnet_client_index( world.clients[0] );
    const int other = 1 - index;
    check( rocketnet_server_set_send_kbps( server, other, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, index, 2000 ) == ROCKETNET_STATUS_OK );

    // the client leaves: the slot refuses again
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_server_set_send_kbps( server, index, 3000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // stopped: refused, not misuse
    rocketnet_server_stop( server );
    check( rocketnet_server_set_send_kbps( server, index, 3000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x150A );
    check( rocketnet_server_set_send_kbps( server, rocketnet_client_index( world.clients[0] ), 3000 ) == ROCKETNET_STATUS_OK );
    lifecycle_exchange( &world, 0, 0xB3, 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_set_send_kbps_refused_unless_connected );

static void test_client_set_send_kbps_refused_unless_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // every handshake state refuses
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x150B, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    int handshake_checks = 0;
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_client_set_send_kbps( client, 0 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        handshake_checks++;
        lifecycle_pump( &world );
    }
    check( handshake_checks > 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_api_misuse_count() == misuse );

    // CONNECTED: OK
    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_OK );
    const int index = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world.server, index ); i++ )
        lifecycle_pump( &world );
    lifecycle_exchange( &world, 0, 0xB4, 1 );

    // after disconnect: refused again
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_set_send_kbps_refused_unless_connected );

static void test_server_start_stop_start_again()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 2 );
    lifecycle_connect( &world, 0, 0x1601 );
    lifecycle_connect( &world, 1, 0x1602 );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );
    const int a = rocketnet_client_index( world.clients[0] );
    const int b = rocketnet_client_index( world.clients[1] );
    check( a != b && a < 2 && b < 2 );
    check( rocketnet_server_client_id( world.server, a ) == 0x1601 );
    check( rocketnet_server_client_id( world.server, b ) == 0x1602 );

    // stop: both clients see the disconnect, both slots are free
    rocketnet_server_stop( world.server );
    check( rocketnet_server_running( world.server ) == 0 );
    check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    check( rocketnet_server_client_connected( world.server, a ) == 0 );
    check( rocketnet_server_client_connected( world.server, b ) == 0 );
    check( rocketnet_server_port( world.server ) == 0 );
    check( rocketnet_server_transport_name( world.server ) == NULL );
    check( rocketnet_server_max_clients( world.server ) == 2 );
    rocketnet_server_stop( world.server );      // a stopped server's stop is a no-op
    for ( int i = 0; i < 100; i++ )
    {
        lifecycle_pump( &world );
        if ( rocketnet_client_state( world.clients[0] ) != ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_client_state( world.clients[1] ) != ROCKETNET_CLIENT_STATE_CONNECTED )
            break;
    }
    check( rocketnet_client_state( world.clients[0] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( world.clients[1] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );

    // a stopped server's update and send are counted no-ops
    const uint64_t misuse = rocketnet_api_misuse_count();
    rocketnet_server_update( world.server, world.time );
    rocketnet_server_send_packets( world.server );
    check( rocketnet_api_misuse_count() == misuse + 2 );

    // start again, connect again
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_running( world.server ) == 1 );
    rocketnet_client_disconnect( world.clients[0] );
    rocketnet_client_disconnect( world.clients[1] );
    lifecycle_connect( &world, 0, 0x1603 );
    lifecycle_connect( &world, 1, 0x1604 );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );
    lifecycle_exchange( &world, 0, 0x91, 2 );
    lifecycle_exchange( &world, 1, 0x92, 2 );
    check( rn_server_stats_of( world.server ).clients_connected == 4 );
    lifecycle_world_destroy( &world );      // destroys a running server
}
REGISTER_UNIT_TEST( test_server_start_stop_start_again );

static void test_server_stop_disconnects_every_client()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 3 );
    for ( int i = 0; i < 3; i++ )
        lifecycle_connect( &world, i, 0x1700 + (uint64_t) i );
    check( rocketnet_server_num_connected_clients( world.server ) == 3 );
    rocketnet_server_stop( world.server );
    check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    // the redundant disconnect packets reach every client before any timeout
    for ( int step = 0; step < 20; step++ )
        lifecycle_pump( &world );
    for ( int i = 0; i < 3; i++ )
        check( rocketnet_client_state( world.clients[i] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_stop_disconnects_every_client );

static void test_server_disconnect_all_clients()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 3 );
    for ( int i = 0; i < 3; i++ )
        lifecycle_connect( &world, i, 0x1800 + (uint64_t) i );
    rocketnet_server_disconnect_all_clients( world.server );
    check( rocketnet_server_running( world.server ) == 1 );
    check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    for ( int step = 0; step < 20; step++ )
        lifecycle_pump( &world );
    for ( int i = 0; i < 3; i++ )
        check( rocketnet_client_state( world.clients[i] ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    // still running: the clients come back
    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_client_disconnect( world.clients[i] );
        lifecycle_connect( &world, i, 0x1810 + (uint64_t) i );
    }
    check( rocketnet_server_num_connected_clients( world.server ) == 3 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_disconnect_all_clients );

static void test_connection_reset_client_reconnect_delivers()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1901 );
    lifecycle_exchange( &world, 0, 0x51, 2 );

    // mid-transfer: queue messages both ways, let one update run, then end it
    const int index = rocketnet_client_index( client );
    for ( int i = 0; i < 8; i++ )
    {
        lifecycle_client_send( client, 0x5F, (uint8_t) i );
        lifecycle_server_send( world.server, index, 0x5F, (uint8_t) i );
    }
    lifecycle_pump( &world );
    uint64_t sender = 0;
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( world.server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            sender = batch->client_index[i];
            check( sender == (uint64_t) index );
        }
    }
    lifecycle_disconnect( &world, 0 );

    // the new connection's first messages deliver, and nothing of the old
    lifecycle_connect( &world, 0, 0x1902 );
    lifecycle_exchange( &world, 0, 0x52, 4 );
    for ( int step = 0; step < 50; step++ )
    {
        lifecycle_pump( &world );
        static const struct rocketnet_message_batch_t * nothing = NULL;
        check( ( ( nothing = rocketnet_server_receive_messages( world.server ) ) == NULL ) );
        check( ( ( nothing = rocketnet_client_receive_messages( client ) ) == NULL ) );
    }
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_connection_reset_client_reconnect_delivers );

// ROCKETNET_FLOW_ID_BIT: a destination id with the bit set is refused and
// counted by each destination-taking function, and client indexes
// 0 .. max_clients - 1 round-trip through every index-taking function as
// uint64_t.
static int lifecycle_flow_logs = 0;
static void lifecycle_log_callback( void *, enum rocketnet_log_level_t, const char * message, size_t )
{
    if ( strstr( message, "FLOW_ID_RESERVED" ) )
        lifecycle_flow_logs++;
}

static void test_server_destination_flow_id_refused()
{
    check( ROCKETNET_FLOW_ID_BIT == ( 1ULL << 63 ) );
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    lifecycle_connect( &world, 0, 0x1A01 );
    const uint64_t flow = ROCKETNET_FLOW_ID_BIT | rocketnet_client_index( world.clients[0] );

    // the flow refusal names FLOW_ID_RESERVED; an index merely out of range
    // (no flow bit) is refused without it
    rocketnet_quiet( 0 );
    rocketnet_log_function( lifecycle_log_callback, NULL );
    lifecycle_flow_logs = 0;
    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( world.server, flow, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 && lifecycle_flow_logs == 1 );
    // finish and abort act on the pending start: the refused one left none,
    // so each returns its empty value, uncounted, with no flow log
    misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_finish_message( world.server, 4 ) == 0 );
    rocketnet_server_abort_message( world.server );
    check( rocketnet_api_misuse_count() == misuse && lifecycle_flow_logs == 1 );
    // an index merely out of range is counted misuse without the flow log
    check( rocketnet_server_start_message( world.server, 5, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 && lifecycle_flow_logs == 1 );
    rocketnet_log_function( NULL, NULL );
    rocketnet_quiet( 1 );
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_FLOW_ID_RESERVED ), "FLOW_ID_RESERVED" ) == 0 );

    // the slot's own index still works
    lifecycle_exchange( &world, 0, 0x61, 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_destination_flow_id_refused );

// The client index is an int on every per-client call; only
// rocketnet_server_start_message takes a uint64_t destination id, which is
// the client index today.
static void test_server_client_index_round_trips_as_int()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 3 );
    for ( int i = 0; i < 3; i++ )
        lifecycle_connect( &world, i, 0x1B00 + (uint64_t) i );
    for ( int i = 0; i < 3; i++ )
    {
        const int index = rocketnet_client_index( world.clients[i] );
        check( index >= 0 && index < 3 );
        check( rocketnet_server_client_connected( world.server, index ) == 1 );
        check( rocketnet_server_client_id( world.server, index ) == 0x1B00 + (uint64_t) i );
        check( rocketnet_server_disconnect_reason( world.server, index ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( rocketnet_server_set_send_kbps( world.server, index, 2000 ) == ROCKETNET_STATUS_OK );
        uint8_t * out = rocketnet_server_start_message( world.server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
        check( out != NULL );
        rocketnet_server_abort_message( world.server );
        struct rocketnet_receipt_t receipts[4];
        check( rocketnet_server_receipts( world.server, index, receipts, 4 ) >= 0 );
        (void) rn_server_client_stats_of( world.server, index ).packets_sent;
        (void) rn_server_client_stats_of( world.server, index ).rtt;
        lifecycle_exchange( &world, i, (uint8_t) ( 0x70 + i ), 1 );
        // an index past max_clients, and a negative one, name no client
        check( rocketnet_server_client_connected( world.server, index + 3 ) == 0 );
        check( rocketnet_server_client_connected( world.server, -1 ) == 0 );
        check( rocketnet_server_client_id( world.server, -1 ) == 0 );
        check( rocketnet_server_client_id( world.server, 0x7fffffff ) == 0 );
    }
    rocketnet_server_disconnect_client( world.server, rocketnet_client_index( world.clients[0] ) );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_client_index_round_trips_as_int );

// packets_per_update takes 1 .. 1024 and defaults to 256, client and
// server, through the struct and through the environment.
#if defined(_WIN32)
static int lifecycle_setenv( const char * name, const char * value ) { return _putenv_s( name, value ); }
static int lifecycle_unsetenv( const char * name ) { return _putenv_s( name, "" ); }
#else
static int lifecycle_setenv( const char * name, const char * value ) { return setenv( name, value, 1 ); }
static int lifecycle_unsetenv( const char * name ) { return unsetenv( name ); }
#endif

static void test_config_packets_per_update_range()
{
    rocketnet_quiet( 1 );
    lifecycle_identity_t identity;
    lifecycle_identity( &identity );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    check( client_config.packets_per_update == 256 );
    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &identity, 1 );
    check( server_config.packets_per_update == 256 );

    // the struct
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_COUNT;
    client_config.packets_per_update = 1024;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
    client_config.packets_per_update = 1025;
    create_status = ROCKETNET_STATUS_COUNT;
    client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_STATUS_COUNT );
    client_config.packets_per_update = 0;
    create_status = ROCKETNET_STATUS_COUNT;
    client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_STATUS_COUNT );

    server_config.packets_per_update = 1024;
    create_status = ROCKETNET_STATUS_COUNT;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );
    server_config.packets_per_update = 1025;
    create_status = ROCKETNET_STATUS_COUNT;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, &create_status );
    check( server == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_STATUS_COUNT );
    server_config.packets_per_update = 0;
    create_status = ROCKETNET_STATUS_COUNT;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, &create_status );
    check( server == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_STATUS_COUNT );

    // the environment
    rocketnet_client_default_config( &client_config );
    lifecycle_server_config( &server_config, &identity, 1 );
    // Old public spellings are not aliases. Values outside the valid range
    // make accidental reads visible: both creates must ignore these names.
    check( lifecycle_setenv( "ROCKETNET_CLIENT_MAX_PACKETS_" "PER_UPDATE", "1025" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RTO_" "INITIAL_MS", "60001" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RTO_" "MIN_MS", "-1" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RTO_" "MAX_MS", "60001" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_MAX_PACKETS_" "PER_UPDATE", "1025" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RTO_" "INITIAL_MS", "60001" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RTO_" "MIN_MS", "-1" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RTO_" "MAX_MS", "60001" ) == 0 );
    client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    if ( client ) rocketnet_client_destroy( client );
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL );
    check( server != NULL );
    if ( server ) rocketnet_server_destroy( server );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MAX_PACKETS_" "PER_UPDATE" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RTO_" "INITIAL_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RTO_" "MIN_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RTO_" "MAX_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MAX_PACKETS_" "PER_UPDATE" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RTO_" "INITIAL_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RTO_" "MIN_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RTO_" "MAX_MS" ) == 0 );

    // The new resend-delay names are read for both endpoint configs.
    check( lifecycle_setenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS", "9" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS", "2001" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS", "9" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS", "9" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS", "2001" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS", "9" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE", "1024" ) == 0 );
    client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    rocketnet_client_destroy( client );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE", "1025" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE", "1024" ) == 0 );
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL );
    check( server != NULL );
    rocketnet_server_destroy( server );
    check( lifecycle_setenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE", "1025" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE" ) == 0 );
}
REGISTER_UNIT_TEST( test_config_packets_per_update_range );

// A connection the server ends leaves the client DISCONNECTED with its
// connection object still held; connect straight after (no disconnect call)
// ends it first, and the lifetime counters only grow.
static void test_client_reconnects_after_server_stop_without_disconnect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1C01 );
    lifecycle_exchange( &world, 0, 0x81, 3 );
    rocketnet_server_stop( world.server );
    for ( int i = 0; i < 50 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    struct rocketnet_client_stats_t before;
    rocketnet_client_stats( client, &before );
    check( before.packets_sent > 0 );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1C02 );      // connect, no disconnect first
    struct rocketnet_client_stats_t after;
    rocketnet_client_stats( client, &after );
    check( after.packets_sent >= before.packets_sent );
    check( after.packets_received >= before.packets_received );
    check( after.bytes_sent >= before.bytes_sent );
    check( after.bytes_received >= before.bytes_received );
    check( after.payloads_received >= before.payloads_received );
    check( after.messages_resent >= before.messages_resent );
    check( after.messages_duplicate >= before.messages_duplicate );
    check( after.receipts_acked >= before.receipts_acked );
    check( after.receipts_lost >= before.receipts_lost );
    check( after.budget_drops >= before.budget_drops );
    check( after.send_deferred >= before.send_deferred );
    check( after.not_connected_drops >= before.not_connected_drops );
    lifecycle_exchange( &world, 0, 0x82, 3 );
    check( rn_client_stats_of( client ).packets_sent > before.packets_sent );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_reconnects_after_server_stop_without_disconnect );

// A connection that ends with a reason reads it as the client state until
// disconnect or the next connect ends it: a config mismatch (num_channels is
// a WIRE field) ends the first, disconnect reads DISCONNECTED, and the next
// connect runs a fresh handshake to CONNECTED.
static void test_client_connection_ended_state_kept_until_disconnect()
{
    lifecycle_world_t world;
    memset( &world, 0, sizeof( world ) );
    rocketnet_quiet( 1 );
    world.time = 1000.0;
    lifecycle_now = world.time;
    lifecycle_identity( &world.identity );
    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &world.identity, 1 );     // 32 channels
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL );
    check( world.server && rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.num_channels = 4;
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    world.num_clients = 1;
    rocketnet_client_t * client = world.clients[0];
    check( client != NULL );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1D01, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 2000 && rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
        lifecycle_pump( &world );
    const enum rocketnet_client_state_t ended = rocketnet_client_state( client );
    check( ended == ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH || ended == ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT );
    // kept through further updates
    for ( int i = 0; i < 10; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ended );
    rocketnet_client_disconnect( client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );

    // a matching server: the next connect runs a fresh handshake
    rocketnet_server_destroy( world.server );
    server_config.num_channels = 4;
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL );
    check( world.server && rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1D02, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_connection_ended_state_kept_until_disconnect );

// connect with an expired token: the status and the matching error state,
// refused again until disconnect.
static void test_client_connect_token_expired()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world.server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( world.identity.backend_secret, world.server,
        LIFECYCLE_PROTOCOL, addresses, 1, 0x1E01, 0, 1, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    rocketnet_client_disconnect( client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_connect( &world, 0, 0x1E02 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_connect_token_expired );

// One update sends more packets than one builder call tracks
// (ROCKETNET_PACKET_BUILDER_MAX_PACKETS, 16): the per-update limit is the
// config's packets_per_update.
static void test_client_one_update_sends_more_than_sixteen_packets()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 0 );
    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;   // no budget: the packet limit alone bounds the update
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    world.num_clients = 1;
    check( world.clients[0] != NULL );
    lifecycle_connect( &world, 0, 0x1F01 );
    rocketnet_client_t * client = world.clients[0];
    for ( int i = 0; i < 20; i++ )
        lifecycle_pump( &world );
    for ( int i = 0; i < 40; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( client, i % 4, ROCKETNET_RELIABILITY_RELIABLE, 1200 );
        check( out != NULL );
        if ( out )
        {
            memset( out, i, 1200 );
            check( rocketnet_client_finish_message( client, 1200 ) != 0 );
        }
    }
    const uint64_t before = rn_client_stats_of( client ).packets_sent;
    lifecycle_now = world.time;
    rocketnet_client_update( client, world.time );
    rocketnet_client_send_packets( client );
    const uint64_t sent = rn_client_stats_of( client ).packets_sent - before;
    check( sent > ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_one_update_sends_more_than_sixteen_packets );

// ---------------------------------------------------------------------------
// The one receive queue, pumped by each receive: every reliability type sent on
// every channel by every client is received through the server's one queue,
// each naming its sender, and through each client's one queue. The send's
// channel orders ORDERED and sequences SEQUENCED messages within it and is
// ignored by the other types (sdk/rocketnet_message_server.cpp,
// rn_message_object_start, and rn_message_object_next).
// ---------------------------------------------------------------------------

#define RQ_CLIENTS 2
#define RQ_ROUNDS 3
#define RQ_TYPES 8
#define RQ_LOG ( RQ_CLIENTS * RQ_ROUNDS * RQ_TYPES * ROCKETNET_MAX_CHANNELS )

struct rq_entry_t
{
    uint64_t sender;            // the server's client index, or the receiving client
    uint8_t payload[4];         // origin, type, channel, round
};

struct rq_log_t
{
    int count;
    rq_entry_t entry[RQ_LOG];
};

struct rq_run_t
{
    rq_log_t at_server;
    rq_log_t at_client;
    int index[RQ_CLIENTS];
    int num_channels;
};

static void rq_record( rq_log_t * log, uint64_t sender, const uint8_t * data, int bytes )
{
    check( bytes == 4 );
    check( log->count < RQ_LOG );
    if ( bytes != 4 || log->count >= RQ_LOG )
        return;
    log->entry[log->count].sender = sender;
    memcpy( log->entry[log->count].payload, data, 4 );
    log->count++;
}

// one update's receive: the server's queue and each client's, a batch per call
static void rq_drain( lifecycle_world_t * world, rq_run_t * run )
{
    const struct rocketnet_message_batch_t * received = NULL;
    while ( ( received = rocketnet_server_receive_messages( world->server ) ) != NULL )
    {
        check( received->num_messages <= ROCKETNET_RECEIVE_BATCH_MESSAGES );
        for ( int i = 0; i < received->num_messages; i++ )
        {
            // the channel a message was delivered on: its ordering channel for
            // an ordered or sequenced type, 0 for the types that carry none
            const uint8_t type = received->data[i][1];
            const bool carries = type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
                                 type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
            check( received->bytes[i] == 4 && received->channel[i] == ( carries ? received->data[i][2] : 0 ) );
            rq_record( &run->at_server, received->client_index[i], received->data[i], received->bytes[i] );
        }
    }

    for ( int k = 0; k < world->num_clients; k++ )
    {
        while ( ( received = rocketnet_client_receive_messages( world->clients[k] ) ) != NULL )
        {
            for ( int i = 0; i < received->num_messages; i++ )
            {
                check( received->client_index[i] == 0 );
                rq_record( &run->at_client, (uint64_t) k, received->data[i], received->bytes[i] );
            }
        }
    }
}

// every type on every channel, both ways: a client's payload names the client
// (origin k), the server's names 0x80 + the client it is sent to
static void rq_send_round( lifecycle_world_t * world, rq_run_t * run, int round )
{
    for ( int k = 0; k < world->num_clients; k++ )
    {
        for ( int channel = 0; channel < run->num_channels; channel++ )
        {
            for ( int type = 0; type < RQ_TYPES; type++ )
            {
                uint8_t * out = rocketnet_client_start_message( world->clients[k], channel, (enum rocketnet_reliability_t) type, 4 );
                check( out != NULL );
                if ( out )
                {
                    out[0] = (uint8_t) k; out[1] = (uint8_t) type; out[2] = (uint8_t) channel; out[3] = (uint8_t) round;
                    check( rocketnet_client_finish_message( world->clients[k], 4 ) != 0 );
                }
                out = rocketnet_server_start_message( world->server, (uint64_t) run->index[k], channel, (enum rocketnet_reliability_t) type, 4 );
                check( out != NULL );
                if ( out )
                {
                    out[0] = (uint8_t) ( 0x80 + k ); out[1] = (uint8_t) type; out[2] = (uint8_t) channel; out[3] = (uint8_t) round;
                    check( rocketnet_server_finish_message( world->server, 4 ) != 0 );
                }
            }
        }
    }
}

static void rq_run( rq_run_t * run )
{
    memset( run, 0, sizeof( *run ) );
    lifecycle_world_t world;
    lifecycle_world_create( &world, RQ_CLIENTS, RQ_CLIENTS );
    rocketnet_server_config_t defaults;
    rocketnet_server_default_config( &defaults );
    run->num_channels = defaults.num_channels;
    check( run->num_channels >= 6 && run->num_channels <= ROCKETNET_MAX_CHANNELS );
    for ( int k = 0; k < RQ_CLIENTS; k++ )
    {
        lifecycle_connect( &world, k, 0x1A00 + (uint64_t) k );
        run->index[k] = rocketnet_client_index( world.clients[k] );
    }
    // the rounds are a few kilobytes an update each way: a budget that sends
    // them whole, so no unreliable message is dropped for budget
    for ( int k = 0; k < RQ_CLIENTS; k++ )
    {
        check( rocketnet_client_set_send_kbps( world.clients[k], 100000 ) == ROCKETNET_STATUS_OK );
        check( rocketnet_server_set_send_kbps( world.server, run->index[k], 100000 ) == ROCKETNET_STATUS_OK );
    }
    for ( int round = 0; round < RQ_ROUNDS; round++ )
    {
        rq_send_round( &world, run, round );
        lifecycle_pump( &world );
        rq_drain( &world, run );
    }
    for ( int step = 0; step < 100; step++ )
    {
        lifecycle_pump( &world );
        rq_drain( &world, run );
    }
    lifecycle_world_destroy( &world );
}

// The delivery guarantees over one log: nothing twice; every reliable type
// (RELIABLE, RELIABLE_ORDERED and the two reliable receipt types) exactly
// once whatever channel it was sent on; ordered messages in send order within
// their sender and channel; sequenced messages never older than one seen
// before on their sender and channel. from_client names the direction.
static void rq_check_log( const rq_run_t * run, const rq_log_t * log, bool at_server )
{
    static uint8_t seen[RQ_CLIENTS][RQ_TYPES][ROCKETNET_MAX_CHANNELS][RQ_ROUNDS];
    int ordered_last[RQ_CLIENTS][ROCKETNET_MAX_CHANNELS];
    int sequenced_last[RQ_CLIENTS][ROCKETNET_MAX_CHANNELS];
    memset( seen, 0, sizeof( seen ) );
    for ( int k = 0; k < RQ_CLIENTS; k++ )
        for ( int c = 0; c < ROCKETNET_MAX_CHANNELS; c++ )
            ordered_last[k][c] = sequenced_last[k][c] = -1;

    for ( int i = 0; i < log->count; i++ )
    {
        const rq_entry_t * e = &log->entry[i];
        const int origin = at_server ? e->payload[0] : e->payload[0] - 0x80;
        const int type = e->payload[1], channel = e->payload[2], round = e->payload[3];
        check( origin >= 0 && origin < RQ_CLIENTS && type < RQ_TYPES && channel < run->num_channels && round < RQ_ROUNDS );
        if ( !( origin >= 0 && origin < RQ_CLIENTS && type < RQ_TYPES && channel < run->num_channels && round < RQ_ROUNDS ) )
            continue;
        // the message names its sender: the server's queue the client index,
        // a client's queue is its own
        check( e->sender == ( at_server ? (uint64_t) run->index[origin] : (uint64_t) origin ) );
        check( seen[origin][type][channel][round] == 0 );
        seen[origin][type][channel][round]++;
        if ( type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT )
        {
            const int order = round * 2 + ( type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT );
            check( order == ordered_last[origin][channel] + 1 );                         // in send order, none skipped
            ordered_last[origin][channel] = order;
        }
        if ( type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED )
        {
            const int order = round * 2 + ( type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED );
            check( order > sequenced_last[origin][channel] );                            // never an older state
            sequenced_last[origin][channel] = order;
        }
    }
    for ( int k = 0; k < RQ_CLIENTS; k++ )
        for ( int c = 0; c < run->num_channels; c++ )
            for ( int r = 0; r < RQ_ROUNDS; r++ )
            {
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE][c][r] == 1 );
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE_ORDERED][c][r] == 1 );
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT][c][r] == 1 );
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT][c][r] == 1 );
            }
}

static void test_message_receive_one_queue_every_type_every_channel()
{
    static rq_run_t run;
    rq_run( &run );
    rq_check_log( &run, &run.at_server, true );
    rq_check_log( &run, &run.at_client, false );
}
REGISTER_UNIT_TEST( test_message_receive_one_queue_every_type_every_channel );

// The receive is pumped: each call fills the caller's batch up to
// ROCKETNET_RECEIVE_BATCH_MESSAGES and returns true, and what one call does
// not return waits for the next call of the same update, in order, with no
// per-update cap. More messages than one batch holds arrive in one update;
// pumped to empty they are each seen once, in send order, and the call that
// finds nothing pending returns false with num_messages 0.
static void test_message_receive_pumped_in_order()
{
    // each client sends more than one batch; together the clients' deliveries
    // in one update are more than one batch of the server's one queue, so
    // the window, the send queue and the send buffer hold every message
    enum { NUM_SENDERS = 3 };
    const int per_client = ROCKETNET_RECEIVE_BATCH_MESSAGES + 17;
    lifecycle_queue_messages = 1;
    while ( lifecycle_queue_messages < per_client )
        lifecycle_queue_messages *= 2;
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1B01 );
    const int index = (int) rocketnet_client_index( client );
    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );

    const int total = 900;
    for ( int i = 0; i < total; i++ )
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        check( p != NULL );
        if ( !p )
            break;
        p[0] = (uint8_t) i; p[1] = (uint8_t) ( i >> 8 ); p[2] = 0; p[3] = 0;
        check( rocketnet_client_finish_message( client, 4 ) != 0 );
    }

    const struct rocketnet_message_batch_t * batch = NULL;
    int received = 0;
    for ( int step = 0; step < 200 && received < total; step++ )
    {
        lifecycle_pump( &world );
        while ( ( batch = rocketnet_server_receive_messages( world.server ) ) != NULL )
        {
            check( batch->num_messages > 0 && batch->num_messages <= ROCKETNET_RECEIVE_BATCH_MESSAGES );
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                check( batch->bytes[i] == 4 && batch->client_index[i] == index && batch->channel[i] == 0 );
                const int value = batch->data[i][0] | ( batch->data[i][1] << 8 );
                check( value == received );                // once each, in send order
                received++;
            }
        }
    }
    check( received == total );

    // an update with nothing delivered: the first receive returns NULL
    lifecycle_pump( &world );
    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( rocketnet_client_receive_messages( client ) == NULL );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_message_receive_pumped_in_order );

// The library owns one receive batch per object: each receive refills and
// returns it, a receive that fills nothing returns NULL, the second of two
// receives in a row wipes the first, and a NULL object is one counted misuse
// that returns NULL.
static void test_receive_returns_the_objects_one_batch()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1B02 );

    // nothing pending: NULL, never an empty batch
    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( rocketnet_client_receive_messages( client ) == NULL );

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
        check( p != NULL );
        if ( p )
        {
            p[0] = (uint8_t) ( 0x40 + i );
            check( rocketnet_client_finish_message( client, 1 ) != 0 );
        }
    }
    const struct rocketnet_message_batch_t * first = NULL;
    for ( int step = 0; step < 50 && first == NULL; step++ )
    {
        lifecycle_pump( &world );
        first = rocketnet_server_receive_messages( world.server );
    }
    check( first != NULL && first->num_messages == 2 && first->data[0][0] == 0x40 && first->data[1][0] == 0x41 );
    // the next receive refills the same batch: nothing left, so NULL, and the
    // batch's count is wiped
    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( first->num_messages == 0 );

    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_receive_messages( NULL ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_client_receive_messages( NULL ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_receive_returns_the_objects_one_batch );
