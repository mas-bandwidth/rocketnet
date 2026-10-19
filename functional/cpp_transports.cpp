/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The real connect through the C++ transport interfaces over loopback: a
    PortableServerTransport and a PortableClientTransport, used only through
    their ServerTransport and ClientTransport base references, connect over
    127.0.0.1 with the development keys, exchange payloads each way, and
    disconnect. The checks mirror test_transport_udp_loopback.
*/

#include "rocketnet_config.h"
#include "rocketnet.hpp"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    using namespace rocketnet;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config_current( &server_config );
    server_config.max_clients = 16;

    PortableServerTransport * portable_server = PortableServerTransport::create( NULL, "127.0.0.1", &server_config );
    rocketnet_check( portable_server );
    ServerTransport * server = portable_server;

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", server->port() );

    uint8_t loopback_connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), loopback_connect_token ) );

    PortableClientTransport * portable_client = PortableClientTransport::create( NULL, loopback_connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( portable_client );
    ClientTransport * client = portable_client;

    // pump both until connected, bounded by the wall clock
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        client->update( rocketnet_platform_time() );
        server->update( rocketnet_platform_time() );
        if ( client->state() == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             server->num_connected_clients() == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client->state() == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( client->client_index() == 0 );
    rocketnet_check( server->num_connected_clients() == 1 );
    rocketnet_check( server->is_client_connected( 0 ) );

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
            client->send_packet( (const uint8_t*) "hello", 5 );
            sent_to_server++;
        }
        if ( sent_to_client < 16 )
        {
            server->send_packet( 0, (const uint8_t*) "world", 5 );
            sent_to_client++;
        }

        server->send_packets();

        client->update( rocketnet_platform_time() );
        server->update( rocketnet_platform_time() );

        const int num_payloads = server->num_payloads();
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = server->payload( i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5 );
            rocketnet_check( memcmp( payload, "hello", 5 ) == 0 );
            server_received++;
        }

        // out-of-range indices answer safely rather than trusting the caller
        int oob_client_index, oob_payload_bytes;
        rocketnet_check( server->payload( num_payloads, &oob_client_index, &oob_payload_bytes ) == NULL );
        rocketnet_check( server->payload( -1, &oob_client_index, &oob_payload_bytes ) == NULL );

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = client->receive_packet( packet, (int) sizeof(packet) ) ) > 0 )
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
    client->disconnect();
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 )
    {
        server->update( rocketnet_platform_time() );
        if ( server->num_connected_clients() == 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server->num_connected_clients() == 0 );
    rocketnet_check( client->state() == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    rocketnet_counters_assert_conserved( client->counters() );
    rocketnet_counters_assert_clean( client->counters() );
    delete client;

    server->stop();
    // Stop may need to drain asynchronous I/O. The public contract requires
    // waiting for STOPPED, not a particular number of updates.
    start = rocketnet_platform_time();
    while ( server->state() != ROCKETNET_SERVER_TRANSPORT_STOPPED &&
            rocketnet_platform_time() - start < 5.0 )
    {
        server->update( rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server->state() == ROCKETNET_SERVER_TRANSPORT_STOPPED );

    rocketnet_counters_assert_conserved( server->counters() );
    rocketnet_counters_assert_clean( server->counters() );
    delete server;
}
