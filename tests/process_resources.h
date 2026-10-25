/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    process_resources.h: the process's own resource counts, read the same way
    on every run so a soak can say whether they stayed flat.

        Linux    fds             the entries of /proc/self/fd
                 threads         the entries of /proc/self/task
                 resident_bytes  VmRSS from /proc/self/status
        macOS    fds             proc_pidinfo PROC_PIDLISTFDS
                 threads         proc_pidinfo PROC_PIDTASKINFO pti_threadnum
                 resident_bytes  proc_pidinfo PROC_PIDTASKINFO pti_resident_size
        Windows  handles         GetProcessHandleCount
                 threads         a Toolhelp32 walk of this process's threads,
                                 less those proven the OS's (os_threads)
                 private_bytes   GetProcessMemoryInfo PrivateUsage, the whole
                                 process's commit charge: heap, VirtualAlloc
                                 regions (RIO buffers) and thread stacks alike
                 os_threads      the same walk's threads whose start address
                                 is inside ntdll.dll (process_resources_thread_is_os)

    Every Windows thread is in exactly one of threads and os_threads, so the
    two together are the process's full thread count, and the walk fails
    closed: a list that cannot be opened, a first entry that cannot be read,
    or a walk that ends on any error but ERROR_NO_MORE_FILES reads both as -1.

    The Linux and macOS counters are the ones functional/posix_lifecycle_stress.cpp
    reads for its cells; the Windows counters are the ones load.c reads.
    A count that cannot be read is -1, and process_resources_read returns 0.

    THE WARM-UP (process_resources_touch_all). Memory allocated at create is
    resident only once a page of it is first written, and a light run writes
    a preallocated buffer only as deep as its deepest burst so far: a server
    sized for 1000 clients and soaked by one reaches a new depth of its
    receive buffer (rocketnet_server_transport.cpp's construct skips its
    49 MB of buffers so create stays fast) every few minutes under
    impairment, for as long as the run lasts. That is first touch, not a
    leak, and it never levels off in a 120 s or a 30-minute run. So on Linux
    the post-warmup sample is taken after process_resources_touch_all has
    faulted in every page of every private mapping once (MADV_POPULATE_WRITE
    on the writable ones, MADV_POPULATE_READ on the rest; contents are not
    changed): from then on resident memory grows only when the process maps
    or touches memory it did not hold at the post-warmup sample. The
    allocator must not hand populated pages back and fault them in again: so
    process_resources_keep_freed_pages, called before anything is created,
    stops glibc trimming its heaps (M_TRIM_THRESHOLD at its maximum; free()
    otherwise returns the heap top to the kernel and the next malloc faults it
    back in, a drop of megabytes then a regrowth that is neither a leak nor
    flat). On Windows
    the memory count is the commit charge (PrivateUsage), which counts
    preallocated memory at its allocation, touched or not; macOS has no
    populate call and its count is read as it is.

    THE FLAT RULE (process_resources_flat). Taken after a warm cycle, so every
    path the run repeats has run once, and after the warm-up above: the fd or
    handle count at the end must not exceed its post-warmup count (a fall is
    not a leak: on Windows the handles of the OS's pool workers close as they
    retire), the thread count at the end must equal its post-warmup count exactly (on Windows
    os_threads may fall, as the OS retires its pool workers on its own timers,
    and never rise, so the full thread count never grows), and the
    memory count at the end may exceed its post-warmup count by at most one
    page for every ten minutes of run since the post-warmup sample, rounded
    up, and never less than one page. A memory count below its post-warmup
    count is not a leak and passes.

    A plain C header: soak.c (C) and the C++ suites can both include it. Every
    function is static inline, so a translation unit that uses only some of
    them warns about none.
*/

#ifndef ROCKETNET_TESTS_PROCESS_RESOURCES_H
#define ROCKETNET_TESTS_PROCESS_RESOURCES_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#else
#include <dirent.h>
#include <errno.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#endif

#define PROCESS_RESOURCES_HANDLES 0
#define PROCESS_RESOURCES_THREADS 1
#define PROCESS_RESOURCES_MEMORY 2

#if defined(_WIN32)
#define PROCESS_RESOURCES_OS_THREADS 3
#define PROCESS_RESOURCES_COUNT 4
#else
#define PROCESS_RESOURCES_COUNT 3
#endif

struct process_resources_t
{
    int64_t values[PROCESS_RESOURCES_COUNT];
};

// the name each count prints under: fds, threads, resident_bytes on Linux and
// macOS; handles, threads, private_bytes on Windows

static inline const char * process_resources_name( int index )
{
    switch ( index )
    {
#if defined(_WIN32)
        case PROCESS_RESOURCES_HANDLES: return "handles";
        case PROCESS_RESOURCES_THREADS: return "threads";
        case PROCESS_RESOURCES_MEMORY:  return "private_bytes";
        case PROCESS_RESOURCES_OS_THREADS: return "os_threads";
#else
        case PROCESS_RESOURCES_HANDLES: return "fds";
        case PROCESS_RESOURCES_THREADS: return "threads";
        case PROCESS_RESOURCES_MEMORY:  return "resident_bytes";
#endif
        default:                        return "unknown";
    }
}

static inline int64_t process_resources_page_bytes( void )
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo( &info );
    return (int64_t) info.dwPageSize;
#else
    const long page = sysconf( _SC_PAGESIZE );
    return page > 0 ? (int64_t) page : 4096;
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)

static inline int64_t process_resources_count_directory( const char * path )
{
    DIR * directory = opendir( path );
    if ( !directory )
        return -1;
    int64_t count = 0;
    struct dirent * entry = NULL;
    while ( ( entry = readdir( directory ) ) != NULL )
    {
        if ( strcmp( entry->d_name, "." ) == 0 || strcmp( entry->d_name, ".." ) == 0 )
            continue;
        count++;
    }
    closedir( directory );
    return count;
}

static inline int64_t process_resources_resident_bytes( void )
{
    FILE * file = fopen( "/proc/self/status", "r" );
    if ( !file )
        return -1;
    char line[256];
    int64_t bytes = -1;
    while ( fgets( line, sizeof( line ), file ) )
    {
        if ( strncmp( line, "VmRSS:", 6 ) == 0 )
        {
            bytes = (int64_t) strtoll( line + 6, NULL, 10 ) * 1024;
            break;
        }
    }
    fclose( file );
    return bytes;
}

#endif

#if defined(_WIN32)
// Windows only: a thread's start address (ThreadQuerySetWin32StartAddress),
// and whether it is inside ntdll.dll. A thread starting in ntdll is the OS's
// own: the thread pool's workers and the parallel loader's (TppWorkerThread),
// which the OS starts and retires on its own timers. Every thread the library
// creates starts in the library's own code (rocketnet_platform.cpp passes its
// thread_function_shim to CreateThread), never in ntdll. Returns 1 for a
// thread proven the OS's, and 0 for every other thread, including one whose
// start address cannot be read: only a proven OS thread is left out of the
// threads count.

typedef LONG (WINAPI * process_resources_query_thread_t)( HANDLE thread, int information_class, void * information, ULONG information_bytes, ULONG * returned_bytes );

static inline int process_resources_thread_is_os( DWORD thread_id, uintptr_t * start_offset )
{
    *start_offset = 0;
    HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    if ( !ntdll )
        return 0;
    process_resources_query_thread_t query = (process_resources_query_thread_t) (void *) GetProcAddress( ntdll, "NtQueryInformationThread" );
    if ( !query )
        return 0;
    HANDLE thread = OpenThread( THREAD_QUERY_INFORMATION, FALSE, thread_id );
    if ( !thread )
        return 0;
    void * start_address = NULL;
    const int query_set_win32_start_address = 9; // ThreadQuerySetWin32StartAddress
    const LONG status = query( thread, query_set_win32_start_address, &start_address, (ULONG) sizeof( start_address ), NULL );
    CloseHandle( thread );
    if ( status < 0 || !start_address )
        return 0;
    HMODULE start_module = NULL;
    if ( !GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) start_address, &start_module ) )
        return 0;
    if ( start_module != ntdll )
        return 0;
    *start_offset = (uintptr_t) start_address - (uintptr_t) ntdll;
    return 1;
}

// Windows only: one Toolhelp32 walk of this process's threads into *threads
// (the rest) and *os_threads (proven the OS's). When print_label is not NULL
// each OS thread prints as "soak resources: <label> os_thread id=<n>
// start=ntdll+0x<offset>", so every thread left out of threads is named with
// its start address. Fails closed: returns 0, with both counts -1, when the
// list cannot be opened, its first entry cannot be read, or the walk ends on
// an error other than ERROR_NO_MORE_FILES.

static inline int process_resources_walk_threads( int64_t * threads, int64_t * os_threads, const char * print_label )
{
    *threads = -1;
    *os_threads = -1;
    HANDLE thread_list = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( thread_list == INVALID_HANDLE_VALUE )
        return 0;
    THREADENTRY32 entry;
    memset( &entry, 0, sizeof( entry ) );
    entry.dwSize = sizeof( entry );
    const DWORD self_pid = GetCurrentProcessId();
    int64_t program_count = 0;
    int64_t os_count = 0;
    int ok = 0;
    if ( Thread32First( thread_list, &entry ) )
    {
        for ( ;; )
        {
            if ( entry.th32OwnerProcessID == self_pid )
            {
                uintptr_t start_offset = 0;
                if ( process_resources_thread_is_os( entry.th32ThreadID, &start_offset ) )
                {
                    os_count++;
                    if ( print_label )
                        printf( "soak resources: %s os_thread id=%lu start=ntdll+0x%llx\n", print_label, (unsigned long) entry.th32ThreadID, (unsigned long long) start_offset );
                }
                else
                {
                    program_count++;
                }
            }
            entry.dwSize = sizeof( entry );
            if ( !Thread32Next( thread_list, &entry ) )
            {
                ok = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
        }
    }
    CloseHandle( thread_list );
    if ( !ok )
        return 0;
    *threads = program_count;
    *os_threads = os_count;
    return 1;
}
#endif

// reads every count into *resources; returns 1 when every count was read

static inline int process_resources_read( struct process_resources_t * resources )
{
    for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
        resources->values[i] = -1;

#if defined(_WIN32)

    DWORD handles = 0;
    if ( GetProcessHandleCount( GetCurrentProcess(), &handles ) )
        resources->values[PROCESS_RESOURCES_HANDLES] = (int64_t) handles;

    process_resources_walk_threads( &resources->values[PROCESS_RESOURCES_THREADS], &resources->values[PROCESS_RESOURCES_OS_THREADS], NULL );

    PROCESS_MEMORY_COUNTERS_EX memory;
    memset( &memory, 0, sizeof( memory ) );
    memory.cb = sizeof( memory );
    if ( GetProcessMemoryInfo( GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *) &memory, sizeof( memory ) ) )
        resources->values[PROCESS_RESOURCES_MEMORY] = (int64_t) memory.PrivateUsage;

#elif defined(__APPLE__)

    const int fd_bytes = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, NULL, 0 );
    if ( fd_bytes > 0 )
    {
        struct proc_fdinfo * fds = (struct proc_fdinfo *) malloc( (size_t) fd_bytes );
        if ( fds )
        {
            const int used = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, fds, fd_bytes );
            if ( used >= 0 )
                resources->values[PROCESS_RESOURCES_HANDLES] = (int64_t) ( used / (int) PROC_PIDLISTFD_SIZE );
            free( fds );
        }
    }

    struct proc_taskinfo info;
    if ( proc_pidinfo( getpid(), PROC_PIDTASKINFO, 0, &info, (int) sizeof( info ) ) == (int) sizeof( info ) )
    {
        resources->values[PROCESS_RESOURCES_THREADS] = (int64_t) info.pti_threadnum;
        resources->values[PROCESS_RESOURCES_MEMORY] = (int64_t) info.pti_resident_size;
    }

#else

    // the directory stream each count opens is open while it counts, the
    // same one fd in every sample

    resources->values[PROCESS_RESOURCES_HANDLES] = process_resources_count_directory( "/proc/self/fd" );
    resources->values[PROCESS_RESOURCES_THREADS] = process_resources_count_directory( "/proc/self/task" );
    resources->values[PROCESS_RESOURCES_MEMORY] = process_resources_resident_bytes();

#endif

    for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
    {
        if ( resources->values[i] < 0 )
            return 0;
    }
    return 1;
}

// raises each count of *peak to the matching count of *sample when higher

static inline void process_resources_peak( struct process_resources_t * peak, const struct process_resources_t * sample )
{
    for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
    {
        if ( sample->values[i] > peak->values[i] )
            peak->values[i] = sample->values[i];
    }
}

// the memory growth THE FLAT RULE allows over seconds_since_start of run

static inline int64_t process_resources_memory_allowance_bytes( double seconds_since_start )
{
    int64_t periods = seconds_since_start > 0.0 ? (int64_t) ( seconds_since_start / 600.0 ) : 0;
    if ( (double) periods * 600.0 < seconds_since_start )
        periods++;
    if ( periods < 1 )
        periods = 1;
    return periods * process_resources_page_bytes();
}

// THE FLAT RULE for one count: 1 when the end count is flat against the
// post-warmup count, 0 when it is not (or either count is unread)

static inline int process_resources_flat( int index, int64_t start, int64_t end, double seconds_since_start )
{
    if ( start < 0 || end < 0 )
        return 0;
    if ( index == PROCESS_RESOURCES_MEMORY )
        return end <= start + process_resources_memory_allowance_bytes( seconds_since_start );
    // the handle (fd) count fails on growth only: a fall is not a leak (on
    // Windows the handles of the OS's retiring pool workers close with them)
    if ( index == PROCESS_RESOURCES_HANDLES )
        return end <= start;
#if defined(_WIN32)
    if ( index == PROCESS_RESOURCES_OS_THREADS )
        return end <= start;
#endif
    return end == start;
}

// THE WARM-UP: faults in every page of every private mapping once, so memory
// preallocated at create and not yet reached is resident before the
// post-warmup sample. Returns the bytes of the mappings populated, or -1 where
// the platform (or the kernel: MADV_POPULATE_* is Linux 5.14) has no populate
// call. Shared mappings (io_uring and XDP rings) and the kernel's [vvar],
// [vdso] and [vsyscall] are left alone; a mapping the kernel refuses (no
// read permission, for one) is skipped.

static inline int64_t process_resources_touch_all( void )
{
#if !defined(_WIN32) && !defined(__APPLE__) && defined(MADV_POPULATE_WRITE) && defined(MADV_POPULATE_READ)
    FILE * file = fopen( "/proc/self/maps", "r" );
    if ( !file )
        return -1;
    char line[512];
    int64_t populated = 0;
    int unsupported = 0;
    while ( fgets( line, sizeof( line ), file ) )
    {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char perms[8] = { 0 };
        if ( sscanf( line, "%llx-%llx %7s", &start, &end, perms ) != 3 )
            continue;
        if ( perms[0] != 'r' || perms[3] != 'p' )
            continue;
        if ( strstr( line, "[vvar" ) || strstr( line, "[vdso]" ) || strstr( line, "[vsyscall]" ) )
            continue;
        const int advice = perms[1] == 'w' ? MADV_POPULATE_WRITE : MADV_POPULATE_READ;
        if ( madvise( (void *) (uintptr_t) start, (size_t) ( end - start ), advice ) == 0 )
            populated += (int64_t) ( end - start );
        else if ( errno == EINVAL )
            unsupported = 1;
    }
    fclose( file );
    return unsupported && populated == 0 ? -1 : populated;
#else
    return -1;
#endif
}

// THE WARM-UP, its first half: glibc keeps every freed heap page instead of
// trimming the heap top, so the pages process_resources_touch_all populates
// stay resident (see THE WARM-UP). Call once, before anything is created.
// Returns 1 where it applies and took effect.

static inline int process_resources_keep_freed_pages( void )
{
#if defined(__GLIBC__) && !defined(_WIN32) && !defined(__APPLE__)
    return mallopt( M_TRIM_THRESHOLD, 0x7fffffff ) == 1;
#else
    return 0;
#endif
}

// Linux only: Rss and Private_Dirty from /proc/self/smaps_rollup, in bytes,
// the soak's memory lines beside VmRSS. Returns 1 when both were read.

static inline int process_resources_smaps( int64_t * rss_bytes, int64_t * private_dirty_bytes )
{
    *rss_bytes = -1;
    *private_dirty_bytes = -1;
#if !defined(_WIN32) && !defined(__APPLE__)
    FILE * file = fopen( "/proc/self/smaps_rollup", "r" );
    if ( !file )
        return 0;
    char line[256];
    while ( fgets( line, sizeof( line ), file ) )
    {
        if ( strncmp( line, "Rss:", 4 ) == 0 )
            *rss_bytes = (int64_t) strtoll( line + 4, NULL, 10 ) * 1024;
        else if ( strncmp( line, "Private_Dirty:", 14 ) == 0 )
            *private_dirty_bytes = (int64_t) strtoll( line + 14, NULL, 10 ) * 1024;
    }
    fclose( file );
#endif
    return *rss_bytes >= 0 && *private_dirty_bytes >= 0;
}

#endif // #ifndef ROCKETNET_TESTS_PROCESS_RESOURCES_H
