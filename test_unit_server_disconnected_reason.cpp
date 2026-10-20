/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client can tell that the server ended its connection: receiving the
// server's disconnect packet reports ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED
// within one client update, for rocketnet_server_disconnect_client,
// rocketnet_server_stop and rocketnet_server_destroy. A deliberate client
// disconnect and a connection timeout still read NONE. Everything runs over
// the in-memory loopback transport with step-driven clients: no socket, no
// thread, no sleep, and time is an argument starting at 1000.0.

#include "test_unit_registry.h"
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_internal.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check
#define SERVER_DISCONNECTED_PROTOCOL_ID 0x5344434F4E4E01ULL
#define SERVER_DISCONNECTED_MAX_CLIENTS 4

struct server_disconnected_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
};

static double server_disconnected_now = 1000.0;
static double server_disconnected_clock( void * ) { return server_disconnected_now; }

static void server_disconnected_identity( server_disconnected_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x51 ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_t * server_disconnected_server_create( const server_disconnected_identity_t * identity )
{
    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = SERVER_DISCONNECTED_PROTOCOL_ID;
    server_config.max_clients = SERVER_DISCONNECTED_MAX_CLIENTS;
    memcpy( server_config.backend_public_key, identity->backend_public, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, identity->server_public, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, identity->server_secret, sizeof( server_config.server_secret_key ) );
    server_config.transport = "loopback";
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    return server;
}

static rocketnet_client_t * server_disconnected_client_create( const server_disconnected_identity_t * identity, uint16_t port,
                                                               uint64_t client_id, int32_t timeout_seconds )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
        SERVER_DISCONNECTED_PROTOCOL_ID, addresses, 1, client_id, 1000, 1ULL << 40, timeout_seconds, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = server_disconnected_clock;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( client != NULL );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    return client;
}

static int server_disconnected_connect_client( rocketnet_server_t * server, rocketnet_client_t * client )
{
    for ( int i = 0; i < 400; i++ )
    {
        server_disconnected_now = 1000.0 + (double) i * 0.01;
        check( rocketnet_client_update( client, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
        rocketnet_client_send_packets( client );
        check( rocketnet_server_update( server, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
        rocketnet_server_send_packets( server );

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const int slot = rocketnet_client_index( client );
            if ( rocketnet_server_client_connected( server, slot ) )
                return slot;
        }
    }
    check( false );
    return -1;
}

// DONE-WHEN (3): a client that receives the server's disconnect packet reports
// ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED within one client update,
// for rocketnet_server_disconnect_client, rocketnet_server_stop and
// rocketnet_server_destroy, and the reason is kept until the next connection
// is established, then reads NONE.
static void server_disconnect_sets_server_disconnected_reason()
{
    rocketnet_quiet( 1 );
    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    // rocketnet_server_disconnect_client
    {
        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create( &identity, rocketnet_server_port( server ), 0xF101ULL, 300 );
        const int slot = server_disconnected_connect_client( server, client );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

        rocketnet_server_disconnect_client( server, slot );
        rocketnet_server_send_packets( server );
        check( rocketnet_client_update( client, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
        check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

        // the reason is kept until the next connection is established, then NONE
        rocketnet_server_t * server2 = server_disconnected_server_create( &identity );
        char address2[64];
        snprintf( address2, sizeof( address2 ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server2 ) );
        const char * addresses2[] = { address2 };
        uint8_t token2[ROCKETNET_CONNECT_TOKEN_BYTES];
        check( rocketnet_write_production_connect_token( identity.backend_secret, identity.server_public,
            SERVER_DISCONNECTED_PROTOCOL_ID, addresses2, 1, 0xF102ULL, 1000, 1ULL << 40, 300, NULL,
            token2, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        check( rocketnet_client_connect( client, token2 ) == ROCKETNET_STATUS_OK );
        bool connected2 = false;
        for ( int i = 0; i < 400 && !connected2; i++ )
        {
            server_disconnected_now = 1000.0 + (double) i * 0.01;
            check( rocketnet_client_update( client, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
            rocketnet_client_send_packets( client );
            check( rocketnet_server_update( server2, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
            rocketnet_server_send_packets( server2 );
            if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
                connected2 = true;
        }
        check( connected2 );
        check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );

        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server2 );
        rocketnet_server_destroy( server );
    }

    // rocketnet_server_stop
    {
        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create( &identity, rocketnet_server_port( server ), 0xF111ULL, 300 );
        server_disconnected_connect_client( server, client );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

        rocketnet_server_stop( server );
        check( rocketnet_client_update( client, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
    }

    // rocketnet_server_destroy
    {
        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create( &identity, rocketnet_server_port( server ), 0xF121ULL, 300 );
        server_disconnected_connect_client( server, client );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

        rocketnet_server_destroy( server );
        check( rocketnet_client_update( client, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

        rocketnet_client_destroy( client );
    }
}

REGISTER_UNIT_TEST( server_disconnect_sets_server_disconnected_reason );

// DONE-WHEN (4): a client-initiated rocketnet_client_disconnect still reads
// NONE, and a connection timeout still reads NONE with state
// ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT.
static void client_initiated_disconnect_and_timeout_read_none()
{
    rocketnet_quiet( 1 );
    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    // client-initiated disconnect reads NONE
    {
        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create( &identity, rocketnet_server_port( server ), 0xF201ULL, 300 );
        server_disconnected_connect_client( server, client );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

        rocketnet_client_disconnect( client );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );

        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
    }

    // a connection timeout (the server goes silent, no disconnect packet)
    // still reads NONE with state CONNECTION_TIMED_OUT
    {
        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create( &identity, rocketnet_server_port( server ), 0xF202ULL, 5 );
        server_disconnected_connect_client( server, client );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

        // the server goes silent: only the client advances, so nothing is
        // received and the connection times out to CONNECTION_TIMED_OUT
        bool timed_out = false;
        for ( int i = 0; i < 400 && !timed_out; i++ )
        {
            server_disconnected_now = server_disconnected_now + 0.05;
            check( rocketnet_client_update( client, server_disconnected_now ) == ROCKETNET_UPDATE_OK );
            rocketnet_client_send_packets( client );
            if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT )
                timed_out = true;
        }
        check( timed_out );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT );
        check( rocketnet_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );

        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
    }
}

REGISTER_UNIT_TEST( client_initiated_disconnect_and_timeout_read_none );
