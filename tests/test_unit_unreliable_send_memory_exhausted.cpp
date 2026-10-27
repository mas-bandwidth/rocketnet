/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// An unreliable send that finds no room in the connection's send region ends
// the connection with MESSAGE_MEMORY_EXHAUSTED, as a reliable one does
// (rn_endpoint_start in sdk/rocketnet_endpoint.cpp). The send region is filled
// through the public API with reliable messages, then an unreliable start is
// made: it returns NULL and the update ends the connection. The client and the
// server's slot are checked. In memory over the loopback transport, step
// driven, no clock, sockets or network; time starts at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "token_string.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define UNRELIABLE_FULL_PROTOCOL 0x7A11C0DE5E4D0001ULL

static double g_unreliable_full_now = 1000.0;
static double unreliable_full_clock( void * ) { return g_unreliable_full_now; }

static void unreliable_full_frame( rocketnet_server_t * server, rocketnet_client_t * client )
{
    rocketnet_client_update( client, g_unreliable_full_now );
    while ( rocketnet_client_receive_messages( client ) != NULL ) {}
    rocketnet_client_send_packets( client );
    rocketnet_server_update( server, g_unreliable_full_now );
    while ( rocketnet_server_receive_messages( server ) != NULL ) {}
    rocketnet_server_send_packets( server );
    g_unreliable_full_now += 0.01;
}

static void test_unreliable_send_without_message_memory_ends_connection()
{
    rocketnet_quiet( true );
    g_unreliable_full_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x4C ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = UNRELIABLE_FULL_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    struct rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = 1;
    client_config.time_callback = unreliable_full_clock;
    create_status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, server, UNRELIABLE_FULL_PROTOCOL, addresses, 1,
        (uint64_t) 0x91, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 400 && !( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
                                   rocketnet_server_num_connected_clients( server ) == 1 ); i++ )
        unreliable_full_frame( server, client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int slot = rocketnet_client_index( client );
    check( slot == 0 );

    const int bytes = 1000;
    const enum rocketnet_reliability_t reliable = ROCKETNET_RELIABILITY_RELIABLE_ORDERED;

    // with room, an unreliable start succeeds and is abandoned
    uint8_t * open = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
    check( open != NULL );
    rocketnet_client_abort_message( client );
    open = rocketnet_server_start_message( server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
    check( open != NULL );
    rocketnet_server_abort_message( server );

    // fill both send regions through the public API, asking first
    int client_sent = 0;
    while ( rocketnet_client_can_send_message( client, 0, reliable, bytes ) )
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, reliable, bytes );
        check( p != NULL );
        memset( p, client_sent & 0xFF, (size_t) bytes );
        check( rocketnet_client_finish_message( client, bytes ) != 0 );
        client_sent++;
        check( client_sent < 1000000 );
    }
    check( client_sent > 0 );
    int server_sent = 0;
    while ( rocketnet_server_can_send_message( server, slot, 0, reliable, bytes ) )
    {
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slot, 0, reliable, bytes );
        check( p != NULL );
        memset( p, server_sent & 0xFF, (size_t) bytes );
        check( rocketnet_server_finish_message( server, bytes ) != 0 );
        server_sent++;
        check( server_sent < 1000000 );
    }
    check( server_sent > 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server, slot ) );

    // the unreliable start finds no room: NULL, and the connection ends
    check( !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes ) );
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes ) == NULL );
    check( !rocketnet_server_can_send_message( server, slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes ) );
    check( rocketnet_server_start_message( server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes ) == NULL );

    rocketnet_client_update( client, g_unreliable_full_now );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED );
    rocketnet_server_update( server, g_unreliable_full_now );
    check( rocketnet_server_disconnect_reason( server, slot ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( !rocketnet_server_client_connected( server, slot ) );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    rocketnet_quiet( false );
}
REGISTER_UNIT_TEST( test_unreliable_send_without_message_memory_ends_connection );
