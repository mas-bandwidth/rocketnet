/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP receive handoff (rocketnet_server_socket_linux.cpp,
// proofs/tla/RNXdpReceiveRing.tla): only main swaps receive banks. It claims
// the receive mutex without blocking: one try and, when the worker holds the
// mutex, a flag and a bounded number of further tries, during which a running
// worker lets go between two descriptors. A claim whose every try missed
// takes nothing in that call and is retried on the next, whose view then
// holds every packet staged before it. The fixtures drive the real receive
// call with fake staging buffers and stand in for the worker themselves: no
// socket, kernel object or thread is involved.

#include "test_unit_registry.h"

// the AF_XDP code is Linux only and its test hooks are development-only, so
// these cases are built only where they are
#if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV

// the fixtures are in rocketnet_server_socket_linux.cpp, beside the existing
// native staging cases (test_xdp calls those without kernel resources too)
bool rocketnet_server_socket_test_receive_handoff();
bool rocketnet_server_socket_test_receive_claim();

static void test_xdp_transport_receive_missed_swap_is_retried_next_call()
{
    rocketnet_unit_check( rocketnet_server_socket_test_receive_handoff() );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_missed_swap_is_retried_next_call );

static void test_xdp_transport_receive_claim_swaps_in_the_call_once_the_worker_lets_go()
{
    rocketnet_unit_check( rocketnet_server_socket_test_receive_claim() );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_claim_swaps_in_the_call_once_the_worker_lets_go );

#endif // #if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV
