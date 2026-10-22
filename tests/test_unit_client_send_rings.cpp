/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The two send rings' state machine (sdk/rocketnet_client_send_rings.h)
// against the RNClientIo HandOff / Reopen / IoSend / IoFinishRing actions.
// Single-threaded: no sockets, no real time.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_send_rings.h"

static void test_client_io_handoff_sends_ring_once()
{
    rocketnet_client_send_rings_t r;
    rocketnet_client_send_rings_init( &r );

    rocketnet_unit_check( rocketnet_client_send_rings_hand_off( &r, 3 ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( r.fill == 1 );
    rocketnet_unit_check( r.state[0].load() == ROCKETNET_CLIENT_RING_HANDED_OFF );
    rocketnet_unit_check( r.committed[0] == 3 );

    rocketnet_unit_check( rocketnet_client_send_rings_take( &r ) == 0 );
    rocketnet_unit_check( r.state[0].load() == ROCKETNET_CLIENT_RING_SENDING );
    rocketnet_unit_check( rocketnet_client_send_rings_take( &r ) == -1 );
    rocketnet_unit_check( rocketnet_client_send_rings_in_flight( &r ) );

    rocketnet_client_send_rings_finish( &r, 0 );
    rocketnet_unit_check( r.state[0].load() == ROCKETNET_CLIENT_RING_SENT );
    rocketnet_unit_check( !rocketnet_client_send_rings_in_flight( &r ) );
}
REGISTER_UNIT_TEST( test_client_io_handoff_sends_ring_once );

static void test_client_io_ring_not_reused_before_sent()
{
    rocketnet_client_send_rings_t r;
    rocketnet_client_send_rings_init( &r );

    rocketnet_unit_check( rocketnet_client_send_rings_hand_off( &r, 5 ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_send_rings_take( &r ) == 0 );
    rocketnet_unit_check( r.state[0].load() == ROCKETNET_CLIENT_RING_SENDING );

    rocketnet_unit_check( rocketnet_client_send_rings_hand_off( &r, 7 ) == ROCKETNET_CLIENT_RINGS_LATE );
    rocketnet_unit_check( r.fill == 1 );
    rocketnet_unit_check( !rocketnet_client_send_rings_reopen( &r, 0 ) );
    rocketnet_unit_check( r.state[0].load() == ROCKETNET_CLIENT_RING_SENDING );
    rocketnet_unit_check( r.committed[0] == 5 );

    rocketnet_client_send_rings_finish( &r, 0 );
    rocketnet_unit_check( rocketnet_client_send_rings_hand_off( &r, 9 ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( r.state[0].load() == ROCKETNET_CLIENT_RING_FILLING );
    rocketnet_unit_check( r.committed[0] == 0 );
    rocketnet_unit_check( r.state[1].load() == ROCKETNET_CLIENT_RING_HANDED_OFF );
    rocketnet_unit_check( r.committed[1] == 9 );
}
REGISTER_UNIT_TEST( test_client_io_ring_not_reused_before_sent );

static void test_client_io_handoff_skips_empty_ring()
{
    rocketnet_client_send_rings_t r;
    rocketnet_client_send_rings_init( &r );

    rocketnet_unit_check( rocketnet_client_send_rings_hand_off( &r, 0 ) == ROCKETNET_CLIENT_RINGS_EMPTY );
    rocketnet_unit_check( r.fill == 0 );
    rocketnet_unit_check( r.state[0].load() == ROCKETNET_CLIENT_RING_FILLING );
    rocketnet_unit_check( r.state[1].load() == ROCKETNET_CLIENT_RING_FILLING );
    rocketnet_unit_check( r.committed[0] == 0 );
    rocketnet_unit_check( r.committed[1] == 0 );
    rocketnet_unit_check( rocketnet_client_send_rings_take( &r ) == -1 );
    rocketnet_unit_check( !rocketnet_client_send_rings_in_flight( &r ) );
}
REGISTER_UNIT_TEST( test_client_io_handoff_skips_empty_ring );
