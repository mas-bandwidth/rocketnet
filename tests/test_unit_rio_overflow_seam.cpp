/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_rio.h"

#include <stdint.h>
#include <stdio.h>

#define check rocketnet_unit_check

#if defined( _WIN32 )

// Implemented only by the isolated test-instrumented RIO transport copy.
bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                 int * repost_calls, int * pending_receives, int * guard_hit );
bool rocketnet_server_rio_test_worker_handoff();
bool rocketnet_server_rio_test_stop_drains_sends();
bool rocketnet_server_rio_test_send_accounting();

static void test_rio_transport_overflow_seam()
{
    uint64_t counters[2 * ROCKETNET_RIO_NUM_COUNTERS] = {};
    int packet_counts[2] = { 0, 0 };
    int repost_calls = 0;
    int pending_receives = 0;
    int guard_hit = 0;
    check( rocketnet_server_rio_test_receive_overflow( counters, packet_counts, &repost_calls, &pending_receives, &guard_hit ) );
    check( guard_hit == 0 );

    check( packet_counts[0] == ROCKETNET_RIO_RECEIVES );
    check( counters[ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] == ROCKETNET_RIO_RECEIVES );
    check( counters[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW] == 0 );
    check( counters[ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED] == 0 );
    check( pending_receives == ROCKETNET_RIO_RECEIVES );

    // The queued tail is delivered by the next update. The seam contains two
    // full banks, so both calls stay within the per-call receive bound.

    check( packet_counts[1] == ROCKETNET_RIO_RECEIVES );
    check( counters[ROCKETNET_RIO_NUM_COUNTERS + ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] ==
           2 * ROCKETNET_RIO_RECEIVES );
    check( repost_calls == 2 * ROCKETNET_RIO_RECEIVES );

    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i )
    {
        if ( i == ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED )
            continue;
        check( counters[i] == counters[ROCKETNET_RIO_NUM_COUNTERS + i] );
    }
}

REGISTER_UNIT_TEST( test_rio_transport_overflow_seam );

// The I/O worker's handoffs (proofs/tla/RNRioWorker.tla), stepped on this thread
// with a fake kernel: no socket, no worker thread, no clock.
static void test_rio_transport_worker_handoff()
{
    check( rocketnet_server_rio_test_worker_handoff() );
}

REGISTER_UNIT_TEST( test_rio_transport_worker_handoff );

// Stop keeps the socket open while committed sends are in flight
// (proofs/tla/RNRioWorker.tla WClose), so closesocket cancels none of the stop
// burst: a fake kernel, no socket, no worker thread, no clock.
static void test_rio_transport_stop_drains_sends()
{
    check( rocketnet_server_rio_test_stop_drains_sends() );
}

REGISTER_UNIT_TEST( test_rio_transport_stop_drains_sends );

// Each lost send counts SEND_FAILED once, each sent one PACKETS_SENT once: a
// finish out of range counts its loss, and a refused commit counts nothing
// for the deferred sends that later complete.
static void test_rio_transport_send_accounting()
{
    check( rocketnet_server_rio_test_send_accounting() );
}

REGISTER_UNIT_TEST( test_rio_transport_send_accounting );

#endif // #if defined( _WIN32 )
