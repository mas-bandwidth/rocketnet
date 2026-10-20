/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Parallel suite runner. Each test is a process. A test over 60s prints a
    warning. A test over 120s is killed and fails the suite (a Debug or
    sanitizer build scales the 120s: test/time_scale.h). The ten slowest
    are printed at the end. Each case's peak resident set (wait4 ru_maxrss on
    POSIX, PeakWorkingSetSize on Windows) is printed at the end as
    `<case> peak_rss_mb=<n> bound_mb=<b>`, and a case over the bound fails the
    suite (ROCKETNET_TEST_MAX_CASE_RSS_MB, overridable by that environment
    variable; a sanitizer build prints the peaks and does not enforce the bound
    unless the environment variable sets one). strict_names keeps every line inside the
    names-only shape tools/check_test_environment.py accepts, because that
    checker merges stderr into stdout and is not ours to change.
*/

#pragma once

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

// The largest peak resident set one case process may reach, in MiB: the larger
// of 256 and 1.5 times the largest case measured on a Linux Release build
// (465 MiB, test_server_driver_pin_gso, 2026-10-19), so 698; never over 1024.
// A case over it fails the suite. The environment variable of the same name
// overrides it with a positive value.
#define ROCKETNET_TEST_MAX_CASE_RSS_MB 698

struct suite_exec
{
    const char *name;
    const char *file;
    bool required;
};

struct suite_case
{
    std::string name;
    std::string exec_path; // empty: re-exec this binary with --run name
    bool skipped;
    std::string skip_line;
};

struct suite_result
{
    std::string name;
    double seconds;
    bool skipped;
    bool failed;
    std::string detail;
    long peak_rss_mb; // -1: not measured
};

// The bound in MiB, or 0 when it is not enforced (a sanitizer build with no
// environment override: sanitizer shadow memory is not the case's footprint).
inline long suite_max_case_rss_mb()
{
    if ( const char * env = std::getenv( "ROCKETNET_TEST_MAX_CASE_RSS_MB" ) )
    {
        const long requested = std::atol( env );
        if ( requested > 0 )
            return requested;
    }
#if defined(ROCKETNET_FUNCTIONAL_SANITIZED)
    return 0;
#else
    return ROCKETNET_TEST_MAX_CASE_RSS_MB;
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
    if ( const char * env = std::getenv( "ROCKETNET_TEST_JOBS" ) )
    {
        const int requested = std::atoi( env );
        if ( requested > 0 )
            jobs = (unsigned) requested;
    }
    if ( jobs > 8 )
        jobs = 8;
    return (int) jobs;
}

inline void suite_print_summary( const std::vector<suite_result> & results, bool strict_names )
{
    // A passing run with no verbose switch prints no summary. A failure
    // still prints, and the unit suite (strict_names) is unchanged.
    if ( !strict_names && std::getenv( "ROCKETNET_TEST_VERBOSE" ) == NULL )
    {
        bool quiet = true;
        for ( size_t i = 0; i < results.size(); i++ )
        {
            if ( results[i].failed )
            {
                quiet = false;
                break;
            }
        }
        if ( quiet )
            return;
    }
    std::vector<suite_result> ran;
    for ( size_t i = 0; i < results.size(); i++ )
        if ( !results[i].skipped )
            ran.push_back( results[i] );
    std::sort( ran.begin(), ran.end(), []( const suite_result & a, const suite_result & b ) {
        return a.seconds > b.seconds;
    } );
    const int shown = (int) ran.size() < 10 ? (int) ran.size() : 10;
    if ( !strict_names )
        printf( "slowest ten:\n" );
    for ( int i = 0; i < shown; i++ )
    {
        if ( strict_names )
            printf( "test_timing_slowest_%02d [SKIPPED %.3fs %s]\n", i + 1, ran[i].seconds, ran[i].name.c_str() );
        else
            printf( "  %8.3fs  %s\n", ran[i].seconds, ran[i].name.c_str() );
    }
    std::vector<suite_result> by_rss = ran;
    std::sort( by_rss.begin(), by_rss.end(), []( const suite_result & a, const suite_result & b ) {
        return a.peak_rss_mb > b.peak_rss_mb;
    } );
    const long bound_mb = suite_max_case_rss_mb();
    if ( !strict_names )
        printf( "peak rss, largest first:\n" );
    for ( size_t i = 0; i < by_rss.size(); i++ )
    {
        if ( by_rss[i].peak_rss_mb < 0 )
            continue;
        if ( strict_names )
        {
            if ( i >= 10 )
                break;
            printf( "test_timing_rss_%02d [SKIPPED %s peak_rss_mb=%ld bound_mb=%ld]\n", (int) i + 1, by_rss[i].name.c_str(), by_rss[i].peak_rss_mb, bound_mb );
        }
        else
            printf( "%s peak_rss_mb=%ld bound_mb=%ld\n", by_rss[i].name.c_str(), by_rss[i].peak_rss_mb, bound_mb );
    }
    int warnings = 0;
    int fails = 0;
    for ( size_t i = 0; i < ran.size(); i++ )
    {
        if ( ran[i].seconds > 60.0 )
        {
            warnings++;
            if ( strict_names )
                printf( "test_timing_warning_%02d [SKIPPED %.3fs %s over 60s]\n", warnings, ran[i].seconds, ran[i].name.c_str() );
            else
                printf( "warning: %s %.3fs (over 60s)\n", ran[i].name.c_str(), ran[i].seconds );
        }
        if ( ran[i].failed )
        {
            fails++;
            if ( strict_names )
                printf( "test_timing_fail_%02d [SKIPPED %s]\n", fails, ran[i].detail.c_str() );
            else
                printf( "FAIL: %s\n", ran[i].detail.c_str() );
        }
    }
    fflush( stdout );
}

inline int suite_usage( char ** argv )
{
    fprintf( stderr, "usage: %s [--run <case>]\n", argv[0] );
    return 2;
}

#if !defined(_WIN32)

// A passing case prints its name, or a name plus " [SKIPPED". Anything else
// stays in the capture unless the case fails. The verbose switch does not
// capture, so the child prints straight through. strict_names (the unit
// suite) does not capture.
inline bool suite_passing_line( const std::string & line )
{
    if ( line.empty() )
        return false;
    const char first = line[0];
    if ( !( ( first >= 'A' && first <= 'Z' ) || ( first >= 'a' && first <= 'z' ) || first == '_' ) )
        return false;
    size_t n = 1;
    while ( n < line.size() )
    {
        const char c = line[n];
        const bool ident = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) ||
                           ( c >= '0' && c <= '9' ) || c == '_';
        if ( !ident )
            break;
        n++;
    }
    if ( n == line.size() )
        return true;
    return line.compare( n, 9, " [SKIPPED" ) == 0;
}

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

inline void suite_emit_captured( const std::string & output, bool show_all )
{
    std::string pending = output;
    if ( !pending.empty() && pending.back() != '\n' )
        pending.push_back( '\n' );
    size_t i = 0;
    while ( i < pending.size() )
    {
        const size_t nl = pending.find( '\n', i );
        std::string line = pending.substr( i, nl == std::string::npos ? std::string::npos : nl - i );
        if ( !line.empty() && line.back() == '\r' )
            line.pop_back();
        if ( show_all || suite_passing_line( line ) )
            printf( "%s\n", line.c_str() );
        if ( nl == std::string::npos )
            break;
        i = nl + 1;
    }
    fflush( stdout );
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
    const char ** skip_lines, int nskips,
    bool strict_names )
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
        if ( std::getenv( "ROCKETNET_TEST_SUITE_CAPTURE" ) != NULL )
        {
            unsetenv( "ROCKETNET_TEST_SUITE_CAPTURE" );
            return worker( argv[2] );
        }
        if ( !strict_names && std::getenv( "ROCKETNET_TEST_VERBOSE" ) == NULL )
            return suite_run_names_only( worker, argv[2] );
        return worker( argv[2] );
    }
    for ( int i = 1; i < argc; i++ )
        if ( argv[i][0] == '-' )
            return suite_usage( argv );

    const std::string self = suite_executable_path();
    if ( self.empty() )
    {
        fprintf( stderr, "FAIL: could not resolve this executable\n" );
        return 1;
    }
    const std::string dir = suite_directory_of( self );

    std::vector<suite_case> pending;
    for ( int i = 0; i < count(); i++ )
    {
        suite_case item;
        item.name = name_at( i );
        item.skipped = false;
        pending.push_back( item );
    }
    for ( int i = 0; i < nexecs; i++ )
    {
        suite_case item;
        item.name = execs[i].name;
        item.exec_path = dir + "/" + execs[i].file;
        if ( !suite_file_exists( item.exec_path ) )
        {
            if ( execs[i].required )
            {
                fprintf( stderr, "FAIL: %s missing at %s\n", execs[i].name, item.exec_path.c_str() );
                return 1;
            }
            item.skipped = true;
            item.skip_line = std::string( execs[i].name ) + " [SKIPPED not built on this platform]";
        }
        pending.push_back( item );
    }

    for ( int i = 0; i < nskips; i++ )
    {
        printf( "%s\n", skip_lines[i] );
        fflush( stdout );
    }

    const int jobs = suite_job_count();
    const double limit_s = 120.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE; // Release as written; Debug/sanitizer scaled (test/time_scale.h)
    struct running_t
    {
        std::string name;
        pid_t pid;
        std::chrono::steady_clock::time_point start;
        bool killed;
        int out_fd;
        std::string output;
    };
    const bool suite_capture = !strict_names && std::getenv( "ROCKETNET_TEST_VERBOSE" ) == NULL;
    if ( suite_capture )
        setenv( "ROCKETNET_TEST_SUITE_CAPTURE", "1", 1 );
    std::vector<running_t> running;
    std::vector<suite_result> results;
    size_t next = 0;
    int failures = 0;

    while ( next < pending.size() || !running.empty() )
    {
        while ( (int) running.size() < jobs && next < pending.size() )
        {
            const suite_case item = pending[next++];
            if ( item.skipped )
            {
                printf( "%s\n", item.skip_line.c_str() );
                fflush( stdout );
                suite_result result;
                result.name = item.name;
                result.seconds = 0;
                result.skipped = true;
                result.failed = false;
                result.peak_rss_mb = -1;
                results.push_back( result );
                continue;
            }

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
            if ( suite_capture )
            {
                if ( pipe( pipefd ) != 0 )
                {
                    suite_result result;
                    result.name = item.name;
                    result.seconds = 0;
                    result.skipped = false;
                    result.failed = true;
                    result.peak_rss_mb = -1;
                    result.detail = item.name + " failed to spawn";
                    results.push_back( result );
                    failures++;
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
                suite_result result;
                result.name = item.name;
                result.seconds = 0;
                result.skipped = false;
                result.failed = true;
                result.peak_rss_mb = -1;
                result.detail = item.name + " failed to spawn";
                results.push_back( result );
                failures++;
                continue;
            }
            running_t child;
            child.name = item.name;
            child.pid = pid;
            child.start = std::chrono::steady_clock::now();
            child.killed = false;
            child.out_fd = pipefd[0];
            running.push_back( child );
        }

        for ( size_t i = 0; i < running.size(); )
        {
            int status = 0;
            struct rusage usage;
            std::memset( &usage, 0, sizeof( usage ) );
            const pid_t got = wait4( running[i].pid, &status, WNOHANG, &usage );
            const double seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - running[i].start ).count();
            if ( got == 0 )
            {
                suite_drain( running[i].out_fd, running[i].output );
                if ( !running[i].killed && seconds > limit_s )
                {
                    kill( -running[i].pid, SIGKILL );
                    kill( running[i].pid, SIGKILL );
                    running[i].killed = true;
                }
                i++;
                continue;
            }
            suite_result result;
            result.name = running[i].name;
            result.seconds = seconds;
            result.skipped = false;
            result.failed = false;
#if defined(__APPLE__)
            result.peak_rss_mb = got > 0 ? (long) ( usage.ru_maxrss / ( 1024 * 1024 ) ) : -1; // bytes on macOS
#else
            result.peak_rss_mb = got > 0 ? (long) ( usage.ru_maxrss / 1024 ) : -1; // kilobytes on Linux
#endif
            if ( got < 0 )
            {
                result.failed = true;
                result.detail = running[i].name + " wait failed";
            }
            else if ( running[i].killed || seconds > limit_s )
            {
                result.failed = true;
                result.seconds = seconds < limit_s ? limit_s : seconds;
                { char limit_text[64]; snprintf( limit_text, sizeof( limit_text ), " exceeded %.0fs", limit_s ); result.detail = running[i].name + limit_text; }
            }
            else if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 )
            {
                result.failed = false;
            }
            else if ( WIFEXITED( status ) )
            {
                result.failed = true;
                char buf[64];
                snprintf( buf, sizeof( buf ), " exit %d", WEXITSTATUS( status ) );
                result.detail = running[i].name + buf;
            }
            else
            {
                result.failed = true;
                char buf[64];
                snprintf( buf, sizeof( buf ), " signal %d", WIFSIGNALED( status ) ? WTERMSIG( status ) : 0 );
                result.detail = running[i].name + buf;
            }
            suite_check_rss( result, suite_max_case_rss_mb() );
            if ( running[i].out_fd >= 0 )
            {
                suite_drain( running[i].out_fd, running[i].output );
                close( running[i].out_fd );
                running[i].out_fd = -1;
                suite_emit_captured( running[i].output, result.failed );
            }
            if ( result.failed )
                failures++;
            results.push_back( result );
            running.erase( running.begin() + (std::ptrdiff_t) i );
        }

        if ( !running.empty() )
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    }

    suite_print_summary( results, strict_names );
    return failures == 0 ? 0 : 1;
}

#else

inline int suite_main( int argc, char ** argv,
    int ( *worker )( const char * name ),
    int ( *count )(),
    const char * ( *name_at )( int ),
    const suite_exec * execs, int nexecs,
    const char ** skip_lines, int nskips,
    bool strict_names )
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
        fprintf( stderr, "FAIL: could not resolve this executable\n" );
        return 1;
    }
    const std::string dir = suite_directory_of( self );

    std::vector<suite_case> pending;
    for ( int i = 0; i < count(); i++ )
    {
        suite_case item;
        item.name = name_at( i );
        item.skipped = false;
        pending.push_back( item );
    }
    for ( int i = 0; i < nexecs; i++ )
    {
        suite_case item;
        item.name = execs[i].name;
        item.exec_path = dir + "\\" + execs[i].file + ".exe";
        if ( !suite_file_exists( item.exec_path ) )
        {
            if ( execs[i].required )
            {
                fprintf( stderr, "FAIL: %s missing at %s\n", execs[i].name, item.exec_path.c_str() );
                return 1;
            }
            item.skipped = true;
            item.skip_line = std::string( execs[i].name ) + " [SKIPPED not built on this platform]";
        }
        pending.push_back( item );
    }
    for ( int i = 0; i < nskips; i++ )
    {
        printf( "%s\n", skip_lines[i] );
        fflush( stdout );
    }

    const int jobs = suite_job_count();
    const double limit_s = 120.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE; // Release as written; Debug/sanitizer scaled (test/time_scale.h)
    struct running_t
    {
        std::string name;
        HANDLE process;
        std::chrono::steady_clock::time_point start;
        bool killed;
    };
    std::vector<running_t> running;
    std::vector<suite_result> results;
    size_t next = 0;
    int failures = 0;

    while ( next < pending.size() || !running.empty() )
    {
        while ( (int) running.size() < jobs && next < pending.size() )
        {
            const suite_case item = pending[next++];
            if ( item.skipped )
            {
                printf( "%s\n", item.skip_line.c_str() );
                fflush( stdout );
                suite_result result;
                result.name = item.name;
                result.seconds = 0;
                result.skipped = true;
                result.failed = false;
                result.peak_rss_mb = -1;
                results.push_back( result );
                continue;
            }
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
            PROCESS_INFORMATION info;
            std::memset( &info, 0, sizeof( info ) );
            if ( wide_path.empty() || !CreateProcessW( wide_path.c_str(), mutable_command.data(), NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &startup, &info ) )
            {
                suite_result result;
                result.name = item.name;
                result.seconds = 0;
                result.skipped = false;
                result.failed = true;
                result.peak_rss_mb = -1;
                result.detail = item.name + " failed to spawn";
                results.push_back( result );
                failures++;
                continue;
            }
            CloseHandle( info.hThread );
            running_t child;
            child.name = item.name;
            child.process = info.hProcess;
            child.start = std::chrono::steady_clock::now();
            child.killed = false;
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
            suite_result result;
            result.name = running[i].name;
            result.seconds = seconds;
            result.skipped = false;
            result.failed = false;
            result.peak_rss_mb = measured ? (long) ( memory.PeakWorkingSetSize / ( 1024 * 1024 ) ) : -1;
            if ( running[i].killed || seconds > limit_s )
            {
                result.failed = true;
                result.seconds = seconds < limit_s ? limit_s : seconds;
                { char limit_text[64]; snprintf( limit_text, sizeof( limit_text ), " exceeded %.0fs", limit_s ); result.detail = running[i].name + limit_text; }
            }
            else if ( code == 0 )
            {
                result.failed = false;
            }
            else
            {
                result.failed = true;
                char buf[64];
                snprintf( buf, sizeof( buf ), " exit %lu", (unsigned long) code );
                result.detail = running[i].name + buf;
            }
            suite_check_rss( result, suite_max_case_rss_mb() );
            if ( result.failed )
                failures++;
            results.push_back( result );
            running.erase( running.begin() + (std::ptrdiff_t) i );
        }
    }

    suite_print_summary( results, strict_names );
    return failures == 0 ? 0 : 1;
}

#endif
