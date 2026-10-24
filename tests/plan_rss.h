/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The peak resident set bound of one case process, from its plan.

    Every byte the SDK allocates goes through the allocator rocketnet_init is
    given, and the SDK allocates exactly its memory plan (the plans are checked
    against allocator observations: tools/check_memory.py). A case process
    installs plan_rss_malloc and plan_rss_free with rocketnet_init; they count
    the bytes the SDK holds and keep the peak. The bound of the process is that
    peak plus PLAN_RSS_SLACK_MB, a fixed allowance for what no plan counts: the
    executable and its libraries, the C and C++ runtimes, thread stacks and the
    case's own bookkeeping. A process past it has memory the SDK does not
    account for, or a case that holds memory outside the SDK, however large the
    server the case legitimately builds.

    The case process reports its bound as one line, PLAN_RSS_LINE then the
    bound in MiB, after the case passed and only while the counter is still the
    installed allocator; the suite runner (tests/suite_runner.h) reads that line
    from the captured output and holds the process's peak resident set (wait4
    ru_maxrss, PeakWorkingSetSize) to it. A case that re-initialises rocketnet
    with an allocator of its own leaves the counter, reports nothing, and keeps
    the suite-wide bound, ROCKETNET_TEST_MAX_CASE_RSS_MB; so does every process
    that never installs the counter. The environment variable of that name
    replaces every bound, this one included.

    Planned bytes the SDK takes from the OS outside the allocator are counted
    by the case that makes them: the AF_XDP UMEM (rocketnet_xdp_umem_bytes) is
    in the server's plan but is a posix_memalign block registered with the
    kernel (sdk/rocketnet_server_socket_linux.cpp:1712), so a case that builds
    an xdp server counts it with plan_rss_count_outside_allocator after the
    create and plan_rss_uncount_outside_allocator after the destroy.
*/

#ifndef ROCKETNET_TEST_PLAN_RSS_H
#define ROCKETNET_TEST_PLAN_RSS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// the line a case process prints to report its bound, then the bound in MiB
#define PLAN_RSS_LINE "plan_rss_bound_mb="

// The slack, measured 2026-10-23 over every case that reported its plan,
// Release: the largest peak resident set over the plan's peak was 7 MiB on
// macOS arm64 (mini-m5-1, ./bin/functional and its nightly tier,
// test_posix_send_burst) and 5 MiB on Linux x86_64 (amd-1, ./bin/functional,
// test_gso_transport_server_probe). Most cases peak far under their plan,
// whose pages are not all touched. 64 MiB leaves room for a Debug build's
// larger image, which was not measured (docs/MACOS.md).
#define PLAN_RSS_SLACK_MB 64

// Blocks of at least this many bytes are mapped from the OS and unmapped at
// free. The C allocator keeps a large block it freed resident for reuse (on
// macOS, its large-allocation cache), so a process that builds a server,
// destroys it and builds a larger one would peak at the sum of both while
// its plan peaks at the larger. Mapped memory leaves the resident set at its
// unmap, so the peak resident set follows the plan's peak.
#define PLAN_RSS_MAP_BYTES ( 1024 * 1024 )

#define PLAN_RSS_MAP_GRANULE ( 64 * 1024 )

// the counted block's header: the bytes the SDK asked for, and the mapping's
// length for a mapped block (0: from malloc); two words, so the block keeps
// the C allocator's alignment for the SDK's own alignment step
struct plan_rss_header_t
{
    size_t bytes;
    size_t mapped_bytes;
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

// every allocation the counter served: plan_rss_installed reads it
inline std::atomic<uint64_t> & plan_rss_calls()
{
    static std::atomic<uint64_t> calls( 0 );
    return calls;
}

inline void plan_rss_count( uint64_t bytes )
{
    const uint64_t live = plan_rss_live().fetch_add( bytes ) + bytes;
    uint64_t peak = plan_rss_peak().load();
    while ( live > peak && !plan_rss_peak().compare_exchange_weak( peak, live ) ) {}
}

inline void plan_rss_uncount( uint64_t bytes )
{
    plan_rss_live().fetch_sub( bytes );
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

inline void * plan_rss_malloc( void * context, size_t bytes )
{
    (void) context;
    plan_rss_calls().fetch_add( 1 );
    if ( bytes > SIZE_MAX - sizeof( plan_rss_header_t ) - PLAN_RSS_MAP_GRANULE )
        return NULL;
    const size_t total = bytes + sizeof( plan_rss_header_t );
    size_t mapped_bytes = 0;
    void * block = NULL;
    if ( total >= PLAN_RSS_MAP_BYTES )
    {
        mapped_bytes = ( total + PLAN_RSS_MAP_GRANULE - 1 ) & ~(size_t) ( PLAN_RSS_MAP_GRANULE - 1 );
        block = plan_rss_map( mapped_bytes );
    }
    else
    {
        block = malloc( total );
    }
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
    (void) context;
    if ( !p )
        return;
    plan_rss_header_t * header = ( (plan_rss_header_t*) p ) - 1;
    plan_rss_uncount( header->bytes );
    if ( header->mapped_bytes )
        plan_rss_unmap( header, header->mapped_bytes );
    else
        free( header );
}

// planned bytes the SDK holds outside the allocator (the header comment)
inline void plan_rss_count_outside_allocator( uint64_t bytes )
{
    plan_rss_count( bytes );
}

inline void plan_rss_uncount_outside_allocator( uint64_t bytes )
{
    plan_rss_uncount( bytes );
}

// the bound: the plan's peak in whole MiB plus the slack
inline long plan_rss_bound_mb()
{
    const uint64_t mib = 1024 * 1024;
    return (long) ( ( plan_rss_peak().load() + mib - 1 ) / mib ) + PLAN_RSS_SLACK_MB;
}

// whether the counter is the installed allocator: allocate( 1 ) is the
// installed allocator's rocketnet_malloc, free( block ) its rocketnet_free.
// The caller probes only an initialised library with no allocator context,
// so the probe never calls a case's allocator whose context has gone
inline bool plan_rss_installed( void * ( *allocate )( void * context, size_t bytes ), void ( *release )( void * context, void * p ) )
{
    const uint64_t before = plan_rss_calls().load();
    void * block = allocate( NULL, 1 );
    if ( !block )
        return false;
    const bool counted = plan_rss_calls().load() != before;
    release( NULL, block );
    return counted;
}

// the report line of a case process that kept the counter
inline void plan_rss_report()
{
    printf( PLAN_RSS_LINE "%ld\n", plan_rss_bound_mb() );
    fflush( stdout );
}

#endif // #ifndef ROCKETNET_TEST_PLAN_RSS_H
