/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional suite. Each test is a process. Sibling binaries that open a
    socket or start a real transport are tests of this suite, under the same
    120s failure limit (test/suite_runner.h).
*/

#include "rocketnet_internal.h"
#include "../test/suite_runner.h"
#include "../test/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
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

int rocketnet_posix_hard_killed_child_main( int argc, char ** argv );
int rocketnet_posix_hard_killed_peer_test_group( const char * name );

int main( int argc, char ** argv )
{
    // the child client of the hard-killed peer cases
    // (functional/posix_hard_killed_peer.cpp), started by posix_spawn and
    // ended by the case with SIGKILL
    if ( argc >= 2 && strcmp( argv[1], "--posix-hard-killed-child" ) == 0 )
        return rocketnet_posix_hard_killed_child_main( argc, argv );

#if defined(_WIN32)
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize" },
        { "test_transport_control", "test_transport_control" },
        { "test_iocp", "test_iocp" },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 3 );
#elif defined(__APPLE__)
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize" },
        { "test_transport_control", "test_transport_control" },
        { "test_msgx", "test_msgx" },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 3, rocketnet_posix_hard_killed_peer_test_group );
#elif defined(__linux__)
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize" },
        { "test_transport_control", "test_transport_control" },
#if ROCKETNET_XDP
        { "test_xdp", "test_xdp" },
#endif
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, sizeof(execs) / sizeof(execs[0]) );
#else
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize" },
        { "test_transport_control", "test_transport_control" },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 2 );
#endif
}
