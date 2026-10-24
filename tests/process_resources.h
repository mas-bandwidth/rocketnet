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
                 threads         a Toolhelp32 walk of this process's threads
                 private_bytes   GetProcessMemoryInfo PrivateUsage

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
    or touches memory it did not hold at the post-warmup sample. On Windows
    the memory count is the commit charge (PrivateUsage), which counts
    preallocated memory at its allocation, touched or not; macOS has no
    populate call and its count is read as it is.

    THE FLAT RULE (process_resources_flat). Taken after a warm cycle, so every
    path the run repeats has run once, and after the warm-up above: the fd or handle count and the thread
    count at the end must equal their post-warmup counts exactly, and the
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
#include <sys/mman.h>
#include <unistd.h>
#endif

#define PROCESS_RESOURCES_COUNT 3

#define PROCESS_RESOURCES_HANDLES 0
#define PROCESS_RESOURCES_THREADS 1
#define PROCESS_RESOURCES_MEMORY 2

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

// reads every count into *resources; returns 1 when all three were read

static inline int process_resources_read( struct process_resources_t * resources )
{
    for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
        resources->values[i] = -1;

#if defined(_WIN32)

    DWORD handles = 0;
    if ( GetProcessHandleCount( GetCurrentProcess(), &handles ) )
        resources->values[PROCESS_RESOURCES_HANDLES] = (int64_t) handles;

    HANDLE thread_list = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( thread_list != INVALID_HANDLE_VALUE )
    {
        THREADENTRY32 entry;
        entry.dwSize = sizeof( entry );
        const DWORD self_pid = GetCurrentProcessId();
        int64_t threads = 0;
        if ( Thread32First( thread_list, &entry ) )
        {
            do
            {
                if ( entry.th32OwnerProcessID == self_pid )
                    threads++;
            } while ( Thread32Next( thread_list, &entry ) );
        }
        CloseHandle( thread_list );
        resources->values[PROCESS_RESOURCES_THREADS] = threads;
    }

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
