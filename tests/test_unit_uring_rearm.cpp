/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// io_uring re-arm tests use a fake transport without sockets, a kernel ring,
// or wall clock. Request/completion shutdown is covered by uring_reap_budget.

#include <stdio.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_uring.h"

#if defined(__linux__)
#include "../sdk/rocketnet_uring_codec.h"
#endif

#if defined(__linux__) && defined(IORING_RECV_MULTISHOT) && defined(ROCKETNET_URING_REAP_TEST_HOOKS) && defined(ROCKETNET_IO_URING)
#define ROCKETNET_URING_REARM_TEST 1
#else
#define ROCKETNET_URING_REARM_TEST 0
#endif

#if ROCKETNET_URING_REARM_TEST

// red first: without the re-arm in receive_packets the queue stays unarmed
// after its multishot ends on a failed arm, and every later receive finds it
// still unarmed. With the fix each receive retries and each failure counts.

static void test_io_uring_transport_rearm_retries_a_failed_arm()
{
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
}

REGISTER_UNIT_TEST( test_io_uring_transport_rearm_retries_a_failed_arm );

#endif // #if ROCKETNET_URING_REARM_TEST
