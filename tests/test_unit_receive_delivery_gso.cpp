/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "suite_runner.h"

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_server_gso.h"

// The gso server's receive delivery (sdk/rocketnet_receive_delivery_check.h,
// proofs/tla/RNReceiveHandoff.tla DeliveredByNextCall): every receive call
// returns every datagram a worker pass banked before the call began, so no
// client input waits an extra server update. The worker's passes and main's
// calls are stepped on this thread in a fixed order on the one-queue fixture:
// no socket, no worker thread, no clock.

#if defined( __linux__ ) && ROCKETNET_DEV

static void test_receive_delivery_gso()
{
    rocketnet_unit_check( rocketnet_server_gso_test_receive_delivery() );
}

REGISTER_UNIT_TEST( test_receive_delivery_gso );

#endif // #if defined( __linux__ ) && ROCKETNET_DEV
