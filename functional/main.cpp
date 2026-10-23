/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional suite. Each test is a process. Sibling binaries that open a
    socket or start a real transport are tests of this suite, under the same
    120s failure limit (tests/suite_runner.h).
*/

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_driver_loopback.h"
#include "../tests/suite_runner.h"
#include "../tests/portable_crt.h"

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

#if defined(__APPLE__) || defined(__linux__)

// the suite's groups (tests/suite_runner.h): a group runs alone, so a case that
// measures wall time is not measured against the rest of the suite. The live
// message matrix holds a wall-clock pass bound (functional/live_matrix.cpp), so
// it is a group of its own, after every other case.
static int rocketnet_functional_test_group( const char * name )
{
#if defined(__APPLE__)
    // This case drives eight unpaced senders while destroying their server.
    // On Darwin its flood can overflow the shared loopback input queue and
    // drop packets belonging to unrelated exact-delivery cases. Keep the
    // flood and every assertion intact, but run it apart from those cases.
    if ( strcmp( name, "test_posix_destroy_under_traffic_100" ) == 0 )
        return 3;
#endif
    if ( strcmp( name, "test_loopback_transport_live_message_matrix" ) == 0 )
        return 2;
    // This case measures the operating system: it paces 25 frames for real
    // and reads the wall clock around them, so its bound is about the host's
    // sleep and its scheduler, not about the library. It runs alone, in a
    // group of its own, after every other group.
    if ( strcmp( name, "test_frame_sync_wait" ) == 0 )
        return 4;
    return rocketnet_posix_hard_killed_peer_test_group( name );
}

#endif // #if defined(__APPLE__) || defined(__linux__)

// test_transport_control (a sibling binary, test_transport_control.cpp) runs
// test_buffered_control_pressure at 300 and then at ROCKETNET_MAX_CLIENTS
// clients, one process: each builds a loopback server from the development
// config with the default packet payload limit and one reassembly slot, and
// seeds a real ACK observation into every slot, so the stats worker's
// per-slot delta buffers and history become resident by design. Its
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
        config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
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
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 3, rocketnet_functional_test_group );
#elif defined(__linux__)
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize" },
        { "test_transport_control", "test_transport_control" },
#if ROCKETNET_XDP
        { "test_xdp", "test_xdp" },
#endif
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, sizeof(execs) / sizeof(execs[0]), rocketnet_functional_test_group );
#else
    static const suite_exec execs[] = {
        { "test_serialize", "test_serialize" },
        { "test_transport_control", "test_transport_control" },
    };
    return suite_main( argc, argv, rocketnet_functional_worker, rocketnet_functional_test_count, rocketnet_functional_test_name, execs, 2 );
#endif
}
