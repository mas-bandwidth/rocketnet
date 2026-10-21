/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The receive handoff of docs/CLIENT-IO.md _The receive path_, driven in the
// orders tla/RNClientIo.tla allows on one thread: no sockets, no real time.
// ExposedStable pins the buffer update holds against every I/O-side action
// until update's next action; NoReceiveStall pins the return at the end of
// every update, so a batch read while update holds the other buffer reaches
// the next expose; the full-buffer case pins the publish refusal and the
// readable argument of the wait, then update's return makes the publish pass.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_constants.h"
#include "rocketnet_client_receive_handshake.h"

#include <atomic>

static void handshake_init( rocketnet_client_receive_handshake_t * handshake )
{
    rocketnet_client_receive_handshake_init( handshake );
}

// RNClientIo.tla ExposedStable: once update holds an exposed buffer, no
// I/O-side action changes that buffer or its count until update returns it.
static void test_client_io_receive_exposed_stable_until_update()
{
    rocketnet_client_receive_handshake_t handshake;
    handshake_init( &handshake );

    for ( int i = 0; i < 4; i++ )
        rocketnet_unit_check( rocketnet_client_receive_handshake_io_receive( &handshake ) );
    rocketnet_unit_check( rocketnet_client_receive_handshake_io_publish( &handshake ) );

    const int exposed = rocketnet_client_receive_handshake_update_expose( &handshake );
    rocketnet_unit_check( exposed == 0 );
    rocketnet_unit_check( handshake.count[exposed] == 4 );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_receive( &handshake ) );
    rocketnet_unit_check( !rocketnet_client_receive_handshake_io_publish( &handshake ) );
    rocketnet_unit_check( handshake.state[exposed].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_BUFFER_EXPOSED );
    rocketnet_unit_check( handshake.count[exposed] == 4 );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_wait( &handshake ) );
    rocketnet_unit_check( handshake.state[exposed].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_BUFFER_EXPOSED );
    rocketnet_unit_check( handshake.count[exposed] == 4 );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_owe( &handshake ) );
    rocketnet_unit_check( handshake.state[exposed].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_BUFFER_EXPOSED );
    rocketnet_unit_check( handshake.count[exposed] == 4 );

    rocketnet_unit_check( rocketnet_client_receive_handshake_update_return( &handshake ) );
    rocketnet_unit_check( handshake.state[exposed].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_BUFFER_FREE );
    rocketnet_unit_check( handshake.count[exposed] == 0 );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_publish( &handshake ) );
    const int next = rocketnet_client_receive_handshake_update_expose( &handshake );
    rocketnet_unit_check( next == 1 );
    rocketnet_unit_check( handshake.count[next] == 1 );
    rocketnet_client_receive_handshake_update_return( &handshake );
}

// RNClientIo.tla NoReceiveStall: a batch read while update holds the other
// buffer is published by the first update that returns it, with the owed
// wake returned true, and reaches the next expose.
static void test_client_io_receive_handoff_never_stalls()
{
    rocketnet_client_receive_handshake_t handshake;
    handshake_init( &handshake );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_receive( &handshake ) );
    rocketnet_unit_check( rocketnet_client_receive_handshake_io_publish( &handshake ) );

    const int first = rocketnet_client_receive_handshake_update_expose( &handshake );
    rocketnet_unit_check( first == 0 );
    rocketnet_unit_check( handshake.count[first] == 1 );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_receive( &handshake ) );
    rocketnet_unit_check( !rocketnet_client_receive_handshake_io_publish( &handshake ) );
    rocketnet_unit_check( rocketnet_client_receive_handshake_io_owe( &handshake ) );

    rocketnet_unit_check( rocketnet_client_receive_handshake_update_expose( &handshake ) == -1 );
    rocketnet_unit_check( rocketnet_client_receive_handshake_update_return( &handshake ) );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_publish( &handshake ) );
    const int second = rocketnet_client_receive_handshake_update_expose( &handshake );
    rocketnet_unit_check( second == 1 );
    rocketnet_unit_check( handshake.count[second] == 1 );
    rocketnet_client_receive_handshake_update_return( &handshake );
    rocketnet_unit_check( rocketnet_client_receive_handshake_update_expose( &handshake ) == -1 );
}

// RNClientIo.tla FullWaitsOnWake: a full buffer with no Free buffer refuses
// the publish, sets the owed flag and waits on the wake alone; update's
// return frees the other buffer and the publish then succeeds.
static void test_client_io_receive_full_waits_for_update()
{
    rocketnet_client_receive_handshake_t handshake;
    handshake_init( &handshake );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_receive( &handshake ) );
    rocketnet_unit_check( rocketnet_client_receive_handshake_io_publish( &handshake ) );

    const int exposed = rocketnet_client_receive_handshake_update_expose( &handshake );
    rocketnet_unit_check( exposed == 0 );

    for ( int i = 0; i < ROCKETNET_NUM_CLIENT_PACKETS; i++ )
        rocketnet_unit_check( rocketnet_client_receive_handshake_io_receive( &handshake ) );
    rocketnet_unit_check( !rocketnet_client_receive_handshake_io_receive( &handshake ) );
    rocketnet_unit_check( !rocketnet_client_receive_handshake_io_publish( &handshake ) );
    rocketnet_unit_check( rocketnet_client_receive_handshake_io_owe( &handshake ) );
    rocketnet_unit_check( !rocketnet_client_receive_handshake_io_wait( &handshake ) );

    rocketnet_unit_check( rocketnet_client_receive_handshake_update_expose( &handshake ) == -1 );
    rocketnet_unit_check( rocketnet_client_receive_handshake_update_return( &handshake ) );

    rocketnet_unit_check( rocketnet_client_receive_handshake_io_publish( &handshake ) );
    const int full = rocketnet_client_receive_handshake_update_expose( &handshake );
    rocketnet_unit_check( full == 1 );
    rocketnet_unit_check( handshake.count[full] == ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_client_receive_handshake_update_return( &handshake );
}

REGISTER_UNIT_TEST( test_client_io_receive_exposed_stable_until_update );
REGISTER_UNIT_TEST( test_client_io_receive_handoff_never_stalls );
REGISTER_UNIT_TEST( test_client_io_receive_full_waits_for_update );
