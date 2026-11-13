/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// How the io_uring queue modules' counts become the public URING_* rows
// (sdk/rocketnet_uring_counters.h): every input has its own value, so a row
// fed from the wrong module count reads wrong, and two queues sum. Pure: no
// ring, no thread, no clock.

#include "test_unit_registry.h"

#include <string.h>

#include "../sdk/rocketnet_uring_counters.h"

#define check rocketnet_unit_check

static void test_uring_counters_every_row_reads_its_module_count()
{
    uint64_t receive[ROCKETNET_URING_RECEIVE_NUM_COUNTERS];
    uint64_t worker[ROCKETNET_URING_WORKER_NUM_COUNTERS];
    for ( int i = 0; i < ROCKETNET_URING_RECEIVE_NUM_COUNTERS; i++ )
    {
        receive[i] = (uint64_t) 1000 + (uint64_t) i;
    }
    for ( int i = 0; i < ROCKETNET_URING_WORKER_NUM_COUNTERS; i++ )
    {
        worker[i] = (uint64_t) 2000 + (uint64_t) i;
    }

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    memset( counters, 0, sizeof( counters ) );

    // two queues with the same counts: every row is the sum
    rocketnet_uring_counters_add_queue( counters, receive, worker, 3001, 3002, 3003 );
    rocketnet_uring_counters_add_queue( counters, receive, worker, 3001, 3002, 3003 );

    check( counters[ROCKETNET_COUNTER_URING_PACKETS_SENT] == 2 * 3001 );
    check( counters[ROCKETNET_COUNTER_URING_PACKETS_RECEIVED] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_BANKED ) );
    check( counters[ROCKETNET_COUNTER_URING_SUBMIT_SYSCALLS] == 2 * ( 2000 + ROCKETNET_URING_WORKER_COUNTER_SUBMITS ) );
    check( counters[ROCKETNET_COUNTER_URING_SEND_DROPPED] == 2 * ( 3002 + 3003 ) );
    check( counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SIZE] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE ) );
    check( counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_OVERFLOW] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_OVERFLOW ) );
    check( counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SOURCE] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE ) );
    check( counters[ROCKETNET_COUNTER_URING_RECEIVE_NO_BUFFERS] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_NO_BUFFERS ) );
    check( counters[ROCKETNET_COUNTER_URING_RECEIVE_REARMS] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_REARMS ) );
    check( counters[ROCKETNET_COUNTER_URING_REAP_DEFERRED] == 2 * ( 2000 + ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED ) );
    check( counters[ROCKETNET_COUNTER_URING_SEND_DEFERRED] == 2 * ( 2000 + ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL ) );
    check( counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_NO_BUFFER] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_NO_BUFFER ) );
    check( counters[ROCKETNET_COUNTER_URING_RECEIVE_REARM_FAILED] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_REARM_FAILED ) );
    check( counters[ROCKETNET_COUNTER_URING_GSO_SENDS] == 2 * ( 2000 + ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS ) );
    check( counters[ROCKETNET_COUNTER_URING_GSO_FALLBACKS] == 2 * ( 2000 + ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS ) );
    check( counters[ROCKETNET_COUNTER_URING_GRO_BATCHES] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_GRO_BATCHES ) );
    check( counters[ROCKETNET_COUNTER_URING_GRO_CMSG_FAULTS] == 2 * ( 1000 + ROCKETNET_URING_RECEIVE_COUNTER_GRO_CMSG_FAULTS ) );

    // no other row moved
    for ( int row = 0; row < ROCKETNET_NUM_COUNTERS; row++ )
    {
        if ( row < ROCKETNET_COUNTER_URING_PACKETS_SENT || row > ROCKETNET_COUNTER_URING_GRO_CMSG_FAULTS )
        {
            check( counters[row] == 0 );
        }
    }
}

REGISTER_UNIT_TEST( test_uring_counters_every_row_reads_its_module_count );
