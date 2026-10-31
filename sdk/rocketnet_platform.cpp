/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_platform.h"
#include "rocketnet_constants.h"
#include <atomic>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <atomic>
#if ROCKETNET_DEV
#include <map>
#include <string>
#endif // #if ROCKETNET_DEV
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// the direct allocation count (rocketnet_memory.h). relaxed: it is a total,
// read by a memory check, and orders against nothing
static std::atomic<uint64_t> rocketnet_direct_allocation_total{ 0 };

void rocketnet_direct_allocation_add( uint64_t bytes )
{
    rocketnet_direct_allocation_total.fetch_add( bytes, std::memory_order_relaxed );
}

void rocketnet_direct_allocation_remove( uint64_t bytes )
{
    const uint64_t before = rocketnet_direct_allocation_total.fetch_sub( bytes, std::memory_order_relaxed );
    rocketnet_assert( before >= bytes );
    (void) before;
}

extern "C" uint64_t rocketnet_direct_allocation_bytes( void )
{
    return rocketnet_direct_allocation_total.load( std::memory_order_relaxed );
}

// The token clock's epoch. A configured epoch is used as is. Zero reads the
// wall clock once, here, and takes the epoch as that Unix time less the
// caller's time, so the token clock is the epoch plus the caller's time from
// then on and advances only as the caller's clock does: a paused clock pauses
// token expiry. The only wall-clock read on the token path.

uint64_t rocketnet_platform_token_epoch( uint64_t configured, double time )
{
    if ( configured != 0 )
        return configured;

    const time_t now = ::time( NULL );
    const uint64_t wall = now > 0 ? (uint64_t) now : 0;
    const uint64_t elapsed = time > 0.0 ? (uint64_t) time : 0;

    return wall > elapsed ? wall - elapsed : 1;
}

#if ROCKETNET_DEV

// The unit suite installs this table before main. Reads then stop at the
// table, including an absent name, so ambient process values cannot enter a
// unit case. Standard strings keep test values exact and impose no SDK limit;
// the caller's buffer remains the only limit rocketnet_platform_getenv reports.
static bool rocketnet_platform_test_environment_installed;

static std::map<std::string, std::string> & rocketnet_platform_test_environment()
{
    static std::map<std::string, std::string> environment;
    return environment;
}

void rocketnet_platform_test_environment_install()
{
    rocketnet_platform_test_environment_installed = true;
}

void rocketnet_platform_test_environment_set( const char * name, const char * value )
{
    rocketnet_assert( name && value );
    rocketnet_platform_test_environment()[name] = value;
}

void rocketnet_platform_test_environment_unset( const char * name )
{
    rocketnet_assert( name );
    rocketnet_platform_test_environment().erase( name );
}

void rocketnet_platform_test_environment_reset()
{
    rocketnet_platform_test_environment().clear();
}

// Returns true when the installed table answered the read. An unset name is
// an answer (0), distinct from no table, which leaves the real platform path.
static bool rocketnet_platform_test_environment_getenv( const char * name, char * buffer, size_t size, int * result )
{
    if ( !rocketnet_platform_test_environment_installed )
        return false;
    buffer[0] = '\0';
    const std::map<std::string, std::string> & environment = rocketnet_platform_test_environment();
    const std::map<std::string, std::string>::const_iterator entry = environment.find( name );
    if ( entry == environment.end() )
    {
        *result = 0;
        return true;
    }

    if ( entry->second.size() >= size )
    {
        *result = -1;
        return true;
    }

    memcpy( buffer, entry->second.c_str(), entry->second.size() + 1 );
    *result = 1;

    return true;
}

#endif // #if ROCKETNET_DEV

// The Windows sleep, as a pure function of the request so it is unit-tested
// without a clock (test_unit_platform_sleep.cpp); compiled on every platform.
// A request of zero, less, or NaN yields only (Sleep(0)). Otherwise the
// high-resolution waitable timer gets a relative due time in negative 100-ns
// units, rounded up and never zero, and the timeBeginPeriod fallback sleeps
// whole milliseconds, rounded up, at least 1 and capped below INFINITE. The
// rounding forgives the floating-point error of the scaling (a millionth of
// a unit), so 0.0005 s is exactly 5000 units and not 5001.

struct timeval rocketnet_platform_timeval_from_seconds( float seconds )
{
    struct timeval tv;

    if ( !( seconds > 0.0f ) )
    {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        return tv;
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    tv.tv_sec = (long) seconds;
    tv.tv_usec = (long) ( ( seconds - (float) tv.tv_sec ) * 1000000.0f );
#else
    tv.tv_sec = (time_t) seconds;
    tv.tv_usec = (suseconds_t) ( ( seconds - (float) tv.tv_sec ) * 1000000.0f );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( tv.tv_usec >= 1000000 )
    {
        tv.tv_sec += 1;
        tv.tv_usec -= 1000000;
    }

    return tv;
}

uint32_t rocketnet_platform_timeout_milliseconds( float seconds )
{
    // Round positive timeouts up and cap before conversion, never requesting an infinite wait.
    const double milliseconds = ceil( (double) seconds * 1000.0 );
    if ( !( milliseconds > 1.0 ) )
        return 1;
    if ( milliseconds >= 4294967294.0 )
        return UINT32_C( 0xFFFFFFFE );
    return (uint32_t) milliseconds;
}

struct rocketnet_platform_sleep_split_t rocketnet_platform_sleep_split( double time )
{
    struct rocketnet_platform_sleep_split_t split;

    if ( !( time > 0.0 ) )
    {
        split.seconds = 0;
        split.microseconds = 0;
        return split;
    }

    split.seconds = (uint32_t) time;

    double frac = time - (double) split.seconds;
    split.microseconds = (uint32_t) ( frac * 1000000.0 );
    if ( split.microseconds >= 1000000 )
    {
        split.seconds += 1;
        split.microseconds -= 1000000;
    }

    return split;
}

struct rocketnet_platform_sleep_plan_t
{
    bool yield_only;
    int64_t due_time;
    uint32_t fallback_milliseconds;
};

#define ROCKETNET_PLATFORM_SLEEP_TAIL_SECONDS 0.00005
#define ROCKETNET_PLATFORM_SLEEP_MAX_DUE_UNITS 9.0e18
#define ROCKETNET_PLATFORM_SLEEP_MAX_FALLBACK_MILLISECONDS 0xFFFFFFFEu

rocketnet_platform_sleep_plan_t rocketnet_platform_sleep_plan( double time )
{
    rocketnet_platform_sleep_plan_t plan;
    plan.yield_only = !( time > 0.0 );
    plan.due_time = 0;
    plan.fallback_milliseconds = 0;
    if ( plan.yield_only )
        return plan;

    double units = ceil( time * 10000000.0 - 0.000001 );
    if ( units < 1.0 )
        units = 1.0;
    if ( units > ROCKETNET_PLATFORM_SLEEP_MAX_DUE_UNITS )
        units = ROCKETNET_PLATFORM_SLEEP_MAX_DUE_UNITS;
    plan.due_time = -(int64_t) units;

    double milliseconds = ceil( time * 1000.0 - 0.000001 );
    if ( milliseconds < 1.0 )
        milliseconds = 1.0;
    if ( milliseconds > (double) ROCKETNET_PLATFORM_SLEEP_MAX_FALLBACK_MILLISECONDS )
        milliseconds = (double) ROCKETNET_PLATFORM_SLEEP_MAX_FALLBACK_MILLISECONDS;
    plan.fallback_milliseconds = (uint32_t) milliseconds;

    return plan;
}

// After the timer fires, the sleep yields (SwitchToThread) until the request
// is up only when what remains is positive and at most 50 us; it never spins.
bool rocketnet_platform_sleep_tail_yields( double remaining )
{
    return remaining > 0.0 && remaining <= ROCKETNET_PLATFORM_SLEEP_TAIL_SECONDS;
}

extern void * rocketnet_malloc( void * context, size_t bytes );
extern void rocketnet_free( void * context, void * p );

// test seam for the client bind-family choice: true unless a test turns it
// off, in which case an IPv6 socket create behaves as it does on a host with
// no IPv6 stack. the client's availability probe reads the same flag.
#if ROCKETNET_DEV
static bool rocketnet_platform_ipv6_socket_available_for_testing = true;

// test seam for the network lookups (rocketnet_internal.h): NULL runs the real
// IPv6 probe; a unit test's table replaces it.
static std::atomic<const rocketnet_platform_network_lookups_t *> rocketnet_platform_network_lookups_override{ NULL };

void rocketnet_platform_set_network_lookups_for_testing( const rocketnet_platform_network_lookups_t * lookups )
{
    rocketnet_platform_network_lookups_override.store( lookups, std::memory_order_release );
}
#endif // #if ROCKETNET_DEV

// Network-thread priority (docs/CLIENT-IO.md, TRANSPORTS.md). The calls and their
// values are the flow library's (flow_platform.cpp), unchanged. DEV builds
// allow a unit test to replace the real calls through a table; production
// calls the real wrappers directly. A refused call is counted process-wide and
// logged once per call kind; it never fails the caller.

static std::atomic<bool> rocketnet_platform_thread_priority_on{ true };
static std::atomic<uint64_t> rocketnet_platform_thread_priority_refusal_count{ 0 };
static std::atomic<bool> rocketnet_platform_thread_priority_logged[ROCKETNET_PLATFORM_PRIORITY_CALL_COUNT];
#if ROCKETNET_DEV
static std::atomic<const rocketnet_platform_priority_calls_t *> rocketnet_platform_priority_calls_override{ NULL };

static const rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls()
{
    const rocketnet_platform_priority_calls_t * calls = rocketnet_platform_priority_calls_override.load( std::memory_order_acquire );
    return calls ? calls : rocketnet_platform_priority_calls_real();
}

void rocketnet_platform_set_priority_calls_for_testing( const rocketnet_platform_priority_calls_t * calls )
{
    rocketnet_platform_priority_calls_override.store( calls, std::memory_order_release );
}

const rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls_current()
{
    return rocketnet_platform_priority_calls();
}

void rocketnet_platform_thread_priority_reset_logged_for_testing()
{
    for ( int i = 0; i < ROCKETNET_PLATFORM_PRIORITY_CALL_COUNT; ++i )
        rocketnet_platform_thread_priority_logged[i].store( false, std::memory_order_relaxed );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
#define ROCKETNET_PLATFORM_PRIORITY_CALL( name ) ( rocketnet_platform_priority_calls()->name )
#else
#define ROCKETNET_PLATFORM_PRIORITY_CALL( name ) rocketnet_platform_real_##name
#endif // #if ROCKETNET_DEV

void rocketnet_platform_set_thread_priority_enabled( bool enabled )
{
    rocketnet_platform_thread_priority_on.store( enabled, std::memory_order_relaxed );
}

bool rocketnet_platform_thread_priority_enabled()
{
    return rocketnet_platform_thread_priority_on.load( std::memory_order_relaxed );
}

void rocketnet_platform_thread_priority_refused( int kind, const char * prefix, const char * call, const char * reason, const char * effect )
{
    rocketnet_platform_thread_priority_refusal_count.fetch_add( 1, std::memory_order_relaxed );
    if ( kind < 0 || kind >= ROCKETNET_PLATFORM_PRIORITY_CALL_COUNT )
        return;
    if ( rocketnet_platform_thread_priority_logged[kind].exchange( true, std::memory_order_relaxed ) )
        return;
    rocketnet_warn( "%s: %s refused (%s); %s", prefix, call, reason, effect );
}

uint64_t rocketnet_platform_thread_priority_refusals()
{
    return rocketnet_platform_thread_priority_refusal_count.load( std::memory_order_relaxed );
}

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// POSIX: the one call, pthread_setschedparam( thread, SCHED_RR,
// rocketnet_platform_network_thread_rr_priority() ), on the created thread
static int rocketnet_platform_real_set_sched( rocketnet_platform_thread_t * thread, int policy, int priority )
{
    struct sched_param param;
    param.sched_priority = priority;
    return pthread_setschedparam( thread->handle, policy, &param );
}

#if ROCKETNET_DEV
static int rocketnet_platform_real_set_thread_priority( rocketnet_platform_thread_t * thread, int priority )
{
    (void) thread;
    (void) priority;
    return 1;
}

static unsigned int rocketnet_platform_real_time_period( unsigned int period )
{
    (void) period;
    return 0;
}

static void rocketnet_platform_real_timer_frequency( void * frequency )
{
    (void) frequency;
}
#endif // #if ROCKETNET_DEV

static int rocketnet_platform_real_mutex_init( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );

    memset( mutex, 0, sizeof( rocketnet_platform_mutex_t ) );

    pthread_mutexattr_t attr;
    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_NORMAL );

    int result = pthread_mutex_init( &mutex->handle, &attr );
    pthread_mutexattr_destroy( &attr );
    if ( result != 0 )
        return result;
    mutex->ok = true;

    return 0;
}

#if ROCKETNET_DEV
static int rocketnet_platform_real_platform_start( void )
{
    return 0;
}

static void * rocketnet_platform_real_create_waitable_timer( unsigned flags )
{
    return flags == 0x00000002u ? (void *) 1 : NULL;
}

static const rocketnet_platform_priority_calls_t rocketnet_platform_priority_calls_real_table = {
    rocketnet_platform_real_set_sched,
    rocketnet_platform_real_set_thread_priority,
    rocketnet_platform_real_time_period,
    rocketnet_platform_real_time_period,
    rocketnet_platform_real_timer_frequency,
    rocketnet_platform_real_mutex_init,
    rocketnet_platform_real_platform_start,
    rocketnet_platform_real_create_waitable_timer,
};

const rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls_real()
{
    return &rocketnet_platform_priority_calls_real_table;
}
#endif // #if ROCKETNET_DEV

// The network threads run above every ordinary thread. On Linux the SCHED_RR
// minimum (1) already does: ordinary threads are SCHED_OTHER at 0. On macOS
// the SCHED_RR range is 15 to 47 and an ordinary thread sits at 31, so the
// minimum demoted the client I/O thread and under CPU contention its sends and
// receives stalled for hundreds of milliseconds; there the maximum (47, the
// band of user-interactive threads) is the same intent. Elsewhere the minimum,
// raised to a default thread's priority where that is higher.

int rocketnet_platform_network_thread_rr_priority()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    return sched_get_priority_max( SCHED_RR );
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    int priority = sched_get_priority_min( SCHED_RR );

    pthread_attr_t attr;
    if ( pthread_attr_init( &attr ) == 0 )
    {
        struct sched_param param;
        if ( pthread_attr_getschedparam( &attr, &param ) == 0 && param.sched_priority > priority )
            priority = param.sched_priority;
        pthread_attr_destroy( &attr );
    }

    return priority;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
}

static void rocketnet_platform_network_thread_priority( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );

    if ( !rocketnet_platform_thread_priority_enabled() )
        return;
    const int priority = rocketnet_platform_network_thread_rr_priority();
    const int result = ROCKETNET_PLATFORM_PRIORITY_CALL( set_sched )( thread, SCHED_RR, priority );
    if ( result != 0 )
    {
        char call[64];
        snprintf( call, sizeof( call ), "pthread_setschedparam SCHED_RR %d", priority );
        rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_SCHED, "thread priority", call, strerror( result ), "the thread runs at its default priority" );
    }
}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// A send that answers -1 (SOCKET_ERROR on Windows) drops its datagram: the
// drop is counted on the socket that made it, so a caller can price what the
// kernel refused (ENOBUFS from a full interface queue, EAGAIN from a full send
// buffer, a refused destination). Atomic: the client's I/O thread and the
// main thread both read. send_failures counts each dropped datagram, and each
// refused sendmsg_x chunk once (its delivered prefix is indeterminate);
// send_replay_failures counts the refusals of that chunk's per-packet replay,
// which may resend datagrams the kernel had already delivered.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static inline void rocketnet_platform_note_send_failure( rocketnet_platform_socket_t * socket )
{
    rocketnet_platform_atomic_add_u64( &socket->send_failures, 1 );
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

uint64_t rocketnet_platform_socket_send_failures( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    return rocketnet_platform_atomic_load_u64( &socket->send_failures );
}

#if ROCKETNET_DEV
uint64_t rocketnet_platform_socket_send_replay_failures( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    return rocketnet_platform_atomic_load_u64( &socket->send_replay_failures );
}
#endif // #if ROCKETNET_DEV

// Send failures by errno (rocketnet_platform.h). The counts are relaxed
// atomics: the sending worker adds, the destroy that logs them reads after it
// has stopped. The first failure of a call and kind is the add that saw zero.

int rocketnet_platform_send_errno_kind( int error )
{
    if ( error == 0 )
        return ROCKETNET_PLATFORM_SEND_ERRNO_NONE;
    if ( error == EAGAIN || error == EWOULDBLOCK )
        return ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN;
    if ( error == ENOBUFS )
        return ROCKETNET_PLATFORM_SEND_ERRNO_ENOBUFS;
    if ( error == EHOSTUNREACH )
        return ROCKETNET_PLATFORM_SEND_ERRNO_EHOSTUNREACH;
    if ( error == ENETUNREACH )
        return ROCKETNET_PLATFORM_SEND_ERRNO_ENETUNREACH;
    if ( error == ECONNREFUSED )
        return ROCKETNET_PLATFORM_SEND_ERRNO_ECONNREFUSED;
#if defined( EHOSTDOWN )
    if ( error == EHOSTDOWN )
        return ROCKETNET_PLATFORM_SEND_ERRNO_EHOSTDOWN;
#endif // #if defined( EHOSTDOWN )
    if ( error == ENETDOWN )
        return ROCKETNET_PLATFORM_SEND_ERRNO_ENETDOWN;
    if ( error == EMSGSIZE )
        return ROCKETNET_PLATFORM_SEND_ERRNO_EMSGSIZE;
    if ( error == EADDRNOTAVAIL )
        return ROCKETNET_PLATFORM_SEND_ERRNO_EADDRNOTAVAIL;
    if ( error == EPERM || error == EACCES )
        return ROCKETNET_PLATFORM_SEND_ERRNO_EPERM;
    if ( error == EINTR )
        return ROCKETNET_PLATFORM_SEND_ERRNO_EINTR;
    return ROCKETNET_PLATFORM_SEND_ERRNO_OTHER;
}

const char * rocketnet_platform_send_errno_kind_name( int kind )
{
    static const char * const names[ROCKETNET_PLATFORM_SEND_ERRNO_KINDS] = {
        "none", "EAGAIN", "ENOBUFS", "EHOSTUNREACH", "ENETUNREACH", "ECONNREFUSED", "EHOSTDOWN",
        "ENETDOWN", "EMSGSIZE", "EADDRNOTAVAIL", "EPERM", "EINTR", "other"
    };
    if ( kind < 0 || kind >= ROCKETNET_PLATFORM_SEND_ERRNO_KINDS )
        return "unknown";
    return names[kind];
}

const char * rocketnet_platform_send_call_name( int call )
{
    static const char * const names[ROCKETNET_PLATFORM_SEND_CALLS] = { "sendto", "sendmsg_x", "replay sendto" };
    if ( call < 0 || call >= ROCKETNET_PLATFORM_SEND_CALLS )
        return "unknown";
    return names[call];
}

bool rocketnet_platform_send_errnos_count( rocketnet_platform_send_errnos_t * errnos, int call, int error )
{
    rocketnet_assert( errnos );
    rocketnet_assert( call >= 0 && call < ROCKETNET_PLATFORM_SEND_CALLS );

    const int kind = rocketnet_platform_send_errno_kind( error );
    const bool first = rocketnet_platform_atomic_add_u64( &errnos->counts[call][kind], 1 ) == 0;
    if ( first && kind == ROCKETNET_PLATFORM_SEND_ERRNO_OTHER )
        rocketnet_platform_atomic_add_u64( &errnos->other_error, (uint64_t) (unsigned int) error );
    return first;
}

uint64_t rocketnet_platform_send_errnos_read( rocketnet_platform_send_errnos_t * errnos, int call, int kind )
{
    rocketnet_assert( errnos );
    if ( call < 0 || call >= ROCKETNET_PLATFORM_SEND_CALLS || kind < 0 || kind >= ROCKETNET_PLATFORM_SEND_ERRNO_KINDS )
        return 0;
    return rocketnet_platform_atomic_load_u64( &errnos->counts[call][kind] );
}

int rocketnet_platform_send_errnos_format( rocketnet_platform_send_errnos_t * errnos, char * buffer, size_t size )
{
    rocketnet_assert( errnos );
    rocketnet_assert( buffer );

    if ( size == 0 )
        return 0;
    buffer[0] = '\0';

    size_t used = 0;
    int written = 0;

    for ( int call = 0; call < ROCKETNET_PLATFORM_SEND_CALLS; call++ )
    {
        for ( int kind = 0; kind < ROCKETNET_PLATFORM_SEND_ERRNO_KINDS; kind++ )
        {
            const uint64_t count = rocketnet_platform_send_errnos_read( errnos, call, kind );
            if ( count == 0 )
                continue;
            const int result = snprintf( buffer + used, size - used, "%s%s %s=%llu", written ? " " : "",
                                         rocketnet_platform_send_call_name( call ), rocketnet_platform_send_errno_kind_name( kind ),
                                         (unsigned long long) count );
            if ( result < 0 || (size_t) result >= size - used )
            {
                // truncated: keep the whole words that fit, still terminated
                buffer[used] = '\0';
                return written;
            }

            used += (size_t) result;
            written++;
        }
    }

    return written;
}

// Test-only receive seam, compiled in under ROCKETNET_DEV only. While a hook is set for one socket,
// the single-packet receive path on that socket takes each datagram from the
// hook instead of the kernel, so a unit case drives the drop-and-retry of a
// refused source with no socket. Other sockets never see the hook.

#if ROCKETNET_DEV

static rocketnet_platform_socket_t * rocketnet_platform_receive_hook_socket = NULL;
static rocketnet_platform_receive_hook_t rocketnet_platform_receive_hook_function = NULL;
static void * rocketnet_platform_receive_hook_user = NULL;

void rocketnet_platform_socket_set_receive_hook( rocketnet_platform_socket_t * socket, rocketnet_platform_receive_hook_t hook, void * user )
{
    rocketnet_platform_receive_hook_socket = hook ? socket : NULL;
    rocketnet_platform_receive_hook_function = hook;
    rocketnet_platform_receive_hook_user = user;
}

static bool rocketnet_platform_receive_hooked( rocketnet_platform_socket_t * socket, void * packet_data, int max_packet_size, struct sockaddr_storage * from, size_t from_bytes, int * result )
{
    if ( rocketnet_platform_receive_hook_function == NULL || socket != rocketnet_platform_receive_hook_socket )
        return false;
    memset( from, 0, from_bytes );
    *result = rocketnet_platform_receive_hook_function( rocketnet_platform_receive_hook_user, packet_data, max_packet_size, from );

    return true;
}

#endif // #if ROCKETNET_DEV
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
// The file status flags a non-blocking socket is set to: the descriptor's
// current flags (fcntl F_GETFL) with O_NONBLOCK added. F_SETFL replaces the
// flags it is given, so the socket create paths pass this merge rather than
// O_NONBLOCK alone.

int rocketnet_platform_nonblocking_flags( int flags )
{
    return flags | O_NONBLOCK;
}
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
// Test-only send-failure seam, compiled into the development library only.
// The next N sendto and sendmsg_x calls on one socket report the chosen errno
// without reaching the kernel, so a functional case can drive the send error
// arms. A registry of live sockets lets a case find a transport's socket. The
// send path pays one relaxed load while nothing is armed.

#if ROCKETNET_DEV && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#define ROCKETNET_PLATFORM_TEST_SOCKETS 256

struct rocketnet_platform_test_socket_t
{
    rocketnet_platform_socket_t * socket;
    int fail_remaining;
    int fail_error;
};

static rocketnet_platform_test_socket_t rocketnet_platform_test_sockets[ROCKETNET_PLATFORM_TEST_SOCKETS];
static int rocketnet_platform_test_num_sockets = 0;
static int rocketnet_platform_test_armed = 0;
static int rocketnet_platform_test_lock = 0;

static void rocketnet_platform_test_acquire()
{
    while ( __atomic_exchange_n( &rocketnet_platform_test_lock, 1, __ATOMIC_ACQUIRE ) != 0 )
    {
    }
}

static void rocketnet_platform_test_release()
{
    __atomic_store_n( &rocketnet_platform_test_lock, 0, __ATOMIC_RELEASE );
}

static void rocketnet_platform_test_recount_armed()
{
    int armed = 0;
    for ( int i = 0; i < rocketnet_platform_test_num_sockets; i++ )
        armed += rocketnet_platform_test_sockets[i].fail_remaining;
    __atomic_store_n( &rocketnet_platform_test_armed, armed, __ATOMIC_RELAXED );
}

static void rocketnet_platform_test_register( rocketnet_platform_socket_t * socket )
{
    rocketnet_platform_test_acquire();

    if ( rocketnet_platform_test_num_sockets < ROCKETNET_PLATFORM_TEST_SOCKETS )
    {
        rocketnet_platform_test_socket_t * entry = &rocketnet_platform_test_sockets[rocketnet_platform_test_num_sockets++];
        entry->socket = socket;
        entry->fail_remaining = 0;
        entry->fail_error = 0;
    }

    rocketnet_platform_test_release();
}

static void rocketnet_platform_test_unregister( rocketnet_platform_socket_t * socket )
{
    rocketnet_platform_test_acquire();

    for ( int i = 0; i < rocketnet_platform_test_num_sockets; i++ )
    {
        if ( rocketnet_platform_test_sockets[i].socket != socket )
            continue;
        for ( int j = i + 1; j < rocketnet_platform_test_num_sockets; j++ )
            rocketnet_platform_test_sockets[j - 1] = rocketnet_platform_test_sockets[j];
        rocketnet_platform_test_num_sockets--;
        break;
    }

    rocketnet_platform_test_recount_armed();
    rocketnet_platform_test_release();
}

int rocketnet_platform_sockets_for_testing( rocketnet_platform_socket_t ** sockets, int max_sockets )
{
    rocketnet_platform_test_acquire();

    int count = 0;
    for ( int i = 0; i < rocketnet_platform_test_num_sockets && count < max_sockets; i++ )
        sockets[count++] = rocketnet_platform_test_sockets[i].socket;
    rocketnet_platform_test_release();

    return count;
}

void rocketnet_platform_socket_fail_next_sends( rocketnet_platform_socket_t * socket, int count, int error )
{
    rocketnet_platform_test_acquire();

    for ( int i = 0; i < rocketnet_platform_test_num_sockets; i++ )
    {
        if ( rocketnet_platform_test_sockets[i].socket != socket )
            continue;
        rocketnet_platform_test_sockets[i].fail_remaining = count > 0 ? count : 0;
        rocketnet_platform_test_sockets[i].fail_error = error;
    }

    rocketnet_platform_test_recount_armed();
    rocketnet_platform_test_release();
}

// Test-only create seam: a callback between the bind and the first batch
// decision, so a functional case can put a datagram on a fixed-port bind
// before anything reads it. Development library only, like the sends seam.

typedef void ( *rocketnet_platform_create_receive_hook_t )( int handle );

static rocketnet_platform_create_receive_hook_t rocketnet_platform_create_receive_hook = NULL;

void rocketnet_platform_set_create_receive_hook( rocketnet_platform_create_receive_hook_t hook )
{
    rocketnet_platform_create_receive_hook = hook;
}

static bool rocketnet_platform_send_failure_should_inject( rocketnet_platform_socket_t * socket, int * error )
{
    if ( __atomic_load_n( &rocketnet_platform_test_armed, __ATOMIC_RELAXED ) == 0 )
        return false;
    bool inject = false;
    rocketnet_platform_test_acquire();

    for ( int i = 0; i < rocketnet_platform_test_num_sockets; i++ )
    {
        rocketnet_platform_test_socket_t * entry = &rocketnet_platform_test_sockets[i];
        if ( entry->socket != socket || entry->fail_remaining <= 0 )
            continue;
        entry->fail_remaining--;
        *error = entry->fail_error;
        inject = true;
        break;
    }

    rocketnet_platform_test_recount_armed();
    rocketnet_platform_test_release();

    return inject;
}

#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// ---------------------------------------------------

// Server receive buffer sizing (rocketnet_internal.h, USAGE.md "Receive
// buffer sizing"). A server updating at update_hz must hold one update's
// inbound packets at max_clients in its socket: the portable transport's
// receive worker stops reading while every receive bank waits for the next
// update (proofs/tla/RNPortableRx.tla HoldOnFull), so the kernel holds the rest.

int rocketnet_server_receive_buffer_bytes( int max_clients, int receive_kbps_per_client, int update_hz )
{
    const uint64_t clients = max_clients > 1 ? (uint64_t) max_clients : 1u;
    const uint64_t kbps = receive_kbps_per_client > 1 ? (uint64_t) receive_kbps_per_client : 1u;
    const uint64_t hz = update_hz > 1 ? (uint64_t) update_hz : 1u;
    const uint64_t bytes_per_second_per_kbps = 125u;
    const uint64_t scale = bytes_per_second_per_kbps * (uint64_t) ROCKETNET_SERVER_RECEIVE_BUFFER_MARGIN;
    const uint64_t rate = clients * kbps; // both below 2^31: no overflow
    if ( rate > UINT64_MAX / scale )
        return INT_MAX;
    uint64_t bytes = ( rate * scale + hz - 1u ) / hz;
    if ( bytes < (uint64_t) ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE )
        bytes = (uint64_t) ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE;
    if ( bytes > (uint64_t) INT_MAX )
        bytes = (uint64_t) INT_MAX;
    return (int) bytes;
}

int rocketnet_server_receive_buffer_request( int max_clients )
{
    return rocketnet_server_receive_buffer_bytes( max_clients, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, ROCKETNET_SERVER_RECEIVE_MIN_UPDATE_HZ );
}

#if ROCKETNET_DEV
static thread_local int rocketnet_server_receive_buffer_warning_grant_override = 0;

void rocketnet_server_receive_buffer_warning_grant_override_for_testing( int granted_bytes )
{
    rocketnet_server_receive_buffer_warning_grant_override = granted_bytes;
}
#endif // #if ROCKETNET_DEV

void rocketnet_server_receive_buffer_granted( const char * name, int requested_bytes, int granted_bytes )
{
#if ROCKETNET_DEV
    int warning_granted_bytes = granted_bytes;
    bool warning_grant_overridden = false;

    if ( rocketnet_server_receive_buffer_warning_grant_override > 0 )
    {
        rocketnet_info( "server %s: socket receive buffer actual grant %d of %d requested bytes; DEV test warning grant override %d bytes %s",
                        name ? name : "socket", granted_bytes, requested_bytes,
                        rocketnet_server_receive_buffer_warning_grant_override,
                        granted_bytes >= requested_bytes ? "applied" : "not needed" );
        if ( granted_bytes >= requested_bytes )
        {
            warning_granted_bytes = rocketnet_server_receive_buffer_warning_grant_override;
            warning_grant_overridden = true;
        }

        rocketnet_server_receive_buffer_warning_grant_override = 0;
    }
#else
    const int warning_granted_bytes = granted_bytes;
#endif
    // one line per server socket: what the kernel granted and what was asked
    // for. A grant below the request is a warning that names the limit to
    // raise, because the kernel drops what arrives past the grant.
    if ( warning_granted_bytes >= requested_bytes )
    {
        rocketnet_info( "server %s: socket receive buffer granted %d of %d requested bytes",
                        name ? name : "socket", warning_granted_bytes, requested_bytes );
        return;
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    const char * limit = "net.core.rmem_max";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const char * limit = "the system socket buffer limit";
#else
    const char * limit = "kern.ipc.maxsockbuf";
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#if ROCKETNET_DEV
    const char * warning_reason = warning_grant_overridden ? "DEV test warning grant override; actual socket grant was reported above" : "packets arriving between updates past the grant are dropped by the kernel";
#else
    const char * warning_reason = "packets arriving between updates past the grant are dropped by the kernel";
#endif // #if ROCKETNET_DEV
    rocketnet_warn( "server %s: socket receive buffer granted %d of %d requested bytes; %s: raise %s",
                    name ? name : "socket", warning_granted_bytes, requested_bytes, warning_reason, limit );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS // POSIX

// linux, the BSDs, macOS and iOS: one POSIX section. What differs per system
// sits inside it: the clock and the thread wake (mach on darwin), the client
// wake (eventfd, pipe, kqueue), the batch pump (sendmmsg/recvmmsg on linux,
// sendmsg_x/recvmsg_x on macOS) and a handful of socket options.

#if defined( __APPLE__ )
#define __APPLE_USE_RFC_3542
#endif // #if defined( __APPLE__ )

#include "rocketnet_address.h"

#include <limits.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <poll.h>
#include <time.h>

#if defined( __linux__ )
#include <sys/eventfd.h>
#endif // #if defined( __linux__ )

#if defined( __APPLE__ )
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <sys/event.h>
#endif // #if defined( __APPLE__ )

// the BSDs have no CLOCK_MONOTONIC_RAW; CLOCK_MONOTONIC is their
// unadjusted-rate monotonic clock

#if !defined( __APPLE__ ) && !defined( CLOCK_MONOTONIC_RAW )
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif // #if !defined( __APPLE__ ) && !defined( CLOCK_MONOTONIC_RAW )

// ---------------------------------------------------

#if defined( __APPLE__ )

// The darwin batch pump: sendmsg_x/recvmsg_x, private but stable. They are
// resolved by dlsym at init, never linked, and each socket's first batch call
// is its probe: a refusal degrades that socket to the per-packet loop without
// losing a packet. iOS never resolves them (a private symbol is an App Store
// finding), so iOS is portable by construction. struct msghdr_x is the
// syscall ABI from xnu bsd/sys/socket_private.h.

struct rocketnet_msghdr_x_t
{
    void * msg_name;
    socklen_t msg_namelen;
    struct iovec * msg_iov;
    int msg_iovlen;
    void * msg_control;
    socklen_t msg_controllen;
    int msg_flags;
    size_t msg_datalen;
};

typedef ssize_t ( *rocketnet_sendmsg_x_fn )( int s, const struct rocketnet_msghdr_x_t * msgp, u_int cnt, int flags );
typedef ssize_t ( *rocketnet_recvmsg_x_fn )( int s, struct rocketnet_msghdr_x_t * msgp, u_int cnt, int flags );

static rocketnet_sendmsg_x_fn rocketnet_sendmsg_x = NULL;
static rocketnet_recvmsg_x_fn rocketnet_recvmsg_x = NULL;

#endif // #if defined( __APPLE__ )

// ---------------------------------------------------

#if defined( __APPLE__ )
static mach_timebase_info_data_t timebase_info;
static uint64_t time_start;
#else // #if defined( __APPLE__ )
static double time_start;
#endif // #if defined( __APPLE__ )

bool rocketnet_platform_init()
{
#if ROCKETNET_DEV
    if ( ROCKETNET_PLATFORM_PRIORITY_CALL( platform_start )() != 0 )
        return false;
#endif // #if ROCKETNET_DEV

#if defined( __APPLE__ )
    mach_timebase_info( &timebase_info );
    time_start = mach_absolute_time();
#else // #if defined( __APPLE__ )
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    time_start = (double) ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
#endif // #if defined( __APPLE__ )

#if defined( __APPLE__ ) && TARGET_OS_OSX
    // absence is not an error: a socket without them runs the portable pump
    rocketnet_sendmsg_x = (rocketnet_sendmsg_x_fn) dlsym( RTLD_DEFAULT, "sendmsg_x" );
    rocketnet_recvmsg_x = (rocketnet_recvmsg_x_fn) dlsym( RTLD_DEFAULT, "recvmsg_x" );
#endif // #if defined( __APPLE__ ) && TARGET_OS_OSX

    return true;
}

void rocketnet_platform_term()
{
}

int rocketnet_platform_getenv( const char * var, char * buffer, size_t size )
{
    rocketnet_assert( var && buffer && size > 0 );
#if ROCKETNET_DEV
    int test_result = 0;
    if ( rocketnet_platform_test_environment_getenv( var, buffer, size, &test_result ) )
        return test_result;
#endif // #if ROCKETNET_DEV
    buffer[0] = '\0';

#if defined( __GLIBC__ ) && defined( _GNU_SOURCE )
    // secure_getenv returns NULL in a setuid/setgid process: configuration
    // never comes from an environment an unprivileged caller controls.
    const char * value = secure_getenv( var );
#else
    const char * value = getenv( var );
#endif
    if ( !value )
        return 0;
    const size_t length = strlen( value );
    if ( length >= size )
        return -1;
    memcpy( buffer, value, length + 1 );

    return 1;
}

int rocketnet_platform_id()
{
    return ROCKETNET_PLATFORM;
}

bool rocketnet_platform_client_dual_stack()
{
#if defined( __OpenBSD__ )
    // openbsd rejects v4-mapped addresses by design: no socket is ever dual
    // stack, so the client transport binds the server's family instead
    return false;
#else // #if defined( __OpenBSD__ )
    return true;
#endif // #if defined( __OpenBSD__ )
}

double rocketnet_platform_time()
{
#if defined( __APPLE__ )
    uint64_t current = mach_absolute_time();
    return ( (double) ( current - time_start ) ) * ( (double) timebase_info.numer ) / ( (double) timebase_info.denom ) / 1000000000.0;
#else // #if defined( __APPLE__ )
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    double current = (double) ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    return current - time_start;
#endif // #if defined( __APPLE__ )
}

void rocketnet_platform_sleep( double time )
{
    if ( !( time > 0.0 ) )
        return;
    const rocketnet_platform_sleep_split_t split = rocketnet_platform_sleep_split( time );
    if ( split.seconds > 0 )
        sleep( split.seconds );
    if ( split.microseconds > 0 )
        usleep( split.microseconds );
}

// ---------------------------------------------------

// address <-> sockaddr. Every send, bind, connect and receive converts through
// the platform-wide functions at the end of this file.

static bool rocketnet_platform_socket_dual_stack( const rocketnet_platform_socket_t * socket )
{
#if defined( __OpenBSD__ )
    // ipv6 sockets stay ipv6-only here, so a v4-mapped source is spoofed
    (void) socket;
    return false;
#else // #if defined( __OpenBSD__ )
    return socket->ipv6;
#endif // #if defined( __OpenBSD__ )
}

// a received source: false for a family the transport does not speak, or a
// source the socket must drop (rocketnet_address_accept_received_source)

bool rocketnet_platform_mmsg_read_sockaddr( rocketnet_platform_socket_t * socket, const sockaddr_storage * storage, rocketnet_address_t * from )
{
    return rocketnet_platform_sockaddr_to_address( storage, rocketnet_platform_socket_dual_stack( socket ), from );
}

// ---------------------------------------------------

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket );

// linux clamps an over-limit buffer request silently; the BSDs and darwin
// refuse it. On refusal walk down by halves to what the kernel grants, and
// fail only when nothing can be set. warn names a grant below the request.

static bool rocketnet_platform_set_socket_buffer_size( int handle, int option, int requested_bytes, const char * direction, bool warn )
{
    int size = requested_bytes;
    if ( setsockopt( handle, SOL_SOCKET, option, (char *) ( &size ), sizeof( int ) ) == 0 )
        return true;
    while ( size > 1 )
    {
        size /= 2;
        if ( setsockopt( handle, SOL_SOCKET, option, (char *) ( &size ), sizeof( int ) ) == 0 )
        {
            if ( warn )
                rocketnet_warn( "socket %s buffer request of %d refused by the kernel, granted %d", direction, requested_bytes, size );
            return true;
        }
    }

    return false;
}

int rocketnet_platform_socket_receive_buffer_bytes( rocketnet_platform_socket_t * socket )
{
    if ( !socket )
        return 0;
    int granted = 0;
    socklen_t length = sizeof( granted );
    if ( getsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char *) ( &granted ), &length ) != 0 )
        return 0;
#if defined( __linux__ )
    // linux doubles the request for its bookkeeping and reports the double
    granted /= 2;
#endif // #if defined( __linux__ )
    return granted;
}

// Darwin picks an ephemeral port for an IPv6 wildcard without checking that
// an IPv4 socket already owns it; both binds then coexist and IPv4 traffic
// goes to the other socket. Reserving the port in the IPv4 namespace first,
// and binding the IPv6 wildcard to it while the reservation is held, makes the
// dual-stack socket own both. Returns the reservation (closed by the caller
// after the bind), -1 when none is needed, or -2 on failure.

#if defined( __APPLE__ )
// the reservations a dual-stack bind to a kernel-chosen port takes before it
// gives up: each one a different port, refused only when that port is already
// bound in the IPv6 namespace
#define ROCKETNET_PLATFORM_DUAL_STACK_BIND_ATTEMPTS 16

static int rocketnet_platform_reserve_ipv4_port( const rocketnet_address_t * address, uint16_t * bind_port )
{
    if ( address->type != ROCKETNET_ADDRESS_IPV6 )
        return -1;
    for ( int i = 0; i < 8; i++ )
    {
        if ( address->data.ipv6[i] != 0 )
            return -1;
    }

    const int reservation = ::socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( reservation < 0 )
    {
        rocketnet_error( "failed to create ipv4 port reservation for dual stack socket" );
        return -2;
    }

    sockaddr_in reservation_address;
    memset( &reservation_address, 0, sizeof( reservation_address ) );
    reservation_address.sin_family = AF_INET;
    reservation_address.sin_addr.s_addr = INADDR_ANY;
    reservation_address.sin_port = rocketnet_platform_htons( *bind_port );
    if ( bind( reservation, (sockaddr *) &reservation_address, sizeof( reservation_address ) ) < 0 )
    {
        rocketnet_error( "failed to reserve ipv4 port for dual stack socket" );
        close( reservation );
        return -2;
    }

    if ( *bind_port == 0 )
    {
        socklen_t reservation_length = sizeof( reservation_address );
        if ( getsockname( reservation, (sockaddr *) &reservation_address, &reservation_length ) < 0 )
        {
            rocketnet_error( "failed to get reserved ipv4 port for dual stack socket" );
            close( reservation );
            return -2;
        }

        *bind_port = rocketnet_platform_ntohs( reservation_address.sin_port );
    }

    return reservation;
}
#endif // #if defined( __APPLE__ )

// batch_pump false suppresses the darwin batch pump for this socket (a pinned
// "portable" transport). Nothing is read at create: the first real batch call
// is the probe and delivers what it reads.

static rocketnet_platform_socket_t * rocketnet_platform_socket_create_internal( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, bool batch_pump, int * status )
{
    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    rocketnet_platform_socket_t * socket = (rocketnet_platform_socket_t *) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );
    if ( !socket )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    memset( socket, 0, sizeof( rocketnet_platform_socket_t ) );
    socket->context = context;
    socket->type = socket_type;
    socket->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;

#if ROCKETNET_DEV
    if ( address->type == ROCKETNET_ADDRESS_IPV6 && !rocketnet_platform_ipv6_socket_available_for_testing )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }
#endif // #if ROCKETNET_DEV

    socket->handle = ::socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( socket->handle < 0 )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }

#if defined( __APPLE__ )
    // a darwin UDP send can raise SIGPIPE: keep it a counted send failure on
    // this socket rather than a signal to the embedding application
    const int no_sigpipe = 1;
    if ( setsockopt( socket->handle, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof( no_sigpipe ) ) != 0 )
    {
        rocketnet_error( "failed to suppress socket SIGPIPE" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }
#endif // #if defined( __APPLE__ )

    // dual stack: openbsd forbids clearing IPV6_V6ONLY, so there an ipv6
    // socket stays ipv6-only (rocketnet_platform_client_dual_stack)

#if !defined( __OpenBSD__ )
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( socket->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char *) ( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
#endif // #if !defined( __OpenBSD__ )

#if defined( __APPLE__ )
    const bool send_buffer_set = setsockopt( socket->handle, SOL_SOCKET, SO_SNDBUF, (char *) ( &send_buffer_size ), sizeof( int ) ) == 0;
    const bool warn_on_walk = false;
#else // #if defined( __APPLE__ )
    const bool send_buffer_set = rocketnet_platform_set_socket_buffer_size( socket->handle, SO_SNDBUF, send_buffer_size, "send", true );
    const bool warn_on_walk = true;
#endif // #if defined( __APPLE__ )

    if ( !send_buffer_set )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    // a server's receive request is sized from its max_clients and may pass
    // the kernel limit; the server reads the grant back and names the limit
    // (rocketnet_server_receive_buffer_granted)

    if ( !rocketnet_platform_set_socket_buffer_size( socket->handle, SO_RCVBUF, receive_buffer_size, "receive", warn_on_walk ) )
    {
        rocketnet_error( "failed to set socket receive buffer size" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    // bind, and when bound to port 0 read back the port the kernel chose

    rocketnet_address_t bind_address = *address;
    sockaddr_storage bind_storage;
    int bind_result = -1;

#if defined( __APPLE__ )
    // The port an IPv4 reservation takes from the kernel is free in the IPv4
    // namespace only: another socket may hold it in the IPv6 namespace, and the
    // dual-stack bind to it then fails EADDRINUSE. A kernel-chosen port that is
    // refused so is held, and the next reservation takes another port; every
    // reservation is closed once the bind is done.

    int held[ROCKETNET_PLATFORM_DUAL_STACK_BIND_ATTEMPTS];
    int num_held = 0;
    int ipv4_port_reservation;

    while ( true )
    {
        bind_address.port = address->port;
        ipv4_port_reservation = rocketnet_platform_reserve_ipv4_port( address, &bind_address.port );
        if ( ipv4_port_reservation == -2 )
            break;

        const socklen_t bind_bytes = (socklen_t) rocketnet_platform_sockaddr_from_address( &bind_address, &bind_storage );
        bind_result = bind( socket->handle, (sockaddr *) &bind_storage, bind_bytes );
        if ( bind_result >= 0 || ipv4_port_reservation < 0 || address->port != 0 || errno != EADDRINUSE ||
             num_held == ROCKETNET_PLATFORM_DUAL_STACK_BIND_ATTEMPTS - 1 )
            break;

        held[num_held++] = ipv4_port_reservation;
    }

    for ( int i = 0; i < num_held; i++ )
        close( held[i] );

    if ( ipv4_port_reservation == -2 )
    {
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    if ( ipv4_port_reservation >= 0 )
    {
        close( ipv4_port_reservation );
        if ( bind_result >= 0 )
            address->port = bind_address.port;
    }
#else // #if defined( __APPLE__ )
    const socklen_t bind_bytes = (socklen_t) rocketnet_platform_sockaddr_from_address( &bind_address, &bind_storage );
    bind_result = bind( socket->handle, (sockaddr *) &bind_storage, bind_bytes );
#endif // #if defined( __APPLE__ )

    if ( bind_result < 0 )
    {
        rocketnet_error( "failed to bind socket (%s)", socket->ipv6 ? "ipv6" : "ipv4" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    if ( address->port == 0 )
    {
        sockaddr_storage bound;
        socklen_t bound_bytes = sizeof( bound );
        if ( getsockname( socket->handle, (sockaddr *) &bound, &bound_bytes ) == -1 )
        {
            rocketnet_error( "failed to get socket port (%s)", socket->ipv6 ? "ipv6" : "ipv4" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }

        address->port = bound.ss_family == AF_INET6 ? rocketnet_platform_ntohs( ( (sockaddr_in6 *) &bound )->sin6_port )
                                                    : rocketnet_platform_ntohs( ( (sockaddr_in *) &bound )->sin_port );
    }

    if ( socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        const int flags = fcntl( socket->handle, F_GETFL, 0 );
        if ( flags == -1 || fcntl( socket->handle, F_SETFL, rocketnet_platform_nonblocking_flags( flags ) ) == -1 )
        {
            rocketnet_error( "failed to set socket to non-blocking" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else if ( timeout_seconds > 0.0f )
    {
        struct timeval tv = rocketnet_platform_timeval_from_seconds( timeout_seconds );
        if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) ) < 0 )
        {
            rocketnet_error( "failed to set socket receive timeout" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // don't fragment: linux through PMTU discovery, everything else through
    // the RFC 3542 options where the system has them. A refusal is tolerated.

    int dont_fragment = 1;
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
#if defined( __linux__ )
        dont_fragment = IPV6_PMTUDISC_DO;
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &dont_fragment, sizeof( dont_fragment ) );
#elif defined( IPV6_DONTFRAG )
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_DONTFRAG, &dont_fragment, sizeof( dont_fragment ) );
#endif // #if defined( __linux__ )
    }
    else
    {
#if defined( __linux__ )
        dont_fragment = IP_PMTUDISC_DO;
        setsockopt( socket->handle, IPPROTO_IP, IP_MTU_DISCOVER, &dont_fragment, sizeof( dont_fragment ) );
#elif defined( IP_DONTFRAG )
        setsockopt( socket->handle, IPPROTO_IP, IP_DONTFRAG, &dont_fragment, sizeof( dont_fragment ) );
#endif // #if defined( __linux__ )
    }

#if defined( __APPLE__ ) && TARGET_OS_OSX
#if ROCKETNET_DEV
    if ( rocketnet_platform_create_receive_hook )
        rocketnet_platform_create_receive_hook( socket->handle );
#endif // #if ROCKETNET_DEV
    socket->msgx = batch_pump && rocketnet_sendmsg_x && rocketnet_recvmsg_x && socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING;
#else // #if defined( __APPLE__ ) && TARGET_OS_OSX
    (void) batch_pump;
#endif // #if defined( __APPLE__ ) && TARGET_OS_OSX

#if ROCKETNET_DEV && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    rocketnet_platform_test_register( socket );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

    return socket;
}

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, int * status )
{
    return rocketnet_platform_socket_create_internal( context, address, socket_type, timeout_seconds, send_buffer_size, receive_buffer_size, true, status );
}

bool rocketnet_platform_socket_close( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

    if ( rocketnet_platform_socket_handle_is_valid( socket->handle ) )
    {
        close( socket->handle );
        socket->handle = -1;
    }

    return true;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

#if ROCKETNET_DEV
    rocketnet_platform_test_unregister( socket );
#endif // #if ROCKETNET_DEV

#if defined( __APPLE__ )
    char failures[512];
    if ( rocketnet_platform_send_errnos_format( &socket->send_errnos, failures, sizeof( failures ) ) > 0 )
    {
        const uint64_t other_error = rocketnet_platform_atomic_load_u64( &socket->send_errnos.other_error );
        rocketnet_info( "socket send failures by errno: %s (first other errno %llu)", failures, (unsigned long long) other_error );
    }
#endif // #if defined( __APPLE__ )

    rocketnet_platform_socket_close( socket );
    rocketnet_free( socket->context, socket );
}

// A connected socket has one remote endpoint: every send goes to it with no
// destination named, since a BSD stack refuses a sendto that names one on a
// connected socket (EISCONN).

static inline ssize_t rocketnet_platform_sendto( rocketnet_platform_socket_t * socket, const void * packet_data, int packet_bytes, const sockaddr * to, socklen_t to_bytes )
{
    if ( socket->connected )
        return send( socket->handle, packet_data, (size_t) packet_bytes, 0 );
    return sendto( socket->handle, packet_data, (size_t) packet_bytes, 0, to, to_bytes );
}

// Connects the socket to its one remote endpoint, so the kernel drops every
// datagram from any other source (the client trusts only the server: STANDARD t11).

bool rocketnet_platform_socket_connect( rocketnet_platform_socket_t * socket, const rocketnet_address_t * to )
{
    rocketnet_assert( socket );
    rocketnet_assert( to );

    sockaddr_storage storage;
    const socklen_t storage_bytes = (socklen_t) rocketnet_platform_sockaddr_for( socket->ipv6, to, &storage );
    if ( storage_bytes == 0 )
        return false;

    if ( connect( socket->handle, (const sockaddr *) &storage, storage_bytes ) != 0 )
    {
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_address_to_string( to, address_string );
        rocketnet_error( "connect (%s) failed: %s", address_string, strerror( errno ) );
        return false;
    }

    socket->connected = true;

    return true;
}

// One failed send, counted in *failures (send_failures, or on darwin
// send_replay_failures for the replay of a refused sendmsg_x chunk). Darwin
// also counts by call and errno: the first of each logs, the destroy logs the
// totals.

static void rocketnet_platform_send_failed( rocketnet_platform_socket_t * socket, uint64_t * failures, int call, int error )
{
    rocketnet_platform_atomic_add_u64( failures, 1 );
#if defined( __APPLE__ )
    if ( rocketnet_platform_send_errnos_count( &socket->send_errnos, call, error ) )
    {
        rocketnet_info( "socket %s failed (%s, errno %d): first of its kind on this socket; later ones are counted and logged when the socket closes",
                        rocketnet_platform_send_call_name( call ), strerror( error ), error );
    }
#else // #if defined( __APPLE__ )
    (void) socket;
    (void) call;
    (void) error;
#endif // #if defined( __APPLE__ )
}

static void rocketnet_platform_send_one( rocketnet_platform_socket_t * socket, const rocketnet_address_t * to, const void * packet_data, int packet_bytes, uint64_t * failures )
{
    rocketnet_assert( socket );
    rocketnet_assert( to );
    rocketnet_assert( to->type == ROCKETNET_ADDRESS_IPV6 || to->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    const int call = failures == &socket->send_failures ? ROCKETNET_PLATFORM_SEND_CALL_SENDTO : ROCKETNET_PLATFORM_SEND_CALL_REPLAY;

    sockaddr_storage storage;
    const socklen_t storage_bytes = (socklen_t) rocketnet_platform_sockaddr_for( socket->ipv6, to, &storage );
    if ( storage_bytes == 0 )
    {
        rocketnet_error( "invalid address. could not send packet" );
        return;
    }

    int error = 0;
#if ROCKETNET_DEV
    if ( !rocketnet_platform_send_failure_should_inject( socket, &error ) )
#endif // #if ROCKETNET_DEV
    {
        if ( rocketnet_platform_sendto( socket, packet_data, packet_bytes, (const sockaddr *) &storage, storage_bytes ) >= 0 )
            return;
        error = errno;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_address_to_string( to, address_string );
    rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( error ) );
    rocketnet_platform_send_failed( socket, failures, call, error );
    errno = error;
}

void rocketnet_platform_socket_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * to, const void * packet_data, int packet_bytes )
{
    rocketnet_assert( socket );
    rocketnet_platform_send_one( socket, to, packet_data, packet_bytes, &socket->send_failures );
}

// ---------------------------------------------------

// UDP's ordinary weather on the receive path (TRANSPORTS.md, ICMP errors): a
// connected client socket gets an ICMP error (port or host unreachable) on its
// next receive, and a full buffer pool reports ENOBUFS. Each says nothing
// about the datagrams queued behind it, so a drain skips it and reads on.
// A weather errno can repeat, so the skips are bounded per call: the bound,
// not the kernel, guarantees every drain returns. EAGAIN ends the drain.

static bool rocketnet_platform_receive_weather( int error )
{
    return error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH ||
           error == EHOSTDOWN || error == ENETDOWN || error == ENOBUFS || error == EINTR;
}

#define ROCKETNET_PLATFORM_RECEIVE_WEATHER_SKIPS 64

#if ROCKETNET_DEV

// the next count receives on any socket return -1 with this errno instead of
// reading, leaving every queued datagram in place. A count of 1 is the
// one-shot form; an error of 0 clears it.

static std::atomic<int> rocketnet_platform_receive_errno_for_testing( 0 );
static std::atomic<int> rocketnet_platform_receive_errno_count_for_testing( 0 );

void rocketnet_platform_inject_receive_errno_repeat_for_testing( int error, int count )
{
    rocketnet_platform_receive_errno_for_testing.store( error );
    rocketnet_platform_receive_errno_count_for_testing.store( error != 0 && count > 0 ? count : 0 );
}

void rocketnet_platform_inject_receive_errno_for_testing( int error )
{
    rocketnet_platform_inject_receive_errno_repeat_for_testing( error, 1 );
}

int rocketnet_platform_receive_errno_remaining_for_testing()
{
    return rocketnet_platform_receive_errno_count_for_testing.load();
}

static int rocketnet_platform_take_receive_errno_for_testing()
{
    int remaining = rocketnet_platform_receive_errno_count_for_testing.load();
    while ( remaining > 0 )
    {
        if ( rocketnet_platform_receive_errno_count_for_testing.compare_exchange_weak( remaining, remaining - 1 ) )
            return rocketnet_platform_receive_errno_for_testing.load();
    }

    return 0;
}

#endif // #if ROCKETNET_DEV

// One datagram, its true size returned even when only a prefix fits:
// recvfrom's MSG_TRUNC reports it where the system supports it, and on
// darwin, which reports only the bytes copied, a second iovec into the
// caller's overflow area takes the tail. The transport's prefilter drops an
// oversize datagram as PREFILTER_TOO_LARGE.

static ssize_t rocketnet_platform_receive_one( rocketnet_platform_socket_t * socket, void * packet_data, int max_packet_size, uint8_t * overflow, size_t overflow_bytes, sockaddr_storage * from )
{
#if ROCKETNET_DEV
    const int injected = rocketnet_platform_take_receive_errno_for_testing();
    if ( injected != 0 )
    {
        errno = injected;
        return -1;
    }
#endif // #if ROCKETNET_DEV

#if defined( __APPLE__ )
    struct iovec iov[2];
    iov[0].iov_base = packet_data;
    iov[0].iov_len = (size_t) max_packet_size;
    iov[1].iov_base = overflow;
    iov[1].iov_len = overflow_bytes;

    struct msghdr msg;
    memset( &msg, 0, sizeof( msg ) );
    msg.msg_name = from;
    msg.msg_namelen = sizeof( sockaddr_storage );
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    return recvmsg( socket->handle, &msg, 0 );
#else // #if defined( __APPLE__ )
    (void) overflow;
    (void) overflow_bytes;
    socklen_t from_length = sizeof( sockaddr_storage );
    const int flags = ( socket->type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING ? MSG_DONTWAIT : 0 ) | MSG_TRUNC;
    return recvfrom( socket->handle, (char *) packet_data, (size_t) max_packet_size, flags, (sockaddr *) from, &from_length );
#endif // #if defined( __APPLE__ )
}

// the per-packet receive behind the public call and every batch pump's
// per-packet path. The caller owns the overflow area and the weather budget,
// so a batch call shares one of each across all its packets.

static int rocketnet_platform_socket_receive_packet_into( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size, uint8_t * overflow, size_t overflow_bytes, int * weather_remaining )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( max_packet_size > 0 );
    rocketnet_assert( weather_remaining && *weather_remaining > 0 );

    for ( ;; )
    {
        sockaddr_storage sockaddr_from;
        memset( &sockaddr_from, 0, sizeof( sockaddr_from ) );
        int result;

#if ROCKETNET_DEV
        if ( !rocketnet_platform_receive_hooked( socket, packet_data, max_packet_size, &sockaddr_from, sizeof( sockaddr_from ), &result ) )
#endif // #if ROCKETNET_DEV
        {
            result = (int) rocketnet_platform_receive_one( socket, packet_data, max_packet_size, overflow, overflow_bytes, &sockaddr_from );
            if ( result < 0 )
            {
                const int error = errno;
                if ( rocketnet_platform_receive_weather( error ) )
                {
                    if ( --*weather_remaining > 0 )
                        continue;
                    return 0;
                }
                if ( error != EAGAIN && error != EWOULDBLOCK )
                    rocketnet_debug( "recvfrom failed with error %d", error );
                return 0;
            }
        }

        if ( result < 0 )
            return 0;

        // a zero-length datagram is legal UDP carrying nothing the protocol
        // admits, and a dropped source is spoofed chaff: skip both and read
        // on, so neither reads as "socket drained"

        if ( result == 0 )
            continue;

        if ( !rocketnet_platform_mmsg_read_sockaddr( socket, &sockaddr_from, from ) )
        {
            rocketnet_assert( sockaddr_from.ss_family == AF_INET6 );
            if ( sockaddr_from.ss_family != AF_INET6 )
                return 0;
            continue;
        }

        return result;
    }
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
#if defined( __APPLE__ )
    uint8_t overflow[65535];
#else // #if defined( __APPLE__ )
    uint8_t overflow[1];
#endif // #if defined( __APPLE__ )
    int weather_remaining = ROCKETNET_PLATFORM_RECEIVE_WEATHER_SKIPS;
    return rocketnet_platform_socket_receive_packet_into( socket, from, packet_data, max_packet_size, overflow, sizeof( overflow ), &weather_remaining );
}

// ---------------------------------------------------

#if defined( __linux__ )

// The linux client's batch send and receive (#client-io-linux-mmsg): the I/O
// step sends a handed-off ring whole and drains into the receive buffer
// (proofs/tla/RNClientIo.tla IoSend and IoReceive) in as few syscalls as
// sendmmsg/recvmmsg allow. A packet is sent, dropped or delivered exactly as
// the per-packet calls would.

#define ROCKETNET_MSS_BATCH 64

void rocketnet_platform_socket_send_packets_mmsg( rocketnet_platform_socket_t * socket, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( sends || num_packets == 0 );

    int base = 0;
    while ( base < num_packets )
    {
        // one chunk. An address the socket cannot carry is dropped here, as
        // the per-packet path drops it; map[] keeps each entry's index in sends.

        sockaddr_storage addresses[ROCKETNET_MSS_BATCH];
        struct iovec iov[ROCKETNET_MSS_BATCH];
        struct mmsghdr msg[ROCKETNET_MSS_BATCH];
        int map[ROCKETNET_MSS_BATCH];

        int count = 0;
        int scanned = 0;
        while ( base + scanned < num_packets && count < ROCKETNET_MSS_BATCH )
        {
            const rocketnet_platform_send_t * send = &sends[base + scanned];
            rocketnet_assert( send->to );
            rocketnet_assert( send->packet_data );
            rocketnet_assert( send->packet_bytes > 0 );

            const socklen_t sockaddr_bytes = (socklen_t) rocketnet_platform_sockaddr_for( socket->ipv6, send->to, &addresses[count] );
            if ( sockaddr_bytes == 0 )
            {
                rocketnet_error( "invalid address. could not send packet" );
            }
            else
            {
                memset( &msg[count], 0, sizeof( msg[count] ) );
                iov[count].iov_base = const_cast<uint8_t *>( send->packet_data );
                iov[count].iov_len = (size_t) send->packet_bytes;
                msg[count].msg_hdr.msg_name = socket->connected ? NULL : &addresses[count];
                msg[count].msg_hdr.msg_namelen = socket->connected ? 0 : sockaddr_bytes;
                msg[count].msg_hdr.msg_iov = &iov[count];
                msg[count].msg_hdr.msg_iovlen = 1;
                map[count] = base + scanned;
                count++;
            }

            scanned++;
        }

        int sent = 0;
        while ( sent < count )
        {
            errno = 0;

            int result = sendmmsg( socket->handle, msg + sent, (unsigned int) ( count - sent ), 0 );
            if ( result > 0 )
            {
                sent += result;
                continue;
            }

            if ( errno == EINTR )
                continue;

            // a full socket: drop the unsent tail, as the per-packet path would

            if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS )
            {
                for ( int i = sent; i < count; i++ )
                {
                    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
                    rocketnet_address_to_string( sends[map[i]].to, address_string );
                    rocketnet_debug( "sendmmsg (%s) failed: %s", address_string, strerror( errno ) );
                    rocketnet_platform_send_failed( socket, &socket->send_failures, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, errno );
                }

                break;
            }

            // nothing went in this call, so the first unsent entry is the one
            // the kernel refused: drop it and retry the rest in order

            const int error = errno;
            char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            rocketnet_address_to_string( sends[map[sent]].to, address_string );
            rocketnet_debug( "sendmmsg (%s) failed: %s", address_string, strerror( error ) );
            rocketnet_platform_send_failed( socket, &socket->send_failures, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, error );
            sent += 1;
        }

        base += scanned;
    }
}

int rocketnet_platform_socket_receive_packets_mmsg( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_stride > 0 );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( max_packets > 0 );

    int total = 0;
    int weather_remaining = max_packets;

    while ( total < max_packets )
    {
        const int chunk_base = total;
        int want = max_packets - chunk_base;
        if ( want > ROCKETNET_MSS_BATCH )
            want = ROCKETNET_MSS_BATCH;

        sockaddr_storage addresses[ROCKETNET_MSS_BATCH];
        struct iovec iov[ROCKETNET_MSS_BATCH];
        struct mmsghdr msg[ROCKETNET_MSS_BATCH];

        for ( int i = 0; i < want; i++ )
        {
            memset( &msg[i], 0, sizeof( msg[i] ) );
            iov[i].iov_base = packet_data + (size_t) ( chunk_base + i ) * (size_t) packet_stride;
            iov[i].iov_len = (size_t) packet_stride;
            msg[i].msg_hdr.msg_name = &addresses[i];
            msg[i].msg_hdr.msg_namelen = sizeof( addresses[i] );
            msg[i].msg_hdr.msg_iov = &iov[i];
            msg[i].msg_hdr.msg_iovlen = 1;
        }

        errno = 0;

        // MSG_TRUNC: each msg_len is the datagram's true size

        int result = recvmmsg( socket->handle, msg, (unsigned int) want, MSG_DONTWAIT | MSG_TRUNC, NULL );
        if ( result < 0 )
        {
            if ( rocketnet_platform_receive_weather( errno ) && --weather_remaining > 0 )
                continue;
            if ( errno != EAGAIN && errno != EWOULDBLOCK && !rocketnet_platform_receive_weather( errno ) )
                rocketnet_debug( "recvmmsg failed with error %d", errno );
            return total;
        }

        if ( result == 0 )
            return total;

        // compact the chunk in place: a zero-length datagram or a refused
        // source makes no entry, and the next kept one moves down

        int kept = 0;
        for ( int i = 0; i < result; i++ )
        {
            rocketnet_address_t parsed;
            if ( !rocketnet_platform_mmsg_read_sockaddr( socket, &addresses[i], &parsed ) )
                continue;
            const int bytes = (int) msg[i].msg_len;
            if ( bytes <= 0 )
                continue;
            const int copy_bytes = bytes < packet_stride ? bytes : packet_stride;
            const uint8_t * source = packet_data + (size_t) ( chunk_base + i ) * (size_t) packet_stride;
            uint8_t * target = packet_data + (size_t) ( chunk_base + kept ) * (size_t) packet_stride;
            if ( target != source )
                memmove( target, source, (size_t) copy_bytes );
            from[chunk_base + kept] = parsed;
            packet_bytes[chunk_base + kept] = bytes;
            kept++;
        }

        total += kept;

        if ( result < want )
            return total;
    }

    return total;
}

#endif // #if defined( __linux__ )

// ---------------------------------------------------

#if defined( __APPLE__ )

// The darwin batch pump. Every path either moves the batch through
// sendmsg_x/recvmsg_x or hands the rest to the per-packet calls, so a packet
// is sent or dropped where the portable pump would have, with one priced
// exception at the send error path. On iOS msgx is never set and these are
// the portable loop.
//
// A hard refusal (an errno outside UDP's ordinary weather) pauses the pump on
// that socket: batch calls run portable until the pause expires, and the
// first batch call after it is the re-probe (proofs/tla/RNMsgxPause.tla).

#define ROCKETNET_PLATFORM_HAS_BATCH_PUMP 1

#define ROCKETNET_MSGX_BATCH 32
#define ROCKETNET_MSGX_RETRY_SECONDS 60.0

// the TX and RX workers both read and write the pause: relaxed atomics, since
// it orders nothing else

static inline double rocketnet_msgx_down_until( rocketnet_platform_socket_t * socket )
{
    double value;
    __atomic_load( &socket->msgx_down_until, &value, __ATOMIC_RELAXED );
    return value;
}

static inline void rocketnet_msgx_set_down_until( rocketnet_platform_socket_t * socket, double value )
{
    __atomic_store( &socket->msgx_down_until, &value, __ATOMIC_RELAXED );
}

// the live path, for logs and tests: a paused pump reads as portable

const char * rocketnet_platform_socket_pump( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

    if ( !socket->msgx )
        return "portable";
    const double down_until = rocketnet_msgx_down_until( socket );
    if ( down_until > 0.0 && rocketnet_platform_time() < down_until )
        return "portable";
    return "msgx";
}

#if ROCKETNET_DEV
static thread_local void ( *rocketnet_msgx_before_expiry_clear )( rocketnet_platform_socket_t * ) = NULL;
#endif // #if ROCKETNET_DEV

// proofs/tla/RNMsgxPause.tla Probe, ObserveExpired, ClearIfUnchanged and PollPaused:
// an expired pause is cleared only if it is still the value observed, so a
// fresh pause published by the other worker wins. FinishProbe abstracts a
// successful batch call after the probe: the batch path stays active.

static bool rocketnet_msgx_active( rocketnet_platform_socket_t * socket )
{
    if ( !socket->msgx )
        return false;
    double down_until = rocketnet_msgx_down_until( socket );
    while ( down_until > 0.0 )
    {
        if ( rocketnet_platform_time() < down_until )
            return false;
#if ROCKETNET_DEV
        if ( rocketnet_msgx_before_expiry_clear )
            rocketnet_msgx_before_expiry_clear( socket );
#endif // #if ROCKETNET_DEV
        double cleared = 0.0;
        if ( __atomic_compare_exchange( &socket->msgx_down_until, &down_until, &cleared, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED ) )
        {
            rocketnet_debug( "msgx pump pause expired. next batch call is the re-probe" );
            break;
        }
    }

    return true;
}

#if ROCKETNET_DEV
bool rocketnet_msgx_test_pause_expiry_race()
{
    rocketnet_platform_socket_t socket = {};
    socket.msgx = true;
    rocketnet_msgx_set_down_until( &socket, rocketnet_platform_time() );
    rocketnet_msgx_before_expiry_clear = []( rocketnet_platform_socket_t * raced )
    {
        rocketnet_msgx_before_expiry_clear = NULL;
        rocketnet_msgx_set_down_until( raced, rocketnet_platform_time() + ROCKETNET_MSGX_RETRY_SECONDS );
    };

    const bool active = rocketnet_msgx_active( &socket );
    rocketnet_msgx_before_expiry_clear = NULL;

    return !active && rocketnet_msgx_down_until( &socket ) > rocketnet_platform_time();
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNMsgxPause.tla Pause: one retry deadline shared by TX and RX
static void rocketnet_msgx_pause( rocketnet_platform_socket_t * socket, const char * which, int error )
{
    rocketnet_msgx_set_down_until( socket, rocketnet_platform_time() + ROCKETNET_MSGX_RETRY_SECONDS );
    rocketnet_warn( "%s refused (%s). msgx pump pauses %g seconds", which, strerror( error ), ROCKETNET_MSGX_RETRY_SECONDS );
}

// errnos a routine bad destination or a full buffer also produces on the
// per-packet path: these never pause the pump, or one forged source port
// would be a kill switch for it

static bool rocketnet_msgx_send_weather( int error )
{
    return error == EINTR || error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS || error == ECONNREFUSED ||
           error == EHOSTUNREACH || error == ENETUNREACH || error == EMSGSIZE || error == EADDRNOTAVAIL ||
           error == EPERM || error == EACCES || error == EHOSTDOWN || error == ENETDOWN;
}

void rocketnet_platform_socket_send_packets( rocketnet_platform_socket_t * socket, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( sends || num_packets == 0 );

    int base = 0;
    while ( base < num_packets && rocketnet_msgx_active( socket ) )
    {
        sockaddr_storage addresses[ROCKETNET_MSGX_BATCH];
        struct iovec iov[ROCKETNET_MSGX_BATCH];
        struct rocketnet_msghdr_x_t msg[ROCKETNET_MSGX_BATCH];
        int map[ROCKETNET_MSGX_BATCH];
        memset( msg, 0, sizeof( msg ) );

        int count = 0;
        int scanned = 0;
        while ( base + scanned < num_packets && count < ROCKETNET_MSGX_BATCH )
        {
            const rocketnet_platform_send_t * send = &sends[base + scanned];
            rocketnet_assert( send->to );
            rocketnet_assert( send->packet_data );
            rocketnet_assert( send->packet_bytes > 0 );

            const socklen_t sockaddr_bytes = (socklen_t) rocketnet_platform_sockaddr_for( socket->ipv6, send->to, &addresses[count] );
            if ( sockaddr_bytes == 0 )
            {
                rocketnet_error( "invalid address. could not send packet" );
            }
            else
            {
                iov[count].iov_base = const_cast<uint8_t *>( send->packet_data );
                iov[count].iov_len = (size_t) send->packet_bytes;
                msg[count].msg_name = socket->connected ? NULL : &addresses[count];
                msg[count].msg_namelen = socket->connected ? 0 : sockaddr_bytes;
                msg[count].msg_iov = &iov[count];
                msg[count].msg_iovlen = 1;
                map[count] = base + scanned;
                count++;
            }

            scanned++;
        }

        int offset = 0;
        while ( offset < count )
        {
            errno = 0;

            ssize_t result = -1;
            int injected_error = 0;
#if ROCKETNET_DEV
            if ( rocketnet_platform_send_failure_should_inject( socket, &injected_error ) )
                errno = injected_error;
            else
#endif // #if ROCKETNET_DEV
                result = rocketnet_sendmsg_x( socket->handle, msg + offset, (u_int) ( count - offset ), 0 );
            (void) injected_error;

            if ( result > 0 )
            {
                offset += (int) result;
                continue;
            }

            // A refused batch has an indeterminate delivered prefix: xnu can
            // deliver part of a batch and still return -1 with no count
            // (measured: 8 messages, a refused destination at index 5, 0-4
            // delivered). So the tail gets exactly one per-packet attempt each,
            // and a message the kernel already delivered may go twice: at most
            // one duplicate per message per error event, eaten by the
            // receiver's replay protection. The refused chunk counts once in
            // send_failures; each refusal of its replay counts in
            // send_replay_failures. Retrying EINTR here instead could hold a
            // server stop in this loop indefinitely.

            const int error = errno;

            if ( result == 0 && error == 0 )
                rocketnet_debug( "sendmsg_x accepted nothing. chunk finishes per-packet" );
            else if ( rocketnet_msgx_send_weather( error ) )
                rocketnet_debug( "sendmsg_x error (%s). chunk finishes per-packet", strerror( error ) );
            else
                rocketnet_msgx_pause( socket, "sendmsg_x", error );

            rocketnet_platform_send_failed( socket, &socket->send_failures, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, error );

            for ( int i = offset; i < count; i++ )
            {
                const rocketnet_platform_send_t * send = &sends[map[i]];
                rocketnet_platform_send_one( socket, send->to, send->packet_data, send->packet_bytes, &socket->send_replay_failures );
            }

            offset = count;
        }

        base += scanned;
    }

    // a paused pump, or a pause that landed mid-call: the rest goes per-packet

    for ( int i = base; i < num_packets; i++ )
        rocketnet_platform_socket_send_packet( socket, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
}

int rocketnet_platform_socket_receive_packets( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_stride > 0 );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( max_packets > 0 );

    // each message keeps its prefix in its own slot; the discarded tails may
    // overlap, since none is read after the syscall returns. One weather
    // budget covers both pumps and every packet of the call.

    uint8_t overflow[65535];
    int total = 0;
    int weather_remaining = max_packets;

    bool batch = rocketnet_msgx_active( socket );

    while ( batch && total < max_packets )
    {
        int want = max_packets - total;
        if ( want > ROCKETNET_MSGX_BATCH )
            want = ROCKETNET_MSGX_BATCH;

        const int chunk_base = total;

        sockaddr_storage addresses[ROCKETNET_MSGX_BATCH];
        struct iovec iov[ROCKETNET_MSGX_BATCH][2];
        struct rocketnet_msghdr_x_t msg[ROCKETNET_MSGX_BATCH];
        memset( msg, 0, sizeof( msg ) );

        for ( int i = 0; i < want; i++ )
        {
            iov[i][0].iov_base = packet_data + (size_t) ( chunk_base + i ) * (size_t) packet_stride;
            iov[i][0].iov_len = (size_t) packet_stride;
            iov[i][1].iov_base = overflow;
            iov[i][1].iov_len = sizeof( overflow );
            msg[i].msg_name = &addresses[i];
            msg[i].msg_namelen = sizeof( addresses[i] );
            msg[i].msg_iov = iov[i];
            msg[i].msg_iovlen = 2;
        }

        errno = 0;

        ssize_t result;
#if ROCKETNET_DEV
        const int injected = rocketnet_platform_take_receive_errno_for_testing();
        if ( injected != 0 )
        {
            errno = injected;
            result = -1;
        }
        else
#endif // #if ROCKETNET_DEV
            result = rocketnet_recvmsg_x( socket->handle, msg, (u_int) want, 0 );

        if ( result < 0 )
        {
            const int error = errno;
            if ( error == EAGAIN || error == EWOULDBLOCK )
                return total;

            // weather: skip it and stay on the batch pump, up to the budget

            if ( rocketnet_platform_receive_weather( error ) )
            {
                if ( --weather_remaining > 0 )
                    continue;
                return total;
            }

            // anything else pauses the pump; this call finishes per-packet

            rocketnet_msgx_pause( socket, "recvmsg_x", error );
            batch = false;
            break;
        }

        if ( result == 0 )
            return total;

        // compact the chunk in place, as the linux batch receive does.
        // msg_datalen is the true size; the slot holds at most its stride.

        for ( ssize_t i = 0; i < result; i++ )
        {
            const int bytes = (int) msg[i].msg_datalen;
            rocketnet_address_t parsed;
            if ( bytes <= 0 || !rocketnet_platform_mmsg_read_sockaddr( socket, &addresses[i], &parsed ) )
                continue;
            uint8_t * written = packet_data + (size_t) ( chunk_base + (int) i ) * (size_t) packet_stride;
            uint8_t * target = packet_data + (size_t) total * (size_t) packet_stride;
            const int copy_bytes = bytes < packet_stride ? bytes : packet_stride;
            if ( target != written )
                memmove( target, written, (size_t) copy_bytes );
            from[total] = parsed;
            packet_bytes[total] = bytes;
            total++;
        }

        if ( result < want )
            return total;
    }

    if ( batch )
        return total;

    while ( total < max_packets )
    {
        const int bytes = rocketnet_platform_socket_receive_packet_into( socket, &from[total], packet_data + (size_t) total * (size_t) packet_stride, packet_stride,
                                                                         overflow, sizeof( overflow ), &weather_remaining );
        if ( bytes == 0 )
            break;
        packet_bytes[total] = bytes;
        total++;
    }

    return total;
}

#endif // #if defined( __APPLE__ )

// ---------------------------------------------------

#if defined( __APPLE__ )

// The client I/O wake on darwin: one kqueue holding the socket's EVFILT_READ
// and an EV_CLEAR EVFILT_USER trigger (docs/CLIENT-IO.md "The wake"). A set
// made while no wait runs makes the next wait return at once, and that wait
// consumes it. readable toggles the read filter in the same kevent call, so a
// full buffer waits on the trigger alone (proofs/tla/RNClientIo.tla IoWait, IoWake).

#define ROCKETNET_PLATFORM_WAKE_KEVENT_IDENT 1

struct rocketnet_platform_wake_t
{
    void * context;
    int kq;
    int socket_fd;
    bool read_enabled;
};

rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket, int * status )
{
    rocketnet_platform_wake_t * wake = (rocketnet_platform_wake_t *) rocketnet_malloc( context, sizeof( rocketnet_platform_wake_t ) );
    if ( !wake )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    wake->context = context;
    wake->socket_fd = socket ? socket->handle : -1;
    wake->read_enabled = socket != NULL;
    wake->kq = kqueue();
    if ( wake->kq < 0 )
    {
        rocketnet_free( context, wake );
        return NULL;
    }

    struct kevent changes[2];
    int num_changes = 0;
    if ( socket )
    {
        EV_SET( &changes[num_changes], socket->handle, EVFILT_READ, EV_ADD, 0, 0, NULL );
        num_changes++;
    }
    EV_SET( &changes[num_changes], ROCKETNET_PLATFORM_WAKE_KEVENT_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL );
    num_changes++;

    if ( kevent( wake->kq, changes, num_changes, NULL, 0, NULL ) < 0 )
    {
        close( wake->kq );
        rocketnet_free( context, wake );
        return NULL;
    }

    return wake;
}

bool rocketnet_platform_wake_close( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    if ( wake->kq >= 0 )
        close( wake->kq );
    wake->kq = -1;
    return true;
}

bool rocketnet_platform_wake_wait( rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds )
{
    rocketnet_assert( wake );

    if ( readable && wake->socket_fd < 0 )
        return false;

    struct kevent change;
    int num_changes = 0;
    if ( readable != wake->read_enabled )
    {
        EV_SET( &change, wake->socket_fd, EVFILT_READ, readable ? EV_ENABLE : EV_DISABLE, 0, 0, NULL );
        num_changes = 1;
        wake->read_enabled = readable;
    }

    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 0;
    if ( timeout_seconds > 0.0 )
    {
        timeout.tv_sec = (time_t) timeout_seconds;
        timeout.tv_nsec = (long) ( ( timeout_seconds - (double) timeout.tv_sec ) * 1000000000.0 );
        if ( timeout.tv_nsec >= 1000000000L )
            timeout.tv_nsec = 999999999L;
    }

    struct kevent events[2];
    const int result = kevent( wake->kq, num_changes ? &change : NULL, num_changes, events, 2, &timeout );
    if ( result <= 0 )
        return false;

    bool woken = false;
    for ( int i = 0; i < result; i++ )
    {
        if ( events[i].flags & EV_ERROR )
        {
            // a refused read filter change keeps the old state, so the next
            // wait carries the change again
            if ( events[i].filter == EVFILT_READ && events[i].data != 0 )
                wake->read_enabled = !readable;
            continue;
        }
        woken = true;
    }

    return woken;
}

void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    struct kevent change;
    EV_SET( &change, ROCKETNET_PLATFORM_WAKE_KEVENT_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL );
    kevent( wake->kq, &change, 1, NULL, 0, NULL );
}

#elif defined( __linux__ ) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD // #if defined( __APPLE__ )

// The wake on linux and the BSDs: a readable descriptor polled beside the
// socket (docs/CLIENT-IO.md "The wake"; proofs/tla/RNClientIo.tla IoWake, IoWait and
// Wake). Linux uses an eventfd, the BSDs a nonblocking pipe (OpenBSD has no
// EVFILT_USER). Level-triggered: a set made while no wait runs makes the next
// wait return at once, and the wait drains it. The BSD wake is event-only for
// now: it takes no socket, so BSD clients keep their receive-thread path.

struct rocketnet_platform_wake_t
{
    void * context;
    int socket_fd;
    int read_fd;
    int write_fd;
};

#if !defined( __linux__ )
static bool rocketnet_platform_wake_fd_flags( int fd )
{
    const int status = fcntl( fd, F_GETFL, 0 );
    const int flags = fcntl( fd, F_GETFD, 0 );
    return status >= 0 && flags >= 0 && fcntl( fd, F_SETFL, status | O_NONBLOCK ) == 0 && fcntl( fd, F_SETFD, flags | FD_CLOEXEC ) == 0;
}
#endif // #if !defined( __linux__ )

rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket, int * status )
{
#if !defined( __linux__ )
    if ( socket )
        return NULL;
#endif // #if !defined( __linux__ )

    rocketnet_platform_wake_t * wake = (rocketnet_platform_wake_t *) rocketnet_malloc( context, sizeof( rocketnet_platform_wake_t ) );
    if ( !wake )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    wake->context = context;
    wake->socket_fd = socket ? socket->handle : -1;

#if defined( __linux__ )
    wake->read_fd = eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
    wake->write_fd = wake->read_fd;
    if ( wake->read_fd < 0 )
    {
        rocketnet_free( context, wake );
        return NULL;
    }
#else // #if defined( __linux__ )
    int fds[2];
    if ( pipe( fds ) != 0 )
    {
        rocketnet_free( context, wake );
        return NULL;
    }
    if ( !rocketnet_platform_wake_fd_flags( fds[0] ) || !rocketnet_platform_wake_fd_flags( fds[1] ) )
    {
        close( fds[0] );
        close( fds[1] );
        rocketnet_free( context, wake );
        return NULL;
    }
    wake->read_fd = fds[0];
    wake->write_fd = fds[1];
#endif // #if defined( __linux__ )

    return wake;
}

bool rocketnet_platform_wake_close( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    if ( wake->read_fd >= 0 )
        close( wake->read_fd );
    if ( wake->write_fd >= 0 && wake->write_fd != wake->read_fd )
        close( wake->write_fd );
    wake->read_fd = -1;
    wake->write_fd = -1;
    return true;
}

bool rocketnet_platform_wake_wait( rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds )
{
    rocketnet_assert( wake );

    if ( readable && wake->socket_fd < 0 )
        return false;

    struct pollfd fds[2];
    int nfds = 0;
    fds[nfds].fd = wake->read_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    nfds++;
    if ( readable )
    {
        fds[nfds].fd = wake->socket_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    const int timeout_ms = timeout_seconds > 0.0 ? (int) ceil( timeout_seconds * 1000.0 ) : 0;
    if ( poll( fds, (nfds_t) nfds, timeout_ms ) <= 0 )
        return false;

    if ( fds[0].revents & POLLIN )
    {
        // an eventfd read takes its whole 8-byte counter; a pipe read takes
        // up to 64 pending one-byte sets
        uint8_t buffer[64];
        const ssize_t bytes = read( wake->read_fd, buffer, sizeof( buffer ) );
#if defined( __linux__ )
        (void) bytes;
        return true;
#else // #if defined( __linux__ )
        return bytes > 0;
#endif // #if defined( __linux__ )
    }

    return nfds > 1 && ( fds[1].revents & POLLIN );
}

void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );

    // an eventfd takes an 8-byte add; a full pipe (EAGAIN) already holds a
    // pending wake
#if defined( __linux__ )
    const uint64_t one = 1;
#else // #if defined( __linux__ )
    const uint8_t one = 1;
#endif // #if defined( __linux__ )
    ssize_t n = write( wake->write_fd, &one, sizeof( one ) );
    (void) n;
}

#endif // #if defined( __APPLE__ )

#if defined( __APPLE__ ) || defined( __linux__ ) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
void rocketnet_platform_wake_destroy( rocketnet_platform_wake_t * wake )
{
    rocketnet_platform_wake_close( wake );
    rocketnet_free( wake->context, wake );
}
#endif // #if defined( __APPLE__ ) || defined( __linux__ ) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

// ---------------------------------------------------

// The thread and its shim are one allocation that lives from create to
// destroy, so no allocation is freed when the worker begins and a restart's
// peak never depends on scheduling. The thread is the first member, so
// freeing the thread frees the record.

struct thread_shim_data_t
{
    void * context;
    void * real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function;
};

struct thread_record_t
{
    rocketnet_platform_thread_t thread;
    thread_shim_data_t shim;
};
static_assert( offsetof( thread_record_t, thread ) == 0, "the thread is the record's first member" );

static void * thread_function_shim( void * data )
{
    rocketnet_assert( data );
    thread_record_t * record = (thread_record_t *) data;
    record->shim.real_thread_function( &record->thread, record->shim.real_thread_data );
    return NULL;
}

// thread_wait and thread_wake: a mach semaphore on darwin, which has no
// monotonic condition-variable clock; elsewhere a condition variable on
// CLOCK_MONOTONIC, so the wait is immune to wall-clock steps. woken is set
// once and never cleared.

static bool rocketnet_platform_thread_wake_init( rocketnet_platform_thread_t * thread )
{
    thread->woken = 0;
#if defined( __APPLE__ )
    semaphore_t semaphore = MACH_PORT_NULL;
    if ( semaphore_create( mach_task_self(), &semaphore, SYNC_POLICY_FIFO, 0 ) != KERN_SUCCESS )
        return false;
    thread->wake_semaphore = semaphore;
    return true;
#else // #if defined( __APPLE__ )
    pthread_condattr_t attr;
    if ( pthread_condattr_init( &attr ) != 0 )
        return false;
    if ( pthread_condattr_setclock( &attr, CLOCK_MONOTONIC ) != 0 )
    {
        pthread_condattr_destroy( &attr );
        return false;
    }
    const int cond_result = pthread_cond_init( &thread->wake_cond, &attr );
    pthread_condattr_destroy( &attr );
    if ( cond_result != 0 )
        return false;
    if ( pthread_mutex_init( &thread->wake_mutex, NULL ) != 0 )
    {
        pthread_cond_destroy( &thread->wake_cond );
        return false;
    }
    return true;
#endif // #if defined( __APPLE__ )
}

static void rocketnet_platform_thread_wake_term( rocketnet_platform_thread_t * thread )
{
#if defined( __APPLE__ )
    semaphore_destroy( mach_task_self(), (semaphore_t) thread->wake_semaphore );
#else // #if defined( __APPLE__ )
    pthread_cond_destroy( &thread->wake_cond );
    pthread_mutex_destroy( &thread->wake_mutex );
#endif // #if defined( __APPLE__ )
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg, int * status )
{
    thread_record_t * record = (thread_record_t *) rocketnet_malloc( context, sizeof( thread_record_t ) );
    if ( !record )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    rocketnet_platform_thread_t * thread = &record->thread;
    thread->context = context;

    thread_shim_data_t * shim_data = &record->shim;
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;

    if ( !rocketnet_platform_thread_wake_init( thread ) )
    {
        rocketnet_free( context, thread );
        return NULL;
    }

    if ( pthread_create( &thread->handle, NULL, thread_function_shim, record ) != 0 )
    {
        rocketnet_platform_thread_wake_term( thread );
        rocketnet_free( context, thread );
        return NULL;
    }

    return thread;
}

void rocketnet_platform_thread_join( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    pthread_join( thread->handle, NULL );
}

void rocketnet_platform_thread_destroy( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    rocketnet_platform_thread_wake_term( thread );
    rocketnet_free( thread->context, thread );
}

void rocketnet_platform_thread_sleep( rocketnet_platform_thread_t * thread, double time )
{
    rocketnet_assert( thread );
    (void) thread;
    rocketnet_platform_sleep( time );
}

void rocketnet_platform_thread_wait( rocketnet_platform_thread_t * thread, double time )
{
    rocketnet_assert( thread );

    // whole seconds and nanoseconds converted apart, clamped to a day, so no
    // conversion overflows a 32-bit long

    if ( !( time > 0.0 ) )
        time = 0.0;
    if ( time > 86400.0 )
        time = 86400.0;

#if defined( __APPLE__ )
    // woken is stored before the semaphore is signalled: a wake that lands
    // before this check ends the wait here, one that lands after it ends the
    // semaphore wait
    if ( __atomic_load_n( &thread->woken, __ATOMIC_ACQUIRE ) )
        return;
    const unsigned int whole_seconds = (unsigned int) time;
    mach_timespec_t relative;
    relative.tv_sec = whole_seconds;
    relative.tv_nsec = (clock_res_t) ( ( time - (double) whole_seconds ) * 1000000000.0 );
    semaphore_timedwait( (semaphore_t) thread->wake_semaphore, relative );
#else // #if defined( __APPLE__ )
    const long whole_seconds = (long) time;
    const long nanoseconds = (long) ( ( time - (double) whole_seconds ) * 1000000000.0 );
    struct timespec deadline;
    clock_gettime( CLOCK_MONOTONIC, &deadline );
    deadline.tv_sec += whole_seconds;
    deadline.tv_nsec += nanoseconds;
    if ( deadline.tv_nsec >= 1000000000L )
    {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock( &thread->wake_mutex );
    while ( !thread->woken )
    {
        if ( pthread_cond_timedwait( &thread->wake_cond, &thread->wake_mutex, &deadline ) != 0 )
            break;
    }
    pthread_mutex_unlock( &thread->wake_mutex );
#endif // #if defined( __APPLE__ )
}

void rocketnet_platform_thread_wake( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
#if defined( __APPLE__ )
    __atomic_store_n( &thread->woken, 1, __ATOMIC_RELEASE );
    semaphore_signal( (semaphore_t) thread->wake_semaphore );
#else // #if defined( __APPLE__ )
    pthread_mutex_lock( &thread->wake_mutex );
    thread->woken = 1;
    pthread_cond_broadcast( &thread->wake_cond );
    pthread_mutex_unlock( &thread->wake_mutex );
#endif // #if defined( __APPLE__ )
}

// IMPORTANT: adjust the client and server network thread priority here. Both
// set SCHED_RR at rocketnet_platform_network_thread_rr_priority() and nothing
// else. That priority is sched_get_priority_max( SCHED_RR ) on macOS, and
// elsewhere sched_get_priority_min( SCHED_RR ), never below the default's.
// No macOS server transport calls the server hook.

void rocketnet_platform_client_thread_priority( rocketnet_platform_thread_t * thread )
{
    rocketnet_platform_network_thread_priority( thread );
}

void rocketnet_platform_server_thread_priority( rocketnet_platform_thread_t * thread )
{
    rocketnet_platform_network_thread_priority( thread );
}

// ---------------------------------------------------

bool rocketnet_platform_mutex_create( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    return ROCKETNET_PLATFORM_PRIORITY_CALL( mutex_init )( mutex ) == 0;
}

void rocketnet_platform_mutex_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    pthread_mutex_lock( &mutex->handle );
}

bool rocketnet_platform_mutex_try_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex && mutex->ok );
    return pthread_mutex_trylock( &mutex->handle ) == 0;
}

void rocketnet_platform_mutex_release( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    pthread_mutex_unlock( &mutex->handle );
}

void rocketnet_platform_mutex_destroy( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    if ( mutex->ok )
    {
        pthread_mutex_destroy( &mutex->handle );
        memset( mutex, 0, sizeof( rocketnet_platform_mutex_t ) );
    }
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS // #if POSIX

// windows

#include "rocketnet_platform.h"
#include <limits.h>
#include <math.h>
#include "rocketnet_address.h"

#if ROCKETNET_UNREAL_ENGINE
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/PreWindowsApi.h"
#endif // #if ROCKETNET_UNREAL_ENGINE

#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#if !ROCKETNET_UNREAL_ENGINE
#include <windows.h>
#else // #if !ROCKETNET_UNREAL_ENGINE
#include "Windows/MinWindows.h"
#endif // #if !ROCKETNET_UNREAL_ENGINE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <malloc.h>
#include <mswsock.h>
#include <stdlib.h>

#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static_assert( sizeof( uintptr_t ) == 8 && ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2 &&
                   ATOMIC_INT_LOCK_FREE == 2,
               "Windows lifecycle ledger needs lock-free x64 atomics" );

// Test-only resource identities. State packs the generation so a recycled
// slot cannot make a stale close retire a different handle with the same raw
// value. Every scan is bounded by the fixed table; there is no wait or heap.
struct rocketnet_lifecycle_slot_t
{
    std::atomic<uint64_t> state{ 0 };
    std::atomic<uintptr_t> raw{ 0 };
    std::atomic<const char *> site{ NULL };
    std::atomic<unsigned> kind{ 0 };
};

static rocketnet_lifecycle_slot_t rocketnet_lifecycle_slots[ROCKETNET_LIFECYCLE_MAX_LIVE];
static std::atomic<uint64_t> rocketnet_lifecycle_next_generation{ 1 };
static std::atomic<uint64_t> rocketnet_lifecycle_created[ROCKETNET_LIFECYCLE_RESOURCE_KINDS]{};
static std::atomic<uint64_t> rocketnet_lifecycle_closed[ROCKETNET_LIFECYCLE_RESOURCE_KINDS]{};
static std::atomic<unsigned> rocketnet_lifecycle_errors{ 0 };
static std::atomic<bool> rocketnet_lifecycle_enabled{ false };

static uint64_t rocketnet_lifecycle_state( uint64_t generation, uint64_t state )
{
    return ( generation << 2 ) | state;
}

static void rocketnet_lifecycle_invariant_failure( const char * message )
{
    rocketnet_lifecycle_errors.fetch_add( 1, std::memory_order_relaxed );
    fprintf( stderr, "SDK lifecycle ledger invariant failed: %s\n", message );
    fflush( stderr );
    TerminateProcess( GetCurrentProcess(), 86 );
    abort();
}

void rocketnet_lifecycle_track_begin()
{
    rocketnet_lifecycle_enabled.store( true, std::memory_order_release );
}

void rocketnet_lifecycle_track_end()
{
    rocketnet_lifecycle_enabled.store( false, std::memory_order_release );
}

uint64_t rocketnet_lifecycle_track_open( unsigned kind, uintptr_t raw, const char * site )
{
    if ( !rocketnet_lifecycle_enabled.load( std::memory_order_acquire ) )
        return 0;
    if ( kind >= ROCKETNET_LIFECYCLE_RESOURCE_KINDS || !raw )
    {
        rocketnet_lifecycle_errors.fetch_add( 1, std::memory_order_relaxed );
        return 0;
    }

    const uint64_t generation = rocketnet_lifecycle_next_generation.fetch_add( 1, std::memory_order_relaxed );
    if ( generation == 0 || generation > ( UINT64_MAX >> 2 ) )
    {
        rocketnet_lifecycle_invariant_failure( "generation space exhausted" );
        return 0;
    }

    for ( unsigned i = 0; i < ROCKETNET_LIFECYCLE_MAX_LIVE; ++i )
    {
        uint64_t empty = 0;
        if ( rocketnet_lifecycle_slots[i].state.compare_exchange_strong( empty, rocketnet_lifecycle_state( generation, 1 ),
                                                                         std::memory_order_acq_rel ) )
        {
            rocketnet_lifecycle_slots[i].raw.store( raw, std::memory_order_relaxed );
            rocketnet_lifecycle_slots[i].kind.store( kind, std::memory_order_relaxed );
            rocketnet_lifecycle_slots[i].site.store( site, std::memory_order_relaxed );
            rocketnet_lifecycle_slots[i].state.store( rocketnet_lifecycle_state( generation, 2 ), std::memory_order_release );
            rocketnet_lifecycle_created[kind].fetch_add( 1, std::memory_order_relaxed );
            return generation;
        }
    }

    rocketnet_lifecycle_errors.fetch_add( 1, std::memory_order_relaxed );

    return 0;
}

bool rocketnet_lifecycle_track_close_begin( unsigned kind, uintptr_t raw, uint64_t generation )
{
    if ( generation == 0 )
        return true; // created before this case was armed
    for ( unsigned i = 0; i < ROCKETNET_LIFECYCLE_MAX_LIVE; ++i )
    {
        uint64_t live = rocketnet_lifecycle_state( generation, 2 );
        if ( rocketnet_lifecycle_slots[i].state.compare_exchange_strong( live, rocketnet_lifecycle_state( generation, 3 ),
                                                                         std::memory_order_acq_rel ) )
        {
            if ( rocketnet_lifecycle_slots[i].kind.load( std::memory_order_relaxed ) == kind &&
                 rocketnet_lifecycle_slots[i].raw.load( std::memory_order_relaxed ) == raw )
                return true;
            rocketnet_lifecycle_slots[i].state.store( live, std::memory_order_release );
            break;
        }
    }

    rocketnet_lifecycle_invariant_failure( "unknown or mismatched close" );

    return false;
}

void rocketnet_lifecycle_track_close_end( uint64_t generation, bool success )
{
    if ( generation == 0 )
        return;
    for ( unsigned i = 0; i < ROCKETNET_LIFECYCLE_MAX_LIVE; ++i )
    {
        uint64_t closing = rocketnet_lifecycle_state( generation, 3 );
        if ( rocketnet_lifecycle_slots[i].state.load( std::memory_order_acquire ) != closing )
            continue;
        const unsigned kind = rocketnet_lifecycle_slots[i].kind.load( std::memory_order_relaxed );

        if ( rocketnet_lifecycle_slots[i].state.compare_exchange_strong( closing,
                                                                         success ? 0 : rocketnet_lifecycle_state( generation, 2 ), std::memory_order_acq_rel ) )
        {
            if ( success )
                rocketnet_lifecycle_closed[kind].fetch_add( 1, std::memory_order_relaxed );
            else
                rocketnet_lifecycle_errors.fetch_add( 1, std::memory_order_relaxed );
            return;
        }
    }

    rocketnet_lifecycle_invariant_failure( "missing closing generation" );
}

bool rocketnet_lifecycle_track_read( rocketnet_lifecycle_resources_t * result )
{
    if ( !result )
        return false;
    memset( result, 0, sizeof( *result ) );
    result->latest_generation = rocketnet_lifecycle_next_generation.load( std::memory_order_acquire ) - 1;
    result->errors = rocketnet_lifecycle_errors.load( std::memory_order_acquire );

    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
    {
        result->created[kind] = rocketnet_lifecycle_created[kind].load( std::memory_order_acquire );
        result->closed[kind] = rocketnet_lifecycle_closed[kind].load( std::memory_order_acquire );
    }

    for ( unsigned i = 0; i < ROCKETNET_LIFECYCLE_MAX_LIVE; ++i )
    {
        const uint64_t state = rocketnet_lifecycle_slots[i].state.load( std::memory_order_acquire );
        if ( !state )
            continue;
        if ( ( state & 3 ) != 2 )
        {
            result->errors++;
            continue;
        }

        rocketnet_lifecycle_resource_t & resource = result->live[result->live_count++];
        resource.generation = state >> 2;
        resource.raw = rocketnet_lifecycle_slots[i].raw.load( std::memory_order_relaxed );
        resource.kind = rocketnet_lifecycle_slots[i].kind.load( std::memory_order_relaxed );
        resource.site = rocketnet_lifecycle_slots[i].site.load( std::memory_order_relaxed );
        if ( rocketnet_lifecycle_slots[i].state.load( std::memory_order_acquire ) != state )
            result->errors++;
    }

    for ( unsigned i = 0; i < result->live_count; ++i )
        for ( unsigned j = i + 1; j < result->live_count; ++j )
            if ( result->live[i].kind == result->live[j].kind && result->live[i].raw == result->live[j].raw )
                result->errors++;
    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
    {
        unsigned live = 0;
        for ( unsigned i = 0; i < result->live_count; ++i )
            if ( result->live[i].kind == kind )
                live++;
        if ( result->created[kind] < result->closed[kind] || result->created[kind] - result->closed[kind] != live )
            result->errors++;
    }

    return result->errors == 0;
}
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#pragma comment( lib, "WS2_32.lib" )

#ifdef SetPort
#undef SetPort
#endif // #ifdef SetPort

extern void * rocketnet_malloc( void * context, size_t bytes );

extern void rocketnet_free( void * context, void * p );

static LARGE_INTEGER timer_frequency;
static LARGE_INTEGER timer_start;

// Windows: SetThreadPriority( thread, THREAD_PRIORITY_TIME_CRITICAL ) on the
// network threads (flow_platform.cpp:1839-1851), and the process timer at 1 ms
// from platform init to platform term, matching the host application's lifecycle

#if ROCKETNET_DEV
static int rocketnet_platform_real_set_sched( rocketnet_platform_thread_t * thread, int policy, int priority )
{
    (void) thread;
    (void) policy;
    (void) priority;
    return 0;
}
#endif // #if ROCKETNET_DEV

static int rocketnet_platform_real_set_thread_priority( rocketnet_platform_thread_t * thread, int priority )
{
    return SetThreadPriority( thread->handle, priority ) ? 1 : 0;
}

static unsigned int rocketnet_platform_real_time_begin_period( unsigned int period )
{
    return (unsigned int) timeBeginPeriod( period );
}

static unsigned int rocketnet_platform_real_time_end_period( unsigned int period )
{
    return (unsigned int) timeEndPeriod( period );
}

static void rocketnet_platform_real_timer_frequency( void * frequency )
{
    QueryPerformanceFrequency( (LARGE_INTEGER *) frequency );
}

static int rocketnet_platform_real_mutex_init( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    memset( mutex, 0, sizeof( rocketnet_platform_mutex_t ) );
    if ( !InitializeCriticalSectionAndSpinCount( (LPCRITICAL_SECTION) &mutex->handle, 0xFF ) )
        return 1;
    mutex->ok = true;

    return 0;
}

static int rocketnet_platform_real_platform_start( void )
{
    WSADATA WsaData;
    return WSAStartup( MAKEWORD(2,2), &WsaData ) == NO_ERROR ? 0 : 1;
}

#if ROCKETNET_DEV
static void * rocketnet_platform_real_create_waitable_timer( unsigned flags )
{
    return CreateWaitableTimerExW( NULL, NULL, flags, TIMER_ALL_ACCESS );
}

static const rocketnet_platform_priority_calls_t rocketnet_platform_priority_calls_real_table = {
    rocketnet_platform_real_set_sched,
    rocketnet_platform_real_set_thread_priority,
    rocketnet_platform_real_time_begin_period,
    rocketnet_platform_real_time_end_period,
    rocketnet_platform_real_timer_frequency,
    rocketnet_platform_real_mutex_init,
    rocketnet_platform_real_platform_start,
    rocketnet_platform_real_create_waitable_timer,
};

const rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls_real()
{
    return &rocketnet_platform_priority_calls_real_table;
}
#endif // #if ROCKETNET_DEV

static void rocketnet_platform_network_thread_priority( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );

    if ( !rocketnet_platform_thread_priority_enabled() )
        return;
    if ( !ROCKETNET_PLATFORM_PRIORITY_CALL( set_thread_priority )( thread, THREAD_PRIORITY_TIME_CRITICAL ) )
    {
        char reason[32];
        snprintf( reason, sizeof( reason ), "error %lu", (unsigned long) GetLastError() );
        rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_THREAD_PRIORITY, "thread priority", "SetThreadPriority THREAD_PRIORITY_TIME_CRITICAL", reason, "the thread runs at its default priority" );
    }
}

// true from a granted timeBeginPeriod( 1 ) at init to its timeEndPeriod( 1 )
// at term; a refused or skipped begin has no end
static bool timer_period_begun = false;

static void rocketnet_platform_timer_period_end()
{
    if ( !timer_period_begun )
        return;
    timer_period_begun = false;

    const unsigned int result = ROCKETNET_PLATFORM_PRIORITY_CALL( time_end_period )( 1 );
    if ( result != TIMERR_NOERROR )
    {
        char reason[32];
        snprintf( reason, sizeof( reason ), "error %u", result );
        rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_TIME_END_PERIOD, "timer period", "timeEndPeriod 1", reason, "the timer period request stays until the process exits" );
    }
}

// init

bool rocketnet_platform_init()
{
    timer_period_begun = false;

    if ( rocketnet_platform_thread_priority_enabled() )
    {
        const unsigned int result = ROCKETNET_PLATFORM_PRIORITY_CALL( time_begin_period )( 1 );
        if ( result == TIMERR_NOERROR )
        {
            timer_period_begun = true;
        }
        else
        {
            char reason[32];
            snprintf( reason, sizeof( reason ), "error %u", result );
            rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_TIME_BEGIN_PERIOD, "timer period", "timeBeginPeriod 1", reason, "the timer keeps its default period" );
        }
    }

    ROCKETNET_PLATFORM_PRIORITY_CALL( timer_frequency )
    ( &timer_frequency );
    QueryPerformanceCounter( &timer_start );

    if ( ROCKETNET_PLATFORM_PRIORITY_CALL( platform_start )() != 0 )
    {
        rocketnet_error( "WSAStartup failed" );
        rocketnet_platform_timer_period_end();
        return false;
    }

    return true;
}

void rocketnet_platform_term()
{
    WSACleanup();
    rocketnet_platform_timer_period_end();
}

int rocketnet_platform_getenv( const char * var, char * buffer, size_t size )
{
    rocketnet_assert( var && buffer && size > 0 );
#if ROCKETNET_DEV
    int test_result = 0;
    if ( rocketnet_platform_test_environment_getenv( var, buffer, size, &test_result ) )
        return test_result;
#endif // #if ROCKETNET_DEV
    buffer[0] = '\0';

    // getenv_s with no buffer reports the bytes the value needs, terminator
    // included, and zero when the variable is unset

    size_t required = 0;
    if ( getenv_s( &required, NULL, 0, var ) != 0 || required == 0 )
        return 0;
    if ( required > size )
        return -1;
    if ( getenv_s( &required, buffer, size, var ) != 0 )
    {
        buffer[0] = '\0';
        return 0;
    }

    return 1;
}

// threads

struct thread_shim_data_t
{
    void * context;
    void * real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function;
};

// the thread and its shim are one allocation: the shim lives as long as the
// thread record (thread_destroy, after join), so no allocation is freed when
// the worker begins and a restart's peak never depends on scheduling. The
// thread is the first member, so freeing the thread frees the record.
struct thread_record_t
{
    rocketnet_platform_thread_t thread;
    thread_shim_data_t shim;
};
static_assert( offsetof( thread_record_t, thread ) == 0, "the thread is the record's first member" );

static DWORD WINAPI thread_function_shim( void * data )
{
    rocketnet_assert( data );

    thread_record_t * record = (thread_record_t *) data;
    record->shim.real_thread_function( &record->thread, record->shim.real_thread_data );

    return 0;
}

static HANDLE rocketnet_platform_sleep_timer_create();

static void rocketnet_platform_thread_sleep_timer_close( rocketnet_platform_thread_t * thread )
{
    if ( !thread->sleep_timer )
        return;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_TIMER, (uintptr_t) thread->sleep_timer, thread->sleep_timer_generation ) )
    {
        const BOOL closed = CloseHandle( thread->sleep_timer );
        rocketnet_lifecycle_track_close_end( thread->sleep_timer_generation, closed != 0 );
    }
#else
    CloseHandle( thread->sleep_timer );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    thread->sleep_timer = NULL;
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg, int * status )
{
    thread_record_t * record = (thread_record_t *) rocketnet_malloc( context, sizeof( thread_record_t ) );

    if ( !record )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    rocketnet_platform_thread_t * thread = &record->thread;
    thread->context = context;

    thread_shim_data_t * shim_data = &record->shim;
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;

    // the thread's sleep timer exists before the thread runs

    thread->sleep_timer = rocketnet_platform_sleep_timer_create();
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( thread->sleep_timer )
        thread->sleep_timer_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_TIMER, (uintptr_t) thread->sleep_timer, "platform thread sleep timer" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    thread->handle = CreateThread( NULL, 0, thread_function_shim, record, 0, NULL );

    if ( thread->handle == NULL )
    {
        rocketnet_platform_thread_sleep_timer_close( thread );
        rocketnet_free( context, thread );
        return NULL;
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    thread->lifecycle_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_THREAD, (uintptr_t) thread->handle, "platform thread" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    return thread;
}

void rocketnet_platform_thread_join( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    WaitForSingleObject( thread->handle, INFINITE );
}

void rocketnet_platform_thread_destroy( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_THREAD, (uintptr_t) thread->handle, thread->lifecycle_generation ) )
        return;
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const BOOL closed = CloseHandle( thread->handle );
    rocketnet_lifecycle_track_close_end( thread->lifecycle_generation, closed != 0 );
#else
    CloseHandle( thread->handle );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_platform_thread_sleep_timer_close( thread );
    rocketnet_free( thread->context, thread );
}

// Windows keeps the receive thread's sleep: no per-thread event or timer, so
// the wake is a no-op and the thread observes quit within one sleep period
void rocketnet_platform_thread_wake( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    (void) thread;
}

void rocketnet_platform_client_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If your client runs on windows, you can set the thread priority and affinity of the client thread here
    // THREAD_PRIORITY_TIME_CRITICAL, as flow sets it (flow_platform.cpp:1839-1844)
    rocketnet_platform_network_thread_priority( thread );
}

void rocketnet_platform_server_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you have a server that runs on windows, you can set the priority and affinity of the server thread here
    // THREAD_PRIORITY_TIME_CRITICAL, as flow sets it (flow_platform.cpp:1846-1851)
    rocketnet_platform_network_thread_priority( thread );
}

bool rocketnet_platform_mutex_create( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    return ROCKETNET_PLATFORM_PRIORITY_CALL( mutex_init )( mutex ) == 0;
}

void rocketnet_platform_mutex_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    EnterCriticalSection( (LPCRITICAL_SECTION) &mutex->handle );
}

bool rocketnet_platform_mutex_try_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex && mutex->ok );
    return TryEnterCriticalSection( &mutex->handle ) != 0;
}

void rocketnet_platform_mutex_release( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    LeaveCriticalSection( (LPCRITICAL_SECTION) &mutex->handle );
}

void rocketnet_platform_mutex_destroy( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );

    if ( mutex->ok )
    {
        DeleteCriticalSection( (LPCRITICAL_SECTION) &mutex->handle );
        memset( mutex, 0, sizeof( rocketnet_platform_mutex_t ) );
    }
}

// time

// Sleep() rounds up to the system timer tick (15.6 ms by default) and a
// millisecond cast truncates a sub-millisecond request to Sleep(0), so the
// sleep blocks on a high-resolution waitable timer instead (Windows 10 1803
// and later), set for the whole request, which keeps the system-wide timer
// resolution untouched. rocketnet_platform_sleep_plan converts the request;
// rocketnet_platform_sleep_tail_yields decides the at most 50 us
// SwitchToThread tail when the timer wakes before the request is up. A thread
// rocketnet_platform_thread_create started sleeps on its own timer, created
// with its record (rocketnet_platform_thread_sleep); any other caller holds no
// timer, and its sleep creates one for itself and closes it on return
// (rocketnet_platform_sleep). Where the timer cannot be created,
// SetWaitableTimer fails, or the wait does not return WAIT_OBJECT_0, the
// fallback raises the timer to 1 ms only for the duration of a rounded-up
// Sleep.

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif // #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION

#include <timeapi.h>

#pragma comment( lib, "winmm.lib" )

static HANDLE rocketnet_platform_sleep_timer_create()
{
    return CreateWaitableTimerExW( NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS );
}

// the sleep on a timer: true when the timer waited out the request

static bool rocketnet_platform_sleep_on_timer( HANDLE handle, const rocketnet_platform_sleep_plan_t & plan )
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    QueryPerformanceFrequency( &frequency );
    QueryPerformanceCounter( &start );

    LARGE_INTEGER due;
    due.QuadPart = plan.due_time;
    if ( !SetWaitableTimer( handle, &due, 0, NULL, NULL, FALSE ) || WaitForSingleObject( handle, INFINITE ) != WAIT_OBJECT_0 )
        return false;

    const double elapsed_units = (double) -plan.due_time;
    const double counts = ceil( elapsed_units * (double) frequency.QuadPart / 10000000.0 );
    const LONGLONG end = start.QuadPart + ( counts > 9.0e18 ? (LONGLONG) 9.0e18 : (LONGLONG) counts );
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );

    if ( rocketnet_platform_sleep_tail_yields( (double) ( end - now.QuadPart ) / (double) frequency.QuadPart ) )
    {
        while ( now.QuadPart < end )
        {
            SwitchToThread();
            QueryPerformanceCounter( &now );
        }
    }

    return true;
}

// the sleep on timer, which may be NULL: the fallback serves a request the
// timer cannot

static void rocketnet_platform_sleep_on( HANDLE timer, const rocketnet_platform_sleep_plan_t & plan )
{
    if ( timer && rocketnet_platform_sleep_on_timer( timer, plan ) )
        return;

    // the 1 ms timer period, when the priority calls are enabled, is the
    // process's from rocketnet_platform_init; the fallback makes no timer
    // period call of its own

    Sleep( (DWORD) plan.fallback_milliseconds );
}

void rocketnet_platform_sleep( double time )
{
    const rocketnet_platform_sleep_plan_t plan = rocketnet_platform_sleep_plan( time );

    if ( plan.yield_only )
    {
        Sleep( 0 );
        return;
    }

    const HANDLE timer = rocketnet_platform_sleep_timer_create();
    rocketnet_platform_sleep_on( timer, plan );
    if ( timer )
        CloseHandle( timer );
}

void rocketnet_platform_thread_sleep( rocketnet_platform_thread_t * thread, double time )
{
    rocketnet_assert( thread );

    const rocketnet_platform_sleep_plan_t plan = rocketnet_platform_sleep_plan( time );

    if ( plan.yield_only )
    {
        Sleep( 0 );
        return;
    }

    rocketnet_platform_sleep_on( thread->sleep_timer, plan );
}

void rocketnet_platform_thread_wait( rocketnet_platform_thread_t * thread, double time )
{
    rocketnet_platform_thread_sleep( thread, time );
}

double rocketnet_platform_time()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    return ( (double) ( now.QuadPart - timer_start.QuadPart ) ) / ( (double) ( timer_frequency.QuadPart ) );
}

// sockets

bool rocketnet_platform_client_dual_stack()
{
    return true;
}

int rocketnet_platform_id()
{
    return ROCKETNET_PLATFORM_WINDOWS;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * );

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif // #ifndef SIO_UDP_CONNRESET

#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif // #ifndef SIO_UDP_NETRESET

static volatile LONG rocketnet_platform_udp_reset_refusal_logged = 0;
static volatile LONG64 rocketnet_platform_receive_reset_count = 0;
static volatile LONG64 rocketnet_platform_receive_empty_count = 0;

// windows-udp-connreset audit: every socket rocketnet creates on Windows
// (portable server and client, iocp, rio) turns both ICMP reset reports off
// here, so a hard-killed client never turns into receive errors (DRIVERS.md,
// "Windows connection reset")
bool rocketnet_platform_socket_disable_udp_resets( uintptr_t handle )
{
    bool disabled = true;
    const DWORD codes[2] = { SIO_UDP_CONNRESET, SIO_UDP_NETRESET };
    const char * names[2] = { "SIO_UDP_CONNRESET", "SIO_UDP_NETRESET" };

    for ( int i = 0; i < 2; i++ )
    {
        BOOL report = FALSE;
        DWORD returned_bytes = 0;
        if ( WSAIoctl( (SOCKET) handle, codes[i], &report, sizeof( report ), NULL, 0, &returned_bytes, NULL, NULL ) == SOCKET_ERROR )
        {
            disabled = false;
            if ( InterlockedExchange( &rocketnet_platform_udp_reset_refusal_logged, 1 ) == 0 )
                rocketnet_warn( "%s refused (Winsock %d): receive paths skip reset statuses instead", names[i], WSAGetLastError() );
        }
    }

    return disabled;
}

bool rocketnet_platform_receive_status_is_reset( int status )
{
    switch ( status )
    {
        case WSAECONNRESET:
        case WSAENETRESET:
        case WSAECONNREFUSED:
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
        case ERROR_PORT_UNREACHABLE:
        case ERROR_HOST_UNREACHABLE:
        case ERROR_NETWORK_UNREACHABLE:
        case ERROR_CONNECTION_REFUSED:
            return true;
        default:
            return false;
    }
}

void rocketnet_platform_count_receive_reset()
{
    InterlockedIncrement64( &rocketnet_platform_receive_reset_count );
}

uint64_t rocketnet_platform_receive_resets()
{
    return (uint64_t) InterlockedCompareExchange64( &rocketnet_platform_receive_reset_count, 0, 0 );
}

uint64_t rocketnet_platform_receive_empties()
{
    return (uint64_t) InterlockedCompareExchange64( &rocketnet_platform_receive_empty_count, 0, 0 );
}

int rocketnet_platform_socket_receive_buffer_bytes( rocketnet_platform_socket_t * socket )
{
    if ( !socket )
        return 0;
    int granted = 0;
    int length = sizeof( granted );
    if ( getsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char *) ( &granted ), &length ) != 0 )
        return 0;
    return granted;
}

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, int * status )
{
    rocketnet_platform_socket_t * s = (rocketnet_platform_socket_t *) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );

    if ( !s )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    s->context = context;
    s->send_failures = 0;
    s->send_replay_failures = 0;
    s->connected = false;

    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    // create socket

    s->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;

#if ROCKETNET_DEV
    if ( address->type == ROCKETNET_ADDRESS_IPV6 && !rocketnet_platform_ipv6_socket_available_for_testing )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, s );
        return NULL;
    }
#endif // #if ROCKETNET_DEV

    s->handle = socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( s->handle == INVALID_SOCKET )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, s );
        return NULL;
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    s->lifecycle_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) s->handle, "platform socket" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    // IMPORTANT: tell windows we don't want to receive any connection or network
    // reset reports for this socket, otherwise a receive errors out after a remote endpoint
    // disconnects hard and its ICMP port unreachable comes back
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) s->handle );

    // own the port exclusively: without SO_EXCLUSIVEADDRUSE another local
    // process binds the same port with SO_REUSEADDR and takes its datagrams.
    // set before bind, as the IOCP and RIO transports do.

    BOOL exclusive = TRUE;
    if ( setsockopt( s->handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) ( &exclusive ), sizeof( exclusive ) ) != 0 )
    {
        rocketnet_error( "failed to set socket exclusive address use" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    // set ipv6 sockets as dual stack

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( s->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char *) ( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }

    // increase socket send and receive buffer sizes

    if ( setsockopt( s->handle, SOL_SOCKET, SO_SNDBUF, (char *) ( &send_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    if ( setsockopt( s->handle, SOL_SOCKET, SO_RCVBUF, (char *) ( &receive_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket receive buffer size" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    // bind to port

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        rocketnet_platform_address_to_sockaddr_in6( address, &socket_address );

        if ( bind( s->handle, (sockaddr *) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv6)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }
    else
    {
        sockaddr_in socket_address;
        rocketnet_platform_address_to_sockaddr_in( address, &socket_address );

        if ( bind( s->handle, (sockaddr *) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv4)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }

    // if bound to port 0 find the actual port we got

    sockaddr_in sin4;
    sockaddr_in6 sin6;
    sockaddr * addr = NULL;

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        addr = (sockaddr *) &sin6;

        socklen_t len = sizeof( sin6 );
        if ( getsockname( s->handle, addr, &len ) == -1 )
        {
            rocketnet_error( "failed to get socket address (ipv6)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }

        address->port = rocketnet_platform_ntohs( sin6.sin6_port );
    }
    else
    {
        addr = (sockaddr *) &sin4;

        socklen_t len = sizeof( sin4 );
        if ( getsockname( s->handle, addr, &len ) == -1 )
        {
            rocketnet_error( "failed to get socket address (ipv4)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }

        address->port = rocketnet_platform_ntohs( sin4.sin_port );
    }

    // set non-blocking io

    if ( socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        DWORD nonBlocking = 1;
        if ( ioctlsocket( s->handle, (long) FIONBIO, &nonBlocking ) != 0 )
        {
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }
    else if ( timeout_seconds > 0.0f )
    {
        // set receive timeout
        DWORD tv = (DWORD) rocketnet_platform_timeout_milliseconds( timeout_seconds );
        if ( setsockopt( s->handle, SOL_SOCKET, SO_RCVTIMEO, (const char *) ( &tv ), sizeof( tv ) ) < 0 )
        {
            rocketnet_error( "failed to set socket receive timeout" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }
    else
    {
        // timeout < 0, socket is blocking with no timeout
    }

    // set don't fragment bit

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int val = 1;
        setsockopt( s->handle, IPPROTO_IPV6, IPV6_DONTFRAG, (const char *) &val, sizeof( val ) );
    }
    else
    {
        int val = 1;
        setsockopt( s->handle, IPPROTO_IP, IP_DONTFRAGMENT, (const char *) &val, sizeof( val ) );
    }

    return s;
}

// false only when the lifecycle tracker refused the close: the handle is
// still open, so destroy keeps the memory too

bool rocketnet_platform_socket_close( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

    if ( rocketnet_platform_socket_handle_is_valid( socket->handle ) )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) socket->handle,
                                                     socket->lifecycle_generation ) )
            return false;
        const int closed = closesocket( socket->handle );
        rocketnet_lifecycle_track_close_end( socket->lifecycle_generation, closed == 0 );
#else
        closesocket( socket->handle );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        socket->handle = (rocketnet_platform_socket_handle_t) INVALID_SOCKET;
    }

    return true;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    if ( !rocketnet_platform_socket_close( socket ) )
        return;
    rocketnet_free( socket->context, socket );
}

// A Winsock sendto that answers SOCKET_ERROR drops its datagram: count it on
// the socket, and log the error WSAGetLastError() reports (Winsock leaves
// errno untouched), read by the caller before any other call can replace it.
static void rocketnet_platform_windows_send_failed( rocketnet_platform_socket_t * socket, const rocketnet_address_t * to, int error )
{
    rocketnet_platform_note_send_failure( socket );

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_address_to_string( to, address_string );

    char error_string[256] = { 0 };
    const DWORD length = FormatMessageA( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD) error,
                                         MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ), error_string, (DWORD) sizeof( error_string ), NULL );
    for ( DWORD i = length; i > 0 && ( error_string[i - 1] == '\r' || error_string[i - 1] == '\n' || error_string[i - 1] == ' ' ); --i )
        error_string[i - 1] = '\0';
    rocketnet_debug( "sendto (%s) failed: WSA error %d: %s", address_string, error, error_string );
}

// A connected socket (rocketnet_platform_socket_connect) has one remote endpoint: every
// send goes to it with no destination named, as on the other platforms.

static int rocketnet_platform_sendto( rocketnet_platform_socket_t * socket, const void * packet_data, int packet_bytes, const sockaddr * to, int to_bytes )
{
    if ( socket->connected )
        return send( socket->handle, (const char *) packet_data, packet_bytes, 0 );
    return sendto( socket->handle, (const char *) packet_data, packet_bytes, 0, to, to_bytes );
}

// Connects the socket to its one remote endpoint, so the stack drops every datagram
// from any other source (the client trusts only the server: STANDARD t11).
// The destination is converted as send_packet converts it.

bool rocketnet_platform_socket_connect( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );

    sockaddr_storage storage;
    const int storage_bytes = rocketnet_platform_sockaddr_for( socket->ipv6, input_to, &storage );
    if ( storage_bytes == 0 )
        return false;

    if ( connect( socket->handle, (const sockaddr *) &storage, storage_bytes ) == SOCKET_ERROR )
    {
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_address_to_string( input_to, address_string );
        rocketnet_error( "connect (%s) failed: WSA error %d", address_string, WSAGetLastError() );
        return false;
    }

    socket->connected = true;

    return true;
}

void rocketnet_platform_socket_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, const void * packet_data, int packet_bytes )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );
    rocketnet_assert( input_to->type == ROCKETNET_ADDRESS_IPV6 || input_to->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    // a dual stack ipv6 socket sends ipv4 as its v4-mapped form

    sockaddr_storage storage;
    const int storage_bytes = rocketnet_platform_sockaddr_for( socket->ipv6, input_to, &storage );
    if ( storage_bytes == 0 )
    {
        rocketnet_error( "invalid address type. could not send packet" );
        return;
    }

    int result = rocketnet_platform_sendto( socket, packet_data, packet_bytes, (sockaddr *) ( &storage ), storage_bytes );
    if ( result == SOCKET_ERROR )
    {
        rocketnet_address_t to = *input_to;
        if ( socket->ipv6 && to.type == ROCKETNET_ADDRESS_IPV4 )
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        rocketnet_platform_windows_send_failed( socket, &to, WSAGetLastError() );
    }
}

// one datagram from a Windows socket: its bytes (past max_packet_size for an
// oversize one), or -1 when the socket is drained or the receive failed. A
// zero-length datagram returns 0 when keep_empty is set, so a batch hands it
// to the transport's prefilter and it is counted there (PREFILTER_TOO_SHORT);
// otherwise it is skipped, since 0 from the per-packet call reads as drained,
// and counted (rocketnet_platform_receive_empties)
static int rocketnet_platform_socket_receive_datagram( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size, bool keep_empty )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( max_packet_size > 0 );

    sockaddr_storage sockaddr_from;
    socklen_t from_length;
    uint8_t overflow[65535];
    WSABUF buffers[2];
    buffers[0].buf = (char *) packet_data;
    buffers[0].len = (ULONG) max_packet_size;
    buffers[1].buf = (char *) overflow;
    buffers[1].len = sizeof( overflow );

    // every windows ipv6 socket clears IPV6_V6ONLY at create (the setsockopt
    // in rocketnet_platform_socket_create above), so it is dual
    // stack and a mapped source is an ipv4 client.

    const bool dual_stack = socket->ipv6;

    int result;
receive_again:
    for ( ;; )
    {
        from_length = sizeof( sockaddr_from );

        DWORD bytes_received = 0;
        DWORD flags = 0;

#if ROCKETNET_DEV
        if ( rocketnet_platform_receive_hooked( socket, packet_data, max_packet_size, &sockaddr_from, sizeof( sockaddr_from ), &result ) )
        {
            // Preserve the drained sentinel so batch receive cannot manufacture empty packets.
            if ( result < 0 )
                return -1;
            if ( result == 0 )
                continue;
            break;
        }
#endif // #if ROCKETNET_DEV

        int status = WSARecvFrom( socket->handle, buffers, 2, &bytes_received, &flags, (sockaddr *) &sockaddr_from, &from_length, NULL, NULL );

        if ( status == SOCKET_ERROR )
        {
            int error = WSAGetLastError();

            if ( error == WSAEMSGSIZE )
            {
                result = max_packet_size + 1;
                break;
            }

            // an ICMP report of an earlier send is not the end of the
            // queue: skip it and keep reading, or one dead remote endpoint ends every
            // drain early (windows-udp-connreset audit, portable receive)

            if ( rocketnet_platform_receive_status_is_reset( error ) )
            {
                rocketnet_platform_count_receive_reset();
                continue;
            }

            if ( error != WSAEWOULDBLOCK && error != WSAETIMEDOUT )
            {
                rocketnet_debug( "WSARecvFrom failed with error %d", error );
            }

            return -1;
        }

        result = (int) bytes_received;

        if ( result == 0 && !keep_empty )
        {
            // a zero-length datagram is legal UDP carrying nothing the
            // protocol admits -- and returning 0 here would read as
            // "socket drained", letting byte-less chaff cap an entire
            // drain at one packet. skip it, keep reading:
            // both pumps share this semantic. The skip is counted
            // process-wide (rocketnet_platform_receive_empties), so no
            // dropped datagram goes uncounted.
            InterlockedIncrement64( &rocketnet_platform_receive_empty_count );
            continue;
        }

        break;
    }

    if ( sockaddr_from.ss_family != AF_INET6 && sockaddr_from.ss_family != AF_INET )
    {
        rocketnet_assert( 0 );
        return -1;
    }

    if ( !rocketnet_platform_sockaddr_to_address( &sockaddr_from, dual_stack, from ) )
        goto receive_again;

    rocketnet_assert( result >= 0 );

    return result;
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
    const int result = rocketnet_platform_socket_receive_datagram( socket, from, packet_data, max_packet_size, false );
    return result < 0 ? 0 : result;
}

// The client I/O wake on Windows: a manual-reset event beside an event the
// socket signals on FD_READ (docs/CLIENT-IO.md "The wake" and
// rocketnet-client-io-windows; proofs/tla/RNClientIo.tla's IoWait, IoWake and Wake).
// The wake is level-triggered: a set made while no wait runs leaves the event
// signalled, so the next wait returns at once. FD_READ is re-armed by the
// drain the I/O step runs to a would-block, so no readable datagram is missed.

struct rocketnet_platform_wake_t
{
    void * context;
    rocketnet_platform_socket_handle_t socket_handle;
    WSAEVENT read_event;
    HANDLE wake_event;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    uint64_t read_generation;
    uint64_t wake_generation;
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
};

// docs/CLIENT-IO.md "The wake": the socket's FD_READ event and an unset
// manual-reset wake event (proofs/tla/RNClientIo.tla's Init: wake = FALSE)
rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket, int * status )
{
    rocketnet_platform_wake_t * wake = (rocketnet_platform_wake_t *) rocketnet_malloc( context, sizeof( rocketnet_platform_wake_t ) );
    if ( !wake )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    wake->context = context;
    wake->socket_handle = socket ? socket->handle : INVALID_SOCKET;
    wake->read_event = WSA_INVALID_EVENT;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    wake->read_generation = 0;
    wake->wake_generation = 0;
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( socket )
    {
        wake->read_event = WSACreateEvent();
        if ( wake->read_event == WSA_INVALID_EVENT )
        {
            rocketnet_free( context, wake );
            return NULL;
        }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        wake->read_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) wake->read_event,
                                                                "platform WSA read event" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( WSAEventSelect( (SOCKET) socket->handle, wake->read_event, FD_READ ) != 0 )
        {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) wake->read_event, wake->read_generation ) )
            {
                const BOOL closed = WSACloseEvent( wake->read_event );
                rocketnet_lifecycle_track_close_end( wake->read_generation, closed != 0 );
            }
#else
            WSACloseEvent( wake->read_event );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            rocketnet_free( context, wake );
            return NULL;
        }
    }

    // Event-only wakes are reused by a worker. Auto-reset consumes a wake
    // atomically with wait success, so a concurrent SetEvent cannot be erased
    // by a later ResetEvent. Socket-backed client wakes keep their existing
    // manual-reset behavior.

    wake->wake_event = CreateEvent( NULL, socket ? TRUE : FALSE, FALSE, NULL );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( wake->wake_event )
        wake->wake_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) wake->wake_event,
                                                                "platform wake event" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( wake->wake_event == NULL )
    {
        if ( socket )
        {
            WSAEventSelect( (SOCKET) socket->handle, NULL, 0 );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) wake->read_event, wake->read_generation ) )
            {
                const BOOL closed = WSACloseEvent( wake->read_event );
                rocketnet_lifecycle_track_close_end( wake->read_generation, closed != 0 );
            }
#else
            WSACloseEvent( wake->read_event );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        }

        rocketnet_free( context, wake );
        return NULL;
    }

    return wake;
}

// false only when the lifecycle tracker refused a close: that handle is still
// open, so destroy keeps the memory too

bool rocketnet_platform_wake_close( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );

    if ( wake->read_event != WSA_INVALID_EVENT )
    {
        // the socket's event selection is cleared once: a retry after a
        // refused close must not select on a handle the socket's own close
        // may have released for reuse

        WSAEventSelect( (SOCKET) wake->socket_handle, NULL, 0 );
        wake->socket_handle = (rocketnet_platform_socket_handle_t) INVALID_SOCKET;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) wake->read_event, wake->read_generation ) )
            return false;
        const BOOL read_closed = WSACloseEvent( wake->read_event );
        rocketnet_lifecycle_track_close_end( wake->read_generation, read_closed != 0 );
#else
        WSACloseEvent( wake->read_event );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        wake->read_event = WSA_INVALID_EVENT;
    }

    if ( wake->wake_event != NULL )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) wake->wake_event, wake->wake_generation ) )
            return false;
        const BOOL closed = CloseHandle( wake->wake_event );
        rocketnet_lifecycle_track_close_end( wake->wake_generation, closed != 0 );
#else
        CloseHandle( wake->wake_event );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        wake->wake_event = NULL;
    }

    return true;
}

void rocketnet_platform_wake_destroy( rocketnet_platform_wake_t * wake )
{
    if ( !rocketnet_platform_wake_close( wake ) )
        return;
    rocketnet_free( wake->context, wake );
}

// proofs/tla/RNClientIo.tla's IoWait and IoWake: wait on the wake and, only when
// readable is true, the socket (docs/CLIENT-IO.md "The wake": a full buffer
// waits on the wake alone). The wake event is first in the array, so when both
// are signalled WaitForMultipleObjects reports the wake and the wake is
// consumed (IoWake's wake' = FALSE). The signalled handle is reset before the
// return, so a wake set after the reset is kept for the next wait.
bool rocketnet_platform_wake_wait( rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds )
{
    rocketnet_assert( wake );

    if ( readable && wake->read_event == WSA_INVALID_EVENT )
        return false;
    DWORD timeout_ms = 0;

    if ( timeout_seconds > 0.0 )
    {
        const double ms = ceil( timeout_seconds * 1000.0 );
        timeout_ms = ms < 2147483647.0 ? (DWORD) ms : (DWORD) 2147483647;
    }

    DWORD result;

    if ( readable )
    {
        HANDLE handles[2] = { wake->wake_event, wake->read_event };
        result = WaitForMultipleObjects( 2, handles, FALSE, timeout_ms );
    }
    else
    {
        result = WaitForSingleObject( wake->wake_event, timeout_ms );
    }

    if ( result == WAIT_OBJECT_0 )
    {
        if ( wake->read_event != WSA_INVALID_EVENT )
            ResetEvent( wake->wake_event );
        return true;
    }

    if ( readable && result == WAIT_OBJECT_0 + 1 )
    {
        WSAResetEvent( wake->read_event );
        return true;
    }

    return false;
}

// proofs/tla/RNClientIo.tla's Wake: set the level-triggered wake from any thread
void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    SetEvent( wake->wake_event );
}

#if ROCKETNET_UNREAL_ENGINE
#include "Windows/PostWindowsApi.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif // #if ROCKETNET_UNREAL_ENGINE

#endif // #if POSIX

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    // network order IS host order on a big-endian machine, so the swap is
    // the little-endian case and not the definition
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t) ( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
#endif // #if ROCKETNET_BIG_ENDIAN
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t) ( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
#endif // #if ROCKETNET_BIG_ENDIAN
}

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    memset( &sockaddr4, 0, sizeof( sockaddr4 ) );

    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;

    // A failed parse writes nothing: only hand back the address on success.

    if ( success )
        *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return inet_ntop( AF_INET6, (void *) address, address_string, address_string_size ) != NULL;
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return inet_ntop( AF_INET6, (const void *) address, address_string, socklen_t( address_string_size ) ) != NULL;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

#if ROCKETNET_DEV
bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, rocketnet_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof( hints ) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                rocketnet_platform_sockaddr_in6_to_address( (const sockaddr_in6 *) ( result->ai_addr ), address );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in *) ( result->ai_addr ), address );
                freeaddrinfo( result );
                return true;
            }
            else
            {
                rocketnet_assert( 0 );
                freeaddrinfo( result );
                return false;
            }
        }
    }

    return false;
}
#endif // #if ROCKETNET_DEV

uint16_t rocketnet_platform_preferred_client_port()
{
    return 0;
}

// ----------------------------------------------------------------

#if ROCKETNET_DEV
void rocketnet_platform_set_ipv6_socket_available_for_testing( bool available )
{
    rocketnet_platform_ipv6_socket_available_for_testing = available;
}
#endif // #if ROCKETNET_DEV

bool rocketnet_platform_client_ipv6_available()
{
#if ROCKETNET_DEV
    if ( !rocketnet_platform_ipv6_socket_available_for_testing )
        return false;

    const rocketnet_platform_network_lookups_t * lookups = rocketnet_platform_network_lookups_override.load( std::memory_order_acquire );
    if ( lookups )
        return lookups->ipv6_available();
#endif // #if ROCKETNET_DEV

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    SOCKET probe = socket( AF_INET6, SOCK_DGRAM, IPPROTO_UDP );
    if ( probe == INVALID_SOCKET )
        return false;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const uint64_t probe_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) probe,
                                                                      "platform IPv6 probe socket" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) probe );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) probe, probe_generation ) )
        return false;
    const int closed = closesocket( probe );
    rocketnet_lifecycle_track_close_end( probe_generation, closed == 0 );
#else
    closesocket( probe );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return true;
#else
    int probe = (int) ::socket( AF_INET6, SOCK_DGRAM, IPPROTO_UDP );
    if ( probe < 0 )
        return false;
    ::close( probe );
    return true;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

int rocketnet_platform_client_bind_family( int server_address_type, bool ipv6_available, bool dual_stack )
{
    if ( !ipv6_available )
    {
        if ( server_address_type == ROCKETNET_ADDRESS_IPV6 )
            return ROCKETNET_ADDRESS_NONE;
        return ROCKETNET_ADDRESS_IPV4;
    }

    if ( server_address_type == ROCKETNET_ADDRESS_IPV6 || dual_stack )
        return ROCKETNET_ADDRESS_IPV6;

    return ROCKETNET_ADDRESS_IPV4;
}

// ----------------------------------------------------------------

// Endian-neutral IPv4 sockaddr_in conversion: rocketnet_address_t.data.ipv4
// and sockaddr_in.sin_addr are both four network-order bytes in memory, so
// copying those four bytes directly preserves the address on both little- and
// big-endian hosts.

void rocketnet_platform_address_to_sockaddr_in( const struct rocketnet_address_t * address, struct sockaddr_in * out )
{
    rocketnet_assert( address );
    rocketnet_assert( address->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( out );

    memset( out, 0, sizeof( struct sockaddr_in ) );
    out->sin_family = AF_INET;
    memcpy( &out->sin_addr, address->data.ipv4, 4 );
    out->sin_port = rocketnet_platform_htons( address->port );
}

void rocketnet_platform_sockaddr_in_to_address( const struct sockaddr_in * in, struct rocketnet_address_t * address )
{
    rocketnet_assert( in );
    rocketnet_assert( address );
    memset( address, 0, sizeof( struct rocketnet_address_t ) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    memcpy( address->data.ipv4, &in->sin_addr, 4 );
    address->port = rocketnet_platform_ntohs( in->sin_port );
}

void rocketnet_platform_address_to_sockaddr_in6( const struct rocketnet_address_t * address, struct sockaddr_in6 * out )
{
    rocketnet_assert( address );
    rocketnet_assert( address->type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_assert( out );

    memset( out, 0, sizeof( struct sockaddr_in6 ) );
    out->sin6_family = AF_INET6;
    for ( int i = 0; i < 8; ++i )
        ( (uint16_t *) &out->sin6_addr )[i] = rocketnet_platform_htons( address->data.ipv6[i] );
    out->sin6_port = rocketnet_platform_htons( address->port );
}

void rocketnet_platform_sockaddr_in6_to_address( const struct sockaddr_in6 * in, struct rocketnet_address_t * address )
{
    rocketnet_assert( in );
    rocketnet_assert( address );

    memset( address, 0, sizeof( struct rocketnet_address_t ) );
    address->type = ROCKETNET_ADDRESS_IPV6;
    for ( int i = 0; i < 8; ++i )
        address->data.ipv6[i] = rocketnet_platform_ntohs( ( (const uint16_t *) &in->sin6_addr )[i] );
    address->port = rocketnet_platform_ntohs( in->sin6_port );
}

int rocketnet_platform_sockaddr_from_address( const struct rocketnet_address_t * address, struct sockaddr_storage * storage )
{
    memset( storage, 0, sizeof( sockaddr_storage ) );

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        rocketnet_platform_address_to_sockaddr_in6( address, (sockaddr_in6 *) storage );
        return (int) sizeof( sockaddr_in6 );
    }

    if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_platform_address_to_sockaddr_in( address, (sockaddr_in *) storage );
        return (int) sizeof( sockaddr_in );
    }

    return 0;
}

int rocketnet_platform_sockaddr_for( bool ipv6, const struct rocketnet_address_t * input_to, struct sockaddr_storage * storage )
{
    rocketnet_address_t to = *input_to;

    if ( ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        if ( to.type != ROCKETNET_ADDRESS_IPV6 )
            return 0;
    }
    else if ( to.type != ROCKETNET_ADDRESS_IPV4 )
    {
        return 0;
    }

    return rocketnet_platform_sockaddr_from_address( &to, storage );
}

bool rocketnet_platform_sockaddr_to_address( const struct sockaddr_storage * storage, bool dual_stack, struct rocketnet_address_t * address )
{
    if ( storage->ss_family == AF_INET6 )
    {
        rocketnet_platform_sockaddr_in6_to_address( (const sockaddr_in6 *) storage, address );
        return rocketnet_address_accept_received_source( address, dual_stack );
    }

    if ( storage->ss_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in *) storage, address );
        return true;
    }

    return false;
}

// ----------------------------------------------------------------

// the batch pump seam's portable implementation: a platform without a
// batch override loops the per-packet calls, so every platform serves the
// seam and a call site never carries an #ifdef. a platform that defines ROCKETNET_PLATFORM_HAS_BATCH_PUMP in
// its own section above supplies all three functions itself.

#if !defined( ROCKETNET_PLATFORM_HAS_BATCH_PUMP )

const char * rocketnet_platform_socket_pump( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    (void) socket;
    return "portable";
}

void rocketnet_platform_socket_send_packets( rocketnet_platform_socket_t * socket, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( sends || num_packets == 0 );

    for ( int i = 0; i < num_packets; i++ )
    {
        rocketnet_platform_socket_send_packet( socket, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
    }
}

int rocketnet_platform_socket_receive_packets( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_stride > 0 );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( max_packets > 0 );

    int num_packets = 0;
    while ( num_packets < max_packets )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        // a zero-length datagram joins the batch at 0 bytes, so the
        // transport's prefilter counts it (PREFILTER_TOO_SHORT) rather than
        // it vanishing here
        int bytes = rocketnet_platform_socket_receive_datagram( socket, &from[num_packets], packet_data + (size_t) num_packets * (size_t) packet_stride, packet_stride, true );
        if ( bytes < 0 )
            break;
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        int bytes = rocketnet_platform_socket_receive_packet( socket, &from[num_packets], packet_data + (size_t) num_packets * (size_t) packet_stride, packet_stride );
        if ( bytes == 0 )
            break;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        packet_bytes[num_packets] = bytes;
        num_packets++;
    }

    return num_packets;
}

#endif // #if !defined( ROCKETNET_PLATFORM_HAS_BATCH_PUMP )

// ----------------------------------------------------------------

// the client I/O wake's portable fallback: a platform that does not define
// ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE and has no platform wake of its own
// (BSD has the event-only pipe above; macOS and iOS build the socket-capable
// kqueue wake above; Windows builds the event wake above) has no wake.
// create returns NULL, destroy asserts its argument is NULL, wait returns
// false and set does nothing.

#if !defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE ) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_MAC && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_IOS && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket, int * status )
{
    (void) context;
    (void) socket;
    (void) status;
    return NULL;
}

bool rocketnet_platform_wake_close( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( !wake );
    return true;
}

void rocketnet_platform_wake_destroy( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( !wake );
}

bool rocketnet_platform_wake_wait( rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds )
{
    (void) wake;
    (void) readable;
    (void) timeout_seconds;
    return false;
}

void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    (void) wake;
}

#endif // #if !defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE ) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_MAC && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_IOS && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// ----------------------------------------------------------------

// pump-request socket create: the transport ladder's entry into the
// platform pump selection. pump NULL or "auto" is the pre-existing
// behavior -- offer the platform's batch pump. "portable" forces the portable pump: the
// request wins and no batch decision matters. "msgx" REQUIRES the darwin
// batch pump: a socket that cannot carry it (including missing symbols)
// refuses loudly here so a pinned rung refuses rather than
// silently degrading. The first real batch call is the functional probe;
// its own live-refusal pause-and-reprobe discipline remains the
// transport's internal behavior below the ladder.

rocketnet_platform_socket_t * rocketnet_platform_socket_create_with_pump( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, const char * pump, int * status )
{
    const bool require_msgx = pump && strcmp( pump, "msgx" ) == 0;

    // refusals here log at WARN, not ERROR: this call sits under the transport
    // ladder, whose own summary decides whether a refusal was a fall (warn)
    // or a create failure (error) -- an msgx refusal on a falling auto
    // ladder is normal operation, not an error line in every log

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_MAC

    if ( require_msgx )
    {
        rocketnet_warn( "msgx pump is not available on this platform" );
        return NULL;
    }

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS

    // a forced-portable request suppresses the batch pump entirely, and no
    // batch call can read a queued datagram before the caller does

    const bool force_portable = pump && strcmp( pump, "portable" ) == 0;

    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_internal( context, address, socket_type, timeout_seconds, send_buffer_size, receive_buffer_size, !force_portable, status );
    if ( !socket )
        return NULL;

    if ( require_msgx && !socket->msgx )
    {
        rocketnet_warn( "msgx pump is not available on this socket" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    return socket;

#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS

    // every other platform has exactly one pump, so "portable" and auto
    // are the same create

    return rocketnet_platform_socket_create( context, address, socket_type, timeout_seconds, send_buffer_size, receive_buffer_size, status );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
}

uint64_t rocketnet_platform_socket_allocation_bytes()
{
    return rocketnet_memory_allocations( { sizeof( rocketnet_platform_socket_t ) } );
}

uint64_t rocketnet_platform_thread_allocation_bytes()
{
    // the thread record holds its shim: one allocation, live from create to
    // destroy, so the peak is the same whenever the worker begins
    return rocketnet_memory_allocations( { sizeof( thread_record_t ) } );
}

uint64_t rocketnet_platform_wake_allocation_bytes()
{
#if defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )
    return rocketnet_memory_allocations( { sizeof( rocketnet_platform_wake_t ) } );
#else
    return 0;
#endif // #if defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )
}
