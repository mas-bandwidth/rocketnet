/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS batched server transport's receive drain judges every source by
// the one receive rule (rocketnet_address_accept_received_source,
// sdk/rocketnet_address.h), native IPv4 included: a valid datagram from
// source port 0 is refused, counted in RECEIVE_FAILED and never banked; the
// same datagram from port 40000 is banked. A fake recvmsg_x feeds the
// production drain; no socket, no clock, no thread. macOS development
// builds only: the transport and its test hook exist nowhere else.

#include "test_unit_registry.h"

#include "rocketnet_server_msgx.h"

#include <stdint.h>

#if defined( __APPLE__ ) && defined( ROCKETNET_DEV ) && ROCKETNET_DEV

static void test_msgx_transport_source_port_zero_refused()
{
    int banked = -1;
    uint64_t failed = 99;
    rocketnet_unit_check( rocketnet_server_msgx_test_receive_source( 0, &banked, &failed ) );
    rocketnet_unit_check( banked == 0 );
    rocketnet_unit_check( failed == 1 );

    banked = -1;
    failed = 99;
    rocketnet_unit_check( rocketnet_server_msgx_test_receive_source( 40000, &banked, &failed ) );
    rocketnet_unit_check( banked == 1 );
    rocketnet_unit_check( failed == 0 );
}

REGISTER_UNIT_TEST( test_msgx_transport_source_port_zero_refused );

#endif // #if defined( __APPLE__ ) && defined( ROCKETNET_DEV ) && ROCKETNET_DEV
