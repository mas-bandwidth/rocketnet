/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Platform-layer nits with a case that needs no socket and no thread:
//   the non-blocking flag merge the POSIX socket create paths pass to
//     fcntl F_SETFL (rocketnet_platform_nonblocking_flags,
//     sdk/rocketnet_platform.cpp), which keeps the descriptor's other flags;
//   the frame-sync clock (rocketnet_frame_sync_now_ns,
//     sdk/rocketnet_frame_sync.cpp) read before any frame sync is created,
//     which on macOS reads the timebase itself instead of dividing by an
//     unread zero denominator.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_frame_sync.h"

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <fcntl.h>
int rocketnet_platform_nonblocking_flags( int flags );
#endif

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// F_SETFL replaces the status flags, so the merge keeps every flag the
// descriptor had and adds O_NONBLOCK.
static void platform_nonblocking_flags_keep_existing_flags()
{
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( O_APPEND ) == ( O_APPEND | O_NONBLOCK ) );
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( 0 ) == O_NONBLOCK );
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( O_NONBLOCK ) == O_NONBLOCK );
}

REGISTER_UNIT_TEST( platform_nonblocking_flags_keep_existing_flags );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// Each unit test runs in its own worker process, so no frame sync has been
// created here: the first clock read is the before-init read. A clock that
// divided by a zero timebase reads 0 (or traps) instead of a time.
static void frame_sync_now_before_any_create_reads_the_clock()
{
    const uint64_t first = rocketnet_frame_sync_now_ns();
    rocketnet_unit_check( first > 0 );
    const uint64_t second = rocketnet_frame_sync_now_ns();
    rocketnet_unit_check( second >= first );
}

REGISTER_UNIT_TEST( frame_sync_now_before_any_create_reads_the_clock );
