/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP receive handoff's swap-on-request rule
// (rocketnet_server_socket_linux.cpp): main's non-blocking try-acquire can miss
// the receive mutex while the worker is in a heavy pass. Main raises a request
// and returns; the worker completes the bank swap at its pass boundary, so the
// packets reach main's next view without waiting another update. The fixture
// also forces service between main's exhaustion check and bank clear, and
// verifies that a partly consumed bank stays with main. It drives the real
// receive call with fake staging buffers: no socket, kernel object, thread or
// clock is involved.

#include "test_unit_registry.h"

// the AF_XDP code is Linux only and its test hooks are development-only, so
// this case is built only where they are
#if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV

// the fixture is in rocketnet_server_socket_linux.cpp, beside the existing
// native staging cases (test_xdp calls those without kernel resources too)
bool rocketnet_server_socket_test_receive_swap_on_request();

static void test_xdp_transport_receive_swap_on_request_hands_over_packets()
{
    rocketnet_unit_check( rocketnet_server_socket_test_receive_swap_on_request() );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_swap_on_request_hands_over_packets );

#endif // #if defined( __linux__ ) && ROCKETNET_XDP && ROCKETNET_DEV
