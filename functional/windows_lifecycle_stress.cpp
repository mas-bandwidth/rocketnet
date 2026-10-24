/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Windows lifecycle stress, the Windows form of
    functional/posix_lifecycle_stress.cpp. Every case cycles a real server
    transport and the portable client over real loopback sockets 1000 times
    and counts the process's handles, threads and private bytes:

        handles        GetProcessHandleCount
        threads        a Toolhelp32 walk of this process's threads
        heap_bytes     HeapSummary cbAllocated, summed over GetProcessHeaps
        private_bytes  GetProcessMemoryInfo PrivateUsage

    These are the Windows counters of tests/process_resources.h
    and load.c, read the same way, and
    the comparison is that header's flat rule: the counts are taken after one
    warm cycle, so every path the case repeats has run once; at the end the
    handle count must equal its warm count exactly and the thread count must
    not exceed it (the system thread pool retires idle workers of its own, so
    fewer threads pass; a thread left behind is one more). Heap bytes in use,
    the Windows form of the POSIX file's mallinfo2 bytes in use, may exceed
    the warm count by at most one page for every ten minutes of run, rounded
    up, never less than one page. Private bytes also count the heap's free
    but committed pages, which a heap keeps as it fragments, so they are
    judged per cycle: the growth must stay under one page per cycle, which
    any mapping left behind by a cycle exceeds. Counts below the warm count
    pass. The run time is read from the performance counter directly, since
    rocketnet_platform_time restarts at rocketnet_init.
    Each case prints one line with the before and after counts and the run
    time, and fails when a count moved.

    The cells are the Windows server transports (portable, iocp, rio), each
    with the portable client, and each case is one function per cell:
    rocketnet_test_windows_<case>_<cell>. The portable cells are registered in
    functional/rocketnet_functional_tests.cpp; the iocp and rio cells are
    defined here and registered by a follow-up change.

    A connect waits on the client's send rate (10 sends a second,
    sdk/rocketnet_client_protocol.cpp), so the client runs on its own clock,
    the platform clock plus the send intervals the wait has skipped, and the
    wait skips one interval each time the client's state moves, as
    functional/posix_lifecycle_stress.cpp does. Same host is correctness
    only: the run times are printed, never judged.

    On other platforms the file builds empty.
*/

#include "../tests/native_shutdown.h"
#include "rocketnet_counters.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

// defined in functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

static void windows_lifecycle_check_line( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "windows lifecycle stress check failed: %s:%d: %s\n", __FILE__, line, expression );
    fflush( stdout );
    exit( 1 );
}

#define windows_lifecycle_check( condition ) windows_lifecycle_check_line( (condition), #condition, __LINE__ )

#define WINDOWS_LIFECYCLE_CYCLES 1000

// ---------------------------------------------------------------
// the counts
// ---------------------------------------------------------------

struct windows_resource_counts_t
{
    int64_t handles;
    int64_t threads;
    int64_t heap_bytes;
    int64_t private_bytes;
};

// the run time, from the performance counter: rocketnet_platform_time
// restarts at rocketnet_init, which the init_term case calls every cycle
static double windows_run_clock()
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency( &frequency );
    QueryPerformanceCounter( &counter );
    return (double) counter.QuadPart / (double) frequency.QuadPart;
}

// bytes allocated and not freed, over every heap of the process
static int64_t windows_heap_bytes()
{
    HANDLE heaps[256];
    const DWORD heap_count = GetProcessHeaps( 256, heaps );
    if ( heap_count == 0 || heap_count > 256 )
        return -1;
    int64_t bytes = 0;
    for ( DWORD i = 0; i < heap_count; i++ )
    {
        HEAP_SUMMARY summary;
        memset( &summary, 0, sizeof( summary ) );
        summary.cb = sizeof( summary );
        if ( !HeapSummary( heaps[i], 0, &summary ) )
            return -1;
        bytes += (int64_t) summary.cbAllocated;
    }
    return bytes;
}

static int64_t windows_threads()
{
    HANDLE thread_list = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( thread_list == INVALID_HANDLE_VALUE )
        return -1;
    THREADENTRY32 entry;
    memset( &entry, 0, sizeof( entry ) );
    entry.dwSize = sizeof( entry );
    const DWORD self_process_id = GetCurrentProcessId();
    int64_t threads = 0;
    if ( Thread32First( thread_list, &entry ) )
    {
        do
        {
            if ( entry.th32OwnerProcessID == self_process_id )
                threads++;
        } while ( Thread32Next( thread_list, &entry ) );
    }
    CloseHandle( thread_list );
    return threads;
}

static windows_resource_counts_t windows_read_counts()
{
    windows_resource_counts_t counts;
    counts.handles = -1;
    counts.threads = -1;
    counts.heap_bytes = -1;
    counts.private_bytes = -1;

    // the handle count first: the thread walk opens and closes one handle
    DWORD handles = 0;
    if ( GetProcessHandleCount( GetCurrentProcess(), &handles ) )
        counts.handles = (int64_t) handles;

    counts.threads = windows_threads();
    counts.heap_bytes = windows_heap_bytes();

    PROCESS_MEMORY_COUNTERS_EX memory;
    memset( &memory, 0, sizeof( memory ) );
    memory.cb = sizeof( memory );
    if ( GetProcessMemoryInfo( GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &memory, sizeof( memory ) ) )
        counts.private_bytes = (int64_t) memory.PrivateUsage;

    return counts;
}

static int64_t windows_page_bytes()
{
    SYSTEM_INFO info;
    GetSystemInfo( &info );
    return (int64_t) info.dwPageSize;
}

// the private bytes growth the flat rule allows over seconds of run: one page
// per ten minutes, rounded up, never less than one page
static int64_t windows_memory_allowance_bytes( double seconds )
{
    int64_t periods = seconds > 0.0 ? (int64_t) ( seconds / 600.0 ) : 0;
    if ( (double) periods * 600.0 < seconds )
        periods++;
    if ( periods < 1 )
        periods = 1;
    return periods * windows_page_bytes();
}

// private bytes: under one page per cycle (see the header)
static int64_t windows_private_allowance_bytes()
{
    return (int64_t) WINDOWS_LIFECYCLE_CYCLES * windows_page_bytes();
}

static bool windows_counts_flat( const windows_resource_counts_t * before, const windows_resource_counts_t * after, double seconds )
{
    if ( before->handles < 0 || before->threads < 0 || before->heap_bytes < 0 || before->private_bytes < 0 ||
         after->handles < 0 || after->threads < 0 || after->heap_bytes < 0 || after->private_bytes < 0 )
        return false;
    return after->handles == before->handles && after->threads <= before->threads &&
           after->heap_bytes <= before->heap_bytes + windows_memory_allowance_bytes( seconds ) &&
           after->private_bytes < before->private_bytes + windows_private_allowance_bytes();
}

// A transport's threads are joined inside destroy, and a joined thread leaves
// the Toolhelp list once the kernel has run it down; the handles it held close
// with it. The after counts are polled every millisecond until they are flat
// against the before counts or WINDOWS_SETTLE_SECONDS have passed. A handle,
// thread or allocation really left keeps its count, and the case still fails
// at the deadline.
static const double WINDOWS_SETTLE_SECONDS = 10.0;

static windows_resource_counts_t windows_settled_counts( const windows_resource_counts_t * before, double seconds )
{
    const double deadline = rocketnet_platform_time() + WINDOWS_SETTLE_SECONDS;
    windows_resource_counts_t current = windows_read_counts();
    while ( !windows_counts_flat( before, &current, seconds ) && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
        current = windows_read_counts();
    }
    return current;
}

// the before counts: read after the warm cycle, once two reads 1 ms apart agree
static windows_resource_counts_t windows_stable_counts()
{
    windows_resource_counts_t previous = windows_read_counts();
    for ( int attempt = 0; attempt < 200; attempt++ )
    {
        rocketnet_platform_sleep( 0.001 );
        const windows_resource_counts_t current = windows_read_counts();
        if ( current.handles == previous.handles && current.threads == previous.threads && current.heap_bytes == previous.heap_bytes && current.private_bytes == previous.private_bytes )
            return current;
        previous = current;
    }
    return previous;
}

// ---------------------------------------------------------------
// the cells
// ---------------------------------------------------------------

struct windows_cell_t
{
    const char * name;
    const char * server_transport;
};

static const windows_cell_t windows_cells[] =
{
    { "server portable, client portable", "portable" },
    { "server iocp, client portable",     "iocp" },
    { "server rio, client portable",      "rio" },
};

static const int windows_cell_count = (int) ( sizeof( windows_cells ) / sizeof( windows_cells[0] ) );

static const windows_cell_t * windows_cell( const char * server_transport )
{
    for ( int i = 0; i < windows_cell_count; i++ )
    {
        if ( strcmp( windows_cells[i].server_transport, server_transport ) == 0 )
            return &windows_cells[i];
    }
    windows_lifecycle_check( false );
    return NULL;
}

static void windows_clear_transport_env()
{
    // the cell pins its own transports: an ambient pin must not override them
    _putenv_s( "ROCKETNET_TRANSPORT", "" );
    _putenv_s( "ROCKETNET_SERVER_TRANSPORT", "" );
    _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", "" );
}

static rocketnet_server_transport_t * windows_server_create( const windows_cell_t * cell, const char * address, int max_clients )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;
    snprintf( config.transport, sizeof( config.transport ), "%s", cell->server_transport );
    int status = ROCKETNET_STATUS_OK;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, address, &config, rocketnet_platform_time(), &status );
    if ( !server )
        printf( "server create [%s] at %s failed: %s\n", cell->name, address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    windows_lifecycle_check( server != NULL );
    windows_lifecycle_check( strcmp( rocketnet_server_transport_io_name( server ), cell->server_transport ) == 0 );
    return server;
}

static void windows_server_address( rocketnet_server_transport_t * server, char * buffer, size_t buffer_bytes )
{
    snprintf( buffer, buffer_bytes, "127.0.0.1:%u", (unsigned) rocketnet_server_transport_port( server ) );
}

// the client's clock: the platform clock plus the send intervals skipped.
// Read by the client's receive thread through time_callback, so atomic.
#define WINDOWS_CLIENT_SEND_INTERVAL 0.1

struct windows_client_clock_t
{
    std::atomic<int> skipped;
};

static double windows_client_clock_now( windows_client_clock_t * clock )
{
    return rocketnet_platform_time() + clock->skipped.load( std::memory_order_acquire ) * WINDOWS_CLIENT_SEND_INTERVAL;
}

static double windows_client_clock_callback( void * context )
{
    return windows_client_clock_now( (windows_client_clock_t*) context );
}

static rocketnet_client_transport_t * windows_client_create( const windows_cell_t * cell, const char * server_address, windows_client_clock_t * clock )
{
    clock->skipped.store( 0, std::memory_order_release );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    windows_lifecycle_check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(),
                                                                 rocketnet_functional_token_timestamp( server_config.game_clock_epoch, 1000.0 ),
                                                                 token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.time_callback = windows_client_clock_callback;
    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * client = rocketnet_client_transport_create_ex( clock, token, &config, windows_client_clock_now( clock ), &status );
    if ( !client )
        printf( "client create [%s] to %s failed: %s\n", cell->name, server_address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    windows_lifecycle_check( client != NULL );
    windows_lifecycle_check( strcmp( rocketnet_client_transport_io_name( client ), "portable" ) == 0 );
    return client;
}

// a stalled connect prints the client's state and every nonzero counter of
// the client and the server, so a stall names the side that stopped
static void windows_print_counters( const char * side, const struct rocketnet_counters_t * counters )
{
    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        const uint64_t value = rocketnet_counter_value( counters, i );
        if ( value != 0 )
            printf( "  %s %s = %llu\n", side, rocketnet_counter_name( i ), (unsigned long long) value );
    }
}

static void windows_print_stall( rocketnet_server_transport_t * server, rocketnet_client_transport_t * client )
{
    const int state = rocketnet_client_transport_state( client );
    printf( "connect stalled: client state %s (%d), server connected clients %d\n", rocketnet_client_transport_state_name( state ),
            state, rocketnet_server_transport_num_connected_clients( server ) );
    windows_print_counters( "client", rocketnet_client_transport_counters( client ) );
    windows_print_counters( "server", rocketnet_server_transport_counters( server ) );
    fflush( stdout );
}

static bool windows_wait_connected( rocketnet_server_transport_t * server, rocketnet_client_transport_t * client,
                                    windows_client_clock_t * clock, double seconds )
{
    const double deadline = rocketnet_platform_time() + seconds;
    int previous_state = rocketnet_client_transport_state( client );
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_client_transport_update( client, windows_client_clock_now( clock ) );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        const int state = rocketnet_client_transport_state( client );
        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            return true;
        if ( state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        {
            windows_print_stall( server, client );
            return false;
        }
        if ( state != previous_state )
        {
            // the state moved: the next update sends without waiting out the
            // interval; an unanswered send is resent on the client's own
            // schedule, so its timeouts run at the platform clock's pace
            previous_state = state;
            clock->skipped.fetch_add( 1, std::memory_order_acq_rel );
            continue;
        }
        rocketnet_platform_sleep( 0.0002 );
    }
    windows_print_stall( server, client );
    return false;
}

static bool windows_wait_server_empty( rocketnet_server_transport_t * server, rocketnet_client_transport_t * client,
                                       windows_client_clock_t * clock, double seconds )
{
    const double deadline = rocketnet_platform_time() + seconds;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_client_transport_update( client, windows_client_clock_now( clock ) );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            return true;
        rocketnet_platform_sleep( 0.0002 );
    }
    return rocketnet_server_transport_num_connected_clients( server ) == 0;
}

// one result line per case: the before and after counts and the run time
static void windows_case_result( const char * case_name, const windows_cell_t * cell, const windows_resource_counts_t * before,
                                 const windows_resource_counts_t * after, double seconds )
{
    const bool flat = windows_counts_flat( before, after, seconds );
    printf( "%s [%s]: handles %lld -> %lld, threads %lld -> %lld, heap_bytes %lld -> %lld (allowed +%lld), private_bytes %lld -> %lld (allowed under +%lld), %.3fs%s\n",
            case_name, cell->name, (long long) before->handles, (long long) after->handles, (long long) before->threads,
            (long long) after->threads, (long long) before->heap_bytes, (long long) after->heap_bytes,
            (long long) windows_memory_allowance_bytes( seconds ), (long long) before->private_bytes, (long long) after->private_bytes,
            (long long) windows_private_allowance_bytes(), seconds, flat ? "" : " FAIL resource count moved" );
    fflush( stdout );
    if ( !flat )
        exit( 1 );
}

// ---------------------------------------------------------------
// one cycle of each case
// ---------------------------------------------------------------

static void windows_init_term_cycle()
{
    windows_lifecycle_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    windows_lifecycle_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static void windows_server_create_destroy_cycle( const windows_cell_t * cell )
{
    rn_native_server_transport_stop_and_destroy( windows_server_create( cell, "127.0.0.1:0", 4 ) );
}

static void windows_client_create_destroy_cycle( const windows_cell_t * cell )
{
    windows_client_clock_t clock;
    rn_native_client_transport_stop_and_destroy( windows_client_create( cell, "127.0.0.1:40000", &clock ) );
}

static void windows_connect_disconnect_cycle( const windows_cell_t * cell, rocketnet_server_transport_t * server,
                                              const char * server_address, int cycle )
{
    windows_client_clock_t clock;
    rocketnet_client_transport_t * client = windows_client_create( cell, server_address, &clock );
    if ( !windows_wait_connected( server, client, &clock, 5.0 ) )
        printf( "[%s]: connect stalled at cycle %d\n", cell->name, cycle );
    windows_lifecycle_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_client_transport_disconnect( client );
    windows_lifecycle_check( windows_wait_server_empty( server, client, &clock, 5.0 ) );
    rn_native_client_transport_stop_and_destroy( client );
}

// stop and start the server on its one port, and a client reconnects to it
static void windows_server_restart_cycle( const windows_cell_t * cell, const char * fixed_address, unsigned fixed_port, int cycle )
{
    rocketnet_server_transport_t * server = windows_server_create( cell, fixed_address, 4 );
    windows_lifecycle_check( rocketnet_server_transport_port( server ) == fixed_port );
    windows_client_clock_t clock;
    rocketnet_client_transport_t * client = windows_client_create( cell, fixed_address, &clock );
    if ( !windows_wait_connected( server, client, &clock, 5.0 ) )
        printf( "[%s]: reconnect stalled at restart %d\n", cell->name, cycle );
    windows_lifecycle_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rn_native_client_transport_stop_and_destroy( client );
    rn_native_server_transport_stop_and_destroy( server );
}

// The port the restart case binds for its whole life. A port the kernel picks
// from the dynamic range (49152-65535 by default) is not the case's to keep:
// once released, any other case's client binding port 0 can take it before
// the next start. Take a free port below the dynamic range instead, from a
// slice this cell alone scans, starting at a point set by the process id so
// two suites at once start apart.
static unsigned windows_reserved_restart_port( const windows_cell_t * cell )
{
    const unsigned first_port = 20000;
    const unsigned last_port = 29999;
    const unsigned slice = ( last_port - first_port + 1 ) / (unsigned) windows_cell_count;
    const unsigned base = first_port + (unsigned) ( cell - windows_cells ) * slice;
    unsigned port = base + (unsigned) GetCurrentProcessId() % slice;
    for ( unsigned i = 0; i < slice; i++ )
    {
        const SOCKET probe = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
        if ( probe == INVALID_SOCKET )
            return 0;
        struct sockaddr_in address;
        memset( &address, 0, sizeof( address ) );
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        address.sin_port = htons( (u_short) port );
        const int result = bind( probe, (const struct sockaddr*) &address, (int) sizeof( address ) );
        closesocket( probe );
        if ( result == 0 )
            return port;
        port = base + ( port - base + 1 ) % slice;
    }
    return 0;
}

// ---------------------------------------------------------------
// the cases: the warm cycle, the before counts, 1000 cycles, the after counts
// ---------------------------------------------------------------

static void windows_init_term_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );
    windows_clear_transport_env();
    windows_init_term_cycle();
    const windows_resource_counts_t before = windows_stable_counts();
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
        windows_init_term_cycle();
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_case_result( "init_term_1000", cell, &before, &after, seconds );
}

static void windows_server_create_destroy_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );
    windows_clear_transport_env();
    windows_server_create_destroy_cycle( cell );
    const windows_resource_counts_t before = windows_stable_counts();
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
        windows_server_create_destroy_cycle( cell );
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_case_result( "server_create_destroy_1000", cell, &before, &after, seconds );
}

static void windows_client_create_destroy_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );
    windows_clear_transport_env();
    windows_client_create_destroy_cycle( cell );
    const windows_resource_counts_t before = windows_stable_counts();
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
        windows_client_create_destroy_cycle( cell );
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_case_result( "client_create_destroy_1000", cell, &before, &after, seconds );
}

static void windows_connect_disconnect_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );
    windows_clear_transport_env();
    rocketnet_server_transport_t * server = windows_server_create( cell, "127.0.0.1:0", 8 );
    char server_address[64];
    windows_server_address( server, server_address, sizeof( server_address ) );
    windows_connect_disconnect_cycle( cell, server, server_address, -1 );
    const windows_resource_counts_t before = windows_stable_counts();
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
        windows_connect_disconnect_cycle( cell, server, server_address, i );
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    rn_native_server_transport_stop_and_destroy( server );
    windows_case_result( "connect_disconnect_1000", cell, &before, &after, seconds );
}

static void windows_server_restart_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );
    windows_clear_transport_env();
    const unsigned fixed_port = windows_reserved_restart_port( cell );
    windows_lifecycle_check( fixed_port != 0 );
    char fixed_address[64];
    snprintf( fixed_address, sizeof( fixed_address ), "127.0.0.1:%u", fixed_port );
    windows_server_restart_cycle( cell, fixed_address, fixed_port, -1 );
    const windows_resource_counts_t before = windows_stable_counts();
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
        windows_server_restart_cycle( cell, fixed_address, fixed_port, i );
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_case_result( "server_restart_1000", cell, &before, &after, seconds );
}

// one function per case and cell, rocketnet_test_windows_<case>_<cell>
#define WINDOWS_LIFECYCLE_CELL_CASE( case_name, transport )                                 \
void rocketnet_test_windows_##case_name##_##transport();                                    \
void rocketnet_test_windows_##case_name##_##transport()                                     \
{                                                                                           \
    windows_##case_name( #transport );                                                      \
}

#define WINDOWS_LIFECYCLE_CELL_CASES( transport )                                           \
    WINDOWS_LIFECYCLE_CELL_CASE( init_term_1000, transport )                                \
    WINDOWS_LIFECYCLE_CELL_CASE( server_create_destroy_1000, transport )                    \
    WINDOWS_LIFECYCLE_CELL_CASE( client_create_destroy_1000, transport )                    \
    WINDOWS_LIFECYCLE_CELL_CASE( connect_disconnect_1000, transport )                       \
    WINDOWS_LIFECYCLE_CELL_CASE( server_restart_1000, transport )

WINDOWS_LIFECYCLE_CELL_CASES( portable )
WINDOWS_LIFECYCLE_CELL_CASES( iocp )
WINDOWS_LIFECYCLE_CELL_CASES( rio )

#undef WINDOWS_LIFECYCLE_CELL_CASES
#undef WINDOWS_LIFECYCLE_CELL_CASE

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
