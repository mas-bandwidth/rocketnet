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
//     macOS 32, one above an ordinary thread (31), since its minimum (15)
//     demoted the thread and its maximum (47) put it above the game's threads.
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

#include <limits.h>
#include <stdio.h>
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
    {
        return;
    }
    if ( strstr( message, "thread priority:" ) == NULL && strstr( message, "timer period:" ) == NULL )
    {
        return;
    }
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

// the requested SCHED_RR priority is one above an ordinary thread: flow's
// minimum on Linux, 32 on macOS, where the range is 15 to 47 and an ordinary
// thread 31; never the maximum there, which outranks the game's threads

// POSIX only, and registered only there: Windows has no SCHED_RR, so the body
// would be empty and the case would pass having checked nothing

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
static void test_thread_priority_rr_above_ordinary_threads()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    rocketnet_unit_check( rocketnet_platform_network_thread_rr_priority() == sched_get_priority_min( SCHED_RR ) );
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    rocketnet_unit_check( sched_get_priority_min( SCHED_RR ) == 15 );
    rocketnet_unit_check( sched_get_priority_max( SCHED_RR ) == 47 );
    rocketnet_unit_check( rocketnet_platform_network_thread_rr_priority() == 32 );
#else
    rocketnet_unit_check( rocketnet_platform_network_thread_rr_priority() >= sched_get_priority_min( SCHED_RR ) );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
}
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

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
    // query ran at the process's first init, and a later init keeps it
    rocketnet_unit_check( rocketnet_platform_init() );

    rocketnet_platform_term();

    rocketnet_unit_check( priority_fake_count == 0 );
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

    // the error is named by number and errno.h name, from the platform's own
    // table, and never by strerror's text (which allocates on glibc)

    char named[32];
    snprintf( named, sizeof( named ), "refused (errno %d EPERM);", EPERM );
    rocketnet_unit_check( strstr( priority_log_last, named ) != NULL );
    rocketnet_unit_check( strstr( priority_log_last, strerror( EPERM ) ) == NULL );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_unit_check( strstr( priority_log_last, "the thread runs at its default priority" ) != NULL );

    priority_fake_restore();
}

// A refused priority call is what every client connect and reconnect of an
// unprivileged process takes (rocketnet_client_transport_create calls
// rocketnet_platform_client_thread_priority right after its thread create),
// and a server start the same. It makes no allocation: not through the
// library's allocator, not through operator new, and on glibc not through the
// C allocator either, where strerror and strerror_r call malloc and free on
// every call (their gettext lookup). The allocation trap of
// tests/test_channels.cpp is armed across the connect (the refusal that logs),
// the reconnect (counted, not logged) and a server thread's call; a firing
// trap ends the case with "live allocation: malloc ... during thread priority
// refused".

void rocketnet_test_allocation_trap_arm( const char * phase );
void rocketnet_test_allocation_trap_disarm();

static void test_thread_priority_refusal_makes_no_allocation()
{
    struct rocketnet_platform_thread_t thread;
    memset( &thread, 0, sizeof( thread ) );
    priority_fake_install( true );

    const uint64_t refusals = rocketnet_get_thread_priority_refusals();

    rocketnet_test_allocation_trap_arm( "thread priority refused" );

    rocketnet_platform_client_thread_priority( &thread ); // connect
    rocketnet_platform_client_thread_priority( &thread ); // reconnect
    rocketnet_platform_server_thread_priority( &thread ); // server start

    rocketnet_test_allocation_trap_disarm();

    rocketnet_unit_check( priority_fake_count == 3 );
    rocketnet_unit_check( rocketnet_get_thread_priority_refusals() == refusals + 3 );
    rocketnet_unit_check( priority_log_lines == 1 );
    rocketnet_unit_check( strstr( priority_log_last, "refused" ) != NULL );

    priority_fake_restore();
}

// The timer period exists only on Windows, so both cases are built only there.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static void test_thread_priority_windows_timer_period_at_init_and_term()
{
    priority_fake_install( false );

    // the frequency query ran at the process's first init; a later init
    // keeps the clock and its origin
    rocketnet_unit_check( rocketnet_platform_init() );
    rocketnet_unit_check( priority_fake_count == 1 );
    rocketnet_unit_check( priority_fake_calls[0].kind == PRIORITY_FAKE_TIME_BEGIN_PERIOD );
    rocketnet_unit_check( priority_fake_calls[0].period == 1 );

    rocketnet_platform_term();

    rocketnet_unit_check( priority_fake_count == 2 );
    rocketnet_unit_check( priority_fake_calls[1].kind == PRIORITY_FAKE_TIME_END_PERIOD );
    rocketnet_unit_check( priority_fake_calls[1].period == 1 );

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
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
REGISTER_UNIT_TEST( test_thread_priority_rr_above_ordinary_threads );
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
REGISTER_UNIT_TEST( test_thread_priority_client_requests_space_values );
REGISTER_UNIT_TEST( test_thread_priority_server_requests_space_values );
REGISTER_UNIT_TEST( test_thread_priority_off_makes_no_calls );
REGISTER_UNIT_TEST( test_thread_priority_refusal_counts_and_logs_once );
REGISTER_UNIT_TEST( test_thread_priority_refusal_makes_no_allocation );

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

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
// the mask a Linux worker left on cpu 0 is named by, as ranges: runs, single
// cpus, the empty mask, and a list too long for the text cut with "..."
static void test_platform_cpu_list_text()
{
    unsigned char cpus[16] = {};
    char text[64];
    rocketnet_platform_cpu_list_text( cpus, 16, text, (int) sizeof( text ) );
    rocketnet_unit_check( strcmp( text, "none" ) == 0 );

    cpus[0] = 1;
    rocketnet_platform_cpu_list_text( cpus, 16, text, (int) sizeof( text ) );
    rocketnet_unit_check( strcmp( text, "0" ) == 0 );

    cpus[1] = 1;
    cpus[2] = 1;
    cpus[3] = 1;
    cpus[8] = 1;
    cpus[10] = 1;
    cpus[11] = 1;
    cpus[15] = 1;
    rocketnet_platform_cpu_list_text( cpus, 16, text, (int) sizeof( text ) );
    rocketnet_unit_check( strcmp( text, "0-3,8,10-11,15" ) == 0 );

    char small[8];
    rocketnet_platform_cpu_list_text( cpus, 16, small, (int) sizeof( small ) );
    rocketnet_unit_check( strcmp( small, "0-3..." ) == 0 );
}

REGISTER_UNIT_TEST( test_platform_cpu_list_text );
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// The one text every failure line names an OS error with
// (rocketnet_platform_socket_error_text): the number and its name from a fixed
// table, "?" for a number the table does not hold, always terminated inside
// the caller's 128 bytes, and made with no allocation (the trap is armed
// across every call in a development build; a production suite has no trap
// and checks the text alone).

static void platform_error_text_check( int error, const char * expected )
{
#if ROCKETNET_DEV
    rocketnet_test_allocation_trap_arm( "error text" );
#endif // #if ROCKETNET_DEV
    const struct rocketnet_platform_error_text_t text = rocketnet_platform_socket_error_text( error );
#if ROCKETNET_DEV
    rocketnet_test_allocation_trap_disarm();
#endif // #if ROCKETNET_DEV

    rocketnet_unit_check( memchr( text.text, '\0', sizeof( text.text ) ) != NULL );
    rocketnet_unit_check( strcmp( text.text, expected ) == 0 );
}

static void test_platform_error_text_names_the_error_without_allocating()
{
    char expected[64];

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    platform_error_text_check( WSAEADDRINUSE, "Winsock 10048 WSAEADDRINUSE" );
    platform_error_text_check( WSAEACCES, "Winsock 10013 WSAEACCES" );
    platform_error_text_check( 1, "Winsock 1 ?" );
    platform_error_text_check( 0, "Winsock 0 ?" );
    platform_error_text_check( -1, "Winsock -1 ?" );
    snprintf( expected, sizeof( expected ), "Winsock %d ?", INT_MIN );
    platform_error_text_check( INT_MIN, expected );
#else
    // the errors the connect, send and thread placement paths take; the
    // numbers are the platform's own (EAGAIN is 11 on Linux and 35 on macOS)
    static const struct
    {
        int error;
        const char * name;
    } named[] = {
        { EPERM, "EPERM" },
        { EACCES, "EACCES" },
        { EINVAL, "EINVAL" },
        { EAGAIN, "EAGAIN" },
        { ENOBUFS, "ENOBUFS" },
        { ENOMEM, "ENOMEM" },
        { EADDRINUSE, "EADDRINUSE" },
        { ECONNREFUSED, "ECONNREFUSED" },
        { EHOSTUNREACH, "EHOSTUNREACH" },
        { ENETUNREACH, "ENETUNREACH" },
        { ENETDOWN, "ENETDOWN" },
        { EMSGSIZE, "EMSGSIZE" },
        { EIO, "EIO" },
        { ENOENT, "ENOENT" },
        { EBUSY, "EBUSY" },
        { ENODEV, "ENODEV" },
        { EOPNOTSUPP, "EOPNOTSUPP" },
    };

    for ( size_t i = 0; i < sizeof( named ) / sizeof( named[0] ); i++ )
    {
        snprintf( expected, sizeof( expected ), "errno %d %s", named[i].error, named[i].name );
        platform_error_text_check( named[i].error, expected );
    }

    // a number the table does not hold keeps its number

    platform_error_text_check( 0, "errno 0 ?" );
    platform_error_text_check( -1, "errno -1 ?" );
    platform_error_text_check( 99999, "errno 99999 ?" );
    snprintf( expected, sizeof( expected ), "errno %d ?", INT_MIN );
    platform_error_text_check( INT_MIN, expected );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

REGISTER_UNIT_TEST( test_platform_error_text_names_the_error_without_allocating );

// The whole table. Every row formats as its own number and name, so a row
// whose number an earlier row already holds (it would print the earlier name)
// fails here. Off Windows the table also holds a row for every errno the
// library compares against or hands on in the code a Linux build compiles
// (grep -ow 'E[A-Z0-9]*' over sdk/, less the Windows and Apple files): a line
// that names one of them prints its name, never "?". EWOULDBLOCK and ENOTSUP
// are held by number: where one is its own number it has its own row, and
// where it is EAGAIN's or EOPNOTSUPP's that row names it.

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#define PLATFORM_ERROR_TEXT_REQUIRED( name ) { name, #name },
static const struct
{
    int error;
    const char * name;
} platform_error_text_required[] = {
    PLATFORM_ERROR_TEXT_REQUIRED( E2BIG ) PLATFORM_ERROR_TEXT_REQUIRED( EACCES ) PLATFORM_ERROR_TEXT_REQUIRED( EADDRINUSE )
        PLATFORM_ERROR_TEXT_REQUIRED( EADDRNOTAVAIL ) PLATFORM_ERROR_TEXT_REQUIRED( EAFNOSUPPORT ) PLATFORM_ERROR_TEXT_REQUIRED( EAGAIN )
            PLATFORM_ERROR_TEXT_REQUIRED( EBADF ) PLATFORM_ERROR_TEXT_REQUIRED( EBUSY ) PLATFORM_ERROR_TEXT_REQUIRED( ECANCELED )
                PLATFORM_ERROR_TEXT_REQUIRED( ECONNREFUSED ) PLATFORM_ERROR_TEXT_REQUIRED( ECONNRESET ) PLATFORM_ERROR_TEXT_REQUIRED( EDESTADDRREQ )
                    PLATFORM_ERROR_TEXT_REQUIRED( EEXIST ) PLATFORM_ERROR_TEXT_REQUIRED( EFAULT ) PLATFORM_ERROR_TEXT_REQUIRED( EHOSTUNREACH )
                        PLATFORM_ERROR_TEXT_REQUIRED( EINTR ) PLATFORM_ERROR_TEXT_REQUIRED( EINVAL ) PLATFORM_ERROR_TEXT_REQUIRED( EIO )
                            PLATFORM_ERROR_TEXT_REQUIRED( EISCONN ) PLATFORM_ERROR_TEXT_REQUIRED( EMFILE ) PLATFORM_ERROR_TEXT_REQUIRED( EMSGSIZE )
                                PLATFORM_ERROR_TEXT_REQUIRED( ENETDOWN ) PLATFORM_ERROR_TEXT_REQUIRED( ENETUNREACH ) PLATFORM_ERROR_TEXT_REQUIRED( ENFILE )
                                    PLATFORM_ERROR_TEXT_REQUIRED( ENOBUFS ) PLATFORM_ERROR_TEXT_REQUIRED( ENODEV ) PLATFORM_ERROR_TEXT_REQUIRED( ENOENT )
                                        PLATFORM_ERROR_TEXT_REQUIRED( ENOMEM ) PLATFORM_ERROR_TEXT_REQUIRED( ENOPROTOOPT ) PLATFORM_ERROR_TEXT_REQUIRED( ENOSPC )
                                            PLATFORM_ERROR_TEXT_REQUIRED( ENOSYS ) PLATFORM_ERROR_TEXT_REQUIRED( ENOTCONN ) PLATFORM_ERROR_TEXT_REQUIRED( ENOTSOCK )
                                                PLATFORM_ERROR_TEXT_REQUIRED( ENXIO ) PLATFORM_ERROR_TEXT_REQUIRED( EOPNOTSUPP ) PLATFORM_ERROR_TEXT_REQUIRED( EPERM )
                                                    PLATFORM_ERROR_TEXT_REQUIRED( EPIPE ) PLATFORM_ERROR_TEXT_REQUIRED( EPROTO ) PLATFORM_ERROR_TEXT_REQUIRED( EPROTONOSUPPORT )
                                                        PLATFORM_ERROR_TEXT_REQUIRED( ERANGE ) PLATFORM_ERROR_TEXT_REQUIRED( ESRCH ) PLATFORM_ERROR_TEXT_REQUIRED( ETIMEDOUT )
#if defined( EHOSTDOWN )
                                                            PLATFORM_ERROR_TEXT_REQUIRED( EHOSTDOWN )
#endif // #if defined( EHOSTDOWN )
#if defined( ENONET )
                                                                PLATFORM_ERROR_TEXT_REQUIRED( ENONET )
#endif // #if defined( ENONET )
#if defined( ETIME )
                                                                    PLATFORM_ERROR_TEXT_REQUIRED( ETIME )
#endif // #if defined( ETIME )
};
#undef PLATFORM_ERROR_TEXT_REQUIRED
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

static void test_platform_error_text_names_every_table_row()
{
    char expected[64];
    const int count = rocketnet_platform_socket_error_name_count();
    rocketnet_unit_check( count > 0 );

    for ( int i = 0; i < count; i++ )
    {
        int error = 0;
        const char * name = rocketnet_platform_socket_error_name_at( i, &error );
        rocketnet_unit_check( name != NULL && name[0] != '\0' );
        rocketnet_unit_check( error != 0 );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        snprintf( expected, sizeof( expected ), "Winsock %d %s", error, name );
#else
        snprintf( expected, sizeof( expected ), "errno %d %s", error, name );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        platform_error_text_check( error, expected );
    }

    // outside the table

    int outside = 1;
    rocketnet_unit_check( rocketnet_platform_socket_error_name_at( -1, &outside ) == NULL && outside == 0 );
    outside = 1;
    rocketnet_unit_check( rocketnet_platform_socket_error_name_at( count, &outside ) == NULL && outside == 0 );

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    for ( size_t r = 0; r < sizeof( platform_error_text_required ) / sizeof( platform_error_text_required[0] ); r++ )
    {
        bool held = false;
        for ( int i = 0; i < count; i++ )
        {
            int error = 0;
            const char * name = rocketnet_platform_socket_error_name_at( i, &error );
            if ( strcmp( name, platform_error_text_required[r].name ) == 0 )
            {
                held = error == platform_error_text_required[r].error;
            }
        }
        if ( !held )
        {
            printf( "the error text table holds no row for %s\n", platform_error_text_required[r].name );
        }
        rocketnet_unit_check( held );
    }

    // the two an errno may share its number with: named either way, never "?"

    rocketnet_unit_check( strchr( rocketnet_platform_socket_error_text( EWOULDBLOCK ).text, '?' ) == NULL );
    rocketnet_unit_check( strchr( rocketnet_platform_socket_error_text( ENOTSUP ).text, '?' ) == NULL );
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
}

REGISTER_UNIT_TEST( test_platform_error_text_names_every_table_row );
