/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "suite_runner.h"

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_server_socket.h"

// The xdp server's receive delivery (sdk/rocketnet_receive_delivery_check.h,
// proofs/tla/RNReceiveHandoff.tla DeliveredByNextCall): every receive call
// returns every datagram a worker pass staged before the call began, so no
// client input waits an extra server update. The worker's passes and main's
// calls are stepped on this thread in a fixed order on a one-queue fixture
// with no kernel resources: no socket, no worker thread, no clock.

#if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV

static void test_receive_delivery_xdp()
{
    rocketnet_unit_check( rocketnet_server_socket_test_receive_delivery() );
}

REGISTER_UNIT_TEST( test_receive_delivery_xdp );

#endif // #if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV
