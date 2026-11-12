/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Suite runner. Each test is a process, and cases run in parallel, one
    job per core. The output is one line per test, its name, in suite
    order, then
    "*** ALL TESTS PASSED ***". A failing test prints its own output (its
    check line) and the suite exits 1 at once. A test over 120s is killed and
    fails; `--run <case>` on its own is held to the same limit and names the
    case ("check failed: <case> exceeded Ns (--run)"). The functional runner keeps 120s in every build; the unit runner
    scales it for Debug, sanitizer or emulated builds (tests/time_scale.h).
    ROCKETNET_TEST_CASE_TIME_SCALE in the environment multiplies the case limit
    a suite passes in; the nightly macOS TSAN functional runs set 30 (one hour),
    the nightly Linux threaded TSan leg 4, the nightly Linux Debug and LSan
    functional runs 2, and every other run leaves it unset. ROCKETNET_NIGHTLY_NO_TIMEOUT=1
    disables only this execution watchdog; fixture durations and assertions stay intact. A
    functional case whose peak resident set (wait4 ru_maxrss on POSIX,
    PeakWorkingSetSize on Windows) is over its bound fails
    (ROCKETNET_TEST_MAX_CASE_RSS_MB, overridable by that environment variable;
    a sanitizer build does not enforce it unless the variable sets one). A case
    whose subject is a full-capacity server declares its own bound with
    SUITE_CASE_RSS_BOUND. A case process that counts the SDK's bytes reports
    its own bound, its plan's peak plus a fixed slack (tests/plan_rss.h), and
    is held to that instead. Every registered name is test_ lowercase snake_case,
    or the suite fails before running anything. ROCKETNET_TEST_REPORT=1 prints
    the job count before the cases, and the slowest cases and the peak
    resident sets after the results.
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

// LeakSanitizer (alone or inside AddressSanitizer): a case child that ends
// with _exit skips the exit-time leak check, so it runs the check itself
// (suite_run_names_only).
#if defined( __SANITIZE_ADDRESS__ )
#define SUITE_RUNNER_LSAN 1
#elif defined( __has_feature )
#if __has_feature( address_sanitizer ) || __has_feature( leak_sanitizer )
#define SUITE_RUNNER_LSAN 1
#endif // #if __has_feature( address_sanitizer ) || __has_feature( leak_sanitizer )
#endif // #if defined( __SANITIZE_ADDRESS__ )
#if defined( SUITE_RUNNER_LSAN ) && !defined( _WIN32 )
#include <sanitizer/lsan_interface.h>
#endif // #if defined( SUITE_RUNNER_LSAN ) && !defined( _WIN32 )

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined( __APPLE__ )
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif // #if defined( __APPLE__ )

#if defined( __linux__ )
#include <sched.h>
#endif // #if defined( __linux__ )

#if defined( _WIN32 )
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // #ifndef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#if defined( _MSC_VER )
#include <crtdbg.h>
#endif // #if defined( _MSC_VER )
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

// ROCKETNET_TEST_KEEP_GOING=1, on the nightly tier only: a failing case no
// longer ends the run. Every collected case runs; each failure prints as it
// would, and the summary names each failed case and counts the cases run
// against the cases collected. The suite still exits 1 on any failure. A
// per-commit run stops at the first failure, so the variable without
// ROCKETNET_TEST_NIGHTLY=1 fails the suite before any case runs.

inline bool suite_keep_going_requested()
{
    char value[16];
    return rocketnet_test_getenv( "ROCKETNET_TEST_KEEP_GOING", value, sizeof( value ) ) && std::strcmp( value, "1" ) == 0;
}

inline bool suite_keep_going()
{
    return suite_keep_going_requested() && suite_nightly();
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

    char no_timeout[16];
    const bool unbounded = rocketnet_test_getenv( "ROCKETNET_NIGHTLY_NO_TIMEOUT", no_timeout, sizeof( no_timeout ) ) &&
                           std::strcmp( no_timeout, "1" ) == 0;
    *limit_s = unbounded ? std::numeric_limits<double>::infinity() : case_limit_s * scale;

    return true;
}

// The case limit for `<suite> --run <case>` run on its own (a script, the
// transport matrix, a bench): the same limit a suite run holds the case to,
// from a thread in the case's own process, which prints the case's name and
// exits 1 when it passes. Infinite (ROCKETNET_NIGHTLY_NO_TIMEOUT=1) starts no
// thread. A case the suite runs needs none: its parent kills it at the limit.

inline void suite_run_watchdog_thread( std::string case_name, double limit_s )
{
    std::this_thread::sleep_for( std::chrono::duration<double>( limit_s ) );
    printf( "check failed: %s exceeded %.0fs (--run)\n", case_name.c_str(), limit_s );
    fflush( stdout );
    std::_Exit( 1 );
}

inline void suite_run_watchdog( const char * name, double limit_s )
{
    if ( !( limit_s < std::numeric_limits<double>::infinity() ) )
    {
        return;
    }
    std::thread( suite_run_watchdog_thread, std::string( name ), limit_s ).detach();
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
        {
            return requested;
        }
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
    {
        memmove( &registry->entries[1], &registry->entries[0],
                 (size_t) registry->count * sizeof( registry->entries[0] ) );
    }
    registry->entries[0].name = name;
    registry->entries[0].bound_mb = bound_mb;
    registry->count++;

    return true;
}

// The first entry under a name (the latest registered), or NULL.

inline suite_case_rss_bound_t * suite_case_rss_bound_find( suite_case_rss_bound_registry_t * registry, const char * name )
{
    for ( int i = 0; i < registry->count; i++ )
    {
        if ( strcmp( registry->entries[i].name, name ) == 0 )
        {
            return &registry->entries[i];
        }
    }
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
    {
        return 0;
    }

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

// A case passes only when its function returns 0. A case that ends the
// process itself never returns: exit( 0 ) ahead of its checks would otherwise
// leave with status 0 and pass in every entry (--run, the suite's children,
// shards). While a case runs, an atexit handler stands guard: if the process
// that started the case leaves through exit() before the case returned, the
// handler prints why and ends the process with status 1. A failed check's
// exit( 1 ) still ends with 1; abort and signals never run atexit handlers.
// A child the case forks keeps its own exit status (the pid is compared).

struct suite_case_guard_state
{
    const char * name;
    long pid;
    bool running;
    bool registered;
};

inline suite_case_guard_state & suite_case_guard()
{
    static suite_case_guard_state state = { NULL, 0, false, false };
    return state;
}

inline long suite_process_id()
{
#if defined( _WIN32 )
    return (long) GetCurrentProcessId();
#else
    return (long) getpid();
#endif // #if defined( _WIN32 )
}

// true when an exit() now, from process pid, must fail the case: a case is
// running and this is the process that started it
inline bool suite_case_exit_fails( const suite_case_guard_state & state, long pid )
{
    return state.running && state.pid == pid;
}

inline void suite_case_exit_guard()
{
    const suite_case_guard_state & state = suite_case_guard();
    if ( suite_case_exit_fails( state, suite_process_id() ) )
    {
        printf( "check failed: %s left through exit() before it returned; a case ends only by returning\n", state.name );
        fflush( stdout );
        _exit( 1 );
    }
}

inline int suite_run_case( int ( *worker )( const char * ), const char * name )
{
    suite_case_guard_state & state = suite_case_guard();
    if ( !state.registered )
    {
        if ( atexit( suite_case_exit_guard ) != 0 )
        {
            printf( "check failed: %s: could not register the exit guard\n", name );
            return 1;
        }
        state.registered = true;
    }
    state.name = name;
    state.pid = suite_process_id();
    state.running = true;
    const int rc = worker( name );
    state.running = false;
    return rc;
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
    {
        return bound;
    }

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
    {
        return bound;
    }
    if ( suite_env_is_set( "ROCKETNET_TEST_MAX_CASE_RSS_MB" ) )
    {
        return planned > bound ? planned : bound;
    }

    return planned;
}

// Records a finished case whose peak is over the bound, naming both. The
// functional tier enforces the result; the unit tier keeps the same
// measurement and prints the over-bound line without changing its verdict.

inline void suite_check_rss( suite_result & result, long bound_mb, bool enforce = true )
{
    if ( result.failed || bound_mb <= 0 || result.peak_rss_mb <= bound_mb )
    {
        return;
    }

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
    {
        return false;
    }

    char previous = '_';

    for ( size_t i = 0; i < name.size(); i++ )
    {
        const char c = name[i];
        if ( c == '_' )
        {
            if ( previous == '_' )
            {
                return false;
            }
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
        { "fragment", "window", "receipt", "rnreceipts", "resend", "resent", "watermark", "held", "hold", "endpoint", "budget", "send_budget", "rnscheduler", "message", "channel", "rnchannel", "reliab", "unreliable", "ordered", "violation", "wide", "dispatch", "block", "promoted", "mixed", "rtt_estimator", "shares", "generation", "reservation", "store", "chunk", "span", "builder", NULL },
        // transport basics: the driver/transport interface and the ladder
        { "driver", "transport", "ladder", "network_simulator", "sim", "link", "send", "receive", "fake", "table", "rnslots", NULL },
        // each transport type
        { "portable", "loopback", "msgx", "gso", "io_uring", "xdp", "iocp", "rio", "windows", "posix", "linux", "macos", "umem", NULL },
        // client/server: connect, handshake, slots, disconnect, lifecycle
        { "server", "client", "connect", "handshake", "request", "challenge", "slot", "disconnect", "lifecycle", "keep_alive", "dst", "live", "public", "api", "create", "destroy", "start", "backend", "session", "half", "two", "swarm", "full", "stale", NULL },
        // harnesses, control and examples
        { "harness", "control", "state", "example", NULL },
    };
    const int count = (int) ( sizeof( layers ) / sizeof( layers[0] ) );
    const std::string area = name.compare( 0, 5, "test_" ) == 0 ? name.substr( 5 ) : name;
    for ( int layer = 0; layer < count; layer++ )
    {
        for ( int i = 0; layers[layer][i] != NULL; i++ )
        {
            if ( area.compare( 0, std::strlen( layers[layer][i] ), layers[layer][i] ) == 0 )
            {
                return layer;
            }
        }
    }

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
        {
            return "";
        }
        if ( n < buf.size() )
        {
            return rocketnet_test_wide_to_utf8( buf.data(), (int) n );
        }
        if ( buf.size() >= 32768 )
        {
            return "";
        }
        buf.resize( buf.size() * 2 );
    }
#elif defined( __APPLE__ )
    char buf[4096];
    uint32_t size = (uint32_t) sizeof( buf );
    if ( _NSGetExecutablePath( buf, &size ) != 0 )
    {
        return "";
    }
    return std::string( buf );
#else
    char buf[4096];
    ssize_t n = readlink( "/proc/self/exe", buf, sizeof( buf ) - 1 );
    if ( n < 0 )
    {
        return "";
    }
    buf[n] = '\0';
    return std::string( buf );
#endif // #if defined( _WIN32 )
}

inline std::string suite_directory_of( const std::string & path )
{
    const std::string::size_type pos = path.find_last_of( "/\\" );
    if ( pos == std::string::npos )
    {
        return ".";
    }
    return path.substr( 0, pos );
}

inline bool suite_file_exists( const std::string & path )
{
#if defined( _WIN32 )
    const std::wstring wide = rocketnet_test_wide_path( path );
    if ( wide.empty() )
    {
        return false;
    }

    const DWORD attributes = GetFileAttributesW( wide.c_str() );
    return attributes != INVALID_FILE_ATTRIBUTES && !( attributes & FILE_ATTRIBUTE_DIRECTORY );
#else
    FILE * file = fopen( path.c_str(), "rb" );
    if ( !file )
    {
        return false;
    }
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
        {
            continue;
        }
        unsigned long long value = 0;
        if ( fscanf( file, "%llu", &value ) == 1 && value > 0 && ( limit == 0 || value < limit ) )
        {
            limit = (uint64_t) value; // "max" reads no number: no limit
        }
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
            {
                continue; // cgroup v2 only
            }
            std::string path = "/sys/fs/cgroup" + std::string( line + 3 );
            while ( !path.empty() && ( path.back() == '\n' || path.back() == '/' ) )
            {
                path.pop_back();
            }
            for ( ; path.size() > strlen( "/sys/fs/cgroup" ); path.erase( path.rfind( '/' ) ) )
            {
                const uint64_t limit = suite_cgroup_limit_bytes( path );
                if ( limit > 0 && ( budget == 0 || limit < budget ) )
                {
                    budget = limit;
                }
            }
        }
        fclose( file );
    }
    if ( budget > 0 )
    {
        return budget;
    }

    file = fopen( "/proc/meminfo", "r" );
    if ( file )
    {
        char line[256];
        unsigned long long kib = 0;
        while ( fgets( line, sizeof( line ), file ) )
        {
            if ( sscanf( line, "MemAvailable: %llu kB", &kib ) == 1 )
            {
                break;
            }
        }
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

// The cpus a mask holds: one bit a cpu, in 64-bit words.
inline unsigned suite_cpus_in_mask( const uint64_t * words, size_t count )
{
    unsigned cpus = 0;
    for ( size_t i = 0; i < count; i++ )
    {
        for ( uint64_t word = words[i]; word != 0; word &= word - 1 )
        {
            cpus++;
        }
    }
    return cpus;
}

// The cpus this process may run on, never 0: the host's count when the mask
// cannot be read. std::thread::hardware_concurrency() counts the host's
// online cpus, not the process's:
// on glibc 2.43 a run held to 5 cpus (nova-reserve, taskset, a cgroup cpuset)
// read 48 on a 48-cpu bench and ran 48 jobs on 5 cpus. Linux reads the
// affinity mask. Windows reads the process affinity mask, which covers one
// processor group: it is the count on a one-group host (at most 64 logical
// processors), and on a host with more groups when it is narrower than its
// group's system mask (start /affinity, a job held to some processors). An
// unrestricted process on a many-group host may run in every group, and
// GetProcessAffinityMask reports zero for a process with threads in several
// groups, so there the count stays the host's. macOS has no affinity mask to
// read (thread affinity there is only a hint), so its count is the host's.
inline unsigned suite_usable_cpus()
{
#if defined( __linux__ )
    cpu_set_t set;
    CPU_ZERO( &set );
    if ( sched_getaffinity( 0, sizeof( set ), &set ) == 0 )
    {
        uint64_t words[sizeof( set ) / sizeof( uint64_t )];
        std::memcpy( words, &set, sizeof( words ) );
        const unsigned cpus = suite_cpus_in_mask( words, sizeof( words ) / sizeof( words[0] ) );
        if ( cpus > 0 )
        {
            return cpus;
        }
    }
#elif defined( _WIN32 )
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if ( GetProcessAffinityMask( GetCurrentProcess(), &process_mask, &system_mask ) && process_mask != 0 &&
         ( GetActiveProcessorGroupCount() == 1 || process_mask != system_mask ) )
    {
        const uint64_t word = (uint64_t) process_mask;
        return suite_cpus_in_mask( &word, 1 );
    }
#endif // #if defined( __linux__ )
    return std::thread::hardware_concurrency();
}

// Jobs planned for each cpu this process may use. Most unit cases wait more
// than they compute, so the wall keeps falling past one job a cpu. Measured in
// a 4-cpu nova-reserve slot on 48-cpu bench-nyc-1 (2026-11-12), two runs at
// each ROCKETNET_TEST_JOBS:
// - Debug unit suite: 66/70 s at 4 jobs, 44/44 s at 8, 34/36 s at 12,
//   36/32 s at 16, 35/34 s at 24, 35/37 s at 48;
// - peak load average: about 10 at 12 jobs, 29-32 at 48.
// Past 3 jobs a cpu the wall stops falling and only the load grows.
#define SUITE_JOBS_PER_USABLE_CPU 3

// The cpus the job count is planned on: SUITE_JOBS_PER_USABLE_CPU for each
// usable cpu, never more than the host's count (host 0: unknown, the usable
// count). A process free to use the whole host plans the host's count, as
// before; only a run held to part of it plans fewer.
inline unsigned suite_planned_cpus( unsigned usable, unsigned host )
{
    const unsigned spread = usable * SUITE_JOBS_PER_USABLE_CPU;
    if ( host == 0 )
    {
        return usable;
    }
    return ( std::min )( spread, host );
}

// The job count for a requested count (0: none): the cpus this process may
// run on, this host's memory budget, and this platform's ceiling.
inline int suite_job_count_requested( int requested )
{
#if defined( _WIN32 )
    const int ceiling = SUITE_WAIT_HANDLES;
#else
    const int ceiling = 0;
#endif // #if defined( _WIN32 )

    const unsigned planned = suite_planned_cpus( suite_usable_cpus(), std::thread::hardware_concurrency() );
    return suite_job_count_for( planned, suite_memory_budget_bytes(), requested, ceiling );
}

inline int suite_job_count()
{
    int requested = 0;
    char env[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_JOBS", env, sizeof( env ) ) )
    {
        requested = std::atoi( env );
    }

    return suite_job_count_requested( requested );
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
    {
        return true;
    }
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
        {
            return requested;
        }
    }
#if defined( __linux__ )
    struct utsname host;
    int major = 0;
    int minor = 0;
    if ( uname( &host ) == 0 && sscanf( host.release, "%d.%d", &major, &minor ) == 2 && ( major < 6 || ( major == 6 && minor <= 8 ) ) )
    {
        return ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB_6_8;
    }
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
    {
        return -1;
    }
    return (long) ( limit.rlim_cur / 1024 );
#endif // #if defined( _WIN32 )
}

// How many io_uring cases may run at once: the formula above, capped at jobs.

inline int suite_uring_slots( long limit_kib, long case_kib, int jobs )
{
    if ( limit_kib < 0 || case_kib <= 0 )
    {
        return jobs;
    }

    const long slots = limit_kib / case_kib;
    return slots < (long) jobs ? (int) slots : jobs;
}

inline int suite_usage( char ** argv )
{
    fprintf( stderr, "usage: %s [--run <case> | --list [--shard <i>/<n>] | [--enforce-rss] [--shard <i>/<n>]]\n", argv[0] );
    return 2;
}

// --shard i/n runs (or with --list, lists) only the cases whose shard is i,
// for 0 <= i < n. A case's shard is a function of its name alone (FNV-1a of
// the name, mod n), so it is the same in every build and run of the suite:
// each case is in exactly one shard, and shards 0 to n-1 together are the
// unsharded suite.

inline int suite_shard_of( const char * name, int shards )
{
    uint32_t hash = 2166136261u;
    for ( const char * p = name; *p; p++ )
    {
        hash ^= (uint32_t) (unsigned char) *p;
        hash *= 16777619u;
    }
    return (int) ( hash % (uint32_t) shards );
}

// Reads "<i>/<n>" into index and shards; false unless 0 <= i < n <= 64.

inline bool suite_parse_shard( const char * text, int * index, int * shards )
{
    int i = 0;
    int n = 0;
    const char * p = text;
    if ( *p < '0' || *p > '9' )
    {
        return false;
    }
    while ( *p >= '0' && *p <= '9' && i <= 64 )
    {
        i = i * 10 + ( *p++ - '0' );
    }
    if ( *p++ != '/' || *p < '0' || *p > '9' )
    {
        return false;
    }
    while ( *p >= '0' && *p <= '9' && n <= 64 )
    {
        n = n * 10 + ( *p++ - '0' );
    }
    if ( *p != '\0' || n < 1 || n > 64 || i >= n )
    {
        return false;
    }
    *index = i;
    *shards = n;
    return true;
}

// Removes a trailing "--shard <i>/<n>" from argv. Returns false (after the
// usage line) when the option is malformed; shards stays 0 without it.

inline bool suite_take_shard_option( int * argc, char ** argv, int * index, int * shards )
{
    *index = 0;
    *shards = 0;
    for ( int i = 1; i < *argc; i++ )
    {
        if ( std::strcmp( argv[i], "--shard" ) != 0 )
        {
            continue;
        }
        if ( i != *argc - 2 || !suite_parse_shard( argv[i + 1], index, shards ) )
        {
            suite_usage( argv );
            return false;
        }
        *argc -= 2;
        return true;
    }
    return true;
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
        {
            break;
        }
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
            {
                reported = bound;
            }
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
// failing case prints everything it wrote and one check line naming the
// reason: always when the runner failed it (time, memory), and for a signal
// or an exit only when the case printed no check line itself.
// Returns false once a failure is printed, unless keep_going
// (suite_keep_going), when it prints on past the failure.

// The runner made the call: the case ran past its time limit or its peak was
// over its memory bound. Its reason is printed even when the case's output
// already holds a check line, because a case that exercises the check path
// prints one while passing: with 1500 MiB planted in
// test_suite_finish_fails_when_no_case_ran, an --enforce-rss run failed with
// only that case's own "check failed: no case ran" and no reason.
inline bool suite_runner_failed( const suite_result & result )
{
    return result.rss_over_bound || result.detail.find( " exceeded " ) != std::string::npos;
}

// A failing case gets the runner's check line naming the reason when the
// runner made the call, or when the case printed no check line of its own.
inline bool suite_failure_line_needed( const suite_result & result )
{
    return suite_runner_failed( result ) || result.output.find( "check failed" ) == std::string::npos;
}

inline bool suite_print_ready( std::vector<suite_result> & results, size_t & printed, bool capture, bool keep_going )
{
    while ( printed < results.size() && results[printed].done )
    {
        const suite_result & result = results[printed++];
        if ( result.failed )
        {
            suite_emit_captured( result.output, true );
            if ( suite_failure_line_needed( result ) )
            {
                printf( "check failed: %s\n", result.detail.c_str() );
            }
            fflush( stdout );
            if ( !keep_going )
            {
                return false;
            }
            continue;
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
    {
        return;
    }

    std::vector<suite_result> ran;
    for ( size_t i = 0; i < results.size(); i++ )
    {
        if ( results[i].done )
        {
            ran.push_back( results[i] );
        }
    }
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
    {
        if ( ran[i].peak_rss_mb >= 0 && ran[i].check_rss )
        {
            printf( "  %6ld MiB (bound %ld)  %s\n", ran[i].peak_rss_mb, suite_case_rss_mb( ran[i].name, ran[i].plan_rss_mb ), ran[i].name.c_str() );
            printed++;
        }
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

    if ( suite_keep_going_requested() && !suite_nightly() )
    {
        printf( "check failed: ROCKETNET_TEST_KEEP_GOING=1 is for the nightly tier only; set ROCKETNET_TEST_NIGHTLY=1 or unset it\n" );
        fflush( stdout );
        return false;
    }

    const size_t first = pending.size();
    const bool nightly = suite_nightly();
    int num_registered = 0;
    for ( int i = 0; i < count(); i++ )
    {
        if ( nightly || !group_of || ( group_of( name_at( i ) ) & SUITE_GROUP_NIGHTLY ) == 0 )
        {
            num_registered++;
        }
    }
    pending.resize( first + (size_t) num_registered + (size_t) nexecs );

    for ( int i = 0, kept = 0; i < count(); i++ )
    {
        if ( !nightly && group_of && ( group_of( name_at( i ) ) & SUITE_GROUP_NIGHTLY ) != 0 )
        {
            continue;
        }
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

    // every name once: a registry row or exec row repeated would run twice
    // and the second result would stand for both (VERIFY-5 K3). Compared
    // through pointers to the names, never string copies (the MSan note
    // above)

    {
        std::vector<const char *> names( pending.size() );
        for ( size_t i = 0; i < pending.size(); i++ )
        {
            names[i] = pending[i].name.c_str();
        }
        std::sort( names.begin(), names.end(), []( const char * a, const char * b )
                   { return std::strcmp( a, b ) < 0; } );
        for ( size_t i = 1; i < names.size(); i++ )
        {
            if ( std::strcmp( names[i - 1], names[i] ) == 0 )
            {
                printf( "check failed: test name %s is registered more than once\n", names[i] );
                return false;
            }
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
        {
            return a.group < b.group;
        }
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

// Keeps the collected cases of shard index of shards (suite_shard_of), in
// suite order, and prints how many of the collected cases that is.

inline void suite_keep_shard( std::vector<suite_case> & pending, std::vector<suite_result> & results, int index, int shards, bool report = true )
{
    if ( shards == 0 )
    {
        return;
    }
    const size_t collected = pending.size();
    size_t kept = 0;
    for ( size_t i = 0; i < collected; i++ )
    {
        if ( suite_shard_of( pending[i].name.c_str(), shards ) != index )
        {
            continue;
        }
        if ( kept != i )
        {
            std::swap( pending[kept], pending[i] );
            std::swap( results[kept], results[i] );
        }
        kept++;
    }
    pending.resize( kept );
    results.resize( kept );
    if ( report )
    {
        printf( "shard %d/%d: %zu of %zu cases\n", index, shards, kept, collected );
        fflush( stdout );
    }
}

// --list [--shard i/n]: the names a run with the same environment and shard
// would start, chosen by the run's own code (suite_collect, then
// suite_keep_shard), never by a second reading of the registry: the nightly
// tier's cases are listed only under ROCKETNET_TEST_NIGHTLY=1, an exec row is
// listed with the registered cases, and a case a shard run drops is missing
// from that shard's list too (tools/check_test_shards.py,
// tools/check_unit_case_coverage.py).

inline int suite_list_cases( int ( *count )(), const char * ( *name_at )(int), const suite_exec * execs, int nexecs,
                             int ( *group_of )( const char * name ), const char * exe_suffix, int shard_index, int shards )
{
    const std::string self = suite_executable_path();
    if ( self.empty() )
    {
        printf( "check failed: could not resolve this executable\n" );
        return 1;
    }

    std::vector<suite_case> pending;
    std::vector<suite_result> results;
    if ( !suite_collect( pending, results, count, name_at, execs, nexecs, group_of, suite_directory_of( self ), exe_suffix ) )
    {
        return 1;
    }
    suite_keep_shard( pending, results, shard_index, shards, false );

    for ( size_t i = 0; i < pending.size(); i++ )
    {
        printf( "%s\n", pending[i].name.c_str() );
    }
    fflush( stdout );

    return 0;
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
    {
        return;
    }
    const std::vector<suite_skipped_row> & rows = suite_skipped_rows();
    for ( size_t i = 0; i < rows.size(); i++ )
    {
        printf( "SKIPPED %s: %s\n", rows[i].name.c_str(), rows[i].predicate.c_str() );
    }
    printf( "skipped: %d\n", (int) rows.size() );
    fflush( stdout );
}

// Under suite_keep_going the run is not stopped by a failure, so passed alone
// cannot judge it: every failed case is named, the cases run are counted
// against the cases collected, and any failure or any case left unrun fails
// the suite.

inline bool suite_keep_going_summary( const std::vector<suite_result> & results )
{
    int failed = 0;
    int ran = 0;
    for ( size_t i = 0; i < results.size(); i++ )
    {
        if ( !results[i].done )
        {
            continue;
        }
        ran++;
        if ( results[i].failed )
        {
            failed++;
            printf( "FAILED %s\n", results[i].name.c_str() );
        }
    }
    printf( "keep going: %d failed; %d of %d collected cases ran\n", failed, ran, (int) results.size() );
    fflush( stdout );

    return failed == 0 && ran == (int) results.size();
}

inline int suite_finish( const std::vector<suite_result> & results, bool passed )
{
    suite_print_report( results );
    suite_print_skipped();
    if ( suite_keep_going() && !suite_keep_going_summary( results ) )
    {
        passed = false;
    }
    if ( !passed )
    {
        return 1;
    }
    // a run that collected no case (an empty suite or shard) is a lost
    // registration, never a pass
    if ( results.empty() )
    {
        printf( "check failed: no case ran\n" );
        fflush( stdout );
        return 1;
    }
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
    {
        if ( name == registry->entries[i].name )
        {
            return true;
        }
    }
#else // #if defined( SUITE_RSS_BOUND_CASES_RUN_ALONE )
    (void) name;
#endif // #if defined( SUITE_RSS_BOUND_CASES_RUN_ALONE )

    return false;
}

#if defined( _WIN32 )

// The commit state, printed at the start of a Windows run and by a case that
// gets NULL from a create, so a red names the limit it ran into.

inline std::string suite_commit_text()
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof( status );
    if ( !GlobalMemoryStatusEx( &status ) )
    {
        return "commit state unreadable";
    }
    char text[160];
    snprintf( text, sizeof( text ), "commit available %llu MiB of a %llu MiB limit, physical available %llu MiB",
              (unsigned long long) ( status.ullAvailPageFile >> 20 ), (unsigned long long) ( status.ullTotalPageFile >> 20 ),
              (unsigned long long) ( status.ullAvailPhys >> 20 ) );
    return text;
}

// Running alone inside one run is not enough on Windows: the commit limit is
// the machine's, and the runs beside this one (the other shard, another job's
// legs, on runners sharing the machine) start their full-capacity cases on
// their own schedules. captain (64 GiB, 160 GiB commit limit) logged "low
// virtual memory" (System event 2004) at 22:47:58 on 2026-11-12 with two
// test.exe at 43.4 GiB each and the WSL VM at 40 GiB; 0.9 s later a
// rocketnet_client_adopt in test_clients_disconnect_together_server_count_exact
// came back NULL (CI run 43323005232, windows / Release / unit (0/2)).
//
// So a run takes one machine-wide lock for each such case it starts, and
// releases it when the case ends; a run that dies leaves the lock abandoned,
// which the next wait takes. Global\ reaches every session (the runners are
// services in session 0); a process that may not create Global\ objects
// falls back to its own session's Local\ name.
//
// Holding the lock, the run then checks the case's planned commit (its
// declared bound) against the commit available. A host whose whole limit is
// below the plan fails the case by name at once; one that is short for now
// (memory held by something outside these runs) is waited on, up to
// SUITE_COMMIT_WAIT_SECONDS, then the case fails by name. Either way the
// red says the host is too small for the case, not an allocation somewhere
// inside it.

#define SUITE_COMMIT_LOCK_WAIT_SECONDS 1800
#define SUITE_COMMIT_WAIT_SECONDS 120

inline HANDLE suite_commit_lock_open()
{
    HANDLE lock = CreateMutexW( NULL, FALSE, L"Global\\rocketnet-test-commit-bound" );
    if ( !lock )
    {
        lock = CreateMutexW( NULL, FALSE, L"Local\\rocketnet-test-commit-bound" );
    }
    return lock;
}

inline bool suite_commit_lock_take( HANDLE lock, const std::string & name, long planned_mb, std::string & detail )
{
    if ( !lock )
    {
        detail = name + " could not open the machine-wide commit lock";
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    const DWORD wait = WaitForSingleObject( lock, SUITE_COMMIT_LOCK_WAIT_SECONDS * 1000 );
    const double waited = std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
    if ( wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED )
    {
        char text[128];
        snprintf( text, sizeof( text ), " waited %.0f s for the machine-wide commit lock (wait %lu)", waited, (unsigned long) wait );
        detail = name + text;
        return false;
    }
    if ( waited >= 1.0 )
    {
        printf( "commit lock: %s waited %.1f s for another run's full-capacity case\n", name.c_str(), waited );
        fflush( stdout );
    }

    const auto held = std::chrono::steady_clock::now();
    for ( ;; )
    {
        MEMORYSTATUSEX status;
        status.dwLength = sizeof( status );
        if ( planned_mb <= 0 || !GlobalMemoryStatusEx( &status ) || ( status.ullAvailPageFile >> 20 ) >= (uint64_t) planned_mb )
        {
            return true;
        }
        const double short_s = std::chrono::duration<double>( std::chrono::steady_clock::now() - held ).count();
        if ( ( status.ullTotalPageFile >> 20 ) < (uint64_t) planned_mb || short_s >= SUITE_COMMIT_WAIT_SECONDS )
        {
            char text[256];
            snprintf( text, sizeof( text ), " needs %ld MiB of commit, this host has %llu MiB available of a %llu MiB limit (waited %.0f s)",
                      planned_mb, (unsigned long long) ( status.ullAvailPageFile >> 20 ),
                      (unsigned long long) ( status.ullTotalPageFile >> 20 ), short_s );
            detail = name + text;
            ReleaseMutex( lock );
            return false;
        }
        Sleep( 1000 );
    }
}

inline long suite_case_planned_mb( const std::string & name )
{
    suite_case_rss_bound_t * entry = suite_case_rss_bound_find( suite_case_rss_bound_registry(), name.c_str() );
    return entry ? entry->bound_mb() : 0;
}

#else // #if defined( _WIN32 )

inline std::string suite_commit_text()
{
    return "commit state is Windows only";
}

#endif // #if defined( _WIN32 )

#if !defined( _WIN32 )

inline void suite_drain( int fd, std::string & output )
{
    if ( fd < 0 )
    {
        return;
    }

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
        {
            continue;
        }
        break;
    }
}

// A case run by `--run <case>` with no suite parent (CI's XDP cases, run as
// root one at a time inside virtme-ng guests) holds itself to the bound the
// parent would hold it to: suite_case_rss_mb of the bound it reported from
// its plan (tests/plan_rss.h), or of none, against its own peak resident set
// read as wait4 would read it (the larger of its own and its waited-for
// children's). A passed case over its bound fails by name. Under a suite
// parent (ROCKETNET_TEST_SUITE_CAPTURE) the parent checks, and this does not
// run; a tier that does not enforce its bounds (the unit suite) skips it.

inline int suite_check_own_rss( const char * name, int rc, bool enforce )
{
    if ( rc != 0 || !enforce )
    {
        return rc;
    }

    const auto start = std::chrono::steady_clock::now();

    struct rusage self;
    struct rusage children;
    memset( &self, 0, sizeof( self ) );
    memset( &children, 0, sizeof( children ) );
    getrusage( RUSAGE_SELF, &self );
    getrusage( RUSAGE_CHILDREN, &children );
    const long maxrss = self.ru_maxrss > children.ru_maxrss ? self.ru_maxrss : children.ru_maxrss;
#if defined( __APPLE__ )
    const long peak_mb = maxrss / ( 1024 * 1024 ); // bytes on macOS
#else
    const long peak_mb = maxrss / 1024; // kilobytes on Linux
#endif // #if defined( __APPLE__ )
    const long bound_mb = suite_case_rss_mb( name, plan_rss_reported() ? plan_rss_bound_mb() : -1 );

    const long long check_us = (long long) std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - start ).count();
    printf( "own rss %s: peak_mb=%ld bound_mb=%ld (checked in %lld us)\n", name, peak_mb, bound_mb, check_us );

    if ( bound_mb > 0 && peak_mb > bound_mb )
    {
        printf( "check failed: %s peaked at %ld MiB resident, over its bound of %ld MiB (tests/plan_rss.h)\n", name, peak_mb, bound_mb );
        fflush( stdout );
        return 1;
    }

    fflush( stdout );
    return rc;
}

// Runs one case in a child with its stdout and stderr read through a pipe,
// then prints every line when it failed and only the name lines when it
// passed. The child may exit(1), abort or be killed and the parent still has
// every line it wrote. This is the same names-only shape the suite applies to
// its children, so `--run <case>` does not leak a passing case's diagnostics
// without ROCKETNET_TEST_VERBOSE.

inline int suite_run_names_only( int ( *worker )( const char * ), const char * name, double limit_s, bool enforce_rss )
{
    fflush( stdout );
    fflush( stderr );

    int pipefd[2];
    if ( pipe( pipefd ) != 0 )
    {
        suite_run_watchdog( name, limit_s );
        return suite_check_own_rss( name, suite_run_case( worker, name ), enforce_rss );
    }

    const pid_t pid = fork();
    if ( pid < 0 )
    {
        close( pipefd[0] );
        close( pipefd[1] );
        suite_run_watchdog( name, limit_s );
        return suite_check_own_rss( name, suite_run_case( worker, name ), enforce_rss );
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

        // in the case's process, so its line reaches the pipe the parent prints
        suite_run_watchdog( name, limit_s );
        int rc = suite_check_own_rss( name, suite_run_case( worker, name ), enforce_rss );
#if defined( SUITE_RUNNER_LSAN ) && !defined( _WIN32 )
        // _exit skips LeakSanitizer's exit-time check: run it here, so a
        // single `--run <case>` reports a leak as the whole suite would. It
        // does nothing when detect_leaks is off.
        if ( __lsan_do_recoverable_leak_check() != 0 && rc == 0 )
        {
            printf( "check failed: %s leaked (LeakSanitizer, above)\n", name );
            rc = 1;
        }
#endif // #if defined( SUITE_RUNNER_LSAN ) && !defined( _WIN32 )
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
    {
        rc = WEXITSTATUS( status );
    }
    else if ( waited >= 0 && WIFSIGNALED( status ) )
    {
        rc = 128 + WTERMSIG( status );
    }
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
    int shard_index;
    int shards;
    if ( !suite_take_shard_option( &argc, argv, &shard_index, &shards ) )
    {
        return 2;
    }
    // --enforce-rss before --shard: a shard of the suite enforces the bound too
    if ( argc == 2 && std::strcmp( argv[1], "--enforce-rss" ) == 0 )
    {
        enforce_case_rss = true;
        argc = 1;
    }
    const bool list = argc == 2 && std::strcmp( argv[1], "--list" ) == 0;
    if ( shards > 0 && argc != 1 && !list )
    {
        return suite_usage( argv );
    }

    if ( list )
    {
        return suite_list_cases( count, name_at, execs, nexecs, group_of, "", shard_index, shards );
    }

    if ( argc >= 2 && std::strcmp( argv[1], "--run" ) == 0 )
    {
        // exactly one case: a second name was ignored before, so a caller
        // that believed it ran two cases ran one

        if ( argc != 3 || argv[2][0] == '-' )
        {
            return suite_usage( argv );
        }

        // under a suite run the parent already captures this process, so
        // stream straight through; a second capture would lose lines when the
        // parent kills the process group at the time limit

        if ( suite_env_is_set( "ROCKETNET_TEST_SUITE_CAPTURE" ) )
        {
            unsetenv( "ROCKETNET_TEST_SUITE_CAPTURE" );
            return suite_run_case( worker, argv[2] );
        }

        // on its own, the case is held to the limit a suite run holds it to
        double run_limit_s = case_limit_s;
        if ( !suite_case_limit( case_limit_s, &run_limit_s ) )
        {
            return 1;
        }
        if ( !suite_env_is_set( "ROCKETNET_TEST_VERBOSE" ) )
        {
            return suite_run_names_only( worker, argv[2], run_limit_s, enforce_case_rss );
        }
        suite_run_watchdog( argv[2], run_limit_s );
        return suite_check_own_rss( argv[2], suite_run_case( worker, argv[2] ), enforce_case_rss );
    }

    // a whole-suite run takes no other argument: a bare case name or a
    // misspelt flag is refused, never ignored (a bare name used to run the
    // whole suite and report its status as the case's)

    if ( argc > 1 )
    {
        return suite_usage( argv );
    }

    const std::string self = suite_executable_path();
    if ( self.empty() )
    {
        printf( "check failed: could not resolve this executable\n" );
        return 1;
    }

    std::vector<suite_case> pending;
    std::vector<suite_result> results;
    if ( !suite_collect( pending, results, count, name_at, execs, nexecs, group_of, suite_directory_of( self ), "" ) )
    {
        return 1;
    }
    suite_keep_shard( pending, results, shard_index, shards );

    const int jobs = max_jobs > 0 ? ( std::min )( suite_job_count(), max_jobs ) : suite_job_count();
    if ( suite_env_is_set( "ROCKETNET_TEST_REPORT" ) )
    {
        printf( "jobs: %d (cpus this run may use: %u)\n", jobs, suite_usable_cpus() );
    }
    double limit_s = case_limit_s;
    if ( !suite_case_limit( case_limit_s, &limit_s ) )
    {
        return 1;
    }

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
    const bool keep_going = suite_keep_going();
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

        passed = suite_print_ready( results, printed, capture, keep_going );
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
    {
        return;
    }

    char buf[4096];

    for ( ;; )
    {
        DWORD available = 0;
        if ( !PeekNamedPipe( pipe, NULL, 0, NULL, &available, NULL ) || available == 0 )
        {
            return;
        }

        DWORD n = 0;
        if ( !ReadFile( pipe, buf, available < sizeof( buf ) ? available : (DWORD) sizeof( buf ), &n, NULL ) || n == 0 )
        {
            return;
        }
        output.append( buf, (size_t) n );
    }
}

// A test process on a headless runner never waits on a window. Left at their
// defaults, the CRT answers abort, a failed assertion and an invalid parameter
// with a message box (Debug) or a fault report, and Windows answers a crash
// with the error-reporting dialog. Nobody can see those on a runner, so the
// case sits until the case limit kills it, and a suite of failing cases takes
// hours instead of minutes (#201). With these set, abort exits 3 at once, a
// crash exits with its NTSTATUS, and the report text goes to stderr, where the
// case's captured output shows it. The error mode is inherited by every child
// the suite spawns; each child sets all of it again in its own CRT.
// tests/windows_no_dialogs.cpp and ci.yml's Windows leg (c) prove it.

#if defined( _MSC_VER ) && defined( _DEBUG )

// The debug CRT reports what it detects (a heap block overrun at its free, a
// bad pointer, a CRT assert) and, with the reports routed to stderr so no
// dialog blocks CI, carries on: the case then exits 0 over a printed "HEAP
// CORRUPTION DETECTED". Any error or assert report ends the process red: the
// text goes to stderr first, then the process leaves with status 3. Warnings
// (the leak dump) stay reports.

inline int __cdecl suite_crt_report_fails( int report_type, char * message, int * return_value )
{
    if ( report_type == _CRT_ERROR || report_type == _CRT_ASSERT )
    {
        fprintf( stderr, "check failed: the debug CRT reported an error: %s\n", message ? message : "(no text)" );
        fflush( stderr );
        fflush( stdout );
        _exit( 3 );
    }
    if ( return_value )
    {
        *return_value = 0;
    }
    return FALSE;
}

#endif // #if defined( _MSC_VER ) && defined( _DEBUG )

inline void suite_no_dialogs()
{
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX );
#if defined( _MSC_VER )
    _set_error_mode( _OUT_TO_STDERR );
    _set_abort_behavior( 0, _CALL_REPORTFAULT );
#if defined( _DEBUG )
    _CrtSetReportMode( _CRT_ASSERT, _CRTDBG_MODE_FILE );
    _CrtSetReportFile( _CRT_ASSERT, _CRTDBG_FILE_STDERR );
    _CrtSetReportMode( _CRT_ERROR, _CRTDBG_MODE_FILE );
    _CrtSetReportFile( _CRT_ERROR, _CRTDBG_FILE_STDERR );
    _CrtSetReportMode( _CRT_WARN, _CRTDBG_MODE_FILE );
    _CrtSetReportFile( _CRT_WARN, _CRTDBG_FILE_STDERR );
    _CrtSetReportHook2( _CRT_RPTHOOK_INSTALL, suite_crt_report_fails );
#endif // #if defined( _DEBUG )
#endif // #if defined( _MSC_VER )
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
    suite_no_dialogs();

    int shard_index;
    int shards;
    if ( !suite_take_shard_option( &argc, argv, &shard_index, &shards ) )
    {
        return 2;
    }
    // --enforce-rss before --shard: a shard of the suite enforces the bound too
    if ( argc == 2 && std::strcmp( argv[1], "--enforce-rss" ) == 0 )
    {
        enforce_case_rss = true;
        argc = 1;
    }
    const bool list = argc == 2 && std::strcmp( argv[1], "--list" ) == 0;
    if ( shards > 0 && argc != 1 && !list )
    {
        return suite_usage( argv );
    }

    if ( list )
    {
        return suite_list_cases( count, name_at, execs, nexecs, group_of, ".exe", shard_index, shards );
    }

    if ( argc >= 2 && std::strcmp( argv[1], "--run" ) == 0 )
    {
        // exactly one case: a second name was ignored before, so a caller
        // that believed it ran two cases ran one

        if ( argc != 3 || argv[2][0] == '-' )
        {
            return suite_usage( argv );
        }
        for ( int i = 3; i < argc; i++ )
        {
            if ( argv[i][0] == '-' )
            {
                return suite_usage( argv );
            }
        }
        // the case is held to the limit a suite run holds it to (a case the
        // suite spawned is also killed by its parent at that limit)
        double run_limit_s = case_limit_s;
        if ( !suite_case_limit( case_limit_s, &run_limit_s ) )
        {
            return 1;
        }
        suite_run_watchdog( argv[2], run_limit_s );
        return suite_run_case( worker, argv[2] );
    }

    // a whole-suite run takes no other argument: a bare case name or a
    // misspelt flag is refused, never ignored (a bare name used to run the
    // whole suite and report its status as the case's)

    if ( argc > 1 )
    {
        return suite_usage( argv );
    }

    const std::string self = suite_executable_path();
    if ( self.empty() )
    {
        printf( "check failed: could not resolve this executable\n" );
        return 1;
    }

    std::vector<suite_case> pending;
    std::vector<suite_result> results;
    if ( !suite_collect( pending, results, count, name_at, execs, nexecs, group_of, suite_directory_of( self ), ".exe" ) )
    {
        return 1;
    }
    suite_keep_shard( pending, results, shard_index, shards );

    const int jobs = max_jobs > 0 ? ( std::min )( suite_job_count(), max_jobs ) : suite_job_count();
    if ( suite_env_is_set( "ROCKETNET_TEST_REPORT" ) )
    {
        printf( "jobs: %d (cpus this run may use: %u)\n", jobs, suite_usable_cpus() );
    }
    double limit_s = case_limit_s;
    if ( !suite_case_limit( case_limit_s, &limit_s ) )
    {
        return 1;
    }
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
    const bool keep_going = suite_keep_going();
    bool passed = true;
    HANDLE commit_lock = suite_commit_lock_open();
    printf( "%s\n", suite_commit_text().c_str() );
    fflush( stdout );
    bool commit_held = false;
    while ( passed && ( next < pending.size() || !running.empty() ) )
    {
        while ( (int) running.size() < jobs && next < pending.size() &&
                ( running.empty() || ( running[0].group == pending[next].group && !running[0].alone && !suite_case_runs_alone( pending[next].name ) ) ) )
        {
            const size_t index = next++;
            const suite_case & item = pending[index];
            suite_result & result = results[index];
            if ( suite_case_runs_alone( item.name ) )
            {
                if ( !suite_commit_lock_take( commit_lock, item.name, suite_case_planned_mb( item.name ), result.detail ) )
                {
                    result.done = true;
                    result.failed = true;
                    continue;
                }
                commit_held = true;
            }

            const std::string path = item.exec_path.empty() ? self : item.exec_path;
            const std::wstring wide_path = rocketnet_test_wide_path( path );
            std::wstring command = L"\"" + wide_path + L"\"";
            if ( item.exec_path.empty() )
            {
                command += L" --run " + rocketnet_test_utf8_to_wide( item.name );
            }
            else if ( !item.exec_argument.empty() )
            {
                command += L" " + rocketnet_test_utf8_to_wide( item.exec_argument );
            }
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
                    if ( commit_held )
                    {
                        ReleaseMutex( commit_lock );
                        commit_held = false;
                    }
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
                if ( commit_held )
                {
                    ReleaseMutex( commit_lock );
                    commit_held = false;
                }
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
            if ( running[i].alone && commit_held )
            {
                ReleaseMutex( commit_lock );
                commit_held = false;
            }
            running.erase( running.begin() + (std::ptrdiff_t) i );
        }

        passed = suite_print_ready( results, printed, capture, keep_going );
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

    if ( commit_held )
    {
        ReleaseMutex( commit_lock );
    }
    if ( commit_lock )
    {
        CloseHandle( commit_lock );
    }
    return suite_finish( results, passed );
}

#endif // #if !defined( _WIN32 )

#endif // ROCKETNET_TESTS_SUITE_RUNNER_H
