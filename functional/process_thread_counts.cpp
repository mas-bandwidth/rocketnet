/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The process thread-count cases, moved from the unit suite: reading the
// process's real OS thread count from /proc/self/task is OS state, not unit
// logic, so these run in the functional suite. They check that creating a
// server or a step-driven client starts no thread of its own.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <dirent.h>
#endif

static void rocketnet_process_thread_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                               \
do                                                                                                                  \
{                                                                                                                   \
    if ( !(condition) )                                                                                             \
    {                                                                                                               \
        rocketnet_process_thread_check_handler( #condition, (const char*) __FUNCTION__,                             \
                                                (const char*) __FILE__, __LINE__ );                                  \
    }                                                                                                               \
} while( 0 )

#define check rocketnet_check

#include "../tests/no_worker_world.h"
#include "../tests/public_step_pair.h"

// The threads of this process, counted from /proc/self/task; -1 where the
// platform has no such count, and the thread case then pins nothing there.
static int no_worker_thread_count()
{
#if defined(__linux__)
    DIR * dir = opendir( "/proc/self/task" );
    if ( !dir )
        return -1;
    int count = 0;
    struct dirent * entry;
    while ( ( entry = readdir( dir ) ) != NULL )
    {
        if ( entry->d_name[0] != '.' )
            count++;
    }
    closedir( dir );
    return count;
#else
    return -1;
#endif
}

static int public_step_thread_count()
{
#if defined(__linux__)
    DIR * dir = opendir( "/proc/self/task" );
    if ( !dir )
        return -1;
    int count = 0;
    while ( readdir( dir ) )
        count++;
    closedir( dir );
    return count;
#else
    return -1;
#endif
}

// Creating, starting and updating a server starts no thread: the loopback
// transport has none and the statistics need none, yet the slot's statistics
// are live after the updates. A server that computed its statistics on a
// worker either adds a thread at create or, with the worker held back (the
// manual mode unit builds once defaulted to), publishes nothing.
void rocketnet_test_server_stats_creates_no_thread()
{
    no_worker_world_t * world = &g_no_worker_world;
    const int before = no_worker_thread_count();

    no_worker_server_create( world, NO_WORKER_CLIENTS );
    const int after_create = no_worker_thread_count();
    no_worker_clients_create( world, 1 );
    no_worker_connect_all( world );
    for ( int step = 0; step < 400 && no_worker_connected_slots( world ) < 1; step++ )
        no_worker_step( world, false, 0.1 );
    check( no_worker_connected_slots( world ) == 1 );
    for ( int step = 0; step < 20; step++ )
        no_worker_step( world, true, 0.1 );
    const int after_updates = no_worker_thread_count();
    if ( before >= 0 )
    {
        check( after_create == before );
        check( after_updates == before );
    }

    struct rocketnet_server_client_stats_t stats;
    rocketnet_server_client_stats( world->server, rocketnet_client_index( world->clients[0] ), &stats );
    check( stats.rtt_ms > 0.0 && stats.bandwidth_sent_kbps > 0.0 && stats.bandwidth_received_kbps > 0.0 );

    no_worker_world_destroy( world );
}

void rocketnet_test_public_step_driven_client_connects_without_a_thread()
{
    public_step_identity_t identity;
    public_step_identity( &identity );
    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity, 0, public_step_thread_count );
    if ( pair.threads_before_client >= 0 && pair.threads_after_client >= 0 )
        check( pair.threads_after_client == pair.threads_before_client );
    public_step_connect( &pair );
    public_step_pair_destroy( &pair );
}
