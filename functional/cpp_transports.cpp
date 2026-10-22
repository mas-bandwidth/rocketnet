/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The real connect through the C transport API from a C++ translation unit
    over loopback: a server transport and a client transport connect over
    127.0.0.1 with the development keys, exchange payloads each way, and
    disconnect. The checks mirror test_loopback_transport_udp.
*/

#include "../test/native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// defined in functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

static void rocketnet_cpp_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_cpp_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
    }                                                                                                           \
} while(0)

#define ROCKETNET_COUNTERS_CHECK rocketnet_check
#include "../test/counters_expect.h"

void rocketnet_test_cpp_transports_connect_over_loopback()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t loopback_connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), loopback_connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, loopback_connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    // pump both until connected, bounded by the wall clock
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_client_transport_client_index( client ) == 0 );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    rocketnet_check( rocketnet_server_transport_is_client_connected( server, 0 ) );

    // payloads both ways through the transport surfaces
    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 16 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello", 5 );
            sent_to_server++;
        }
        if ( sent_to_client < 16 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world", 5 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5 );
            rocketnet_check( memcmp( payload, "hello", 5 ) == 0 );
            server_received++;
        }

        // out-of-range indices answer safely rather than trusting the caller
        int oob_client_index, oob_payload_bytes;
        rocketnet_check( rocketnet_server_transport_payload( server, num_payloads, &oob_client_index, &oob_payload_bytes ) == NULL );
        rocketnet_check( rocketnet_server_transport_payload( server, -1, &oob_client_index, &oob_payload_bytes ) == NULL );

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "world", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 16 && client_received >= 16 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 16 );
    rocketnet_check( client_received >= 16 );

    // the deliberate disconnect crosses the wire and the server empties
    rocketnet_client_transport_disconnect( client );
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 )
    {
        // the client keeps updating while the server waits: a disconnect burst whose
        // hand-off found the other send ring busy is deferred and goes out on
        // the client's next update (sdk/rocketnet_client_transport.cpp
        // rocketnet_client_transport_disconnect and _fill_destination), not before
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    rocketnet_counters_assert_conserved( rocketnet_client_transport_counters( client ) );
    rocketnet_counters_assert_clean( rocketnet_client_transport_counters( client ) );
    rn_native_client_transport_stop_and_destroy( client );

    rocketnet_server_transport_stop( server );
    // Stop may need to drain asynchronous I/O. The public contract requires
    // waiting for STOPPED, not a particular number of updates.
    start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED &&
            rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );

    rocketnet_counters_assert_conserved( rocketnet_server_transport_counters( server ) );
    rocketnet_counters_assert_clean( rocketnet_server_transport_counters( server ) );
    rn_native_server_transport_stop_and_destroy( server );
}
