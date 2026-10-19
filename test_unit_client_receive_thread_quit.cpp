/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A destroyed client's receive thread always exits. Destroy stores quit and
// wakes the receive thread's wait explicitly before the join, so the join
// does not depend on the wait's timeout firing. This unit case checks the
// wake's logic only: a wake that lands before the wait makes the wait, and
// every later wait, return at once. It has no sleep and no timed wait that
// runs to its timeout. The wake landing during a wait, and 1000 create and
// destroy cycles of a client transport, are in the functional suite
// (client_receive_thread_quit_*).

#include "test_unit_registry.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#include <atomic>
#include <stdio.h>

#define check rocketnet_unit_check

struct client_receive_thread_quit_waiter_t
{
    std::atomic<int> go;
    std::atomic<int> waits_done;
};

static void client_receive_thread_quit_wait_after_wake( void * data )
{
    client_receive_thread_quit_waiter_t * waiter = (client_receive_thread_quit_waiter_t*) data;
    // the test wakes this thread before it sets go, so both waits start after the wake
    while ( !waiter->go.load( std::memory_order_acquire ) ) {}
    // an hour-long wait: only the wake ends it within the test
    rocketnet_platform_thread_wait( 3600.0 );
    // once woken, every later wait returns at once
    rocketnet_platform_thread_wait( 3600.0 );
    waiter->waits_done.store( 2, std::memory_order_release );
}

// a wake stored before the wait starts ends the wait and every later one. A
// wake that is lost does not fail a check here: the case hangs, and the
// suite's time limit reports it.
static void client_receive_thread_quit_wake_before_wait_returns()
{
    for ( int i = 0; i < 4; i++ )
    {
        client_receive_thread_quit_waiter_t waiter;
        waiter.go.store( 0 );
        waiter.waits_done.store( 0 );
        rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, client_receive_thread_quit_wait_after_wake, &waiter );
        check( thread );
        rocketnet_platform_thread_wake( thread );
        waiter.go.store( 1, std::memory_order_release );
        rocketnet_platform_thread_join( thread );
        rocketnet_platform_thread_destroy( thread );
        check( waiter.waits_done.load( std::memory_order_acquire ) == 2 );
    }
    printf( "client_receive_thread_quit_wake_before_wait_returns\n" );
}

REGISTER_UNIT_TEST( client_receive_thread_quit_wake_before_wait_returns );
