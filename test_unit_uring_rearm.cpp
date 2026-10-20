/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring re-arm and destroy-bound cases. Both run through the driver's
// development-only seam: a fake driver with queues but no kernel ring, whose
// arm calls can be told to find no submission entry, and destroy's wait loop
// driven by a fake completion source. No socket, no ring, no wall clock.

#include <stdio.h>

#include "test_unit_registry.h"

#include "sdk/rocketnet_server_uring.h"

#if defined(__linux__) && defined(ROCKETNET_IO_URING) && ROCKETNET_IO_URING && defined(ROCKETNET_URING_TEST_HOOKS)
#define ROCKETNET_URING_REARM_TEST 1
#else
#define ROCKETNET_URING_REARM_TEST 0
#endif

#if ROCKETNET_URING_REARM_TEST
struct rocketnet_server_uring_t;
struct rocketnet_server_uring_t * rocketnet_server_uring_test_create_fake( int num_queues );
void rocketnet_server_uring_test_destroy_fake( struct rocketnet_server_uring_t * uring );
void rocketnet_server_uring_test_set_arm_failures( struct rocketnet_server_uring_t * uring, int count );
void rocketnet_server_uring_test_multishot_ends( struct rocketnet_server_uring_t * uring, int queue_index, bool arm_finds_no_sqe );
bool rocketnet_server_uring_test_queue_armed( struct rocketnet_server_uring_t * uring, int queue_index );
uint64_t rocketnet_server_uring_test_counter( struct rocketnet_server_uring_t * uring, int counter );
int rocketnet_server_uring_test_destroy_wait_rounds( bool always_completes, int guard );
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

// red first: with a fake completion source that always yields a CQE every
// round is non-silent, so destroy's empty-round count never moves and the wait
// never ends. With the fix a total round bound ends it; the case's own guard
// stops the red run instead of hanging it.

static void uring_destroy_wait_is_bounded()
{
#if ROCKETNET_URING_REARM_TEST
    const int completions_rounds = rocketnet_server_uring_test_destroy_wait_rounds( true, 1000 );
    rocketnet_unit_check( completions_rounds == 20 );

    // with no completions the older silent-round bound still ends the wait
    const int silent_rounds = rocketnet_server_uring_test_destroy_wait_rounds( false, 1000 );
    rocketnet_unit_check( silent_rounds == 5 );
#else
    printf( "uring_destroy_wait_is_bounded [SKIPPED the io_uring driver and its test seam need Linux with liburing]\n" );
#endif
}

REGISTER_UNIT_TEST( uring_rearm_retries_a_failed_arm );
REGISTER_UNIT_TEST( uring_destroy_wait_is_bounded );
