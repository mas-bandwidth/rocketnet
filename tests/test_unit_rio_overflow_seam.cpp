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
bool rocketnet_server_rio_test_reservation_wrap();
bool rocketnet_server_rio_test_stop_drains_sends();
bool rocketnet_server_rio_test_send_accounting();
bool rocketnet_server_rio_test_create_failure_drain( int late_waits, bool refuse_close, bool * stopped_out, int * closes_out,
                                                     int * waits_out, int * longest_wait_milliseconds_out );

static void test_rio_transport_overflow_seam()
{
    uint64_t counters[2 * ROCKETNET_NUM_COUNTERS] = {};
    int packet_counts[2] = { 0, 0 };
    int repost_calls = 0;
    int pending_receives = 0;
    int guard_hit = 0;
    check( rocketnet_server_rio_test_receive_overflow( counters, packet_counts, &repost_calls, &pending_receives, &guard_hit ) );
    check( guard_hit == 0 );

    check( packet_counts[0] == rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    check( counters[ROCKETNET_COUNTER_RIO_PACKETS_RECEIVED] == (uint64_t) rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    check( counters[ROCKETNET_COUNTER_RIO_RECEIVE_DROPPED_OVERFLOW] == 0 );
    check( counters[ROCKETNET_COUNTER_RIO_RECEIVE_REPOST_FAILED] == 0 );
    check( pending_receives == rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );

    // The queued tail is delivered by the next update. The seam contains two
    // full banks, so both calls stay within the per-call receive bound.

    check( packet_counts[1] == rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    check( counters[ROCKETNET_NUM_COUNTERS + ROCKETNET_COUNTER_RIO_PACKETS_RECEIVED] ==
           2 * (uint64_t) rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    check( repost_calls == 2 * rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; ++i )
    {
        if ( i == ROCKETNET_COUNTER_RIO_PACKETS_RECEIVED )
        {
            continue;
        }
        check( counters[i] == counters[ROCKETNET_NUM_COUNTERS + i] );
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

// A fill phase past 2^32 reservations without a flush refuses the next slot as
// full, and never hands out a slot it handed out before.
static void test_rio_transport_reservation_does_not_wrap()
{
    check( rocketnet_server_rio_test_reservation_wrap() );
}

REGISTER_UNIT_TEST( test_rio_transport_reservation_does_not_wrap );
// Stop keeps the socket open while committed sends are in flight
// (proofs/tla/RNRioWorker.tla WClose), so closesocket cancels none of the stop
// burst, and it drains all 4,096 cancelled receives in that pass rather than
// one 256 completion batch per 1 ms wait: a fake kernel, no socket, no worker
// thread, no clock.
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

// A create that fails with every receive slice posted drains inline
// (proofs/tla/RNRioWorker.tla InlineStop, BoundedStopDrain): a fake kernel, no
// socket, no worker thread, and the wait between two passes is told its bound
// instead of waiting. The drain is bounded by counting passes and completions,
// never by seconds: it waits only for cancelled receives still on their way
// to the completion queue, each wait bounded by 50 ms on the queue's
// notification, and gives up after ten failed passes
// (ROCKETNET_RIO_DRAIN_WAITS).
static void test_rio_transport_create_failure_drain_counts_passes_not_seconds()
{
    bool stopped = false;
    int closes = 0;
    int waits = 0;
    int longest = 0;

    // closesocket queues every cancelled receive: one pass, no wait

    check( rocketnet_server_rio_test_create_failure_drain( 0, false, &stopped, &closes, &waits, &longest ) );
    check( stopped && closes == 1 && waits == 0 );

    // the cancelled receives reach the queue over two waits

    check( rocketnet_server_rio_test_create_failure_drain( 2, false, &stopped, &closes, &waits, &longest ) );
    check( stopped && closes == 1 && waits == 2 );
    check( longest <= 50 );

    // closesocket refused every time: nothing is on its way, so nothing is
    // waited for, and the eleventh failed pass is the last

    check( rocketnet_server_rio_test_create_failure_drain( 0, true, &stopped, &closes, &waits, &longest ) );
    check( !stopped && closes == 11 );
    check( waits == 0 );

    // cancelled receives that never arrive: ten bounded waits, then it gives up

    check( rocketnet_server_rio_test_create_failure_drain( 1 << 30, false, &stopped, &closes, &waits, &longest ) );
    check( !stopped && closes == 1 && waits == 10 );
    check( longest <= 50 );
}

REGISTER_UNIT_TEST( test_rio_transport_create_failure_drain_counts_passes_not_seconds );

// Only a pass that made no progress counts against the drain's bound. The
// cancelled receives here reach the queue over twenty-five waits, more than
// ROCKETNET_RIO_DRAIN_WAITS, and every wait brings some: the drain accounts
// for all of them and stops, where a bound on every failed pass gives up
// after ten and leaks a region that was draining.
static void test_rio_transport_create_failure_drain_counts_only_stalled_passes()
{
    bool stopped = false;
    int closes = 0;
    int waits = 0;
    int longest = 0;

    check( rocketnet_server_rio_test_create_failure_drain( 25, false, &stopped, &closes, &waits, &longest ) );
    check( stopped );
    check( closes == 1 && waits == 25 );
    check( longest <= 50 );
}

REGISTER_UNIT_TEST( test_rio_transport_create_failure_drain_counts_only_stalled_passes );

#endif // #if defined( _WIN32 )
