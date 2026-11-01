/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Windows lifecycle stress, the Windows form of
    tests/functional/posix_lifecycle_stress.cpp. Every case cycles a real server
    transport and the portable client over real loopback sockets 1000 times
    and counts the process's handles, threads and private bytes:

        handles        GetProcessHandleCount
        threads        a Toolhelp32 walk of this process's threads
                       (process_resources_walk_threads, tests/process_resources.h)
        heap_bytes     HeapSummary cbAllocated on the library's test heap
        library_allocated_bytes HeapSize accounting in its allocator callbacks
        private_bytes  GetProcessMemoryInfo PrivateUsage

    The library's public allocator callbacks use a heap owned by this case.
    That keeps SDK allocations distinct from Winsock and other Windows code
    that can allocate from the process heap. The counts are taken after one
    warm cycle, so every path the case repeats has run once; at the end the
    SDK-owned handle generations must close and the thread count must
    not exceed it (the system thread pool retires idle workers of its own, so
    fewer threads pass; a thread left behind is one more). The case-owned heap's
    bytes in use, the Windows form of the POSIX file's mallinfo2 bytes, may exceed
    the warm count by at most one page for every ten minutes of run, rounded
    up, never less than one page. Private bytes also count the heap's free
    but committed pages, which a heap keeps as it fragments, so they are
    judged per cycle: the growth must stay under one page per cycle, which
    any mapping left behind by a cycle exceeds. Counts below the warm count
    pass. Under ASan, private bytes include sanitizer quarantine and shadow
    metadata, so they remain diagnostic; the allocator callbacks still enforce
    the same flat allowance on SDK-owned live bytes. The unsanitized runs
    retain the private-byte check, including mappings outside the allocator.
    The run time is read from the performance counter directly, since
    rocketnet_platform_time restarts at rocketnet_init.
    Each case prints the before and after counts, every heap's allocated
    bytes, SDK-owned resource identities and the run time. Process-wide
    handle deltas remain unattributed unless ownership is proven separately.

    The cells are the Windows server transports (portable, iocp, rio), each
    with the portable client, and each case is one function per cell:
    rocketnet_test_windows_<case>_<cell>. The portable cells are registered in
    tests/functional/rocketnet_functional_tests.cpp; the iocp and rio cells are
    defined here and registered by a follow-up change.

    A connect waits on the client's send rate (10 sends a second,
    sdk/rocketnet_client_protocol.cpp), so the client runs on its own clock,
    the platform clock plus the send intervals the wait has skipped, and the
    wait skips one interval each time the client's state moves, as
    tests/functional/posix_lifecycle_stress.cpp does. Same host is correctness
    only: the run times are printed, never judged.

    On other platforms the file builds empty.
*/

#include "../native_shutdown.h"
#include "rocketnet_counters.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif // #ifndef NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "../process_resources.h"

// defined in tests/functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

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
    int64_t library_allocated_bytes;
    int64_t private_bytes;
    bool asan;
    int heap_count;
    HANDLE heap_handles[256];
    int64_t heap_used_bytes[256];
};

static HANDLE windows_library_heap = NULL;
// Windows ASan intercepts custom-heap blocks, which HeapSummary does not
// count. Account directly at the SDK allocator boundary in every build.
static std::atomic<int64_t> windows_library_allocated_bytes( 0 );
static std::atomic<int> windows_library_accounting_failed( 0 );

static void * windows_library_malloc( void * context, size_t bytes )
{
    // Object creation can pass NULL context even while the allocator is
    // installed. All allocations in this case still belong to its one heap.

    (void) context;
    void * p = HeapAlloc( windows_library_heap, 0, bytes );
    if ( p )
    {
        const SIZE_T allocated = HeapSize( windows_library_heap, 0, p );
        if ( allocated == (SIZE_T) -1 || allocated > (SIZE_T) INT64_MAX )
            windows_library_accounting_failed.store( 1, std::memory_order_relaxed );
        else
        {
            const int64_t previous = windows_library_allocated_bytes.fetch_add( (int64_t) allocated, std::memory_order_relaxed );
            if ( previous > INT64_MAX - (int64_t) allocated )
                windows_library_accounting_failed.store( 1, std::memory_order_relaxed );
        }
    }
    return p;
}

static void windows_library_free( void * context, void * p )
{
    (void) context;
    const SIZE_T allocated = HeapSize( windows_library_heap, 0, p );
    windows_lifecycle_check( HeapFree( windows_library_heap, 0, p ) != 0 );
    if ( allocated == (SIZE_T) -1 || allocated > (SIZE_T) INT64_MAX )
        windows_library_accounting_failed.store( 1, std::memory_order_relaxed );
    else
    {
        const int64_t previous = windows_library_allocated_bytes.fetch_sub( (int64_t) allocated, std::memory_order_relaxed );
        if ( previous < (int64_t) allocated )
            windows_library_accounting_failed.store( 1, std::memory_order_relaxed );
    }
}

static void windows_library_heap_begin()
{
    windows_lifecycle_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    windows_library_allocated_bytes.store( 0, std::memory_order_relaxed );
    windows_library_accounting_failed.store( 0, std::memory_order_relaxed );
    windows_library_heap = HeapCreate( 0, 0, 0 );
    windows_lifecycle_check( windows_library_heap != NULL );
    windows_lifecycle_check( rocketnet_init( windows_library_heap, windows_library_malloc, windows_library_free ) == ROCKETNET_STATUS_OK );
}

static void windows_library_heap_end()
{
    windows_lifecycle_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    windows_lifecycle_check( HeapDestroy( windows_library_heap ) != 0 );
    windows_library_heap = NULL;
}

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

// RocketNet's default allocator is the CRT malloc/free pair. Keep every
// process heap's figure for diagnosis, but judge retained allocations in the
// case-owned heap only: Windows services can allocate elsewhere beyond cleanup.
static int64_t windows_heap_bytes( windows_resource_counts_t * counts )
{
    HANDLE heaps[256];
    const DWORD heap_count = GetProcessHeaps( 256, heaps );
    if ( heap_count == 0 || heap_count > 256 )
        return -1;

    const HANDLE library_heap = windows_library_heap;

    counts->heap_count = (int) heap_count;

    int64_t library_bytes = -1;

    for ( DWORD i = 0; i < heap_count; i++ )
    {
        HEAP_SUMMARY summary;
        memset( &summary, 0, sizeof( summary ) );
        summary.cb = sizeof( summary );
        if ( !HeapSummary( heaps[i], 0, &summary ) )
            return -1;
        counts->heap_handles[i] = heaps[i];
        counts->heap_used_bytes[i] = (int64_t) summary.cbAllocated;
        if ( heaps[i] == library_heap )
        {
            library_bytes = counts->heap_used_bytes[i];
        }
    }

    return library_bytes;
}

// every thread of this process, the program's and the OS's pool workers alike:
// the walk of tests/process_resources.h (process_resources_walk_threads),
// which fails closed and splits the two; this case counts their sum
static int64_t windows_threads()
{
    int64_t program_threads = -1;
    int64_t os_threads = -1;
    if ( !process_resources_walk_threads( &program_threads, &os_threads, NULL ) )
        return -1;

    return program_threads + os_threads;
}

static windows_resource_counts_t windows_read_counts()
{
    windows_resource_counts_t counts;
    counts.handles = -1;
    counts.threads = -1;
    counts.heap_bytes = -1;
    counts.library_allocated_bytes = windows_library_accounting_failed.load( std::memory_order_relaxed )
                                         ? -1
                                         : windows_library_allocated_bytes.load( std::memory_order_relaxed );
    counts.private_bytes = -1;
    counts.asan = GetModuleHandleA( "clang_rt.asan_dynamic-x86_64.dll" ) != NULL;
    counts.heap_count = 0;

    // the handle count first: the thread walk opens and closes one handle

    DWORD handles = 0;
    if ( GetProcessHandleCount( GetCurrentProcess(), &handles ) )
    {
        counts.handles = (int64_t) handles;
    }

    counts.threads = windows_threads();
    counts.heap_bytes = windows_heap_bytes( &counts );

    PROCESS_MEMORY_COUNTERS_EX memory;
    memset( &memory, 0, sizeof( memory ) );
    memory.cb = sizeof( memory );
    if ( GetProcessMemoryInfo( GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *) &memory, sizeof( memory ) ) )
    {
        counts.private_bytes = (int64_t) memory.PrivateUsage;
    }

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
    {
        periods++;
    }

    if ( periods < 1 )
    {
        periods = 1;
    }

    return periods * windows_page_bytes();
}

// private bytes: under one page per cycle (see the header)
static int64_t windows_private_allowance_bytes()
{
    return (int64_t) WINDOWS_LIFECYCLE_CYCLES * windows_page_bytes();
}

static bool windows_counts_flat( const windows_resource_counts_t * before, const windows_resource_counts_t * after, double seconds )
{
    if ( before->library_allocated_bytes < 0 || after->library_allocated_bytes < 0 ||
         before->handles < 0 || before->threads < 0 || before->heap_bytes < 0 || before->private_bytes < 0 ||
         after->handles < 0 || after->threads < 0 || after->heap_bytes < 0 || after->private_bytes < 0 )
        return false;
    // the process thread count includes the operating system's thread pool
    // workers, which come and go on their own; the library's threads are
    // checked exactly by the SDK identity ledger, so threads are reported here
    // and not compared
    if ( before->asan != after->asan )
        return false;

    const int64_t allowance = windows_memory_allowance_bytes( seconds );
    const bool memory_flat = before->asan || after->private_bytes < before->private_bytes + windows_private_allowance_bytes();
    return after->heap_bytes <= before->heap_bytes + allowance &&
           after->library_allocated_bytes <= before->library_allocated_bytes + allowance && memory_flat;
}

// A transport's threads are joined inside destroy, and a joined thread leaves
// the Toolhelp list once the kernel has run it down. The after counts are
// polled every millisecond until the checked thread and memory counts settle
// or WINDOWS_SETTLE_SECONDS has passed. SDK handle identities are checked by
// the generation ledger below, independent of process-wide count changes.
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
        if ( current.handles == previous.handles && current.threads == previous.threads && current.heap_bytes == previous.heap_bytes && current.private_bytes == previous.private_bytes && current.asan == previous.asan && current.library_allocated_bytes == previous.library_allocated_bytes )
            return current;
        previous = current;
    }

    return previous;
}

// A real retained allocation must fail the resource oracle under both native
// heaps and ASan interception; freeing it restores the original baseline.
void rocketnet_test_windows_lifecycle_memory_accounting()
{
    windows_library_heap_begin();
    const windows_resource_counts_t before = windows_stable_counts();
    void * retained = windows_library_malloc( NULL, 8192 );
    windows_lifecycle_check( retained != NULL );
    const windows_resource_counts_t during = windows_read_counts();
    windows_lifecycle_check( during.library_allocated_bytes >= before.library_allocated_bytes + 8192 );
    windows_lifecycle_check( !windows_counts_flat( &before, &during, 1.0 ) );
    windows_library_free( NULL, retained );
    const windows_resource_counts_t after = windows_settled_counts( &before, 1.0 );
    windows_lifecycle_check( windows_counts_flat( &before, &after, 1.0 ) );

    if ( !before.asan )
    {
        const SIZE_T bytes = (SIZE_T) windows_private_allowance_bytes() + (SIZE_T) windows_page_bytes();
        void * mapping = VirtualAlloc( NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
        windows_lifecycle_check( mapping != NULL );
        const windows_resource_counts_t mapped = windows_read_counts();
        windows_lifecycle_check( !windows_counts_flat( &after, &mapped, 1.0 ) );
        windows_lifecycle_check( VirtualFree( mapping, 0, MEM_RELEASE ) != 0 );
        const windows_resource_counts_t unmapped = windows_settled_counts( &after, 1.0 );
        windows_lifecycle_check( windows_counts_flat( &after, &unmapped, 1.0 ) );
    }
    windows_library_heap_end();
}

// ---------------------------------------------------------------
// the cells
// ---------------------------------------------------------------

struct windows_cell_t
{
    const char * name;
    const char * server_transport;
};

static const windows_cell_t windows_cells[] = {
    { "server portable, client portable", "portable" },
    { "server iocp, client portable", "iocp" },
    { "server rio, client portable", "rio" },
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
    {
        printf( "server create [%s] at %s failed: %s\n", cell->name, address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    }

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
    return windows_client_clock_now( (windows_client_clock_t *) context );
}

// server_public_key NULL: the token is sealed to the development server key,
// which the transport-only servers of these cases hold. A public server
// (rocketnet_server_create) makes its own keypair, and ignores a token sealed
// to any other key (ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY),
// so its client's token is sealed to rocketnet_server_public_key.
static rocketnet_client_transport_t * windows_client_create( const windows_cell_t * cell, const char * server_address, windows_client_clock_t * clock,
                                                             const uint8_t * server_public_key )
{
    clock->skipped.store( 0, std::memory_order_release );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const uint64_t create_timestamp = rocketnet_functional_token_timestamp( server_config.token_epoch_unix_seconds, 1000.0 );

    if ( server_public_key )
    {
        const char * addresses[1] = { server_address };

        windows_lifecycle_check( rocketnet_write_production_connect_token( rocketnet_dev_backend_secret_key, server_public_key, ROCKETNET_DEV_PROTOCOL_ID,
                                                                           addresses, 1, rocketnet_random_uint64(), create_timestamp,
                                                                           create_timestamp + ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS,
                                                                           ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, NULL,
                                                                           token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    }
    else
    {
        windows_lifecycle_check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(), create_timestamp,
                                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    }

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.time_callback = windows_client_clock_callback;

    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * client = rocketnet_client_transport_create_ex( clock, token, &config, windows_client_clock_now( clock ), &status );
    if ( !client )
    {
        printf( "client create [%s] to %s failed: %s\n", cell->name, server_address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    }

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
        {
            printf( "  %s %s = %llu\n", side, rocketnet_counter_name( i ), (unsigned long long) value );
        }
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
static bool windows_case_result( const char * case_name, const windows_cell_t * cell, const windows_resource_counts_t * before,
                                 const windows_resource_counts_t * after, double seconds )
{
    const bool flat = windows_counts_flat( before, after, seconds );

    printf( "%s [%s]: handles %lld -> %lld, threads %lld -> %lld, library_heap_bytes %lld -> %lld (allowed +%lld)",
            case_name, cell->name, (long long) before->handles, (long long) after->handles, (long long) before->threads,
            (long long) after->threads, (long long) before->heap_bytes, (long long) after->heap_bytes,
            (long long) windows_memory_allowance_bytes( seconds ) );
    printf( ", library_allocated_bytes %lld -> %lld (allowed +%lld)",
            (long long) before->library_allocated_bytes, (long long) after->library_allocated_bytes,
            (long long) windows_memory_allowance_bytes( seconds ) );
    if ( before->asan )
        printf( ", private_bytes %lld -> %lld (ASan quarantine/shadow diagnostic)",
                (long long) before->private_bytes, (long long) after->private_bytes );
    else
        printf( ", private_bytes %lld -> %lld (allowed under +%lld)", (long long) before->private_bytes,
                (long long) after->private_bytes, (long long) windows_private_allowance_bytes() );
    printf( ", %.3fs%s", seconds, flat ? "" : " FAIL resource count moved" );
    const HANDLE library_heap = windows_library_heap;
    const HANDLE process_heap = GetProcessHeap();

    for ( int i = 0; i < before->heap_count; i++ )
    {
        int64_t after_bytes = -1;

        for ( int j = 0; j < after->heap_count; j++ )
        {
            if ( after->heap_handles[j] == before->heap_handles[i] )
            {
                after_bytes = after->heap_used_bytes[j];
                break;
            }
        }

        const char * role = before->heap_handles[i] == library_heap && before->heap_handles[i] == process_heap ? "library/process" : before->heap_handles[i] == library_heap ? "library"
                                                                                                                                 : before->heap_handles[i] == process_heap   ? "process"
                                                                                                                                                                             : "windows";
        printf( ", heap[%p:%s] %lld -> %lld", before->heap_handles[i], role,
                (long long) before->heap_used_bytes[i], (long long) after_bytes );
    }

    for ( int j = 0; j < after->heap_count; j++ )
    {
        bool found = false;

        for ( int i = 0; i < before->heap_count; i++ )
        {
            if ( before->heap_handles[i] == after->heap_handles[j] )
            {
                found = true;
                break;
            }
        }

        if ( !found )
        {
            printf( ", heap[%p:new] -1 -> %lld", after->heap_handles[j], (long long) after->heap_used_bytes[j] );
        }
    }

    printf( ", process_handle_delta %lld UNATTRIBUTED\n", (long long) ( after->handles - before->handles ) );
    fflush( stdout );

    return flat;
}

static void windows_ledger_read( rocketnet_lifecycle_resources_t * resources )
{
    const bool ok = rocketnet_lifecycle_track_read( resources );
    if ( !ok )
    {
        printf( "SDK resource ledger read failed: %u errors\n", resources->errors );
    }

    windows_lifecycle_check( ok );
}

static bool windows_ledger_same_live( const rocketnet_lifecycle_resources_t * expected,
                                      const rocketnet_lifecycle_resources_t * actual )
{
    if ( expected->live_count != actual->live_count )
        return false;

    for ( unsigned i = 0; i < expected->live_count; ++i )
    {
        bool found = false;

        for ( unsigned j = 0; j < actual->live_count; ++j )
            if ( expected->live[i].generation == actual->live[j].generation &&
                 expected->live[i].kind == actual->live[j].kind && expected->live[i].raw == actual->live[j].raw )
                found = true;
        if ( !found )
            return false;
    }

    return true;
}

static bool windows_ledger_result( const char * case_name, const windows_cell_t * cell,
                                   const rocketnet_lifecycle_resources_t * expected,
                                   const rocketnet_lifecycle_resources_t * actual, const char * phase )
{
    static const char * kinds[ROCKETNET_LIFECYCLE_RESOURCE_KINDS] = { "Thread", "Event", "socket", "IOCP", "timer", "RIO buffer ID", "RIO completion queue ID", "RIO request queue ID" };
    const bool same = windows_ledger_same_live( expected, actual ) && actual->errors == 0;

    printf( "%s [%s] SDK identities %s: %u -> %u live%s", case_name, cell->name, phase,
            expected->live_count, actual->live_count, same ? "" : " FAIL" );
    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
        printf( ", %s created +%" PRIu64 " closed +%" PRIu64, kinds[kind],
                actual->created[kind] - expected->created[kind],
                actual->closed[kind] - expected->closed[kind] );
    printf( "\n" );

    if ( !same )
    {
        for ( unsigned i = 0; i < actual->live_count; ++i )
            printf( "  SDK live kind=%s raw=0x%" PRIxPTR " generation=%" PRIu64 " site=%s\n", kinds[actual->live[i].kind],
                    actual->live[i].raw, actual->live[i].generation,
                    actual->live[i].site ? actual->live[i].site : "unknown" );
    }

    fflush( stdout );

    return same;
}

// The restart case's live set between cycles. rocketnet_server_stop keeps the
// stopped transport (server->retiring, sdk/rocketnet_api.cpp) until the next
// start or destroy frees it (proofs/tla/ServerSession.tla Start, Destroy), so after
// any cycle the identities the last stop left are live: the same kinds and
// counts as after the warm cycle, each either one that was live then or one
// opened since. A held identity older than the warm cycle's, or any count that
// moved, is one more left behind and fails; destroy then must close every one
// (the "after server destroy" ledger, compared identity by identity).
static bool windows_ledger_held_result( const char * case_name, const windows_cell_t * cell,
                                        const rocketnet_lifecycle_resources_t * expected,
                                        const rocketnet_lifecycle_resources_t * actual, const char * phase )
{
    bool same = actual->errors == 0 && expected->live_count == actual->live_count;
    unsigned expected_kind_count[ROCKETNET_LIFECYCLE_RESOURCE_KINDS] = {};
    unsigned actual_kind_count[ROCKETNET_LIFECYCLE_RESOURCE_KINDS] = {};

    for ( unsigned i = 0; i < expected->live_count; ++i )
        if ( expected->live[i].kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS )
        {
            expected_kind_count[expected->live[i].kind]++;
        }

    for ( unsigned j = 0; j < actual->live_count; ++j )
    {
        const rocketnet_lifecycle_resource_t * held = &actual->live[j];
        if ( held->kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS )
        {
            actual_kind_count[held->kind]++;
        }

        bool accounted = held->generation > expected->latest_generation;
        for ( unsigned i = 0; i < expected->live_count && !accounted; ++i )
            if ( expected->live[i].generation == held->generation && expected->live[i].kind == held->kind && expected->live[i].raw == held->raw )
            {
                accounted = true;
            }

        if ( !accounted )
        {
            same = false;
        }
    }

    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
        if ( expected_kind_count[kind] != actual_kind_count[kind] )
        {
            same = false;
        }

    static const char * kinds[ROCKETNET_LIFECYCLE_RESOURCE_KINDS] = { "Thread", "Event", "socket", "IOCP", "timer", "RIO buffer ID", "RIO completion queue ID", "RIO request queue ID" };

    printf( "%s [%s] SDK identities %s, the last stop's transport held: %u -> %u live%s", case_name, cell->name, phase,
            expected->live_count, actual->live_count, same ? "" : " FAIL" );
    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
        printf( ", %s live %u -> %u created +%" PRIu64 " closed +%" PRIu64, kinds[kind], expected_kind_count[kind], actual_kind_count[kind],
                actual->created[kind] - expected->created[kind],
                actual->closed[kind] - expected->closed[kind] );
    printf( "\n" );

    if ( !same )
    {
        for ( unsigned i = 0; i < actual->live_count; ++i )
            printf( "  SDK live kind=%s raw=0x%" PRIxPTR " generation=%" PRIu64 " site=%s\n",
                    actual->live[i].kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS ? kinds[actual->live[i].kind] : "unknown",
                    actual->live[i].raw, actual->live[i].generation,
                    actual->live[i].site ? actual->live[i].site : "unknown" );
    }

    fflush( stdout );

    return same;
}

// The same functional executable exercises the ledger's fail-closed paths in
// bounded child runs. Synthetic raw values are never passed to Windows APIs.
void rocketnet_test_windows_lifecycle_ledger()
{
    static const char * mode_name = "ROCKETNET_LIFECYCLE_LEDGER_CASE";
    static const uintptr_t raw = (uintptr_t) 0x70000000u;
    char mode[16] = {};
    const DWORD mode_bytes = GetEnvironmentVariableA( mode_name, mode, (DWORD) sizeof( mode ) );
    if ( mode_bytes != 0 )
    {
        windows_lifecycle_check( mode_bytes < (DWORD) sizeof( mode ) );
        rocketnet_lifecycle_track_begin();

        if ( strcmp( mode, "stale" ) == 0 )
        {
            const uint64_t old_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, raw, "stale fixture old" );
            windows_lifecycle_check( old_generation != 0 );
            windows_lifecycle_check( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, raw, old_generation ) );
            rocketnet_lifecycle_track_close_end( old_generation, true );
            windows_lifecycle_check( rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, raw, "stale fixture new" ) != 0 );
            rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, raw, old_generation );
            windows_lifecycle_check( false ); // the stale close must terminate the child first
        }

        windows_lifecycle_check( strcmp( mode, "overflow" ) == 0 );

        for ( unsigned i = 0; i < ROCKETNET_LIFECYCLE_MAX_LIVE; ++i )
        {
            windows_lifecycle_check( rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, raw + i, "overflow fixture" ) != 0 );
        }

        windows_lifecycle_check( rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT,
                                                                 raw + ROCKETNET_LIFECYCLE_MAX_LIVE, "overflow fixture" ) == 0 );
        rocketnet_lifecycle_resources_t full;
        windows_lifecycle_check( !rocketnet_lifecycle_track_read( &full ) && full.errors != 0 &&
                                 full.live_count == ROCKETNET_LIFECYCLE_MAX_LIVE );
        printf( "SDK lifecycle ledger overflow detected at capacity %u\n", (unsigned) ROCKETNET_LIFECYCLE_MAX_LIVE );
        return;
    }

    rocketnet_lifecycle_resources_t before;
    windows_ledger_read( &before );
    rocketnet_lifecycle_track_begin();

    const uint64_t old_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, raw, "reuse fixture old" );
    windows_lifecycle_check( old_generation != 0 );
    windows_lifecycle_check( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, raw, old_generation ) );

    const uint64_t new_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, raw, "reuse fixture new" );
    windows_lifecycle_check( new_generation != 0 && new_generation != old_generation );
    rocketnet_lifecycle_track_close_end( old_generation, true );
    windows_lifecycle_check( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, raw, new_generation ) );
    rocketnet_lifecycle_track_close_end( new_generation, true );
    rocketnet_lifecycle_track_end();

    rocketnet_lifecycle_resources_t after;
    windows_ledger_read( &after );
    windows_lifecycle_check( windows_ledger_same_live( &before, &after ) &&
                             after.created[ROCKETNET_LIFECYCLE_EVENT] == before.created[ROCKETNET_LIFECYCLE_EVENT] + 2 &&
                             after.closed[ROCKETNET_LIFECYCLE_EVENT] == before.closed[ROCKETNET_LIFECYCLE_EVENT] + 2 );

    wchar_t path[MAX_PATH];
    const DWORD path_bytes = GetModuleFileNameW( NULL, path, MAX_PATH );
    windows_lifecycle_check( path_bytes != 0 && path_bytes < MAX_PATH );

    for ( int i = 0; i < 2; ++i )
    {
        const char * child_mode = i == 0 ? "stale" : "overflow";
        wchar_t command[MAX_PATH + 80];
        _snwprintf_s( command, MAX_PATH + 80, _TRUNCATE, L"\"%s\" --run test_windows_lifecycle_ledger", path );
        windows_lifecycle_check( _putenv_s( mode_name, child_mode ) == 0 );

        STARTUPINFOW startup = {};
        startup.cb = (DWORD) sizeof( startup );

        PROCESS_INFORMATION process = {};

        const BOOL created = CreateProcessW( path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                                             NULL, NULL, &startup, &process );
        windows_lifecycle_check( _putenv_s( mode_name, "" ) == 0 );
        windows_lifecycle_check( created != 0 );

        const DWORD waited = WaitForSingleObject( process.hProcess, 15000 );
        if ( waited != WAIT_OBJECT_0 )
        {
            TerminateProcess( process.hProcess, 1 );
            WaitForSingleObject( process.hProcess, 5000 );
        }

        DWORD exit_code = STILL_ACTIVE;
        const BOOL exited = GetExitCodeProcess( process.hProcess, &exit_code );

        CloseHandle( process.hThread );
        CloseHandle( process.hProcess );
        windows_lifecycle_check( waited == WAIT_OBJECT_0 && exited && exit_code == ( i == 0 ? 86u : 0u ) );
    }

    printf( "SDK lifecycle ledger balanced, reused-raw, stale-generation and overflow checks passed\n" );
}

// ---------------------------------------------------------------
// one cycle of each case
// ---------------------------------------------------------------

static void windows_init_term_cycle()
{
    windows_lifecycle_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    windows_lifecycle_check( rocketnet_init( windows_library_heap, windows_library_malloc, windows_library_free ) == ROCKETNET_STATUS_OK );
}

static void windows_server_create_destroy_cycle( const windows_cell_t * cell )
{
    rn_native_server_transport_stop_and_destroy( windows_server_create( cell, "127.0.0.1:0", 4 ) );
}

static void windows_client_create_destroy_cycle( const windows_cell_t * cell )
{
    windows_client_clock_t clock;
    rn_native_client_transport_stop_and_destroy( windows_client_create( cell, "127.0.0.1:40000", &clock, NULL ) );
}

static void windows_connect_disconnect_cycle( const windows_cell_t * cell, rocketnet_server_transport_t * server,
                                              const char * server_address, int cycle )
{
    windows_client_clock_t clock;
    rocketnet_client_transport_t * client = windows_client_create( cell, server_address, &clock, NULL );
    if ( !windows_wait_connected( server, client, &clock, 5.0 ) )
    {
        printf( "[%s]: connect stalled at cycle %d\n", cell->name, cycle );
    }

    windows_lifecycle_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_client_transport_disconnect( client );
    windows_lifecycle_check( windows_wait_server_empty( server, client, &clock, 5.0 ) );
    rn_native_client_transport_stop_and_destroy( client );
}

// Keep one public server object through every stop/start. The transport-only
// object used by the other cases has no restart operation.
static void windows_server_restart_cycle( const windows_cell_t * cell, rocketnet_server_t * server,
                                          const char * fixed_address, unsigned fixed_port, int cycle )
{
    windows_lifecycle_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    windows_lifecycle_check( rocketnet_server_port( server ) == fixed_port );

    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public_key );

    windows_client_clock_t clock;
    rocketnet_client_transport_t * client = windows_client_create( cell, fixed_address, &clock, server_public_key );
    const double deadline = rocketnet_platform_time() + 5.0;
    int previous_state = rocketnet_client_transport_state( client );
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_client_transport_update( client, windows_client_clock_now( &clock ) );
        rocketnet_server_update( server, rocketnet_platform_time() );
        rocketnet_server_send_packets( server );

        const int state = rocketnet_client_transport_state( client );
        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && rocketnet_server_num_connected_clients( server ) == 1 )
            break;
        if ( state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            break;

        if ( state != previous_state )
        {
            previous_state = state;
            clock.skipped.fetch_add( 1, std::memory_order_acq_rel );
            continue;
        }

        rocketnet_platform_sleep( 0.0002 );
    }

    if ( rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ||
         rocketnet_server_num_connected_clients( server ) != 1 )
    {
        printf( "[%s]: reconnect stalled at restart %d, client state %d, server clients %d\n", cell->name, cycle,
                rocketnet_client_transport_state( client ), rocketnet_server_num_connected_clients( server ) );
        windows_print_counters( "client", rocketnet_client_transport_counters( client ) );
        fflush( stdout );
    }

    windows_lifecycle_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                             rocketnet_server_num_connected_clients( server ) == 1 );
    rn_native_client_transport_stop_and_destroy( client );
    rocketnet_server_stop( server );

    const double stop_deadline = rocketnet_platform_time() + 10.0;
    while ( !rocketnet_server_stopped( server ) && rocketnet_platform_time() < stop_deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }

    windows_lifecycle_check( rocketnet_server_stopped( server ) );
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

        const int result = bind( probe, (const struct sockaddr *) &address, (int) sizeof( address ) );

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
    windows_library_heap_begin();
    rocketnet_platform_sleep( 0.001 );

    rocketnet_lifecycle_resources_t initial_resources;
    windows_ledger_read( &initial_resources );
    windows_init_term_cycle();

    const windows_resource_counts_t before = windows_stable_counts();
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );

    const double start = windows_run_clock();

    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_init_term_cycle();
    }

    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    rocketnet_lifecycle_resources_t after_resources;
    windows_ledger_read( &after_resources );

    const bool counts_ok = windows_case_result( "init_term_1000", cell, &before, &after, seconds );
    const bool loop_ok = windows_ledger_result( "init_term_1000", cell, &before_resources, &after_resources, "after loop" );
    const bool full_ok = windows_ledger_result( "init_term_1000", cell, &initial_resources, &after_resources, "after warm and loop" );
    windows_lifecycle_check( counts_ok && loop_ok && full_ok );
    windows_library_heap_end();
}

static void windows_server_create_destroy_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );

    windows_clear_transport_env();
    windows_library_heap_begin();
    rocketnet_platform_sleep( 0.001 );

    rocketnet_lifecycle_resources_t initial_resources;
    windows_ledger_read( &initial_resources );
    windows_server_create_destroy_cycle( cell );

    const windows_resource_counts_t before = windows_stable_counts();
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );

    const double start = windows_run_clock();

    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_server_create_destroy_cycle( cell );
    }

    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    rocketnet_lifecycle_resources_t after_resources;
    windows_ledger_read( &after_resources );

    const bool counts_ok = windows_case_result( "server_create_destroy_1000", cell, &before, &after, seconds );
    const bool loop_ok = windows_ledger_result( "server_create_destroy_1000", cell, &before_resources, &after_resources, "after loop" );
    const bool full_ok = windows_ledger_result( "server_create_destroy_1000", cell, &initial_resources, &after_resources, "after warm and loop" );
    windows_lifecycle_check( counts_ok && loop_ok && full_ok );
    windows_library_heap_end();
}

static void windows_client_create_destroy_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );

    windows_clear_transport_env();
    windows_library_heap_begin();
    rocketnet_platform_sleep( 0.001 );

    rocketnet_lifecycle_resources_t initial_resources;
    windows_ledger_read( &initial_resources );
    windows_client_create_destroy_cycle( cell );

    const windows_resource_counts_t before = windows_stable_counts();
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );

    const double start = windows_run_clock();

    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_client_create_destroy_cycle( cell );
    }

    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    rocketnet_lifecycle_resources_t after_resources;
    windows_ledger_read( &after_resources );

    const bool counts_ok = windows_case_result( "client_create_destroy_1000", cell, &before, &after, seconds );
    const bool loop_ok = windows_ledger_result( "client_create_destroy_1000", cell, &before_resources, &after_resources, "after loop" );
    const bool full_ok = windows_ledger_result( "client_create_destroy_1000", cell, &initial_resources, &after_resources, "after warm and loop" );
    windows_lifecycle_check( counts_ok && loop_ok && full_ok );
    windows_library_heap_end();
}

static void windows_connect_disconnect_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );

    windows_clear_transport_env();
    windows_library_heap_begin();

    // A sleep on this thread creates and closes its own timer, so the
    // server/client ownership interval below holds only what they own.

    rocketnet_platform_sleep( 0.001 );

    rocketnet_lifecycle_resources_t initial_resources;
    windows_ledger_read( &initial_resources );

    rocketnet_server_transport_t * server = windows_server_create( cell, "127.0.0.1:0", 8 );
    char server_address[64];
    windows_server_address( server, server_address, sizeof( server_address ) );
    windows_connect_disconnect_cycle( cell, server, server_address, -1 );

    const windows_resource_counts_t before = windows_stable_counts();
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );

    const double start = windows_run_clock();

    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_connect_disconnect_cycle( cell, server, server_address, i );
    }

    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    rocketnet_lifecycle_resources_t after_resources;
    windows_ledger_read( &after_resources );
    rn_native_server_transport_stop_and_destroy( server );

    rocketnet_lifecycle_resources_t final_resources;
    windows_ledger_read( &final_resources );

    const bool counts_ok = windows_case_result( "connect_disconnect_1000", cell, &before, &after, seconds );
    const bool loop_ok = windows_ledger_result( "connect_disconnect_1000", cell, &before_resources, &after_resources, "after loop" );
    const bool destroy_ok = windows_ledger_result( "connect_disconnect_1000", cell, &initial_resources, &final_resources, "after server destroy" );
    windows_lifecycle_check( counts_ok && loop_ok && destroy_ok );
    windows_library_heap_end();
}

static void windows_server_restart_1000( const char * server_transport )
{
    const windows_cell_t * cell = windows_cell( server_transport );

    windows_clear_transport_env();
    windows_library_heap_begin();
    rocketnet_platform_sleep( 0.001 );

    rocketnet_lifecycle_resources_t initial_resources;
    windows_ledger_read( &initial_resources );

    const unsigned fixed_port = windows_reserved_restart_port( cell );
    windows_lifecycle_check( fixed_port != 0 );

    char fixed_address[64];
    snprintf( fixed_address, sizeof( fixed_address ), "127.0.0.1:%u", fixed_port );

    struct rocketnet_server_config_t config;
    windows_lifecycle_check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config.backend_public_key, rocketnet_dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = 4;
    config.transport = cell->server_transport;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, fixed_address, NULL, &config, &status );
    if ( !server )
    {
        printf( "server create [%s] at %s failed: %s\n", cell->name, fixed_address, rocketnet_status_name( status ) );
    }

    windows_lifecycle_check( server != NULL );
    windows_server_restart_cycle( cell, server, fixed_address, fixed_port, -1 );

    const windows_resource_counts_t before = windows_stable_counts();
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );

    const double start = windows_run_clock();

    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_server_restart_cycle( cell, server, fixed_address, fixed_port, i );
    }

    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    rocketnet_lifecycle_resources_t after_resources;
    windows_ledger_read( &after_resources );
    rocketnet_server_destroy( server );

    rocketnet_lifecycle_resources_t final_resources;
    windows_ledger_read( &final_resources );

    const bool counts_ok = windows_case_result( "server_restart_1000", cell, &before, &after, seconds );
    const bool loop_ok = windows_ledger_held_result( "server_restart_1000", cell, &before_resources, &after_resources, "after loop" );
    const bool destroy_ok = windows_ledger_result( "server_restart_1000", cell, &initial_resources, &final_resources, "after server destroy" );
    windows_lifecycle_check( counts_ok && loop_ok && destroy_ok );
    windows_library_heap_end();
}

// one function per case and cell, rocketnet_test_windows_<case>_<cell>
#define WINDOWS_LIFECYCLE_CELL_CASE( case_name, transport )                                 \
void rocketnet_test_windows_##case_name##_##transport();                                    \
void rocketnet_test_windows_##case_name##_##transport()                                     \
{                                                                                           \
    rocketnet_lifecycle_track_begin();                                                      \
    windows_##case_name( #transport );                                                      \
    rocketnet_lifecycle_track_end();                                                        \
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
