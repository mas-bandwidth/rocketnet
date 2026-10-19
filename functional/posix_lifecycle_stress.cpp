/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    POSIX lifecycle stress: cycle init and term, server and client create and
    destroy, same-port restart, connect and disconnect, and destroy under
    traffic on every transport this platform builds. Each cell counts the
    process's open file descriptors and threads before and after and fails on
    any difference, so a socket not closed or a thread not joined is a red.

    The macOS counts are libproc's proc_pidinfo (PROC_PIDLISTFDS and
    PROC_PIDTASKINFO pti_threadnum); the Linux counts are /proc/self/fd and
    /proc/self/task. The Linux counts belong to the Linux lifecycle card.
*/

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#elif defined(__linux__)
#include <dirent.h>
#endif

#define POSIX_LIFECYCLE_CHECK( condition )                                                                          \
do                                                                                                              \
{                                                                                                               \
    if ( !( condition ) )                                                                                       \
    {                                                                                                           \
        printf( "posix lifecycle check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__ );                 \
        fflush( stdout );                                                                                       \
        exit( 1 );                                                                                              \
    }                                                                                                           \
} while ( 0 )

struct posix_lifecycle_counts_t
{
    long file_descriptors;
    long threads;
};

// PROC_PIDLISTFDS returns a byte count; the count is recalculated first so a
// cell that opens more descriptors than the previous sample still reads all of
// them.
static long posix_lifecycle_file_descriptor_count()
{
#if defined(__APPLE__)
    const int needed = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, NULL, 0 );
    if ( needed <= 0 )
        return -1;
    const int capacity = needed + (int) ( 32 * sizeof( struct proc_fdinfo ) );
    struct proc_fdinfo * descriptors = (struct proc_fdinfo *) malloc( (size_t) capacity );
    if ( !descriptors )
        return -1;
    const int bytes = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, descriptors, capacity );
    free( descriptors );
    if ( bytes <= 0 )
        return -1;
    return (long) ( bytes / (int) sizeof( struct proc_fdinfo ) );
#elif defined(__linux__)
    DIR * directory = opendir( "/proc/self/fd" );
    if ( !directory )
        return -1;
    long count = 0;
    struct dirent * entry;
    while ( ( entry = readdir( directory ) ) != NULL )
    {
        if ( strcmp( entry->d_name, "." ) == 0 || strcmp( entry->d_name, ".." ) == 0 )
            continue;
        count++;
    }
    closedir( directory );
    return count;
#else
    return -1;
#endif
}

// The task thread count is the kernel's own number, so a thread that was
// created but never joined is visible even after its joinable object is gone.
static long posix_lifecycle_thread_count()
{
#if defined(__APPLE__)
    struct proc_taskinfo info;
    memset( &info, 0, sizeof( info ) );
    const int bytes = proc_pidinfo( getpid(), PROC_PIDTASKINFO, 0, &info, (int) sizeof( info ) );
    if ( bytes != (int) sizeof( info ) )
        return -1;
    return (long) info.pti_threadnum;
#elif defined(__linux__)
    DIR * directory = opendir( "/proc/self/task" );
    if ( !directory )
        return -1;
    long count = 0;
    struct dirent * entry;
    while ( ( entry = readdir( directory ) ) != NULL )
    {
        if ( strcmp( entry->d_name, "." ) == 0 || strcmp( entry->d_name, ".." ) == 0 )
            continue;
        count++;
    }
    closedir( directory );
    return count;
#else
    return -1;
#endif
}

// init and term reset the library's own time origin, so the run-time line uses
// a clock that survives the cycle: CLOCK_MONOTONIC.
static double posix_lifecycle_seconds()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1000000000.0;
}

static struct posix_lifecycle_counts_t posix_lifecycle_snapshot()
{
    struct posix_lifecycle_counts_t counts;
    counts.file_descriptors = posix_lifecycle_file_descriptor_count();
    counts.threads = posix_lifecycle_thread_count();
    return counts;
}

static void posix_lifecycle_report( const char * test, const char * cell,
                                    const struct posix_lifecycle_counts_t * before,
                                    const struct posix_lifecycle_counts_t * after,
                                    double seconds )
{
    const long fd_delta = after->file_descriptors - before->file_descriptors;
    const long thread_delta = after->threads - before->threads;
    printf( "posix_lifecycle %s cell=%s fds_before=%ld fds_after=%ld fd_delta=%ld threads_before=%ld threads_after=%ld thread_delta=%ld seconds=%.3f\n",
            test, cell, before->file_descriptors, after->file_descriptors, fd_delta,
            before->threads, after->threads, thread_delta, seconds );
    fflush( stdout );
}

static void posix_lifecycle_check_flat( const char * test, const char * cell,
                                        const struct posix_lifecycle_counts_t * before, double seconds )
{
    const struct posix_lifecycle_counts_t after = posix_lifecycle_snapshot();
    posix_lifecycle_report( test, cell, before, &after, seconds );
    if ( before->file_descriptors < 0 || after.file_descriptors != before->file_descriptors )
    {
        printf( "posix_lifecycle %s cell=%s FAILED: file descriptors before=%ld after=%ld\n",
                test, cell, before->file_descriptors, after.file_descriptors );
        fflush( stdout );
        exit( 1 );
    }
    if ( before->threads < 0 || after.threads != before->threads )
    {
        printf( "posix_lifecycle %s cell=%s FAILED: threads before=%ld after=%ld\n",
                test, cell, before->threads, after.threads );
        fflush( stdout );
        exit( 1 );
    }
}

// A cell names the driver list and the pump override that create sees. A
// platform without msgx contributes only portable.
struct posix_lifecycle_cell_t
{
    const char * label;
    const char * driver;
    const char * pump;
};

static int posix_lifecycle_server_cells( struct posix_lifecycle_cell_t * cells )
{
    int count = 0;
    cells[count].label = "portable";
    cells[count].driver = "portable";
    cells[count].pump = NULL;
    count++;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    cells[count].label = "msgx";
    cells[count].driver = "msgx";
    cells[count].pump = NULL;
    count++;
    // the msgx rung under the pre-#106 override refuses and the list falls to
    // the portable rung, so this is the degraded msgx path, not a pin
    cells[count].label = "msgx_forced_portable";
    cells[count].driver = "msgx,portable";
    cells[count].pump = "portable";
    count++;
#endif
    return count;
}

static int posix_lifecycle_client_cells( struct posix_lifecycle_cell_t * cells )
{
    int count = 0;
    cells[count].label = "portable";
    cells[count].driver = "portable";
    cells[count].pump = NULL;
    count++;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    cells[count].label = "msgx";
    cells[count].driver = "msgx";
    cells[count].pump = NULL;
    count++;
#endif
    return count;
}

static void posix_lifecycle_enter_cell( const struct posix_lifecycle_cell_t * cell )
{
    // the driver is pinned through the config; the ambient name lists are
    // cleared so a harness-wide pin cannot turn a cell into another transport
    unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
    unsetenv( "ROCKETNET_CLIENT_TRANSPORT" );
    if ( cell->pump )
        setenv( "ROCKETNET_PUMP", cell->pump, 1 );
    else
        unsetenv( "ROCKETNET_PUMP" );
}

static void posix_lifecycle_leave_cell()
{
    unsetenv( "ROCKETNET_PUMP" );
}

static struct rocketnet_server_transport_t * posix_lifecycle_create_server( const char * driver,
                                                                            const char * public_address,
                                                                            int max_clients )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;
    snprintf( config.driver, sizeof( config.driver ), "%s", driver );
    return rocketnet_server_transport_create( NULL, public_address, &config );
}

static struct rocketnet_client_transport_t * posix_lifecycle_create_client( const char * driver,
                                                                            const uint8_t * connect_token )
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.driver, sizeof( config.driver ), "%s", driver );
    return rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
}

static bool posix_lifecycle_write_token( const char * address, uint8_t * connect_token )
{
    return rocketnet_write_dev_connect_token( address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token );
}

static bool posix_lifecycle_connect( struct rocketnet_server_transport_t * server,
                                     struct rocketnet_client_transport_t * client,
                                     double seconds )
{
    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < seconds )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) >= 1 )
            return true;
        if ( rocketnet_client_transport_state( client ) < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            return false;
        rocketnet_platform_sleep( 0.001 );
    }
    return false;
}

static bool posix_lifecycle_connect_all( struct rocketnet_server_transport_t * server,
                                         struct rocketnet_client_transport_t ** clients,
                                         int num_clients,
                                         double seconds )
{
    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < seconds )
    {
        int connected = 0;
        for ( int i = 0; i < num_clients; i++ )
        {
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
            const int state = rocketnet_client_transport_state( clients[i] );
            if ( state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
                return false;
            if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                connected++;
        }
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( connected == num_clients && rocketnet_server_transport_num_connected_clients( server ) >= num_clients )
            return true;
        rocketnet_platform_sleep( 0.001 );
    }
    return false;
}

static void posix_lifecycle_wait_disconnected( struct rocketnet_server_transport_t * server, int clients, double seconds )
{
    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < seconds && rocketnet_server_transport_num_connected_clients( server ) > 0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }
    (void) clients;
}

static void posix_lifecycle_init_term_cell( const struct posix_lifecycle_cell_t * cell )
{
    posix_lifecycle_enter_cell( cell );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    // the worker initialized to run this case, so close that first generation
    // and then alternate 1000 times, ending initialized for the worker's term
    rocketnet_term();
    for ( int i = 0; i < 1000; i++ )
    {
        POSIX_LIFECYCLE_CHECK( rocketnet_init( NULL ) );
        rocketnet_term();
    }
    POSIX_LIFECYCLE_CHECK( rocketnet_init( NULL ) );

    posix_lifecycle_check_flat( "posix_init_term_1000", cell->label, &before, posix_lifecycle_seconds() - start );

    posix_lifecycle_leave_cell();
}

static void posix_lifecycle_server_create_destroy_cell( const struct posix_lifecycle_cell_t * cell )
{
    posix_lifecycle_enter_cell( cell );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    for ( int i = 0; i < 1000; i++ )
    {
        struct rocketnet_server_transport_t * server = posix_lifecycle_create_server( cell->driver, "127.0.0.1:0", 8 );
        POSIX_LIFECYCLE_CHECK( server != NULL );
        rocketnet_server_transport_destroy( server );
    }

    posix_lifecycle_check_flat( "posix_server_create_destroy_1000", cell->label, &before, posix_lifecycle_seconds() - start );

    posix_lifecycle_leave_cell();
}

static void posix_lifecycle_client_create_destroy_cell( const struct posix_lifecycle_cell_t * cell )
{
    posix_lifecycle_enter_cell( cell );

    // one server only mints the address the tokens name; it is not part of the
    // cell's counts
    struct rocketnet_server_transport_t * token_server = posix_lifecycle_create_server( "portable", "127.0.0.1:0", 8 );
    POSIX_LIFECYCLE_CHECK( token_server != NULL );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", rocketnet_server_transport_port( token_server ) );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    for ( int i = 0; i < 1000; i++ )
    {
        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        POSIX_LIFECYCLE_CHECK( posix_lifecycle_write_token( address, connect_token ) );
        struct rocketnet_client_transport_t * client = posix_lifecycle_create_client( cell->driver, connect_token );
        POSIX_LIFECYCLE_CHECK( client != NULL );
        rocketnet_client_transport_destroy( client );
    }

    posix_lifecycle_check_flat( "posix_client_create_destroy_1000", cell->label, &before, posix_lifecycle_seconds() - start );

    rocketnet_server_transport_destroy( token_server );
    posix_lifecycle_leave_cell();
}

static void posix_lifecycle_server_restart_same_port_cell( const struct posix_lifecycle_cell_t * server_cell,
                                                           const struct posix_lifecycle_cell_t * client_cell )
{
    posix_lifecycle_enter_cell( server_cell );

    // discover a port first; every one of the 100 restarts then asks the
    // kernel for that same port again, which is the BSD reuse question
    struct rocketnet_server_transport_t * probe = posix_lifecycle_create_server( server_cell->driver, "127.0.0.1:0", 8 );
    POSIX_LIFECYCLE_CHECK( probe != NULL );
    const uint16_t port = rocketnet_server_transport_port( probe );
    rocketnet_server_transport_destroy( probe );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );

    char label[128];
    snprintf( label, sizeof( label ), "%s+%s", server_cell->label, client_cell->label );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    for ( int i = 0; i < 100; i++ )
    {
        struct rocketnet_server_transport_t * server = posix_lifecycle_create_server( server_cell->driver, address, 8 );
        POSIX_LIFECYCLE_CHECK( server != NULL );

        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        POSIX_LIFECYCLE_CHECK( posix_lifecycle_write_token( address, connect_token ) );
        struct rocketnet_client_transport_t * client = posix_lifecycle_create_client( client_cell->driver, connect_token );
        POSIX_LIFECYCLE_CHECK( client != NULL );
        POSIX_LIFECYCLE_CHECK( posix_lifecycle_connect( server, client, 5.0 ) );

        rocketnet_client_transport_destroy( client );
        rocketnet_server_transport_destroy( server );
    }

    posix_lifecycle_check_flat( "posix_server_restart_same_port_100", label, &before, posix_lifecycle_seconds() - start );

    posix_lifecycle_leave_cell();
}

static void posix_lifecycle_connect_disconnect_cell( const struct posix_lifecycle_cell_t * server_cell,
                                                     const struct posix_lifecycle_cell_t * client_cell )
{
    posix_lifecycle_enter_cell( server_cell );

    struct rocketnet_server_transport_t * server = posix_lifecycle_create_server( server_cell->driver, "127.0.0.1:0", 8 );
    POSIX_LIFECYCLE_CHECK( server != NULL );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    char label[128];
    snprintf( label, sizeof( label ), "%s+%s", server_cell->label, client_cell->label );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    for ( int i = 0; i < 1000; i++ )
    {
        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        POSIX_LIFECYCLE_CHECK( posix_lifecycle_write_token( address, connect_token ) );
        struct rocketnet_client_transport_t * client = posix_lifecycle_create_client( client_cell->driver, connect_token );
        POSIX_LIFECYCLE_CHECK( client != NULL );
        POSIX_LIFECYCLE_CHECK( posix_lifecycle_connect( server, client, 5.0 ) );
        rocketnet_client_transport_disconnect( client );
        posix_lifecycle_wait_disconnected( server, 1, 3.0 );
        rocketnet_client_transport_destroy( client );
    }

    posix_lifecycle_check_flat( "posix_connect_disconnect_1000", label, &before, posix_lifecycle_seconds() - start );

    rocketnet_server_transport_destroy( server );
    posix_lifecycle_leave_cell();
}

static void posix_lifecycle_destroy_under_traffic_cell( const struct posix_lifecycle_cell_t * server_cell,
                                                        const struct posix_lifecycle_cell_t * client_cell )
{
    posix_lifecycle_enter_cell( server_cell );

    char label[128];
    snprintf( label, sizeof( label ), "%s+%s", server_cell->label, client_cell->label );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    for ( int cycle = 0; cycle < 100; cycle++ )
    {
        struct rocketnet_server_transport_t * server = posix_lifecycle_create_server( server_cell->driver, "127.0.0.1:0", 8 );
        POSIX_LIFECYCLE_CHECK( server != NULL );
        char address[64];
        snprintf( address, sizeof( address ), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

        struct rocketnet_client_transport_t * clients[8];
        for ( int i = 0; i < 8; i++ )
        {
            uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
            POSIX_LIFECYCLE_CHECK( posix_lifecycle_write_token( address, connect_token ) );
            clients[i] = posix_lifecycle_create_client( client_cell->driver, connect_token );
            POSIX_LIFECYCLE_CHECK( clients[i] != NULL );
        }
        // the eight handshakes run together, so a cycle costs one handshake
        // cadence rather than eight
        POSIX_LIFECYCLE_CHECK( posix_lifecycle_connect_all( server, clients, 8, 5.0 ) );

        // the clients keep sending until the server goes away mid-traffic
        const double traffic_start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - traffic_start < 0.01 )
        {
            for ( int i = 0; i < 8; i++ )
            {
                const uint8_t traffic[16] = { (uint8_t) cycle, (uint8_t) i };
                rocketnet_client_transport_send_packet( clients[i], traffic, (int) sizeof( traffic ) );
                rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
            }
            rocketnet_server_transport_update( server, rocketnet_platform_time() );
            rocketnet_server_transport_send_packets( server );
        }
        rocketnet_server_transport_destroy( server );

        for ( int i = 0; i < 8; i++ )
        {
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
            rocketnet_client_transport_destroy( clients[i] );
        }
    }

    posix_lifecycle_check_flat( "posix_destroy_under_traffic_100", label, &before, posix_lifecycle_seconds() - start );

    posix_lifecycle_leave_cell();
}

static bool posix_lifecycle_client_cell_fits( const struct posix_lifecycle_cell_t * server_cell,
                                              const struct posix_lifecycle_cell_t * client_cell )
{
    // ROCKETNET_PUMP is process-wide: the forced-portable server cell refuses a
    // pinned msgx client, so that pairing is not a lifecycle cell
    if ( server_cell->pump && strcmp( server_cell->pump, "portable" ) == 0 &&
         strcmp( client_cell->driver, "portable" ) != 0 )
        return false;
    return true;
}

static void posix_lifecycle_run_combined( void ( *run )( const struct posix_lifecycle_cell_t *,
                                                          const struct posix_lifecycle_cell_t * ) )
{
    struct posix_lifecycle_cell_t server_cells[8];
    struct posix_lifecycle_cell_t client_cells[8];
    const int num_server_cells = posix_lifecycle_server_cells( server_cells );
    const int num_client_cells = posix_lifecycle_client_cells( client_cells );
    for ( int s = 0; s < num_server_cells; s++ )
    {
        for ( int c = 0; c < num_client_cells; c++ )
        {
            if ( !posix_lifecycle_client_cell_fits( &server_cells[s], &client_cells[c] ) )
                continue;
            run( &server_cells[s], &client_cells[c] );
        }
    }
}

static void rocketnet_posix_init_term_1000()
{
    struct posix_lifecycle_cell_t cells[8];
    const int count = posix_lifecycle_server_cells( cells );
    for ( int i = 0; i < count; i++ )
        posix_lifecycle_init_term_cell( &cells[i] );
}

static void rocketnet_posix_server_create_destroy_1000()
{
    struct posix_lifecycle_cell_t cells[8];
    const int count = posix_lifecycle_server_cells( cells );
    for ( int i = 0; i < count; i++ )
        posix_lifecycle_server_create_destroy_cell( &cells[i] );
}

static void rocketnet_posix_client_create_destroy_1000()
{
    struct posix_lifecycle_cell_t cells[8];
    const int count = posix_lifecycle_client_cells( cells );
    for ( int i = 0; i < count; i++ )
        posix_lifecycle_client_create_destroy_cell( &cells[i] );
}

static void rocketnet_posix_server_restart_same_port_100()
{
    posix_lifecycle_run_combined( posix_lifecycle_server_restart_same_port_cell );
}

static void rocketnet_posix_connect_disconnect_1000()
{
    posix_lifecycle_run_combined( posix_lifecycle_connect_disconnect_cell );
}

static void rocketnet_posix_destroy_under_traffic_100()
{
    posix_lifecycle_run_combined( posix_lifecycle_destroy_under_traffic_cell );
}

static bool posix_lifecycle_run_case( const char * name )
{
    static const struct
    {
        const char * name;
        void ( *function )();
    }
    cases[] =
    {
        { "posix_init_term_1000", rocketnet_posix_init_term_1000 },
        { "posix_server_create_destroy_1000", rocketnet_posix_server_create_destroy_1000 },
        { "posix_client_create_destroy_1000", rocketnet_posix_client_create_destroy_1000 },
        { "posix_server_restart_same_port_100", rocketnet_posix_server_restart_same_port_100 },
        { "posix_connect_disconnect_1000", rocketnet_posix_connect_disconnect_1000 },
        { "posix_destroy_under_traffic_100", rocketnet_posix_destroy_under_traffic_100 },
    };
    for ( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
    {
        if ( strcmp( cases[i].name, name ) != 0 )
            continue;
        printf( "%s\n", name );
        fflush( stdout );
        cases[i].function();
        return true;
    }
    return false;
}

#else // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// The suite builds on Windows too; the POSIX cases have nothing to count there.
static bool posix_lifecycle_run_case( const char * name )
{
    (void) name;
    return false;
}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

bool rocketnet_posix_lifecycle_run_named( const char * name )
{
    return posix_lifecycle_run_case( name );
}
