/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "sdk/rocketnet_server_rio.h"

#include <stdint.h>
#include <stdio.h>

#define check rocketnet_unit_check

#if defined(_WIN32)

// Implemented only by the isolated test-instrumented RIO driver copy.
bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                  int * repost_calls, int * pending_receives, int * guard_hit );

static void rio_overflow_seam()
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
    check( repost_calls == ROCKETNET_RIO_RECEIVES );
    check( pending_receives == ROCKETNET_RIO_RECEIVES );

    // The queued tail is delivered by the next update. The seam contains two
    // full banks, so both calls stay within the per-call receive bound.
    check( packet_counts[1] == ROCKETNET_RIO_RECEIVES );
    check( counters[ROCKETNET_RIO_NUM_COUNTERS + ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] ==
           2 * ROCKETNET_RIO_RECEIVES );
    check( repost_calls == 2 * ROCKETNET_RIO_RECEIVES );
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i )
        check( counters[i] <= counters[ROCKETNET_RIO_NUM_COUNTERS + i] );
}

#else

static void rio_overflow_seam()
{
    printf( "rio_overflow_seam [SKIPPED the RIO completion seam requires Windows]\n" );
}

#endif

REGISTER_UNIT_TEST( rio_overflow_seam );
