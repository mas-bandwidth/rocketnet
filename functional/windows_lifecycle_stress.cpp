/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Windows lifecycle stress, the Windows form of
    functional/posix_lifecycle_stress.cpp. Every case cycles a real server
    transport and the portable client over real loopback sockets 1000 times
    and counts the process's handles, threads and private bytes:

        handles        GetProcessHandleCount
        threads        a Toolhelp32 walk of this process's threads
                       (process_resources_walk_threads, tests/process_resources.h)
        heap_bytes     HeapSummary cbAllocated on the library's test heap
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
    pass. The run time is read from the performance counter directly, since
    rocketnet_platform_time restarts at rocketnet_init.
    Each case prints the before and after counts, every heap's allocated
    bytes, SDK-owned resource identities and the run time, then the handle
    table line: every handle value the loop added or removed with its object
    type, and the cycles at which the handle count reached a new high, so a
    process-wide delta is named handle by handle.

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
#include <malloc.h>

#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "../tests/process_resources.h"

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
    int heap_count;
    HANDLE heap_handles[256];
    int64_t heap_used_bytes[256];
};

static HANDLE windows_library_heap = NULL;

static void * windows_library_malloc( void * context, size_t bytes )
{
    // Object creation can pass NULL context even while the allocator is
    // installed. All allocations in this case still belong to its one heap.
    (void) context;
    return HeapAlloc( windows_library_heap, 0, bytes );
}

static void windows_library_free( void * context, void * p )
{
    (void) context;
    windows_lifecycle_check( HeapFree( windows_library_heap, 0, p ) != 0 );
}

static void windows_library_heap_begin()
{
    windows_lifecycle_check( rocketnet_term() == ROCKETNET_STATUS_OK );
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
            library_bytes = counts->heap_used_bytes[i];
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
    counts.private_bytes = -1;
    counts.heap_count = 0;

    // the handle count first: the thread walk opens and closes one handle
    DWORD handles = 0;
    if ( GetProcessHandleCount( GetCurrentProcess(), &handles ) )
        counts.handles = (int64_t) handles;

    counts.threads = windows_threads();
    counts.heap_bytes = windows_heap_bytes( &counts );

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
    return after->threads <= before->threads &&
           after->heap_bytes <= before->heap_bytes + windows_memory_allowance_bytes( seconds ) &&
           after->private_bytes < before->private_bytes + windows_private_allowance_bytes();
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
        if ( current.handles == previous.handles && current.threads == previous.threads && current.heap_bytes == previous.heap_bytes && current.private_bytes == previous.private_bytes )
            return current;
        previous = current;
    }
    return previous;
}

// ---------------------------------------------------------------
// the handle table
// ---------------------------------------------------------------

// The process handle count says how many handles changed, not whose they
// are. The table names each one: every handle value the process holds,
// found by GetHandleInformation over the value range (which opens nothing),
// with its object type from NtQueryObject. A socket is told apart from other
// File objects by getsockopt SO_TYPE, a thread by GetThreadId and whether it
// still runs. The case prints the handles the loop added and removed, so a
// process-wide delta comes with the type and value of every handle in it.

#define WINDOWS_HANDLE_TABLE_ENTRIES 8192
#define WINDOWS_HANDLE_VALUE_LIMIT 0x40000

struct windows_handle_entry_t
{
    uint32_t value;
    wchar_t type[32];
};

struct windows_handle_table_t
{
    int count;
    bool overflow;
    windows_handle_entry_t entries[WINDOWS_HANDLE_TABLE_ENTRIES];
};

typedef LONG ( WINAPI * windows_query_object_t )( HANDLE, int, void *, ULONG, ULONG * );

// the counted UTF-16 string NtQueryObject writes at the start of its buffer
struct windows_counted_string_t
{
    USHORT length_bytes;
    USHORT maximum_length_bytes;
    wchar_t * buffer;
};


// RUN-ONLY DIAGNOSTIC: kernel handle tracing, the open stack of every added handle
#pragma warning( push )
#pragma warning( disable : 4091 4201 4365 4668 5039 )
#include <dbghelp.h>
#pragma warning( pop )
struct diag_trace_entry_t { HANDLE handle; HANDLE process_id; HANDLE thread_id; ULONG type; void * stacks[16]; };
struct diag_trace_query_t { HANDLE handle; ULONG total; diag_trace_entry_t entries[1]; };
typedef LONG ( WINAPI * diag_set_info_t )( HANDLE, int, void *, ULONG );
typedef LONG ( WINAPI * diag_query_info_t )( HANDLE, int, void *, ULONG, ULONG * );
typedef BOOL ( WINAPI * diag_sym_init_t )( HANDLE, PCSTR, BOOL );
typedef BOOL ( WINAPI * diag_sym_from_t )( HANDLE, DWORD64, DWORD64 *, SYMBOL_INFO * );
typedef DWORD ( WINAPI * diag_sym_opt_t )( DWORD );
static uint8_t diag_trace_buffer[4 << 20];
static diag_sym_from_t diag_sym_from = NULL;
static void diag_trace_enable()
{
    HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    diag_set_info_t set = (diag_set_info_t) (void *) GetProcAddress( ntdll, "NtSetInformationProcess" );
    struct { ULONG flags; ULONG slots; } ex = { 0, 262144 };
    LONG status = set( GetCurrentProcess(), 32, &ex, sizeof( ex ) );
    printf( "DIAG handle tracing enable_ex status 0x%lx\n", (unsigned long) status );
    if ( status < 0 )
    {
        ULONG flags = 0;
        status = set( GetCurrentProcess(), 32, &flags, sizeof( flags ) );
        printf( "DIAG handle tracing enable status 0x%lx\n", (unsigned long) status );
    }
    if ( !diag_sym_from )
    {
        HMODULE dbghelp = LoadLibraryW( L"dbghelp.dll" );
        diag_sym_opt_t opt = (diag_sym_opt_t) (void *) GetProcAddress( dbghelp, "SymSetOptions" );
        diag_sym_init_t init = (diag_sym_init_t) (void *) GetProcAddress( dbghelp, "SymInitialize" );
        diag_sym_from = (diag_sym_from_t) (void *) GetProcAddress( dbghelp, "SymFromAddr" );
        opt( SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS );
        init( GetCurrentProcess(), NULL, TRUE );
    }
}
static void diag_frame( void * address )
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH] = L"?";
    if ( GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) address, &module ) )
        GetModuleFileNameW( module, path, MAX_PATH );
    const wchar_t * base = wcsrchr( path, L'\\' );
    base = base ? base + 1 : path;
    union { SYMBOL_INFO info; uint8_t bytes[sizeof( SYMBOL_INFO ) + 256]; } symbol;
    memset( &symbol, 0, sizeof( symbol ) );
    symbol.info.SizeOfStruct = sizeof( SYMBOL_INFO );
    symbol.info.MaxNameLen = 255;
    DWORD64 displacement = 0;
    if ( diag_sym_from && diag_sym_from( GetCurrentProcess(), (DWORD64) (uintptr_t) address, &displacement, &symbol.info ) )
        printf( "    %ls!%s+0x%llx\n", base, symbol.info.Name, (unsigned long long) displacement );
    else
        printf( "    %ls+0x%llx\n", base, (unsigned long long) ( (uintptr_t) address - (uintptr_t) module ) );
}
static void diag_trace_print( uint32_t value )
{
    HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    diag_query_info_t query = (diag_query_info_t) (void *) GetProcAddress( ntdll, "NtQueryInformationProcess" );
    diag_trace_query_t * q = (diag_trace_query_t *) diag_trace_buffer;
    memset( q, 0, sizeof( diag_trace_query_t ) );
    q->handle = (HANDLE) (uintptr_t) value;
    ULONG returned = 0;
    const LONG status = query( GetCurrentProcess(), 32, q, (ULONG) sizeof( diag_trace_buffer ), &returned );
    if ( status < 0 ) { printf( "  DIAG trace 0x%x query status 0x%lx\n", value, (unsigned long) status ); return; }
    // newest first: print the newest open of this value, and say whether a close follows it
    for ( ULONG i = 0; i < q->total; i++ )
    {
        if ( q->entries[i].type != 1 ) continue;
        printf( "  DIAG 0x%x opened by thread %llu (%lu traces of this value, newest entry type %lu):\n", value,
                (unsigned long long) (uintptr_t) q->entries[i].thread_id, (unsigned long) q->total, (unsigned long) q->entries[0].type );
        for ( int f = 0; f < 16 && q->entries[i].stacks[f]; f++ ) diag_frame( q->entries[i].stacks[f] );
        return;
    }
    printf( "  DIAG 0x%x: no open trace (%lu traces)\n", value, (unsigned long) q->total );
}

static windows_handle_table_t windows_handles_before;
static windows_handle_table_t windows_handles_after;

static void windows_handle_type( windows_query_object_t query, HANDLE handle, wchar_t * type, size_t type_chars )
{
    // ObjectTypeInformation (2): a UNICODE_STRING type name, then the type's figures
    union
    {
        windows_counted_string_t name;
        uint8_t bytes[1024];
    } information;
    ULONG returned_bytes = 0;
    wcscpy_s( type, type_chars, L"?" );
    if ( !query || query( handle, 2, &information, (ULONG) sizeof( information ), &returned_bytes ) < 0 )
        return;
    const size_t chars = information.name.length_bytes / sizeof( wchar_t );
    if ( information.name.buffer && chars > 0 )
        wcsncpy_s( type, type_chars, information.name.buffer, chars < type_chars - 1 ? chars : type_chars - 1 );
}

static void windows_handle_table_read( windows_handle_table_t * table )
{
    HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    const windows_query_object_t query = ntdll ? (windows_query_object_t) (void *) GetProcAddress( ntdll, "NtQueryObject" ) : NULL;
    table->count = 0;
    table->overflow = false;
    for ( uint32_t value = 4; value < WINDOWS_HANDLE_VALUE_LIMIT; value += 4 )
    {
        const HANDLE handle = (HANDLE) (uintptr_t) value;
        DWORD flags = 0;
        if ( !GetHandleInformation( handle, &flags ) )
            continue;
        if ( table->count == WINDOWS_HANDLE_TABLE_ENTRIES )
        {
            table->overflow = true;
            return;
        }
        windows_handle_entry_t * entry = &table->entries[table->count++];
        entry->value = value;
        windows_handle_type( query, handle, entry->type, sizeof( entry->type ) / sizeof( entry->type[0] ) );
    }
}

static bool windows_handle_table_has( const windows_handle_table_t * table, const windows_handle_entry_t * entry )
{
    for ( int i = 0; i < table->count; i++ )
    {
        if ( table->entries[i].value == entry->value && wcscmp( table->entries[i].type, entry->type ) == 0 )
            return true;
    }
    return false;
}

// what a handle the loop added still refers to, read while it is held
static void windows_handle_describe( const windows_handle_entry_t * entry )
{
    const HANDLE handle = (HANDLE) (uintptr_t) entry->value;
    printf( " 0x%x:%ls", entry->value, entry->type );
    if ( wcscmp( entry->type, L"File" ) == 0 )
    {
        int socket_type = 0;
        int option_bytes = (int) sizeof( socket_type );
        if ( getsockopt( (SOCKET) (uintptr_t) entry->value, SOL_SOCKET, SO_TYPE, (char*) &socket_type, &option_bytes ) == 0 )
        {
            struct sockaddr_storage address;
            int address_bytes = (int) sizeof( address );
            memset( &address, 0, sizeof( address ) );
            unsigned port = 0;
            if ( getsockname( (SOCKET) (uintptr_t) entry->value, (struct sockaddr*) &address, &address_bytes ) == 0 )
                port = address.ss_family == AF_INET6 ? ntohs( ( (struct sockaddr_in6*) &address )->sin6_port )
                                                     : ntohs( ( (struct sockaddr_in*) &address )->sin_port );
            printf( "(socket type %d port %u)", socket_type, port );
        }
        else
            printf( "(file type %lu)", (unsigned long) GetFileType( handle ) );
    }
    else if ( wcscmp( entry->type, L"Thread" ) == 0 )
    {
        DWORD exit_code = 0;
        const bool running = GetExitCodeThread( handle, &exit_code ) && exit_code == STILL_ACTIVE;
        printf( "(thread %lu %s)", (unsigned long) GetThreadId( handle ), running ? "running" : "exited" );
    }
}

// one line: every handle value the loop added and every one it removed, by type
static void windows_handle_table_print( const char * case_name, const windows_handle_table_t * before, const windows_handle_table_t * after )
{
    printf( "%s handle table: %d -> %d%s, added", case_name, before->count, after->count,
            before->overflow || after->overflow ? " (table full)" : "" );
    int added = 0;
    for ( int i = 0; i < after->count; i++ )
    {
        if ( windows_handle_table_has( before, &after->entries[i] ) )
            continue;
        windows_handle_describe( &after->entries[i] );
        added++;
        printf( "\n" );
        diag_trace_print( after->entries[i].value );
    }
    if ( added == 0 )
        printf( " none" );
    printf( ", removed" );
    int removed = 0;
    for ( int i = 0; i < before->count; i++ )
    {
        if ( windows_handle_table_has( after, &before->entries[i] ) )
            continue;
        printf( " 0x%x:%ls", before->entries[i].value, before->entries[i].type );
        removed++;
    }
    if ( removed == 0 )
        printf( " none" );
    printf( "\n" );
    fflush( stdout );
}

// the cycles at which the process handle count first reached each new high
// over the before count, at most eight, printed with the case's result
static int64_t windows_handle_high = 0;
static int windows_handle_high_count = 0;
static int windows_handle_high_cycle[8];
static int64_t windows_handle_high_value[8];

static void windows_handle_high_begin( int64_t handles )
{
    windows_handle_high = handles;
    windows_handle_high_count = 0;
}

static void windows_handle_high_note( int cycle )
{
    DWORD handles = 0;
    if ( !GetProcessHandleCount( GetCurrentProcess(), &handles ) || (int64_t) handles <= windows_handle_high )
        return;
    windows_handle_high = (int64_t) handles;
    if ( windows_handle_high_count < 8 )
    {
        windows_handle_high_cycle[windows_handle_high_count] = cycle;
        windows_handle_high_value[windows_handle_high_count] = (int64_t) handles;
        windows_handle_high_count++;
    }
}

static void windows_handle_high_print( const char * case_name )
{
    printf( "%s handle count highs:", case_name );
    if ( windows_handle_high_count == 0 )
        printf( " none" );
    for ( int i = 0; i < windows_handle_high_count; i++ )
        printf( " %lld at cycle %d", (long long) windows_handle_high_value[i], windows_handle_high_cycle[i] );
    printf( "\n" );
    fflush( stdout );
}

// the before table, read with the before counts
static void windows_handle_before( const windows_resource_counts_t * before )
{
    diag_trace_enable();
    windows_handle_table_read( &windows_handles_before );
    windows_handle_high_begin( before->handles );
}

// the after table, read with the after counts and printed at once, while
// every handle it names is still held
static void windows_handle_after( const char * case_name )
{
    windows_handle_table_read( &windows_handles_after );
    windows_handle_table_print( case_name, &windows_handles_before, &windows_handles_after );
    windows_handle_high_print( case_name );
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
    const uint64_t create_timestamp = rocketnet_functional_token_timestamp( server_config.game_clock_epoch, 1000.0 );
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
static bool windows_case_result( const char * case_name, const windows_cell_t * cell, const windows_resource_counts_t * before,
                                 const windows_resource_counts_t * after, double seconds )
{
    const bool flat = windows_counts_flat( before, after, seconds );
    printf( "%s [%s]: handles %lld -> %lld, threads %lld -> %lld, library_heap_bytes %lld -> %lld (allowed +%lld), private_bytes %lld -> %lld (allowed under +%lld), %.3fs%s",
            case_name, cell->name, (long long) before->handles, (long long) after->handles, (long long) before->threads,
            (long long) after->threads, (long long) before->heap_bytes, (long long) after->heap_bytes,
            (long long) windows_memory_allowance_bytes( seconds ), (long long) before->private_bytes, (long long) after->private_bytes,
            (long long) windows_private_allowance_bytes(), seconds, flat ? "" : " FAIL resource count moved" );
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
        const char * role = before->heap_handles[i] == library_heap && before->heap_handles[i] == process_heap ? "library/process" :
                            before->heap_handles[i] == library_heap ? "library" :
                            before->heap_handles[i] == process_heap ? "process" : "windows";
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
            printf( ", heap[%p:new] -1 -> %lld", after->heap_handles[j], (long long) after->heap_used_bytes[j] );
    }
    printf( ", process_handle_delta %lld (each handle by type on the handle table line)\n", (long long) ( after->handles - before->handles ) );
    fflush( stdout );
    return flat;
}

static void windows_ledger_read( rocketnet_lifecycle_resources_t * resources )
{
    const bool ok = rocketnet_lifecycle_track_read( resources );
    if ( !ok ) printf( "SDK resource ledger read failed: %u errors\n", resources->errors );
    windows_lifecycle_check( ok );
}

static bool windows_ledger_same_live( const rocketnet_lifecycle_resources_t * expected,
                                      const rocketnet_lifecycle_resources_t * actual )
{
    if ( expected->live_count != actual->live_count ) return false;
    for ( unsigned i = 0; i < expected->live_count; ++i )
    {
        bool found = false;
        for ( unsigned j = 0; j < actual->live_count; ++j )
            if ( expected->live[i].generation == actual->live[j].generation &&
                 expected->live[i].kind == actual->live[j].kind && expected->live[i].raw == actual->live[j].raw )
                found = true;
        if ( !found ) return false;
    }
    return true;
}

static bool windows_ledger_result( const char * case_name, const windows_cell_t * cell,
                                   const rocketnet_lifecycle_resources_t * expected,
                                   const rocketnet_lifecycle_resources_t * actual, const char * phase )
{
    static const char * kinds[ROCKETNET_LIFECYCLE_RESOURCE_KINDS] =
    { "Thread", "Event", "socket", "IOCP", "timer", "RIO buffer ID", "RIO completion queue ID", "RIO request queue ID" };
    const bool same = windows_ledger_same_live( expected, actual ) && actual->errors == 0;
    printf( "%s [%s] SDK identities %s: %u -> %u live%s", case_name, cell->name, phase,
            expected->live_count, actual->live_count, same ? "" : " FAIL" );
    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
        printf( ", %s created +%llu closed +%llu", kinds[kind],
                (unsigned long long) ( actual->created[kind] - expected->created[kind] ),
                (unsigned long long) ( actual->closed[kind] - expected->closed[kind] ) );
    printf( "\n" );
    if ( !same )
    {
        for ( unsigned i = 0; i < actual->live_count; ++i )
            printf( "  SDK live kind=%s raw=0x%llx generation=%llu site=%s\n", kinds[actual->live[i].kind],
                    (unsigned long long) actual->live[i].raw, (unsigned long long) actual->live[i].generation,
                    actual->live[i].site ? actual->live[i].site : "unknown" );
    }
    fflush( stdout );
    return same;
}

// The restart case's live set between cycles. rocketnet_server_stop keeps the
// stopped transport (server->retiring, sdk/rocketnet_api.cpp) until the next
// start or destroy frees it (tla/ServerSession.tla Start, Destroy), so after
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
            expected_kind_count[expected->live[i].kind]++;
    for ( unsigned j = 0; j < actual->live_count; ++j )
    {
        const rocketnet_lifecycle_resource_t * held = &actual->live[j];
        if ( held->kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS )
            actual_kind_count[held->kind]++;
        bool accounted = held->generation > expected->latest_generation;
        for ( unsigned i = 0; i < expected->live_count && !accounted; ++i )
            if ( expected->live[i].generation == held->generation && expected->live[i].kind == held->kind && expected->live[i].raw == held->raw )
                accounted = true;
        if ( !accounted )
            same = false;
    }
    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
        if ( expected_kind_count[kind] != actual_kind_count[kind] )
            same = false;
    static const char * kinds[ROCKETNET_LIFECYCLE_RESOURCE_KINDS] =
    { "Thread", "Event", "socket", "IOCP", "timer", "RIO buffer ID", "RIO completion queue ID", "RIO request queue ID" };
    printf( "%s [%s] SDK identities %s, the last stop's transport held: %u -> %u live%s", case_name, cell->name, phase,
            expected->live_count, actual->live_count, same ? "" : " FAIL" );
    for ( unsigned kind = 0; kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS; ++kind )
        printf( ", %s live %u -> %u created +%llu closed +%llu", kinds[kind], expected_kind_count[kind], actual_kind_count[kind],
                (unsigned long long) ( actual->created[kind] - expected->created[kind] ),
                (unsigned long long) ( actual->closed[kind] - expected->closed[kind] ) );
    printf( "\n" );
    if ( !same )
    {
        for ( unsigned i = 0; i < actual->live_count; ++i )
            printf( "  SDK live kind=%s raw=0x%llx generation=%llu site=%s\n",
                    actual->live[i].kind < ROCKETNET_LIFECYCLE_RESOURCE_KINDS ? kinds[actual->live[i].kind] : "unknown",
                    (unsigned long long) actual->live[i].raw, (unsigned long long) actual->live[i].generation,
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
    const DWORD mode_bytes = GetEnvironmentVariableA( mode_name, mode, (DWORD) sizeof(mode) );
    if ( mode_bytes != 0 )
    {
        windows_lifecycle_check( mode_bytes < (DWORD) sizeof(mode) );
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
            windows_lifecycle_check( rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, raw + i, "overflow fixture" ) != 0 );
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
        startup.cb = (DWORD) sizeof(startup);
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
        printf( "[%s]: connect stalled at cycle %d\n", cell->name, cycle );
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
        rocketnet_platform_sleep( 0.001 );
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
    windows_library_heap_begin();
    rocketnet_platform_sleep( 0.001 );
    rocketnet_lifecycle_resources_t initial_resources;
    windows_ledger_read( &initial_resources );
    windows_init_term_cycle();
    const windows_resource_counts_t before = windows_stable_counts();
    windows_handle_before( &before );
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_init_term_cycle();
        windows_handle_high_note( i );
    }
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_handle_after( "init_term_1000" );
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
    windows_handle_before( &before );
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_server_create_destroy_cycle( cell );
        windows_handle_high_note( i );
    }
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_handle_after( "server_create_destroy_1000" );
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
    windows_handle_before( &before );
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_client_create_destroy_cycle( cell );
        windows_handle_high_note( i );
    }
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_handle_after( "client_create_destroy_1000" );
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
    // The caller's reusable sleep timer belongs to the process thread; warm
    // it before marking the server/client ownership interval.
    rocketnet_platform_sleep( 0.001 );
    rocketnet_lifecycle_resources_t initial_resources;
    windows_ledger_read( &initial_resources );
    rocketnet_server_transport_t * server = windows_server_create( cell, "127.0.0.1:0", 8 );
    char server_address[64];
    windows_server_address( server, server_address, sizeof( server_address ) );
    windows_connect_disconnect_cycle( cell, server, server_address, -1 );
    const windows_resource_counts_t before = windows_stable_counts();
    windows_handle_before( &before );
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_connect_disconnect_cycle( cell, server, server_address, i );
        windows_handle_high_note( i );
    }
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_handle_after( "connect_disconnect_1000" );
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
        printf( "server create [%s] at %s failed: %s\n", cell->name, fixed_address, rocketnet_status_name( status ) );
    windows_lifecycle_check( server != NULL );
    windows_server_restart_cycle( cell, server, fixed_address, fixed_port, -1 );
    const windows_resource_counts_t before = windows_stable_counts();
    windows_handle_before( &before );
    rocketnet_lifecycle_resources_t before_resources;
    windows_ledger_read( &before_resources );
    const double start = windows_run_clock();
    for ( int i = 0; i < WINDOWS_LIFECYCLE_CYCLES; i++ )
    {
        windows_server_restart_cycle( cell, server, fixed_address, fixed_port, i );
        windows_handle_high_note( i );
    }
    const double seconds = windows_run_clock() - start;
    const windows_resource_counts_t after = windows_settled_counts( &before, seconds );
    windows_handle_after( "server_restart_1000" );
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
