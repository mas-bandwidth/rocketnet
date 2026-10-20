/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional suite. Each test is a process. Sibling binaries that open a
    socket or start a real driver are tests of this suite, under the same
    60s warning and 120s failure limits.
*/

#include "rocketnet_internal.h"
#include "../test/suite_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
int rocketnet_posix_hard_killed_child_main( const char * host, int port );
int rocketnet_xdp_hard_killed_child_main( const char * server_address );
#endif

int rocketnet_functional_test_count();
const char * rocketnet_functional_test_name( int index );
void rocketnet_functional_prepare();
int rocketnet_functional_run_named( const char * name );

static int rocketnet_functional_worker( const char * name )
{
#if defined(_WIN32)
    // MSVC treats _IOLBF as full buffering and rejects a zero buffer size.
    // A 4096-byte buffer paired with explicit fflush keeps each worker's
    // test-name line in a single pipe write so parallel workers never tear lines.
    if ( setvbuf( stdout, NULL, _IOFBF, 4096 ) != 0 ||
         setvbuf( stderr, NULL, _IONBF, 0 ) != 0 )
        return 1;
#else
    setvbuf( stdout, NULL, _IOLBF, 0 );
    setvbuf( stderr, NULL, _IOLBF, 0 );
#endif

#if defined(_WIN32)
    const int cleared = _putenv_s( "ROCKETNET_LOG_LEVEL", "" );
#else
    const int cleared = unsetenv( "ROCKETNET_LOG_LEVEL" );
#endif
    if ( cleared != 0 )
        return 1;

    rocketnet_set_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_functional_prepare();
    const int result = rocketnet_functional_run_named( name );
    rocketnet_term();
    fflush( stdout );
    return result;
}

int main( int argc, char ** argv )
{
#if defined(__APPLE__) || defined(__linux__)
    // the hard-killed-peer case starts this same binary as its child; give it
    // an absolute path so posix_spawn does not depend on the caller's cwd.
    if ( argc >= 4 && strcmp( argv[1], "--posix-hard-killed-child" ) == 0 )
        return rocketnet_posix_hard_killed_child_main( argv[2], atoi( argv[3] ) );
    // the xdp hard-killed case starts this binary in the far namespace as the
    // client it kills
    if ( argc >= 3 && strcmp( argv[1], "--xdp-hard-killed-child" ) == 0 )
        return rocketnet_xdp_hard_killed_child_main( argv[2] );
#endif

#if defined(__APPLE__)
    {
        char self_path[4096];
        uint32_t self_size = (uint32_t) sizeof(self_path);
        if ( _NSGetExecutablePath( self_path, &self_size ) == 0 )
            setenv( "ROCKETNET_FUNCTIONAL_SELF", self_path, 1 );
    }
#endif

#if defined(_WIN32)
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize", true },
        { "test_transport_control", "test_transport_control", true },
        { "test_iocp", "test_iocp", true },
    };
    static const char * skip_lines[] = {
        "test_platform_batch_pump_refused_destination [SKIPPED on windows: posix raw-socket injector]",
        "test_platform_pump_zero_length_chaff [SKIPPED on windows: posix raw-socket injector]",
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 3, skip_lines, 2, false );
#elif defined(__APPLE__)
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize", true },
        { "test_transport_control", "test_transport_control", true },
        { "test_msgx", "test_msgx", true },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 3, NULL, 0, false );
#elif defined(__linux__)
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize", true },
        { "test_transport_control", "test_transport_control", true },
        { "test_xdp", "test_xdp", true },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 3, NULL, 0, false );
#else
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize", true },
        { "test_transport_control", "test_transport_control", true },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 2, NULL, 0, false );
#endif
}
