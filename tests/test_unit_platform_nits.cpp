/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The platform-layer nits a unit case shows with no socket, no thread and no
// clock: the non-blocking flag merge the POSIX socket create paths pass to
// fcntl F_SETFL (rocketnet_platform_nonblocking_flags in
// sdk/rocketnet_platform.cpp, called by both POSIX socket create paths), which
// keeps the descriptor's other flags; and the platform mutex, which is not
// recursive on any platform.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#include <fcntl.h>

int rocketnet_platform_nonblocking_flags( int flags );

// F_SETFL replaces the status flags, so the merge keeps every flag the
// descriptor had and adds O_NONBLOCK.
static void test_platform_nonblocking_flags_keep_existing_flags()
{
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( O_APPEND ) == ( O_APPEND | O_NONBLOCK ) );
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( 0 ) == O_NONBLOCK );
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( O_NONBLOCK ) == O_NONBLOCK );
}

REGISTER_UNIT_TEST( test_platform_nonblocking_flags_keep_existing_flags );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_DEV

// The mutex is not recursive on any platform (PTHREAD_MUTEX_NORMAL on POSIX,
// an SRWLOCK on Windows), so the thread that holds it cannot take it again.
static void test_platform_mutex_try_acquire_fails_for_the_owner()
{
    struct rocketnet_platform_mutex_t mutex;
    rocketnet_unit_check( rocketnet_platform_mutex_create( &mutex ) );

    rocketnet_unit_check( rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_unit_check( !rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_platform_mutex_release( &mutex );

    rocketnet_platform_mutex_acquire( &mutex );
    rocketnet_unit_check( !rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_platform_mutex_release( &mutex );

    rocketnet_unit_check( rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_platform_mutex_release( &mutex );

    rocketnet_platform_mutex_destroy( &mutex );
}

REGISTER_UNIT_TEST( test_platform_mutex_try_acquire_fails_for_the_owner );

#endif // #if ROCKETNET_DEV
