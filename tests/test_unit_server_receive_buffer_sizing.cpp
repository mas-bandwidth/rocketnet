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
