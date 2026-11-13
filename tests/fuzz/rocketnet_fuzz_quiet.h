/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every fuzz harness silences the library's log: hostile input makes the
// receive paths log on nearly every call, and that output would swamp both
// the fuzzer and the replay logs. The library's default assert handler
// reports through that same log, so with the log silenced a failed assert
// left an empty log and only the trap's exit status (VERIFY-5 W10). This
// keeps the log silenced and installs an assert handler that writes the
// failed condition to stderr whatever the log level, then traps as the
// default handler does, so libFuzzer and the replay driver still see a crash.

#pragma once

#ifndef ROCKETNET_TESTS_FUZZ_ROCKETNET_FUZZ_QUIET_H
#define ROCKETNET_TESTS_FUZZ_ROCKETNET_FUZZ_QUIET_H

#include "rocketnet.h"

#include <stdio.h>

static void rocketnet_fuzz_assert_to_stderr( const char * condition, const char * function, const char * file, int line )
{
    fprintf( stderr, "assert failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stderr );
#if defined( _MSC_VER )
    __debugbreak();
#elif defined( __clang__ )
    __builtin_debugtrap();
#elif defined( __GNUC__ )
    __builtin_trap();
#else
#error "asserts not supported on this platform!"
#endif // #if defined( _MSC_VER )
}

static inline void rocketnet_fuzz_quiet_log()
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_assert_function( rocketnet_fuzz_assert_to_stderr );
}

#endif // ROCKETNET_TESTS_FUZZ_ROCKETNET_FUZZ_QUIET_H
