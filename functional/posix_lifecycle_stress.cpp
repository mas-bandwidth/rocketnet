/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    POSIX lifecycle stress. Every case cycles a real transport over real
    sockets and counts the process's open file descriptors and threads before
    and after the case. A count that moves is a leak and the case fails; the
    before and after counts and the run time are printed for every cell.

    The macOS counters use libproc (proc_pidinfo PROC_PIDLISTFDS and
    PROC_PIDTASKINFO pti_threadnum); the Linux counters read /proc. The cases
    share one shape: a resource is created and destroyed repeatedly in a cell
    (server transport, client transport or both), and the process must end the
    cell exactly as it started it.
*/

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#if !defined(_WIN32)

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define posix_check( condition )                                                                                      \
do                                                                                                              \
{                                                                                                               \
    if ( !( condition ) )                                                                                       \
    {                                                                                                           \
        printf( "posix lifecycle stress check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition );           \
        fflush( stdout );                                                                                       \
        exit( 1 );                                                                                              \
    }                                                                                                           \
} while ( 0 )

struct posix_resource_counts_t
{
    int fds;
    int threads;
};

static int posix_open_file_descriptors()
{
#if defined(__APPLE__)
    struct proc_fdinfo entries[4096];
    const int wanted = (int) sizeof( entries );
    const int got = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, entries, wanted );
    if ( got <= 0 )
        return -1;
    return got / (int) sizeof( struct proc_fdinfo );
#else
    DIR * directory = opendir( "/proc/self/fd" );
    if ( !directory )
        return -1;
    int count = 0;
    struct dirent * entry = NULL;
    while ( ( entry = readdir( directory ) ) != NULL )
    {
        if ( strcmp( entry->d_name, "." ) == 0 || strcmp( entry->d_name, ".." ) == 0 )
            continue;
        count++;
    }
    closedir( directory );
    return count;
#endif
}

static int posix_threads()
{
#if defined(__APPLE__)
    struct proc_taskinfo info;
    const int got = proc_pidinfo( getpid(), PROC_PIDTASKINFO, 0, &info, (int) sizeof( info ) );
    if ( got != (int) sizeof( info ) )
        return -1;
    return (int) info.pti_threadnum;
#else
    DIR * directory = opendir( "/proc/self/task" );
    if ( !directory )
        return -1;
    int count = 0;
    struct dirent * entry = NULL;
    while ( ( entry = readdir( directory ) ) != NULL )
    {
        if ( strcmp( entry->d_name, "." ) == 0 || strcmp( entry->d_name, ".." ) == 0 )
            continue;
        count++;
    }
    closedir( directory );
    return count;
#endif
}

static posix_resource_counts_t posix_read_counts()
{
    posix_resource_counts_t counts;
    counts.fds = posix_open_file_descriptors();
    counts.threads = posix_threads();
    return counts;
}

// The receive and I/O threads are joined inside destroy, so the count settles
// as soon as the last destroy returns. Give the kernel a short window to reap
// a thread it has already been told to join, then compare two equal reads: a
// real leak is stable at the higher count and still fails.
static posix_resource_counts_t posix_stable_counts()
{
    posix_resource_counts_t previous = posix_read_counts();
    for ( int attempt = 0; attempt < 200; attempt++ )
    {
        rocketnet_platform_sleep( 0.001 );
        const posix_resource_counts_t current = posix_read_counts();
        if ( current.fds == previous.fds && current.threads == previous.threads )
            return current;
        previous = current;
    }
    return previous;
}

// The cells the card names: every server cell (portable, msgx, msgx forced
// onto the portable pump) against every client cell (portable, msgx). The
// forced cell requests msgx first and lets ROCKETNET_PUMP=portable refuse that
// rung, so the ladder grants portable; the grant is checked, not assumed.
// The override is read at socket create (sdk/rocketnet_platform.cpp:4122 in
// rocketnet_platform_socket_create_with_pump), so posix_server_create sets it
// around the server create only: a msgx client in the same cell keeps the
// darwin batch pump instead of being refused by the server's override.
struct posix_cell_t
{
    const char * name;
    const char * server_driver;
    const char * server_granted;
    const char * client_driver;
    const char * client_granted;
    const char * pump;
};

static const posix_cell_t posix_cells[] =
{
    { "server portable, client portable", "portable",      "portable", "portable", "portable", NULL },
    { "server portable, client msgx",     "portable",      "portable", "msgx",     "msgx",     NULL },
    { "server msgx, client portable",     "msgx",          "msgx",     "portable", "portable", NULL },
    { "server msgx, client msgx",         "msgx",          "msgx",     "msgx",     "msgx",     NULL },
    { "server msgx forced portable, client portable", "msgx,portable", "portable", "portable", "portable", "portable" },
    { "server msgx forced portable, client msgx",     "msgx,portable", "portable", "msgx",     "msgx",     "portable" },
};

static double posix_now_seconds()
{
    using namespace std::chrono;
    return duration<double>( steady_clock::now().time_since_epoch() ).count();
}

static void posix_require_loopback_bind( const char * name )
{
    const int fd = (int) socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( fd < 0 )
    {
        const int error = errno;
        printf( "%s: socket() failed (errno=%d %s); the real-socket cases cannot run here\n", name, error, strerror( error ) );
        fflush( stdout );
        exit( 1 );
    }

    struct sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    address.sin_port = 0;

    if ( bind( fd, (const struct sockaddr*) &address, sizeof( address ) ) != 0 )
    {
        const int error = errno;
        printf( "%s: this process cannot bind a loopback UDP socket (errno=%d %s); the real-socket cases cannot run here\n", name, error, strerror( error ) );
        fflush( stdout );
        close( fd );
        exit( 1 );
    }

    close( fd );
}

static const int posix_cell_count = (int) ( sizeof( posix_cells ) / sizeof( posix_cells[0] ) );

struct posix_env_guard_t
{
    bool set;
    char saved[1024];
};

static void posix_env_clear( posix_env_guard_t * guard, const char * name )
{
    const char * value = getenv( name );
    guard->set = value != NULL;
    guard->saved[0] = '\0';
    if ( value )
        snprintf( guard->saved, sizeof( guard->saved ), "%s", value );
    unsetenv( name );
}

static void posix_env_restore( const posix_env_guard_t * guard, const char * name )
{
    if ( guard->set )
        setenv( name, guard->saved, 1 );
    else
        unsetenv( name );
}

static void posix_cell_begin( const posix_cell_t * cell, posix_env_guard_t * server_guard, posix_env_guard_t * client_guard,
                             posix_env_guard_t * pump_guard )
{
    posix_env_clear( server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    posix_env_clear( client_guard, "ROCKETNET_CLIENT_TRANSPORT" );
    posix_env_clear( pump_guard, "ROCKETNET_PUMP" );
    (void) cell;
}

static void posix_cell_end( const posix_env_guard_t * server_guard, const posix_env_guard_t * client_guard,
                            const posix_env_guard_t * pump_guard )
{
    posix_env_restore( pump_guard, "ROCKETNET_PUMP" );
    posix_env_restore( client_guard, "ROCKETNET_CLIENT_TRANSPORT" );
    posix_env_restore( server_guard, "ROCKETNET_SERVER_TRANSPORT" );
}

static rocketnet_server_transport_t * posix_server_create( const posix_cell_t * cell, const char * address, int max_clients )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;
    snprintf( config.driver, sizeof( config.driver ), "%s", cell->server_driver );
    int status = ROCKETNET_STATUS_OK;
    if ( cell->pump )
        posix_check( setenv( "ROCKETNET_PUMP", cell->pump, 1 ) == 0 );
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, address, &config, rocketnet_platform_time(), &status );
    if ( cell->pump )
        posix_check( unsetenv( "ROCKETNET_PUMP" ) == 0 );
    if ( !server )
        printf( "server create [%s] at %s failed: %s\n", cell->name, address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    posix_check( server != NULL );
    posix_check( strcmp( rocketnet_server_transport_driver( server ), cell->server_granted ) == 0 );
    return server;
}

static rocketnet_client_transport_t * posix_client_create( const posix_cell_t * cell, const char * server_address )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    posix_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ),
                                                    token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.driver, sizeof( config.driver ), "%s", cell->client_driver );
    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * client = rocketnet_client_transport_create_ex( NULL, token, &config, rocketnet_platform_time(), &status );
    if ( !client )
        printf( "client create [%s] to %s failed: %s\n", cell->name, server_address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    posix_check( client != NULL );
    posix_check( strcmp( rocketnet_client_transport_driver( client ), cell->client_granted ) == 0 );
    return client;
}

static const char * posix_server_address( rocketnet_server_transport_t * server, char * buffer, size_t buffer_bytes )
{
    snprintf( buffer, buffer_bytes, "127.0.0.1:%u", (unsigned) rocketnet_server_transport_port( server ) );
    return buffer;
}

static bool posix_wait_connected( rocketnet_server_transport_t * server, rocketnet_client_transport_t * client, double seconds )
{
    const double deadline = rocketnet_platform_time() + seconds;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        const int state = rocketnet_client_transport_state( client );
        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            return true;
        if ( state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            return false;
        rocketnet_platform_sleep( 0.0002 );
    }
    return false;
}

static bool posix_wait_server_empty( rocketnet_server_transport_t * server, double seconds )
{
    const double deadline = rocketnet_platform_time() + seconds;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            return true;
        rocketnet_platform_sleep( 0.0002 );
    }
    return rocketnet_server_transport_num_connected_clients( server ) == 0;
}

static void posix_case_result( const char * name, const posix_cell_t * cell, const posix_resource_counts_t * before,
                               const posix_resource_counts_t * after, double seconds )
{
    printf( "%s [%s]: fds %d -> %d, threads %d -> %d, %.3fs\n", name, cell->name,
            before->fds, after->fds, before->threads, after->threads, seconds );
    fflush( stdout );
    if ( before->fds != after->fds || before->threads != after->threads )
    {
        printf( "%s [%s]: FAIL resource count moved (fds %d -> %d, threads %d -> %d)\n", name, cell->name,
                before->fds, after->fds, before->threads, after->threads );
        fflush( stdout );
        exit( 1 );
    }
}

// ---------------------------------------------------------------
// the cases
// ---------------------------------------------------------------

void rocketnet_test_posix_init_term_1000()
{
    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        posix_env_guard_t server_guard, client_guard, pump_guard;
        posix_cell_begin( cell, &server_guard, &client_guard, &pump_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();
        for ( int i = 0; i < 1000; i++ )
        {
            posix_check( rocketnet_term() == ROCKETNET_STATUS_OK );
            posix_check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_stable_counts();

        posix_cell_end( &server_guard, &client_guard, &pump_guard );
        posix_case_result( "posix_init_term_1000", cell, &before, &after, seconds );
    }
}

void rocketnet_test_posix_server_create_destroy_1000()
{
    posix_require_loopback_bind( "posix_server_create_destroy_1000" );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        posix_env_guard_t server_guard, client_guard, pump_guard;
        posix_cell_begin( cell, &server_guard, &client_guard, &pump_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();
        for ( int i = 0; i < 1000; i++ )
        {
            rocketnet_server_transport_t * server = posix_server_create( cell, "127.0.0.1:0", 4 );
            rocketnet_server_transport_destroy( server );
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_stable_counts();

        posix_cell_end( &server_guard, &client_guard, &pump_guard );
        posix_case_result( "posix_server_create_destroy_1000", cell, &before, &after, seconds );
    }
}

void rocketnet_test_posix_client_create_destroy_1000()
{
    posix_require_loopback_bind( "posix_client_create_destroy_1000" );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        posix_env_guard_t server_guard, client_guard, pump_guard;
        posix_cell_begin( cell, &server_guard, &client_guard, &pump_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();
        for ( int i = 0; i < 1000; i++ )
        {
            rocketnet_client_transport_t * client = posix_client_create( cell, "127.0.0.1:40000" );
            rocketnet_client_transport_destroy( client );
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_stable_counts();

        posix_cell_end( &server_guard, &client_guard, &pump_guard );
        posix_case_result( "posix_client_create_destroy_1000", cell, &before, &after, seconds );
    }
}

void rocketnet_test_posix_server_restart_same_port_100()
{
    posix_require_loopback_bind( "posix_server_restart_same_port_100" );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        posix_env_guard_t server_guard, client_guard, pump_guard;
        posix_cell_begin( cell, &server_guard, &client_guard, &pump_guard );

        const posix_resource_counts_t before = posix_stable_counts();

        // discover a free port, release it, and from then on bind that exact
        // port again and again. The restart is what SO_REUSEADDR behaviour on
        // BSD is on trial for; the client connects every round.
        rocketnet_server_transport_t * discovery = posix_server_create( cell, "127.0.0.1:0", 4 );
        const unsigned fixed_port = (unsigned) rocketnet_server_transport_port( discovery );
        rocketnet_server_transport_destroy( discovery );
        posix_check( fixed_port != 0 );

        char fixed_address[64];
        snprintf( fixed_address, sizeof( fixed_address ), "127.0.0.1:%u", fixed_port );

        const double start = posix_now_seconds();
        for ( int i = 0; i < 100; i++ )
        {
            rocketnet_server_transport_t * server = posix_server_create( cell, fixed_address, 4 );
            posix_check( rocketnet_server_transport_port( server ) == fixed_port );

            char server_address[64];
            posix_server_address( server, server_address, sizeof( server_address ) );
            rocketnet_client_transport_t * client = posix_client_create( cell, server_address );
            posix_check( posix_wait_connected( server, client, 5.0 ) );
            rocketnet_client_transport_destroy( client );
            rocketnet_server_transport_destroy( server );
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_stable_counts();

        posix_cell_end( &server_guard, &client_guard, &pump_guard );
        posix_case_result( "posix_server_restart_same_port_100", cell, &before, &after, seconds );
    }
}

void rocketnet_test_posix_connect_disconnect_1000()
{
    posix_require_loopback_bind( "posix_connect_disconnect_1000" );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        posix_env_guard_t server_guard, client_guard, pump_guard;
        posix_cell_begin( cell, &server_guard, &client_guard, &pump_guard );

        const posix_resource_counts_t before = posix_stable_counts();

        rocketnet_server_transport_t * server = posix_server_create( cell, "127.0.0.1:0", 8 );
        char server_address[64];
        posix_server_address( server, server_address, sizeof( server_address ) );

        const double start = posix_now_seconds();
        for ( int i = 0; i < 1000; i++ )
        {
            rocketnet_client_transport_t * client = posix_client_create( cell, server_address );
            posix_check( posix_wait_connected( server, client, 5.0 ) );
            rocketnet_client_transport_disconnect( client );
            posix_check( posix_wait_server_empty( server, 5.0 ) );
            rocketnet_client_transport_destroy( client );
        }
        const double seconds = posix_now_seconds() - start;

        rocketnet_server_transport_destroy( server );
        const posix_resource_counts_t after = posix_stable_counts();

        posix_cell_end( &server_guard, &client_guard, &pump_guard );
        posix_case_result( "posix_connect_disconnect_1000", cell, &before, &after, seconds );
    }
}

struct posix_sender_t
{
    rocketnet_client_transport_t * client;
    std::atomic<bool> * running;
};

static void posix_sender_thread( posix_sender_t * sender )
{
    uint8_t payload[64];
    memset( payload, 0xA5, sizeof( payload ) );
    while ( sender->running->load( std::memory_order_acquire ) )
        rocketnet_client_transport_send_packet( sender->client, payload, (int) sizeof( payload ) );
}

void rocketnet_test_posix_destroy_under_traffic_100()
{
    posix_require_loopback_bind( "posix_destroy_under_traffic_100" );

    const int client_count = 8;

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        posix_env_guard_t server_guard, client_guard, pump_guard;
        posix_cell_begin( cell, &server_guard, &client_guard, &pump_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();

        for ( int iteration = 0; iteration < 100; iteration++ )
        {
            rocketnet_server_transport_t * server = posix_server_create( cell, "127.0.0.1:0", client_count );
            char server_address[64];
            posix_server_address( server, server_address, sizeof( server_address ) );

            rocketnet_client_transport_t * clients[client_count];
            for ( int i = 0; i < client_count; i++ )
            {
                clients[i] = posix_client_create( cell, server_address );
                posix_check( posix_wait_connected( server, clients[i], 5.0 ) );
            }

            std::atomic<bool> running( true );
            posix_sender_t senders[client_count];
            std::thread threads[client_count];
            for ( int i = 0; i < client_count; i++ )
            {
                senders[i].client = clients[i];
                senders[i].running = &running;
                threads[i] = std::thread( posix_sender_thread, &senders[i] );
            }

            const double traffic_deadline = rocketnet_platform_time() + 0.02;
            while ( rocketnet_platform_time() < traffic_deadline )
            {
                rocketnet_server_transport_update( server, rocketnet_platform_time() );
                rocketnet_server_transport_send_packets( server );
                rocketnet_platform_sleep( 0.0005 );
            }

            // destroy the server while all eight clients are sending, then
            // stop the senders before the clients themselves are destroyed
            rocketnet_server_transport_destroy( server );

            running.store( false, std::memory_order_release );
            for ( int i = 0; i < client_count; i++ )
                threads[i].join();
            for ( int i = 0; i < client_count; i++ )
                rocketnet_client_transport_destroy( clients[i] );
        }

        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_stable_counts();

        posix_cell_end( &server_guard, &client_guard, &pump_guard );
        posix_case_result( "posix_destroy_under_traffic_100", cell, &before, &after, seconds );
    }
}

#endif // #if !defined(_WIN32)
