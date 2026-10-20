/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring re-arm and destroy-bound cases. Both run through the driver's
// development-only seam: a fake driver with queues but no kernel ring, whose
// arm calls can be told to find no submission entry, and the end condition
// of destroy's wait loop (rocketnet_server_uring_destroy_wait_over) run with
// every round completing or every round silent. No socket, no ring, no wall
// clock.

#include <stdio.h>

#include "test_unit_registry.h"

#include "sdk/rocketnet_server_uring.h"

#if defined(__linux__)
#include "sdk/rocketnet_uring_codec.h"
#endif

#if defined(__linux__) && defined(IORING_RECV_MULTISHOT) && defined(ROCKETNET_URING_REAP_TEST_HOOKS)
#define ROCKETNET_URING_REARM_TEST 1
#else
#define ROCKETNET_URING_REARM_TEST 0
#endif

// red first: without the re-arm in receive_packets the queue stays unarmed
// after its multishot ends on a failed arm, and every later receive finds it
// still unarmed. With the fix each receive retries and each failure counts.

static void uring_rearm_retries_a_failed_arm()
{
#if ROCKETNET_URING_REARM_TEST
    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create_fake( 1 );
    rocketnet_unit_check( uring != NULL );
    if ( !uring )
        return;

    // the multishot ends and the one-shot re-arm in the CQE handler finds no
    // submission entry: the queue is left unarmed and the failure is counted
    rocketnet_server_uring_test_multishot_ends( uring, 0, true );
    rocketnet_unit_check( !rocketnet_server_uring_test_queue_armed( uring, 0 ) );
    rocketnet_unit_check( rocketnet_server_uring_test_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARM_FAILED ) == 1 );

    // receives that also find no entry retry every call and stay counted
    rocketnet_server_uring_test_set_arm_failures( uring, 2 );
    rocketnet_server_uring_receive_packets( uring );
    rocketnet_server_uring_receive_packets( uring );
    rocketnet_unit_check( !rocketnet_server_uring_test_queue_armed( uring, 0 ) );
    rocketnet_unit_check( rocketnet_server_uring_test_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARM_FAILED ) == 3 );
    rocketnet_unit_check( rocketnet_server_uring_test_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS ) == 0 );

    // the next receive finds an entry and arms the queue again
    rocketnet_server_uring_receive_packets( uring );
    rocketnet_unit_check( rocketnet_server_uring_test_queue_armed( uring, 0 ) );
    rocketnet_unit_check( rocketnet_server_uring_test_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS ) == 1 );

    rocketnet_server_uring_test_destroy_fake( uring );
#else
    printf( "uring_rearm_retries_a_failed_arm [SKIPPED the io_uring driver and its test seam need Linux with liburing]\n" );
#endif
}

// red first: when every round yields a CQE no round is silent, so destroy's
// empty-round count never moves and the wait would never end without the total
// round bound. The case runs rocketnet_server_uring_destroy_wait_over, the end
// condition rocketnet_server_uring_destroy's wait loop uses; the case's own
// guard stops a wait that does not end.

static void uring_destroy_wait_is_bounded()
{
#if ROCKETNET_URING_REARM_TEST
    const int completions_rounds = rocketnet_server_uring_test_destroy_wait_rounds( true, 1000 );
    rocketnet_unit_check( completions_rounds == 64 );

    // with no completions the older silent-round bound still ends the wait
    const int silent_rounds = rocketnet_server_uring_test_destroy_wait_rounds( false, 1000 );
    rocketnet_unit_check( silent_rounds == 5 );
#else
    printf( "uring_destroy_wait_is_bounded [SKIPPED the io_uring driver and its test seam need Linux with liburing]\n" );
#endif
}

REGISTER_UNIT_TEST( uring_rearm_retries_a_failed_arm );
REGISTER_UNIT_TEST( uring_destroy_wait_is_bounded );
