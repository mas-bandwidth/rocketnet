/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring transport's send reservation (rocketnet_server_uring_start_packet)
// on a fake transport without sockets, a kernel ring or a clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_uring.h"

#if defined( __linux__ )
#include "../sdk/rocketnet_uring_codec.h"
#endif // #if defined( __linux__ )

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )

#include <string.h>

// A caller that has taken 2^32 - 1 reservations without a flush is refused
// the next two: the reservation count does not wrap back onto a slot it
// handed out.

static void test_io_uring_transport_reservation_does_not_wrap()
{
    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create_fake( 1 );
    rocketnet_unit_check( uring != NULL );
    if ( !uring )
        return;

    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40001;

    rocketnet_server_uring_test_set_reserved( uring, 0xFFFFFFFFull );

    uint64_t packet_id = 0;
    rocketnet_unit_check( rocketnet_server_uring_start_packet( uring, &to, &packet_id ) == NULL );
    rocketnet_unit_check( rocketnet_server_uring_start_packet( uring, &to, &packet_id ) == NULL );

    rocketnet_server_uring_test_destroy_fake( uring );
}

REGISTER_UNIT_TEST( test_io_uring_transport_reservation_does_not_wrap );

#endif // #if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )
