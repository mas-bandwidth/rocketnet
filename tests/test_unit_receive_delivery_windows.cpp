/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "../sdk/rocketnet_internal.h"

#include <stdio.h>

// The Windows server transports' receive delivery
// (sdk/rocketnet_receive_delivery_check.h, proofs/tla/RNReceiveHandoff.tla
// DeliveredByNextCall): every receive call returns every datagram a worker
// banked before the call began, so no client input waits an extra server
// update. The workers' banking and main's calls are stepped on this thread in
// a fixed order with a fake kernel: no socket, no worker thread, no clock.

#if defined( _WIN32 ) && ROCKETNET_DEV

bool rocketnet_server_rio_test_receive_delivery();
bool rocketnet_server_iocp_test_receive_delivery( int workers );

static void test_receive_delivery_rio()
{
    rocketnet_unit_check( rocketnet_server_rio_test_receive_delivery() );
}

REGISTER_UNIT_TEST( test_receive_delivery_rio );

// one completion worker, each pass of the script one completion batch
static void test_receive_delivery_iocp()
{
    rocketnet_unit_check( rocketnet_server_iocp_test_receive_delivery( 1 ) );
}

REGISTER_UNIT_TEST( test_receive_delivery_iocp );

// two completion workers, the script's passes dealt to them in turn: main's
// call returns what every worker banked before it
static void test_receive_delivery_iocp_two_workers()
{
    rocketnet_unit_check( rocketnet_server_iocp_test_receive_delivery( 2 ) );
}

REGISTER_UNIT_TEST( test_receive_delivery_iocp_two_workers );

bool rocketnet_server_iocp_test_receive_rotation( int frames, int * longest_wait, uint64_t * dropped );

// Main's walk over the completion workers starts one worker later each call:
// worker 0 fills the process view every frame, and worker 1's datagram still
// waits at most one call, never for good.
static void test_receive_rotation_iocp()
{
    int longest_wait = 0;
    uint64_t dropped = 0;
    rocketnet_unit_check( rocketnet_server_iocp_test_receive_rotation( 12, &longest_wait, &dropped ) );
    if ( longest_wait > 1 )
    {
        printf( "iocp rotation: worker 1 waited %d calls in a row\n", longest_wait );
    }
    rocketnet_unit_check( longest_wait <= 1 );
}

REGISTER_UNIT_TEST( test_receive_rotation_iocp );

#endif // #if defined( _WIN32 ) && ROCKETNET_DEV
