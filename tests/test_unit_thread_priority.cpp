/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The network-thread priority calls (sdk/rocketnet_platform.cpp), through the
// platform call table seam: a recording fake stands in for the operating
// system and a thread handle that is never started stands in for the thread.
// No real thread, no real time, no socket.
//
// The values are the flow library's (flow_platform.cpp), unchanged:
//   POSIX client and server thread: pthread_setschedparam( thread, SCHED_RR,
//     sched_get_priority_min( SCHED_RR ) ), one call.
//   Windows client and server thread: SetThreadPriority( thread,
//     THREAD_PRIORITY_TIME_CRITICAL ), one call.
//   Windows platform init: timeBeginPeriod( 1 ) before the timer frequency
//     query; platform term: timeEndPeriod( 1 ).
// Off (rocketnet_thread_priority( false )) makes no priority call and no
// timer period call. A refused call adds one to the process-wide refusal count
// and logs one warning per call kind, and the caller carries on.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_frame_sync.h"

#include <string.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <errno.h>
#include <sched.h>
#endif

enum
{
    PRIORITY_FAKE_SET_SCHED = 1,
    PRIORITY_FAKE_SET_THREAD_PRIORITY,
    PRIORITY_FAKE_TIME_BEGIN_PERIOD,
    PRIORITY_FAKE_TIME_END_PERIOD,
    PRIORITY_FAKE_TIMER_FREQUENCY,
};

struct priority_fake_call_t
{
    int kind;
    struct rocketnet_platform_thread_t * thread;
    int policy;
    int priority;
    unsigned int period;
};

static priority_fake_call_t priority_fake_calls[16];
static int priority_fake_count;
static bool priority_fake_fail;
static const rocketnet_platform_priority_calls_t * priority_fake_real;

static void priority_fake_push( int kind, struct rocketnet_platform_thread_t * thread, int policy, int priority, unsigned int period )
{
    rocketnet_unit_check( priority_fake_count < 16 );
    priority_fake_call_t * call = &priority_fake_calls[priority_fake_count++];
    call->kind = kind;
    call->thread = thread;
    call->policy = policy;
    call->priority = priority;
    call->period = period;
}

static int priority_fake_set_sched( struct rocketnet_platform_thread_t * thread, int policy, int priority )
{
    priority_fake_push( PRIORITY_FAKE_SET_SCHED, thread, policy, priority, 0 );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return priority_fake_fail ? 1 : 0;
#else
    return priority_fake_fail ? EPERM : 0;
#endif
}

static int priority_fake_set_thread_priority( struct rocketnet_platform_thread_t * thread, int priority )
{
    priority_fake_push( PRIORITY_FAKE_SET_THREAD_PRIORITY, thread, 0, priority, 0 );
    return priority_fake_fail ? 0 : 1;
}

static unsigned int priority_fake_time_begin_period( unsigned int period )
{
    priority_fake_push( PRIORITY_FAKE_TIME_BEGIN_PERIOD, NULL, 0, 0, period );
    return priority_fake_fail ? 97u : 0u;  // 97 is TIMERR_NOCANDO
}

static unsigned int priority_fake_time_end_period( unsigned int period )
{
    priority_fake_push( PRIORITY_FAKE_TIME_END_PERIOD, NULL, 0, 0, period );
    return 0u;
}

static void priority_fake_timer_frequency( void * frequency )
{
    priority_fake_push( PRIORITY_FAKE_TIMER_FREQUENCY, NULL, 0, 0, 0 );
    // the platform clock still needs the real frequency
    priority_fake_real->timer_frequency( frequency );
}

static rocketnet_platform_priority_calls_t priority_fake_table;

static int priority_log_lines;
static char priority_log_last[1024];

static void priority_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    if ( level != ROCKETNET_LOG_LEVEL_WARN )
        return;
    if ( strstr( message, "thread priority:" ) == NULL && strstr( message, "timer period:" ) == NULL )
        return;
    priority_log_lines++;
    const size_t n = length < sizeof( priority_log_last ) - 1 ? length : sizeof( priority_log_last ) - 1;
    memcpy( priority_log_last, message, n );
    priority_log_last[n] = '\0';
}

static enum rocketnet_log_level_t priority_saved_level;

static void priority_fake_install( bool fail )
{
    priority_fake_real = rocketnet_platform_priority_calls_real();
    priority_fake_table.set_sched = priority_fake_set_sched;
    priority_fake_table.set_thread_priority = priority_fake_set_thread_priority;
    priority_fake_table.time_begin_period = priority_fake_time_begin_period;
    priority_fake_table.time_end_period = priority_fake_time_end_period;
    priority_fake_table.timer_frequency = priority_fake_timer_frequency;
    priority_fake_table.mutex_init = rocketnet_platform_priority_calls_real()->mutex_init;
    priority_fake_table.platform_start = rocketnet_platform_priority_calls_real()->platform_start;
    priority_fake_table.create_waitable_timer = rocketnet_platform_priority_calls_real()->create_waitable_timer;
    priority_fake_count = 0;
    priority_fake_fail = fail;
    memset( priority_fake_calls, 0, sizeof( priority_fake_calls ) );
    priority_log_lines = 0;
    priority_log_last[0] = '\0';
    priority_saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( priority_log_capture, NULL );
    rocketnet_platform_thread_priority_reset_logged_for_testing();
    rocketnet_platform_set_priority_calls_for_testing( &priority_fake_table );
}

// restores the real table, the switch and the log sink at the end of each case
static void priority_fake_restore()
{
    rocketnet_platform_set_priority_calls_for_testing( NULL );
    rocketnet_thread_priority( true );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( priority_saved_level );
    rocketnet_platform_thread_priority_reset_logged_for_testing();
}

static void priority_check_one_thread_call( struct rocketnet_platform_thread_t * thread )
{
    rocketnet_unit_check( priority_fake_count == 1 );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_unit_check( priority_fake_calls[0].kind == PRIORITY_FAKE_SET_THREAD_PRIORITY );
    rocketnet_unit_check( priority_fake_calls[0].thread == thread );
    rocketnet_unit_check( priority_fake_calls[0].priority == THREAD_PRIORITY_TIME_CRITICAL );
#else
    rocketnet_unit_check( priority_fake_calls[0].kind == PRIORITY_FAKE_SET_SCHED );
    rocketnet_unit_check( priority_fake_calls[0].thread == thread );
    rocketnet_unit_check( priority_fake_calls[0].policy == SCHED_RR );
    rocketnet_unit_check( priority_fake_calls[0].priority == sched_get_priority_min( SCHED_RR ) );
#endif
}

static void test_thread_priority_default_is_on()
{
    rocketnet_unit_check( rocketnet_get_thread_priority() );
    rocketnet_thread_priority( false );
    rocketnet_unit_check( !rocketnet_get_thread_priority() );
    rocketnet_thread_priority( true );
    rocketnet_unit_check( rocketnet_get_thread_priority() );
}

static void test_thread_priority_client_requests_space_values()
{
    struct rocketnet_platform_thread_t thread;
    memset( &thread, 0, sizeof( thread ) );
    priority_fake_install( false );
    const uint64_t refusals = rocketnet_get_thread_priority_refusals();
    rocketnet_platform_client_thread_priority( &thread );
    priority_check_one_thread_call( &thread );
    rocketnet_unit_check( rocketnet_get_thread_priority_refusals() == refusals );
    rocketnet_unit_check( priority_log_lines == 0 );
    priority_fake_restore();
}

static void test_thread_priority_server_requests_space_values()
{
    struct rocketnet_platform_thread_t thread;
    memset( &thread, 0, sizeof( thread ) );
    priority_fake_install( false );
    const uint64_t refusals = rocketnet_get_thread_priority_refusals();
    rocketnet_platform_server_thread_priority( &thread );
    priority_check_one_thread_call( &thread );
    rocketnet_unit_check( rocketnet_get_thread_priority_refusals() == refusals );
    rocketnet_unit_check( priority_log_lines == 0 );
    priority_fake_restore();
}

static void test_thread_priority_off_makes_no_calls()
{
    struct rocketnet_platform_thread_t thread;
    memset( &thread, 0, sizeof( thread ) );
    priority_fake_install( false );
    rocketnet_thread_priority( false );
    rocketnet_platform_client_thread_priority( &thread );
    rocketnet_platform_server_thread_priority( &thread );
    rocketnet_unit_check( priority_fake_count == 0 );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    // off at init: no timer period call at init or at term; the frequency
    // query still runs
    rocketnet_unit_check( rocketnet_platform_init() );
    rocketnet_platform_term();
    rocketnet_unit_check( priority_fake_count == 1 );
    rocketnet_unit_check( priority_fake_calls[0].kind == PRIORITY_FAKE_TIMER_FREQUENCY );
#endif
    priority_fake_restore();
}

static void test_thread_priority_refusal_counts_and_logs_once()
{
    struct rocketnet_platform_thread_t thread;
    memset( &thread, 0, sizeof( thread ) );
    priority_fake_install( true );
    const uint64_t refusals = rocketnet_get_thread_priority_refusals();
    rocketnet_platform_client_thread_priority( &thread );
    rocketnet_unit_check( rocketnet_get_thread_priority_refusals() == refusals + 1 );
    rocketnet_unit_check( priority_log_lines == 1 );
    rocketnet_platform_server_thread_priority( &thread );
    rocketnet_unit_check( rocketnet_get_thread_priority_refusals() == refusals + 2 );
    rocketnet_unit_check( priority_log_lines == 1 );
    rocketnet_unit_check( priority_fake_count == 2 );
    rocketnet_unit_check( strstr( priority_log_last, "refused" ) != NULL );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_unit_check( strstr( priority_log_last, "SetThreadPriority THREAD_PRIORITY_TIME_CRITICAL" ) != NULL );
#else
    rocketnet_unit_check( strstr( priority_log_last, "pthread_setschedparam SCHED_RR" ) != NULL );
    rocketnet_unit_check( strstr( priority_log_last, strerror( EPERM ) ) != NULL );
#endif
    rocketnet_unit_check( strstr( priority_log_last, "the thread runs at its default priority" ) != NULL );
    priority_fake_restore();
}

// The timer period exists only on Windows, so both cases are built only there.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static void test_thread_priority_windows_timer_period_at_init_and_term()
{
    priority_fake_install( false );
    rocketnet_unit_check( rocketnet_platform_init() );
    rocketnet_unit_check( priority_fake_count == 2 );
    rocketnet_unit_check( priority_fake_calls[0].kind == PRIORITY_FAKE_TIME_BEGIN_PERIOD );
    rocketnet_unit_check( priority_fake_calls[0].period == 1 );
    rocketnet_unit_check( priority_fake_calls[1].kind == PRIORITY_FAKE_TIMER_FREQUENCY );
    rocketnet_platform_term();
    rocketnet_unit_check( priority_fake_count == 3 );
    rocketnet_unit_check( priority_fake_calls[2].kind == PRIORITY_FAKE_TIME_END_PERIOD );
    rocketnet_unit_check( priority_fake_calls[2].period == 1 );
    priority_fake_restore();
}
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static void test_thread_priority_windows_timer_refusal_counts_and_logs_once()
{
    priority_fake_install( true );
    const uint64_t refusals = rocketnet_get_thread_priority_refusals();
    rocketnet_unit_check( rocketnet_platform_init() );
    rocketnet_platform_term();
    rocketnet_unit_check( rocketnet_platform_init() );
    rocketnet_platform_term();
    rocketnet_unit_check( rocketnet_get_thread_priority_refusals() == refusals + 2 );
    rocketnet_unit_check( priority_log_lines == 1 );
    rocketnet_unit_check( strstr( priority_log_last, "timeBeginPeriod 1 refused" ) != NULL );
    // a refused begin has no matching end
    for ( int i = 0; i < priority_fake_count; ++i )
        rocketnet_unit_check( priority_fake_calls[i].kind != PRIORITY_FAKE_TIME_END_PERIOD );
    priority_fake_restore();
}
#endif

// Frame sync's own priority step (sdk/rocketnet_frame_sync.cpp,
// rocketnet_frame_sync_elevate_priority) honours the same switch: off, it
// returns before any call, so it neither logs its "frame sync:" line nor
// holds priority, and nothing reaches the call table. The step runs on a
// frame sync that is never created: no timer, no thread, no wait.

extern int rocketnet_frame_sync_elevate_priority_for_testing( uint64_t period_ns );

static int priority_frame_sync_lines;

static void priority_frame_sync_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    (void) length;
    if ( strstr( message, "frame sync:" ) != NULL )
        priority_frame_sync_lines++;
}

static void test_thread_priority_off_frame_sync_takes_no_priority()
{
    priority_fake_install( false );
    rocketnet_log_function( priority_frame_sync_log_capture, NULL );
    priority_frame_sync_lines = 0;
    rocketnet_thread_priority( false );
    const int held = rocketnet_frame_sync_elevate_priority_for_testing( 16666666 );
    rocketnet_unit_check( priority_frame_sync_lines == 0 );
    rocketnet_unit_check( held == 0 );
    rocketnet_unit_check( priority_fake_count == 0 );
    priority_fake_restore();
}

REGISTER_UNIT_TEST( test_thread_priority_off_frame_sync_takes_no_priority );

// The time constraint policy values (rocketnet_frame_sync_time_constraint) for
// the macOS and iOS priority step: a period that does not fit the uint32_t
// policy fields, or whose period_ns * denom overflows uint64_t, is refused
// with nothing written, never truncated. Pure: no thread, no clock, no policy
// call.

static void test_frame_sync_time_constraint_refuses_period_over_uint32()
{
    const uint32_t sentinel = 0xDEADBEEF;

    // 5 s on a 1/1 timebase: 5e9 mach units is past UINT32_MAX

    uint32_t period = sentinel;
    uint32_t computation = sentinel;
    uint32_t constraint = sentinel;
    rocketnet_unit_check( rocketnet_frame_sync_time_constraint( 5000000000ULL, 1, 1, &period, &computation, &constraint ) == 0 );
    rocketnet_unit_check( period == sentinel );
    rocketnet_unit_check( computation == sentinel );
    rocketnet_unit_check( constraint == sentinel );

    // 1/60 s on 1/1: period, period / 4 and period / 2

    const uint64_t period_ns = 16666666;
    rocketnet_unit_check( rocketnet_frame_sync_time_constraint( period_ns, 1, 1, &period, &computation, &constraint ) == 1 );
    rocketnet_unit_check( period == 16666666 );
    rocketnet_unit_check( computation == 16666666 / 4 );
    rocketnet_unit_check( constraint == 16666666 / 2 );

    // 1/60 s on 125/3 (numer 125, denom 3)

    const uint64_t period_mach = period_ns * 3 / 125;
    rocketnet_unit_check( rocketnet_frame_sync_time_constraint( period_ns, 125, 3, &period, &computation, &constraint ) == 1 );
    rocketnet_unit_check( period == (uint32_t) period_mach );
    rocketnet_unit_check( computation == (uint32_t) ( period_mach / 4 ) );
    rocketnet_unit_check( constraint == (uint32_t) ( period_mach / 2 ) );

    // UINT64_MAX / 2 ns on 125/3: period_ns * 3 overflows uint64_t, refused

    period = sentinel;
    computation = sentinel;
    constraint = sentinel;
    rocketnet_unit_check( rocketnet_frame_sync_time_constraint( UINT64_MAX / 2, 125, 3, &period, &computation, &constraint ) == 0 );
    rocketnet_unit_check( period == sentinel );
    rocketnet_unit_check( computation == sentinel );
    rocketnet_unit_check( constraint == sentinel );
}

// A wake 2^31 + 1 periods late (rocketnet_frame_sync_advance) reports 2^31 + 1
// drops, not a wrapped int count. Pure: no thread, no clock.

static void test_frame_sync_advance_counts_drops_past_int_max()
{
    const uint64_t period = 1000000;
    const uint64_t late_periods = ( 1ULL << 31 ) + 1;
    uint64_t deadline = 0;
    uint64_t error = 0;
    const uint64_t woke = late_periods * period + 1;
    const uint64_t drops = (uint64_t) rocketnet_frame_sync_advance( &deadline, period, woke, &error );
    rocketnet_unit_check( drops == late_periods );
    rocketnet_unit_check( error == woke );
    rocketnet_unit_check( deadline == ( late_periods + 1 ) * period );
    rocketnet_unit_check( deadline > woke );
}

REGISTER_UNIT_TEST( test_frame_sync_time_constraint_refuses_period_over_uint32 );
REGISTER_UNIT_TEST( test_frame_sync_advance_counts_drops_past_int_max );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
REGISTER_UNIT_TEST( test_thread_priority_windows_timer_period_at_init_and_term );
REGISTER_UNIT_TEST( test_thread_priority_windows_timer_refusal_counts_and_logs_once );
#endif

REGISTER_UNIT_TEST( test_thread_priority_default_is_on );
REGISTER_UNIT_TEST( test_thread_priority_client_requests_space_values );
REGISTER_UNIT_TEST( test_thread_priority_server_requests_space_values );
REGISTER_UNIT_TEST( test_thread_priority_off_makes_no_calls );
REGISTER_UNIT_TEST( test_thread_priority_refusal_counts_and_logs_once );
