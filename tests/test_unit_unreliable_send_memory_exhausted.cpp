/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// An unreliable send that finds no room in the connection's send region
// (after the in-place transport frame path does not take it, or when an
// oversized unreliable send promotes to its reliable twin) ends the
// connection with MESSAGE_MEMORY_EXHAUSTED and returns NULL, on both
// client and server. Step-driven over loopback, no real time, sockets
// or threads; time starts at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "token_string.h"

#include <stdio.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

#define UNRELIABLE_EXHAUST_PROTOCOL 0x6A3F91C0B4E27D15ULL

static double g_unreliable_exhaust_now = 1000.0;
static double unreliable_exhaust_clock( void * ) { return g_unreliable_exhaust_now; }

static int g_unreliable_exhaust_asserts = 0;
static void unreliable_exhaust_count_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    g_unreliable_exhaust_asserts++;
}

static void unreliable_exhaust_frame( rocketnet_server_t * server, rocketnet_client_t ** clients, int num_clients )
{
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_update( clients[i], g_unreliable_exhaust_now );
        while ( rocketnet_client_receive_messages( clients[i] ) != NULL )
        {
        }
        rocketnet_client_send_packets( clients[i] );
    }
    rocketnet_server_update( server, g_unreliable_exhaust_now );
    while ( rocketnet_server_receive_messages( server ) != NULL )
    {
    }
    rocketnet_server_send_packets( server );
    g_unreliable_exhaust_now += 0.01;
}

static void run_unreliable_exhaust_case( enum rocketnet_reliability_t probe_reliability, int probe_bytes )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x4D ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = UNRELIABLE_EXHAUST_PROTOCOL;
    server_config.max_clients = 3;
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    server_config.packets_per_update = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    rocketnet_client_t * clients[2];
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_test_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = 1;
        client_config.time_callback = unreliable_exhaust_clock;
        client_config.packets_per_update = 1;
        create_status = ROCKETNET_STATUS_OK;
        clients[i] = rocketnet_client_create( NULL, &client_config, &create_status );
        check( clients[i] != NULL );
        check( create_status == ROCKETNET_STATUS_OK );

        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        check( rn_test_server_token_string( backend_secret, server, UNRELIABLE_EXHAUST_PROTOCOL, addresses, 1,
                                            (uint64_t) ( 0x81 + i ), 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
        check( rocketnet_client_connect( clients[i], token ) == ROCKETNET_STATUS_OK );
    }

    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_num_connected_clients( server ) == 2 )
            break;
        unreliable_exhaust_frame( server, clients, 2 );
    }
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int server_slot = rocketnet_client_index( clients[1] );
    const int other_slot = rocketnet_client_index( clients[0] );
    check( server_slot >= 0 && server_slot < 3 );
    check( other_slot >= 0 && other_slot < 3 && other_slot != server_slot );

    // Fill client[0]'s send region through the public API using unreliable sends.
    int client_sent = 0;
    while ( rocketnet_client_can_send_message( clients[0], 0, probe_reliability, probe_bytes ) )
    {
        uint8_t * p = rocketnet_client_start_message( clients[0], 0, probe_reliability, probe_bytes );
        check( p != NULL );
        memset( p, (int) ( client_sent & 0xFF ), (size_t) probe_bytes );
        check( rocketnet_client_finish_message( clients[0], probe_bytes ) != 0 );
        client_sent++;
        check( client_sent < 1000000 );
    }
    check( client_sent > 1 );
    check( !rocketnet_client_can_send_message( clients[0], 0, probe_reliability, probe_bytes ) );
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    // Fill server_slot's send region through the public API using unreliable sends.
    int server_sent = 0;
    while ( rocketnet_server_can_send_message( server, server_slot, 0, probe_reliability, probe_bytes ) )
    {
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) server_slot, 0, probe_reliability, probe_bytes );
        check( p != NULL );
        memset( p, (int) ( server_sent & 0xFF ), (size_t) probe_bytes );
        check( rocketnet_server_finish_message( server, probe_bytes ) != 0 );
        server_sent++;
        check( server_sent < 1000000 );
    }
    check( server_sent > 1 );
    check( !rocketnet_server_can_send_message( server, server_slot, 0, probe_reliability, probe_bytes ) );
    check( rocketnet_server_client_connected( server, server_slot ) );
    check( rocketnet_server_can_send_message( server, other_slot, 0, probe_reliability, probe_bytes ) );

    // Starting an unreliable send without message memory returns NULL and ends
    // the connection with MESSAGE_MEMORY_EXHAUSTED on both client and server.
    check( rocketnet_client_start_message( clients[0], 0, probe_reliability, probe_bytes ) == NULL );
    check( rocketnet_server_start_message( server, (uint64_t) server_slot, 0, probe_reliability, probe_bytes ) == NULL );

    rocketnet_client_update( clients[0], g_unreliable_exhaust_now );
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED );
    check( !rocketnet_client_can_send_message( clients[0], 0, probe_reliability, probe_bytes ) );

    rocketnet_server_update( server, g_unreliable_exhaust_now );
    check( !rocketnet_server_client_connected( server, server_slot ) );
    check( rocketnet_server_disconnect_reason( server, server_slot ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( !rocketnet_server_can_send_message( server, server_slot, 0, probe_reliability, probe_bytes ) );

    for ( int i = 0; i < 2; i++ )
        rn_test_client_stop_and_destroy( clients[i] );
    rn_test_server_stop_and_destroy( server );
}

static void test_unreliable_send_without_message_memory_ends_connection()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( unreliable_exhaust_count_assert );
    g_unreliable_exhaust_asserts = 0;
    g_unreliable_exhaust_now = 1000.0;

    run_unreliable_exhaust_case( ROCKETNET_RELIABILITY_UNRELIABLE, 64 );
    run_unreliable_exhaust_case( ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 64 );
    run_unreliable_exhaust_case( ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 64 );
    run_unreliable_exhaust_case( ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES + 1 );

    check( g_unreliable_exhaust_asserts == 0 );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}
REGISTER_UNIT_TEST( test_unreliable_send_without_message_memory_ends_connection );
