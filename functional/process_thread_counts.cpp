/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The process thread-count cases, moved from the unit suite: reading the
// process's real OS thread count from /proc/self/task is OS state, not unit
// logic, so these run in the functional suite. They check that creating a
// server or a development-only step-driven client starts no thread of its own.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

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

static std::string process_thread_environment_value( const char * name, bool * was_set )
{
#if defined( _WIN32 )
    char * value = NULL;
    size_t value_bytes = 0;
    check( _dupenv_s( &value, &value_bytes, name ) == 0 );
    *was_set = value != NULL;
    const std::string result = value ? value : "";
    free( value );
    return result;
#else
    const char * value = getenv( name );
    *was_set = value != NULL;
    return value ? value : "";
#endif
}

// These cases require the in-memory transport even when the suite pins sockets.
struct process_thread_transport_guard_t
{
    bool server_was_set;
    bool client_was_set;
    std::string server_value;
    std::string client_value;

    process_thread_transport_guard_t()
    {
        server_value = process_thread_environment_value( "ROCKETNET_SERVER_TRANSPORT", &server_was_set );
        client_value = process_thread_environment_value( "ROCKETNET_CLIENT_TRANSPORT", &client_was_set );
#if defined( _WIN32 )
        check( _putenv_s( "ROCKETNET_SERVER_TRANSPORT", "loopback" ) == 0 );
        check( _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", "loopback" ) == 0 );
#else
        check( setenv( "ROCKETNET_SERVER_TRANSPORT", "loopback", 1 ) == 0 );
        check( setenv( "ROCKETNET_CLIENT_TRANSPORT", "loopback", 1 ) == 0 );
#endif
    }

    ~process_thread_transport_guard_t()
    {
#if defined( _WIN32 )
        check( _putenv_s( "ROCKETNET_SERVER_TRANSPORT", server_was_set ? server_value.c_str() : "" ) == 0 );
        check( _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", client_was_set ? client_value.c_str() : "" ) == 0 );
#else
        check( ( server_was_set ? setenv( "ROCKETNET_SERVER_TRANSPORT", server_value.c_str(), 1 ) : unsetenv( "ROCKETNET_SERVER_TRANSPORT" ) ) == 0 );
        check( ( client_was_set ? setenv( "ROCKETNET_CLIENT_TRANSPORT", client_value.c_str(), 1 ) : unsetenv( "ROCKETNET_CLIENT_TRANSPORT" ) ) == 0 );
#endif
    }
};

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
    process_thread_transport_guard_t transport_guard;
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
    else
    {
        printf( "SKIP process thread-count assertions: /proc/self/task is unavailable; loopback and stop completion remain checked.\n" );
    }
    struct rocketnet_server_client_stats_t stats;
    rocketnet_server_client_stats( world->server, rocketnet_client_index( world->clients[0] ), &stats );
    check( stats.rtt_ms > 0.0 && stats.bandwidth_sent_kbps > 0.0 && stats.bandwidth_received_kbps > 0.0 );

    no_worker_server_stop_and_wait( world );
    const int after_stop = no_worker_thread_count();
    if ( before >= 0 )
        check( after_stop == before );
    no_worker_world_destroy( world );
    const int after_destroy = no_worker_thread_count();
    if ( before >= 0 )
        check( after_destroy == before );
}

void rocketnet_test_public_step_driven_client_connects_without_a_thread()
{
    process_thread_transport_guard_t transport_guard;
    public_step_identity_t identity;
    public_step_identity( &identity );
    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity, 0, public_step_thread_count );
    if ( pair.threads_before_client >= 0 && pair.threads_after_client >= 0 )
        check( pair.threads_after_client == pair.threads_before_client );
    else
        printf( "SKIP process thread-count assertions: /proc/self/task is unavailable; loopback and stop completion remain checked.\n" );
    public_step_connect( &pair );
    public_step_pair_stop_server_and_wait( &pair );
    public_step_pair_destroy( &pair );
    if ( pair.threads_before_pair >= 0 )
        check( public_step_thread_count() == pair.threads_before_pair );
}
