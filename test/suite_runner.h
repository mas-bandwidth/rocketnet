/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Parallel suite runner. Each test is a process. The output is the netcode
    shape: one line per test, its name, in suite order, then
    "*** ALL TESTS PASSED ***". A failing test prints its own output (its
    check line) and the suite exits 1 at once. A test over 120s is killed and
    fails (a Debug or sanitizer build scales the 120s: test/time_scale.h). A
    case process holds its own peak resident set to its plan: the bytes the
    SDK held at its peak plus a fixed slack (test/plan_rss.h), and fails
    itself past it. The runner records each case's peak resident set (wait4
    ru_maxrss on POSIX, PeakWorkingSetSize on Windows) and enforces a bound of
    its own only when ROCKETNET_TEST_MAX_CASE_RSS_MB sets one for every case or
    a case declares one with SUITE_CASE_RSS_BOUND; a sanitizer build enforces
    none unless the variable sets one. Every registered name is test_ lowercase snake_case,
    or the suite fails before running anything. ROCKETNET_TEST_REPORT=1 prints
    the slowest cases and the peak resident sets after the results.
*/

#pragma once

#include "portable_crt.h"
#include "time_scale.h"
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

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include "resource_file.h"
#else
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char **environ;
#endif

// There is no suite-wide fixed bound on a case's peak resident set: each case
// process holds itself to its own plan (test/plan_rss.h, the SDK's planned
// bytes plus a fixed slack) and fails itself. The parent enforces a bound only
// when ROCKETNET_TEST_MAX_CASE_RSS_MB sets one for every case, or when a case
// declares one with SUITE_CASE_RSS_BOUND.

// Whether the environment variable is set (to a value of at most 4095 bytes).
inline bool suite_env_is_set( const char * name )
{
    char value[4096];
    return rocketnet_test_getenv( name, value, sizeof( value ) ) != 0;
}

struct suite_exec
{
    const char *name;
    const char *file;
};

struct suite_case
{
    std::string name;
    std::string exec_path; // empty: re-exec this binary with --run name
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
    std::string output;
};

// The environment's bound for every case in MiB, or 0 when none is set.
inline long suite_max_case_rss_mb()
{
    char env[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_MAX_CASE_RSS_MB", env, sizeof( env ) ) )
    {
        const long requested = std::atol( env );
        if ( requested > 0 )
            return requested;
    }
    return 0;
}

// A case whose subject is a full-capacity server allocates its designed
// per-client memory up front, so its footprint is set by the SDK's memory
// plan, not by the suite-wide bound. Such a case declares its bound beside
// itself with SUITE_CASE_RSS_BOUND( name, function ): function returns the
// bound in MiB, computed from the memory plan for the configuration the case
// uses plus a margin the case states (suite_rss_bound_mb). A static
// registrar adds it to a list before main runs, so the parent reads it by
// case name. Every other case keeps ROCKETNET_TEST_MAX_CASE_RSS_MB.

struct suite_case_rss_bound_t
{
    const char * name;
    long ( *bound_mb )();
    suite_case_rss_bound_t * next;
};

inline suite_case_rss_bound_t ** suite_case_rss_bound_head()
{
    static suite_case_rss_bound_t * head = NULL;
    return &head;
}

struct suite_case_rss_bound_registrar_t
{
    suite_case_rss_bound_t entry;
    suite_case_rss_bound_registrar_t( const char * name, long ( *bound_mb )() )
    {
        entry.name = name;
        entry.bound_mb = bound_mb;
        entry.next = *suite_case_rss_bound_head();
        *suite_case_rss_bound_head() = &entry;
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

// The parent's bound for one case: the environment override when set, else
// none in a sanitizer build, else the bound the case declares, else none (the
// case process holds itself to its plan: test/plan_rss.h). 0 is no bound.
inline long suite_case_rss_mb( const std::string & name )
{
    const long bound = suite_max_case_rss_mb();
    if ( bound > 0 )
        return bound;
#if defined(ROCKETNET_FUNCTIONAL_SANITIZED)
    (void) name;
    return 0;
#else
    for ( suite_case_rss_bound_t * entry = *suite_case_rss_bound_head(); entry != NULL; entry = entry->next )
    {
        if ( name == entry->name )
        {
            const long declared = entry->bound_mb();
            return declared > 0 ? declared : 0;
        }
    }
    return 0;
#endif
}

// Fails a finished case whose peak is over the bound, naming both.
inline void suite_check_rss( suite_result & result, long bound_mb )
{
    if ( result.failed || bound_mb <= 0 || result.peak_rss_mb <= bound_mb )
        return;
    char text[96];
    snprintf( text, sizeof( text ), " peak_rss_mb=%ld bound_mb=%ld", result.peak_rss_mb, bound_mb );
    result.failed = true;
    result.detail = result.name + text;
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

// The suite's order, simplest first: helpers, packet, acks, transport basics,
// each transport type, client/server, messages. A case's layer is the first
// rule whose area word its name starts with (after test_); the cases keep
// their registration order within a layer.
inline int suite_layer_of( const std::string & name )
{
    static const char * const layers[][48] =
    {
        // helpers: platform, time, base64, config, logging, memory
        { "platform", "time", "update_time", "sleep", "thread", "endian", "read", "bounded", "base64", "hash", "copy", "address", "anonymize", "random", "config", "log", "memory", "allocator", "status", "version", "value_tracker", "secret", "suite", "fp", "hostname", "counter", "class", "sized", "dev", "abi", "unit", "runtime", "user", "deterministic", "fuzz", "pcapng", "rocketnet", "default", "disabled", "limits", "release", "exhaustion", NULL },
        // packet: wire, crypto, tokens, prefilter, fragments
        { "wire", "crypto", "wycheproof", "token", "connect_token", "protocol", "signed", "prefilter", "packet", "sealed", "unsealed", "open_sealed", "fragment", "reassembly", "cookie", "standard", "serialize", "replay", "integrity", "keyed", "encryption", "used", "hostile", "tampered", "vectors", "salt", "response", "admission", "payload", "max", "large", "small", "chunk", "span", "builder", "coalesce", "v4", "ipv4", "ipv6", "reused", "parse", NULL },
        // acks: ack, window, sequence, receipts
        { "ack", "window", "sequence", "receipt", "rnreceipts", "send_history", "loss", "jitter", "out_of_order", "resend", "resent", "reply", "previous", "pending", "never", "newer", "older", "newest", "equal", "watermark", "same", "repeated", "higher", "round", "one", "both", "first", "each", "held", "hold", NULL },
        // transport basics: the driver/transport interface and the ladder
        { "driver", "transport", "ladder", "endpoint", "network_simulator", "sim", "link", "send", "receive", "budget", "fake", "table", "rnslots", "rnscheduler", NULL },
        // each transport type
        { "portable", "loopback", "msgx", "gso", "io_uring", "xdp", "iocp", "rio", "windows", "posix", "linux", "macos", "umem", NULL },
        // client/server: connect, handshake, slots, disconnect, lifecycle
        { "server", "client", "connect", "handshake", "request", "challenge", "slot", "disconnect", "lifecycle", "keep_alive", "frame_sync", "dst", "live", "public", "api", "create", "destroy", "start", "backend", "session", "half", "two", "swarm", "full", "stale", NULL },
        // messages: channels, reliability, message layer, examples
        { "message", "channel", "rnchannel", "reliab", "unreliable", "ordered", "violation", "wide", "dispatch", "harness", "control", "state", "block", "promoted", "mixed", "rtt_estimator", "shares", "example", NULL },
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
#if defined(_WIN32)
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
#elif defined(__APPLE__)
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
#endif
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
#if defined(_WIN32)
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
#endif
}

inline int suite_job_count()
{
    unsigned jobs = std::thread::hardware_concurrency();
    if ( jobs == 0 )
        jobs = 1;
    char env[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_JOBS", env, sizeof( env ) ) )
    {
        const int requested = std::atoi( env );
        if ( requested > 0 )
            jobs = (unsigned) requested;
    }
    if ( jobs > 8 )
        jobs = 8;
    return (int) jobs;
}

inline int suite_usage( char ** argv )
{
    fprintf( stderr, "usage: %s [--run <case>]\n", argv[0] );
    return 2;
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
            line.pop_back();
        emit( line );
        if ( nl == std::string::npos )
            break;
        i = nl + 1;
    }
}

// A passing case's output keeps only its test-name lines. Returns the number
// of lines printed.
inline int suite_emit_captured( const std::string & output, bool show_all )
{
    int printed = 0;
    suite_for_each_line( output, [show_all, &printed]( const std::string & line ) {
        if ( show_all || suite_name_is_test_snake_case( line ) )
        {
            printf( "%s\n", line.c_str() );
            printed++;
        }
    } );
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
                printf( "check failed: %s\n", result.detail.c_str() );
            fflush( stdout );
            return false;
        }
        if ( !capture )
            continue;
        if ( !result.exec || suite_emit_captured( result.output, false ) == 0 )
            printf( "%s\n", result.name.c_str() );
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
    std::sort( ran.begin(), ran.end(), []( const suite_result & a, const suite_result & b ) { return a.seconds > b.seconds; } );
    printf( "\nslowest cases:\n" );
    for ( size_t i = 0; i < ran.size() && i < 10; i++ )
        printf( "  %8.3fs  %s\n", ran[i].seconds, ran[i].name.c_str() );
    std::sort( ran.begin(), ran.end(), []( const suite_result & a, const suite_result & b ) { return a.peak_rss_mb > b.peak_rss_mb; } );
    printf( "peak rss:\n" );
    for ( size_t i = 0; i < ran.size() && i < 10; i++ )
        if ( ran[i].peak_rss_mb >= 0 )
            printf( "  %6ld MiB (bound %ld)  %s\n", ran[i].peak_rss_mb, suite_case_rss_mb( ran[i].name ), ran[i].name.c_str() );
    fflush( stdout );
}

// The cases in suite order, or false (after printing why) when a name is not
// test_ snake_case or an exec binary is missing.
inline bool suite_collect( std::vector<suite_case> & pending, std::vector<suite_result> & results,
    int ( *count )(), const char * ( *name_at )( int ), const suite_exec * execs, int nexecs,
    int ( *group_of )( const char * name ), const std::string & dir, const char * exe_suffix )
{
    // Each case is built in place from its name's characters, never copied
    // or moved as a whole string: under MemorySanitizer the prebuilt
    // libstdc++ copies a short (SSO) string without telling the shadow, so a
    // copied name reads as uninitialised (tools/run_msan.sh).
    const size_t first = pending.size();
    const int num_registered = count();
    pending.resize( first + (size_t) num_registered + (size_t) nexecs );
    for ( int i = 0; i < num_registered; i++ )
    {
        suite_case & item = pending[first + (size_t) i];
        item.name = name_at( i );
        item.group = 0;
    }
    for ( int i = 0; i < nexecs; i++ )
    {
        suite_case & item = pending[first + (size_t) num_registered + (size_t) i];
        item.name = execs[i].name;
        item.exec_path = dir + "/" + execs[i].file + exe_suffix;
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
    for ( suite_case_rss_bound_t * entry = *suite_case_rss_bound_head(); entry != NULL; entry = entry->next )
    {
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
        pending[i].group = group_of ? group_of( pending[i].name.c_str() ) : 0;
    std::stable_sort( pending.begin(), pending.end(), []( const suite_case & a, const suite_case & b ) {
        if ( a.group != b.group )
            return a.group < b.group;
        return suite_layer_of( a.name ) < suite_layer_of( b.name );
    } );
    for ( size_t i = 0; i < pending.size(); i++ )
    {
        suite_result result;
        result.name = pending[i].name;
        result.exec = !pending[i].exec_path.empty();
        result.done = false;
        result.seconds = 0;
        result.failed = false;
        result.peak_rss_mb = -1;
        results.push_back( result );
    }
    return true;
}

inline int suite_finish( const std::vector<suite_result> & results, bool passed )
{
    suite_print_report( results );
    if ( !passed )
        return 1;
    printf( "\n*** ALL TESTS PASSED ***\n\n" );
    fflush( stdout );
    return 0;
}

#if !defined(_WIN32)

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
            _exit( 127 );
        if ( pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO )
            close( pipefd[1] );
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
    const char * ( *name_at )( int ),
    const suite_exec * execs, int nexecs,
    int ( *group_of )( const char * name ) = NULL )
{
    if ( argc >= 2 && std::strcmp( argv[1], "--run" ) == 0 )
    {
        if ( argc < 3 || argv[2][0] == '-' )
            return suite_usage( argv );
        for ( int i = 3; i < argc; i++ )
            if ( argv[i][0] == '-' )
                return suite_usage( argv );
        // Under a suite run the parent already captures this process, so
        // stream straight through; a second capture would lose lines when the
        // parent kills the process group at the time limit.
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

    const int jobs = suite_job_count();
    const double limit_s = 120.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE; // Release as written; Debug/sanitizer scaled (test/time_scale.h)
    struct running_t
    {
        size_t index;
        pid_t pid;
        std::chrono::steady_clock::time_point start;
        bool killed;
        int group;
        int out_fd;
    };
    const bool capture = !suite_env_is_set( "ROCKETNET_TEST_VERBOSE" );
    if ( capture )
        setenv( "ROCKETNET_TEST_SUITE_CAPTURE", "1", 1 );
    std::vector<running_t> running;
    size_t next = 0;
    size_t printed = 0;
    bool passed = true;

    while ( passed && ( next < pending.size() || !running.empty() ) )
    {
        while ( (int) running.size() < jobs && next < pending.size() && ( running.empty() || running[0].group == pending[next].group ) )
        {
            const size_t index = next++;
            const suite_case & item = pending[index];
            suite_result & result = results[index];
            const std::string path = item.exec_path.empty() ? self : item.exec_path;
            std::vector<char> arg0( path.begin(), path.end() );
            arg0.push_back( '\0' );
            std::vector<char> arg_run;
            std::vector<char> arg_name;
            char * spawn_argv[4];
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
                spawn_argv[1] = NULL;
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
                    fcntl( pipefd[0], F_SETFL, fl | O_NONBLOCK );
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
                posix_spawn_file_actions_destroy( &actions );
            if ( pipefd[1] >= 0 )
                close( pipefd[1] );
            if ( spawned != 0 )
            {
                if ( pipefd[0] >= 0 )
                    close( pipefd[0] );
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
#if defined(__APPLE__)
            result.peak_rss_mb = got > 0 ? (long) ( usage.ru_maxrss / ( 1024 * 1024 ) ) : -1; // bytes on macOS
#else
            result.peak_rss_mb = got > 0 ? (long) ( usage.ru_maxrss / 1024 ) : -1; // kilobytes on Linux
#endif
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
            suite_check_rss( result, suite_case_rss_mb( result.name ) );
            if ( running[i].out_fd >= 0 )
            {
                suite_drain( running[i].out_fd, result.output );
                close( running[i].out_fd );
            }
            result.done = true;
            running.erase( running.begin() + (std::ptrdiff_t) i );
        }

        passed = suite_print_ready( results, printed, capture );
        if ( passed && !running.empty() )
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    }

    // a failure ends the run: the cases still running are killed and reaped
    for ( size_t i = 0; i < running.size(); i++ )
    {
        kill( -running[i].pid, SIGKILL );
        kill( running[i].pid, SIGKILL );
        int status = 0;
        while ( waitpid( running[i].pid, &status, 0 ) < 0 && errno == EINTR ) {}
        if ( running[i].out_fd >= 0 )
            close( running[i].out_fd );
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

inline int suite_main( int argc, char ** argv,
    int ( *worker )( const char * name ),
    int ( *count )(),
    const char * ( *name_at )( int ),
    const suite_exec * execs, int nexecs,
    int ( *group_of )( const char * name ) = NULL )
{
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

    const int jobs = suite_job_count();
    const double limit_s = 120.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE; // Release as written; Debug/sanitizer scaled (test/time_scale.h)
    struct running_t
    {
        size_t index;
        HANDLE process;
        std::chrono::steady_clock::time_point start;
        bool killed;
        int group;
        HANDLE out;
    };
    const bool capture = !suite_env_is_set( "ROCKETNET_TEST_VERBOSE" );
    std::vector<running_t> running;
    size_t next = 0;
    size_t printed = 0;
    bool passed = true;

    while ( passed && ( next < pending.size() || !running.empty() ) )
    {
        while ( (int) running.size() < jobs && next < pending.size() && ( running.empty() || running[0].group == pending[next].group ) )
        {
            const size_t index = next++;
            const suite_case & item = pending[index];
            suite_result & result = results[index];
            const std::string path = item.exec_path.empty() ? self : item.exec_path;
            const std::wstring wide_path = rocketnet_test_wide_path( path );
            std::wstring command = L"\"" + wide_path + L"\"";
            if ( item.exec_path.empty() )
                command += L" --run " + rocketnet_test_utf8_to_wide( item.name );
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
                    if ( read_end )
                        CloseHandle( read_end );
                    if ( write_end )
                        CloseHandle( write_end );
                    result.done = true;
                    result.failed = true;
                    result.detail = item.name + " failed to spawn";
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
            if ( write_end )
                CloseHandle( write_end );
            if ( !created )
            {
                if ( read_end )
                    CloseHandle( read_end );
                result.done = true;
                result.failed = true;
                result.detail = item.name + " failed to spawn";
                continue;
            }
            CloseHandle( info.hThread );
            running_t child;
            child.index = index;
            child.process = info.hProcess;
            child.start = std::chrono::steady_clock::now();
            child.killed = false;
            child.group = item.group;
            child.out = read_end;
            running.push_back( child );
        }

        if ( !running.empty() )
        {
            HANDLE handles[8];
            const int n = (int) running.size();
            for ( int i = 0; i < n; i++ )
                handles[i] = running[i].process;
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
            suite_check_rss( result, suite_case_rss_mb( result.name ) );
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
            CloseHandle( running[i].out );
    }
    return suite_finish( results, passed );
}

#endif
