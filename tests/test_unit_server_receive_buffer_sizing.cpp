/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server receive buffer request (rocketnet_internal.h, USAGE.md "Receive
// buffer sizing"): one update's inbound packets at max_clients, times the
// margin, never below ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE and capped at
// INT_MAX. Pure arithmetic: no socket is opened.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_constants.h"
#include "rocketnet_layers.h"

#include <limits.h>

#define check rocketnet_unit_check

static void test_server_receive_buffer_bytes_formula()
{
    // the stated constants the values below are computed from

    check( ROCKETNET_SERVER_RECEIVE_BUFFER_MARGIN == 2 );
    check( ROCKETNET_SERVER_RECEIVE_MIN_UPDATE_HZ == 10 );
    check( ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE == 1000000 );
    check( ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS == 1000 );

    // 1000 clients at the default 1000 kbps: 125,000 bytes/s each, 125 MB/s,
    // x 2 margin, over one update interval

    check( rocketnet_server_receive_buffer_bytes( 1000, 1000, 10 ) == 25000000 );
    check( rocketnet_server_receive_buffer_bytes( 1000, 1000, 20 ) == 12500000 );
    check( rocketnet_server_receive_buffer_bytes( 1000, 1000, 30 ) == 8333334 ); // 8,333,333.3 rounded up
    check( rocketnet_server_receive_buffer_bytes( 1000, 1000, 100 ) == 2500000 );

    // 1000 clients at 256 kbps: 32,000 bytes/s each, 3.2 MB per 0.1 s update

    check( rocketnet_server_receive_buffer_bytes( 1000, 256, 10 ) == 6400000 );
    check( rocketnet_server_receive_buffer_bytes( 1000, 256, 20 ) == 3200000 );
    check( rocketnet_server_receive_buffer_bytes( 1000, 256, 30 ) == 2133334 );
    check( rocketnet_server_receive_buffer_bytes( 1000, 256, 100 ) == 1000000 ); // 640,000: the floor

    // one client is always the floor

    check( rocketnet_server_receive_buffer_bytes( 1, 1000, 10 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    check( rocketnet_server_receive_buffer_bytes( 1, 1000, 20 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    check( rocketnet_server_receive_buffer_bytes( 1, 1000, 30 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    check( rocketnet_server_receive_buffer_bytes( 1, 1000, 100 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    check( rocketnet_server_receive_buffer_bytes( 1, 256, 10 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );

    // the request every server socket makes: max_clients at the default
    // client send budget and the slowest supported update rate

    check( rocketnet_server_receive_buffer_request( 1000 ) == 25000000 );
    check( rocketnet_server_receive_buffer_request( 1 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    check( rocketnet_server_receive_buffer_request( 64 ) == 1600000 );

    // capped at INT_MAX, without overflow at the largest arguments

    check( rocketnet_server_receive_buffer_bytes( 10000, 1000000, 1 ) == INT_MAX );
    check( rocketnet_server_receive_buffer_bytes( INT_MAX, INT_MAX, 1 ) == INT_MAX );
    check( rocketnet_server_receive_buffer_bytes( INT_MAX, INT_MAX, INT_MAX ) == INT_MAX );

    // arguments below 1 count as 1

    check( rocketnet_server_receive_buffer_bytes( 0, 1000, 10 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    check( rocketnet_server_receive_buffer_bytes( -5, 1000, 10 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    check( rocketnet_server_receive_buffer_bytes( 1000, 1000, 0 ) == rocketnet_server_receive_buffer_bytes( 1000, 1000, 1 ) );
    check( rocketnet_server_receive_buffer_bytes( 1000, 1000, 1 ) == 250000000 );
    check( rocketnet_server_receive_buffer_bytes( 1000, 0, 1 ) == ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
}

REGISTER_UNIT_TEST( test_server_receive_buffer_bytes_formula );

static void test_server_transport_receive_buffer_request()
{
    // portable and msgx size for the config's max_clients

    check( rocketnet_server_transport_receive_buffer_request( "portable", 64 ) == rocketnet_server_receive_buffer_request( 64 ) );
    check( rocketnet_server_transport_receive_buffer_request( "msgx", 64 ) == rocketnet_server_receive_buffer_request( 64 ) );
    check( rocketnet_server_transport_receive_buffer_request( "portable", ROCKETNET_MAX_CLIENTS ) ==
           rocketnet_server_receive_buffer_request( ROCKETNET_MAX_CLIENTS ) );

    // gso, io_uring, iocp and rio size every socket for ROCKETNET_MAX_CLIENTS,
    // whatever max_clients the config carries: their creates do not carry it,
    // and an SO_REUSEPORT queue socket may receive any share of the clients

    const char * native[] = { "gso", "io_uring", "iocp", "rio" };
    for ( const char * transport : native )
    {
        check( rocketnet_server_transport_receive_buffer_request( transport, 1 ) == rocketnet_server_receive_buffer_request( ROCKETNET_MAX_CLIENTS ) );
        check( rocketnet_server_transport_receive_buffer_request( transport, 64 ) == rocketnet_server_receive_buffer_request( ROCKETNET_MAX_CLIENTS ) );
    }

#if ROCKETNET_MAX_CLIENTS == 1000
    // at the default ceiling: 25,000,000 bytes per socket, not the 1,000,000
    // floor (ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE)

    for ( const char * transport : native )
        check( rocketnet_server_transport_receive_buffer_request( transport, 1000 ) == 25000000 );
    check( rocketnet_server_transport_receive_buffer_request( "portable", 1000 ) == 25000000 );
    check( rocketnet_server_transport_receive_buffer_request( "gso", 1000 ) != ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
#endif // #if ROCKETNET_MAX_CLIENTS == 1000

    // no UDP socket, no request

    check( rocketnet_server_transport_receive_buffer_request( "xdp", 1000 ) == 0 );
    check( rocketnet_server_transport_receive_buffer_request( "custom", 1000 ) == 0 );
    check( rocketnet_server_transport_receive_buffer_request( "auto", 1000 ) == 0 );
    check( rocketnet_server_transport_receive_buffer_request( NULL, 1000 ) == 0 );
}

REGISTER_UNIT_TEST( test_server_transport_receive_buffer_request );

// The host memory estimate charges each socket exactly the request the
// transport's create passes, at every queue count and max_clients: no socket
// is opened, the estimate is arithmetic over the config.
static void test_server_host_estimate_charges_the_receive_request()
{
    const char * transports[] = { "portable", "msgx", "gso", "io_uring", "iocp", "rio" };
    const int client_counts[] = { 1, 64, ROCKETNET_MAX_CLIENTS };
    int estimated = 0;

    for ( const char * transport : transports )
    {
        for ( int max_clients : client_counts )
        {
            for ( int queues = 1; queues <= 4; queues++ )
            {
                struct rocketnet_server_transport_config_t config;
                rocketnet_server_transport_default_config( &config );
                config.max_clients = max_clients;
                config.num_xdp_queues = queues;

                struct rocketnet_host_memory_estimate_t estimate;
                if ( !rocketnet_server_transport_host_memory_estimate( &config, transport, &estimate ) )
                    continue;

                estimated++;

                const uint64_t request = (uint64_t) rocketnet_server_transport_receive_buffer_request( transport, max_clients );
                check( request > 0 );
                check( estimate.sockets >= 1 );
                check( estimate.socket_receive_buffer_bytes == (uint64_t) estimate.sockets * request );
                check( estimate.socket_send_buffer_bytes == (uint64_t) estimate.sockets * ROCKETNET_SOCKET_SEND_BUFFER_SIZE );
            }
        }
    }

    check( estimated > 0 );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_MAX_CLIENTS == 1000
    // gso at 4 queues and 1000 clients: four sockets of 25,000,000 bytes each

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = 1000;
    config.num_xdp_queues = 4;

    struct rocketnet_host_memory_estimate_t estimate;
    check( rocketnet_server_transport_host_memory_estimate( &config, "gso", &estimate ) );
    check( estimate.sockets == 4 );
    check( estimate.socket_receive_buffer_bytes == (uint64_t) 100000000 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_MAX_CLIENTS == 1000
}

REGISTER_UNIT_TEST( test_server_host_estimate_charges_the_receive_request );
