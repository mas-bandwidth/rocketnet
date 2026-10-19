/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    POSIX lifecycle stress: cycle init and term, server and client create and
    destroy, same-port restart, connect and disconnect, and destroy under
    traffic on every transport this platform builds. Each cell counts the
    process's open file descriptors and threads before and after and fails on
    any difference, so a socket not closed or a thread not joined is a red.

    The macOS counts are libproc's proc_pidinfo (PROC_PIDLISTFDS and
    PROC_PIDTASKINFO pti_threadnum); the Linux counts are /proc/self/fd,
    /proc/self/task and the locked memory (VmLck in /proc/self/status).

    On Linux the server cells are portable, gso, io_uring and xdp, each with
    the portable client. The xdp cell needs root, a network namespace and a
    veth pair, so it runs only as root inside a virtme-ng guest (or as root
    with ROCKETNET_TEST_XDP_VETH=1); anywhere else it is a named skip. Its
    server binds the near end of the pair, its clients open their sockets in
    the far end's namespace, and its 1000-count loops run 100 times, since
    every create attaches to the pair. ROCKETNET_POSIX_LIFECYCLE_CELLS, a
    comma list of cell labels, runs only those cells.
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
#include <fcntl.h>
#include <sched.h>
#include <sys/vfs.h>
#endif

#if defined(__linux__) && ROCKETNET_XDP
#include "rocketnet_server_socket.h"
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
    long locked_kb;
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

// The locked memory is VmLck from /proc/self/status, in kB: memory a cell
// locks and never unlocks is a difference. Other platforms read zero.
static long posix_lifecycle_locked_kb()
{
#if defined(__linux__)
    FILE * file = fopen( "/proc/self/status", "r" );
    if ( !file )
        return -1;
    char line[256];
    long locked = -1;
    while ( fgets( line, sizeof( line ), file ) )
    {
        if ( strncmp( line, "VmLck:", 6 ) == 0 )
        {
            locked = strtol( line + 6, NULL, 10 );
            break;
        }
    }
    fclose( file );
    return locked;
#else
    return 0;
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
    counts.locked_kb = posix_lifecycle_locked_kb();
    return counts;
}

static void posix_lifecycle_report( const char * test, const char * cell,
                                    const struct posix_lifecycle_counts_t * before,
                                    const struct posix_lifecycle_counts_t * after,
                                    double seconds )
{
    const long fd_delta = after->file_descriptors - before->file_descriptors;
    const long thread_delta = after->threads - before->threads;
    const long locked_delta = after->locked_kb - before->locked_kb;
    printf( "posix_lifecycle %s cell=%s fds_before=%ld fds_after=%ld fd_delta=%ld threads_before=%ld threads_after=%ld thread_delta=%ld locked_kb_before=%ld locked_kb_after=%ld locked_kb_delta=%ld seconds=%.3f\n",
            test, cell, before->file_descriptors, after->file_descriptors, fd_delta,
            before->threads, after->threads, thread_delta,
            before->locked_kb, after->locked_kb, locked_delta, seconds );
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
    if ( before->locked_kb < 0 || after.locked_kb != before->locked_kb )
    {
        printf( "posix_lifecycle %s cell=%s FAILED: locked memory kB before=%ld after=%ld\n",
                test, cell, before->locked_kb, after.locked_kb );
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
    bool xdp;
};

// The xdp cell's pair: the server on the near end, the clients in the far
// end's namespace. Made once per process when the first xdp cell enters, and
// removed at exit.

#define POSIX_LIFECYCLE_XDP_NETNS       "rnxdplc"
#define POSIX_LIFECYCLE_XDP_NEAR        "rnxdplc0"
#define POSIX_LIFECYCLE_XDP_FAR         "rnxdplc1"
#define POSIX_LIFECYCLE_XDP_HOST        "10.77.3.1"
#define POSIX_LIFECYCLE_XDP_FAR_HOST    "10.77.3.2"
#define POSIX_LIFECYCLE_XDP_PORT        40421

#if defined(__linux__) && ROCKETNET_XDP

static bool posix_lifecycle_xdp_made = false;

static void posix_lifecycle_xdp_teardown()
{
    if ( posix_lifecycle_xdp_made )
    {
        // deleting the namespace takes the far end, and with it the pair
        if ( system( "ip netns del " POSIX_LIFECYCLE_XDP_NETNS " 2>/dev/null; ip link del " POSIX_LIFECYCLE_XDP_NEAR " 2>/dev/null" ) != 0 )
        {
            // already gone
        }
        posix_lifecycle_xdp_made = false;
    }
    rocketnet_server_socket_forbid_commit();
}

static void posix_lifecycle_xdp_run( const char * command )
{
    const int result = system( command );
    if ( result != 0 )
        printf( "posix lifecycle command failed (%d): %s\n", result, command );
    POSIX_LIFECYCLE_CHECK( result == 0 );
}

static bool posix_lifecycle_xdp_offered()
{
    if ( geteuid() != 0 )
        return false;
    const char * opt_in = getenv( "ROCKETNET_TEST_XDP_VETH" );
    if ( opt_in && strcmp( opt_in, "1" ) == 0 )
        return true;
    FILE * file = fopen( "/proc/cmdline", "r" );
    if ( !file )
        return false;
    char line[4096];
    const bool read = fgets( line, sizeof( line ), file ) != NULL;
    fclose( file );
    return read && strstr( line, "virtme" ) != NULL;
}

static void posix_lifecycle_xdp_setup()
{
    if ( posix_lifecycle_xdp_made )
        return;

    atexit( posix_lifecycle_xdp_teardown );

    struct statfs bpffs;
    if ( statfs( "/sys/fs/bpf", &bpffs ) != 0 || (uint64_t) bpffs.f_type != (uint64_t) 0xcafe4a11 )
        posix_lifecycle_xdp_run( "mount -t bpf bpf /sys/fs/bpf" );

    if ( system( "ip netns del " POSIX_LIFECYCLE_XDP_NETNS " 2>/dev/null; ip link del " POSIX_LIFECYCLE_XDP_NEAR " 2>/dev/null" ) != 0 )
    {
        // nothing left from an earlier run
    }

    posix_lifecycle_xdp_made = true;
    posix_lifecycle_xdp_run( "ip netns add " POSIX_LIFECYCLE_XDP_NETNS );
    posix_lifecycle_xdp_run( "ip link add " POSIX_LIFECYCLE_XDP_NEAR " mtu 1500 type veth peer name " POSIX_LIFECYCLE_XDP_FAR " mtu 1500" );
    posix_lifecycle_xdp_run( "ip link set " POSIX_LIFECYCLE_XDP_FAR " netns " POSIX_LIFECYCLE_XDP_NETNS );
    posix_lifecycle_xdp_run( "ip addr add " POSIX_LIFECYCLE_XDP_HOST "/24 dev " POSIX_LIFECYCLE_XDP_NEAR );
    posix_lifecycle_xdp_run( "ip -n " POSIX_LIFECYCLE_XDP_NETNS " addr add " POSIX_LIFECYCLE_XDP_FAR_HOST "/24 dev " POSIX_LIFECYCLE_XDP_FAR );
    posix_lifecycle_xdp_run( "ip link set " POSIX_LIFECYCLE_XDP_NEAR " up" );
    // the busy-poll pair the xdp pre-flight verifies
    posix_lifecycle_xdp_run( "echo 2 > /sys/class/net/" POSIX_LIFECYCLE_XDP_NEAR "/napi_defer_hard_irqs" );
    posix_lifecycle_xdp_run( "echo 200000 > /sys/class/net/" POSIX_LIFECYCLE_XDP_NEAR "/gro_flush_timeout" );
    posix_lifecycle_xdp_run( "ip -n " POSIX_LIFECYCLE_XDP_NETNS " link set lo up" );
    posix_lifecycle_xdp_run( "ip -n " POSIX_LIFECYCLE_XDP_NETNS " link set " POSIX_LIFECYCLE_XDP_FAR " up" );

    rocketnet_server_socket_allow_commit();
}

#endif // #if defined(__linux__) && ROCKETNET_XDP

// io_uring is a cell when the kernel offers it: kernel.io_uring_disabled
// reads 0, or the file is absent on a kernel older than the switch
static bool posix_lifecycle_io_uring_offered()
{
#if defined(__linux__)
    FILE * file = fopen( "/proc/sys/kernel/io_uring_disabled", "r" );
    if ( !file )
        return true;
    int disabled = 0;
    const bool read = fscanf( file, "%d", &disabled ) == 1;
    fclose( file );
    return !read || disabled == 0;
#else
    return false;
#endif
}

// a 1000-count loop runs 100 times on the xdp cell
static int posix_lifecycle_count( const struct posix_lifecycle_cell_t * cell, int count )
{
    if ( cell->xdp && count > 100 )
        return 100;
    return count;
}

// the bind address a cell's server takes: loopback with a discovered port,
// or the near end of the pair on the xdp cell's fixed port
static void posix_lifecycle_bind_address( const struct posix_lifecycle_cell_t * cell, char * address, size_t bytes )
{
    if ( cell->xdp )
        snprintf( address, bytes, "%s:%d", POSIX_LIFECYCLE_XDP_HOST, POSIX_LIFECYCLE_XDP_PORT );
    else
        snprintf( address, bytes, "127.0.0.1:0" );
}

static void posix_lifecycle_server_address( const struct posix_lifecycle_cell_t * cell, uint16_t port, char * address, size_t bytes )
{
    snprintf( address, bytes, "%s:%u", cell->xdp ? POSIX_LIFECYCLE_XDP_HOST : "127.0.0.1", (unsigned) port );
}

// ROCKETNET_POSIX_LIFECYCLE_CELLS, when set, names the cells that run
static bool posix_lifecycle_cell_selected( const char * label )
{
    const char * list = getenv( "ROCKETNET_POSIX_LIFECYCLE_CELLS" );
    if ( !list || list[0] == '\0' )
        return true;
    const size_t length = strlen( label );
    const char * cursor = list;
    while ( *cursor )
    {
        const char * end = strchr( cursor, ',' );
        const size_t item = end ? (size_t) ( end - cursor ) : strlen( cursor );
        if ( item == length && strncmp( cursor, label, length ) == 0 )
            return true;
        if ( !end )
            break;
        cursor = end + 1;
    }
    return false;
}

static int posix_lifecycle_server_cells( struct posix_lifecycle_cell_t * cells )
{
    int count = 0;
    memset( cells, 0, sizeof( struct posix_lifecycle_cell_t ) * 8 );
    cells[count].label = "portable";
    cells[count].driver = "portable";
    cells[count].pump = NULL;
    count++;
#if defined(__linux__)
    cells[count].label = "gso";
    cells[count].driver = "gso";
    count++;
    if ( posix_lifecycle_io_uring_offered() )
    {
        cells[count].label = "io_uring";
        cells[count].driver = "io_uring";
        count++;
    }
    else
    {
        printf( "posix_lifecycle cell=io_uring [SKIPPED this kernel disables io_uring]\n" );
    }
#if ROCKETNET_XDP
    if ( posix_lifecycle_xdp_offered() )
    {
        cells[count].label = "xdp";
        cells[count].driver = "xdp";
        cells[count].xdp = true;
        count++;
    }
    else
    {
        printf( "posix_lifecycle cell=xdp [SKIPPED needs root inside a virtme-ng guest, or root and ROCKETNET_TEST_XDP_VETH=1]\n" );
    }
#endif
#endif
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
    memset( cells, 0, sizeof( struct posix_lifecycle_cell_t ) * 8 );
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
#if defined(__linux__) && ROCKETNET_XDP
    if ( cell->xdp )
        posix_lifecycle_xdp_setup();
#endif
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
    if ( strcmp( driver, "xdp" ) == 0 )
    {
        // one queue, on the pair's one channel
        config.num_xdp_queues = 1;
        config.num_os_cpus = 1;
    }
    snprintf( config.driver, sizeof( config.driver ), "%s", driver );
    return rocketnet_server_transport_create( NULL, public_address, &config );
}

static struct rocketnet_client_transport_t * posix_lifecycle_create_client( const char * driver,
                                                                            const uint8_t * connect_token,
                                                                            bool far_namespace = false )
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.driver, sizeof( config.driver ), "%s", driver );
#if defined(__linux__)
    if ( far_namespace )
    {
        // the client's socket and threads open in the far end's namespace;
        // the two namespace descriptors close before the count is read
        const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
        const int far_ns = open( "/var/run/netns/" POSIX_LIFECYCLE_XDP_NETNS, O_RDONLY );
        POSIX_LIFECYCLE_CHECK( self_ns >= 0 && far_ns >= 0 );
        POSIX_LIFECYCLE_CHECK( setns( far_ns, CLONE_NEWNET ) == 0 );
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
        POSIX_LIFECYCLE_CHECK( setns( self_ns, CLONE_NEWNET ) == 0 );
        close( self_ns );
        close( far_ns );
        return client;
    }
#else
    (void) far_namespace;
#endif
    return rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
}

static bool posix_lifecycle_write_token( const char * address, uint8_t * connect_token )
{
    return rocketnet_write_dev_connect_token( address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token );
}

static void posix_lifecycle_pump( struct rocketnet_server_transport_t * server,
                                  struct rocketnet_client_transport_t * client,
                                  double seconds )
{
    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < seconds )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }
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
    if ( !posix_lifecycle_cell_selected( cell->label ) )
        return;
    posix_lifecycle_enter_cell( cell );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    // the worker initialized to run this case, so close that first generation
    // and then alternate 1000 times, ending initialized for the worker's term
    rocketnet_term();
    const int count = posix_lifecycle_count( cell, 1000 );
    for ( int i = 0; i < count; i++ )
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
    if ( !posix_lifecycle_cell_selected( cell->label ) )
        return;
    posix_lifecycle_enter_cell( cell );

    char bind_address[64];
    posix_lifecycle_bind_address( cell, bind_address, sizeof( bind_address ) );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    const int count = posix_lifecycle_count( cell, 1000 );
    for ( int i = 0; i < count; i++ )
    {
        struct rocketnet_server_transport_t * server = posix_lifecycle_create_server( cell->driver, bind_address, 8 );
        POSIX_LIFECYCLE_CHECK( server != NULL );
        rocketnet_server_transport_destroy( server );
    }

    posix_lifecycle_check_flat( "posix_server_create_destroy_1000", cell->label, &before, posix_lifecycle_seconds() - start );

    posix_lifecycle_leave_cell();
}

static void posix_lifecycle_client_create_destroy_cell( const struct posix_lifecycle_cell_t * cell )
{
    if ( !posix_lifecycle_cell_selected( cell->label ) )
        return;
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
    // kernel for that same port again, which is the BSD reuse question (the
    // xdp cell's port is fixed)
    char bind_address[64];
    posix_lifecycle_bind_address( server_cell, bind_address, sizeof( bind_address ) );
    struct rocketnet_server_transport_t * probe = posix_lifecycle_create_server( server_cell->driver, bind_address, 8 );
    POSIX_LIFECYCLE_CHECK( probe != NULL );
    const uint16_t port = rocketnet_server_transport_port( probe );
    rocketnet_server_transport_destroy( probe );
    char address[64];
    posix_lifecycle_server_address( server_cell, port, address, sizeof( address ) );

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
        struct rocketnet_client_transport_t * client = posix_lifecycle_create_client( client_cell->driver, connect_token, server_cell->xdp );
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

    char bind_address[64];
    posix_lifecycle_bind_address( server_cell, bind_address, sizeof( bind_address ) );
    struct rocketnet_server_transport_t * server = posix_lifecycle_create_server( server_cell->driver, bind_address, 8 );
    POSIX_LIFECYCLE_CHECK( server != NULL );
    char address[64];
    posix_lifecycle_server_address( server_cell, rocketnet_server_transport_port( server ), address, sizeof( address ) );

    char label[128];
    snprintf( label, sizeof( label ), "%s+%s", server_cell->label, client_cell->label );

    const struct posix_lifecycle_counts_t before = posix_lifecycle_snapshot();
    const double start = posix_lifecycle_seconds();

    const int count = posix_lifecycle_count( server_cell, 1000 );
    for ( int i = 0; i < count; i++ )
    {
        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        POSIX_LIFECYCLE_CHECK( posix_lifecycle_write_token( address, connect_token ) );
        struct rocketnet_client_transport_t * client = posix_lifecycle_create_client( client_cell->driver, connect_token, server_cell->xdp );
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

    char bind_address[64];
    posix_lifecycle_bind_address( server_cell, bind_address, sizeof( bind_address ) );

    for ( int cycle = 0; cycle < 100; cycle++ )
    {
        struct rocketnet_server_transport_t * server = posix_lifecycle_create_server( server_cell->driver, bind_address, 8 );
        POSIX_LIFECYCLE_CHECK( server != NULL );
        char address[64];
        posix_lifecycle_server_address( server_cell, rocketnet_server_transport_port( server ), address, sizeof( address ) );

        struct rocketnet_client_transport_t * clients[8];
        for ( int i = 0; i < 8; i++ )
        {
            uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
            POSIX_LIFECYCLE_CHECK( posix_lifecycle_write_token( address, connect_token ) );
            clients[i] = posix_lifecycle_create_client( client_cell->driver, connect_token, server_cell->xdp );
            POSIX_LIFECYCLE_CHECK( clients[i] != NULL );
            POSIX_LIFECYCLE_CHECK( posix_lifecycle_connect( server, clients[i], 5.0 ) );
        }

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
            if ( !posix_lifecycle_cell_selected( server_cells[s].label ) )
                continue;
            run( &server_cells[s], &client_cells[c] );
        }
    }
}

void rocketnet_posix_init_term_1000()
{
    struct posix_lifecycle_cell_t cells[8];
    const int count = posix_lifecycle_server_cells( cells );
    for ( int i = 0; i < count; i++ )
        posix_lifecycle_init_term_cell( &cells[i] );
}

void rocketnet_posix_server_create_destroy_1000()
{
    struct posix_lifecycle_cell_t cells[8];
    const int count = posix_lifecycle_server_cells( cells );
    for ( int i = 0; i < count; i++ )
        posix_lifecycle_server_create_destroy_cell( &cells[i] );
}

void rocketnet_posix_client_create_destroy_1000()
{
    struct posix_lifecycle_cell_t cells[8];
    const int count = posix_lifecycle_client_cells( cells );
    for ( int i = 0; i < count; i++ )
        posix_lifecycle_client_create_destroy_cell( &cells[i] );
}

void rocketnet_posix_server_restart_same_port_100()
{
    posix_lifecycle_run_combined( posix_lifecycle_server_restart_same_port_cell );
}

void rocketnet_posix_connect_disconnect_1000()
{
    posix_lifecycle_run_combined( posix_lifecycle_connect_disconnect_cell );
}

void rocketnet_posix_destroy_under_traffic_100()
{
    posix_lifecycle_run_combined( posix_lifecycle_destroy_under_traffic_cell );
}

#else // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// The suite builds on Windows too; the POSIX cases have nothing to count there.
void rocketnet_posix_init_term_1000() {}
void rocketnet_posix_server_create_destroy_1000() {}
void rocketnet_posix_client_create_destroy_1000() {}
void rocketnet_posix_server_restart_same_port_100() {}
void rocketnet_posix_connect_disconnect_1000() {}
void rocketnet_posix_destroy_under_traffic_100() {}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
