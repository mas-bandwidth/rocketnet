/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// tests/time_scale.h -- the one scale on the suites' wall-clock bounds.
//
// Functional cases target 60 seconds and have a fixed 120-second completion
// limit, including Debug, sanitizer and emulated builds. This is a correctness
// test budget, not a networking performance requirement. Other individual
// waits and the unit runner retain their existing build-dependent scaling.

#ifndef ROCKETNET_TEST_TIME_SCALE_H
#define ROCKETNET_TEST_TIME_SCALE_H

#define ROCKETNET_FUNCTIONAL_CASE_TARGET_SECONDS 60.0
#define ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS 120.0

#define ROCKETNET_FUNCTIONAL_DEBUG_TIME_SCALE 5.0

// a sanitizer build is scaled even with NDEBUG (gcc's __SANITIZE_*__, clang's __has_feature)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ROCKETNET_FUNCTIONAL_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define ROCKETNET_FUNCTIONAL_SANITIZED 1
#endif
#endif

#if defined(NDEBUG) && !defined(ROCKETNET_FUNCTIONAL_SANITIZED)
#define ROCKETNET_FUNCTIONAL_TIME_SCALE 1.0
#else
#define ROCKETNET_FUNCTIONAL_TIME_SCALE ROCKETNET_FUNCTIONAL_DEBUG_TIME_SCALE
#endif

// Full-system emulation runs a Release build, but one case receives only a
// fraction of a native CPU. Scale case completion bounds without lengthening
// workload durations such as the live matrix's measurement window. The
// full-system workflow opts in explicitly; native Release remains 1.

#if defined(ROCKETNET_TEST_EMULATED)
#define ROCKETNET_TEST_CASE_TIME_SCALE ROCKETNET_FUNCTIONAL_DEBUG_TIME_SCALE
#else
#define ROCKETNET_TEST_CASE_TIME_SCALE ROCKETNET_FUNCTIONAL_TIME_SCALE
#endif

// ---------------------------------------------------------------------------
// A bounded wait for an event the host delivers (a datagram, a counter).
//
// The wait is bounded by a count of polls -- updates, receives or readiness
// polls -- and never by wall time: a loaded host may take any amount of wall
// time to schedule the case that waits, and the library's own clock is the
// only clock a timeout the library owns is measured in. The wall cap below
// is a hang guard only: it fires when the polls themselves stop making
// progress, and it names the case that hung.

#ifdef __cplusplus

#include <chrono>
#include <cstdio>

struct rocketnet_bounded_wait_t
{
    std::chrono::steady_clock::time_point start;
    double guard_seconds;
    int poll_bound;
    int polls;
    const char * name;
};

// guard_seconds: the wall cap, generous, a hang guard. poll_bound: the
// number of polls that bounds the wait.
inline void rocketnet_bounded_wait_start( rocketnet_bounded_wait_t & wait, const char * name,
    double guard_seconds, int poll_bound )
{
    wait.start = std::chrono::steady_clock::now();
    wait.guard_seconds = guard_seconds;
    wait.poll_bound = poll_bound;
    wait.polls = 0;
    wait.name = name;
}

// One poll of a bounded wait. True while the wait may continue; false when
// the poll bound or the wall cap was reached, after printing the hang
// loudly and naming the case.
inline bool rocketnet_bounded_wait_poll( rocketnet_bounded_wait_t & wait )
{
    wait.polls++;
    const double seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - wait.start ).count();
    if ( wait.polls <= wait.poll_bound && seconds < wait.guard_seconds )
        return true;
    printf( "check failed: %s hung: %d polls, %.1f s without the event\n", wait.name, wait.polls, seconds );
    fflush( stdout );
    return false;
}

#endif // __cplusplus

#endif // ROCKETNET_TEST_TIME_SCALE_H
