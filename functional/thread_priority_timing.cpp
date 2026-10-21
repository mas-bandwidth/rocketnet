/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Network-thread priority timing (docs/CLIENT-IO.md, Thread priority). A
    client connects to a server over loopback; a server thread sends one small
    unreliable payload every 10 ms; the case records the gaps between the
    client's receive wakes. The client time_callback is the receive stamp
    clock and runs on the I/O thread once per transport receive call; calls closer
    than 200 us apart are one wake. Printed: wake gap p50, p99 and max in ms,
    the I/O thread's scheduling as read from inside the thread, and the
    process-wide priority refusal count.

    Off and on are the same binary: the functional worker turns the library's
    priority calls off before rocketnet_init when
    ROCKETNET_FUNCTIONAL_THREAD_PRIORITY=off (functional/main.cpp). Numbers
    are reported, never asserted; each case asserts only that the client stays
    connected with zero timeouts. ROCKETNET_TIMING_SECONDS sets the measured
    stretch (default 2 s). The Linux load runs only in a measured run (with
    ROCKETNET_TIMING_SECONDS set): in the parallel suite, 2 x cores spinning
    threads would starve the cases running beside it.

      test_thread_priority_timing_receive_gaps            default; on Linux under
                                                     2 x cores busy-spinning
                                                     threads of its own
      test_thread_priority_timing_taskpolicy_background   macOS: the same case, the
                                                     whole process under
                                                     `taskpolicy -b`
      test_thread_priority_windows_readback               Windows: each network
                                                     thread's priority read back
*/

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "../test/portable_crt.h"

#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <vector>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include <windows.h>
#include <tlhelp32.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include <dirent.h>
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#include <mach-o/dyld.h>
#include <pthread/qos.h>
#include <spawn.h>
#include <sys/wait.h>
extern char ** environ;
#endif

#define PRIORITY_TIMING_SERVER_INTERVAL 0.010
#define PRIORITY_TIMING_UPDATE_INTERVAL ( 1.0 / 60.0 )
#define PRIORITY_TIMING_WAKE_GROUP 0.0002
#define PRIORITY_TIMING_MAX_WAKES ( 1024 * 1024 )
#define PRIORITY_TIMING_CHILD_ENV "ROCKETNET_PRIORITY_TIMING_TASKPOLICY_CHILD"

static void priority_timing_check( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "thread_priority_timing check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define priority_timing_require( condition ) priority_timing_check( (condition), #condition, __LINE__ )

static double priority_timing_seconds()
{
    char value[256];
    if ( !rocketnet_test_getenv( "ROCKETNET_TIMING_SECONDS", value, sizeof( value ) ) )
        return 2.0;
    const double seconds = atof( value );
    return seconds > 0.0 ? seconds : 2.0;
}

// written only by the client I/O thread while the client lives; read after destroy
struct priority_timing_wakes_t
{
    double last_call;
    double group_start;
    uint64_t calls;
    uint64_t wakes;
    uint64_t dropped;
    int io_policy;      // POSIX: the scheduling policy; Windows: GetThreadPriority
    int io_priority;    // POSIX: the scheduling priority
    int io_qos;         // macOS: the QoS class, pthread_get_qos_class_np; elsewhere -1
    double * intervals;
    uint64_t num_intervals;
};

static priority_timing_wakes_t priority_timing_wakes;

static void priority_timing_read_self( int * policy, int * priority )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    *policy = GetThreadPriority( GetCurrentThread() );
    *priority = 0;
#else
    struct sched_param param;
    memset( &param, 0, sizeof( param ) );
    *policy = -1;
    if ( pthread_getschedparam( pthread_self(), policy, &param ) != 0 )
        *policy = -1;
    *priority = param.sched_priority;
#endif
}

// the calling thread's QoS class on macOS (the library sets none: the one
// macOS call is pthread_setschedparam SCHED_RR); -1 elsewhere
static int priority_timing_read_qos()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relative = 0;
    if ( pthread_get_qos_class_np( pthread_self(), &qos, &relative ) != 0 )
        return -1;
    return (int) qos;
#else
    return -1;
#endif
}

static const char * priority_timing_qos_name( int qos )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    switch ( qos )
    {
        case QOS_CLASS_USER_INTERACTIVE: return "USER_INTERACTIVE";
        case QOS_CLASS_USER_INITIATED: return "USER_INITIATED";
        case QOS_CLASS_DEFAULT: return "DEFAULT";
        case QOS_CLASS_UTILITY: return "UTILITY";
        case QOS_CLASS_BACKGROUND: return "BACKGROUND";
        case QOS_CLASS_UNSPECIFIED: return "UNSPECIFIED";
        default: return "other";
    }
#else
    (void) qos;
    return "none";
#endif
}

uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

static double priority_timing_time_callback( void * context )
{
    (void) context;
    const double now = rocketnet_platform_time();
    priority_timing_wakes_t * w = &priority_timing_wakes;
    if ( w->calls == 0 )
    {
        priority_timing_read_self( &w->io_policy, &w->io_priority );
        w->io_qos = priority_timing_read_qos();
        w->group_start = now;
        w->wakes = 1;
    }
    else if ( now - w->last_call > PRIORITY_TIMING_WAKE_GROUP )
    {
        if ( w->num_intervals < PRIORITY_TIMING_MAX_WAKES )
            w->intervals[w->num_intervals++] = now - w->group_start;
        else
            w->dropped++;
        w->group_start = now;
        w->wakes++;
    }
    w->last_call = now;
    w->calls++;
    return now;
}

static const char * priority_timing_policy_name( int policy )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    switch ( policy )
    {
        case THREAD_PRIORITY_TIME_CRITICAL: return "THREAD_PRIORITY_TIME_CRITICAL";
        case THREAD_PRIORITY_HIGHEST: return "THREAD_PRIORITY_HIGHEST";
        case THREAD_PRIORITY_ABOVE_NORMAL: return "THREAD_PRIORITY_ABOVE_NORMAL";
        case THREAD_PRIORITY_NORMAL: return "THREAD_PRIORITY_NORMAL";
        default: return "other";
    }
#else
    switch ( policy )
    {
        case SCHED_RR: return "SCHED_RR";
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_OTHER: return "SCHED_OTHER";
        default: return "other";
    }
#endif
}

struct priority_timing_stats_t
{
    double p50, p99, max;
};

static priority_timing_stats_t priority_timing_stats( std::vector<double> & values )
{
    priority_timing_stats_t s;
    s.p50 = s.p99 = s.max = 0.0;
    if ( values.empty() )
        return s;
    std::sort( values.begin(), values.end() );
    const size_t n = values.size();
    s.p50 = values[( n - 1 ) / 2];
    s.p99 = values[(size_t) ( (double) ( n - 1 ) * 0.99 )];
    s.max = values[n - 1];
    return s;
}

// the warnings the library logs, counted: one line per refused call kind
static std::atomic<int> priority_timing_refusal_lines{ 0 };
static std::atomic<int> priority_timing_iocp_workers{ -1 };
static std::atomic<int> priority_timing_gso_queues{ 0 };
static char priority_timing_first_refusal[512];

// a log line "<prefix> <count>...": the count after the prefix, as sscanf's
// "<prefix> %d" reads it (whitespace skipped, an optional sign, decimal digits)
static bool priority_timing_parse_count( const char * message, const char * prefix, int * value )
{
    const size_t prefix_length = strlen( prefix );
    if ( strncmp( message, prefix, prefix_length ) != 0 )
        return false;
    const char * start = message + prefix_length;
    char * end = NULL;
    const long parsed = strtol( start, &end, 10 );
    if ( end == start )
        return false;
    *value = (int) parsed;
    return true;
}

static void priority_timing_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( strstr( message, "thread priority:" ) || strstr( message, "thread affinity:" ) || strstr( message, "timer period:" ) )
    {
        if ( priority_timing_refusal_lines.fetch_add( 1 ) == 0 )
            snprintf( priority_timing_first_refusal, sizeof( priority_timing_first_refusal ), "%s", message );
    }
    int workers = 0;
    if ( priority_timing_parse_count( message, "iocp transport:", &workers ) )
        priority_timing_iocp_workers.store( workers );
    int queues = 0;
    if ( priority_timing_parse_count( message, "gso transport:", &queues ) )
        priority_timing_gso_queues.store( queues );
    if ( level <= ROCKETNET_LOG_LEVEL_ERROR )
        printf( "%s\n", message );
}

struct priority_timing_server_t
{
    struct rocketnet_server_transport_t * server;
    double start;
    std::atomic<bool> stop;
    uint64_t sent;
};

static double priority_timing_game_time( double start )
{
    return 1000.0 + ( rocketnet_platform_time() - start );
}

static void priority_timing_server_thread( priority_timing_server_t * s )
{
    double next = rocketnet_platform_time();
    while ( !s->stop.load( std::memory_order_acquire ) )
    {
        rocketnet_server_transport_update( s->server, priority_timing_game_time( s->start ) );
        (void) rocketnet_server_transport_num_payloads( s->server );
        if ( rocketnet_server_transport_num_connected_clients( s->server ) == 1 )
        {
            struct rocketnet_packet_context_t context;
            if ( rocketnet_server_transport_start_packet( s->server, 0, 8, &context ) )
            {
                int capacity = 0;
                uint8_t * payload = rocketnet_server_transport_packet_payload( s->server, &context, &capacity );
                if ( payload && capacity >= 8 )
                {
                    const double sent_at = rocketnet_platform_time();
                    memcpy( payload, &sent_at, 8 );
                    if ( rocketnet_server_transport_finish_packet( s->server, &context, 8 ) > 0 )
                        s->sent++;
                }
            }
            rocketnet_server_transport_send_packets( s->server );
        }
        next += PRIORITY_TIMING_SERVER_INTERVAL;
        const double wait = next - rocketnet_platform_time();
        if ( wait > 0.0 )
            rocketnet_platform_sleep( wait );
    }
}

static std::atomic<bool> priority_timing_spin_stop{ false };

static void priority_timing_spinner()
{
    volatile uint64_t n = 0;
    while ( !priority_timing_spin_stop.load( std::memory_order_relaxed ) )
        n = n + 1;
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
// every thread of this process at THREAD_PRIORITY_TIME_CRITICAL, read with
// GetThreadPriority through a thread snapshot
static int priority_timing_time_critical_threads()
{
    HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( snapshot == INVALID_HANDLE_VALUE )
        return -1;
    const DWORD process = GetCurrentProcessId();
    int count = 0;
    THREADENTRY32 entry;
    entry.dwSize = sizeof( entry );
    for ( BOOL more = Thread32First( snapshot, &entry ); more; more = Thread32Next( snapshot, &entry ) )
    {
        if ( entry.th32OwnerProcessID != process )
            continue;
        HANDLE thread = OpenThread( THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID );
        if ( !thread )
            continue;
        if ( GetThreadPriority( thread ) == THREAD_PRIORITY_TIME_CRITICAL )
            count++;
        CloseHandle( thread );
    }
    CloseHandle( snapshot );
    return count;
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
// every thread of this process whose policy is SCHED_RR, read from
// /proc/self/task/<tid>/stat (field 41, policy)
static int priority_timing_sched_rr_threads()
{
    DIR * dir = opendir( "/proc/self/task" );
    if ( !dir )
        return -1;
    int count = 0;
    struct dirent * entry;
    while ( ( entry = readdir( dir ) ) != NULL )
    {
        if ( entry->d_name[0] == '.' )
            continue;
        char path[300];
        snprintf( path, sizeof( path ), "/proc/self/task/%s/stat", entry->d_name );
        FILE * file = fopen( path, "r" );
        if ( !file )
            continue;
        char line[2048];
        const size_t bytes = fread( line, 1, sizeof( line ) - 1, file );
        fclose( file );
        line[bytes] = '\0';
        // fields after the command's closing parenthesis start at field 3
        char * rest = strrchr( line, ')' );
        if ( !rest )
            continue;
        int field = 3;
        char * save = NULL;
        for ( char * token = strtok_r( rest + 1, " ", &save ); token; token = strtok_r( NULL, " ", &save ), ++field )
        {
            if ( field == 41 )
            {
                if ( atoi( token ) == SCHED_RR )
                    count++;
                break;
            }
        }
    }
    closedir( dir );
    return count;
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

static void priority_timing_run( const char * condition, int spinners, bool readback )
{
    const double seconds = priority_timing_seconds();
    const bool enabled = rocketnet_get_thread_priority();

    memset( &priority_timing_wakes, 0, sizeof( priority_timing_wakes ) );
    std::vector<double> wake_storage( PRIORITY_TIMING_MAX_WAKES );
    priority_timing_wakes.intervals = wake_storage.data();

    // the refusal lines are warnings: raise the level for the case and count them
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( priority_timing_log, NULL );

    const double start = rocketnet_platform_time();

    // the server create makes one priority call per server network thread
    // (the GSO queue threads on Linux, the IOCP workers on Windows)
    const uint64_t refusals_before_server = rocketnet_get_thread_priority_refusals();

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;
    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, priority_timing_game_time( start ), &status );
    priority_timing_require( server != NULL );
    priority_timing_require( status == ROCKETNET_STATUS_OK );
    const uint64_t server_refusals = rocketnet_get_thread_priority_refusals() - refusals_before_server;

    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_transport_port( server ) );

    priority_timing_server_t server_state;
    server_state.server = server;
    server_state.start = start;
    server_state.stop.store( false );
    server_state.sent = 0;
    std::thread server_thread( priority_timing_server_thread, &server_state );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.time_callback = priority_timing_time_callback;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    priority_timing_require( rocketnet_write_test_connect_token( server_address, 1, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, priority_timing_game_time( start ) ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    // the client create makes the client network thread's priority call
    // (rocketnet_client_transport_create, after its thread create): the
    // refusals it adds are counted around the create alone
    const uint64_t refusals_before_client = rocketnet_get_thread_priority_refusals();
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, priority_timing_game_time( start ) );
    const uint64_t client_refusals = rocketnet_get_thread_priority_refusals() - refusals_before_client;
    priority_timing_require( client != NULL );

    double next = rocketnet_platform_time();
    bool connected = false;
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, priority_timing_game_time( start ) );
        (void) rocketnet_client_transport_num_payloads( client );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            connected = true;
            break;
        }
        next += PRIORITY_TIMING_UPDATE_INTERVAL;
        const double wait = next - rocketnet_platform_time();
        if ( wait > 0.0 )
            rocketnet_platform_sleep( wait );
    }
    priority_timing_require( connected );

    // the load starts after the connect, so it only competes with the measured stretch
    priority_timing_spin_stop.store( false );
    std::vector<std::thread> load;
    for ( int i = 0; i < spinners; ++i )
        load.push_back( std::thread( priority_timing_spinner ) );

    // the wakes before the measured stretch are not counted
    priority_timing_wakes.num_intervals = 0;

    uint64_t timeouts = 0;
    uint64_t received = 0;
    bool stayed = true;
    const double measure_start = rocketnet_platform_time();
    next = measure_start;
    while ( rocketnet_platform_time() - measure_start < seconds )
    {
        rocketnet_client_transport_update( client, priority_timing_game_time( start ) );
        received += (uint64_t) rocketnet_client_transport_num_payloads( client );
        const int state = rocketnet_client_transport_state( client );
        if ( state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            stayed = false;
            if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT )
                timeouts++;
            break;
        }
        next += PRIORITY_TIMING_UPDATE_INTERVAL;
        const double wait = next - rocketnet_platform_time();
        if ( wait > 0.0 )
            rocketnet_platform_sleep( wait );
    }
    const double measured_seconds = rocketnet_platform_time() - measure_start;

    int time_critical_threads = -1;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( readback )
        time_critical_threads = priority_timing_time_critical_threads();
#else
    (void) readback;
#endif

    int sched_rr_threads = -1;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    sched_rr_threads = priority_timing_sched_rr_threads();
#endif

    priority_timing_spin_stop.store( true );
    for ( size_t i = 0; i < load.size(); ++i )
        load[i].join();

    rocketnet_client_transport_destroy( client );
    server_state.stop.store( true, std::memory_order_release );
    server_thread.join();
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, priority_timing_game_time( start ) );
    rocketnet_server_transport_destroy( server );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    std::vector<double> wakes( wake_storage.begin(), wake_storage.begin() + (ptrdiff_t) priority_timing_wakes.num_intervals );
    const priority_timing_stats_t w = priority_timing_stats( wakes );

    printf( "thread_priority_timing condition=%s priority=%s seconds=%.1f spinners=%d io_sched=%s io_priority=%d io_qos=%s client_refusals=%llu server_refusals=%llu gso_queues=%d sched_rr_threads=%d "
            "wakes=%llu wake_dropped=%llu gap_ms p50=%.3f p99=%.3f max=%.3f received=%llu server_sent=%llu "
            "timeouts=%llu refusals=%llu refusal_lines=%d iocp_workers=%d time_critical_threads=%d\n",
            condition, enabled ? "on" : "off", measured_seconds, spinners,
            priority_timing_policy_name( priority_timing_wakes.io_policy ), priority_timing_wakes.io_priority,
            priority_timing_qos_name( priority_timing_wakes.io_qos ), (unsigned long long) client_refusals,
            (unsigned long long) server_refusals, priority_timing_gso_queues.load(), sched_rr_threads,
            (unsigned long long) priority_timing_wakes.wakes, (unsigned long long) priority_timing_wakes.dropped,
            w.p50 * 1000.0, w.p99 * 1000.0, w.max * 1000.0,
            (unsigned long long) received, (unsigned long long) server_state.sent,
            (unsigned long long) timeouts, (unsigned long long) rocketnet_get_thread_priority_refusals(),
            priority_timing_refusal_lines.load(), priority_timing_iocp_workers.load(), time_critical_threads );
    if ( priority_timing_first_refusal[0] )
        printf( "thread_priority_timing first refusal line: %s\n", priority_timing_first_refusal );
    fflush( stdout );

    priority_timing_require( stayed );
    priority_timing_require( timeouts == 0 );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    // macOS: the library sets no QoS class on its threads, on or off; the I/O
    // thread's class, read inside it, is never the USER_INTERACTIVE a
    // library call would give it
    priority_timing_require( priority_timing_wakes.io_qos != (int) QOS_CLASS_USER_INTERACTIVE );
#endif

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    // POSIX: on, the client network thread made its one call,
    // pthread_setschedparam SCHED_RR at sched_get_priority_min( SCHED_RR ):
    // either granted (read back inside the I/O thread) or refused (counted
    // during the client create). off, neither
    const bool io_granted = priority_timing_wakes.io_policy == SCHED_RR && priority_timing_wakes.io_priority == sched_get_priority_min( SCHED_RR );
    if ( enabled )
        priority_timing_require( io_granted || client_refusals >= 1 );
    else
        priority_timing_require( !io_granted && client_refusals == 0 );
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    // Linux: every library network thread, the client thread and each GSO
    // queue thread, made its one call: on, each is either SCHED_RR (read back
    // from /proc) or a counted refusal; off, none is either
    const int network_threads = 1 + priority_timing_gso_queues.load();
    priority_timing_require( sched_rr_threads >= 0 );
    if ( enabled )
        priority_timing_require( sched_rr_threads + (int) ( client_refusals + server_refusals ) == network_threads );
    else
        priority_timing_require( sched_rr_threads == 0 && client_refusals + server_refusals == 0 );
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( readback )
    {
        // inside the I/O thread: TIME_CRITICAL on, the default off. outside,
        // by snapshot: the client I/O thread plus every IOCP worker, and no
        // other thread, is TIME_CRITICAL on; none off. the I/O thread's read
        // is its first receive call, after the create returned
        const int workers = priority_timing_iocp_workers.load() > 0 ? priority_timing_iocp_workers.load() : 0;
        if ( enabled )
        {
            priority_timing_require( priority_timing_wakes.io_policy == THREAD_PRIORITY_TIME_CRITICAL );
            priority_timing_require( time_critical_threads == 1 + workers );
        }
        else
        {
            priority_timing_require( priority_timing_wakes.io_policy != THREAD_PRIORITY_TIME_CRITICAL );
            priority_timing_require( time_critical_threads == 0 );
        }
    }
#endif
}

void thread_priority_timing_receive_gaps()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    const unsigned int cores = std::thread::hardware_concurrency();
    if ( getenv( "ROCKETNET_TIMING_SECONDS" ) )
        priority_timing_run( "receive_gaps_under_load", (int) ( 2 * ( cores ? cores : 1 ) ), false );
    else
        priority_timing_run( "receive_gaps", 0, false );
#else
    priority_timing_run( "receive_gaps", 0, false );
#endif
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
void thread_priority_windows_readback()
{
    priority_timing_run( "windows_readback", 0, true );
}
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
void thread_priority_timing_taskpolicy_background()
{
    if ( getenv( PRIORITY_TIMING_CHILD_ENV ) )
    {
        priority_timing_run( "taskpolicy_background", 0, false );
        return;
    }
    char exe[4096];
    uint32_t exe_bytes = sizeof( exe );
    priority_timing_require( _NSGetExecutablePath( exe, &exe_bytes ) == 0 );
    priority_timing_require( setenv( PRIORITY_TIMING_CHILD_ENV, "1", 1 ) == 0 );
    char taskpolicy[] = "/usr/sbin/taskpolicy";
    char background[] = "-b";
    char run[] = "--run";
    char name[] = "test_thread_priority_timing_taskpolicy_background";
    char * argv[] = { taskpolicy, background, exe, run, name, NULL };
    pid_t pid = 0;
    const int spawned = posix_spawn( &pid, taskpolicy, NULL, NULL, argv, environ );
    unsetenv( PRIORITY_TIMING_CHILD_ENV );
    priority_timing_require( spawned == 0 );
    int child_status = 0;
    priority_timing_require( waitpid( pid, &child_status, 0 ) == pid );
    priority_timing_require( WIFEXITED( child_status ) && WEXITSTATUS( child_status ) == 0 );
}
#endif
