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

    THE FLAT RULE. Taken after a warm-up window that lets every preallocated
    buffer the run repeats be touched once (the caller's warm cycle: its
    first full send/receive and reconnect cycle, and never under 30 s), so
    pages of preallocated memory faulting in on first use are not counted:

      fds (handles) and threads   the count at the end equals the
                                  post-warmup count exactly
                                  (process_resources_flat).
      memory                      the least-squares slope of the memory
                                  samples taken every 10 s after the
                                  warm-up (resident_bytes; on Linux also
                                  smaps Rss and Private_Dirty) is at most
                                  one page per minute
                                  (process_resources_slope_flat). A
                                  falling memory count is not a leak and
                                  passes.

    Nothing is prefaulted to make the rule hold: memory still growing after
    the warm-up is reported, never excused.

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

// THE FLAT RULE for an fd (handle) or thread count: 1 when the end count
// equals the post-warmup count, 0 when it does not (or either is unread);
// memory is judged by its slope (process_resources_slope_flat)

static inline int process_resources_flat( int64_t start, int64_t end )
{
    if ( start < 0 || end < 0 )
        return 0;
    return end == start;
}

// the mapped-and-touched memory of the process on Linux: Rss and
// Private_Dirty from /proc/self/smaps_rollup, in bytes; both -1 elsewhere or
// when unreadable, and the function returns 0

#define PROCESS_RESOURCES_DETAIL_COUNT 2

#define PROCESS_RESOURCES_DETAIL_SMAPS_RSS 0
#define PROCESS_RESOURCES_DETAIL_PRIVATE_DIRTY 1

static inline const char * process_resources_detail_name( int index )
{
    switch ( index )
    {
        case PROCESS_RESOURCES_DETAIL_SMAPS_RSS:      return "smaps_rss_bytes";
        case PROCESS_RESOURCES_DETAIL_PRIVATE_DIRTY:  return "private_dirty_bytes";
        default:                                      return "unknown";
    }
}

static inline int process_resources_read_detail( int64_t * detail )
{
    detail[PROCESS_RESOURCES_DETAIL_SMAPS_RSS] = -1;
    detail[PROCESS_RESOURCES_DETAIL_PRIVATE_DIRTY] = -1;
#if !defined(_WIN32) && !defined(__APPLE__)
    FILE * file = fopen( "/proc/self/smaps_rollup", "r" );
    if ( !file )
        return 0;
    char line[256];
    while ( fgets( line, sizeof( line ), file ) )
    {
        if ( strncmp( line, "Rss:", 4 ) == 0 )
            detail[PROCESS_RESOURCES_DETAIL_SMAPS_RSS] = (int64_t) strtoll( line + 4, NULL, 10 ) * 1024;
        else if ( strncmp( line, "Private_Dirty:", 14 ) == 0 )
            detail[PROCESS_RESOURCES_DETAIL_PRIVATE_DIRTY] = (int64_t) strtoll( line + 14, NULL, 10 ) * 1024;
    }
    fclose( file );
#endif
    return detail[PROCESS_RESOURCES_DETAIL_SMAPS_RSS] >= 0 && detail[PROCESS_RESOURCES_DETAIL_PRIVATE_DIRTY] >= 0;
}

// the least-squares slope of count samples (seconds[i], bytes[i]), in bytes
// per minute; 0 for fewer than two samples or samples all at one time

static inline double process_resources_slope_bytes_per_minute( const double * seconds, const int64_t * bytes, int count )
{
    if ( count < 2 )
        return 0.0;
    double mean_seconds = 0.0;
    double mean_bytes = 0.0;
    for ( int i = 0; i < count; i++ )
    {
        mean_seconds += seconds[i];
        mean_bytes += (double) bytes[i];
    }
    mean_seconds /= (double) count;
    mean_bytes /= (double) count;
    double covariance = 0.0;
    double variance = 0.0;
    for ( int i = 0; i < count; i++ )
    {
        const double ds = seconds[i] - mean_seconds;
        covariance += ds * ( (double) bytes[i] - mean_bytes );
        variance += ds * ds;
    }
    if ( variance <= 0.0 )
        return 0.0;
    return covariance / variance * 60.0;
}

// THE FLAT RULE for memory: 1 when the post-warmup slope is at most one page
// per minute (a falling count passes), 0 when it is not or there are fewer
// than two post-warmup samples

static inline int process_resources_slope_flat( double bytes_per_minute, int count )
{
    if ( count < 2 )
        return 0;
    return bytes_per_minute <= (double) process_resources_page_bytes();
}

#endif // #ifndef ROCKETNET_TESTS_PROCESS_RESOURCES_H
