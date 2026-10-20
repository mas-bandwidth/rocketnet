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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define LIFECYCLE_PROTOCOL 0x7A11C1C1E5EED001ULL
#define LIFECYCLE_MAX_CLIENTS 4

struct lifecycle_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
};

static double lifecycle_now = 1000.0;
static double lifecycle_clock( void * ) { return lifecycle_now; }

static void lifecycle_identity( lifecycle_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static void lifecycle_server_config( rocketnet_server_config_t * config, const lifecycle_identity_t * identity, int max_clients )
{
    rocketnet_server_default_config( config );
    config->protocol_id = LIFECYCLE_PROTOCOL;
    config->max_clients = max_clients;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    memcpy( config->server_public_key, identity->server_public, sizeof( config->server_public_key ) );
    memcpy( config->server_secret_key, identity->server_secret, sizeof( config->server_secret_key ) );
    config->transport = "loopback";
}

static void lifecycle_client_config( rocketnet_client_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = lifecycle_clock;
}

static void lifecycle_token( uint16_t port, const lifecycle_identity_t * identity, uint64_t client_id, int32_t timeout_seconds, uint8_t * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
        LIFECYCLE_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, timeout_seconds, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
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
    rocketnet_set_quiet( 1 );
    world->time = 1000.0;
    lifecycle_now = world->time;
    lifecycle_identity( &world->identity );
    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &world->identity, max_clients );
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( world->server != NULL );
    check( rocketnet_server_running( world->server ) == 0 );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_running( world->server ) == 1 );
    world->num_clients = num_clients;
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_config_t client_config;
        lifecycle_client_config( &client_config );
        world->clients[i] = rocketnet_client_create( NULL, &client_config, NULL, 0 );
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
        check( rocketnet_client_update( world->clients[i], world->time ) == ROCKETNET_UPDATE_OK );
        rocketnet_client_send_packets( world->clients[i] );
    }
    if ( rocketnet_server_running( world->server ) )
    {
        check( rocketnet_server_update( world->server, world->time ) == ROCKETNET_UPDATE_OK );
        rocketnet_server_send_packets( world->server );
    }
    world->time += dt;
}

static void lifecycle_connect( lifecycle_world_t * world, int client, uint64_t client_id )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    lifecycle_token( rocketnet_server_port( world->server ), &world->identity, client_id, 300, token );
    check( rocketnet_client_connect( world->clients[client], token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( world->clients[client] ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( world );
    check( rocketnet_client_state( world->clients[client] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const uint64_t index = rocketnet_client_index( world->clients[client] );
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
    check( rocketnet_server_finish_message( server, index, 2 ) != 0 );
}

// Pumps until the server holds `want` messages from slot index and the client
// `want` from the server, all tagged `tag` and in order; anything with another
// tag fails the test.
static void lifecycle_exchange( lifecycle_world_t * world, int client, uint8_t tag, int want )
{
    rocketnet_client_t * c = world->clients[client];
    const uint64_t index = rocketnet_client_index( c );
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
        while ( rocketnet_server_receive_message( world->server, index, 0, &data, &bytes ) )
        {
            check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_server );
            rocketnet_server_release_message( world->server, index, data );
            at_server++;
        }
        while ( rocketnet_client_receive_message( c, 0, &data, &bytes ) )
        {
            check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_client );
            rocketnet_client_release_message( c, data );
            at_client++;
        }
    }
    check( at_server == want && at_client == want );
}

static void lifecycle_disconnect( lifecycle_world_t * world, int client )
{
    const uint64_t index = rocketnet_client_index( world->clients[client] );
    rocketnet_client_disconnect( world->clients[client] );
    check( rocketnet_client_state( world->clients[client] ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    for ( int i = 0; i < 200 && rocketnet_server_client_connected( world->server, index ); i++ )
        lifecycle_pump( world );
    check( rocketnet_server_client_connected( world->server, index ) == 0 );
}

static void client_reconnects_with_a_second_token()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];

    lifecycle_connect( &world, 0, 0x1001 );
    lifecycle_exchange( &world, 0, 0xA1, 3 );
    const uint64_t sent_first = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_PACKETS_SENT );
    check( sent_first > 0 );
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_client_index( client ) == 0 );
    check( rocketnet_client_transport_name( client ) == NULL );

    // a second token with another client id: sequences and channels start
    // fresh, so the second exchange's values count from 0 again in order, and
    // nothing tagged by the first connection arrives
    lifecycle_connect( &world, 0, 0x2002 );
    lifecycle_exchange( &world, 0, 0xB2, 3 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_PACKETS_SENT ) > sent_first );
    check( rocketnet_server_counter( world.server, ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED ) == 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_reconnects_with_a_second_token );

static void client_destroy_then_create_and_connect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    lifecycle_connect( &world, 0, 0x1101 );
    lifecycle_exchange( &world, 0, 0xC1, 2 );
    const uint64_t index = rocketnet_client_index( world.clients[0] );
    rocketnet_client_destroy( world.clients[0] );
    world.clients[0] = NULL;
    for ( int i = 0; i < 200 && rocketnet_server_client_connected( world.server, index ); i++ )
        lifecycle_pump( &world );
    check( rocketnet_server_client_connected( world.server, index ) == 0 );

    rocketnet_client_config_t client_config;
    lifecycle_client_config( &client_config );
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( world.clients[0] != NULL );
    lifecycle_connect( &world, 0, 0x1102 );
    lifecycle_exchange( &world, 0, 0xC2, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_destroy_then_create_and_connect );

static void client_connect_refused_unless_disconnected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    lifecycle_token( rocketnet_server_port( world.server ), &world.identity, 0x1201, 300, token );

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
REGISTER_UNIT_TEST( client_connect_refused_unless_disconnected );

static void server_start_refused_unless_stopped()
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
REGISTER_UNIT_TEST( server_start_refused_unless_stopped );

static void client_error_state_then_disconnect_then_connect()
{
    // denied: a server of one slot, held by client 0; client 1 is refused
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );
    lifecycle_connect( &world, 0, 0x1401 );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    lifecycle_token( rocketnet_server_port( world.server ), &world.identity, 0x1402, 300, token );
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
    lifecycle_token( 1, &world.identity, 0x1404, 1, token );
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
REGISTER_UNIT_TEST( client_error_state_then_disconnect_then_connect );

// A send on a connection that is not CONNECTED is dropped and counted, on the
// client and on the server alike (standard/MESSAGES.md m1.5;
// tla/ClientSession.tla DropSend, tla/ServerSession.tla ServerDropSend):
// start message returns NULL and finish message 0, the NOT_CONNECTED_DROPS
// counter moves by exactly 1 per start, the misuse count does not move, and
// nothing of it is ever sent.
static void lifecycle_check_client_send_dropped( rocketnet_client_t * client )
{
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t drops = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS );
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out == NULL );
    if ( out )
    {
        out[0] = 0xEE; out[1] = 0xEE;
    }
    check( rocketnet_client_finish_message( client, 2 ) == 0 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == drops + 1 );
    check( rocketnet_api_misuse_count() == misuse );
}

static void lifecycle_check_server_send_dropped( rocketnet_server_t * server, uint64_t index )
{
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t drops = rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS );
    uint8_t * out = rocketnet_server_start_message( server, index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out == NULL );
    if ( out )
    {
        out[0] = 0xEE; out[1] = 0xEE;
    }
    check( rocketnet_server_finish_message( server, index, 2 ) == 0 );
    check( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS ) == drops + 1 );
    check( rocketnet_api_misuse_count() == misuse );
}

// once CONNECTED and pumped further, nothing arrives at the server from the
// client on channel 0, nor at the client from the server; then the first send
// after CONNECTED is delivered
static void lifecycle_check_nothing_then_first_send( lifecycle_world_t * world, int client, uint8_t tag )
{
    rocketnet_client_t * c = world->clients[client];
    check( rocketnet_client_state( c ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const uint64_t index = rocketnet_client_index( c );
    for ( int step = 0; step < 50; step++ )
    {
        lifecycle_pump( world );
        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_server_receive_message( world->server, index, 0, &data, &bytes ) == 0 );
        check( rocketnet_client_receive_message( c, 0, &data, &bytes ) == 0 );
    }
    lifecycle_exchange( world, client, tag, 1 );
}

static void client_send_dropped_before_connect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];

    // right after create: DISCONNECTED, no connection object
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 0 );
    lifecycle_check_client_send_dropped( client );
    lifecycle_check_client_send_dropped( client );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 2 );

    // an invalid token: the client is in an error state with no connection
    uint8_t bad[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( bad, 0, sizeof( bad ) );
    check( rocketnet_client_connect( client, bad ) == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );
    lifecycle_check_client_send_dropped( client );
    rocketnet_client_disconnect( client );

    lifecycle_connect( &world, 0, 0x1501 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 3 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA5 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 3 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_send_dropped_before_connect );

static void client_send_dropped_while_connecting()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );
    rocketnet_client_t * client = world.clients[0];

    // a handshake state: the connection object exists, its transport is not
    // CONNECTED, and an unreliable send is dropped the same as a reliable one
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    lifecycle_token( rocketnet_server_port( world.server ), &world.identity, 0x1502, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( client ) < ROCKETNET_CLIENT_STATE_CONNECTED );
    lifecycle_check_client_send_dropped( client );
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 2 ) == NULL );
    check( rocketnet_client_finish_message( client, 2 ) == 0 );
    check( rocketnet_api_misuse_count() == misuse );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 2 );

    // every handshake state the pump passes through drops the same way
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( &world );
        if ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
            lifecycle_check_client_send_dropped( client );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const uint64_t drops = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS );
    check( drops >= 2 );
    const uint64_t index = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world.server, index ); i++ )
        lifecycle_pump( &world );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA6 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == drops );

    // an error state with its connection object: the second client is denied
    // by the full server
    rocketnet_client_t * late = world.clients[1];
    lifecycle_token( rocketnet_server_port( world.server ), &world.identity, 0x1503, 300, token );
    check( rocketnet_client_connect( late, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( late ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    lifecycle_check_client_send_dropped( late );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_send_dropped_while_connecting );

static void client_send_dropped_after_disconnect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1504 );
    lifecycle_exchange( &world, 0, 0xA7, 2 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 0 );

    // the client disconnects itself: DISCONNECTED, no connection object
    lifecycle_disconnect( &world, 0 );
    lifecycle_check_client_send_dropped( client );

    // reconnect: the counter is a lifetime counter, kept across connections,
    // and nothing dropped is sent on the new one
    lifecycle_connect( &world, 0, 0x1505 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 1 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA8 );

    // the server ends the connection: DISCONNECTED while its object exists
    rocketnet_server_disconnect_client( world.server, rocketnet_client_index( client ) );
    for ( int i = 0; i < 200 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_check_client_send_dropped( client );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 2 );

    lifecycle_connect( &world, 0, 0x1506 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS ) == 2 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA9 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_send_dropped_after_disconnect );

static void server_send_dropped_to_slot_not_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 1 );
    rocketnet_server_t * server = world.server;
    lifecycle_connect( &world, 0, 0x1507 );
    const uint64_t index = rocketnet_client_index( world.clients[0] );
    const uint64_t free_slot = ( index + 1 ) % 3;
    check( rocketnet_server_client_connected( server, free_slot ) == 0 );
    check( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS ) == 0 );

    // a free slot below max_clients
    lifecycle_check_server_send_dropped( server, free_slot );

    // an index at or past max_clients stays a counted misuse, and is no drop
    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( server, 3, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( rocketnet_server_finish_message( server, 3, 2 ) == 0 );
    check( rocketnet_server_start_message( server, 1000, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 3 );
    check( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS ) == 1 );

    // the connected slot still sends; then its client leaves and the slot drops
    lifecycle_exchange( &world, 0, 0xB1, 1 );
    lifecycle_disconnect( &world, 0 );
    lifecycle_check_server_send_dropped( server, index );
    check( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS ) == 2 );

    // a stopped server: the same drop, the counter kept across the stop
    rocketnet_server_stop( server );
    check( rocketnet_server_running( server ) == 0 );
    check( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS ) == 2 );
    lifecycle_check_server_send_dropped( server, 0 );
    lifecycle_check_server_send_dropped( server, 2 );
    misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( server, 3, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( rocketnet_server_finish_message( server, 3, 2 ) == 0 );
    check( rocketnet_api_misuse_count() == misuse + 2 );
    check( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS ) == 4 );

    // start again: nothing dropped is sent to the client that connects next
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1508 );
    check( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS ) == 4 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xB2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( server_send_dropped_to_slot_not_connected );

// set_send_kbps follows the same rule: a connection that is not CONNECTED has
// no rate to set. The call returns INVALID_ARGUMENT, changes nothing and is
// not misuse; once CONNECTED the same call is OK.
static void server_set_send_kbps_refused_unless_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_server_t * server = world.server;
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_set_send_kbps( server, 0, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, 1, 0 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );

    lifecycle_connect( &world, 0, 0x1509 );
    const uint64_t index = rocketnet_client_index( world.clients[0] );
    const uint64_t other = 1 - index;
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
REGISTER_UNIT_TEST( server_set_send_kbps_refused_unless_connected );

static void client_set_send_kbps_refused_unless_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // every handshake state refuses
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    lifecycle_token( rocketnet_server_port( world.server ), &world.identity, 0x150B, 300, token );
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
    const uint64_t index = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world.server, index ); i++ )
        lifecycle_pump( &world );
    lifecycle_exchange( &world, 0, 0xB4, 1 );

    // after disconnect: refused again
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_set_send_kbps_refused_unless_connected );

static void server_start_stop_start_again()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 2 );
    lifecycle_connect( &world, 0, 0x1601 );
    lifecycle_connect( &world, 1, 0x1602 );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );
    const uint64_t a = rocketnet_client_index( world.clients[0] );
    const uint64_t b = rocketnet_client_index( world.clients[1] );
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
    check( rocketnet_server_update( world.server, world.time ) == ROCKETNET_UPDATE_OK );
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
    check( rocketnet_server_counter( world.server, ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED ) == 4 );
    lifecycle_world_destroy( &world );      // destroys a running server
}
REGISTER_UNIT_TEST( server_start_stop_start_again );

static void server_stop_disconnects_every_client()
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
REGISTER_UNIT_TEST( server_stop_disconnects_every_client );

static void server_disconnect_all_clients()
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
REGISTER_UNIT_TEST( server_disconnect_all_clients );

static void connection_reset_client_reconnect_delivers()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1901 );
    lifecycle_exchange( &world, 0, 0x51, 2 );

    // mid-transfer: queue messages both ways, let one update run, then end it
    const uint64_t index = rocketnet_client_index( client );
    for ( int i = 0; i < 8; i++ )
    {
        lifecycle_client_send( client, 0x5F, (uint8_t) i );
        lifecycle_server_send( world.server, index, 0x5F, (uint8_t) i );
    }
    lifecycle_pump( &world );
    const uint8_t * data = NULL;
    int bytes = 0;
    while ( rocketnet_server_receive_message( world.server, index, 0, &data, &bytes ) )
        rocketnet_server_release_message( world.server, index, data );
    lifecycle_disconnect( &world, 0 );

    // the new connection's first messages deliver, and nothing of the old
    lifecycle_connect( &world, 0, 0x1902 );
    lifecycle_exchange( &world, 0, 0x52, 4 );
    for ( int step = 0; step < 50; step++ )
    {
        lifecycle_pump( &world );
        const uint64_t now_index = rocketnet_client_index( client );
        check( rocketnet_server_receive_message( world.server, now_index, 0, &data, &bytes ) == 0 );
        check( rocketnet_client_receive_message( client, 0, &data, &bytes ) == 0 );
    }
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( connection_reset_client_reconnect_delivers );

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

static void server_destination_flow_id_refused()
{
    check( ROCKETNET_FLOW_ID_BIT == ( 1ULL << 63 ) );
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    lifecycle_connect( &world, 0, 0x1A01 );
    const uint64_t flow = ROCKETNET_FLOW_ID_BIT | rocketnet_client_index( world.clients[0] );

    // the flow refusal names FLOW_ID_RESERVED; an index merely out of range
    // (no flow bit) is refused without it
    rocketnet_set_quiet( 0 );
    rocketnet_set_log_callback( lifecycle_log_callback, NULL );
    lifecycle_flow_logs = 0;
    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( world.server, flow, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 && lifecycle_flow_logs == 1 );
    misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_finish_message( world.server, flow, 4 ) == 0 );
    check( rocketnet_api_misuse_count() == misuse + 1 && lifecycle_flow_logs == 2 );
    misuse = rocketnet_api_misuse_count();
    rocketnet_server_abort_message( world.server, flow );
    check( rocketnet_api_misuse_count() == misuse + 1 && lifecycle_flow_logs == 3 );
    check( rocketnet_server_start_message( world.server, 5, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( rocketnet_server_finish_message( world.server, 5, 4 ) == 0 );
    check( lifecycle_flow_logs == 3 );
    rocketnet_set_log_callback( NULL, NULL );
    rocketnet_set_quiet( 1 );
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_FLOW_ID_RESERVED ), "FLOW_ID_RESERVED" ) == 0 );

    // the slot's own index still works
    lifecycle_exchange( &world, 0, 0x61, 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( server_destination_flow_id_refused );

static void server_client_index_round_trips_as_uint64()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 3 );
    for ( int i = 0; i < 3; i++ )
        lifecycle_connect( &world, i, 0x1B00 + (uint64_t) i );
    for ( int i = 0; i < 3; i++ )
    {
        const uint64_t index = rocketnet_client_index( world.clients[i] );
        check( index < 3 && ( index & ROCKETNET_FLOW_ID_BIT ) == 0 );
        check( rocketnet_server_client_connected( world.server, index ) == 1 );
        check( rocketnet_server_client_id( world.server, index ) == 0x1B00 + (uint64_t) i );
        check( rocketnet_server_disconnect_reason( world.server, index ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( rocketnet_server_set_send_kbps( world.server, index, 2000 ) == ROCKETNET_STATUS_OK );
        uint8_t * out = rocketnet_server_start_message( world.server, index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
        check( out != NULL );
        rocketnet_server_abort_message( world.server, index );
        struct rocketnet_receipt_t receipts[4];
        check( rocketnet_server_receipts( world.server, index, receipts, 4 ) >= 0 );
        (void) rocketnet_server_client_counter( world.server, index, ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT );
        (void) rocketnet_server_client_stat( world.server, index, ROCKETNET_SERVER_CLIENT_STAT_RTT );
        lifecycle_exchange( &world, i, (uint8_t) ( 0x70 + i ), 1 );
        // an index past max_clients, and one with the high bit, name no client
        check( rocketnet_server_client_connected( world.server, index + 3 ) == 0 );
        check( rocketnet_server_client_connected( world.server, index | ROCKETNET_FLOW_ID_BIT ) == 0 );
        check( rocketnet_server_client_id( world.server, index | ( 1ULL << 32 ) ) == 0 );
    }
    rocketnet_server_disconnect_client( world.server, rocketnet_client_index( world.clients[0] ) );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( server_client_index_round_trips_as_uint64 );

// packets_per_update takes 1 .. 1024 and defaults to 256, client and
// server, through the struct and through the environment.
#if defined(_WIN32)
static int lifecycle_setenv( const char * name, const char * value ) { return _putenv_s( name, value ); }
static int lifecycle_unsetenv( const char * name ) { return _putenv_s( name, "" ); }
#else
static int lifecycle_setenv( const char * name, const char * value ) { return setenv( name, value, 1 ); }
static int lifecycle_unsetenv( const char * name ) { return unsetenv( name ); }
#endif

static void config_packets_per_update_range()
{
    rocketnet_set_quiet( 1 );
    lifecycle_identity_t identity;
    lifecycle_identity( &identity );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    check( client_config.packets_per_update == 256 );
    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &identity, 1 );
    check( server_config.packets_per_update == 256 );

    // the struct
    client_config.packets_per_update = 1024;
    check( rocketnet_client_validate_config( &client_config ) == ROCKETNET_STATUS_OK );
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( client != NULL );
    rocketnet_client_destroy( client );
    client_config.packets_per_update = 1025;
    check( rocketnet_client_validate_config( &client_config ) != ROCKETNET_STATUS_OK );
    check( rocketnet_client_create( NULL, &client_config, NULL, 0 ) == NULL );
    client_config.packets_per_update = 0;
    check( rocketnet_client_validate_config( &client_config ) != ROCKETNET_STATUS_OK );

    server_config.packets_per_update = 1024;
    check( rocketnet_server_validate_config( &server_config ) == ROCKETNET_STATUS_OK );
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( server != NULL );
    rocketnet_server_destroy( server );
    server_config.packets_per_update = 1025;
    check( rocketnet_server_validate_config( &server_config ) != ROCKETNET_STATUS_OK );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 ) == NULL );
    server_config.packets_per_update = 0;
    check( rocketnet_server_validate_config( &server_config ) != ROCKETNET_STATUS_OK );

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
    client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( client != NULL );
    if ( client ) rocketnet_client_destroy( client );
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
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
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RESEND_DELAY_INITIAL_MS", "9" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RESEND_DELAY_INITIAL_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RESEND_DELAY_MIN_MS", "2001" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RESEND_DELAY_MIN_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RESEND_DELAY_MAX_MS", "9" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RESEND_DELAY_MAX_MS" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_SERVER_RESEND_DELAY_INITIAL_MS", "9" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RESEND_DELAY_INITIAL_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RESEND_DELAY_MIN_MS", "2001" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RESEND_DELAY_MIN_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RESEND_DELAY_MAX_MS", "9" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RESEND_DELAY_MAX_MS" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE", "1024" ) == 0 );
    client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( client != NULL );
    rocketnet_client_destroy( client );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE", "1025" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE", "1024" ) == 0 );
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( server != NULL );
    rocketnet_server_destroy( server );
    check( lifecycle_setenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE", "1025" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE" ) == 0 );
}
REGISTER_UNIT_TEST( config_packets_per_update_range );

// A connection the server ends leaves the client DISCONNECTED with its
// connection object still held; connect straight after (no disconnect call)
// ends it first, and the lifetime counters only grow.
static void client_reconnects_after_server_stop_without_disconnect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1C01 );
    lifecycle_exchange( &world, 0, 0x81, 3 );
    rocketnet_server_stop( world.server );
    for ( int i = 0; i < 50 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    uint64_t before[ROCKETNET_CLIENT_COUNTER_COUNT];
    for ( int i = 0; i < ROCKETNET_CLIENT_COUNTER_COUNT; i++ )
        before[i] = rocketnet_client_counter( client, (enum rocketnet_client_counter_t) i );
    check( before[ROCKETNET_CLIENT_COUNTER_PACKETS_SENT] > 0 );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1C02 );      // connect, no disconnect first
    for ( int i = 0; i < ROCKETNET_CLIENT_COUNTER_COUNT; i++ )
        check( rocketnet_client_counter( client, (enum rocketnet_client_counter_t) i ) >= before[i] );
    lifecycle_exchange( &world, 0, 0x82, 3 );
    check( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_PACKETS_SENT ) > before[ROCKETNET_CLIENT_COUNTER_PACKETS_SENT] );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_reconnects_after_server_stop_without_disconnect );

// A connection that ends with a reason keeps it until the next connection is
// established: a config mismatch (num_channels is a WIRE field) ends the
// first, the reason reads through disconnect and the next handshake, and the
// next CONNECTED reads NONE.
static void client_disconnect_reason_kept_until_next_connected()
{
    lifecycle_world_t world;
    memset( &world, 0, sizeof( world ) );
    rocketnet_set_quiet( 1 );
    world.time = 1000.0;
    lifecycle_now = world.time;
    lifecycle_identity( &world.identity );
    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &world.identity, 1 );     // 32 channels
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( world.server && rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    rocketnet_client_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.num_channels = 4;
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    world.num_clients = 1;
    rocketnet_client_t * client = world.clients[0];
    check( client != NULL );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    lifecycle_token( rocketnet_server_port( world.server ), &world.identity, 0x1D01, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 2000 && rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE; i++ )
        lifecycle_pump( &world );
    const int reason = rocketnet_client_disconnect_reason( client );
    check( reason == ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH || reason == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT );
    check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    rocketnet_client_disconnect( client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_disconnect_reason( client ) == reason );

    // a matching server: during the handshake the old reason still reads
    rocketnet_server_destroy( world.server );
    server_config.num_channels = 4;
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( world.server && rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    lifecycle_token( rocketnet_server_port( world.server ), &world.identity, 0x1D02, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_disconnect_reason( client ) == reason );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lifecycle_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_disconnect_reason_kept_until_next_connected );

// connect with an expired token: the status and the matching error state,
// refused again until disconnect.
static void client_connect_token_expired()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    rocketnet_client_t * client = world.clients[0];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world.server ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( world.identity.backend_secret, world.identity.server_public,
        LIFECYCLE_PROTOCOL, addresses, 1, 0x1E01, 0, 1, 300, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    rocketnet_client_disconnect( client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_connect( &world, 0, 0x1E02 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_connect_token_expired );

// One update sends more packets than one builder call tracks
// (ROCKETNET_PACKET_BUILDER_MAX_PACKETS, 16): the per-update limit is the
// config's packets_per_update.
static void client_one_update_sends_more_than_sixteen_packets()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 0 );
    rocketnet_client_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.send_kbps = 0;                    // no budget: the packet limit alone bounds the update
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL, 0 );
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
    const uint64_t before = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_PACKETS_SENT );
    lifecycle_now = world.time;
    check( rocketnet_client_update( client, world.time ) == ROCKETNET_UPDATE_OK );
    rocketnet_client_send_packets( client );
    const uint64_t sent = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_PACKETS_SENT ) - before;
    check( sent > ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( client_one_update_sends_more_than_sixteen_packets );
