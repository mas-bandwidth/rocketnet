/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Suite runner. Each test is a process, and cases run in parallel, one
    job per core. The output is the netcode
    shape: one line per test, its name, in suite order, then
    "*** ALL TESTS PASSED ***". A failing test prints its own output (its
    check line) and the suite exits 1 at once. A test over 120s is killed and
    fails. The functional runner keeps 120s in every build; the unit runner
    scales it for Debug, sanitizer or emulated builds (tests/time_scale.h).
    ROCKETNET_TEST_CASE_TIME_SCALE in the environment multiplies the case limit
    a suite passes in; the nightly macOS TSAN functional runs set 30, a
    one-hour case limit, and every other run leaves it unset. A
    functional case whose peak resident set (wait4 ru_maxrss on POSIX,
    PeakWorkingSetSize on Windows) is over its bound fails
    (ROCKETNET_TEST_MAX_CASE_RSS_MB, overridable by that environment variable;
    a sanitizer build does not enforce it unless the variable sets one). A case
    whose subject is a full-capacity server declares its own bound with
    SUITE_CASE_RSS_BOUND. A case process that counts the SDK's bytes reports
    its own bound, its plan's peak plus a fixed slack (tests/plan_rss.h), and
    is held to that instead. Every registered name is test_ lowercase snake_case,
    or the suite fails before running anything. ROCKETNET_TEST_REPORT=1 prints
    the slowest cases and the peak resident sets after the results.
    ROCKETNET_TEST_NIGHTLY=1 adds the nightly tier: the cases group_of marks
    with SUITE_GROUP_NIGHTLY, which a per-commit run leaves out.
*/

#pragma once

#ifndef ROCKETNET_TESTS_SUITE_RUNNER_H
#define ROCKETNET_TESTS_SUITE_RUNNER_H

#include "plan_rss.h"
#include "portable_crt.h"
#include "time_scale.h"
#include "rocketnet.h"                      // ROCKETNET_MAX_CLIENTS
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined( __APPLE__ )
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif // #if defined( __APPLE__ )

#if defined( _WIN32 )
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // #ifndef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include "resource_file.h"
#else
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char ** environ;
#endif // #if defined( _WIN32 )

// The largest peak resident set one case process may reach, in MiB: the larger
// of 256 and 1.5 times the largest case measured on a Linux Release build
// (465 MiB, test_gso_transport_server_io_pin, 2026-10-19), so 698; never over 1024.
// A case over it fails the suite. The environment variable of the same name
// overrides it with a positive value. A case process that reports its plan's
// bound (tests/plan_rss.h) is held to that instead.
//
// A build for more clients (-DROCKETNET_MAX_CLIENTS, BUILDING.md "Maximum
// clients") carries larger per-server tables whatever the case's max_clients:
// the receive arrays and payload batches hold 16 packets for every client
// the build holds. There the bound is 1.5 times the largest case measured at
// 10000 clients on a macOS Release build (811 MiB, 2026-10-23, a server
// statistics case since deleted with the background recompute it pinned),
// so 1217; a build for more than 10000 sets the environment variable.
#if ROCKETNET_MAX_CLIENTS <= 1000
#define ROCKETNET_TEST_MAX_CASE_RSS_MB 698
#else
#define ROCKETNET_TEST_MAX_CASE_RSS_MB 1217
#endif // #if ROCKETNET_MAX_CLIENTS <= 1000

// group_of's flag for a case of the nightly tier: the case runs, in the group
// the rest of the value names, only under ROCKETNET_TEST_NIGHTLY=1. The cases
// moved there each wait out a real protocol timeout or repeat a transport
// pair whose every assertion a per-commit case of the same name keeps.
#define SUITE_GROUP_NIGHTLY 0x100

inline bool suite_nightly()
{
    char value[16];
    return rocketnet_test_getenv( "ROCKETNET_TEST_NIGHTLY", value, sizeof( value ) ) && std::strcmp( value, "1" ) == 0;
}

// Whether the environment variable is set (to a value of at most 4095 bytes).

inline bool suite_env_is_set( const char * name )
{
    char value[4096];
    return rocketnet_test_getenv( name, value, sizeof( value ) ) != 0;
}

// The case limit after the environment's scale. ROCKETNET_TEST_CASE_TIME_SCALE
// unset or empty leaves the limit as the suite passed it; set, it is a number over 0
// and at most 1000 that multiplies the limit, or the suite refuses to run.

inline bool suite_case_limit( double case_limit_s, double * limit_s )
{
    *limit_s = case_limit_s;

    double scale = 1.0;
    if ( !rocketnet_test_case_env_time_scale( &scale ) )
    {
        char env[64];
        rocketnet_test_getenv( "ROCKETNET_TEST_CASE_TIME_SCALE", env, sizeof( env ) );
        printf( "check failed: ROCKETNET_TEST_CASE_TIME_SCALE=%s is not a number over 0 and at most 1000\n", env );
        return false;
    }

    *limit_s = case_limit_s * scale;

    return true;
}

struct suite_exec
{
    const char * name;
    const char * file;
    const char * argument;
    bool check_rss;
};

struct suite_case
{
    std::string name;
    std::string exec_path; // empty: re-exec this binary with --run name
    std::string exec_argument;
    bool check_rss;
    int group; // suite_main's group_of: cases of different groups never run at once
};

struct suite_result
{
    std::string name;
    bool exec;
    bool done;
    double seconds;
    bool failed;
    std::string detail;
    long peak_rss_mb; // -1: not measured
    long plan_rss_mb; // the bound the case process reported from its plan (tests/plan_rss.h); -1: none
    bool rss_over_bound;
    bool check_rss;
    std::string output;
};

// The bound in MiB, or 0 when it is not enforced (a sanitizer build with no
// environment override: sanitizer shadow memory is not the case's footprint).

inline long suite_max_case_rss_mb()
{
    char env[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_MAX_CASE_RSS_MB", env, sizeof( env ) ) )
    {
        const long requested = std::atol( env );
        if ( requested > 0 )
            return requested;
    }
#if defined( ROCKETNET_FUNCTIONAL_SANITIZED )
    return 0;
#else
    return ROCKETNET_TEST_MAX_CASE_RSS_MB;
#endif // #if defined( ROCKETNET_FUNCTIONAL_SANITIZED )
}

// A case whose subject is a full-capacity server allocates its designed
// per-client memory up front, so its footprint is set by the SDK's memory
// plan, not by the suite-wide bound. Such a case declares its bound beside
// itself with SUITE_CASE_RSS_BOUND( name, function ): function returns the
// bound in MiB, computed from the memory plan for the configuration the case
// uses plus a margin the case states (suite_rss_bound_mb). A static
// registrar adds it to a fixed registry before main runs, so the parent reads it by
// case name. Every other case keeps ROCKETNET_TEST_MAX_CASE_RSS_MB.

struct suite_case_rss_bound_t
{
    const char * name;
    long ( *bound_mb )();
};

// Fifteen bounds existed when this registry replaced the pointer chain. The
// next power of two with another full doubling of headroom is 64. It remains
// allocation-free during static initialization and refuses overflow loudly.
#define SUITE_CASE_RSS_BOUND_CAPACITY 64

struct suite_case_rss_bound_registry_t
{
    suite_case_rss_bound_t entries[SUITE_CASE_RSS_BOUND_CAPACITY];
    int count;
};

inline suite_case_rss_bound_registry_t * suite_case_rss_bound_registry()
{
    static suite_case_rss_bound_registry_t registry = {};
    return &registry;
}

// Adds a bound at the front, the order the pointer chain had: the most
// recently constructed registrar is enumerated first, so when a name is
// registered twice the later registration is the one a lookup finds, exactly
// as the chain did. A full registry is left unchanged and the overflow
// function is called with the omitted name and the capacity; false then.

inline bool suite_case_rss_bound_add( suite_case_rss_bound_registry_t * registry, int capacity, const char * name,
                                      long ( *bound_mb )(), void ( *overflow )( const char * name, int capacity ) )
{
    if ( registry->count >= capacity )
    {
        overflow( name, capacity );
        return false;
    }

    if ( registry->count > 0 )
        memmove( &registry->entries[1], &registry->entries[0],
                 (size_t) registry->count * sizeof( registry->entries[0] ) );
    registry->entries[0].name = name;
    registry->entries[0].bound_mb = bound_mb;
    registry->count++;

    return true;
}

// The first entry under a name (the latest registered), or NULL.

inline suite_case_rss_bound_t * suite_case_rss_bound_find( suite_case_rss_bound_registry_t * registry, const char * name )
{
    for ( int i = 0; i < registry->count; i++ )
        if ( strcmp( registry->entries[i].name, name ) == 0 )
            return &registry->entries[i];
    return NULL;
}

// The line printed before the abort when a registration would overflow.

inline int suite_case_rss_bound_overflow_text( char * text, size_t bytes, const char * name, int capacity )
{
    return snprintf( text, bytes, "error: suite RSS bound registry capacity %d exceeded while registering %s\n",
                     capacity, name );
}

// A static registrar's overflow: print the omitted name, then abort before
// main runs, so a bound is never silently dropped.

inline void suite_case_rss_bound_overflow_abort( const char * name, int capacity )
{
    char text[256];
    suite_case_rss_bound_overflow_text( text, sizeof( text ), name, capacity );
    fputs( text, stderr );
    fflush( stderr );
    abort();
}

struct suite_case_rss_bound_registrar_t
{
    suite_case_rss_bound_registrar_t( const char * name, long ( *bound_mb )() ) noexcept
    {
        suite_case_rss_bound_add( suite_case_rss_bound_registry(), SUITE_CASE_RSS_BOUND_CAPACITY, name, bound_mb,
                                  suite_case_rss_bound_overflow_abort );
    }
};

#define SUITE_CASE_RSS_BOUND_JOIN_( a, b ) a##b
#define SUITE_CASE_RSS_BOUND_JOIN( a, b ) SUITE_CASE_RSS_BOUND_JOIN_( a, b )
#define SUITE_CASE_RSS_BOUND( name, function ) \
    static suite_case_rss_bound_registrar_t SUITE_CASE_RSS_BOUND_JOIN( suite_case_rss_bound_, __LINE__ )( #name, function )

// A planned byte count as a bound in MiB: the bytes rounded up to whole MiB,
// plus the case's stated margin for the process itself (executable, runtime
// and stacks), which the plan does not count. 0 when the plan refused.

inline long suite_rss_bound_mb( uint64_t planned_bytes, long margin_mb )
{
    if ( planned_bytes == 0 )
        return 0;

    const uint64_t mib = 1024 * 1024;
    return (long) ( ( planned_bytes + mib - 1 ) / mib ) + margin_mb;
}

// The size of one page of this process's memory in bytes: 4 KiB on x86_64
// Linux and Windows, 16 KiB on Apple silicon. A case whose library tables are
// touched sparsely holds one page resident for each touch, so its resident
// set grows with the page; a bound for such a case scales with this.

inline uint64_t suite_page_bytes()
{
#if defined( _WIN32 )
    SYSTEM_INFO info;
    GetSystemInfo( &info );
    return info.dwPageSize > 0 ? (uint64_t) info.dwPageSize : 4096;
#else
    const long bytes = sysconf( _SC_PAGESIZE );
    return bytes > 0 ? (uint64_t) bytes : 4096;
#endif // #if defined( _WIN32 )
}

// The bound for one case: none in a sanitizer build without the environment
// override, else the bound the case declares, else the bound its process
// reported from its plan (plan_mb, -1 for none), else the suite-wide one.
// The environment override replaces the suite-wide bound for every case, and
// a declared or reported bound under it: it lifts a case's plan, never
// lowers it. A declared bound of 0 (a refused plan) keeps the suite-wide
// bound.

inline long suite_case_rss_mb( const std::string & name, long plan_mb )
{
    const long bound = suite_max_case_rss_mb();
    if ( bound <= 0 )
        return bound;

    long planned = plan_mb > 0 ? plan_mb : 0;
    suite_case_rss_bound_t * entry = suite_case_rss_bound_find( suite_case_rss_bound_registry(), name.c_str() );
    if ( entry != NULL )
    {
        const long declared = entry->bound_mb();
        if ( declared > 0 )
        {
            planned = declared;
        }
    }

    if ( planned <= 0 )
        return bound;
    if ( suite_env_is_set( "ROCKETNET_TEST_MAX_CASE_RSS_MB" ) )
        return planned > bound ? planned : bound;

    return planned;
}

// Records a finished case whose peak is over the bound, naming both. The
// functional tier enforces the result; the unit tier keeps the same
// measurement and prints the over-bound line without changing its verdict.

inline void suite_check_rss( suite_result & result, long bound_mb, bool enforce = true )
{
    if ( result.failed || bound_mb <= 0 || result.peak_rss_mb <= bound_mb )
        return;

    char text[96];
    snprintf( text, sizeof( text ), " peak_rss_mb=%ld bound_mb=%ld", result.peak_rss_mb, bound_mb );
    result.detail = result.name + text;
    result.rss_over_bound = true;
    result.failed = enforce;
}

// A test name is test_ then lowercase snake_case: [a-z0-9] words joined by
// single underscores.

inline bool suite_name_is_test_snake_case( const std::string & name )
{
    if ( name.size() <= 5 || name.compare( 0, 5, "test_" ) != 0 )
        return false;

    char previous = '_';

    for ( size_t i = 0; i < name.size(); i++ )
    {
        const char c = name[i];
        if ( c == '_' )
        {
            if ( previous == '_' )
                return false;
        }
        else if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) )
        {
            return false;
        }

        previous = c;
    }

    return previous != '_';
}

// The suite's order, simplest first: core (init, allocator, logging), helpers,
// packet, acks, transport basics, each transport type, client/server, messages. A case's layer is the first
// rule whose area word its name starts with (after test_); the cases keep
// their registration order within a layer.

inline int suite_layer_of( const std::string & name )
{
    static const char * const layers[][48] = {
        // core: init, allocator, logging, status names
        { "log", "allocator", "status_name", NULL },
        // helpers: platform, time, base64, config, memory
        { "platform", "time", "update_time", "sleep", "thread", "endian", "read", "base64", "hash", "copy", "address", "random", "config", "memory", "status", "version", "value_tracker", "secret", "suite", "fp", "hostname", "counter", "class", "sized", "dev", "abi", "unit", "runtime", "user", "deterministic", "fuzz", "pcapng", "rocketnet", "default", "disabled", "limits", "release", "exhaustion", NULL },
        // packet: wire, crypto, tokens, prefilter, sealed packets
        { "wire", "crypto", "wycheproof", "token", "connect_token", "protocol", "signed", "prefilter", "packet", "sealed", "unsealed", "open_sealed", "cookie", "standard", "serialize", "replay", "integrity", "keyed", "encryption", "used", "hostile", "tampered", "vectors", "salt", "response", "admission", "payload", "max", "large", "small", "coalesce", "v4", "ipv4", "ipv6", "reused", "parse", NULL },
        // acks: the transport's ack system and sequence buffers
        { "ack", "sequence", "send_history", "loss", "jitter", "out_of_order", "reply", "previous", "pending", "never", "newer", "older", "newest", "equal", "same", "repeated", "higher", "round", "one", "both", "first", "each", NULL },
        // the message layer: fragments, the window, receipts, channels, the endpoint and the message objects
        { "fragment", "reassembly", "window", "receipt", "rnreceipts", "resend", "resent", "watermark", "held", "hold", "endpoint", "budget", "send_budget", "rnscheduler", "message", "channel", "rnchannel", "reliab", "unreliable", "ordered", "violation", "wide", "dispatch", "block", "promoted", "mixed", "rtt_estimator", "shares", "generation", "reservation", "store", "chunk", "span", "builder", NULL },
        // transport basics: the driver/transport interface and the ladder
        { "driver", "transport", "ladder", "network_simulator", "sim", "link", "send", "receive", "fake", "table", "rnslots", NULL },
        // each transport type
        { "portable", "loopback", "msgx", "gso", "io_uring", "xdp", "iocp", "rio", "windows", "posix", "linux", "macos", "umem", NULL },
        // client/server: connect, handshake, slots, disconnect, lifecycle
        { "server", "client", "connect", "handshake", "request", "challenge", "slot", "disconnect", "lifecycle", "keep_alive", "frame_sync", "dst", "live", "public", "api", "create", "destroy", "start", "backend", "session", "half", "two", "swarm", "full", "stale", NULL },
        // harnesses, control and examples
        { "harness", "control", "state", "example", NULL },
    };
    const int count = (int) ( sizeof( layers ) / sizeof( layers[0] ) );
    const std::string area = name.compare( 0, 5, "test_" ) == 0 ? name.substr( 5 ) : name;
    for ( int layer = 0; layer < count; layer++ )
        for ( int i = 0; layers[layer][i] != NULL; i++ )
            if ( area.compare( 0, std::strlen( layers[layer][i] ), layers[layer][i] ) == 0 )
                return layer;

    return count;
}

inline std::string suite_executable_path()
{
#if defined( _WIN32 )
    std::vector<wchar_t> buf( MAX_PATH );

    for ( ;; )
    {
        const DWORD n = GetModuleFileNameW( NULL, buf.data(), (DWORD) buf.size() );
        if ( n == 0 )
            return "";
        if ( n < buf.size() )
            return rocketnet_test_wide_to_utf8( buf.data(), (int) n );
        if ( buf.size() >= 32768 )
            return "";
        buf.resize( buf.size() * 2 );
    }
#elif defined( __APPLE__ )
    char buf[4096];
    uint32_t size = (uint32_t) sizeof( buf );
    if ( _NSGetExecutablePath( buf, &size ) != 0 )
        return "";
    return std::string( buf );
#else
    char buf[4096];
    ssize_t n = readlink( "/proc/self/exe", buf, sizeof( buf ) - 1 );
    if ( n < 0 )
        return "";
    buf[n] = '\0';
    return std::string( buf );
#endif // #if defined( _WIN32 )
}

inline std::string suite_directory_of( const std::string & path )
{
    const std::string::size_type pos = path.find_last_of( "/\\" );
    if ( pos == std::string::npos )
        return ".";
    return path.substr( 0, pos );
}

inline bool suite_file_exists( const std::string & path )
{
#if defined( _WIN32 )
    const std::wstring wide = rocketnet_test_wide_path( path );
    if ( wide.empty() )
        return false;

    const DWORD attributes = GetFileAttributesW( wide.c_str() );
    return attributes != INVALID_FILE_ATTRIBUTES && !( attributes & FILE_ATTRIBUTE_DIRECTORY );
#else
    FILE * file = fopen( path.c_str(), "rb" );
    if ( !file )
        return false;
    fclose( file );
    return true;
#endif // #if defined( _WIN32 )
}

// The memory the suite's cases may use at once, in bytes, or 0 when unknown.
// On Linux it is the tightest memory.high or memory.max of this process's
// cgroup and its parents (a CI runner service is capped there: 10 GiB high
// and 12 GiB max on space, 2026-10-29), else MemAvailable; on macOS the
// physical memory; on Windows the available physical memory.

#if defined( __linux__ )
inline uint64_t suite_cgroup_limit_bytes( const std::string & path )
{
    uint64_t limit = 0;
    for ( const char * name : { "memory.high", "memory.max" } )
    {
        FILE * file = fopen( ( path + "/" + name ).c_str(), "r" );
        if ( !file )
            continue;
        unsigned long long value = 0;
        if ( fscanf( file, "%llu", &value ) == 1 && value > 0 && ( limit == 0 || value < limit ) )
            limit = (uint64_t) value; // "max" reads no number: no limit
        fclose( file );
    }
    return limit;
}
#endif // #if defined( __linux__ )

inline uint64_t suite_memory_budget_bytes()
{
#if defined( __linux__ )
    uint64_t budget = 0;
    FILE * file = fopen( "/proc/self/cgroup", "r" );
    if ( file )
    {
        char line[4096];
        while ( fgets( line, sizeof( line ), file ) )
        {
            if ( strncmp( line, "0::", 3 ) != 0 )
                continue; // cgroup v2 only
            std::string path = "/sys/fs/cgroup" + std::string( line + 3 );
            while ( !path.empty() && ( path.back() == '\n' || path.back() == '/' ) )
                path.pop_back();
            for ( ; path.size() > strlen( "/sys/fs/cgroup" ); path.erase( path.rfind( '/' ) ) )
            {
                const uint64_t limit = suite_cgroup_limit_bytes( path );
                if ( limit > 0 && ( budget == 0 || limit < budget ) )
                    budget = limit;
            }
        }
        fclose( file );
    }
    if ( budget > 0 )
        return budget;

    file = fopen( "/proc/meminfo", "r" );
    if ( file )
    {
        char line[256];
        unsigned long long kib = 0;
        while ( fgets( line, sizeof( line ), file ) )
            if ( sscanf( line, "MemAvailable: %llu kB", &kib ) == 1 )
                break;
        fclose( file );
        return (uint64_t) kib * 1024;
    }
    return 0;
#elif defined( __APPLE__ )
    uint64_t bytes = 0;
    size_t size = sizeof( bytes );
    return sysctlbyname( "hw.memsize", &bytes, &size, NULL, 0 ) == 0 ? bytes : 0;
#elif defined( _WIN32 )
    MEMORYSTATUSEX status;
    status.dwLength = sizeof( status );
    return GlobalMemoryStatusEx( &status ) ? (uint64_t) status.ullAvailPhys : 0;
#else
    return 0;
#endif // #if defined( __linux__ )
}

// The memory one job is planned at. The heaviest unit cases peak at 2.5 GiB
// (the 1000-client disconnect and mass-join cases) and most at a few MiB.
// Measured on space under a 10 GiB high / 12 GiB max cgroup, the Debug unit
// suite ran 48 s at 4 jobs, 36 s at 6, 44 s at 8 and 78 s at 12 (2026-10-29):
// past 6 the cgroup reclaims. 1.6 GiB a job gives 6 there and 11 under the
// rocketnet runners' 18 GiB.
#define SUITE_JOB_MEMORY_BYTES ( 1638ull * 1024 * 1024 )

// The Windows runner waits on every running case in one WaitForMultipleObjects
// call, which takes at most MAXIMUM_WAIT_OBJECTS (64) handles, from a fixed
// array of this size. On Windows the job count stops here; elsewhere it has
// no ceiling.
#define SUITE_WAIT_HANDLES 64

// One job per core, as many as the memory budget holds; a requested count
// (ROCKETNET_TEST_JOBS) sets it outright. A ceiling above zero caps the result.
inline int suite_job_count_for( unsigned cores, uint64_t budget, int requested, int ceiling )
{
    unsigned jobs = cores;
    if ( jobs == 0 )
    {
        jobs = 1;
    }

    if ( budget > 0 )
    {
        const uint64_t fit = budget / SUITE_JOB_MEMORY_BYTES;
        jobs = (unsigned) ( std::min )( (uint64_t) jobs, ( std::max )( fit, (uint64_t) 1 ) );
    }

    if ( requested > 0 )
    {
        jobs = (unsigned) requested;
    }

    if ( ceiling > 0 && jobs > (unsigned) ceiling )
    {
        jobs = (unsigned) ceiling;
    }

    return (int) jobs;
}

inline int suite_job_count()
{
    int requested = 0;
    char env[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_JOBS", env, sizeof( env ) ) )
    {
        requested = std::atoi( env );
    }

#if defined( _WIN32 )
    const int ceiling = SUITE_WAIT_HANDLES;
#else
    const int ceiling = 0;
#endif // #if defined( _WIN32 )

    return suite_job_count_for( std::thread::hardware_concurrency(), suite_memory_budget_bytes(), requested, ceiling );
}

// io_uring cases and RLIMIT_MEMLOCK (docs/LINUX.md sections 5 and 8). The
// io_uring transport never raises RLIMIT_MEMLOCK, and on kernel 7.0 the kernel
// charges every ring's locked pages to the user, across all of that user's
// processes, against the creating process's limit: io_uring cases running at
// once can refuse at create with ENOMEM though each passes alone. A suite that
// bounds them (the functional suite on Linux) runs at most
//
//     io_uring slots = floor( RLIMIT_MEMLOCK soft limit / per-case need )
//
// io_uring cases at once, capped at the job count (an unlimited limit is the
// job count); the other cases are not bounded. The per-case need is
// ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB on kernels after 6.8: twice the
// largest case measured on 7.0.0-38 (2026-10-23, the smallest `ulimit -l`
// that passes alone), test_io_uring_transport_posix_server_create_destroy_1000
// at 2064 KiB: a destroyed ring's pages are released by kernel work after
// the case has moved on, so a create and destroy loop holds several rings'
// charge, where one default 4-queue server creates at 656 KiB. On 6.8 and
// older it is ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB_6_8: a 4-queue server
// creates at 16 KiB on 6.8.0-139. The environment variable of the same name
// as the first overrides both. Zero slots with an io_uring case in the suite
// fails the suite before any case runs, naming the limit. A case is an
// io_uring case when its name says uring, or every case is when
// ROCKETNET_SERVER_TRANSPORT names io_uring. Other suites of the same user
// (other CI runners of the host) share the kernel's count, which this process
// cannot see: tools/check_memlock.sh checks the limit covers them.
#define ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB 4096
#define ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB_6_8 128

// Whether the case holds io_uring rings while it runs.

inline bool suite_case_is_io_uring( const std::string & name )
{
    char transport[256];
    if ( rocketnet_test_getenv( "ROCKETNET_SERVER_TRANSPORT", transport, sizeof( transport ) ) && std::strstr( transport, "io_uring" ) != NULL )
        return true;
    return name.find( "uring" ) != std::string::npos;
}

// The per-case need in KiB on this kernel.

inline long suite_uring_case_memlock_kib()
{
    char env[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB", env, sizeof( env ) ) )
    {
        const long requested = std::atol( env );
        if ( requested > 0 )
            return requested;
    }
#if defined( __linux__ )
    struct utsname host;
    int major = 0;
    int minor = 0;
    if ( uname( &host ) == 0 && sscanf( host.release, "%d.%d", &major, &minor ) == 2 && ( major < 6 || ( major == 6 && minor <= 8 ) ) )
        return ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB_6_8;
#endif // #if defined( __linux__ )
    return ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB;
}

// The soft RLIMIT_MEMLOCK in KiB, or -1 when unlimited (or not POSIX).

inline long suite_memlock_limit_kib()
{
#if defined( _WIN32 )
    return -1;
#else
    struct rlimit limit;
    if ( getrlimit( RLIMIT_MEMLOCK, &limit ) != 0 || limit.rlim_cur == RLIM_INFINITY )
        return -1;
    return (long) ( limit.rlim_cur / 1024 );
#endif // #if defined( _WIN32 )
}

// How many io_uring cases may run at once: the formula above, capped at jobs.

inline int suite_uring_slots( long limit_kib, long case_kib, int jobs )
{
    if ( limit_kib < 0 || case_kib <= 0 )
        return jobs;

    const long slots = limit_kib / case_kib;
    return slots < (long) jobs ? (int) slots : jobs;
}

inline int suite_usage( char ** argv )
{
    fprintf( stderr, "usage: %s [--run <case> | --list | --enforce-rss]\n", argv[0] );
    return 2;
}

inline int suite_list_registered_cases( int ( *count )(), const char * ( *name_at )(int) )
{
    const int n = count ? count() : 0;
    for ( int i = 0; i < n; i++ )
    {
        printf( "%s\n", name_at( i ) );
    }
    fflush( stdout );

    return 0;
}

// Calls emit( line ) for each line of output, without its line ending.

template <typename F> inline void suite_for_each_line( const std::string & output, F emit )
{
    size_t i = 0;
    while ( i < output.size() )
    {
        const size_t nl = output.find( '\n', i );
        std::string line = output.substr( i, nl == std::string::npos ? std::string::npos : nl - i );
        if ( !line.empty() && line.back() == '\r' )
        {
            line.pop_back();
        }
        emit( line );
        if ( nl == std::string::npos )
            break;
        i = nl + 1;
    }
}

// The bound a case process reported on its plan_rss line (tests/plan_rss.h)
// in its captured output, or -1 when it reported none.

inline long suite_reported_plan_rss_mb( const std::string & output )
{
    const char * line = PLAN_RSS_LINE;
    const size_t length = std::strlen( line );
    long reported = -1;
    suite_for_each_line( output, [line, length, &reported]( const std::string & text )
                         {
        if ( text.compare( 0, length, line ) == 0 )
        {
            const long bound = std::atol( text.c_str() + length );
            if ( bound > 0 )
                reported = bound;
        } } );

    return reported;
}

// A passing case's output keeps only its test-name lines. Returns the number
// of lines printed.

inline int suite_emit_captured( const std::string & output, bool show_all )
{
    int printed = 0;
    suite_for_each_line( output, [show_all, &printed]( const std::string & line )
                         {
        if ( show_all || suite_name_is_test_snake_case( line ) )
        {
            printf( "%s\n", line.c_str() );
            printed++;
        } } );
    fflush( stdout );

    return printed;
}

// Prints the finished cases in suite order, up to the first still running.
// A passing case prints its name (an exec case, the test names it printed); a
// failing case prints everything it wrote and, when the runner failed it
// (time, memory, a signal, an exit with no check line), one check line.
// Returns false once a failure is printed.

inline bool suite_print_ready( std::vector<suite_result> & results, size_t & printed, bool capture )
{
    while ( printed < results.size() && results[printed].done )
    {
        const suite_result & result = results[printed++];
        if ( result.failed )
        {
            suite_emit_captured( result.output, true );
            if ( result.output.find( "check failed" ) == std::string::npos )
            {
                printf( "check failed: %s\n", result.detail.c_str() );
            }
            fflush( stdout );
            return false;
        }

        if ( capture && ( !result.exec || suite_emit_captured( result.output, false ) == 0 ) )
        {
            printf( "%s\n", result.name.c_str() );
        }
        if ( result.rss_over_bound )
        {
            printf( "%s\n", result.detail.c_str() );
        }
    }

    fflush( stdout );

    return true;
}

// ROCKETNET_TEST_REPORT=1: the slowest cases and the peak resident sets, as
// one labelled block after the results.

inline void suite_print_report( const std::vector<suite_result> & results )
{
    if ( !suite_env_is_set( "ROCKETNET_TEST_REPORT" ) )
        return;

    std::vector<suite_result> ran;
    for ( size_t i = 0; i < results.size(); i++ )
        if ( results[i].done )
            ran.push_back( results[i] );
    std::sort( ran.begin(), ran.end(), []( const suite_result & a, const suite_result & b )
               { return a.seconds > b.seconds; } );
    // ROCKETNET_TEST_REPORT=all lists every case, the per-commit budget's input
    char report[16] = { 0 };
    rocketnet_test_getenv( "ROCKETNET_TEST_REPORT", report, sizeof( report ) );
    const size_t listed = std::strcmp( report, "all" ) == 0 ? ran.size() : 10;
    printf( "\nslowest cases:\n" );
    for ( size_t i = 0; i < ran.size() && i < listed; i++ )
    {
        printf( "  %8.3fs  %s\n", ran[i].seconds, ran[i].name.c_str() );
    }
    std::sort( ran.begin(), ran.end(), []( const suite_result & a, const suite_result & b )
               { return a.peak_rss_mb > b.peak_rss_mb; } );
    printf( "peak rss:\n" );

    int printed = 0;
    for ( size_t i = 0; i < ran.size() && printed < 10; i++ )
        if ( ran[i].peak_rss_mb >= 0 && ran[i].check_rss )
        {
            printf( "  %6ld MiB (bound %ld)  %s\n", ran[i].peak_rss_mb, suite_case_rss_mb( ran[i].name, ran[i].plan_rss_mb ), ran[i].name.c_str() );
            printed++;
        }
    fflush( stdout );
}

// The cases in suite order, or false (after printing why) when a name is not
// test_ snake_case or an exec binary is missing.

inline bool suite_collect( std::vector<suite_case> & pending, std::vector<suite_result> & results,
                           int ( *count )(), const char * ( *name_at )(int), const suite_exec * execs, int nexecs,
                           int ( *group_of )( const char * name ), const std::string & dir, const char * exe_suffix )
{
    // Each case is built in place from its name's characters, never copied
    // or moved as a whole string: under MemorySanitizer the prebuilt
    // libstdc++ copies a short (SSO) string without telling the shadow, so a
    // copied name reads as uninitialised (tools/run_msan.sh).
    // A registered case group_of marks SUITE_GROUP_NIGHTLY is not collected
    // outside the nightly tier.

    const size_t first = pending.size();
    const bool nightly = suite_nightly();
    int num_registered = 0;
    for ( int i = 0; i < count(); i++ )
        if ( nightly || !group_of || ( group_of( name_at( i ) ) & SUITE_GROUP_NIGHTLY ) == 0 )
            num_registered++;
    pending.resize( first + (size_t) num_registered + (size_t) nexecs );

    for ( int i = 0, kept = 0; i < count(); i++ )
    {
        if ( !nightly && group_of && ( group_of( name_at( i ) ) & SUITE_GROUP_NIGHTLY ) != 0 )
            continue;
        suite_case & item = pending[first + (size_t) kept++];
        item.name = name_at( i );
        item.check_rss = true;
        item.group = 0;
    }

    for ( int i = 0; i < nexecs; i++ )
    {
        suite_case & item = pending[first + (size_t) num_registered + (size_t) i];
        item.name = execs[i].name;
        item.exec_path = dir + "/" + execs[i].file + exe_suffix;
        item.exec_argument = execs[i].argument ? execs[i].argument : "";
        item.check_rss = execs[i].check_rss;
        item.group = 0;
        if ( !suite_file_exists( item.exec_path ) )
        {
            printf( "check failed: %s missing at %s\n", execs[i].name, item.exec_path.c_str() );
            return false;
        }
    }

    for ( size_t i = 0; i < pending.size(); i++ )
    {
        if ( !suite_name_is_test_snake_case( pending[i].name ) )
        {
            printf( "check failed: test name %s is not test_ lowercase snake_case\n", pending[i].name.c_str() );
            return false;
        }
    }

    suite_case_rss_bound_registry_t * rss_registry = suite_case_rss_bound_registry();
    for ( int i = 0; i < rss_registry->count; i++ )
    {
        suite_case_rss_bound_t * entry = &rss_registry->entries[i];
        if ( !suite_name_is_test_snake_case( entry->name ) )
        {
            printf( "check failed: SUITE_CASE_RSS_BOUND name %s is not test_ lowercase snake_case\n", entry->name );
            return false;
        }
    }

    // group_of (NULL: every case is group 0) keeps cases that measure wall
    // time apart from cases that load the host: the cases run in ascending
    // group order, and a case starts only while every running case is of its
    // own group, so a group runs in parallel within itself and alone. Within
    // a group the cases run in layer order (suite_layer_of).

    for ( size_t i = 0; i < pending.size(); i++ )
    {
        pending[i].group = group_of ? ( group_of( pending[i].name.c_str() ) & ~SUITE_GROUP_NIGHTLY ) : 0;
    }
    std::stable_sort( pending.begin(), pending.end(), []( const suite_case & a, const suite_case & b )
                      {
        if ( a.group != b.group )
            return a.group < b.group;
        return suite_layer_of( a.name ) < suite_layer_of( b.name ); } );

    for ( size_t i = 0; i < pending.size(); i++ )
    {
        suite_result result;
        result.name = pending[i].name;
        result.exec = !pending[i].exec_path.empty();
        result.done = false;
        result.seconds = 0;
        result.failed = false;
        result.peak_rss_mb = -1;
        result.plan_rss_mb = -1;
        result.rss_over_bound = false;
        result.check_rss = pending[i].check_rss;
        results.push_back( result );
    }

    return true;
}

// Rows a suite omitted because their runs_here predicate is false on this
// host. A suite that filters its table by predicate (the functional suite)
// notes each omitted row here; suite_finish then prints one SKIPPED line
// per row and the skipped count, so an omitted row is never silent. Suites
// that never note a row print nothing extra.
struct suite_skipped_row
{
    std::string name;
    std::string predicate;
};

inline std::vector<suite_skipped_row> & suite_skipped_rows()
{
    static std::vector<suite_skipped_row> rows;
    return rows;
}

inline bool & suite_skipped_reporting()
{
    static bool enabled = false;
    return enabled;
}

inline void suite_enable_skipped_reporting()
{
    suite_skipped_reporting() = true;
}

inline void suite_note_skipped( const char * name, const char * predicate )
{
    suite_enable_skipped_reporting();

    suite_skipped_row row;
    row.name = name;
    row.predicate = predicate;
    suite_skipped_rows().push_back( row );
}

inline void suite_print_skipped()
{
    if ( !suite_skipped_reporting() )
        return;
    const std::vector<suite_skipped_row> & rows = suite_skipped_rows();
    for ( size_t i = 0; i < rows.size(); i++ )
    {
        printf( "SKIPPED %s: %s\n", rows[i].name.c_str(), rows[i].predicate.c_str() );
    }
    printf( "skipped: %d\n", (int) rows.size() );
    fflush( stdout );
}

inline int suite_finish( const std::vector<suite_result> & results, bool passed )
{
    suite_print_report( results );
    suite_print_skipped();
    if ( !passed )
        return 1;
    printf( "\n*** ALL TESTS PASSED ***\n\n" );
    fflush( stdout );

    return 0;
}

// A case that declares SUITE_CASE_RSS_BOUND is a full-capacity server: its
// memory plan allocates every slot up front, and where that plan is charged
// as it is allocated rather than as it is touched, such a case runs alone.
//
// Windows charges committed memory against the system commit limit when it is
// allocated, not when it is touched, so one such case commits tens of GiB, and
// two at once (or one with the cases beside it) can pass the limit: every
// allocation in every running case can then fail, wherever it is (a world that
// does not create, a connection that comes back NULL, a case that does not
// spawn).
//
// MemorySanitizer writes shadow and origin bytes for the blocks it poisons, so
// a planned block is resident in shadow though the case never touches it. The
// full-capacity cases keep their largest blocks unpoisoned
// (tests/msan_scale_allocator.h), and each still peaks at 3 to 6 GiB; five of
// them at once filled a 20 GiB runner and stalled every case beside them past
// the 600 s case limit (amd-2, 2026-10-28). Under MSan such a case runs alone.

#if defined( __has_feature )
#if __has_feature( memory_sanitizer )
#define SUITE_RSS_BOUND_CASES_RUN_ALONE 1
#endif // #if __has_feature( memory_sanitizer )
#endif // #if defined( __has_feature )
#if defined( _WIN32 )
#define SUITE_RSS_BOUND_CASES_RUN_ALONE 1
#endif // #if defined( _WIN32 )

inline bool suite_case_runs_alone( const std::string & name )
{
#if defined( SUITE_RSS_BOUND_CASES_RUN_ALONE )
    suite_case_rss_bound_registry_t * registry = suite_case_rss_bound_registry();
    for ( int i = 0; i < registry->count; i++ )
        if ( name == registry->entries[i].name )
            return true;
#else // #if defined( SUITE_RSS_BOUND_CASES_RUN_ALONE )
    (void) name;
#endif // #if defined( SUITE_RSS_BOUND_CASES_RUN_ALONE )

    return false;
}

#if !defined( _WIN32 )

inline void suite_drain( int fd, std::string & output )
{
    if ( fd < 0 )
        return;

    char buf[4096];

    for ( ;; )
    {
        const ssize_t n = ::read( fd, buf, sizeof( buf ) );
        if ( n > 0 )
        {
            output.append( buf, (size_t) n );
            continue;
        }

        if ( n < 0 && errno == EINTR )
            continue;
        break;
    }
}

// Runs one case in a child with its stdout and stderr read through a pipe,
// then prints every line when it failed and only the name lines when it
// passed. The child may exit(1), abort or be killed and the parent still has
// every line it wrote. This is the same names-only shape the suite applies to
// its children, so `--run <case>` does not leak a passing case's diagnostics
// without ROCKETNET_TEST_VERBOSE.

inline int suite_run_names_only( int ( *worker )( const char * ), const char * name )
{
    fflush( stdout );
    fflush( stderr );

    int pipefd[2];
    if ( pipe( pipefd ) != 0 )
        return worker( name );

    const pid_t pid = fork();
    if ( pid < 0 )
    {
        close( pipefd[0] );
        close( pipefd[1] );
        return worker( name );
    }

    if ( pid == 0 )
    {
        close( pipefd[0] );
        if ( dup2( pipefd[1], STDOUT_FILENO ) < 0 || dup2( pipefd[1], STDERR_FILENO ) < 0 )
        {
            _exit( 127 );
        }
        if ( pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO )
        {
            close( pipefd[1] );
        }

        const int rc = worker( name );
        fflush( stdout );
        fflush( stderr );
        _exit( rc );
    }

    close( pipefd[1] );

    std::string output;
    suite_drain( pipefd[0], output );
    close( pipefd[0] );

    int status = 0;
    pid_t waited;

    do
    {
        waited = waitpid( pid, &status, 0 );
    } while ( waited < 0 && errno == EINTR );

    int rc = 1;
    if ( waited >= 0 && WIFEXITED( status ) )
        rc = WEXITSTATUS( status );
    else if ( waited >= 0 && WIFSIGNALED( status ) )
        rc = 128 + WTERMSIG( status );
    suite_emit_captured( output, rc != 0 );

    return rc;
}

inline int suite_main( int argc, char ** argv,
                       int ( *worker )( const char * name ),
                       int ( *count )(),
                       const char * ( *name_at )(int),
                       const suite_exec * execs, int nexecs,
                       int ( *group_of )( const char * name ) = NULL,
                       double case_limit_s = 120.0 * ROCKETNET_TEST_CASE_TIME_SCALE,
                       int max_jobs = 0,
                       bool bound_io_uring = false,
                       bool enforce_case_rss = true )
{
    if ( argc == 2 && std::strcmp( argv[1], "--list" ) == 0 )
        return suite_list_registered_cases( count, name_at );

    if ( argc == 2 && std::strcmp( argv[1], "--enforce-rss" ) == 0 )
    {
        enforce_case_rss = true;
        argc = 1;
    }

    if ( argc >= 2 && std::strcmp( argv[1], "--run" ) == 0 )
    {
        if ( argc < 3 || argv[2][0] == '-' )
            return suite_usage( argv );
        for ( int i = 3; i < argc; i++ )
            if ( argv[i][0] == '-' )
                return suite_usage( argv );

        // under a suite run the parent already captures this process, so
        // stream straight through; a second capture would lose lines when the
        // parent kills the process group at the time limit

        if ( suite_env_is_set( "ROCKETNET_TEST_SUITE_CAPTURE" ) )
        {
            unsetenv( "ROCKETNET_TEST_SUITE_CAPTURE" );
            return worker( argv[2] );
        }

        if ( !suite_env_is_set( "ROCKETNET_TEST_VERBOSE" ) )
            return suite_run_names_only( worker, argv[2] );
        return worker( argv[2] );
    }

    for ( int i = 1; i < argc; i++ )
        if ( argv[i][0] == '-' )
            return suite_usage( argv );

    const std::string self = suite_executable_path();
    if ( self.empty() )
    {
        printf( "check failed: could not resolve this executable\n" );
        return 1;
    }

    std::vector<suite_case> pending;
    std::vector<suite_result> results;
    if ( !suite_collect( pending, results, count, name_at, execs, nexecs, group_of, suite_directory_of( self ), "" ) )
        return 1;

    const int jobs = max_jobs > 0 ? ( std::min )( suite_job_count(), max_jobs ) : suite_job_count();
    double limit_s = case_limit_s;
    if ( !suite_case_limit( case_limit_s, &limit_s ) )
        return 1;

    int uring_slots = jobs;

    if ( bound_io_uring )
    {
        const long limit_kib = suite_memlock_limit_kib();
        const long case_kib = suite_uring_case_memlock_kib();
        uring_slots = suite_uring_slots( limit_kib, case_kib, jobs );
        for ( size_t i = 0; uring_slots == 0 && i < pending.size(); i++ )
        {
            if ( suite_case_is_io_uring( pending[i].name ) )
            {
                printf( "check failed: RLIMIT_MEMLOCK %ld KiB is under one io_uring case's %ld KiB (%s); raise the limit (tools/check_memlock.sh, docs/LINUX.md section 8)\n",
                        limit_kib, case_kib, pending[i].name.c_str() );
                return 1;
            }
        }
    }

    int uring_running = 0;
    struct running_t
    {
        size_t index;
        pid_t pid;
        std::chrono::steady_clock::time_point start;
        bool killed;
        int group;
        int out_fd;
        bool uring;
        bool alone;
    };
    const bool capture = !suite_env_is_set( "ROCKETNET_TEST_VERBOSE" );
    if ( capture )
    {
        setenv( "ROCKETNET_TEST_SUITE_CAPTURE", "1", 1 );
    }

    std::vector<running_t> running;
    size_t next = 0;
    size_t printed = 0;
    bool passed = true;
    while ( passed && ( next < pending.size() || !running.empty() ) )
    {
        while ( (int) running.size() < jobs && next < pending.size() &&
                ( running.empty() || ( running[0].group == pending[next].group && !running[0].alone && !suite_case_runs_alone( pending[next].name ) ) ) &&
                !( bound_io_uring && uring_running >= uring_slots && suite_case_is_io_uring( pending[next].name ) ) )
        {
            const size_t index = next++;
            const suite_case & item = pending[index];
            suite_result & result = results[index];

            const std::string path = item.exec_path.empty() ? self : item.exec_path;
            std::vector<char> arg0( path.begin(), path.end() );
            arg0.push_back( '\0' );

            std::vector<char> arg_run;
            std::vector<char> arg_name;
            std::vector<char> exec_argument;
            char * spawn_argv[5];
            spawn_argv[0] = arg0.data();

            if ( item.exec_path.empty() )
            {
                arg_run.assign( 6, '\0' );
                std::memcpy( arg_run.data(), "--run", 6 );
                arg_name.assign( item.name.begin(), item.name.end() );
                arg_name.push_back( '\0' );
                spawn_argv[1] = arg_run.data();
                spawn_argv[2] = arg_name.data();
                spawn_argv[3] = NULL;
            }
            else
            {
                if ( item.exec_argument.empty() )
                {
                    spawn_argv[1] = NULL;
                }
                else
                {
                    exec_argument.assign( item.exec_argument.begin(), item.exec_argument.end() );
                    exec_argument.push_back( '\0' );
                    spawn_argv[1] = exec_argument.data();
                    spawn_argv[2] = NULL;
                }
            }

            int pipefd[2] = { -1, -1 };
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_t * actionsp = NULL;
            bool actions_live = false;

            if ( capture )
            {
                if ( pipe( pipefd ) != 0 )
                {
                    result.done = true;
                    result.failed = true;
                    result.detail = item.name + " failed to spawn";
                    continue;
                }

                fcntl( pipefd[0], F_SETFD, FD_CLOEXEC );

                int fl = fcntl( pipefd[0], F_GETFL, 0 );
                if ( fl >= 0 )
                {
                    fcntl( pipefd[0], F_SETFL, fl | O_NONBLOCK );
                }
                posix_spawn_file_actions_init( &actions );
                actions_live = true;
                posix_spawn_file_actions_adddup2( &actions, pipefd[1], STDOUT_FILENO );
                posix_spawn_file_actions_adddup2( &actions, pipefd[1], STDERR_FILENO );
                posix_spawn_file_actions_addclose( &actions, pipefd[0] );
                posix_spawn_file_actions_addclose( &actions, pipefd[1] );
                actionsp = &actions;
            }

            posix_spawnattr_t attr;
            posix_spawnattr_init( &attr );
            posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETPGROUP );
            posix_spawnattr_setpgroup( &attr, 0 );

            pid_t pid = 0;
            const int spawned = posix_spawn( &pid, path.c_str(), actionsp, &attr, spawn_argv, environ );
            posix_spawnattr_destroy( &attr );
            if ( actions_live )
            {
                posix_spawn_file_actions_destroy( &actions );
            }
            if ( pipefd[1] >= 0 )
            {
                close( pipefd[1] );
            }

            if ( spawned != 0 )
            {
                if ( pipefd[0] >= 0 )
                {
                    close( pipefd[0] );
                }
                result.done = true;
                result.failed = true;
                result.detail = item.name + " failed to spawn";
                continue;
            }

            running_t child;
            child.index = index;
            child.pid = pid;
            child.start = std::chrono::steady_clock::now();
            child.killed = false;
            child.group = item.group;
            child.out_fd = pipefd[0];
            child.uring = bound_io_uring && suite_case_is_io_uring( item.name );
            child.alone = suite_case_runs_alone( item.name );
            uring_running += child.uring ? 1 : 0;
            running.push_back( child );
        }

        for ( size_t i = 0; i < running.size(); )
        {
            suite_result & result = results[running[i].index];

            int status = 0;
            struct rusage usage;
            std::memset( &usage, 0, sizeof( usage ) );

            const pid_t got = wait4( running[i].pid, &status, WNOHANG, &usage );
            const double seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - running[i].start ).count();

            if ( got == 0 )
            {
                suite_drain( running[i].out_fd, result.output );

                if ( !running[i].killed && seconds > limit_s )
                {
                    kill( -running[i].pid, SIGKILL );
                    kill( running[i].pid, SIGKILL );
                    running[i].killed = true;
                }

                i++;
                continue;
            }

            result.seconds = seconds;
#if defined( __APPLE__ )
            result.peak_rss_mb = got > 0 ? (long) ( usage.ru_maxrss / ( 1024 * 1024 ) ) : -1; // bytes on macOS
#else
            result.peak_rss_mb = got > 0 ? (long) ( usage.ru_maxrss / 1024 ) : -1; // kilobytes on Linux
#endif // #if defined( __APPLE__ )
            char text[64];

            if ( got < 0 )
            {
                result.failed = true;
                result.detail = result.name + " wait failed";
            }
            else if ( running[i].killed || seconds > limit_s )
            {
                result.failed = true;
                snprintf( text, sizeof( text ), " exceeded %.0fs", limit_s );
                result.detail = result.name + text;
            }
            else if ( WIFEXITED( status ) && WEXITSTATUS( status ) != 0 )
            {
                result.failed = true;
                snprintf( text, sizeof( text ), " exit %d", WEXITSTATUS( status ) );
                result.detail = result.name + text;
            }
            else if ( !WIFEXITED( status ) )
            {
                result.failed = true;
                snprintf( text, sizeof( text ), " signal %d", WIFSIGNALED( status ) ? WTERMSIG( status ) : 0 );
                result.detail = result.name + text;
            }

            if ( running[i].out_fd >= 0 )
            {
                suite_drain( running[i].out_fd, result.output );
                close( running[i].out_fd );
            }

            result.plan_rss_mb = suite_reported_plan_rss_mb( result.output );
            if ( result.check_rss )
            {
                suite_check_rss( result, suite_case_rss_mb( result.name, result.plan_rss_mb ), enforce_case_rss );
            }
            result.done = true;
            uring_running -= running[i].uring ? 1 : 0;
            running.erase( running.begin() + (std::ptrdiff_t) i );
        }

        passed = suite_print_ready( results, printed, capture );
        if ( passed && !running.empty() )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }
    }

    // a failure ends the run: the cases still running are killed and reaped

    for ( size_t i = 0; i < running.size(); i++ )
    {
        kill( -running[i].pid, SIGKILL );
        kill( running[i].pid, SIGKILL );

        int status = 0;
        while ( waitpid( running[i].pid, &status, 0 ) < 0 && errno == EINTR )
        {
        }

        if ( running[i].out_fd >= 0 )
        {
            close( running[i].out_fd );
        }
    }

    return suite_finish( results, passed );
}

#else

inline void suite_drain( HANDLE pipe, std::string & output )
{
    if ( pipe == NULL )
        return;

    char buf[4096];

    for ( ;; )
    {
        DWORD available = 0;
        if ( !PeekNamedPipe( pipe, NULL, 0, NULL, &available, NULL ) || available == 0 )
            return;

        DWORD n = 0;
        if ( !ReadFile( pipe, buf, available < sizeof( buf ) ? available : (DWORD) sizeof( buf ), &n, NULL ) || n == 0 )
            return;
        output.append( buf, (size_t) n );
    }
}

// A spawn failure names the call and its Windows error code, so a red that
// never started its case says why (1455 is the commit limit, 8 and 1450 are
// out of memory or system resources).

inline std::string suite_spawn_failure_text( const char * call, DWORD error )
{
    char text[96];
    snprintf( text, sizeof( text ), " failed to spawn (%s error %lu)", call, (unsigned long) error );
    return text;
}

inline int suite_main( int argc, char ** argv,
                       int ( *worker )( const char * name ),
                       int ( *count )(),
                       const char * ( *name_at )(int),
                       const suite_exec * execs, int nexecs,
                       int ( *group_of )( const char * name ) = NULL,
                       double case_limit_s = 120.0 * ROCKETNET_TEST_CASE_TIME_SCALE,
                       int max_jobs = 0,
                       bool bound_io_uring = false,
                       bool enforce_case_rss = true )
{
    if ( argc == 2 && std::strcmp( argv[1], "--list" ) == 0 )
        return suite_list_registered_cases( count, name_at );

    if ( argc == 2 && std::strcmp( argv[1], "--enforce-rss" ) == 0 )
    {
        enforce_case_rss = true;
        argc = 1;
    }

    if ( argc >= 2 && std::strcmp( argv[1], "--run" ) == 0 )
    {
        if ( argc < 3 || argv[2][0] == '-' )
            return suite_usage( argv );
        for ( int i = 3; i < argc; i++ )
            if ( argv[i][0] == '-' )
                return suite_usage( argv );
        return worker( argv[2] );
    }

    for ( int i = 1; i < argc; i++ )
        if ( argv[i][0] == '-' )
            return suite_usage( argv );

    const std::string self = suite_executable_path();
    if ( self.empty() )
    {
        printf( "check failed: could not resolve this executable\n" );
        return 1;
    }

    std::vector<suite_case> pending;
    std::vector<suite_result> results;
    if ( !suite_collect( pending, results, count, name_at, execs, nexecs, group_of, suite_directory_of( self ), ".exe" ) )
        return 1;

    const int jobs = max_jobs > 0 ? ( std::min )( suite_job_count(), max_jobs ) : suite_job_count();
    double limit_s = case_limit_s;
    if ( !suite_case_limit( case_limit_s, &limit_s ) )
        return 1;
    (void) bound_io_uring; // no io_uring on Windows

    struct running_t
    {
        size_t index;
        HANDLE process;
        std::chrono::steady_clock::time_point start;
        bool killed;
        int group;
        bool alone;
        HANDLE out;
    };
    const bool capture = !suite_env_is_set( "ROCKETNET_TEST_VERBOSE" );
    std::vector<running_t> running;
    size_t next = 0;
    size_t printed = 0;
    bool passed = true;
    while ( passed && ( next < pending.size() || !running.empty() ) )
    {
        while ( (int) running.size() < jobs && next < pending.size() &&
                ( running.empty() || ( running[0].group == pending[next].group && !running[0].alone && !suite_case_runs_alone( pending[next].name ) ) ) )
        {
            const size_t index = next++;
            const suite_case & item = pending[index];
            suite_result & result = results[index];

            const std::string path = item.exec_path.empty() ? self : item.exec_path;
            const std::wstring wide_path = rocketnet_test_wide_path( path );
            std::wstring command = L"\"" + wide_path + L"\"";
            if ( item.exec_path.empty() )
                command += L" --run " + rocketnet_test_utf8_to_wide( item.name );
            else if ( !item.exec_argument.empty() )
                command += L" " + rocketnet_test_utf8_to_wide( item.exec_argument );
            std::vector<wchar_t> mutable_command( command.begin(), command.end() );
            mutable_command.push_back( L'\0' );

            STARTUPINFOW startup;
            std::memset( &startup, 0, sizeof( startup ) );
            startup.cb = sizeof( startup );

            HANDLE read_end = NULL;
            HANDLE write_end = NULL;

            if ( capture )
            {
                SECURITY_ATTRIBUTES inherit;
                std::memset( &inherit, 0, sizeof( inherit ) );
                inherit.nLength = sizeof( inherit );
                inherit.bInheritHandle = TRUE;
                if ( !CreatePipe( &read_end, &write_end, &inherit, 1 << 20 ) || !SetHandleInformation( read_end, HANDLE_FLAG_INHERIT, 0 ) )
                {
                    const DWORD error = GetLastError();
                    if ( read_end )
                    {
                        CloseHandle( read_end );
                    }
                    if ( write_end )
                    {
                        CloseHandle( write_end );
                    }
                    result.done = true;
                    result.failed = true;
                    result.detail = item.name + suite_spawn_failure_text( "CreatePipe", error );
                    continue;
                }

                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
                startup.hStdOutput = write_end;
                startup.hStdError = write_end;
            }

            PROCESS_INFORMATION info;
            std::memset( &info, 0, sizeof( info ) );

            const BOOL created = !wide_path.empty() && CreateProcessW( wide_path.c_str(), mutable_command.data(), NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &startup, &info );
            const DWORD error = created ? 0 : GetLastError();
            if ( write_end )
            {
                CloseHandle( write_end );
            }

            if ( !created )
            {
                if ( read_end )
                {
                    CloseHandle( read_end );
                }
                result.done = true;
                result.failed = true;
                result.detail = item.name + suite_spawn_failure_text( "CreateProcessW", error );
                continue;
            }

            CloseHandle( info.hThread );

            running_t child;
            child.index = index;
            child.process = info.hProcess;
            child.start = std::chrono::steady_clock::now();
            child.killed = false;
            child.group = item.group;
            child.alone = suite_case_runs_alone( item.name );
            child.out = read_end;
            running.push_back( child );
        }

        if ( !running.empty() )
        {
            static_assert( SUITE_WAIT_HANDLES == MAXIMUM_WAIT_OBJECTS, "the wait set is one WaitForMultipleObjects call" );
            HANDLE handles[SUITE_WAIT_HANDLES];
            const size_t n = ( std::min )( running.size(), (size_t) SUITE_WAIT_HANDLES );
            for ( size_t i = 0; i < n; i++ )
            {
                handles[i] = running[i].process;
            }
            WaitForMultipleObjects( (DWORD) n, handles, FALSE, 50 );
        }

        for ( size_t i = 0; i < running.size(); )
        {
            suite_result & result = results[running[i].index];
            suite_drain( running[i].out, result.output );

            const double seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - running[i].start ).count();
            const DWORD wait = WaitForSingleObject( running[i].process, 0 );
            if ( wait == WAIT_TIMEOUT )
            {
                if ( !running[i].killed && seconds > limit_s )
                {
                    TerminateProcess( running[i].process, 124 );
                    running[i].killed = true;
                }

                i++;
                continue;
            }

            DWORD code = 1;
            GetExitCodeProcess( running[i].process, &code );

            PROCESS_MEMORY_COUNTERS memory;
            std::memset( &memory, 0, sizeof( memory ) );
            memory.cb = sizeof( memory );

            const bool measured = GetProcessMemoryInfo( running[i].process, &memory, sizeof( memory ) ) != 0;
            CloseHandle( running[i].process );

            if ( running[i].out )
            {
                suite_drain( running[i].out, result.output );
                CloseHandle( running[i].out );
            }

            result.seconds = seconds;
            result.peak_rss_mb = measured ? (long) ( memory.PeakWorkingSetSize / ( 1024 * 1024 ) ) : -1;

            char text[64];

            if ( running[i].killed || seconds > limit_s )
            {
                result.failed = true;
                snprintf( text, sizeof( text ), " exceeded %.0fs", limit_s );
                result.detail = result.name + text;
            }
            else if ( code != 0 )
            {
                result.failed = true;
                snprintf( text, sizeof( text ), " exit %lu", (unsigned long) code );
                result.detail = result.name + text;
            }

            result.plan_rss_mb = suite_reported_plan_rss_mb( result.output );
            if ( result.check_rss )
            {
                suite_check_rss( result, suite_case_rss_mb( result.name, result.plan_rss_mb ), enforce_case_rss );
            }
            result.done = true;
            running.erase( running.begin() + (std::ptrdiff_t) i );
        }

        passed = suite_print_ready( results, printed, capture );
    }

    // a failure ends the run: the cases still running are ended

    for ( size_t i = 0; i < running.size(); i++ )
    {
        TerminateProcess( running[i].process, 124 );
        WaitForSingleObject( running[i].process, INFINITE );
        CloseHandle( running[i].process );
        if ( running[i].out )
        {
            CloseHandle( running[i].out );
        }
    }

    return suite_finish( results, passed );
}

#endif // #if !defined( _WIN32 )

#endif // ROCKETNET_TESTS_SUITE_RUNNER_H
