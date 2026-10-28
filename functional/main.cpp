/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional suite. Each test is a process, and cases run in parallel (one
    job per core, ROCKETNET_TEST_JOBS to change it); a case that must not
    share the host runs in its own group. Sibling binaries that open a socket
    or start a real transport are tests of this suite, under the same
    120s failure limit (tests/suite_runner.h).
*/

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_driver_loopback.h"
#if ROCKETNET_DEV
#include "channel_harness.h"
#endif // #if ROCKETNET_DEV
#include "../tests/plan_rss.h"
#include "../tests/suite_runner.h"
#include "../tests/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rocketnet_functional_test_count();
const char * rocketnet_functional_test_name( int index );
void rocketnet_functional_prepare();
int rocketnet_functional_run_named( const char * name );

// the allocator context rocketnet_init was given, NULL after rocketnet_term
// (sdk/rocketnet.cpp; declared for Linux in rocketnet_platform.h)
extern void * rocketnet_global_context;

static int rocketnet_functional_worker( const char * name )
{
#if defined( _WIN32 )
    // MSVC treats _IOLBF as full buffering and rejects a zero buffer size.
    // A 4096-byte buffer paired with explicit fflush keeps each worker's
    // test-name line in a single pipe write.

    if ( setvbuf( stdout, NULL, _IOFBF, 4096 ) != 0 ||
         setvbuf( stderr, NULL, _IONBF, 0 ) != 0 )
        return 1;
#else
    setvbuf( stdout, NULL, _IOLBF, 0 );
    setvbuf( stderr, NULL, _IOLBF, 0 );
#endif // #if defined( _WIN32 )

#if defined( _WIN32 )
    const int cleared = _putenv_s( "ROCKETNET_LOG_LEVEL", "" );
#else
    const int cleared = unsetenv( "ROCKETNET_LOG_LEVEL" );
#endif // #if defined( _WIN32 )
    if ( cleared != 0 )
        return 1;

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    // the SDK's bytes are counted, so the process is held to its plan
    // (tests/plan_rss.h): it reports that bound when the case passed with the
    // counter still installed, and keeps the suite-wide bound otherwise

    if ( rocketnet_init( NULL, plan_rss_malloc, plan_rss_free ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_functional_prepare();

    const int result = rocketnet_functional_run_named( name );

    // a case that ended the library or re-initialised it with a context of
    // its own installed its own allocator, which may hold that context: it is
    // not called. Otherwise one probe allocation names the installed allocator

    if ( result == 0 && rocketnet_library_initialized() && rocketnet_global_context == NULL &&
         plan_rss_installed( rocketnet_malloc, rocketnet_free ) )
        plan_rss_report();
    rocketnet_term();
    fflush( stdout );

    return result;
}

int rocketnet_posix_hard_killed_child_main( int argc, char ** argv );
suite_exec rocketnet_unit_case_memory_exec();

// The nightly tier: exact names, or a prefix ending in '*'. On macOS the
// hard-killed client cells on the portable server wait out the real 10 s
// token timeout, and the connect/disconnect x1000 pairs other than msgx/msgx
// repeat a pair a per-commit case keeps.
static const char * const rocketnet_functional_nightly_cases[] = {
#if defined( __APPLE__ )
    "test_posix_hard_killed_client_portable_*",
    "test_posix_connect_disconnect_1000_portable_*",
    "test_posix_connect_disconnect_1000_msgx_portable",
#endif // #if defined( __APPLE__ )
#if defined( __linux__ )
    // XDP cases share the host's XDP state (the pre-flight answer, the commit
    // barrier, interfaces): they cannot run beside other cases, so they run in
    // the Linux nightly tier, which runs serially (nightly.yml)
    "test_xdp_transport_*",
    "test_session_reconnect_isolation_xdp",
#endif // #if defined( __linux__ )
#if defined( _WIN32 )
    // the RIO and IOCP x1000 server cycles, 16-45 s each on the Windows bench
    // (captain, 2026-10-29); the portable x1000 cycles stay on every commit
    "test_rio_transport_windows_server_restart_1000",
    "test_rio_transport_windows_server_create_destroy_1000",
    "test_iocp_transport_windows_server_restart_1000",
    "test_iocp_transport_windows_server_create_destroy_1000",
#endif // #if defined( _WIN32 )
    NULL,
};

// Every functional case runs in parallel (Glenn 2026-10-29: "You are
// literally supposed to run all functional tests in parallel"). A case that
// cannot share the host is fixed or moved to the nightly tier
// (SUITE_GROUP_NIGHTLY, ROCKETNET_TEST_NIGHTLY=1, nightly.yml), never run
// alone per commit. The nightly tier holds every case over a second on any
// operating system, measured 2026-10-29 (ROCKETNET_TEST_REPORT=all).
static int rocketnet_functional_test_group( const char * name )
{
    for ( const char * const * nightly = rocketnet_functional_nightly_cases; *nightly; ++nightly )
    {
        const size_t length = strlen( *nightly );
        if ( length > 0 && ( *nightly )[length - 1] == '*' ? strncmp( name, *nightly, length - 1 ) == 0 : strcmp( name, *nightly ) == 0 )
            return SUITE_GROUP_NIGHTLY;
    }

    return 0;
}

// test_transport_control (a sibling binary, test_transport_control.cpp) runs
// test_buffered_control_pressure at 300 and then at ROCKETNET_MAX_CLIENTS
// clients, one process: each builds a loopback server from the development
// config with the default packet payload limit and one reassembly slot, and
// seeds a real ACK observation into every slot, so the per-slot ACK
// history becomes resident by design. Its
// footprint is those servers' memory plans, not the suite-wide bound. The
// 300-client server is destroyed before the full-capacity one is created,
// but an allocator may keep its freed pages resident: macOS does (a 994 MiB
// peak with both runs, 782 MiB with the full-capacity run alone, while Linux
// measures 772 MiB either way), so both plans count. Loopback has no plan
// row: the portable plan stands in for its common storage and each server's
// loopback queue is added, as the other loopback full-capacity cases do,
// plus 32 MiB for the process. The servers it builds after these have at
// most 12 clients and reuse that memory.

static long rocketnet_transport_control_rss_bound_mb()
{
    uint64_t planned = 0;

    for ( int clients : { 300, ROCKETNET_MAX_CLIENTS } )
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        config.max_clients = clients;
        config.max_fragmented_messages = 1;

        rocketnet_server_memory_plan_t plan;
        if ( !rocketnet_server_transport_memory_plan( &config, "portable", plan ) )
            return 0; // a refused plan keeps the strict suite-wide bound

        const uint64_t queue = rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS ) + rocketnet_allocation_overhead();
        if ( !rocketnet_memory_add( planned, 1, plan.total_bytes ) || !rocketnet_memory_add( planned, 1, queue ) )
            return 0;
    }

    const long bound = suite_rss_bound_mb( planned, 32 );
    char verbose[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", verbose, sizeof( verbose ) ) )
        printf( "test_transport_control rss_budget planned_bytes=%llu margin_mb=32 bound_mb=%ld\n",
                (unsigned long long) planned, bound );
    return bound;
}

SUITE_CASE_RSS_BOUND( test_transport_control, rocketnet_transport_control_rss_bound_mb );

int main( int argc, char ** argv )
{
    // the child client of the hard-killed client cases
    // (functional/posix_hard_killed_client.cpp), started by posix_spawn and
    // ended by the case with SIGKILL
#if ROCKETNET_DEV
    if ( argc >= 2 && strcmp( argv[1], "--posix-hard-killed-child" ) == 0 )
        return rocketnet_posix_hard_killed_child_main( argc, argv );

    if ( !rocketnet_channel_delivery_ring_self_test() )
    {
        fprintf( stderr, "error: channel harness delivery sequence self-test failed\n" );
        return 1;
    }
#endif // #if ROCKETNET_DEV
#if !ROCKETNET_DEV
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, NULL, 0, rocketnet_functional_test_group, ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS );
#elif defined( _WIN32 )
    const suite_exec execs[] = {
        rocketnet_unit_case_memory_exec(),
        { "test_serialize", "test_serialize", NULL, true },
        { "test_transport_control", "test_transport_control", NULL, true },
        { "test_iocp", "test_iocp", NULL, true },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 4, rocketnet_functional_test_group, ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS );
#elif defined( __APPLE__ )
    const suite_exec execs[] = {
        rocketnet_unit_case_memory_exec(),
        { "test_serialize", "test_serialize", NULL, true },
        { "test_transport_control", "test_transport_control", NULL, true },
        { "test_msgx", "test_msgx", NULL, true },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 4, rocketnet_functional_test_group, ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS );
#elif defined( __linux__ )
    const suite_exec execs[] = {
        rocketnet_unit_case_memory_exec(),
        { "test_serialize", "test_serialize", NULL, true },
        { "test_transport_control", "test_transport_control", NULL, true },
#if ROCKETNET_XDP
        { "test_xdp", "test_xdp", NULL, true },
#endif // #if ROCKETNET_XDP
    };

    // io_uring cases are bounded by RLIMIT_MEMLOCK as well as by the one job:
    // a limit under one case's need fails the suite before it starts, naming
    // the limit (tests/suite_runner.h, tools/check_memlock.sh)

    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, sizeof( execs ) / sizeof( execs[0] ), rocketnet_functional_test_group, ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS, 0, true );
#else
    const suite_exec execs[] = {
        rocketnet_unit_case_memory_exec(),
        { "test_serialize", "test_serialize", NULL, true },
        { "test_transport_control", "test_transport_control", NULL, true },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, sizeof( execs ) / sizeof( execs[0] ), rocketnet_functional_test_group, ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS );
#endif // #if !ROCKETNET_DEV
}
