/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The network-thread priority calls (sdk/rocketnet_platform.cpp), through the
// platform call table seam: a recording fake stands in for the operating
// system and a thread handle that is never started stands in for the thread.
// No real thread, no real time, no socket.
//
// The values are the flow library's (flow_platform.cpp), with one change:
//   POSIX client and server thread: pthread_setschedparam( thread, SCHED_RR,
//     sched_get_priority_min( SCHED_RR ) ), one call, on Linux as before; on
//     macOS sched_get_priority_max( SCHED_RR ) (47), since its minimum (15)
//     demoted the thread below every ordinary one (31).
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

#include <string.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <errno.h>
#include <sched.h>
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_DEV
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
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
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
static const rocketnet_platform_priority_calls_t * priority_fake_saved;

static void priority_fake_install( bool fail )
{
    priority_fake_real = rocketnet_platform_priority_calls_real();

    // the worker's table (its fake platform start) is restored at the end

    priority_fake_saved = rocketnet_platform_priority_calls_current();
    priority_fake_table.set_sched = priority_fake_set_sched;
    priority_fake_table.set_thread_priority = priority_fake_set_thread_priority;
    priority_fake_table.time_begin_period = priority_fake_time_begin_period;
    priority_fake_table.time_end_period = priority_fake_time_end_period;
    priority_fake_table.timer_frequency = priority_fake_timer_frequency;
    priority_fake_table.mutex_init = rocketnet_platform_priority_calls_real()->mutex_init;
    priority_fake_table.platform_start = priority_fake_saved->platform_start;
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
    rocketnet_platform_set_priority_calls_for_testing( priority_fake_saved );
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
    rocketnet_unit_check( priority_fake_calls[0].priority == rocketnet_platform_network_thread_rr_priority() );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

// the requested SCHED_RR priority is above every ordinary thread: flow's
// minimum on Linux, the maximum (47) on macOS, where the minimum is 15 and an
// ordinary thread 31

static void test_thread_priority_rr_above_ordinary_threads()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    rocketnet_unit_check( rocketnet_platform_network_thread_rr_priority() == sched_get_priority_min( SCHED_RR ) );
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    rocketnet_unit_check( sched_get_priority_min( SCHED_RR ) == 15 );
    rocketnet_unit_check( rocketnet_platform_network_thread_rr_priority() == 47 );
#elif ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    rocketnet_unit_check( rocketnet_platform_network_thread_rr_priority() >= sched_get_priority_min( SCHED_RR ) );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
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
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
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
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
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
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

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
    {
        rocketnet_unit_check( priority_fake_calls[i].kind != PRIORITY_FAKE_TIME_END_PERIOD );
    }

    priority_fake_restore();
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
REGISTER_UNIT_TEST( test_thread_priority_windows_timer_period_at_init_and_term );
REGISTER_UNIT_TEST( test_thread_priority_windows_timer_refusal_counts_and_logs_once );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

REGISTER_UNIT_TEST( test_thread_priority_default_is_on );
REGISTER_UNIT_TEST( test_thread_priority_rr_above_ordinary_threads );
REGISTER_UNIT_TEST( test_thread_priority_client_requests_space_values );
REGISTER_UNIT_TEST( test_thread_priority_server_requests_space_values );
REGISTER_UNIT_TEST( test_thread_priority_off_makes_no_calls );
REGISTER_UNIT_TEST( test_thread_priority_refusal_counts_and_logs_once );

#endif // #if ROCKETNET_DEV

static void test_thread_priority_enable_disable_policy()
{
    const bool previous = rocketnet_platform_thread_priority_enabled();
    rocketnet_thread_priority( false );

    rocketnet_unit_check( !rocketnet_platform_thread_priority_enabled() );

    rocketnet_thread_priority( true );

    rocketnet_unit_check( rocketnet_platform_thread_priority_enabled() );

    rocketnet_thread_priority( previous );
}

REGISTER_UNIT_TEST( test_thread_priority_enable_disable_policy );
