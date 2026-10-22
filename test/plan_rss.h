/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The peak memory bound of one test process, derived from the plan.

    Every byte the SDK allocates goes through the allocator rocketnet_init
    is given, and the SDK allocates exactly its memory plan (the plans are
    checked against allocator observations: tools/check_memory.py). A test
    process passes plan_rss_malloc and plan_rss_free to rocketnet_init; they
    count the bytes the SDK holds and keep the peak. When the case ends,
    plan_rss_check compares the process's peak resident set with that peak
    plus PLAN_RSS_SLACK_MB, a fixed allowance for what no plan counts: the
    executable and its libraries, the C and C++ runtimes, thread stacks, the
    test's own bookkeeping. A process that grows past its plan by more than
    the slack fails, naming its peak, the plan's peak and the slack, so memory
    the SDK does not account for (or a test that leaks outside the SDK) still
    fails, however large the plan the case legitimately builds.

    A case that passes its own allocator to rocketnet_init can chain it under
    the counter with plan_rss_set_inner. A case that re-initialises rocketnet
    with an allocator of its own leaves the counter; its process is held to the
    peak counted before that, plus the slack.

    With ROCKETNET_TEST_MAX_CASE_RSS_MB set, the suite runner applies that
    bound to every case instead and plan_rss_check checks nothing. A sanitizer
    build checks nothing: sanitizer shadow memory is not the case's own.
*/

#ifndef ROCKETNET_TEST_PLAN_RSS_H
#define ROCKETNET_TEST_PLAN_RSS_H

#include "portable_crt.h"
#include "time_scale.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#endif

typedef void * ( *plan_rss_malloc_function_t )( void * context, size_t bytes );
typedef void ( *plan_rss_free_function_t )( void * context, void * p );

// Blocks of at least this many bytes are mapped from the OS and unmapped at
// free. The C allocator keeps a large block it freed resident for reuse (on
// macOS, its large-allocation cache), so a process that builds a 300-client
// server, destroys it and builds a 1000-client one peaked at the sum of both:
// 994 MiB against a plan peak of 862 MiB. Mapped, freed memory leaves the
// resident set at once, and the peak resident set follows the plan's peak.
#define PLAN_RSS_MAP_BYTES ( 1024 * 1024 )

// Measured on 2026-10-21, Release, every case of both suites with the
// nightly tier: the largest remainder over the plan's peak was 48 MiB on macOS
// arm64 (unit test_user_block_is_used_and_nothing_else; functional at most
// 32 MiB, test_lifecycle_transport) and 44 MiB on Linux x64 (unit; functional
// at most 14 MiB). 64 MiB keeps 16 MiB over the largest.
#define PLAN_RSS_SLACK_MB 64

// the counted allocation's header: the bytes the SDK asked for and, for a
// mapped block, the mapping's length; two words, so the SDK's own alignment
// sees an ordinarily aligned block
struct plan_rss_header_t
{
    size_t bytes;
    size_t mapped_bytes; // 0: from malloc; else the mapping's length
};

inline std::atomic<uint64_t> & plan_rss_live()
{
    static std::atomic<uint64_t> live( 0 );
    return live;
}

inline std::atomic<uint64_t> & plan_rss_peak()
{
    static std::atomic<uint64_t> peak( 0 );
    return peak;
}


inline plan_rss_malloc_function_t & plan_rss_inner_malloc()
{
    static plan_rss_malloc_function_t function = NULL;
    return function;
}

inline plan_rss_free_function_t & plan_rss_inner_free()
{
    static plan_rss_free_function_t function = NULL;
    return function;
}

// chain a case's own allocator under the counter
inline void plan_rss_set_inner( plan_rss_malloc_function_t malloc_function, plan_rss_free_function_t free_function )
{
    plan_rss_inner_malloc() = malloc_function;
    plan_rss_inner_free() = free_function;
}

inline void * plan_rss_map( size_t bytes )
{
#if defined(_WIN32)
    return VirtualAlloc( NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
#else
    void * block = mmap( NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    return block == MAP_FAILED ? NULL : block;
#endif
}

inline void plan_rss_unmap( void * block, size_t bytes )
{
#if defined(_WIN32)
    (void) bytes;
    VirtualFree( block, 0, MEM_RELEASE );
#else
    munmap( block, bytes );
#endif
}

// the sizes of the blocks the inner allocator holds: its blocks are passed
// through exactly as the SDK asked, so a case that checks its own allocator's
// byte counts sees the same bytes with or without the counter
inline std::mutex & plan_rss_inner_mutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<void*, size_t> & plan_rss_inner_sizes()
{
    static std::unordered_map<void*, size_t> sizes;
    return sizes;
}

inline void plan_rss_count( size_t bytes )
{
    const uint64_t live = plan_rss_live().fetch_add( bytes ) + bytes;
    uint64_t peak = plan_rss_peak().load();
    while ( live > peak && !plan_rss_peak().compare_exchange_weak( peak, live ) ) {}
}

inline void * plan_rss_malloc( void * context, size_t bytes )
{
    if ( plan_rss_inner_malloc() )
    {
        void * block = plan_rss_inner_malloc()( context, bytes );
        if ( !block )
            return NULL;
        {
            std::lock_guard<std::mutex> lock( plan_rss_inner_mutex() );
            plan_rss_inner_sizes()[block] = bytes;
        }
        plan_rss_count( bytes );
        return block;
    }
    if ( bytes > SIZE_MAX - sizeof( plan_rss_header_t ) - 65536 )
        return NULL;
    const size_t total = bytes + sizeof( plan_rss_header_t );
    size_t mapped_bytes = 0;
    void * block = NULL;
    if ( total >= PLAN_RSS_MAP_BYTES )
    {
        mapped_bytes = ( total + 65535 ) & ~(size_t) 65535;
        block = plan_rss_map( mapped_bytes );
    }
    else
        block = malloc( total );
    if ( !block )
        return NULL;
    plan_rss_header_t * header = (plan_rss_header_t*) block;
    header->bytes = bytes;
    header->mapped_bytes = mapped_bytes;
    plan_rss_count( bytes );
    return header + 1;
}

inline void plan_rss_free( void * context, void * p )
{
    if ( !p )
        return;
    size_t inner_bytes = 0;
    bool inner = false;
    {
        std::lock_guard<std::mutex> lock( plan_rss_inner_mutex() );
        std::unordered_map<void*, size_t>::iterator it = plan_rss_inner_sizes().find( p );
        if ( it != plan_rss_inner_sizes().end() )
        {
            inner = true;
            inner_bytes = it->second;
            plan_rss_inner_sizes().erase( it );
        }
    }
    if ( inner )
    {
        plan_rss_live().fetch_sub( inner_bytes );
        plan_rss_inner_free()( context, p );
        return;
    }
    plan_rss_header_t * header = ( (plan_rss_header_t*) p ) - 1;
    plan_rss_live().fetch_sub( header->bytes );
    if ( header->mapped_bytes )
        plan_rss_unmap( header, header->mapped_bytes );
    else
        free( header );
}

// a case's own allocator chained under the counter, for rocketnet_init:
// rocketnet_init( context, plan_rss_chain( my_malloc, my_free ), plan_rss_free )
inline plan_rss_malloc_function_t plan_rss_chain( plan_rss_malloc_function_t malloc_function, plan_rss_free_function_t free_function )
{
    plan_rss_set_inner( malloc_function, free_function );
    return plan_rss_malloc;
}

// this process's peak resident set in MiB, or -1
inline long plan_rss_process_peak_mb()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS memory;
    if ( !GetProcessMemoryInfo( GetCurrentProcess(), &memory, sizeof( memory ) ) )
        return -1;
    return (long) ( memory.PeakWorkingSetSize / ( 1024 * 1024 ) );
#else
    struct rusage usage;
    if ( getrusage( RUSAGE_SELF, &usage ) != 0 )
        return -1;
#if defined(__APPLE__)
    return (long) ( usage.ru_maxrss / ( 1024 * 1024 ) ); // bytes on macOS
#else
    return (long) ( usage.ru_maxrss / 1024 ); // kilobytes on Linux
#endif
#endif
}

// the bound: the plan's peak in whole MiB plus the slack
inline long plan_rss_bound_mb()
{
    const uint64_t mib = 1024 * 1024;
    return (long) ( ( plan_rss_peak().load() + mib - 1 ) / mib ) + PLAN_RSS_SLACK_MB;
}

// 0 when the process kept within its plan, else 1 after the check line
inline int plan_rss_check( const char * name )
{
#if defined(ROCKETNET_FUNCTIONAL_SANITIZED)
    (void) name;
    return 0;
#else
    char override_value[64];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_MAX_CASE_RSS_MB", override_value, sizeof( override_value ) ) )
        return 0;
    const long peak_rss_mb = plan_rss_process_peak_mb();
    const long bound_mb = plan_rss_bound_mb();
    if ( peak_rss_mb < 0 || peak_rss_mb <= bound_mb )
        return 0;
    printf( "check failed: %s peak_rss_mb=%ld bound_mb=%ld (plan peak %llu bytes + %d MiB slack)\n", name, peak_rss_mb, bound_mb,
            (unsigned long long) plan_rss_peak().load(), PLAN_RSS_SLACK_MB );
    fflush( stdout );
    return 1;
#endif
}

#endif // #ifndef ROCKETNET_TEST_PLAN_RSS_H
