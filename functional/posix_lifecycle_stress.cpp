/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    POSIX lifecycle stress. Every case cycles a real transport over real
    sockets and counts the process's open file descriptors and threads before
    and after the case. A count that moves is a leak and the case fails; the
    before and after counts and the run time are printed for every cell.

    The macOS counters use libproc (proc_pidinfo PROC_PIDLISTFDS and
    PROC_PIDTASKINFO pti_threadnum); the Linux counters read /proc/self/fd,
    /proc/self/task and the heap in use, counted in whole xdp umems. An xdp
    umem is a posix_memalign block of ROCKETNET_XDP_NUM_FRAMES *
    ROCKETNET_XDP_FRAME_SIZE bytes per queue, freed with free() at destroy
    (sdk/rocketnet_server_socket_linux.cpp:1716-1718 and :1999), with no mlock,
    so it moves neither VmLck nor VmPin; the kernel charges its pin to the
    user's RLIMIT_MEMLOCK account. The Linux cells keep the heap with mmap
    off and trimming off (posix_heap_keep below), so the umem is served from
    the heap and the address space stays mapped after its free; the free
    shows only in the heap's bytes in use. The count is glibc's bytes in use
    (mallinfo2 uordblks plus hblkhd) divided by one umem's bytes: zero
    outside an xdp server's life, and one more for every leaked umem. A live
    server (its umems and the rest of its memory) moves it by at least one,
    which the xdp server create case checks.

    The cases share one shape: a resource is created and destroyed
    repeatedly in a cell (server transport, client transport or both), and
    the process must end the cell exactly as it started it.

    On Linux the server cells are portable, gso, io_uring and xdp, each with
    the portable client. The io_uring cell is not in the suite on a kernel
    whose kernel.io_uring_disabled is not 0. The xdp cell needs root, a
    network namespace and a veth pair, so it is in the suite only as root
    inside a virtme-ng guest (or as root with ROCKETNET_TEST_XDP_VETH=1). Its server binds the near end of the pair on a fixed port,
    its clients open their sockets in the far end's namespace, and its
    1000-count loops run 100 times, since every create attaches to the pair.
    ROCKETNET_POSIX_LIFECYCLE_CELLS, a comma list of server transport names, runs
    only those cells. On Linux every case is registered once per server cell
    (posix_<case>_<cell>), so each run covers one cell. On macOS the 1000-cycle
    connect/disconnect case is registered once per exact server/client pair.

    A connect waits on the client's send rate: the client sends at most once
    per 1/send_rate, 10 per second (sdk/rocketnet_client_protocol.cpp:88 and
    :296), and a handshake is three sends, so on the platform clock alone one
    connect takes 0.2 s. The client here runs on its own clock, the platform
    clock plus the send intervals the wait has skipped (its time_callback and
    the time every update is given), and the wait skips one interval each
    time the client's state moves, so the next update sends at once. The
    skip runs on every platform. A connect that does not complete prints the
    client's state, its packets sent and received, the server's packets and
    cookie requests received during that connect, and every nonzero counter
    of both ends, so a stall names the side that stopped.

    test_posix_connect_disconnect_1000 holds 1024 IPv4 sockets on loopback
    ephemeral ports while its clients cycle, as a host's other programs hold
    them. A client whose socket shares a port with one of them never hears
    its server, since the IPv4 datagrams to that port reach the held socket.
    Darwin gives a dual stack IPv6 socket such a port, so the macOS client of
    an IPv4 server binds an IPv4 socket (sdk/rocketnet_client_transport.cpp:796).
*/

#include "../test/native_shutdown.h"
#include "rocketnet_counters.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"

#if !defined(_WIN32)

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// defined in functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#else
#include <dirent.h>
#include <fcntl.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <sched.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

#if defined(__linux__) && ROCKETNET_XDP
#include "rocketnet_server_socket.h"
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
    long umems;
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

// The heap in use in whole xdp umems (see the header): a leaked umem is a
// difference, and the few MB the cells hold besides it never reach one. Zero
// without glibc or xdp, and on macOS.
#if defined(__GLIBC__) && ROCKETNET_XDP
static const unsigned long long posix_umem_bytes = (unsigned long long) ROCKETNET_XDP_NUM_FRAMES * ROCKETNET_XDP_FRAME_SIZE;
#endif

static long posix_heap_umems()
{
#if defined(__GLIBC__) && ROCKETNET_XDP
    const struct mallinfo2 info = mallinfo2();
    return (long) ( ( (unsigned long long) info.uordblks + info.hblkhd ) / posix_umem_bytes );
#else
    return 0;
#endif
}

static posix_resource_counts_t posix_read_counts()
{
    posix_resource_counts_t counts;
    counts.fds = posix_open_file_descriptors();
    counts.threads = posix_threads();
    counts.umems = posix_heap_umems();
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
        if ( current.fds == previous.fds && current.threads == previous.threads && current.umems == previous.umems )
            return current;
        previous = current;
    }
    return previous;
}

// pthread_join returns once the kernel clears the thread id, and the thread
// leaves /proc/self/task (read by posix_threads, above) only when the kernel
// reaps it afterwards. On a machine two suites share, an exited thread can
// stay listed past two equal reads 1 ms apart, and posix_stable_counts then
// reads it as a leak (threads 1 -> 2). The after counts are read here: the
// stable counts are polled every millisecond until they equal the before
// counts or POSIX_SETTLE_SECONDS have passed. A thread, descriptor or umem
// really left keeps its count, and the case still fails at the deadline.
static const double POSIX_SETTLE_SECONDS = 10.0;

static posix_resource_counts_t posix_settled_counts( const posix_resource_counts_t * before )
{
    const double deadline = rocketnet_platform_time() + POSIX_SETTLE_SECONDS;
    posix_resource_counts_t current = posix_stable_counts();
    while ( ( current.fds != before->fds || current.threads != before->threads || current.umems != before->umems ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
        current = posix_stable_counts();
    }
    return current;
}

// Each cell pins its server and client transport through the canonical config fields.
struct posix_cell_t
{
    const char * name;
    const char * server_transport_io;
    const char * server_granted;
    const char * client_transport_io;
    const char * client_granted;
    bool xdp;
};

static const posix_cell_t posix_cells[] =
{
#if defined(__linux__)
    // the Linux server cells, each against the portable client
    { "server portable, client portable", "portable", "portable", "portable", "portable", false },
    { "server gso, client portable",      "gso",      "gso",      "portable", "portable", false },
    { "server io_uring, client portable", "io_uring", "io_uring", "portable", "portable", false },
    { "server xdp, client portable",      "xdp",      "xdp",      "portable", "portable", true },
#else
    { "server portable, client portable", "portable",      "portable", "portable", "portable", false },
    { "server portable, client msgx",     "portable",      "portable", "msgx",     "msgx",     false },
    { "server msgx, client portable",     "msgx",          "msgx",     "portable", "portable", false },
    { "server msgx, client msgx",         "msgx",          "msgx",     "msgx",     "msgx",     false },
#endif
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

// The xdp cell's pair: the server on the near end, the clients in the far
// end's namespace. Made once per process when the first xdp cell begins, and
// removed at exit.

#define POSIX_XDP_NETNS       "rnxdplc"
#define POSIX_XDP_NEAR        "rnxdplc0"
#define POSIX_XDP_FAR         "rnxdplc1"
#define POSIX_XDP_HOST        "10.77.3.1"
#define POSIX_XDP_FAR_HOST    "10.77.3.2"
#define POSIX_XDP_PORT        40421

#if defined(__linux__) && ROCKETNET_XDP

static bool posix_xdp_made = false;

static void posix_xdp_teardown()
{
    if ( posix_xdp_made )
    {
        // deleting the namespace takes the far end, and with it the pair
        if ( system( "ip netns del " POSIX_XDP_NETNS " 2>/dev/null; ip link del " POSIX_XDP_NEAR " 2>/dev/null" ) != 0 )
        {
            // already gone
        }
        posix_xdp_made = false;
    }
    rocketnet_server_socket_forbid_commit();
}

static void posix_xdp_run( const char * command )
{
    const int result = system( command );
    if ( result != 0 )
        printf( "posix lifecycle stress command failed (%d): %s\n", result, command );
    posix_check( result == 0 );
}

static bool posix_xdp_offered()
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

static void posix_xdp_setup()
{
    if ( posix_xdp_made )
        return;

    atexit( posix_xdp_teardown );

    struct statfs bpffs;
    if ( statfs( "/sys/fs/bpf", &bpffs ) != 0 || (uint64_t) bpffs.f_type != (uint64_t) 0xcafe4a11 )
        posix_xdp_run( "mount -t bpf bpf /sys/fs/bpf" );

    if ( system( "ip netns del " POSIX_XDP_NETNS " 2>/dev/null; ip link del " POSIX_XDP_NEAR " 2>/dev/null" ) != 0 )
    {
        // nothing left from an earlier run
    }

    posix_xdp_made = true;
    posix_xdp_run( "ip netns add " POSIX_XDP_NETNS );
    posix_xdp_run( "ip link add " POSIX_XDP_NEAR " mtu 1500 type veth peer name " POSIX_XDP_FAR " mtu 1500" );
    posix_xdp_run( "ip link set " POSIX_XDP_FAR " netns " POSIX_XDP_NETNS );
    posix_xdp_run( "ip addr add " POSIX_XDP_HOST "/24 dev " POSIX_XDP_NEAR );
    posix_xdp_run( "ip -n " POSIX_XDP_NETNS " addr add " POSIX_XDP_FAR_HOST "/24 dev " POSIX_XDP_FAR );
    posix_xdp_run( "ip link set " POSIX_XDP_NEAR " up" );
    // the busy-poll pair the xdp pre-flight verifies
    posix_xdp_run( "echo 2 > /sys/class/net/" POSIX_XDP_NEAR "/napi_defer_hard_irqs" );
    posix_xdp_run( "echo 200000 > /sys/class/net/" POSIX_XDP_NEAR "/gro_flush_timeout" );
    posix_xdp_run( "ip -n " POSIX_XDP_NETNS " link set lo up" );
    posix_xdp_run( "ip -n " POSIX_XDP_NETNS " link set " POSIX_XDP_FAR " up" );

    rocketnet_server_socket_allow_commit();
}

#endif // #if defined(__linux__) && ROCKETNET_XDP

// io_uring is offered when kernel.io_uring_disabled reads 0, or the file is
// absent on a kernel older than the switch
static bool posix_io_uring_offered()
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

// ROCKETNET_POSIX_LIFECYCLE_CELLS, when set, names the server transports whose
// cells run
static const char * posix_only_server_transport_io = NULL;
#if defined(__APPLE__)
// A macOS connect/disconnect case selects one exact server/client pair. The
// server-only selector would run both client transports under one watchdog.
static const char * posix_only_client_transport_io = NULL;
#endif

static bool posix_cell_selected( const posix_cell_t * cell )
{
#if defined(__APPLE__)
    if ( posix_only_client_transport_io && strcmp( posix_only_client_transport_io, cell->client_transport_io ) != 0 )
        return false;
#endif
    if ( posix_only_server_transport_io && strcmp( posix_only_server_transport_io, cell->server_transport_io ) != 0 )
        return false;
    const char * list = getenv( "ROCKETNET_POSIX_LIFECYCLE_CELLS" );
    if ( !list || list[0] == '\0' )
        return true;
    const char * label = cell->server_transport_io;
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

// whether a cell runs here: selected, and its transport offered. The
// single-transport cases of an unoffered transport are not in the suite
// (rocketnet_test_posix_io_uring_offered, rocketnet_test_posix_xdp_offered)
static bool posix_cell_runs( const char * name, const posix_cell_t * cell )
{
    (void) name;
    if ( !posix_cell_selected( cell ) )
        return false;
    if ( strcmp( cell->server_transport_io, "io_uring" ) == 0 && !posix_io_uring_offered() )
        return false;
    if ( cell->xdp )
    {
#if defined(__linux__) && ROCKETNET_XDP
        if ( !posix_xdp_offered() )
#endif
            return false;
    }
    return true;
}

// a 1000-count loop runs 100 times on the xdp cell
static int posix_count( const posix_cell_t * cell, int count )
{
    if ( cell->xdp && count > 100 )
        return 100;
    return count;
}

// the bind address a cell's server takes: loopback with a kernel-chosen port,
// or the near end of the pair on the xdp cell's fixed port
static const char * posix_bind_address( const posix_cell_t * cell )
{
    if ( cell->xdp )
    {
        static char xdp_address[64];
        snprintf( xdp_address, sizeof( xdp_address ), "%s:%d", POSIX_XDP_HOST, POSIX_XDP_PORT );
        return xdp_address;
    }
    return "127.0.0.1:0";
}

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

static void posix_cell_begin( const posix_cell_t * cell, posix_env_guard_t * server_guard, posix_env_guard_t * client_guard )
{
    (void) cell;
    posix_env_clear( server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    posix_env_clear( client_guard, "ROCKETNET_CLIENT_TRANSPORT" );
#if defined(__linux__) && ROCKETNET_XDP
    if ( cell->xdp )
        posix_xdp_setup();
#else
    (void) cell;
#endif
}

static void posix_cell_end( const posix_env_guard_t * server_guard, const posix_env_guard_t * client_guard )
{
    posix_env_restore( client_guard, "ROCKETNET_CLIENT_TRANSPORT" );
    posix_env_restore( server_guard, "ROCKETNET_SERVER_TRANSPORT" );
}

static rocketnet_server_transport_t * posix_server_create( const posix_cell_t * cell, const char * address, int max_clients )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;
    if ( cell->xdp )
    {
        // one queue, on the pair's one channel
        config.num_xdp_queues = 1;
        config.num_os_cpus = 1;
    }
    snprintf( config.transport, sizeof( config.transport ), "%s", cell->server_transport_io );
    int status = ROCKETNET_STATUS_OK;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, address, &config, rocketnet_platform_time(), &status );
    if ( !server )
        printf( "server create [%s] at %s failed: %s\n", cell->name, address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    posix_check( server != NULL );
    posix_check( strcmp( rocketnet_server_transport_io_name( server ), cell->server_granted ) == 0 );
    return server;
}

// the client's clock: the platform clock plus the send intervals skipped.
// Read by the client's receive thread through time_callback, so atomic.
#define POSIX_CLIENT_SEND_INTERVAL 0.1

struct posix_client_clock_t
{
    std::atomic<int> skipped;
};

static double posix_client_clock_now( posix_client_clock_t * clock )
{
    return rocketnet_platform_time() + clock->skipped.load( std::memory_order_acquire ) * POSIX_CLIENT_SEND_INTERVAL;
}

static double posix_client_clock_callback( void * context )
{
    return posix_client_clock_now( (posix_client_clock_t*) context );
}

static void posix_client_clock_skip( posix_client_clock_t * clock )
{
    clock->skipped.fetch_add( 1, std::memory_order_acq_rel );
}

static rocketnet_client_transport_t * posix_client_create( const posix_cell_t * cell, const char * server_address,
                                                           posix_client_clock_t * clock )
{
    clock->skipped.store( 0, std::memory_order_release );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    posix_check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(),
                                                    rocketnet_functional_token_timestamp( server_config.game_clock_epoch, 1000.0 ),
                                                    token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", cell->client_transport_io );
    config.time_callback = posix_client_clock_callback;
    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * client = NULL;
#if defined(__linux__)
    if ( cell->xdp )
    {
        // the client's socket and threads open in the far end's namespace;
        // the two namespace descriptors close before any count is read
        const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
        const int far_ns = open( "/var/run/netns/" POSIX_XDP_NETNS, O_RDONLY );
        posix_check( self_ns >= 0 && far_ns >= 0 );
        posix_check( setns( far_ns, CLONE_NEWNET ) == 0 );
        client = rocketnet_client_transport_create_ex( clock, token, &config, posix_client_clock_now( clock ), &status );
        posix_check( setns( self_ns, CLONE_NEWNET ) == 0 );
        close( self_ns );
        close( far_ns );
    }
    else
#endif
    {
        client = rocketnet_client_transport_create_ex( clock, token, &config, posix_client_clock_now( clock ), &status );
    }
    if ( !client )
        printf( "client create [%s] to %s failed: %s\n", cell->name, server_address, rocketnet_status_name( (enum rocketnet_status_t) status ) );
    posix_check( client != NULL );
    posix_check( strcmp( rocketnet_client_transport_io_name( client ), cell->client_granted ) == 0 );
    return client;
}

static const char * posix_server_address( const posix_cell_t * cell, rocketnet_server_transport_t * server, char * buffer, size_t buffer_bytes )
{
    snprintf( buffer, buffer_bytes, "%s:%u", cell->xdp ? POSIX_XDP_HOST : "127.0.0.1", (unsigned) rocketnet_server_transport_port( server ) );
    return buffer;
}

// IPv4 sockets held on loopback ephemeral ports while the clients cycle, as
// a host's other programs hold them (a VPN daemon's wildcard IPv4 socket was
// the one met). A client bound to a port one of these holds never hears its
// server: the IPv4 datagrams to that port reach the held socket.
#define POSIX_HELD_IPV4_PORTS 1024

static void posix_hold_ipv4_ports( int * sockets, int count )
{
    struct rlimit limit;
    posix_check( getrlimit( RLIMIT_NOFILE, &limit ) == 0 );
    const rlim_t wanted = (rlim_t) count + 1024;
    if ( limit.rlim_cur < wanted )
    {
        limit.rlim_cur = limit.rlim_max < wanted ? limit.rlim_max : wanted;
        posix_check( setrlimit( RLIMIT_NOFILE, &limit ) == 0 );
    }
    for ( int i = 0; i < count; i++ )
    {
        sockets[i] = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
        posix_check( sockets[i] >= 0 );
        struct sockaddr_in address;
        memset( &address, 0, sizeof( address ) );
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        posix_check( bind( sockets[i], (struct sockaddr*) &address, sizeof( address ) ) == 0 );
    }
}

static void posix_release_ipv4_ports( const int * sockets, int count )
{
    for ( int i = 0; i < count; i++ )
        close( sockets[i] );
}

// a stalled connect prints the client's state and every nonzero counter of
// the client and the server, so a stall names the side that stopped
static void posix_print_counters( const char * side, const struct rocketnet_counters_t * counters )
{
    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        const uint64_t value = rocketnet_counter_value( counters, i );
        if ( value != 0 )
            printf( "  %s %s = %llu\n", side, rocketnet_counter_name( i ), (unsigned long long) value );
    }
}

static void posix_print_stall( rocketnet_server_transport_t * server, rocketnet_client_transport_t * client,
                               uint64_t server_received_before, uint64_t server_no_cookie_before )
{
    const int state = rocketnet_client_transport_state( client );
    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( client, &stats );
    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( server );
    printf( "connect stalled: client state %s (%d), server connected clients %d\n", rocketnet_client_transport_state_name( state ),
            state, rocketnet_server_transport_num_connected_clients( server ) );
    printf( "  this connect: client packets sent %llu, received %llu; server packets received %llu, cookie requests %llu\n",
            (unsigned long long) stats.packets_sent, (unsigned long long) stats.packets_received,
            (unsigned long long) ( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_PACKETS_RECEIVED ) - server_received_before ),
            (unsigned long long) ( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE ) - server_no_cookie_before ) );
    posix_print_counters( "client", rocketnet_client_transport_counters( client ) );
    posix_print_counters( "server", rocketnet_server_transport_counters( server ) );
    fflush( stdout );
}

static bool posix_wait_connected( rocketnet_server_transport_t * server, rocketnet_client_transport_t * client,
                                  posix_client_clock_t * clock, double seconds )
{
    const double deadline = rocketnet_platform_time() + seconds;
    int previous_state = rocketnet_client_transport_state( client );
    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( server );
    const uint64_t server_received_before = rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_PACKETS_RECEIVED );
    const uint64_t server_no_cookie_before = rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE );
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_client_transport_update( client, posix_client_clock_now( clock ) );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        const int state = rocketnet_client_transport_state( client );
        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            return true;
        if ( state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        {
            posix_print_stall( server, client, server_received_before, server_no_cookie_before );
            return false;
        }
        if ( state != previous_state )
        {
            // the state moved: the next update sends without waiting out the
            // interval. A send that goes unanswered is resent on the client's
            // own schedule, so its timeouts run at the platform clock's pace.
            previous_state = state;
            posix_client_clock_skip( clock );
            continue;
        }
        rocketnet_platform_sleep( 0.0002 );
    }
    posix_print_stall( server, client, server_received_before, server_no_cookie_before );
    return false;
}

static bool posix_wait_server_empty( rocketnet_server_transport_t * server, rocketnet_client_transport_t * client,
                                     posix_client_clock_t * clock, double seconds )
{
    const double deadline = rocketnet_platform_time() + seconds;
    while ( rocketnet_platform_time() < deadline )
    {
        // Disconnect may defer its burst while the I/O thread owns the other
        // ring. Keep updating the client to retry that hand-off, as specified
        // by docs/CLIENT-IO.md, without advancing either clock artificially.
        rocketnet_client_transport_update( client, posix_client_clock_now( clock ) );
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
    printf( "%s [%s]: fds %d -> %d, threads %d -> %d, heap umems %ld -> %ld, %.3fs\n", name, cell->name,
            before->fds, after->fds, before->threads, after->threads, before->umems, after->umems, seconds );
    fflush( stdout );
    if ( before->fds < 0 || before->threads < 0 || before->umems < 0 ||
         before->fds != after->fds || before->threads != after->threads || before->umems != after->umems )
    {
        printf( "%s [%s]: FAIL resource count moved (fds %d -> %d, threads %d -> %d, heap umems %ld -> %ld)\n", name, cell->name,
                before->fds, after->fds, before->threads, after->threads, before->umems, after->umems );
        fflush( stdout );
        exit( 1 );
    }
}

// ---------------------------------------------------------------
// the cases
// ---------------------------------------------------------------

void rocketnet_test_posix_init_term_1000()
{
    const char * case_name = "test_posix_init_term_1000";
    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        if ( !posix_cell_runs( case_name, cell ) )
            continue;
        posix_env_guard_t server_guard, client_guard;
        posix_cell_begin( cell, &server_guard, &client_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();
        const int count = posix_count( cell, 1000 );
        for ( int i = 0; i < count; i++ )
        {
            posix_check( rocketnet_term() == ROCKETNET_STATUS_OK );
            posix_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_settled_counts( &before );

        posix_cell_end( &server_guard, &client_guard );
        posix_case_result( case_name, cell, &before, &after, seconds );
    }
}

void rocketnet_test_posix_server_create_destroy_1000()
{
    const char * case_name = "test_posix_server_create_destroy_1000";
    posix_require_loopback_bind( case_name );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        if ( !posix_cell_runs( case_name, cell ) )
            continue;
        posix_env_guard_t server_guard, client_guard;
        posix_cell_begin( cell, &server_guard, &client_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();
        // the gso and io_uring server creates cost the most, so those two
        // cells run 250 cycles and keep two suites at once under the 120
        // second case limit; the counts after the first and last cycle still
        // catch a leak per cycle
        const bool expensive_server = strcmp( cell->server_granted, "gso" ) == 0 || strcmp( cell->server_granted, "io_uring" ) == 0;
        const int count = expensive_server ? 250 : posix_count( cell, 1000 );
        for ( int i = 0; i < count; i++ )
        {
            rocketnet_server_transport_t * server = posix_server_create( cell, posix_bind_address( cell ), 4 );

            // a live xdp server holds at least one umem, so the count sees
            // it: a count that cannot move cannot catch a leaked umem
            if ( i == 0 && cell->xdp )
            {
                const posix_resource_counts_t live = posix_stable_counts();
                printf( "test_posix_server_create_destroy_1000 live server [%s]: heap umems %ld -> %ld\n", cell->name, before.umems, live.umems );
                fflush( stdout );
#if defined(__GLIBC__) && ROCKETNET_XDP
                posix_check( live.umems >= before.umems + 1 );
#endif
            }

            rn_native_server_transport_stop_and_destroy( server );

            // a leak per cycle exhausts a kernel limit long before the last of
            // 1000 creates, so the counts are also compared after the first
            if ( i == 0 )
            {
                const posix_resource_counts_t first = posix_settled_counts( &before );
                posix_case_result( "test_posix_server_create_destroy_1000 after one cycle", cell, &before, &first, posix_now_seconds() - start );
            }
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_settled_counts( &before );

        posix_cell_end( &server_guard, &client_guard );
        posix_case_result( case_name, cell, &before, &after, seconds );
    }
}

void rocketnet_test_posix_client_create_destroy_1000()
{
    const char * case_name = "test_posix_client_create_destroy_1000";
    posix_require_loopback_bind( case_name );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        if ( !posix_cell_runs( case_name, cell ) )
            continue;
        posix_env_guard_t server_guard, client_guard;
        posix_cell_begin( cell, &server_guard, &client_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();
        const int count = posix_count( cell, 1000 );
        for ( int i = 0; i < count; i++ )
        {
            posix_client_clock_t clock;
            rocketnet_client_transport_t * client = posix_client_create( cell, "127.0.0.1:40000", &clock );
            rn_native_client_transport_stop_and_destroy( client );
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_settled_counts( &before );

        posix_cell_end( &server_guard, &client_guard );
        posix_case_result( case_name, cell, &before, &after, seconds );
    }
}

// The port the restart case binds for its whole life. A kernel-chosen
// ephemeral port is not the case's to keep: the discovery server releases it,
// and any sibling test's client or scratch socket binding port 0 can take it
// before the next rebind. On Linux a failing run saw the io_uring rung refuse
// a discovered 127.0.0.1:44960 with errno 98 (EADDRINUSE), then exhaust the
// ladder, because the port had gone to a socket that does not share the
// transport's SO_REUSEPORT. Take a port below the kernel's ephemeral range
// instead, in a slice this cell alone scans.
static unsigned posix_reserved_restart_port( int cell_index )
{
    const unsigned first_port = 1025;
    unsigned last_port = 29999;
#if defined(__linux__)
    FILE * port_range = fopen( "/proc/sys/net/ipv4/ip_local_port_range", "r" );
    if ( !port_range )
        return 0;
    unsigned ephemeral_first = 0;
    unsigned ephemeral_last = 0;
    const int range_read = fscanf( port_range, "%u %u", &ephemeral_first, &ephemeral_last );
    fclose( port_range );
    if ( range_read != 2 || ephemeral_first <= first_port || ephemeral_last < ephemeral_first )
        return 0;
    if ( last_port >= ephemeral_first )
        last_port = ephemeral_first - 1;
#endif
    if ( last_port < first_port )
        return 0;

    // the restart_same_port transport cases run concurrently in the suite and
    // each reserves its own port. seeding every reservation on the same
    // getpid() starts them all at the same port, so their probe-then-release
    // reservations can pick the same port and one rebind fails with
    // EADDRINUSE. seed on the cell index so each case scans a disjoint
    // subrange.
    const unsigned slice_count = (unsigned) posix_cell_count;
    if ( slice_count == 0 )
        return 0;
    const unsigned port_count = last_port - first_port + 1;
    const unsigned slice = port_count / slice_count;
    const unsigned slice_index = (unsigned) cell_index % slice_count;
    const unsigned base = first_port + slice_index * slice;
    const unsigned remainder = ( slice_index == slice_count - 1 ) ? port_count % slice_count : 0;
    const unsigned usable = slice + remainder;
    if ( usable == 0 )
        return 0;
    unsigned port = base + (unsigned) getpid() % usable;
    if ( port > last_port )
        return 0;
    for ( unsigned i = 0; i < usable; i++ )
    {
        const int fd = (int) socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
        if ( fd < 0 )
            return 0;

        struct sockaddr_in address;
        memset( &address, 0, sizeof( address ) );
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl( INADDR_ANY );
        address.sin_port = htons( (uint16_t) port );
        const int result = bind( fd, (struct sockaddr*) &address, sizeof( address ) );
        const int error = errno;
        close( fd );
        if ( result == 0 )
            return port;
        if ( error != EADDRINUSE && error != EACCES )
            return 0;
        port = base + ( port - base + 1 ) % usable;
    }
    return 0;
}

void rocketnet_test_posix_server_restart_same_port_100()
{
    const char * case_name = "test_posix_server_restart_same_port_100";
    posix_require_loopback_bind( case_name );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        if ( !posix_cell_runs( case_name, cell ) )
            continue;
        posix_env_guard_t server_guard, client_guard;
        posix_cell_begin( cell, &server_guard, &client_guard );

        const posix_resource_counts_t before = posix_stable_counts();

        unsigned fixed_port = 0;
        if ( cell->xdp )
        {
            rocketnet_server_transport_t * discovery = posix_server_create( cell, posix_bind_address( cell ), 4 );
            fixed_port = (unsigned) rocketnet_server_transport_port( discovery );
            rn_native_server_transport_stop_and_destroy( discovery );
        }
        else
            fixed_port = posix_reserved_restart_port( cell_index );
        posix_check( fixed_port != 0 );

        char fixed_address[64];
        snprintf( fixed_address, sizeof( fixed_address ), "%s:%u", cell->xdp ? POSIX_XDP_HOST : "127.0.0.1", fixed_port );

        const double start = posix_now_seconds();
        for ( int i = 0; i < 100; i++ )
        {
            rocketnet_server_transport_t * server = posix_server_create( cell, fixed_address, 4 );
            posix_check( rocketnet_server_transport_port( server ) == fixed_port );

            char server_address[64];
            posix_server_address( cell, server, server_address, sizeof( server_address ) );
            posix_client_clock_t clock;
            rocketnet_client_transport_t * client = posix_client_create( cell, server_address, &clock );
            posix_check( posix_wait_connected( server, client, &clock, 5.0 ) );
            rn_native_client_transport_stop_and_destroy( client );
            rn_native_server_transport_stop_and_destroy( server );
        }
        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_settled_counts( &before );

        posix_cell_end( &server_guard, &client_guard );
        posix_case_result( case_name, cell, &before, &after, seconds );
    }
}

void rocketnet_test_posix_connect_disconnect_1000()
{
    const char * case_name = "test_posix_connect_disconnect_1000";
    posix_require_loopback_bind( case_name );

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        if ( !posix_cell_runs( case_name, cell ) )
            continue;
        posix_env_guard_t server_guard, client_guard;
        posix_cell_begin( cell, &server_guard, &client_guard );

        const posix_resource_counts_t before = posix_stable_counts();

        rocketnet_server_transport_t * server = posix_server_create( cell, posix_bind_address( cell ), 8 );
        char server_address[64];
        posix_server_address( cell, server, server_address, sizeof( server_address ) );

        static int held_ports[POSIX_HELD_IPV4_PORTS];
        posix_hold_ipv4_ports( held_ports, POSIX_HELD_IPV4_PORTS );

        const double start = posix_now_seconds();
        const int count = posix_count( cell, 1000 );
        for ( int i = 0; i < count; i++ )
        {
            posix_client_clock_t clock;
            rocketnet_client_transport_t * client = posix_client_create( cell, server_address, &clock );
            if ( !posix_wait_connected( server, client, &clock, 5.0 ) )
                printf( "%s [%s]: connect stalled at cycle %d\n", case_name, cell->name, i );
            posix_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
            rocketnet_client_transport_disconnect( client );
            posix_check( posix_wait_server_empty( server, client, &clock, 5.0 ) );
            rn_native_client_transport_stop_and_destroy( client );
        }
        const double seconds = posix_now_seconds() - start;

        posix_release_ipv4_ports( held_ports, POSIX_HELD_IPV4_PORTS );
        rn_native_server_transport_stop_and_destroy( server );
        const posix_resource_counts_t after = posix_settled_counts( &before );

        posix_cell_end( &server_guard, &client_guard );
        posix_case_result( case_name, cell, &before, &after, seconds );
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
    const char * case_name = "test_posix_destroy_under_traffic_100";
    posix_require_loopback_bind( case_name );

    const int client_count = 8;

    for ( int cell_index = 0; cell_index < posix_cell_count; cell_index++ )
    {
        const posix_cell_t * cell = &posix_cells[cell_index];
        if ( !posix_cell_runs( case_name, cell ) )
            continue;
        posix_env_guard_t server_guard, client_guard;
        posix_cell_begin( cell, &server_guard, &client_guard );

        const posix_resource_counts_t before = posix_stable_counts();
        const double start = posix_now_seconds();

        for ( int iteration = 0; iteration < 100; iteration++ )
        {
            rocketnet_server_transport_t * server = posix_server_create( cell, posix_bind_address( cell ), client_count );
            char server_address[64];
            posix_server_address( cell, server, server_address, sizeof( server_address ) );

            rocketnet_client_transport_t * clients[client_count];
            posix_client_clock_t clocks[client_count];
            for ( int i = 0; i < client_count; i++ )
            {
                clients[i] = posix_client_create( cell, server_address, &clocks[i] );
                posix_check( posix_wait_connected( server, clients[i], &clocks[i], 5.0 ) );
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
            rn_native_server_transport_stop_and_destroy( server );

            running.store( false, std::memory_order_release );
            for ( int i = 0; i < client_count; i++ )
                threads[i].join();
            for ( int i = 0; i < client_count; i++ )
                rn_native_client_transport_stop_and_destroy( clients[i] );
        }

        const double seconds = posix_now_seconds() - start;
        const posix_resource_counts_t after = posix_settled_counts( &before );

        posix_cell_end( &server_guard, &client_guard );
        posix_case_result( case_name, cell, &before, &after, seconds );
    }
}

#if defined(__linux__)

// one registered case per Linux server cell: the case runs that cell alone

// The server transport and the gso transport are single allocations of 56 MB
// and 116 MB (strace of one create: two anonymous mmaps of those sizes),
// past glibc's largest mmap threshold, so every create maps them fresh and
// the value-initialising placement new (sdk/rocketnet_server_transport.cpp:1163,
// sdk/rocketnet_server_gso_linux.cpp:644) faults every page in: ~42k minor
// faults and ~0.12 s of system time a gso create on a 64-core Linux member.
// A per-cell case serves those allocations from the main heap and keeps the
// heap untrimmed, so a create after the first reuses resident pages. The
// counts read file descriptors, threads and the heap's bytes in use, none
// of which this moves.
static void posix_heap_keep( bool keep )
{
#if defined(__SANITIZE_ADDRESS__)
    (void) keep;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    (void) keep;
#elif defined(__GLIBC__)
    posix_check( mallopt( M_MMAP_MAX, keep ? 0 : 65536 ) == 1 );
    posix_check( mallopt( M_TRIM_THRESHOLD, keep ? INT_MAX : 128 * 1024 ) == 1 );
#else
    (void) keep;
#endif
#elif defined(__GLIBC__)
    posix_check( mallopt( M_MMAP_MAX, keep ? 0 : 65536 ) == 1 );
    posix_check( mallopt( M_TRIM_THRESHOLD, keep ? INT_MAX : 128 * 1024 ) == 1 );
#else
    (void) keep;
#endif
}

#define POSIX_LINUX_CELL_CASE( case_name, transport )                                                                    \
void rocketnet_test_##case_name##_##transport()                                                                        \
{                                                                                                                   \
    posix_only_server_transport_io = #transport;                                                                             \
    posix_heap_keep( true );                                                                                        \
    rocketnet_test_##case_name();                                                                                   \
    posix_heap_keep( false );                                                                                       \
    posix_only_server_transport_io = NULL;                                                                                \
}

#define POSIX_LINUX_CELL_CASES( case_name )                                                                           \
    POSIX_LINUX_CELL_CASE( case_name, portable )                                                                    \
    POSIX_LINUX_CELL_CASE( case_name, gso )                                                                         \
    POSIX_LINUX_CELL_CASE( case_name, io_uring )                                                                    \
    POSIX_LINUX_CELL_CASE( case_name, xdp )

bool rocketnet_test_posix_io_uring_offered()
{
    return posix_io_uring_offered();
}

bool rocketnet_test_posix_xdp_offered()
{
#if ROCKETNET_XDP
    return posix_xdp_offered();
#else
    return false;
#endif
}

POSIX_LINUX_CELL_CASES( posix_init_term_1000 )
POSIX_LINUX_CELL_CASES( posix_server_create_destroy_1000 )
POSIX_LINUX_CELL_CASES( posix_client_create_destroy_1000 )
POSIX_LINUX_CELL_CASES( posix_server_restart_same_port_100 )
POSIX_LINUX_CELL_CASES( posix_connect_disconnect_1000 )
POSIX_LINUX_CELL_CASES( posix_destroy_under_traffic_100 )

#endif // #if defined(__linux__)

#if defined(__APPLE__)
// Keep all four 1000-cycle pairs, with the same resource assertions and the
// same per-test watchdog. Each registered case runs exactly one pair.
#define POSIX_MAC_CONNECT_CASE( server_io, client_io )                             \
void rocketnet_test_posix_connect_disconnect_1000_##server_io##_##client_io()      \
{                                                                                  \
    posix_only_server_transport_io = #server_io;                                   \
    posix_only_client_transport_io = #client_io;                                   \
    int selected = 0;                                                              \
    for ( int i = 0; i < posix_cell_count; i++ )                                     \
        selected += posix_cell_selected( &posix_cells[i] ) ? 1 : 0;                 \
    if ( selected != 1 )                                                           \
        printf( "macOS POSIX lifecycle filter excludes or duplicates %s/%s\n",   \
                #server_io, #client_io );                                           \
    posix_check( selected == 1 );                                                   \
    rocketnet_test_posix_connect_disconnect_1000();                                \
    posix_only_client_transport_io = NULL;                                         \
    posix_only_server_transport_io = NULL;                                         \
}

POSIX_MAC_CONNECT_CASE( portable, portable )
POSIX_MAC_CONNECT_CASE( portable, msgx )
POSIX_MAC_CONNECT_CASE( msgx, portable )
POSIX_MAC_CONNECT_CASE( msgx, msgx )
#undef POSIX_MAC_CONNECT_CASE
#endif

#endif // #if !defined(_WIN32)

/*
    macOS record (Darwin arm64, macOS 27.0.1, Apple clang 21.0.0, Debug
    build, ROCKETNET_TEST_VERBOSE=1 so the suite prints the lines of a
    passing case). Each run is one ./bin/functional --run <case> over the six
    macOS cells (posix_cells). Seconds per run:

        case                                 r1    r2    r3    r4    r5
        test_posix_init_term_1000                 12    11    10    8     8
        test_posix_server_create_destroy_1000     owed: a run takes about 150 s
        test_posix_client_create_destroy_1000     29    28    27    26    27
        test_posix_server_restart_same_port_100   23    22    22    20    20
        test_posix_connect_disconnect_1000        52    54    52    51    50
        test_posix_destroy_under_traffic_100      77    76    78    78    78

    Every cell of every run printed fds 4 -> 4 and threads 1 -> 1. One
    earlier run of test_posix_client_create_destroy_1000 on the same host, a tree
    whose only difference in this file was this record, printed threads
    1 -> 5 (fds 4 -> 4) in [server msgx forced portable, client msgx] and
    failed in posix_case_result, with the host's load average near 15.
    Twelve runs of that case over the msgx server cells
    (ROCKETNET_POSIX_LIFECYCLE_CELLS=msgx) right after it passed with flat
    counts. The four extra threads are not explained yet.

    The red proof, each a scratch build that is not kept, each run as
    ./bin/functional --run test_posix_client_create_destroy_1000 and failing at
    posix_case_result:

    - the macOS rocketnet_platform_socket_destroy skips its close:
      [server portable, client portable] FAIL fds 4 -> 1004, threads 1 -> 1.
    - the macOS rocketnet_platform_thread_join skips pthread_join
      and the thread, its function returned, does not exit:
      [server portable, client portable] FAIL fds 4 -> 4, threads 1 -> 1001.

    The thread count is pti_threadnum (posix_threads), the threads alive in
    the task. A skipped join of a thread that then exits leaves no live thread
    and the case stays green, threads 1 -> 1 in every cell. A
    skipped join in rocketnet_client_transport_destroy
    lets destroy free the client while its I/O thread still steps it, and the
    process ends in SIGSEGV in rocketnet_client_io_step, called from
    rocketnet_client_io_thread_function.
*/
