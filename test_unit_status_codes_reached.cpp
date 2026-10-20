/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The status codes no committed unit case reached before this file: the
// platform-resource refusals PLATFORM_INIT_FAILED and TIMER_CREATE_FAILED.
// Each is reached through the platform call-table seam
// (rocketnet_platform_priority_calls_t) with no real platform failure: a
// refused platform_start fails rocketnet_init, and a refused waitable-timer
// create fails frame sync (Windows; named skip elsewhere). MUTEX_CREATE_FAILED
// is reachable through the same seam but its server-transport case corrupts
// the loopback driver's shared static mutex under the fault table and is not
// landed here.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_frame_sync.h"
#include "rocketnet_platform.h"

#include <string.h>

#define check rocketnet_unit_check

static int refuse_platform_start( void )
{
    return 1;
}

static void* refuse_waitable_timer( unsigned flags )
{
    (void) flags;
    return NULL;
}

// a refused platform start fails rocketnet_init with PLATFORM_INIT_FAILED
static void platform_init_failed_reached()
{
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    struct rocketnet_platform_priority_calls_t fake;
    fake = *rocketnet_platform_priority_calls_real();
    fake.platform_start = refuse_platform_start;
    rocketnet_platform_set_priority_calls_for_testing( &fake );

    int status = -1;
    check( !rocketnet_init_ex( NULL, &status ) );
    check( status == ROCKETNET_STATUS_PLATFORM_INIT_FAILED );

    rocketnet_platform_set_priority_calls_for_testing( NULL );
}
REGISTER_UNIT_TEST( platform_init_failed_reached );

// a refused waitable-timer create fails frame sync with TIMER_CREATE_FAILED;
// the timer is Windows-only, so elsewhere the case is a named skip
static void timer_create_failed_reached()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    struct rocketnet_platform_priority_calls_t fake;
    fake = *rocketnet_platform_priority_calls_real();
    fake.create_waitable_timer = refuse_waitable_timer;
    rocketnet_platform_set_priority_calls_for_testing( &fake );

    int status = -1;
    check( rocketnet_frame_sync_create_ex( NULL, 60.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_TIMER_CREATE_FAILED );

    rocketnet_platform_set_priority_calls_for_testing( NULL );
#else
    printf( "timer_create_failed_reached [SKIPPED off windows: the waitable-timer create is windows-only]\n" );
#endif
}
REGISTER_UNIT_TEST( timer_create_failed_reached );
