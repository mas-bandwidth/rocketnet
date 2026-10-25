/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_platform.h"
#include "rocketnet_constants.h"
#include "rocketnet_route.h"
#include <atomic>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
#include <map>
#include <string>
#endif // #if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
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

#if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )

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

#endif // #if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )

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
#endif
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
        return UINT32_C(0xFFFFFFFE);
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

// The buffer loop of the Windows connection-type query (get_connection_type,
// below), over a query callback so it is unit-tested with a refusing allocator
// and a fake query (test_unit_driver_create_out_of_memory.cpp); compiled on
// every platform. Each attempt asks rocketnet_malloc for bytes and runs the
// query on the buffer. A refused allocation ends the fetch at once: the query
// never sees a NULL buffer. An overflow answer, with bytes set to the size the
// query needs, retries; ROCKETNET_PLATFORM_ADAPTERS_ATTEMPTS bounds the
// attempts in all, so the loop ends whatever the allocator and the query
// answer. Every end but success frees the buffer and returns NULL; a refusal
// and running out of attempts are logged.

extern void * rocketnet_malloc( void * context, size_t bytes );
extern void rocketnet_free( void * context, void * p );

#define ROCKETNET_PLATFORM_ADAPTERS_ATTEMPTS 3

enum
{
    ROCKETNET_PLATFORM_ADAPTERS_OK,
    ROCKETNET_PLATFORM_ADAPTERS_OVERFLOW,
    ROCKETNET_PLATFORM_ADAPTERS_ERROR,
};

void * rocketnet_platform_adapters_fetch( void * context, unsigned long bytes, int (*query)( void * buffer, unsigned long * bytes ) )
{
    // ROCKETNET_PLATFORM_ADAPTERS_ATTEMPTS bounds this loop: each pass makes one
    // allocation and one query; a refused allocation and an error answer return
    // at once, and only an overflow answer goes round again
    for ( int attempt = 0; attempt < ROCKETNET_PLATFORM_ADAPTERS_ATTEMPTS; ++attempt )
    {
        void * buffer = rocketnet_malloc( context, bytes );
        if ( !buffer )
        {
            rocketnet_warn( "connection type: allocation of %lu bytes refused; the connection type is unknown", bytes );
            return NULL;
        }
        const int answer = query( buffer, &bytes );
        if ( answer == ROCKETNET_PLATFORM_ADAPTERS_OK )
            return buffer;
        rocketnet_free( context, buffer );
        if ( answer != ROCKETNET_PLATFORM_ADAPTERS_OVERFLOW )
            return NULL;
    }
    rocketnet_warn( "connection type: buffer still too small after %d attempts; the connection type is unknown", ROCKETNET_PLATFORM_ADAPTERS_ATTEMPTS );
    return NULL;
}

// test seam for the client bind-family choice: true unless a test turns it
// off, in which case an IPv6 socket create behaves as it does on a host with
// no IPv6 stack. the client's availability probe reads the same flag.
static bool rocketnet_platform_ipv6_socket_available_for_testing = true;

// test seam for the network lookups (rocketnet_internal.h): NULL runs the real
// connection-type lookup and IPv6 probe; a unit test's table replaces both.
static std::atomic<const rocketnet_platform_network_lookups_t *> rocketnet_platform_network_lookups_override{ NULL };

#if ROCKETNET_TEST_SEAMS
void rocketnet_platform_set_network_lookups_for_testing( const rocketnet_platform_network_lookups_t * lookups )
{
    rocketnet_platform_network_lookups_override.store( lookups, std::memory_order_release );
}
#endif // #if ROCKETNET_TEST_SEAMS

// Network-thread priority (docs/CLIENT-IO.md, TRANSPORTS.md). The calls and their
// values are the flow library's (flow_platform.cpp), unchanged; each platform
// block below fills the real call table. A unit test replaces the table
// (test_unit_thread_priority.cpp). A refused call is counted process-wide and
// logged once per call kind; it never fails the caller.

static std::atomic<bool> rocketnet_platform_thread_priority_on{ true };
static std::atomic<uint64_t> rocketnet_platform_thread_priority_refusal_count{ 0 };
static std::atomic<bool> rocketnet_platform_thread_priority_logged[ROCKETNET_PLATFORM_PRIORITY_CALL_COUNT];
static std::atomic<const rocketnet_platform_priority_calls_t *> rocketnet_platform_priority_calls_override{ NULL };

static const rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls()
{
    const rocketnet_platform_priority_calls_t * calls = rocketnet_platform_priority_calls_override.load( std::memory_order_acquire );
    return calls ? calls : rocketnet_platform_priority_calls_real();
}

#if ROCKETNET_TEST_SEAMS
void rocketnet_platform_set_priority_calls_for_testing( const rocketnet_platform_priority_calls_t * calls )
{
    rocketnet_platform_priority_calls_override.store( calls, std::memory_order_release );
}
#endif // #if ROCKETNET_TEST_SEAMS

const rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls_current()
{
    return rocketnet_platform_priority_calls();
}

#if ROCKETNET_TEST_SEAMS
void rocketnet_platform_thread_priority_reset_logged_for_testing()
{
    for ( int i = 0; i < ROCKETNET_PLATFORM_PRIORITY_CALL_COUNT; ++i )
        rocketnet_platform_thread_priority_logged[i].store( false, std::memory_order_relaxed );
}
#endif // #if ROCKETNET_TEST_SEAMS

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
// sched_get_priority_min( SCHED_RR ) ), on the created thread
static int rocketnet_platform_real_set_sched( rocketnet_platform_thread_t * thread, int policy, int priority )
{
    struct sched_param param;
    param.sched_priority = priority;
    return pthread_setschedparam( thread->handle, policy, &param );
}

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

static int rocketnet_platform_real_mutex_init( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );
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

static int rocketnet_platform_real_platform_start( void )
{
    return 0;
}

static void* rocketnet_platform_real_create_waitable_timer( unsigned flags )
{
    (void) flags;
    return (void*) 1;
}

static const rocketnet_platform_priority_calls_t rocketnet_platform_priority_calls_real_table =
{
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

static void rocketnet_platform_network_thread_priority( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    if ( !rocketnet_platform_thread_priority_enabled() )
        return;
    const int priority = sched_get_priority_min( SCHED_RR );
    const int result = rocketnet_platform_priority_calls()->set_sched( thread, SCHED_RR, priority );
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

static inline void rocketnet_platform_note_send_failure( rocketnet_platform_socket_t * socket )
{
    rocketnet_platform_atomic_add_u64( &socket->send_failures, 1 );
}

uint64_t rocketnet_platform_socket_send_failures( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    return rocketnet_platform_atomic_load_u64( &socket->send_failures );
}

uint64_t rocketnet_platform_socket_send_replay_failures( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    return rocketnet_platform_atomic_load_u64( &socket->send_replay_failures );
}

// Test-only receive seam, compiled into the unit suite's hook library only
// (CMakeLists.txt, rocketnet_test_hooks). While a hook is set for one socket,
// the single-packet receive path on that socket takes each datagram from the
// hook instead of the kernel, so a unit case drives the drop-and-retry of a
// refused source with no socket. Other sockets never see the hook.

#if defined(ROCKETNET_RECEIVE_TEST_HOOKS)

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

#endif // #if defined(ROCKETNET_RECEIVE_TEST_HOOKS)
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

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#define ROCKETNET_PLATFORM_SEND_SEAM 1
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
    while ( __atomic_exchange_n( &rocketnet_platform_test_lock, 1, __ATOMIC_ACQUIRE ) != 0 ) {}
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

typedef void (*rocketnet_platform_create_receive_hook_t)( int handle );

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

#endif // #if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// ---------------------------------------------------

// Server receive buffer sizing (rocketnet_internal.h, USAGE.md "Receive
// buffer sizing"). A server updating at update_hz must hold one update's
// inbound packets at max_clients in its socket: the portable transport's
// receive worker stops reading while every receive bank waits for the next
// update (tla/RNPortableRx.tla HoldOnFull), so the kernel holds the rest.

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

void rocketnet_server_receive_buffer_granted( const char * name, int requested_bytes, int granted_bytes )
{
    // one line per server socket: what the kernel granted and what was asked
    // for. A grant below the request is a warning that names the limit to
    // raise, because the kernel drops what arrives past the grant.
    if ( granted_bytes >= requested_bytes )
    {
        rocketnet_info( "server %s: socket receive buffer granted %d of %d requested bytes",
                        name ? name : "socket", granted_bytes, requested_bytes );
        return;
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    const char * limit = "net.core.rmem_max";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const char * limit = "the system socket buffer limit";
#else
    const char * limit = "kern.ipc.maxsockbuf";
#endif
    rocketnet_warn( "server %s: socket receive buffer granted %d of %d requested bytes; packets arriving between updates past the grant are dropped by the kernel: raise %s",
                    name ? name : "socket", granted_bytes, requested_bytes, limit );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

// linux and the BSDs. one POSIX section: the sockets, threads and mutexes
// are identical; everything linux-only in here (netlink, the wireless
// ioctl, PMTU discovery, the raw clock) sits behind __linux__ with the
// portable shape beside it.

#include "rocketnet_platform.h"
#include <limits.h>
#include "rocketnet_address.h"

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if defined(__linux__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/wireless.h>
#include <alloca.h>
#include <sys/eventfd.h>
#include <poll.h>
#endif // #if defined(__linux__)
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
#include <poll.h>
#endif

// the BSDs have no CLOCK_MONOTONIC_RAW; CLOCK_MONOTONIC is their
// unadjusted-rate monotonic clock and serves the same reads
#if !defined(CLOCK_MONOTONIC_RAW)
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif // #if !defined(CLOCK_MONOTONIC_RAW)

static int connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

static int get_connection_type();

static double time_start;

bool rocketnet_platform_init()
{
    if ( rocketnet_platform_priority_calls()->platform_start() != 0 )
        return false;
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    time_start = (double) ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    const rocketnet_platform_network_lookups_t * lookups = rocketnet_platform_network_lookups_override.load( std::memory_order_acquire );
    connection_type = lookups ? lookups->connection_type() : get_connection_type();
    return true;
}

void rocketnet_platform_term()
{
    // ...
}

int rocketnet_platform_getenv( const char * var, char * buffer, size_t size )
{
    rocketnet_assert( var && buffer && size > 0 );
#if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
    int test_result = 0;
    if ( rocketnet_platform_test_environment_getenv( var, buffer, size, &test_result ) )
        return test_result;
#endif // #if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
    buffer[0] = '\0';
    const char * value = getenv( var );
    if ( !value )
        return 0;
    const size_t length = strlen( value );
    if ( length >= size )
        return -1;
    memcpy( buffer, value, length + 1 );
    return 1;
}

// ---------------------------------------------------

int rocketnet_platform_id()
{
    return ROCKETNET_PLATFORM;
}

int rocketnet_platform_connection_type()
{
    return connection_type;
}

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    // network order IS host order on a big-endian machine, so the swap is
    // the little-endian case and not the definition. Swapping
    // unconditionally reversed every ipv6 group and every port on the one
    // architecture where this function is supposed to do nothing (#127,
    // found by the #111 big-endian leg).
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
#endif // #if ROCKETNET_BIG_ENDIAN
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
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
    return inet_ntop( AF_INET6, (const void*)address, address_string, socklen_t( address_string_size ) ) != NULL;
}

bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, rocketnet_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof(hints) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                sockaddr_in6 * addr_ipv6 = (sockaddr_in6 *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV6;
                for ( int i = 0; i < 8; ++i )
                {
                    address->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
                }
                address->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in *)( result->ai_addr ), address );
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

uint16_t rocketnet_platform_preferred_client_port()
{
    return 0;
}

bool rocketnet_platform_client_dual_stack()
{
#if defined(__OpenBSD__)
    // openbsd rejects v4-mapped addresses by design: no socket is ever
    // dual stack. the client transport binds the server's family instead.
    return false;
#else
    return true;
#endif // #if defined(__OpenBSD__)
}

// ---------------------------------------------------

double rocketnet_platform_time()
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    double current = (double) ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    return current - time_start;
}

void rocketnet_platform_sleep( double time )
{
    if ( !( time > 0.0 ) )
        return;
    const rocketnet_platform_sleep_split_t split = rocketnet_platform_sleep_split( time );
    if ( split.seconds > 0 )
    {
        sleep( split.seconds );
    }
    if ( split.microseconds > 0 )
    {
        usleep( split.microseconds );
    }
}

// ---------------------------------------------------

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket );

static bool rocketnet_platform_set_socket_buffer_size( int handle, int option, int requested_bytes, const char * direction )
{
    int size = requested_bytes;
    if ( setsockopt( handle, SOL_SOCKET, option, (char*)( &size ), sizeof( int ) ) == 0 )
        return true;
    while ( size > 1 )
    {
        size /= 2;
        if ( setsockopt( handle, SOL_SOCKET, option, (char*)( &size ), sizeof( int ) ) == 0 )
        {
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
    if ( getsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &granted ), &length ) != 0 )
        return 0;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    // linux doubles the request for its bookkeeping and reports the double
    granted /= 2;
#endif
    return granted;
}

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size )
{
    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    rocketnet_platform_socket_t * socket = (rocketnet_platform_socket_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );

    if ( !socket )
        return NULL;

    socket->context = context;

    // create socket

    socket->type = socket_type;

    socket->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;
    socket->send_failures = 0;
    socket->send_replay_failures = 0;
    socket->connected = false;

    if ( address->type == ROCKETNET_ADDRESS_IPV6 && !rocketnet_platform_ipv6_socket_available_for_testing )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }

    socket->handle = ::socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( socket->handle < 0 )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }

    // enable dual stack for ipv6 sockets. openbsd forbids clearing
    // IPV6_V6ONLY (no v4-mapped addresses, by design), so there the socket
    // stays ipv6-only and rocketnet_platform_client_dual_stack() says so.

#if !defined(__OpenBSD__)
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( socket->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
#endif // #if !defined(__OpenBSD__)

    // increase socket send and receive buffer sizes.
    //
    // linux silently clamps an over-limit request to net.core.wmem_max /
    // rmem_max (defaults near 208KB), so this call has always meant "the
    // largest buffer the kernel grants, up to the request". the BSDs refuse
    // an over-limit request outright instead (netbsd: ENOBUFS above
    // kern.sbmax, default 256KB), so on refusal walk down to what the
    // kernel grants and say so; fail only when nothing can be set. on
    // linux the first attempt succeeds and behavior is unchanged.

    if ( !rocketnet_platform_set_socket_buffer_size( socket->handle, SO_SNDBUF, send_buffer_size, "send" ) )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    if ( !rocketnet_platform_set_socket_buffer_size( socket->handle, SO_RCVBUF, receive_buffer_size, "receive" ) )
    {
        rocketnet_error( "failed to set socket receive buffer size" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    // bind to port

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( sockaddr_in6 ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( address->data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( address->port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv6)" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        sockaddr_in socket_address;
        rocketnet_platform_address_to_sockaddr_in( address, &socket_address );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv4)" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // if bound to port 0 find the actual port we got

    if ( address->port == 0 )
    {
        if ( address->type == ROCKETNET_ADDRESS_IPV6 )
        {
            sockaddr_in6 sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv6)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin6_port );
        }
        else
        {
            sockaddr_in sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv4)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin_port );
        }
    }

    // set non-blocking io and receive timeout

    if ( socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        // non-blocking, keeping the descriptor's other status flags
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
        // blocking with receive timeout
        struct timeval tv = rocketnet_platform_timeval_from_seconds( timeout_seconds );
        if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) ) < 0 )
        {
            rocketnet_error( "failed to set socket receive timeout" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        // blocking with no timeout
    }

    // set don't fragment bit

#if defined(__linux__)
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int val = IPV6_PMTUDISC_DO;
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val) );
    }
    else
    {
        int val = IP_PMTUDISC_DO;
        setsockopt( socket->handle, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val) );
    }
#else
    // the BSDs express DF via the RFC 3542 options where they have them
    // (FreeBSD both, OpenBSD and NetBSD IPv6 only); same shape the darwin
    // section uses, and like the linux calls above, refusal is tolerated
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
    #if defined(IPV6_DONTFRAG)
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_DONTFRAG, &val, sizeof(val) );
    #endif // #if defined(IPV6_DONTFRAG)
    }
    else
    {
    #if defined(IP_DONTFRAG)
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val) );
    #endif // #if defined(IP_DONTFRAG)
    }
#endif // #if defined(__linux__)

#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
    rocketnet_platform_test_register( socket );
#endif

    return socket;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
    rocketnet_platform_test_unregister( socket );
#endif
    if ( rocketnet_platform_socket_handle_is_valid( socket->handle ) )
    {
        close( socket->handle );
        socket->handle = -1;
    }
    rocketnet_free( socket->context, socket );
}

// A connected socket (rocketnet_platform_socket_connect) has one remote endpoint: every
// send goes to it with no destination named, since a BSD stack refuses a
// sendto that names one on a connected socket (EISCONN). An unconnected
// socket names each datagram's destination.

static inline ssize_t rocketnet_platform_sendto( rocketnet_platform_socket_t * socket, const void * packet_data, int packet_bytes, const sockaddr * to, socklen_t to_bytes )
{
    if ( socket->connected )
        return send( socket->handle, packet_data, (size_t) packet_bytes, 0 );
    return sendto( socket->handle, packet_data, (size_t) packet_bytes, 0, to, to_bytes );
}

// Connects the socket to its one remote endpoint, so the kernel drops every datagram
// from any other source (the client trusts only the server: STANDARD t11).
// The destination is converted as send_packet converts it.

bool rocketnet_platform_socket_connect( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );

    rocketnet_address_t to = *input_to;
    sockaddr_storage storage;
    memset( &storage, 0, sizeof(storage) );
    socklen_t storage_bytes = 0;

    if ( socket->ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        if ( to.type != ROCKETNET_ADDRESS_IPV6 )
            return false;
        sockaddr_in6 * socket_address = (sockaddr_in6*) &storage;
        socket_address->sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
            ( (uint16_t*) &socket_address->sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        socket_address->sin6_port = rocketnet_platform_htons( to.port );
        storage_bytes = sizeof(sockaddr_in6);
    }
    else
    {
        if ( to.type != ROCKETNET_ADDRESS_IPV4 )
            return false;
        rocketnet_platform_address_to_sockaddr_in( &to, (sockaddr_in*) &storage );
        storage_bytes = sizeof(sockaddr_in);
    }

    if ( connect( socket->handle, (const sockaddr*) &storage, storage_bytes ) != 0 )
    {
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_address_to_string( input_to, address_string );
        rocketnet_error( "connect (%s) failed: %s", address_string, strerror( errno ) );
        return false;
    }

    socket->connected = true;
    return true;
}

void rocketnet_platform_socket_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * to_input, const void * packet_data, int packet_bytes )
{
    rocketnet_assert( socket );
    rocketnet_assert( to_input );
    rocketnet_assert( to_input->type == ROCKETNET_ADDRESS_IPV6 || to_input->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
    int injected_error = 0;
    if ( rocketnet_platform_send_failure_should_inject( socket, &injected_error ) )
    {
        errno = injected_error;
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_address_to_string( to_input, address_string );
        rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
        rocketnet_platform_note_send_failure( socket );
        return;
    }
#endif

    rocketnet_address_t to = *to_input;

    if ( socket->ipv6 )
    {
        // socket is dual stack ipv4 and ipv6

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( to.port );
        int result = int( rocketnet_platform_sendto( socket, packet_data, packet_bytes, (sockaddr*)( &socket_address ), sizeof(sockaddr_in6) ) );
        if ( result < 0 )
        {
            char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            rocketnet_address_to_string( &to, address_string );
            rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
            rocketnet_platform_note_send_failure( socket );
        }
    }
    else
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            sockaddr_in socket_address;
            rocketnet_platform_address_to_sockaddr_in( &to, &socket_address );
            int result = int( rocketnet_platform_sendto( socket, packet_data, packet_bytes, (sockaddr*)( &socket_address ), sizeof(sockaddr_in) ) );
            if ( result < 0 )
            {
                char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
                rocketnet_address_to_string( &to, address_string );
                rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
                rocketnet_platform_note_send_failure( socket );
            }
        }
        else
        {
            rocketnet_error( "invalid address. could not send packet" );
        }
    }
}

// UDP's ordinary weather on the receive path (TRANSPORTS.md, ICMP errors):
// a client socket is connected to its server, so the kernel reports an ICMP
// error (port or host unreachable) to its next receive, and a full buffer
// pool reports ENOBUFS. each is a one-shot report, so a drain that skips it
// always reaches a datagram or EAGAIN.

static bool rocketnet_platform_receive_weather( int error )
{
    return error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH ||
           error == EHOSTDOWN || error == ENETDOWN || error == ENOBUFS || error == EINTR;
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( max_packet_size > 0 );

    sockaddr_storage sockaddr_from;
    socklen_t from_length;

    // openbsd keeps ipv6 sockets ipv6-only (see socket create), so there a
    // mapped source is spoofed and dropped; everywhere else the socket is
    // dual stack and a mapped source is an ipv4 client.

#if defined(__OpenBSD__)
    const bool dual_stack = false;
#else
    const bool dual_stack = socket->ipv6;
#endif

    int result;
receive_again:
    for ( ;; )
    {
        from_length = sizeof( sockaddr_from );

        int flags = socket->type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING ? MSG_DONTWAIT : 0;
        flags |= MSG_TRUNC;

#if defined(ROCKETNET_RECEIVE_TEST_HOOKS)
        if ( rocketnet_platform_receive_hooked( socket, packet_data, max_packet_size, &sockaddr_from, sizeof( sockaddr_from ), &result ) )
        {
            if ( result < 0 )
                return 0;
            if ( result == 0 )
                continue;
            break;
        }
#endif // #if defined(ROCKETNET_RECEIVE_TEST_HOOKS)

        result = int( recvfrom( socket->handle, (char*) packet_data, (size_t) max_packet_size, flags, (sockaddr*) &sockaddr_from, &from_length ) );

        if ( result < 0 )
        {
            // an ICMP report to a connected client socket, or buffer
            // weather: one-shot, says nothing about the datagrams queued
            // behind it, so skip it and keep draining
            if ( rocketnet_platform_receive_weather( errno ) )
                continue;
            if ( errno != EAGAIN )
            {
                rocketnet_debug( "recvfrom failed with error %d", errno );
            }
            return 0;
        }

        if ( result == 0 )
        {
            // a zero-length datagram is legal UDP carrying nothing the
            // protocol admits -- and returning 0 here would read as
            // "socket drained", letting byte-less chaff cap an entire
            // drain at one packet (#100's review). skip it, keep reading:
            // both pumps share this semantic.
            continue;
        }

        // result may exceed max_packet_size: MSG_TRUNC reports the true
        // datagram size. deliver it, not the truncated prefix, so the
        // transport's prefilter drops it as PREFILTER_TOO_LARGE.
        break;
    }

    if ( sockaddr_from.ss_family == AF_INET6 )
    {
        sockaddr_in6 * addr_ipv6 = (sockaddr_in6*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );

        // a dropped source is skipped like a zero-length datagram: keep
        // reading, so spoofed chaff never reads as "socket drained".
        if ( !rocketnet_address_accept_received_source( from, dual_stack ) )
            goto receive_again;
    }
    else if ( sockaddr_from.ss_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in*) &sockaddr_from, from );
    }
    else
    {
        rocketnet_assert( 0 );
        return 0;
    }

    rocketnet_assert( result >= 0 );

    return result;
}

// ---------------------------------------------------

#if defined(__linux__)

// The client I/O wake on Linux: an eventfd beside the socket. The wake is
// level-triggered (docs/CLIENT-IO.md "The wake"; tla/RNClientIo.tla's
// IoWake, IoWait and Wake), so a set made while no wait runs makes the next
// wait return at once. wait polls the eventfd (and the socket only when
// readable is true); a readable eventfd is drained by reading its 8-byte
// counter, consuming the wake(s).

struct rocketnet_platform_wake_t
{
    void * context;
    int socket_fd;
    int event_fd;
};

rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket )
{
    rocketnet_platform_wake_t * wake = (rocketnet_platform_wake_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_wake_t ) );
    if ( !wake )
        return NULL;
    wake->context = context;
    wake->socket_fd = socket ? socket->handle : -1;
    wake->event_fd = eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
    if ( wake->event_fd < 0 )
    {
        rocketnet_free( context, wake );
        return NULL;
    }
    return wake;
}

void rocketnet_platform_wake_destroy( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    if ( wake->event_fd >= 0 )
        close( wake->event_fd );
    rocketnet_free( wake->context, wake );
}

bool rocketnet_platform_wake_wait( rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds )
{
    rocketnet_assert( wake );
    if ( readable && wake->socket_fd < 0 )
        return false;
    struct pollfd fds[2];
    int nfds = 0;
    fds[nfds].fd = wake->event_fd;
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
    const int poll_result = poll( fds, (nfds_t) nfds, timeout_ms );
    if ( poll_result <= 0 )
        return false;
    for ( int i = 0; i < nfds; i++ )
    {
        if ( !( fds[i].revents & POLLIN ) )
            continue;
        if ( fds[i].fd == wake->event_fd )
        {
            uint64_t value;
            ssize_t n = read( wake->event_fd, &value, sizeof( value ) );
            (void) n;
        }
        return true;
    }
    return false;
}

void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    uint64_t one = 1;
    ssize_t n = write( wake->event_fd, &one, sizeof( one ) );
    (void) n;
}

#endif // #if defined(__linux__)

// ---------------------------------------------------

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

// BSD server workers need an event-only reusable wake. A nonblocking pipe
// works on FreeBSD, OpenBSD and NetBSD (OpenBSD lacks EVFILT_USER). Keep
// socket-backed wakes unsupported so clients retain their synchronous path.
struct rocketnet_platform_wake_t
{
    void * context;
    int read_fd;
    int write_fd;
};

static bool rocketnet_platform_bsd_wake_fd_flags( int fd )
{
    const int status = fcntl( fd, F_GETFL, 0 );
    const int flags = fcntl( fd, F_GETFD, 0 );
    return status >= 0 && flags >= 0 &&
           fcntl( fd, F_SETFL, status | O_NONBLOCK ) == 0 &&
           fcntl( fd, F_SETFD, flags | FD_CLOEXEC ) == 0;
}

rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket )
{
    if ( socket )
        return NULL;
    rocketnet_platform_wake_t * wake = (rocketnet_platform_wake_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_wake_t ) );
    if ( !wake )
        return NULL;
    wake->context = context;
    int fds[2];
    if ( pipe( fds ) != 0 )
    {
        rocketnet_free( context, wake );
        return NULL;
    }
    if ( !rocketnet_platform_bsd_wake_fd_flags( fds[0] ) || !rocketnet_platform_bsd_wake_fd_flags( fds[1] ) )
    {
        close( fds[0] );
        close( fds[1] );
        rocketnet_free( context, wake );
        return NULL;
    }
    wake->read_fd = fds[0];
    wake->write_fd = fds[1];
    return wake;
}

void rocketnet_platform_wake_destroy( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    close( wake->read_fd );
    close( wake->write_fd );
    rocketnet_free( wake->context, wake );
}

bool rocketnet_platform_wake_wait( rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds )
{
    rocketnet_assert( wake );
    if ( readable )
        return false;
    struct pollfd fd = { wake->read_fd, POLLIN, 0 };
    const int timeout_ms = timeout_seconds > 0.0 ? (int) ceil( timeout_seconds * 1000.0 ) : 0;
    if ( poll( &fd, 1, timeout_ms ) <= 0 || !( fd.revents & POLLIN ) )
        return false;
    uint8_t buffer[64];
    return read( wake->read_fd, buffer, sizeof(buffer) ) > 0;
}

void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    const uint8_t one = 1;
    ssize_t n = write( wake->write_fd, &one, sizeof(one) );
    (void) n; // EAGAIN means the pipe already holds a pending wake.
}

#endif // ROCKETNET_PLATFORM_BSD

// ---------------------------------------------------

#if defined(__linux__)

// The Linux client's batch send/receive (#client-io-linux-mmsg). The B
// I/O step sends a handed-off ring whole (RNClientIo IoSend) and drains
// into the receive buffer (RNClientIo IoReceive); the batch pump seam's
// portable loop (above) does that one syscall per packet, and these add
// sendmmsg/recvmmsg so a frame crosses the socket in as few syscalls as
// the platform allows. The chunk size keeps the stack arrays small and
// bounded. Address conversion and the per-entry checks mirror the
// per-packet send_packet/receive_packet, so a packet is sent, dropped or
// delivered exactly as the portable path would.
//
// docs/CLIENT-IO.md The design (the batch seam, the I/O step) and Measured;
// tla/RNClientIo.tla IoSend and IoReceive.

#define ROCKETNET_MSS_BATCH 64

// fill a sockaddr for one destination, mirroring send_packet's dual-stack
// conversion. returns the sockaddr length, or 0 for an address the socket
// cannot carry (logged exactly as the per-packet path logs it).

static socklen_t rocketnet_platform_mmsg_fill_sockaddr( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, sockaddr_storage * storage )
{
    rocketnet_address_t to = *input_to;

    if ( socket->ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 * socket_address = (sockaddr_in6*) storage;
        memset( socket_address, 0, sizeof(sockaddr_in6) );
        socket_address->sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address->sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address->sin6_port = rocketnet_platform_htons( to.port );
        return sizeof(sockaddr_in6);
    }

    if ( to.type != ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_error( "invalid address. could not send packet" );
        return 0;
    }

    sockaddr_in * socket_address = (sockaddr_in*) storage;
    rocketnet_platform_address_to_sockaddr_in( &to, socket_address );
    return sizeof(sockaddr_in);
}

// convert one received sockaddr, mirroring receive_packet's mapped-address
// normalization. returns false for a family the transport does not speak.

bool rocketnet_platform_mmsg_read_sockaddr( rocketnet_platform_socket_t * socket, const sockaddr_storage * storage, rocketnet_address_t * from )
{
    if ( storage->ss_family == AF_INET6 )
    {
        const sockaddr_in6 * addr_ipv6 = (const sockaddr_in6*) storage;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (const uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
#if defined(__OpenBSD__)
        const bool dual_stack = false;
#else
        const bool dual_stack = socket->ipv6;
#endif
        return rocketnet_address_accept_received_source( from, dual_stack );
    }

    if ( storage->ss_family == AF_INET )
    {
        const sockaddr_in * addr_ipv4 = (const sockaddr_in*) storage;
        rocketnet_platform_sockaddr_in_to_address( addr_ipv4, from );
        return true;
    }

    return false;
}

void rocketnet_platform_socket_send_packets_mmsg( rocketnet_platform_socket_t * socket, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( sends || num_packets == 0 );

    int base = 0;
    while ( base < num_packets )
    {
        // build one chunk. an address the socket cannot carry is skipped
        // here, already logged by the fill -- the same drop the per-packet
        // path takes. map[] remembers each chunk entry's index in sends,
        // so a refused entry can be named when it is dropped.

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

            socklen_t sockaddr_bytes = rocketnet_platform_mmsg_fill_sockaddr( socket, send->to, &addresses[count] );
            if ( sockaddr_bytes != 0 )
            {
                memset( &msg[count], 0, sizeof(msg[count]) );
                iov[count].iov_base = const_cast<uint8_t*>( send->packet_data ); // the send only reads it
                iov[count].iov_len = (size_t) send->packet_bytes;
                // a connected socket names no destination (rocketnet_platform_sendto)
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

            // EAGAIN and the ordinary send weather: the socket cannot take
            // more now. the per-packet send path logs and drops; drop the
            // unsent tail the same way.
            if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS )
            {
                for ( int i = sent; i < count; i++ )
                {
                    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
                    const rocketnet_platform_send_t * send = &sends[map[i]];
                    rocketnet_address_to_string( send->to, address_string );
                    rocketnet_debug( "sendmmsg (%s) failed: %s", address_string, strerror( errno ) );
                    rocketnet_platform_note_send_failure( socket );
                }
                sent = count;
                break;
            }

            // No message was sent in this call, so the first unsent entry is
            // the one the kernel refused. Drop it as the per-packet path does,
            // then retry the remaining entries in order.
            const int error = errno;
            char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            const rocketnet_platform_send_t * send = &sends[map[sent]];
            rocketnet_address_to_string( send->to, address_string );
            rocketnet_debug( "sendmmsg (%s) failed: %s", address_string, strerror( error ) );
            rocketnet_platform_note_send_failure( socket );
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
            memset( &msg[i], 0, sizeof(msg[i]) );
            iov[i].iov_base = packet_data + (size_t) ( chunk_base + i ) * (size_t) packet_stride;
            iov[i].iov_len = (size_t) packet_stride;
            msg[i].msg_hdr.msg_name = &addresses[i];
            msg[i].msg_hdr.msg_namelen = sizeof(addresses[i]);
            msg[i].msg_hdr.msg_iov = &iov[i];
            msg[i].msg_hdr.msg_iovlen = 1;
        }

        errno = 0;
        // MSG_TRUNC: each msg_len is the datagram's true size, as the
        // per-packet path reports it
        int result = recvmmsg( socket->handle, msg, (unsigned int) want, MSG_DONTWAIT | MSG_TRUNC, NULL );
        if ( result < 0 )
        {
            // EINTR, an ICMP report to a connected client socket, or buffer
            // weather: skip it and keep draining, as the per-packet receive does
            if ( rocketnet_platform_receive_weather( errno ) )
                continue;
            if ( errno == EAGAIN || errno == EWOULDBLOCK )
                return total;

            // outside ordinary weather: stop draining, logged like the
            // per-packet receive
            rocketnet_debug( "recvmmsg failed with error %d", errno );
            return total;
        }

        if ( result == 0 )
            return total;

        // walk the chunk in place. a zero-length datagram or a family the
        // transport does not speak (the per-packet path delivers neither)
        // is compacted out: the next kept message's bytes move down to the
        // gap. rare by nature and bounded by the chunk. an oversize
        // datagram is kept with its true size and the prefix the kernel
        // wrote, so the transport's prefilter counts it PREFILTER_TOO_LARGE.

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
            {
                memmove( target, source, (size_t) copy_bytes );
            }
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

#endif // #if defined(__linux__)

// ---------------------------------------------------

struct thread_shim_data_t
{
    void * context;
    void * real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function;
    rocketnet_platform_thread_t * thread;
};

// the platform thread the calling thread runs as, for thread_wait; NULL on a
// thread that thread_create did not start
static thread_local rocketnet_platform_thread_t * rocketnet_platform_thread_self = NULL;

static void* thread_function_shim( void * data )
{
    rocketnet_assert( data );
    thread_shim_data_t * shim_data = (thread_shim_data_t*) data;
    void * context = shim_data->context;
    void * real_thread_data = shim_data->real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function = shim_data->real_thread_function;
    rocketnet_platform_thread_self = shim_data->thread;
    rocketnet_free( context, data );
    real_thread_function( real_thread_data );
    return NULL;
}

static bool rocketnet_platform_thread_wake_init( rocketnet_platform_thread_t * thread )
{
    // thread_wait's deadline is on CLOCK_MONOTONIC, so the wait is immune to
    // wall-clock steps
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
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg )
{
    rocketnet_platform_thread_t * thread = (rocketnet_platform_thread_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_thread_t) );

    if ( !thread )
        return NULL;

    thread->context = context;

    thread_shim_data_t * shim_data = (thread_shim_data_t*) rocketnet_malloc( context, sizeof(thread_shim_data_t) );
    if ( !shim_data )
    {
        rocketnet_free( context, thread );
        return NULL;
    }
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;
    shim_data->thread = thread;

    thread->woken = false;
    if ( !rocketnet_platform_thread_wake_init( thread ) )
    {
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
        return NULL;
    }

    if ( pthread_create( &thread->handle, NULL, thread_function_shim, shim_data ) != 0 )
    {
        pthread_cond_destroy( &thread->wake_cond );
        pthread_mutex_destroy( &thread->wake_mutex );
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
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
    pthread_cond_destroy( &thread->wake_cond );
    pthread_mutex_destroy( &thread->wake_mutex );
    rocketnet_free( thread->context, thread );
}

void rocketnet_platform_thread_wait( double time )
{
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_self;
    if ( !thread )
    {
        rocketnet_platform_sleep( time );
        return;
    }
    // the condition variable runs on CLOCK_MONOTONIC (thread_wake_init)
    // whole seconds and nanoseconds converted apart, clamped to a day, so no
    // conversion overflows a 32-bit long
    if ( !( time > 0.0 ) )
        time = 0.0;
    if ( time > 86400.0 )
        time = 86400.0;
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
}

void rocketnet_platform_thread_wake( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    pthread_mutex_lock( &thread->wake_mutex );
    thread->woken = true;
    pthread_cond_broadcast( &thread->wake_cond );
    pthread_mutex_unlock( &thread->wake_mutex );
}

void rocketnet_platform_client_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you have linux clients, you can adjust the priority of the client thread here.
    // SCHED_RR at sched_get_priority_min( SCHED_RR ), as flow sets it (flow_platform.cpp:537-543)
    rocketnet_platform_network_thread_priority( thread );
}

void rocketnet_platform_server_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you have linux servers, you can adjust the priority of the server thread here.
    // SCHED_RR at sched_get_priority_min( SCHED_RR ), as flow sets it (flow_platform.cpp:545-551)
    rocketnet_platform_network_thread_priority( thread );
}

// ---------------------------------------------------

bool rocketnet_platform_mutex_create( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    return rocketnet_platform_priority_calls()->mutex_init( mutex ) == 0;
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
        memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );
    }
}

// ---------------------------------------------------

extern void * rocketnet_global_context;

template <typename T> struct rocketnet_vector_t
{
    T * data;
    int length;
    int reserved;

    inline rocketnet_vector_t( int reserve_count = 0 )
    {
        rocketnet_assert( reserve_count >= 0 );
        data = 0;
        length = 0;
        reserved = 0;
        if ( reserve_count > 0 )
        {
            reserve( reserve_count );
        }
    }

    inline ~rocketnet_vector_t()
    {
        clear();
    }

    inline void clear()
    {
        if ( data )
        {
            rocketnet_free( rocketnet_global_context, data );
        }
        data = NULL;
        length = 0;
        reserved = 0;
    }

    inline const T & operator [] ( int i ) const
    {
        rocketnet_assert( data );
        rocketnet_assert( i >= 0 && i < length );
        return *( data + i );
    }

    inline T & operator [] ( int i )
    {
        rocketnet_assert( data );
        rocketnet_assert( i >= 0 && i < length );
        return *( data + i );
    }

    bool reserve( int size )
    {
        if ( size < 0 )
            return false;
        if ( size <= reserved )
            return true;
        int capacity = reserved ? reserved : 1;
        while ( capacity < size )
        {
            const int growth = capacity / 2 + 1;
            if ( capacity > INT_MAX - growth )
            {
                capacity = size;
                break;
            }
            capacity += growth;
        }
        if ( (size_t) capacity > SIZE_MAX / sizeof(T) )
            return false;
        T * new_data = (T*) rocketnet_malloc( rocketnet_global_context, (size_t) capacity * sizeof(T) );
        if ( !new_data )
            return false;
        if ( data )
            memcpy( new_data, data, (size_t) length * sizeof(T) );
        memset( new_data + length, 0, (size_t) ( capacity - length ) * sizeof(T) );
        rocketnet_free( rocketnet_global_context, data );
        data = new_data;
        reserved = capacity;
        return true;
    }

    bool resize( int size )
    {
        if ( !reserve( size ) )
            return false;
        length = size;
        return true;
    }

    T * add()
    {
        if ( length == INT_MAX || !reserve( length + 1 ) )
            return NULL;
        return &data[length++];
    }

    T * add( const T & t )
    {
        T * p = add();
        if ( p )
            *p = t;
        return p;
    }
};


// ---------------------------------------------------

#if defined(__linux__)

struct rocketnet_iftable_t
{
    uint32_t if_index;
    int connection_type;
};

static int classify_connection_type( rocketnet_vector_t<rocketnet_ifforward4_t> * route_table, rocketnet_vector_t<rocketnet_iftable_t> * interface_list, const rocketnet_address_t * addr )
{
    const uint32_t ip = rocketnet_route_ip( addr->data.ipv4 );
    rocketnet_ifforward4_t * best_match = NULL;
    for ( int i = 0; i < route_table->length; i++ )
    {
        rocketnet_ifforward4_t * route = &( ( *route_table )[i] );
        if ( better_match( route, best_match, ip ) )
        {
            best_match = route;
        }
    }

    rocketnet_iftable_t * outgoing_interface = NULL;

    if ( best_match )
    {
        for ( int i = 0; i < interface_list->length; i++ )
        {
            rocketnet_iftable_t * iface = &( (* interface_list )[i] );
            if ( iface->if_index == best_match->if_index )
            {
                outgoing_interface = iface;
                break;
            }
        }
    }

    if ( outgoing_interface )
    {
        return outgoing_interface->connection_type;
    }
    else
    {
        return ROCKETNET_CONNECTION_TYPE_WIRED;
    }
}

// reads the kernel route table replies over netlink until an NLMSG_DONE
// message ends the dump. receive is called for each read; in the real probe
// it is recvmsg, and tests pass a fake. returns true when the dump was read
// in full; false when a receive failed or returned zero (the caller falls
// back to ROCKETNET_CONNECTION_TYPE_UNKNOWN).
bool rocketnet_platform_read_route_table(
    int sock,
    struct sockaddr_nl * kernel,
    rocketnet_vector_t<rocketnet_ifforward4_t> * route_table,
    ssize_t (*receive)( int, struct msghdr *, int ) )
{
    bool done = false;
    while ( !done )
    {
        struct msghdr rtnl_reply;
        struct iovec io_reply;

        memset( &io_reply, 0, sizeof( io_reply ) );
        memset( &rtnl_reply, 0, sizeof( rtnl_reply ) );

        const size_t IFLIST_REPLY_BUFFER = 8192;
        char reply[IFLIST_REPLY_BUFFER];
        io_reply.iov_base = reply;
        io_reply.iov_len = IFLIST_REPLY_BUFFER;
        rtnl_reply.msg_iov = &io_reply;
        rtnl_reply.msg_iovlen = 1;
        rtnl_reply.msg_name = kernel;
        rtnl_reply.msg_namelen = sizeof( *kernel );

        ssize_t len = receive( sock, &rtnl_reply, 0 );

        // a receive interrupted by a signal is retried once; the other exits
        // are named below.
        if ( len == -1 && errno == EINTR )
        {
            len = receive( sock, &rtnl_reply, 0 );
        }

        if ( len > 0 )
        {
            for ( struct nlmsghdr * msg_ptr = (struct nlmsghdr *)( reply ); NLMSG_OK( msg_ptr, len ); msg_ptr = NLMSG_NEXT( msg_ptr, len ) )
            {
                if ( msg_ptr->nlmsg_type == 24 )
                {
                    rocketnet_ifforward4_t route;
                    if ( parse_kernel_route( msg_ptr, &route ) )
                    {
                        if ( !route_table->add( route ) )
                        {
                            // allocation failure: the dump cannot be kept.
                            return false;
                        }
                    }
                }
                else if ( msg_ptr->nlmsg_type == 3 )
                {
                    // NLMSG_DONE: the dump is complete.
                    done = true;
                }
            }
        }
        else
        {
            // len is 0 or -1: no data read; the dump cannot be completed.
            return false;
        }
    }
    // the loop exited on NLMSG_DONE: the route table was read in full.
    return true;
}

static int get_connection_type()
{
    rocketnet_vector_t<rocketnet_iftable_t> interface_list;
    rocketnet_vector_t<rocketnet_ifforward4_t> route_table;

    rocketnet_debug( "getting network info" );

    // get interfaces
    {
        struct ifaddrs * ifaddr;

        if ( getifaddrs( &ifaddr ) == -1 )
        {
            rocketnet_warn( "failed to get network interface addresses" );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        int sock = -1;

        if ( ( sock = socket( AF_INET, SOCK_STREAM, 0 ) ) == -1 )
        {
            freeifaddrs( ifaddr );
            rocketnet_warn( "failed to get network interface addresses" );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        for ( struct ifaddrs * i = ifaddr; i != NULL; i = i->ifa_next )
        {
            struct ifreq req;
            memset( &req, 0, sizeof( req ) );
            strncpy( req.ifr_name, i->ifa_name, IFNAMSIZ - 1 );

            if ( ioctl( sock, SIOCGIFINDEX, &req ) == -1 )
            {
                continue;
            }

            uint32_t if_index = (uint32_t) req.ifr_ifindex;

            // check for duplicates
            bool duplicate = false;
            for ( int j = 0; j < interface_list.length; j++ )
            {
                if ( interface_list[j].if_index == if_index )
                {
                    duplicate = true;
                    break;
                }
            }
            if ( duplicate )
            {
                continue;
            }

            rocketnet_iftable_t * interface_entry = interface_list.add();
            if ( !interface_entry )
            {
                close( sock );
                freeifaddrs( ifaddr );
                return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
            }
            interface_entry->if_index = if_index;

            struct iwreq pwrq;
            memset( &pwrq, 0, sizeof( pwrq ) );
            strncpy( pwrq.ifr_name, i->ifa_name, IFNAMSIZ - 1 );

            if ( ioctl( sock, SIOCGIWNAME, &pwrq ) == -1 )
            {
                interface_entry->connection_type = ROCKETNET_CONNECTION_TYPE_WIRED;
            }
            else
            {
                interface_entry->connection_type = ROCKETNET_CONNECTION_TYPE_WIFI;
            }
        }

        close( sock );

        freeifaddrs( ifaddr );
    }

    // get kernel route table
    {
        // open netlink socket
        
        pid_t pid = getpid();

        int sock = socket( AF_NETLINK, SOCK_RAW, NETLINK_ROUTE );
        if ( sock == -1 )
        {
            rocketnet_warn( "failed to open netlink socket" );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        struct sockaddr_nl local;
        memset( &local, 0, sizeof( local ) );
        local.nl_family = AF_NETLINK;
        local.nl_pid = 0;
        local.nl_groups = 0;

        if ( bind( sock, (struct sockaddr *)( &local ), sizeof( local ) ) < 0 )
        {
            rocketnet_warn( "failed to bind netlink socket" );
            close( sock );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        // prepare route table request
        typedef struct nl_req_s nl_req_t;  
        struct nl_req_s
        {
            struct nlmsghdr hdr;
            struct rtgenmsg gen;
        };

        nl_req_t req;
        memset( &req, 0, sizeof( req ) );
        req.hdr.nlmsg_len = NLMSG_LENGTH( sizeof( struct rtgenmsg ) );
        req.hdr.nlmsg_type = RTM_GETROUTE;
        req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP; 
        req.hdr.nlmsg_seq = 1;
        req.hdr.nlmsg_pid = (__u32) pid;
        req.gen.rtgen_family = AF_INET; 

        struct iovec io;
        io.iov_base = &req;
        io.iov_len = req.hdr.nlmsg_len;

        struct sockaddr_nl kernel;
        memset( &kernel, 0, sizeof( kernel ) );
        kernel.nl_family = AF_NETLINK;
        kernel.nl_groups = 0;

        struct msghdr rtnl_msg;
        memset( &rtnl_msg, 0, sizeof( rtnl_msg ) );
        rtnl_msg.msg_iov = &io;
        rtnl_msg.msg_iovlen = 1;
        rtnl_msg.msg_name = &kernel;
        rtnl_msg.msg_namelen = sizeof( kernel );

        // send request
        sendmsg( sock, (struct msghdr *)( &rtnl_msg ), 0 );

        // read reply; a receive that fails or returns zero ends the probe
        // instead of spinning, and the connection type falls back to UNKNOWN.
        if ( !rocketnet_platform_read_route_table( sock, &kernel, &route_table, recvmsg ) )
        {
            close( sock );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        close( sock );
    }

    rocketnet_address_t address;
    address.port = 80;
    address.data.ipv4[0] = 8;
    address.data.ipv4[1] = 8;
    address.data.ipv4[2] = 8;
    address.data.ipv4[3] = 8;

    return classify_connection_type( &route_table, &interface_list, &address );
}

#else // #if defined(__linux__)

// the BSDs: no netlink and no wireless-extensions ioctl to classify with.
// UNKNOWN is the honest answer and is already the init fallback everywhere.

static int get_connection_type()
{
    return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
}

#endif // #if defined(__linux__)

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// mac or ios

#include "rocketnet_address.h"

#define __APPLE_USE_RFC_3542

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <sys/event.h>
#include <time.h>

extern void * rocketnet_malloc( void * context, size_t bytes );

extern void rocketnet_free( void * context, void * p );

// ---------------------------------------------------

// The darwin batch pump (#100): sendmsg_x/recvmsg_x, Darwin's batch UDP
// syscalls -- private but stable, used by shipping engines, and measured on
// the test machine (see bench/probe_darwin.c) to support the exact server-pump
// shape: unconnected sockets, a per-message destination on send, source
// address and received length filled per message on receive.
//
// The private-API risk is handled by never depending on them: the symbols
// are resolved by dlsym at init (no link-time reference at all -- weaker
// even than a weak import), each socket's first batch call is its
// functional probe, any refusal at runtime degrades that socket to the
// portable per-packet loop mid-flight without losing a packet. iOS never
// resolves them: dlsym of a private symbol is an App Store review finding,
// so the batch pump is never offered and iOS is portable by construction.
//
// struct msghdr_x is from xnu bsd/sys/socket_private.h, defined locally
// because no public SDK header carries it; the layout is the syscall ABI.

struct rocketnet_msghdr_x_t
{
    void *          msg_name;
    socklen_t       msg_namelen;
    struct iovec *  msg_iov;
    int             msg_iovlen;
    void *          msg_control;
    socklen_t       msg_controllen;
    int             msg_flags;
    size_t          msg_datalen;
};

typedef ssize_t (*rocketnet_sendmsg_x_fn)( int s, const struct rocketnet_msghdr_x_t * msgp, u_int cnt, int flags );
typedef ssize_t (*rocketnet_recvmsg_x_fn)( int s, struct rocketnet_msghdr_x_t * msgp, u_int cnt, int flags );

static rocketnet_sendmsg_x_fn rocketnet_sendmsg_x = NULL;
static rocketnet_recvmsg_x_fn rocketnet_recvmsg_x = NULL;

// the seam batch: how many messages one syscall carries. matches the
// transport's own batch shape; chunking inside the pump keeps the stack
// arrays small and bounded.

#define ROCKETNET_MSGX_BATCH 32

// ---------------------------------------------------

static int connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

static mach_timebase_info_data_t timebase_info;

static uint64_t time_start;

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
bool rocketnet_platform_mac_primary_interface_matches( const void * primary_interface_name, const void * bsd_name )
{
    if ( !primary_interface_name || !bsd_name )
        return false;
    if ( CFGetTypeID( (CFTypeRef) primary_interface_name ) != CFStringGetTypeID() ||
         CFGetTypeID( (CFTypeRef) bsd_name ) != CFStringGetTypeID() )
        return false;
    return CFStringCompare( (CFStringRef) primary_interface_name, (CFStringRef) bsd_name, 0 ) == kCFCompareEqualTo;
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

bool rocketnet_platform_init()
{
    if ( rocketnet_platform_priority_calls()->platform_start() != 0 )
        return false;

    mach_timebase_info( &timebase_info );

    time_start = mach_absolute_time();

#if TARGET_OS_OSX
    // resolve the batch pump syscalls once. absence is not an error: every
    // socket that fails this resolution simply runs the portable pump.
    rocketnet_sendmsg_x = (rocketnet_sendmsg_x_fn) dlsym( RTLD_DEFAULT, "sendmsg_x" );
    rocketnet_recvmsg_x = (rocketnet_recvmsg_x_fn) dlsym( RTLD_DEFAULT, "recvmsg_x" );
#endif // #if TARGET_OS_OSX

    connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

    // SystemConfiguration's dynamic store is macOS-only; iOS has the framework
    // but not this key, so the lookup below yields nothing and the sandbox
    // objects to the attempt. iOS keeps the UNKNOWN connection type set above.
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_IOS

    SCDynamicStoreRef dynamic_store = SCDynamicStoreCreate( kCFAllocatorDefault, CFSTR( "FindCurrentInterfaceIpMac" ), NULL, NULL );
    CFPropertyListRef global = dynamic_store ? SCDynamicStoreCopyValue( dynamic_store, CFSTR( "State:/Network/Global/IPv4" ) ) : NULL;
    if ( global )
    {
        const void * primary_interface_name = CFDictionaryGetValue( (CFDictionaryRef)( global ), CFSTR( "PrimaryInterface" ) );
        // Skip the lookup when the dynamic-store entry is missing or malformed.
        if ( primary_interface_name && CFGetTypeID( (CFTypeRef) primary_interface_name ) == CFStringGetTypeID() )
        {
            CFArrayRef interfaces = SCNetworkInterfaceCopyAll();
            CFIndex count = interfaces ? CFArrayGetCount( interfaces ) : 0;
            for ( CFIndex i = 0; i < count; i++ )
            {
                SCNetworkInterfaceRef interface = (SCNetworkInterfaceRef)( CFArrayGetValueAtIndex( interfaces, i ) );
                CFStringRef bsd_name = SCNetworkInterfaceGetBSDName( interface );
                if ( rocketnet_platform_mac_primary_interface_matches( primary_interface_name, bsd_name ) )
                {
                    CFStringRef interface_type = SCNetworkInterfaceGetInterfaceType( interface );
                    if ( CFStringCompare( interface_type, kSCNetworkInterfaceTypeEthernet, 0 ) == kCFCompareEqualTo )
                    {
                        connection_type = ROCKETNET_CONNECTION_TYPE_WIRED;
                    }
                    else if ( CFStringCompare( interface_type, kSCNetworkInterfaceTypeIEEE80211, 0 ) == kCFCompareEqualTo )
                    {
                        connection_type = ROCKETNET_CONNECTION_TYPE_WIFI;
                    }
                    else if ( CFStringCompare( interface_type, kSCNetworkInterfaceTypeWWAN, 0 ) == kCFCompareEqualTo )
                    {
                        connection_type = ROCKETNET_CONNECTION_TYPE_CELLULAR;
                    }
                    break;
                }
            }
            if ( interfaces )
                CFRelease( interfaces );
        }
        CFRelease( global );
    }
    if ( dynamic_store )
        CFRelease( dynamic_store );

#endif // ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_IOS

    return true;
}

void rocketnet_platform_term()
{
    // ...
}

int rocketnet_platform_getenv( const char * var, char * buffer, size_t size )
{
    rocketnet_assert( var && buffer && size > 0 );
#if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
    int test_result = 0;
    if ( rocketnet_platform_test_environment_getenv( var, buffer, size, &test_result ) )
        return test_result;
#endif // #if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
    buffer[0] = '\0';
    const char * value = getenv( var );
    if ( !value )
        return 0;
    const size_t length = strlen( value );
    if ( length >= size )
        return -1;
    memcpy( buffer, value, length + 1 );
    return 1;
}

// ---------------------------------------------------

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    // network order IS host order on a big-endian machine, so the swap is
    // the little-endian case and not the definition. Swapping
    // unconditionally reversed every ipv6 group and every port on the one
    // architecture where this function is supposed to do nothing (#127,
    // found by the #111 big-endian leg).
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
#endif // #if ROCKETNET_BIG_ENDIAN
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
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
    return inet_ntop( AF_INET6, (const void*)address, address_string, socklen_t( address_string_size ) ) != NULL;
}

// ---------------------------------------------------

bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, rocketnet_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof(hints) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                sockaddr_in6 * addr_ipv6 = (sockaddr_in6 *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV6;
                for ( int i = 0; i < 8; ++i )
                {
                    address->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
                }
                address->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in *)( result->ai_addr ), address );
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

uint16_t rocketnet_platform_preferred_client_port()
{
    return 0;
}

bool rocketnet_platform_client_dual_stack()
{
    return true;
}

// ---------------------------------------------------

int rocketnet_platform_id()
{
    // iOS shares this implementation but reports its own build platform.
    return ROCKETNET_PLATFORM;
}

int rocketnet_platform_connection_type()
{
    return connection_type;
}

double rocketnet_platform_time()
{
    uint64_t current = mach_absolute_time();
    return ( (double) ( current - time_start ) ) * ( (double) timebase_info.numer ) / ( (double) timebase_info.denom ) / 1000000000.0;
}

void rocketnet_platform_sleep( double time )
{
    if ( !( time > 0.0 ) )
        return;
    const rocketnet_platform_sleep_split_t split = rocketnet_platform_sleep_split( time );
    if ( split.seconds > 0 )
    {
        sleep( split.seconds );
    }
    if ( split.microseconds > 0 )
    {
        usleep( split.microseconds );
    }
}

// ---------------------------------------------------

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket );

// the pump request (#106): the plain create offers the batch pump (auto);
// a forced-portable create SUPPRESSES the batch pump entirely. Neither form reads the socket
// at create: the first real batch call is the probe and keeps what it reads.

#define ROCKETNET_PLATFORM_PUMP_MODE_AUTO             0
#define ROCKETNET_PLATFORM_PUMP_MODE_FORCE_PORTABLE   1

static rocketnet_platform_socket_t * rocketnet_platform_socket_create_internal( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, int pump_mode );

int rocketnet_platform_socket_receive_buffer_bytes( rocketnet_platform_socket_t * socket )
{
    if ( !socket )
        return 0;
    int granted = 0;
    socklen_t length = sizeof( granted );
    if ( getsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &granted ), &length ) != 0 )
        return 0;
    return granted;
}

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size )
{
    return rocketnet_platform_socket_create_internal( context, address, socket_type, timeout_seconds, send_buffer_size, receive_buffer_size, ROCKETNET_PLATFORM_PUMP_MODE_AUTO );
}

static rocketnet_platform_socket_t * rocketnet_platform_socket_create_internal( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, int pump_mode )
{
    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    rocketnet_platform_socket_t * socket = (rocketnet_platform_socket_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );

    if ( !socket )
        return NULL;

    socket->context = context;

    socket->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;
    socket->send_failures = 0;
    socket->send_replay_failures = 0;
    socket->connected = false;

    socket->msgx = false;

    socket->msgx_down_until = 0.0;

    // create socket

    if ( address->type == ROCKETNET_ADDRESS_IPV6 && !rocketnet_platform_ipv6_socket_available_for_testing )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }

    socket->handle = ::socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( socket->handle < 0 )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }

    // Darwin UDP sends can return EPIPE and raise SIGPIPE. Keep the failure
    // local to this socket so sendto and sendmsg_x report/count it instead
    // of terminating the embedding application. Do not change its signal
    // handlers or the disposition of unrelated sockets.
    const int no_sigpipe = 1;
    if ( setsockopt( socket->handle, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof( no_sigpipe ) ) != 0 )
    {
        rocketnet_error( "failed to suppress socket SIGPIPE" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    // setup socket as dual stack ipv4/ipv6

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( socket->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // increase socket send and receive buffer sizes

    if ( setsockopt( socket->handle, SOL_SOCKET, SO_SNDBUF, (char*)( &send_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    // a request above kern.ipc.maxsockbuf is refused, not clamped: walk down
    // by halves to what the kernel grants (a server's request is sized from
    // its max_clients and may pass the limit); the server reads the grant
    // back and names the limit (rocketnet_server_receive_buffer_granted)
    {
        int size = receive_buffer_size;
        while ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &size ), sizeof( int ) ) != 0 )
        {
            if ( size <= 1 )
            {
                rocketnet_error( "failed to set socket receive buffer size" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            size /= 2;
        }
    }

    // Darwin chooses an ephemeral port for an IPv6 wildcard without checking
    // whether an IPv4 socket already owns that port. Both binds then coexist,
    // and IPv4 traffic goes to the IPv4 socket instead of this dual-stack
    // socket. Reserve the requested port in the IPv4 namespace first. Binding
    // the IPv6 wildcard to that explicit port while the reservation is held
    // makes it own both namespaces after the reservation closes.

    uint16_t bind_port = address->port;
    int ipv4_port_reservation = -1;
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        bool wildcard = true;
        for ( int i = 0; i < 8; i++ )
            wildcard &= address->data.ipv6[i] == 0;
        if ( wildcard )
        {
            ipv4_port_reservation = ::socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
            if ( ipv4_port_reservation < 0 )
            {
                rocketnet_error( "failed to create ipv4 port reservation for dual stack socket" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            sockaddr_in reservation_address;
            memset( &reservation_address, 0, sizeof(reservation_address) );
            reservation_address.sin_family = AF_INET;
            reservation_address.sin_addr.s_addr = INADDR_ANY;
            reservation_address.sin_port = rocketnet_platform_htons( bind_port );
            if ( bind( ipv4_port_reservation, (sockaddr*) &reservation_address, sizeof(reservation_address) ) < 0 )
            {
                rocketnet_error( "failed to reserve ipv4 port for dual stack socket" );
                close( ipv4_port_reservation );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            if ( bind_port == 0 )
            {
                socklen_t reservation_length = sizeof(reservation_address);
                if ( getsockname( ipv4_port_reservation, (sockaddr*) &reservation_address, &reservation_length ) < 0 )
                {
                    rocketnet_error( "failed to get reserved ipv4 port for dual stack socket" );
                    close( ipv4_port_reservation );
                    rocketnet_platform_socket_destroy( socket );
                    return NULL;
                }
                bind_port = rocketnet_platform_ntohs( reservation_address.sin_port );
            }
        }
    }

    // bind to port

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( sockaddr_in6 ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( address->data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( bind_port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv6)" );
            if ( ipv4_port_reservation >= 0 )
                close( ipv4_port_reservation );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
        if ( ipv4_port_reservation >= 0 )
        {
            close( ipv4_port_reservation );
            address->port = bind_port;
        }
    }
    else
    {
        sockaddr_in socket_address;
        rocketnet_platform_address_to_sockaddr_in( address, &socket_address );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv4)" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // if bound to port 0 find the actual port we got

    if ( address->port == 0 )
    {
        if ( address->type == ROCKETNET_ADDRESS_IPV6 )
        {
            sockaddr_in6 sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv6)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin6_port );
        }
        else
        {
            sockaddr_in sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv4)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin_port );
        }
    }

    // set non-blocking io and receive timeout

    if ( socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        // non-blocking, keeping the descriptor's other status flags
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
        // blocking with receive timeout
        struct timeval tv = rocketnet_platform_timeval_from_seconds( timeout_seconds );
        if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) ) < 0 )
        {
            rocketnet_error( "failed to set socket receive timeout" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        // blocking with no timeout
    }

    // set don't fragment bit

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_DONTFRAG, &val, sizeof(val) );
    }
    else
    {
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val) );
    }

#if TARGET_OS_OSX

    // the batch pump: resolved symbols, a non-blocking socket (the pump's
    // spin discipline assumes it) make this
    // socket an msgx candidate. nothing is read at create: recvmsg_x rejects
    // MSG_PEEK, so a peek cannot decide the pump, and the first real batch
    // call is the functional probe instead. a fixed-port bind that already
    // has a datagram queued keeps it -- the receive that decides the pump
    // also delivers what it read. a refusal there pauses the pump and the
    // same call finishes portable, so nothing is lost either way.
    // degradation is silent above the seam.

#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
    if ( rocketnet_platform_create_receive_hook )
        rocketnet_platform_create_receive_hook( socket->handle );
#endif

    if ( pump_mode != ROCKETNET_PLATFORM_PUMP_MODE_FORCE_PORTABLE && rocketnet_sendmsg_x && rocketnet_recvmsg_x && socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        socket->msgx = true;
    }

#endif // #if TARGET_OS_OSX

#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
    rocketnet_platform_test_register( socket );
#endif

    return socket;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
    rocketnet_platform_test_unregister( socket );
#endif

    if ( rocketnet_platform_socket_handle_is_valid( socket->handle ) )
    {
        close( socket->handle );
        socket->handle = -1;
    }
    
    rocketnet_free( socket->context, socket );
}

// A connected socket (rocketnet_platform_socket_connect) has one remote endpoint: every
// send goes to it with no destination named, since a BSD stack refuses a
// sendto that names one on a connected socket (EISCONN). An unconnected
// socket names each datagram's destination.

static inline ssize_t rocketnet_platform_sendto( rocketnet_platform_socket_t * socket, const void * packet_data, int packet_bytes, const sockaddr * to, socklen_t to_bytes )
{
    if ( socket->connected )
        return send( socket->handle, packet_data, (size_t) packet_bytes, 0 );
    return sendto( socket->handle, packet_data, (size_t) packet_bytes, 0, to, to_bytes );
}

// Connects the socket to its one remote endpoint, so the kernel drops every datagram
// from any other source (the client trusts only the server: STANDARD t11).
// The destination is converted as send_packet converts it.

bool rocketnet_platform_socket_connect( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );

    rocketnet_address_t to = *input_to;
    sockaddr_storage storage;
    memset( &storage, 0, sizeof(storage) );
    socklen_t storage_bytes = 0;

    if ( socket->ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        if ( to.type != ROCKETNET_ADDRESS_IPV6 )
            return false;
        sockaddr_in6 * socket_address = (sockaddr_in6*) &storage;
        socket_address->sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
            ( (uint16_t*) &socket_address->sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        socket_address->sin6_port = rocketnet_platform_htons( to.port );
        storage_bytes = sizeof(sockaddr_in6);
    }
    else
    {
        if ( to.type != ROCKETNET_ADDRESS_IPV4 )
            return false;
        rocketnet_platform_address_to_sockaddr_in( &to, (sockaddr_in*) &storage );
        storage_bytes = sizeof(sockaddr_in);
    }

    if ( connect( socket->handle, (const sockaddr*) &storage, storage_bytes ) != 0 )
    {
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_address_to_string( input_to, address_string );
        rocketnet_error( "connect (%s) failed: %s", address_string, strerror( errno ) );
        return false;
    }

    socket->connected = true;
    return true;
}

// The per-packet send. A refusal counts in *failures: send_failures for an
// ordinary send, send_replay_failures for the replay of a refused sendmsg_x
// chunk (rocketnet_platform_socket_send_packets).

static void rocketnet_platform_darwin_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, const void * packet_data, int packet_bytes, volatile uint64_t * failures )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );
    rocketnet_assert( input_to->type == ROCKETNET_ADDRESS_IPV6 || input_to->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( failures == &socket->send_failures || failures == &socket->send_replay_failures );

#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
    int injected_error = 0;
    if ( rocketnet_platform_send_failure_should_inject( socket, &injected_error ) )
    {
        errno = injected_error;
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_address_to_string( input_to, address_string );
        rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
        rocketnet_platform_atomic_add_u64( failures, 1 );
        return;
    }
#endif

    rocketnet_address_t to = *input_to;

    if ( socket->ipv6 )
    {
        // socket is dual stack ipv4 and ipv6

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( to.port );
        int result = int( rocketnet_platform_sendto( socket, packet_data, packet_bytes, (sockaddr*)( &socket_address ), sizeof(sockaddr_in6) ) );
        if ( result < 0 )
        {
            char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            rocketnet_address_to_string( &to, address_string );
            rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
            rocketnet_platform_atomic_add_u64( failures, 1 );
        }
    }
    else
    {
        // socket is ipv4

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            sockaddr_in socket_address;
            rocketnet_platform_address_to_sockaddr_in( &to, &socket_address );
            int result = int( rocketnet_platform_sendto( socket, packet_data, packet_bytes, (sockaddr*)( &socket_address ), sizeof(sockaddr_in) ) );
            if ( result < 0 )
            {
                char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
                rocketnet_address_to_string( &to, address_string );
                rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
                rocketnet_platform_atomic_add_u64( failures, 1 );
            }
        }
        else
        {
            rocketnet_error( "invalid address. could not send packet" );
        }
    }
}

void rocketnet_platform_socket_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, const void * packet_data, int packet_bytes )
{
    rocketnet_assert( socket );
    rocketnet_platform_darwin_send_packet( socket, input_to, packet_data, packet_bytes, &socket->send_failures );
}

// UDP's ordinary weather on the receive path (TRANSPORTS.md, ICMP errors). a
// BSD stack reports an ICMP error (port or host unreachable, a remote endpoint that
// died hard) to the next receive on a CONNECTED socket, and a full mbuf
// pool reports ENOBUFS; neither says anything about the datagrams still
// queued behind it. every receive arm skips these and keeps draining:
// never fatal, never an msgx pause, the end of a drain only at the bound
// below. a client
// socket is connected to its server (rocketnet_platform_socket_connect), so
// the kernel does route ICMP errors to it, and this is the contract that
// keeps each one harmless; server sockets stay unconnected and see none.
// EAGAIN/EWOULDBLOCK stays the drain's
// end: it means the queue is empty. a weather errno can repeat (a lasting
// mbuf shortage keeps returning ENOBUFS), so the skips are bounded per call:
// the bound, not the kernel, is what guarantees every drain returns. a call
// that hits the bound returns what it has; the next drain reads on.

static bool rocketnet_platform_receive_weather( int error )
{
    return error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH ||
           error == EHOSTDOWN || error == ENETDOWN || error == ENOBUFS || error == EINTR;
}

// weather reports one per-packet receive skips before it returns 0 (drained)
#define ROCKETNET_PLATFORM_RECEIVE_WEATHER_SKIPS 64

#if ROCKETNET_DEV

// the test seam (development builds only, never the production library):
// the next count recvmsg or recvmsg_x calls on any socket return -1 with
// this errno instead of calling the kernel, leaving every queued datagram
// in place. the one-shot form is a count of 1; an error of 0 clears it.

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

static ssize_t rocketnet_platform_recvmsg( int handle, struct msghdr * msg )
{
#if ROCKETNET_DEV
    const int injected = rocketnet_platform_take_receive_errno_for_testing();
    if ( injected != 0 )
    {
        errno = injected;
        return -1;
    }
#endif // #if ROCKETNET_DEV
    return recvmsg( handle, msg, 0 );
}

// the per-packet receive behind the public call and the batch pump's
// per-packet path: the caller owns the overflow buffer, so a batch receive
// keeps one 64 KiB discard area on its stack instead of two.

static int rocketnet_platform_socket_receive_packet_into( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size, uint8_t * overflow, size_t overflow_bytes, int * weather_remaining )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( max_packet_size > 0 );
    rocketnet_assert( overflow );

    sockaddr_storage sockaddr_from;
    // Darwin reports bytes copied, not the original size on truncation.
    // Scatter the prefix into caller storage and discard the rest in the
    // caller's overflow buffer. This covers a complete ordinary UDP datagram.
    struct iovec iov[2];
    iov[0].iov_base = packet_data;
    iov[0].iov_len = (size_t) max_packet_size;
    iov[1].iov_base = overflow;
    iov[1].iov_len = overflow_bytes;

    const bool dual_stack = socket->ipv6;

    // The caller shares this budget across every packet and pump fallback.
    rocketnet_assert( weather_remaining && *weather_remaining > 0 );

    int result;
receive_again:
    for ( ;; )
    {
        struct msghdr msg;
        memset( &msg, 0, sizeof(msg) );
        msg.msg_name = &sockaddr_from;
        msg.msg_namelen = sizeof(sockaddr_from);
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;

#if defined(ROCKETNET_RECEIVE_TEST_HOOKS)
        if ( rocketnet_platform_receive_hooked( socket, packet_data, max_packet_size, &sockaddr_from, sizeof( sockaddr_from ), &result ) )
        {
            if ( result < 0 )
                return 0;
            if ( result == 0 )
                continue;
            break;
        }
#endif // #if defined(ROCKETNET_RECEIVE_TEST_HOOKS)

        result = int( rocketnet_platform_recvmsg( socket->handle, &msg ) );

        if ( result < 0 )
        {
            const int error = errno;

            // ICMP-class and buffer weather: skip it and read on, the
            // datagrams behind it are still queued (TRANSPORTS.md, ICMP errors);
            // past the bound the call reports drained and the next one reads on

            if ( rocketnet_platform_receive_weather( error ) )
            {
                if ( --*weather_remaining > 0 )
                    continue;
                return 0;
            }

            // EINTR is weather and never reaches here
            if ( error != EAGAIN && error != EWOULDBLOCK )
            {
                rocketnet_debug( "recvfrom failed with error %d", error );
            }
            return 0;
        }

        if ( result == 0 )
        {
            // a zero-length datagram is legal UDP carrying nothing the
            // protocol admits -- and returning 0 here would read as
            // "socket drained", letting byte-less chaff cap an entire
            // drain at one packet (#100's review). skip it, keep reading:
            // both pumps share this semantic.
            continue;
        }

        // The overflow iovec preserves the datagram size even when only
        // its prefix fits the caller's slot. The transport rejects oversize
        // packets before inspecting their truncated payload.
        break;
    }

    if ( sockaddr_from.ss_family == AF_INET6 )
    {
        sockaddr_in6 * addr_ipv6 = (sockaddr_in6*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );

        if ( !rocketnet_address_accept_received_source( from, dual_stack ) )
            goto receive_again;
    }
    else if ( sockaddr_from.ss_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in*) &sockaddr_from, from );
    }
    else
    {
        rocketnet_assert( 0 );
        return 0;
    }
  
    rocketnet_assert( result >= 0 );

    return result;
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
    // the public call owns its discard area; the batch pump passes its own
    uint8_t overflow[65535];
    int weather_remaining = ROCKETNET_PLATFORM_RECEIVE_WEATHER_SKIPS;
    return rocketnet_platform_socket_receive_packet_into( socket, from, packet_data, max_packet_size, overflow, sizeof(overflow), &weather_remaining );
}

// ---------------------------------------------------

// the darwin batch pump (#100). the portable per-packet functions above
// stay the degradation target: every path through these either moves the
// batch through sendmsg_x/recvmsg_x or hands the remainder to the
// per-packet loop, so a packet is never lost to a refusal -- it is sent
// or dropped where the portable pump would have sent or dropped it, with
// one priced exception recorded at the send loop's error path below. on
// iOS the msgx flag is never set and these ARE the portable loop.
//
// a hard refusal (an errno outside UDP's ordinary weather) PAUSES the
// msgx pump on that socket rather than killing it: batch calls run
// portable until the pause expires, and the first batch attempt after
// expiry is itself the re-probe. a transient misclassification therefore
// costs at most one pause window, never the feature (#100's review: a
// permanent degrade hands any single weird errno a kill switch).

#define ROCKETNET_PLATFORM_HAS_BATCH_PUMP 1

#define ROCKETNET_MSGX_RETRY_SECONDS 60.0

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

// the name reports the LIVE path -- tests and logs treat it as ground
// truth, so a paused pump reads as portable until its pause expires. a
// pure read: expiry itself is cleared by the next batch call's
// rocketnet_msgx_active, never here.

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

// is the msgx pump live for this call? eligibility is decided at create;
// a pause expires here, making the caller's next batch attempt the
// re-probe. the client's batch send and receive run on its I/O thread
// (portable_io_client_send_batch in sdk/rocketnet_driver_builtin.cpp) while
// rocketnet_platform_socket_pump above reads the pause on its caller's
// thread, so msgx_down_until is only loaded and stored atomically (relaxed:
// it orders nothing else).

#if defined(ROCKETNET_MSGX_TEST_HOOKS)
static thread_local void (*rocketnet_msgx_before_expiry_clear)( rocketnet_platform_socket_t * ) = NULL;
#endif

// tla/RNMsgxPause.tla: Probe, ObserveExpired, ClearIfUnchanged and PollPaused
// model the shared expiry; FinishProbe abstracts a successful batch completion.
static bool rocketnet_msgx_active( rocketnet_platform_socket_t * socket )
{
    if ( !socket->msgx )
        return false;
    double down_until = rocketnet_msgx_down_until( socket );
    while ( down_until > 0.0 )
    {
        if ( rocketnet_platform_time() < down_until )
            return false;
#if defined(ROCKETNET_MSGX_TEST_HOOKS)
        if ( rocketnet_msgx_before_expiry_clear ) rocketnet_msgx_before_expiry_clear( socket );
#endif
        // TX and RX may re-probe concurrently. Clear only the expired value
        // observed here: a fresh pause published by the other worker wins.
        double cleared = 0.0;
        if ( __atomic_compare_exchange( &socket->msgx_down_until, &down_until, &cleared,
                                        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED ) )
        {
            rocketnet_debug( "msgx pump pause expired. next batch call is the re-probe" );
            break;
        }
    }
    return true;
}

#if defined(ROCKETNET_MSGX_TEST_HOOKS)
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
#endif

// tla/RNMsgxPause.tla: Pause publishes a retry deadline shared by TX and RX.
static void rocketnet_msgx_pause( rocketnet_platform_socket_t * socket, const char * which, int error )
{
    rocketnet_msgx_set_down_until( socket, rocketnet_platform_time() + ROCKETNET_MSGX_RETRY_SECONDS );
    rocketnet_warn( "%s refused (%s). msgx pump pauses %g seconds", which, strerror( error ), ROCKETNET_MSGX_RETRY_SECONDS );
}

// UDP's ordinary weather on the send path: errnos the per-packet path
// also produces on a routine bad destination or a full buffer, and which
// therefore must not pause the pump -- the review measured EADDRNOTAVAIL
// from a single forged source port, which under a permanent degrade was
// a one-packet kill switch for the feature.

static bool rocketnet_msgx_send_weather( int error )
{
    return error == EINTR || error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS || error == ECONNREFUSED ||
           error == EHOSTUNREACH || error == ENETUNREACH || error == EMSGSIZE || error == EADDRNOTAVAIL ||
           error == EPERM || error == EACCES || error == EHOSTDOWN || error == ENETDOWN;
}

// fill a sockaddr for one destination, mirroring send_packet's dual-stack
// conversion. returns the sockaddr length, or 0 for an address the socket
// cannot carry (logged exactly as the per-packet path logs it).

static socklen_t rocketnet_msgx_fill_sockaddr( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, sockaddr_storage * storage )
{
    rocketnet_address_t to = *input_to;

    if ( socket->ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 * socket_address = (sockaddr_in6*) storage;
        memset( socket_address, 0, sizeof(sockaddr_in6) );
        socket_address->sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address->sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address->sin6_port = rocketnet_platform_htons( to.port );
        return sizeof(sockaddr_in6);
    }

    if ( to.type != ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_error( "invalid address. could not send packet" );
        return 0;
    }

    rocketnet_platform_address_to_sockaddr_in( &to, (sockaddr_in*) storage );
    return sizeof(sockaddr_in);
}

// convert one received sockaddr, mirroring receive_packet's mapped-address
// normalization. returns false for a family the transport does not speak.

bool rocketnet_msgx_read_sockaddr( rocketnet_platform_socket_t * socket, const sockaddr_storage * storage, rocketnet_address_t * from )
{
    if ( storage->ss_family == AF_INET6 )
    {
        const sockaddr_in6 * addr_ipv6 = (const sockaddr_in6*) storage;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (const uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
        const bool dual_stack = socket->ipv6;
        return rocketnet_address_accept_received_source( from, dual_stack );
    }

    if ( storage->ss_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in*) storage, from );
        return true;
    }

    return false;
}

void rocketnet_platform_socket_send_packets( rocketnet_platform_socket_t * socket, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( sends || num_packets == 0 );

    if ( !rocketnet_msgx_active( socket ) )
    {
        for ( int i = 0; i < num_packets; i++ )
        {
            rocketnet_platform_socket_send_packet( socket, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
        }
        return;
    }

    int base = 0;
    while ( base < num_packets )
    {
        // build one chunk. an address the socket cannot carry is skipped
        // here, already logged by the fill -- the same drop the per-packet
        // path takes. map[] remembers each chunk entry's index in sends,
        // so a mid-chunk degradation can replay the exact unsent tail.

        sockaddr_storage addresses[ROCKETNET_MSGX_BATCH];
        struct iovec iov[ROCKETNET_MSGX_BATCH];
        struct rocketnet_msghdr_x_t msg[ROCKETNET_MSGX_BATCH];
        int map[ROCKETNET_MSGX_BATCH];
        memset( msg, 0, sizeof(msg) );

        int count = 0;
        int scanned = 0;
        while ( base + scanned < num_packets && count < ROCKETNET_MSGX_BATCH )
        {
            const rocketnet_platform_send_t * send = &sends[base + scanned];
            rocketnet_assert( send->to );
            rocketnet_assert( send->packet_data );
            rocketnet_assert( send->packet_bytes > 0 );

            socklen_t sockaddr_bytes = rocketnet_msgx_fill_sockaddr( socket, send->to, &addresses[count] );
            if ( sockaddr_bytes != 0 )
            {
                iov[count].iov_base = const_cast<uint8_t*>( send->packet_data ); // the send only reads it
                iov[count].iov_len = (size_t) send->packet_bytes;
                // a connected socket names no destination (rocketnet_platform_sendto)
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
            ssize_t result;
#if defined(ROCKETNET_PLATFORM_SEND_SEAM)
            int injected_error = 0;
            if ( rocketnet_platform_send_failure_should_inject( socket, &injected_error ) )
            {
                errno = injected_error;
                result = -1;
            }
            else
            {
                result = rocketnet_sendmsg_x( socket->handle, msg + offset, (u_int) ( count - offset ), 0 );
            }
#else
            result = rocketnet_sendmsg_x( socket->handle, msg + offset, (u_int) ( count - offset ), 0 );
#endif
            if ( result > 0 )
            {
                offset += (int) result;
                continue;
            }

            // A refused batch, including EINTR, has an INDETERMINATE
            // delivered prefix. Retrying EINTR here could keep a public
            // server stop request in this loop indefinitely. xnu can deliver
            // part of a batch and still
            // return -1 with no count -- measured on the test machine: 8 messages
            // with a refused destination at index 5 delivered 0-4 and
            // returned -1 -- so no retry that assumes "nothing was sent"
            // (nor "only the head failed") can be right. the law here: one
            // bounded per-packet pass finishes the chunk. every message in
            // the tail gets exactly one attempt through the per-packet
            // path, which sends or drops it exactly as the portable pump
            // would; a message the kernel had already delivered may go
            // twice. the duplication is priced: at most one duplicate per
            // message per error event (amplification <= 2x, O(count) cost,
            // never O(count^2)), and the protocol's replay protection eats
            // duplicates at the receiver. a weather errno (a routine bad
            // destination, a full buffer) stays on the msgx pump for the
            // next chunk; anything else pauses the pump for
            // ROCKETNET_MSGX_RETRY_SECONDS and this call finishes portable.
            // the counts: each refusal of the replay counts in
            // send_replay_failures, never in send_failures, since a replayed
            // message may already have reached the wire (a full buffer,
            // EAGAIN or ENOBUFS, that refused the chunk usually refuses its
            // replay too, delivered prefix included). the refused chunk
            // itself counts ONCE in send_failures, whatever its replay does:
            // its fate is indeterminate. a replay that sent every message
            // adds nothing to send_replay_failures.

            const int error = errno;

            if ( result == 0 && error == 0 )
            {
                // zero messages accepted and no errno: nothing to
                // diagnose, nothing to pause over -- weather. the bounded
                // pass below finishes the chunk (inventing a pause from
                // strerror(0) was #100's review, round two).
                rocketnet_debug( "sendmsg_x accepted nothing. chunk finishes per-packet" );
            }
            else if ( rocketnet_msgx_send_weather( error ) )
            {
                rocketnet_debug( "sendmsg_x error (%s). chunk finishes per-packet", strerror( error ) );
            }
            else
            {
                rocketnet_msgx_pause( socket, "sendmsg_x", error );
            }

            rocketnet_platform_note_send_failure( socket );
            for ( int i = offset; i < count; i++ )
            {
                const rocketnet_platform_send_t * send = &sends[map[i]];
                rocketnet_platform_darwin_send_packet( socket, send->to, send->packet_data, send->packet_bytes, &socket->send_replay_failures );
            }
            offset = count;
        }

        base += scanned;

        // a pause landed mid-call: the rest of this batch goes per-packet

        if ( !rocketnet_msgx_active( socket ) )
        {
            for ( int i = base; i < num_packets; i++ )
            {
                rocketnet_platform_socket_send_packet( socket, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
            }
            return;
        }
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

    // Each message keeps its prefix in its own slot. The discarded tails
    // may overlap: none are read after this synchronous syscall returns.
    uint8_t overflow[65535];
    int total = 0;

    // One weather budget covers both pumps and every delivered packet.
    int weather_remaining = max_packets;

    if ( !rocketnet_msgx_active( socket ) )
        goto per_packet;

    while ( total < max_packets )
    {
        int want = max_packets - total;
        if ( want > ROCKETNET_MSGX_BATCH )
            want = ROCKETNET_MSGX_BATCH;

        const int chunk_base = total;

        sockaddr_storage addresses[ROCKETNET_MSGX_BATCH];
        struct iovec iov[ROCKETNET_MSGX_BATCH][2];
        struct rocketnet_msghdr_x_t msg[ROCKETNET_MSGX_BATCH];
        memset( msg, 0, sizeof(msg) );
        for ( int i = 0; i < want; i++ )
        {
            iov[i][0].iov_base = packet_data + (size_t) ( chunk_base + i ) * (size_t) packet_stride;
            iov[i][0].iov_len = (size_t) packet_stride;
            iov[i][1].iov_base = overflow;
            iov[i][1].iov_len = sizeof(overflow);
            msg[i].msg_name = &addresses[i];
            msg[i].msg_namelen = sizeof(addresses[i]);
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

            // ICMP-class and buffer weather (and EINTR): skip it and keep
            // draining on the batch pump until EAGAIN -- never a pause, and
            // the end of a drain upon max_packets weather reports, so a repeating
            // errno cannot spin this call (TRANSPORTS.md, ICMP errors).

            if ( rocketnet_platform_receive_weather( error ) )
            {
                if ( --weather_remaining > 0 )
                    continue;
                return total;
            }

            // outside ordinary weather: pause the pump and finish this
            // call per-packet -- what earlier chunks drained is already
            // delivered, nothing about this call's contract changes. the
            // pause logs once, and a paused pump never reaches this line.

            rocketnet_msgx_pause( socket, "recvmsg_x", error );
            goto per_packet;
        }

        if ( result == 0 )
            return total;

        // walk the chunk in place. a zero-length datagram or a family the
        // transport does not speak (the per-packet path delivers neither)
        // is compacted out: the next kept message's bytes move down to the
        // gap. rare by nature -- attack chaff -- and bounded by the chunk.

        for ( ssize_t i = 0; i < result; i++ )
        {
            int bytes = (int) msg[i].msg_datalen;
            rocketnet_address_t parsed;
            if ( bytes <= 0 || !rocketnet_msgx_read_sockaddr( socket, &addresses[i], &parsed ) )
                continue;
            uint8_t * written = packet_data + (size_t) ( chunk_base + (int) i ) * (size_t) packet_stride;
            uint8_t * target = packet_data + (size_t) total * (size_t) packet_stride;
            // msg_datalen is the datagram's true size; the kernel wrote only
            // packet_stride bytes into the slot. copy what was written and
            // hand the caller the true size, so an oversize datagram reaches
            // the transport's prefilter and is counted PREFILTER_TOO_LARGE.
            const int copy_bytes = bytes < packet_stride ? bytes : packet_stride;
            if ( target != written )
            {
                memmove( target, written, (size_t) copy_bytes );
            }
            from[total] = parsed;
            packet_bytes[total] = bytes;
            total++;
        }

        if ( result < want )
            return total;
    }

    return total;

per_packet:

    while ( total < max_packets )
    {
        // the same overflow buffer as the batch arm: one discard area per call
        int bytes = rocketnet_platform_socket_receive_packet_into( socket, &from[total], packet_data + (size_t) total * (size_t) packet_stride, packet_stride, overflow, sizeof(overflow), &weather_remaining );
        if ( bytes == 0 )
            break;
        packet_bytes[total] = bytes;
        total++;
    }
    return total;
}

// ---------------------------------------------------

struct thread_shim_data_t
{
    void * context;
    void * real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function;
    rocketnet_platform_thread_t * thread;
};

// the platform thread the calling thread runs as, for thread_wait; NULL on a
// thread that thread_create did not start
static thread_local rocketnet_platform_thread_t * rocketnet_platform_thread_self = NULL;

static void * thread_function_shim( void * data )
{
    rocketnet_assert( data );
    thread_shim_data_t * shim_data = (thread_shim_data_t*) data;
    void * context = shim_data->context;
    void * real_thread_data = shim_data->real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function = shim_data->real_thread_function;
    rocketnet_platform_thread_self = shim_data->thread;
    rocketnet_free( context, data );

    real_thread_function( real_thread_data );
    return NULL;
}

static bool rocketnet_platform_thread_wake_init( rocketnet_platform_thread_t * thread )
{
    semaphore_t semaphore = MACH_PORT_NULL;
    if ( semaphore_create( mach_task_self(), &semaphore, SYNC_POLICY_FIFO, 0 ) != KERN_SUCCESS )
        return false;
    thread->wake_semaphore = semaphore;
    return true;
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg )
{
    rocketnet_platform_thread_t * thread = (rocketnet_platform_thread_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_thread_t) );

    if ( !thread )
        return NULL;

    thread->context = context;

    thread_shim_data_t * shim_data = (thread_shim_data_t*) rocketnet_malloc( context, sizeof(thread_shim_data_t) );
    if ( !shim_data )
    {
        rocketnet_free( context, thread );
        return NULL;
    }
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;
    shim_data->thread = thread;

    thread->woken = 0;
    if ( !rocketnet_platform_thread_wake_init( thread ) )
    {
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
        return NULL;
    }

    if ( pthread_create( &thread->handle, NULL, thread_function_shim, shim_data ) != 0 )
    {
        semaphore_destroy( mach_task_self(), (semaphore_t) thread->wake_semaphore );
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
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
    semaphore_destroy( mach_task_self(), (semaphore_t) thread->wake_semaphore );
    rocketnet_free( thread->context, thread );
}

void rocketnet_platform_thread_wait( double time )
{
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_self;
    if ( !thread )
    {
        rocketnet_platform_sleep( time );
        return;
    }
    // woken is stored before the semaphore is signalled: a wake that lands
    // before this check ends the wait here, one that lands after it ends the
    // semaphore wait
    if ( __atomic_load_n( &thread->woken, __ATOMIC_ACQUIRE ) )
        return;
    // whole seconds and nanoseconds converted apart, clamped to a day
    if ( !( time > 0.0 ) )
        time = 0.0;
    if ( time > 86400.0 )
        time = 86400.0;
    const unsigned int whole_seconds = (unsigned int) time;
    mach_timespec_t relative;
    relative.tv_sec = whole_seconds;
    relative.tv_nsec = (clock_res_t) ( ( time - (double) whole_seconds ) * 1000000000.0 );
    semaphore_timedwait( (semaphore_t) thread->wake_semaphore, relative );
}

void rocketnet_platform_thread_wake( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    __atomic_store_n( &thread->woken, 1, __ATOMIC_RELEASE );
    semaphore_signal( (semaphore_t) thread->wake_semaphore );
}

void rocketnet_platform_client_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you are developing for mac or ios you can adjust the affinity and priority for the client thread here.
    // SCHED_RR at sched_get_priority_min( SCHED_RR ) and nothing else, as flow sets it (flow_platform.cpp:1630-1636)
    rocketnet_platform_network_thread_priority( thread );
}

void rocketnet_platform_server_thread_priority( rocketnet_platform_thread_t * thread )
{
    // No macOS server transport calls this hook; it does not set their worker priority.
    // SCHED_RR at sched_get_priority_min( SCHED_RR ) and nothing else, as flow sets it (flow_platform.cpp:1638-1644)
    rocketnet_platform_network_thread_priority( thread );
}

// ---------------------------------------------------

bool rocketnet_platform_mutex_create( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    return rocketnet_platform_priority_calls()->mutex_init( mutex ) == 0;
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
        memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );
    }
}


// ---------------------------------------------------

// The client I/O wake on macOS and iOS: one kqueue holding the socket's
// EVFILT_READ and an EVFILT_USER trigger (docs/CLIENT-IO.md "The wake").
// The trigger is EV_CLEAR, so it is
// level-triggered until a wait returns it: a set made while no wait runs
// makes the next wait return at once, and that wait consumes it.

#define ROCKETNET_PLATFORM_WAKE_KEVENT_IDENT 1

struct rocketnet_platform_wake_t
{
    void * context;
    int kq;
    int socket_fd;
    bool read_enabled;
};

// docs/CLIENT-IO.md "The wake"; tla/RNClientIo.tla Init: no wake set, the
// read filter enabled.

rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket )
{
    rocketnet_platform_wake_t * wake = (rocketnet_platform_wake_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_wake_t ) );
    if ( !wake )
        return NULL;
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

// docs/CLIENT-IO.md "The wake"; tla/RNClientIo.tla IoExit and Free: the
// kqueue, and with it both filters, goes with the I/O thread.

void rocketnet_platform_wake_destroy( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    if ( wake->kq >= 0 )
        close( wake->kq );
    rocketnet_free( wake->context, wake );
}

// tla/RNClientIo.tla IoWait and IoWake: the wait returns on the trigger, or on
// a readable socket only while readable is true (waitReadable). When readable
// changes, the same kevent call carries EV_ENABLE or EV_DISABLE for the read
// filter, so a full buffer waits on the trigger alone (docs/CLIENT-IO.md "The
// wake"). A returned EV_CLEAR trigger is consumed (IoWake's wake' = FALSE).

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
            // the read filter change was refused: keep the old state so the
            // next wait carries the change again
            if ( events[i].filter == EVFILT_READ && events[i].data != 0 )
                wake->read_enabled = !readable;
            continue;
        }
        woken = true;
    }
    return woken;
}

// docs/CLIENT-IO.md "The wake"; tla/RNClientIo.tla HandOff, Return and Stop
// (the actions that set wake'): any thread triggers the EVFILT_USER event; it
// stays set until a wait returns it.

void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    struct kevent change;
    EV_SET( &change, ROCKETNET_PLATFORM_WAKE_KEVENT_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL );
    kevent( wake->kq, &change, 1, NULL, 0, NULL );
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

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
#include <wininet.h>
#include <iphlpapi.h>
#include <qos2.h>
#include <mswsock.h>

#pragma comment( lib, "WS2_32.lib" )
#pragma comment( lib, "IPHLPAPI.lib" )
#pragma comment( lib, "Qwave.lib" )

#ifdef SetPort
#undef SetPort
#endif // #ifdef SetPort

extern void * rocketnet_malloc( void * context, size_t bytes );

extern void rocketnet_free( void * context, void * p );

static int get_connection_type();

static LARGE_INTEGER timer_frequency;
static LARGE_INTEGER timer_start;
static int connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

// Windows: SetThreadPriority( thread, THREAD_PRIORITY_TIME_CRITICAL ) on the
// network threads (flow_platform.cpp:1839-1851), and the process timer at 1 ms
// from platform init to platform term, matching the host application's lifecycle

static int rocketnet_platform_real_set_sched( rocketnet_platform_thread_t * thread, int policy, int priority )
{
    (void) thread;
    (void) policy;
    (void) priority;
    return 0;
}

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
    QueryPerformanceFrequency( (LARGE_INTEGER*) frequency );
}

static int rocketnet_platform_real_mutex_init( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );
    if ( !InitializeCriticalSectionAndSpinCount( (LPCRITICAL_SECTION)&mutex->handle, 0xFF ) )
        return 1;
    mutex->ok = true;
    return 0;
}

static int rocketnet_platform_real_platform_start( void )
{
    WSADATA WsaData;
    return WSAStartup( MAKEWORD(2,2), &WsaData ) == NO_ERROR ? 0 : 1;
}

static void* rocketnet_platform_real_create_waitable_timer( unsigned flags )
{
    return CreateWaitableTimerExW( NULL, NULL, flags, TIMER_ALL_ACCESS );
}

static const rocketnet_platform_priority_calls_t rocketnet_platform_priority_calls_real_table =
{
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

static void rocketnet_platform_network_thread_priority( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    if ( !rocketnet_platform_thread_priority_enabled() )
        return;
    if ( !rocketnet_platform_priority_calls()->set_thread_priority( thread, THREAD_PRIORITY_TIME_CRITICAL ) )
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
    const unsigned int result = rocketnet_platform_priority_calls()->time_end_period( 1 );
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
        const unsigned int result = rocketnet_platform_priority_calls()->time_begin_period( 1 );
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
    rocketnet_platform_priority_calls()->timer_frequency( &timer_frequency );
    QueryPerformanceCounter( &timer_start );

    if ( rocketnet_platform_priority_calls()->platform_start() != 0 )
    {
        rocketnet_error( "WSAStartup failed" );
        rocketnet_platform_timer_period_end();
        return false;
    }

    connection_type = get_connection_type();

    return true;
}

void rocketnet_platform_term()
{
    WSACleanup();
    rocketnet_platform_timer_period_end();
}

int rocketnet_platform_connection_type()
{
    return connection_type;
}

int rocketnet_platform_getenv( const char * var, char * buffer, size_t size )
{
    rocketnet_assert( var && buffer && size > 0 );
#if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
    int test_result = 0;
    if ( rocketnet_platform_test_environment_getenv( var, buffer, size, &test_result ) )
        return test_result;
#endif // #if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
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

static DWORD WINAPI thread_function_shim( void * data )
{
    rocketnet_assert( data );
    thread_shim_data_t * shim_data = (thread_shim_data_t*) data;
    void * context = shim_data->context;
    void * real_thread_data = shim_data->real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function = shim_data->real_thread_function;
    rocketnet_free( context, data );
    real_thread_function( real_thread_data );
    return 0;
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg )
{
    rocketnet_platform_thread_t * thread = (rocketnet_platform_thread_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_thread_t ) );

    if ( !thread )
        return NULL;

    thread->context = context;

    thread_shim_data_t * shim_data = (thread_shim_data_t*) rocketnet_malloc( context, sizeof(thread_shim_data_t) );
    if ( !shim_data )
    {
        rocketnet_free( context, thread );
        return NULL;
    }
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;

    thread->handle = CreateThread(NULL, 0, thread_function_shim, shim_data, 0, NULL);

    if ( thread->handle == NULL )
    {
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
        return NULL;
    }

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
    CloseHandle( thread->handle );
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
    return rocketnet_platform_priority_calls()->mutex_init( mutex ) == 0;
}

void rocketnet_platform_mutex_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    EnterCriticalSection( (LPCRITICAL_SECTION)&mutex->handle );
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
    LeaveCriticalSection( (LPCRITICAL_SECTION)&mutex->handle );
}

void rocketnet_platform_mutex_destroy( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    if ( mutex->ok )
    {
        DeleteCriticalSection( (LPCRITICAL_SECTION)&mutex->handle );
        memset(mutex, 0, sizeof(rocketnet_platform_mutex_t));
    }
}

// time

// Sleep() rounds up to the system timer tick (15.6 ms by default) and a
// millisecond cast truncates a sub-millisecond request to Sleep(0), so the
// sleep blocks on a per-thread high-resolution waitable timer instead (Windows
// 10 1803 and later), set for the whole request, which keeps the system-wide
// timer resolution untouched. rocketnet_platform_sleep_plan converts the
// request; rocketnet_platform_sleep_tail_yields decides the at most 50 us
// SwitchToThread tail when the timer wakes before the request is up. Where
// the timer cannot be created, SetWaitableTimer fails, or the wait does not
// return WAIT_OBJECT_0, the fallback raises the timer to 1 ms only for the
// duration of a rounded-up Sleep.

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif // #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION

#include <timeapi.h>

#pragma comment( lib, "winmm.lib" )

struct rocketnet_platform_sleep_timer_t
{
    HANDLE handle;
    bool tried;
    ~rocketnet_platform_sleep_timer_t()
    {
        if ( handle )
            CloseHandle( handle );
    }
};

static thread_local rocketnet_platform_sleep_timer_t rocketnet_platform_sleep_timer = { NULL, false };

void rocketnet_platform_sleep( double time )
{
    const rocketnet_platform_sleep_plan_t plan = rocketnet_platform_sleep_plan( time );

    if ( plan.yield_only )
    {
        Sleep( 0 );
        return;
    }

    if ( !rocketnet_platform_sleep_timer.tried )
    {
        rocketnet_platform_sleep_timer.tried = true;
        rocketnet_platform_sleep_timer.handle = CreateWaitableTimerExW( NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS );
    }

    if ( rocketnet_platform_sleep_timer.handle )
    {
        LARGE_INTEGER frequency;
        LARGE_INTEGER start;
        QueryPerformanceFrequency( &frequency );
        QueryPerformanceCounter( &start );

        LARGE_INTEGER due;
        due.QuadPart = plan.due_time;
        if ( SetWaitableTimer( rocketnet_platform_sleep_timer.handle, &due, 0, NULL, NULL, FALSE ) &&
             WaitForSingleObject( rocketnet_platform_sleep_timer.handle, INFINITE ) == WAIT_OBJECT_0 )
        {
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
            return;
        }
    }

    timeBeginPeriod( 1 );
    Sleep( (DWORD) plan.fallback_milliseconds );
    timeEndPeriod( 1 );
}

void rocketnet_platform_thread_wait( double time )
{
    rocketnet_platform_sleep( time );
}

double rocketnet_platform_time()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    return ( (double) ( now.QuadPart - timer_start.QuadPart ) ) / ( (double) ( timer_frequency.QuadPart ) );
}

// sockets

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    // network order IS host order on a big-endian machine, so the swap is
    // the little-endian case and not the definition. Swapping
    // unconditionally reversed every ipv6 group and every port on the one
    // architecture where this function is supposed to do nothing (#127,
    // found by the #111 big-endian leg).
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
#endif // #if ROCKETNET_BIG_ENDIAN
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
#if ROCKETNET_BIG_ENDIAN
    return in;
#else // #if ROCKETNET_BIG_ENDIAN
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
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
    return inet_ntop( AF_INET6, (void*)address, address_string, address_string_size ) != NULL;
}

bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, rocketnet_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof(hints) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                sockaddr_in6 * addr_ipv6 = (sockaddr_in6 *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV6;
                for ( int i = 0; i < 8; ++i )
                {
                    address->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
                }
                address->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in *)( result->ai_addr ), address );
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

uint16_t rocketnet_platform_preferred_client_port()
{
    return 0;
}

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
    if ( getsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &granted ), &length ) != 0 )
        return 0;
    return granted;
}

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size )
{
    rocketnet_platform_socket_t * s = (rocketnet_platform_socket_t *) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );

    if ( !s )
        return NULL;

    s->context = context;
    s->send_failures = 0;
    s->send_replay_failures = 0;
    s->connected = false;

    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    // create socket

    s->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;

    if ( address->type == ROCKETNET_ADDRESS_IPV6 && !rocketnet_platform_ipv6_socket_available_for_testing )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, s );
        return NULL;
    }

    s->handle = socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( s->handle == INVALID_SOCKET )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, s );
        return NULL;
    }

    // IMPORTANT: tell windows we don't want to receive any connection or network
    // reset reports for this socket, otherwise a receive errors out after a remote endpoint
    // disconnects hard and its ICMP port unreachable comes back
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) s->handle );

    // own the port exclusively: without SO_EXCLUSIVEADDRUSE another local
    // process binds the same port with SO_REUSEADDR and takes its datagrams.
    // set before bind, as the IOCP and RIO transports do.

    BOOL exclusive = TRUE;
    if ( setsockopt( s->handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)( &exclusive ), sizeof( exclusive ) ) != 0 )
    {
        rocketnet_error( "failed to set socket exclusive address use" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    // set ipv6 sockets as dual stack

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( s->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }

    // increase socket send and receive buffer sizes

    if ( setsockopt( s->handle, SOL_SOCKET, SO_SNDBUF, (char*)( &send_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    if ( setsockopt( s->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &receive_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket receive buffer size" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    // bind to port

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( sockaddr_in6 ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( address->data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( address->port );

        if ( bind( s->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
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

        if ( bind( s->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
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
        addr = (sockaddr*) &sin6;
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
        addr = (sockaddr*) &sin4;
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
        if ( setsockopt( s->handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)( &tv ), sizeof( tv ) ) < 0 )
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
        setsockopt( s->handle, IPPROTO_IPV6, IPV6_DONTFRAG, (const char*) &val, sizeof(val) );
    }
    else
    {
        int val = 1;
        setsockopt( s->handle, IPPROTO_IP, IP_DONTFRAGMENT, (const char*) &val, sizeof(val) );
    }    

    return s;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

    if ( rocketnet_platform_socket_handle_is_valid( socket->handle ) )
    {
        closesocket( socket->handle );
        socket->handle = (rocketnet_platform_socket_handle_t) INVALID_SOCKET;
    }

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
        return send( socket->handle, (const char*) packet_data, packet_bytes, 0 );
    return sendto( socket->handle, (const char*) packet_data, packet_bytes, 0, to, to_bytes );
}

// Connects the socket to its one remote endpoint, so the stack drops every datagram
// from any other source (the client trusts only the server: STANDARD t11).
// The destination is converted as send_packet converts it.

bool rocketnet_platform_socket_connect( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );

    rocketnet_address_t to = *input_to;
    sockaddr_storage storage;
    memset( &storage, 0, sizeof(storage) );
    int storage_bytes = 0;

    if ( socket->ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        if ( to.type != ROCKETNET_ADDRESS_IPV6 )
            return false;
        sockaddr_in6 * socket_address = (sockaddr_in6*) &storage;
        socket_address->sin6_family = AF_INET6;
        for ( int i = 0; i < 8; i++ )
            ( (uint16_t*) &socket_address->sin6_addr )[i] = rocketnet_platform_htons( to.data.ipv6[i] );
        socket_address->sin6_port = rocketnet_platform_htons( to.port );
        storage_bytes = (int) sizeof(sockaddr_in6);
    }
    else
    {
        if ( to.type != ROCKETNET_ADDRESS_IPV4 )
            return false;
        rocketnet_platform_address_to_sockaddr_in( &to, (sockaddr_in*) &storage );
        storage_bytes = (int) sizeof(sockaddr_in);
    }

    if ( connect( socket->handle, (const sockaddr*) &storage, storage_bytes ) == SOCKET_ERROR )
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

    rocketnet_address_t to = *input_to;

    if ( socket->ipv6 )
    {
        // socket is dual stack ipv4 and ipv6

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof(socket_address) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; i++ )
        {
            ( (uint16_t*) &socket_address.sin6_addr)[i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( to.port );
        int result = rocketnet_platform_sendto( socket, packet_data, packet_bytes, (sockaddr*)(&socket_address), (int) sizeof(sockaddr_in6) );
        if ( result == SOCKET_ERROR )
            rocketnet_platform_windows_send_failed( socket, &to, WSAGetLastError() );
    }
    else
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            sockaddr_in socket_address;
            rocketnet_platform_address_to_sockaddr_in( &to, &socket_address );
            int result = rocketnet_platform_sendto( socket, packet_data, packet_bytes, (sockaddr*)(&socket_address), (int) sizeof(sockaddr_in) );
            if ( result == SOCKET_ERROR )
                rocketnet_platform_windows_send_failed( socket, &to, WSAGetLastError() );
        }
        else
        {
            rocketnet_error( "invalid address type. could not send packet" );
        }
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
    buffers[0].buf = (char*) packet_data;
    buffers[0].len = (ULONG) max_packet_size;
    buffers[1].buf = (char*) overflow;
    buffers[1].len = sizeof(overflow);

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

#if defined(ROCKETNET_RECEIVE_TEST_HOOKS)
        if ( rocketnet_platform_receive_hooked( socket, packet_data, max_packet_size, &sockaddr_from, sizeof( sockaddr_from ), &result ) )
        {
            // Preserve the drained sentinel so batch receive cannot manufacture empty packets.
            if ( result < 0 )
                return -1;
            if ( result == 0 )
                continue;
            break;
        }
#endif // #if defined(ROCKETNET_RECEIVE_TEST_HOOKS)

        int status = WSARecvFrom( socket->handle, buffers, 2, &bytes_received, &flags, (sockaddr*) &sockaddr_from, &from_length, NULL, NULL );

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
            // drain at one packet (#100's review). skip it, keep reading:
            // both pumps share this semantic. The skip is counted
            // process-wide (rocketnet_platform_receive_empties), so no
            // dropped datagram goes uncounted.
            InterlockedIncrement64( &rocketnet_platform_receive_empty_count );
            continue;
        }

        break;
    }

    if ( sockaddr_from.ss_family == AF_INET6 )
    {
        sockaddr_in6 * addr_ipv6 = (sockaddr_in6*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );

        if ( !rocketnet_address_accept_received_source( from, dual_stack ) )
            goto receive_again;
    }
    else if ( sockaddr_from.ss_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( (const sockaddr_in*) &sockaddr_from, from );
    }
    else
    {
        rocketnet_assert( 0 );
        return -1;
    }
  
    rocketnet_assert( result >= 0 );

    return result;
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
    const int result = rocketnet_platform_socket_receive_datagram( socket, from, packet_data, max_packet_size, false );
    return result < 0 ? 0 : result;
}

extern void * rocketnet_global_context;

// GetAdaptersAddresses as the fetch's query: on ERROR_BUFFER_OVERFLOW it
// sets the size it needs, which the fetch's next attempt allocates
static int get_connection_type_query( void * buffer, unsigned long * bytes )
{
    ULONG size = *bytes;
    const ULONG return_code = GetAdaptersAddresses( AF_INET, 0, NULL, (IP_ADAPTER_ADDRESSES*) buffer, &size );
    *bytes = size;
    if ( return_code == NO_ERROR )
        return ROCKETNET_PLATFORM_ADAPTERS_OK;
    if ( return_code == ERROR_BUFFER_OVERFLOW )
        return ROCKETNET_PLATFORM_ADAPTERS_OVERFLOW;
    return ROCKETNET_PLATFORM_ADAPTERS_ERROR;
}

static int get_connection_type()
{
    // a refused allocation, an error, or ROCKETNET_PLATFORM_ADAPTERS_ATTEMPTS
    // overflows in a row end rocketnet_platform_adapters_fetch with NULL, and
    // the connection type is unknown
    IP_ADAPTER_ADDRESSES * addresses = (IP_ADAPTER_ADDRESSES*) rocketnet_platform_adapters_fetch( rocketnet_global_context, 15000, get_connection_type_query );
    if ( !addresses )
        return ROCKETNET_CONNECTION_TYPE_UNKNOWN;

    // A nonempty adapter list defaults to wired.
    int result = ROCKETNET_CONNECTION_TYPE_WIRED;

    // if any wifi adapter exists and is connected to a network, assume we're on wifi.
    IP_ADAPTER_ADDRESSES * address = addresses;
    while ( address )
    {
        if ( address->IfType == IF_TYPE_IEEE80211 && address->OperStatus == IfOperStatusUp )
        {
            result = ROCKETNET_CONNECTION_TYPE_WIFI;
            break;
        }
        address = address->Next;
    }

    rocketnet_free( rocketnet_global_context, addresses );

    return result;
}

// ---------------------------------------------------

// The client I/O wake on Windows: a manual-reset event beside an event the
// socket signals on FD_READ (docs/CLIENT-IO.md "The wake" and
// rocketnet-client-io-windows; tla/RNClientIo.tla's IoWait, IoWake and Wake).
// The wake is level-triggered: a set made while no wait runs leaves the event
// signalled, so the next wait returns at once. FD_READ is re-armed by the
// drain the I/O step runs to a would-block, so no readable datagram is missed.

struct rocketnet_platform_wake_t
{
    void * context;
    rocketnet_platform_socket_handle_t socket_handle;
    WSAEVENT read_event;
    HANDLE wake_event;
};

// docs/CLIENT-IO.md "The wake": the socket's FD_READ event and an unset
// manual-reset wake event (tla/RNClientIo.tla's Init: wake = FALSE)
rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket )
{
    rocketnet_platform_wake_t * wake = (rocketnet_platform_wake_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_wake_t ) );
    if ( !wake )
        return NULL;
    wake->context = context;
    wake->socket_handle = socket ? socket->handle : INVALID_SOCKET;
    wake->read_event = WSA_INVALID_EVENT;
    if ( socket )
    {
        wake->read_event = WSACreateEvent();
        if ( wake->read_event == WSA_INVALID_EVENT )
        {
            rocketnet_free( context, wake );
            return NULL;
        }
        if ( WSAEventSelect( (SOCKET) socket->handle, wake->read_event, FD_READ ) != 0 )
        {
            WSACloseEvent( wake->read_event );
            rocketnet_free( context, wake );
            return NULL;
        }
    }
    // Event-only wakes are reused by a worker. Auto-reset consumes a wake
    // atomically with wait success, so a concurrent SetEvent cannot be erased
    // by a later ResetEvent. Socket-backed client wakes keep their existing
    // manual-reset behavior.
    wake->wake_event = CreateEvent( NULL, socket ? TRUE : FALSE, FALSE, NULL );
    if ( wake->wake_event == NULL )
    {
        if ( socket )
        {
            WSAEventSelect( (SOCKET) socket->handle, NULL, 0 );
            WSACloseEvent( wake->read_event );
        }
        rocketnet_free( context, wake );
        return NULL;
    }
    return wake;
}

void rocketnet_platform_wake_destroy( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    if ( wake->read_event != WSA_INVALID_EVENT )
    {
        WSAEventSelect( (SOCKET) wake->socket_handle, NULL, 0 );
        WSACloseEvent( wake->read_event );
    }
    CloseHandle( wake->wake_event );
    rocketnet_free( wake->context, wake );
}

// tla/RNClientIo.tla's IoWait and IoWake: wait on the wake and, only when
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

// tla/RNClientIo.tla's Wake: set the level-triggered wake from any thread
void rocketnet_platform_wake_set( rocketnet_platform_wake_t * wake )
{
    rocketnet_assert( wake );
    SetEvent( wake->wake_event );
}

#if ROCKETNET_UNREAL_ENGINE
#include "Windows/PostWindowsApi.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif // #if ROCKETNET_UNREAL_ENGINE

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// ----------------------------------------------------------------

#if ROCKETNET_TEST_SEAMS
void rocketnet_platform_set_ipv6_socket_available_for_testing( bool available )
{
    rocketnet_platform_ipv6_socket_available_for_testing = available;
}
#endif // #if ROCKETNET_TEST_SEAMS

bool rocketnet_platform_client_ipv6_available()
{
    if ( !rocketnet_platform_ipv6_socket_available_for_testing )
        return false;

    const rocketnet_platform_network_lookups_t * lookups = rocketnet_platform_network_lookups_override.load( std::memory_order_acquire );
    if ( lookups )
        return lookups->ipv6_available();

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    SOCKET probe = socket( AF_INET6, SOCK_DGRAM, IPPROTO_UDP );
    if ( probe == INVALID_SOCKET )
        return false;
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) probe );
    closesocket( probe );
    return true;
#else
    int probe = (int) ::socket( AF_INET6, SOCK_DGRAM, IPPROTO_UDP );
    if ( probe < 0 )
        return false;
    ::close( probe );
    return true;
#endif
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

// ----------------------------------------------------------------

// the batch pump seam's portable implementation (#100): a platform without
// a batch override loops the per-packet calls, so every platform serves the
// seam on day zero (#81's baseline doctrine) and a call site never carries
// an #ifdef. a platform that defines ROCKETNET_PLATFORM_HAS_BATCH_PUMP in
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

rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, rocketnet_platform_socket_t * socket )
{
    (void) context;
    (void) socket;
    return NULL;
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

// pump-request socket create (#106): the transport ladder's entry into the
// platform pump selection. pump NULL or "auto" is the pre-existing
// behavior -- offer the platform's batch pump. "portable" forces the portable pump: the
// request wins and no batch decision matters. "msgx" REQUIRES the darwin
// batch pump: a socket that cannot carry it (including missing symbols)
// refuses loudly here so a pinned rung refuses rather than
// silently degrading. The first real batch call is the functional probe;
// its own live-refusal pause-and-reprobe discipline (#102) remains the
// transport's internal behavior below the ladder.

rocketnet_platform_socket_t * rocketnet_platform_socket_create_with_pump( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, const char * pump )
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

    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_internal( context, address, socket_type, timeout_seconds, send_buffer_size, receive_buffer_size,
                                                                                      force_portable ? ROCKETNET_PLATFORM_PUMP_MODE_FORCE_PORTABLE : ROCKETNET_PLATFORM_PUMP_MODE_AUTO );
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

    return rocketnet_platform_socket_create( context, address, socket_type, timeout_seconds, send_buffer_size, receive_buffer_size );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
}

uint64_t rocketnet_platform_socket_allocation_bytes()
{
    return rocketnet_memory_allocations({sizeof(rocketnet_platform_socket_t)});
}

uint64_t rocketnet_platform_thread_allocation_bytes()
{
    // Includes the transient shim, freed when the worker begins. Counting
    // every shim gives a conservative constructor peak independent of scheduling.
    return rocketnet_memory_allocations({sizeof(rocketnet_platform_thread_t), sizeof(thread_shim_data_t)});
}

uint64_t rocketnet_platform_wake_allocation_bytes()
{
#if defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    return rocketnet_memory_allocations({sizeof(rocketnet_platform_wake_t)});
#else
    return 0;
#endif
}
