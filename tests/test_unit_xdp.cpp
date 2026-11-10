/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP receive handoff (rocketnet_server_socket_linux.cpp,
// proofs/tla/RNXdpReceiveRing.tla): only main swaps receive banks, with a
// non-blocking try-acquire. A try that misses the worker's hold takes nothing
// in that call and is retried on the next, whose view then holds every packet
// staged before it. The fixture drives the real receive call with fake
// staging buffers and holds the receive mutex itself where the worker would:
// no socket, kernel object or thread is involved.

#include "test_unit_registry.h"

// the AF_XDP code is Linux only and its test hooks are development-only, so
// this case is built only where they are
#if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV

// the fixture is in rocketnet_server_socket_linux.cpp, beside the existing
// native staging cases (test_xdp calls those without kernel resources too)
bool rocketnet_server_socket_test_receive_handoff();

static void test_xdp_transport_receive_missed_swap_is_retried_next_call()
{
    rocketnet_unit_check( rocketnet_server_socket_test_receive_handoff() );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_missed_swap_is_retried_next_call );

#endif // #if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV
