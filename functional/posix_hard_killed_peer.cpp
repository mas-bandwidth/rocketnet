/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    A hard-killed peer, and an ICMP error on a UDP socket, must never stall or
    degrade a receive. The BSD arms live in sdk/rocketnet_platform.cpp; these
    cases drive them with real sockets, a child process ended by SIGKILL, and
    (on macOS) the injected-errno seam for the errors the host will not produce
    on demand. The transport matrix cells are shared with the Linux card, which
    adds the Linux cells.
*/

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"

#if defined(__APPLE__) || defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char ** environ;

static void rocketnet_posix_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "posix hard-killed check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !( condition ) )                                                                                       \
        rocketnet_posix_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
} while( 0 )

// a free loopback UDP port: bind one, read it, release it. the caller rebinds
// it immediately; a loopback test tolerates the small race.
static int rocketnet_posix_free_port()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( socket );
    const int port = address.port;
    rocketnet_platform_socket_destroy( socket );
    return port;
}

static rocketnet_platform_socket_t * rocketnet_posix_bind( const char * host, int port )
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, host );
    address.port = port;
    return rocketnet_platform_socket_create( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
}

// bind an ephemeral port and keep it bound: asking the kernel for a port and
// releasing it, then rebinding somewhere later, races other sockets on the
// host and can hand the same port to two of this case's own sockets. bind(0)
// updates the address with the port the kernel actually granted.
static rocketnet_platform_socket_t * rocketnet_posix_bind_any( const char * host, int * port )
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, host );
    address.port = 0;
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );
    *port = address.port;
    return socket;
}

static void rocketnet_posix_settle()
{
    // loopback arrival is asynchronous; a short settle after a send keeps the
    // assertions about a single drain honest.
    rocketnet_platform_sleep( 0.05 );
}

// wait until the socket is readable, bounded. returns true when it is.
static bool rocketnet_posix_wait_readable( rocketnet_platform_socket_t * socket, double seconds )
{
    const double deadline = rocketnet_platform_time() + seconds;
    for ( ;; )
    {
        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( socket->handle, &readable );
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        if ( select( socket->handle + 1, &readable, NULL, NULL, &tv ) > 0 )
            return true;
        if ( rocketnet_platform_time() >= deadline )
            return false;
    }
}

static void rocketnet_posix_send_to( rocketnet_platform_socket_t * socket, const char * host, int port, int tag )
{
    rocketnet_address_t to;
    rocketnet_address_parse( &to, host );
    to.port = port;
    uint8_t packet[16];
    memset( packet, tag, sizeof(packet) );
    rocketnet_platform_socket_send_packet( socket, &to, packet, sizeof(packet) );
}

// the child sends heartbeats to the server until it is killed. no disconnect
// packet is ever sent, which is the whole point.
int rocketnet_posix_hard_killed_child_main( const char * host, int port )
{
    if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    // the child binds the server's family: a bracketed host is ipv6
    int child_port = 0;
    rocketnet_platform_socket_t * socket = rocketnet_posix_bind_any( host[0] == '[' ? "[::1]" : "127.0.0.1", &child_port );
    if ( !socket )
        return 1;

    printf( "posix-hard-killed-child %d\n", child_port );
    fflush( stdout );

    const double deadline = rocketnet_platform_time() + 30.0;
    int tag = 0;
    while ( rocketnet_platform_time() < deadline )
    {
        // 64 bytes: above the smallest datagram the gso and io_uring server
        // drivers stage, so every server cell sees the heartbeats
        rocketnet_address_t to;
        rocketnet_address_parse( &to, host );
        to.port = port;
        uint8_t heartbeat[64];
        memset( heartbeat, tag++, sizeof(heartbeat) );
        rocketnet_platform_socket_send_packet( socket, &to, heartbeat, sizeof(heartbeat) );
        uint8_t received[64];
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, received, sizeof(received) ) > 0 )
        {
        }
        rocketnet_platform_sleep( 0.005 );
    }

    rocketnet_platform_socket_destroy( socket );
    rocketnet_term();
    return 0;
}

static void rocketnet_posix_start_child( pid_t * pid, const char * host, int port )
{
    char port_string[16];
    snprintf( port_string, sizeof(port_string), "%d", port );

    const char * argv[] = { "functional", "--posix-hard-killed-child", host, port_string, NULL };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init( &actions );
    posix_spawn_file_actions_addopen( &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0 );

    const int result = posix_spawn( pid, "/proc/self/exe", &actions, NULL, (char * const *) argv, environ );
    posix_spawn_file_actions_destroy( &actions );

#if defined(__APPLE__)
    // /proc does not exist on darwin: spawn through PATH with argv[0] resolved
    // by the caller's working directory is not reliable either, so the suite
    // passes its own executable path in ROCKETNET_FUNCTIONAL_SELF.
    if ( result != 0 )
    {
        const char * self = getenv( "ROCKETNET_FUNCTIONAL_SELF" );
        rocketnet_check( self != NULL );
        char * argv_self[] = { (char*) self, (char*) "--posix-hard-killed-child", (char*) host, port_string, NULL };
        rocketnet_check( posix_spawn( pid, self, NULL, NULL, argv_self, environ ) == 0 );
    }
#else
    rocketnet_check( result == 0 );
#endif
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// the Linux cells. every server receive arm Linux builds runs each case:
// portable (recvfrom), gso (recvmmsg with UDP_GRO in a worker), gso with GSO
// and GRO off (the plain sendmmsg/recvmmsg tier), and io_uring (the multishot
// receive CQE), on IPv4 and IPv6. linux reports an ICMP error to a UDP socket
// only when it is connected or sets IP_RECVERR/IPV6_RECVERR, and no rocketnet
// socket does either, so every cell also proves the receive arms skipped
// nothing: the process-wide weather count stays where it started.

#include "rocketnet_server_gso.h"
#include "rocketnet_server_uring.h"
#include "rocketnet_packet_batch.h"
#include "rocketnet_constants.h"

#define ROCKETNET_POSIX_CELL_BYTES 64
// one drain of a cell holds what one driver call can return: the gso and
// io_uring process batches are ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS
// (sdk/rocketnet_constants.h:63; the io_uring bank, sdk/rocketnet_server_uring_linux.cpp:63).
// a cell binds an ephemeral port, which can be one another case of the suite
// released and still sends to, so a drain can carry strays; the drains count
// only their own peers' ports
#define ROCKETNET_POSIX_CELL_MAX ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS

// posix_client_server_gone starts again on a fresh port at most this many
// times when another process binds its released port first
#define ROCKETNET_POSIX_PORT_TAKEN_LIMIT 8

enum
{
    ROCKETNET_POSIX_CELL_PORTABLE,
    ROCKETNET_POSIX_CELL_GSO,
    ROCKETNET_POSIX_CELL_GSO_PLAIN,
    ROCKETNET_POSIX_CELL_URING,
    ROCKETNET_POSIX_NUM_CELLS
};

static const char * rocketnet_posix_cell_names[ROCKETNET_POSIX_NUM_CELLS] = { "portable", "gso", "gso-plain", "io_uring" };

struct rocketnet_posix_cell_t
{
    int kind;
    bool ipv6;
    int port;
    rocketnet_platform_socket_t * socket;
    struct rocketnet_server_gso_t * gso;
    struct rocketnet_server_uring_t * uring;
};

static const char * rocketnet_posix_cell_host( bool ipv6 )
{
    return ipv6 ? "[::1]" : "127.0.0.1";
}

static int rocketnet_posix_uring_disabled_value()
{
    FILE * file = fopen( "/proc/sys/kernel/io_uring_disabled", "r" );
    if ( !file )
        return -1;
    int value = -1;
    if ( fscanf( file, "%d", &value ) != 1 )
        value = -1;
    fclose( file );
    return value;
}

// the server side of one cell, bound to port (0: any). false when the host
// does not offer the cell's driver (io_uring disabled), said by name.
// 1 opened, 0 not run (no io_uring on this kernel), -1 the transport create
// failed (a fixed port another process holds)
static int rocketnet_posix_cell_try_open( rocketnet_posix_cell_t * cell, int kind, bool ipv6, int port, const char * case_name )
{
    memset( cell, 0, sizeof(*cell) );
    cell->kind = kind;
    cell->ipv6 = ipv6;
    rocketnet_address_t address;
    rocketnet_check( rocketnet_address_parse( &address, rocketnet_posix_cell_host( ipv6 ) ) );
    address.port = (uint16_t) port;
    if ( kind == ROCKETNET_POSIX_CELL_PORTABLE )
    {
        cell->socket = rocketnet_platform_socket_create( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
        if ( !cell->socket )
            return -1;
    }
    else if ( kind == ROCKETNET_POSIX_CELL_GSO || kind == ROCKETNET_POSIX_CELL_GSO_PLAIN )
    {
        const int flags = kind == ROCKETNET_POSIX_CELL_GSO_PLAIN ? ( ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO ) : 0;
        cell->gso = rocketnet_server_gso_create( NULL, &address, 1, flags );
        if ( !cell->gso )
            return -1;
    }
    else
    {
        if ( !rocketnet_server_uring_available() )
        {
            printf( "%s cell=io_uring %s [NOT RUN this kernel offers no io_uring: io_uring_disabled=%d]\n", case_name, ipv6 ? "ipv6" : "ipv4", rocketnet_posix_uring_disabled_value() );
            return 0;
        }
        cell->uring = rocketnet_server_uring_create( NULL, &address, 1 );
        if ( !cell->uring )
            return -1;
    }
    cell->port = address.port;
    return 1;
}

static bool rocketnet_posix_cell_open( rocketnet_posix_cell_t * cell, int kind, bool ipv6, int port, const char * case_name )
{
    const int opened = rocketnet_posix_cell_try_open( cell, kind, ipv6, port, case_name );
    rocketnet_check( opened >= 0 );
    return opened == 1;
}

static void rocketnet_posix_cell_close( rocketnet_posix_cell_t * cell )
{
    if ( cell->socket )
        rocketnet_platform_socket_destroy( cell->socket );
    if ( cell->gso )
        rocketnet_server_gso_destroy( cell->gso );
    if ( cell->uring )
        rocketnet_server_uring_destroy( cell->uring );
    memset( cell, 0, sizeof(*cell) );
}

// stage one datagram from the server side; the drivers send at flush
static void rocketnet_posix_cell_send( rocketnet_posix_cell_t * cell, const rocketnet_address_t * to, int tag )
{
    uint8_t packet[ROCKETNET_POSIX_CELL_BYTES];
    memset( packet, tag, sizeof(packet) );
    uint64_t packet_id = 0;
    if ( cell->socket )
    {
        rocketnet_platform_socket_send_packet( cell->socket, to, packet, sizeof(packet) );
    }
    else if ( cell->gso )
    {
        uint8_t * frame = rocketnet_server_gso_start_packet( cell->gso, to, &packet_id );
        rocketnet_check( frame );
        memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, packet, sizeof(packet) );
        rocketnet_server_gso_finish_packet( cell->gso, packet_id, frame, (int) sizeof(packet) );
    }
    else
    {
        uint8_t * frame = rocketnet_server_uring_start_packet( cell->uring, to, &packet_id );
        rocketnet_check( frame );
        memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, packet, sizeof(packet) );
        rocketnet_server_uring_finish_packet( cell->uring, packet_id, frame, (int) sizeof(packet) );
    }
}

static void rocketnet_posix_cell_flush( rocketnet_posix_cell_t * cell )
{
    if ( cell->gso )
        rocketnet_server_gso_send_packets( cell->gso );
    if ( cell->uring )
        rocketnet_server_uring_send_packets( cell->uring );
}

// one update's drain: what the cell's receive path delivers in one call
static int rocketnet_posix_cell_receive( rocketnet_posix_cell_t * cell, rocketnet_address_t * from )
{
    if ( cell->socket )
    {
        static uint8_t data[ROCKETNET_POSIX_CELL_MAX * ROCKETNET_POSIX_CELL_BYTES];
        static int bytes[ROCKETNET_POSIX_CELL_MAX];
        return rocketnet_platform_socket_receive_packets( cell->socket, from, data, ROCKETNET_POSIX_CELL_BYTES, bytes, ROCKETNET_POSIX_CELL_MAX );
    }
    struct rocketnet_server_socket_process_packets_t * packets;
    if ( cell->gso )
    {
        rocketnet_server_gso_receive_packets( cell->gso );
        packets = rocketnet_server_gso_process_packets( cell->gso );
    }
    else
    {
        rocketnet_server_uring_receive_packets( cell->uring );
        packets = rocketnet_server_uring_process_packets( cell->uring );
    }
    rocketnet_check( packets->num_packets <= ROCKETNET_POSIX_CELL_MAX );
    for ( int i = 0; i < packets->num_packets; i++ )
        from[i] = packets->from[i];
    return packets->num_packets;
}

static void rocketnet_posix_peer_send( rocketnet_platform_socket_t * socket, const char * host, int port, int tag )
{
    rocketnet_address_t to;
    rocketnet_check( rocketnet_address_parse( &to, host ) );
    to.port = (uint16_t) port;
    uint8_t packet[ROCKETNET_POSIX_CELL_BYTES];
    memset( packet, tag, sizeof(packet) );
    rocketnet_platform_socket_send_packet( socket, &to, packet, sizeof(packet) );
}

static int rocketnet_posix_count_ports( const rocketnet_address_t * from, int count, const int * ports, int num_ports )
{
    int matched = 0;
    for ( int i = 0; i < count; i++ )
        for ( int p = 0; p < num_ports; p++ )
            if ( from[i].port == ports[p] )
            {
                matched++;
                break;
            }
    return matched;
}

// drain the cell, counting datagrams from the given ports. the portable arm
// reads synchronously, so after a settle its queue is complete and one drain
// (calls until the first empty one) must deliver all of it: a drain that ends
// early on an errno is a stall, and is not retried. the gso and io_uring
// arms stage on their own, so they drain until the count or the deadline.
static int rocketnet_posix_cell_drain( rocketnet_posix_cell_t * cell, const int * ports, int num_ports, int want, double seconds )
{
    static rocketnet_address_t from[ROCKETNET_POSIX_CELL_MAX];
    int matched = 0;
    if ( cell->socket )
    {
        for ( ;; )
        {
            const int got = rocketnet_posix_cell_receive( cell, from );
            if ( got == 0 )
                break;
            matched += rocketnet_posix_count_ports( from, got, ports, num_ports );
        }
        return matched;
    }
    const double deadline = rocketnet_platform_time() + seconds;
    while ( matched < want && rocketnet_platform_time() < deadline )
    {
        const int got = rocketnet_posix_cell_receive( cell, from );
        if ( got == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        matched += rocketnet_posix_count_ports( from, got, ports, num_ports );
    }
    return matched;
}

static void rocketnet_posix_cell_check_no_weather( const char * case_name, const rocketnet_posix_cell_t * cell, uint64_t before )
{
    const uint64_t skipped = rocketnet_platform_receive_weather_skipped() - before;
    if ( skipped != 0 )
        printf( "%s cell=%s %s: the receive arms skipped %llu ICMP-class errors: a socket is connected or sets IP_RECVERR\n",
                case_name, rocketnet_posix_cell_names[cell->kind], cell->ipv6 ? "ipv6" : "ipv4", (unsigned long long) skipped );
    rocketnet_check( skipped == 0 );
}

// a pending SO_ERROR on the portable server socket, read for the record: zero
// while the socket is unconnected without IP_RECVERR
static int rocketnet_posix_cell_so_error( const rocketnet_posix_cell_t * cell )
{
    if ( !cell->socket )
        return 0;
    int error = 0;
    socklen_t length = sizeof(error);
    if ( getsockopt( cell->socket->handle, SOL_SOCKET, SO_ERROR, &error, &length ) != 0 )
        return -1;
    return error;
}

static void rocketnet_posix_cell_hard_killed_client( int kind, bool ipv6 )
{
    const char * case_name = "posix_hard_killed_client";
    const uint64_t weather_before = rocketnet_platform_receive_weather_skipped();
    rocketnet_posix_cell_t cell;
    if ( !rocketnet_posix_cell_open( &cell, kind, ipv6, 0, case_name ) )
        return;
    const char * host = rocketnet_posix_cell_host( ipv6 );

    rocketnet_platform_socket_t * live[3];
    int live_port[3] = { 0, 0, 0 };
    for ( int i = 0; i < 3; i++ )
    {
        live[i] = rocketnet_posix_bind_any( host, &live_port[i] );
        rocketnet_posix_peer_send( live[i], host, cell.port, 0x10 + i );
    }

    pid_t child = 0;
    rocketnet_posix_start_child( &child, host, cell.port );
    rocketnet_check( child > 0 );

    // the warmup reads the live hellos and the child's heartbeats
    static rocketnet_address_t from[ROCKETNET_POSIX_CELL_MAX];
    rocketnet_address_t dead_address;
    memset( &dead_address, 0, sizeof(dead_address) );
    bool have_dead = false;
    // at least a second, and until the child's first heartbeat: a child
    // started on a loaded host can take longer than a second to send it
    const double warmup = rocketnet_platform_time() + 1.0;
    const double warmup_limit = rocketnet_platform_time() + 10.0;
    while ( rocketnet_platform_time() < warmup || ( !have_dead && rocketnet_platform_time() < warmup_limit ) )
    {
        const int got = rocketnet_posix_cell_receive( &cell, from );
        for ( int i = 0; i < got; i++ )
        {
            if ( from[i].port != live_port[0] && from[i].port != live_port[1] && from[i].port != live_port[2] )
            {
                dead_address = from[i];
                have_dead = true;
            }
        }
        rocketnet_platform_sleep( 0.002 );
    }
    rocketnet_check( have_dead );

    rocketnet_check( kill( child, SIGKILL ) == 0 );
    int status = 0;
    rocketnet_check( waitpid( child, &status, 0 ) == child );
    rocketnet_check( WIFSIGNALED( status ) && WTERMSIG( status ) == SIGKILL );

    // a heartbeat in flight at the kill drains here, before the count starts
    rocketnet_posix_settle();
    rocketnet_posix_cell_drain( &cell, live_port, 3, 0, 0.1 );

    // the server keeps sending a four-packet snapshot a round to the dead
    // peer (the stack answers each with ICMP port unreachable) while the live
    // clients send to it; the server drains every 20 rounds
    int expected = 0;
    int live_received = 0;
    for ( int i = 0; i < 200; i++ )
    {
        for ( int j = 0; j < 4; j++ )
            rocketnet_posix_cell_send( &cell, &dead_address, i );
        rocketnet_posix_cell_flush( &cell );
        for ( int c = 0; c < 3; c++ )
        {
            rocketnet_posix_peer_send( live[c], host, cell.port, (uint8_t) ( i + c ) );
            expected++;
        }
        if ( ( i % 20 ) == 19 )
        {
            rocketnet_posix_settle();
            live_received += rocketnet_posix_cell_drain( &cell, live_port, 3, expected - live_received, 5.0 );
        }
    }
    rocketnet_posix_settle();
    live_received += rocketnet_posix_cell_drain( &cell, live_port, 3, expected - live_received, 5.0 );

    const int so_error = rocketnet_posix_cell_so_error( &cell );
    printf( "%s cell=%s %s live=%d/%d dead_port=%d so_error=%d weather_skipped=%llu\n", case_name, rocketnet_posix_cell_names[kind], ipv6 ? "ipv6" : "ipv4",
            live_received, expected, dead_address.port, so_error, (unsigned long long) ( rocketnet_platform_receive_weather_skipped() - weather_before ) );
    fflush( stdout );
    rocketnet_check( live_received == expected );
    rocketnet_posix_cell_check_no_weather( case_name, &cell, weather_before );

    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_destroy( live[i] );
    rocketnet_posix_cell_close( &cell );
}

static void rocketnet_posix_cell_send_to_closed_port( int kind, bool ipv6 )
{
    const char * case_name = "posix_send_to_closed_port";
    const uint64_t weather_before = rocketnet_platform_receive_weather_skipped();
    rocketnet_posix_cell_t cell;
    if ( !rocketnet_posix_cell_open( &cell, kind, ipv6, 0, case_name ) )
        return;
    const char * host = rocketnet_posix_cell_host( ipv6 );

    int live_port = 0;
    rocketnet_platform_socket_t * live = rocketnet_posix_bind_any( host, &live_port );

    // a closed port: bound, read and released, then never bound again here
    int closed_port = 0;
    rocketnet_platform_socket_destroy( rocketnet_posix_bind_any( host, &closed_port ) );
    rocketnet_address_t closed;
    rocketnet_check( rocketnet_address_parse( &closed, host ) );
    closed.port = (uint16_t) closed_port;

    int expected = 0;
    int received = 0;
    for ( int i = 0; i < 1000; i++ )
    {
        rocketnet_posix_cell_send( &cell, &closed, i );
        rocketnet_posix_cell_flush( &cell );
        rocketnet_posix_peer_send( live, host, cell.port, (uint8_t) i );
        expected++;
        if ( ( i % 50 ) == 49 )
        {
            rocketnet_posix_settle();
            received += rocketnet_posix_cell_drain( &cell, &live_port, 1, expected - received, 5.0 );
        }
    }
    rocketnet_posix_settle();
    received += rocketnet_posix_cell_drain( &cell, &live_port, 1, expected - received, 5.0 );

    printf( "%s cell=%s %s live=%d/%d closed_port=%d so_error=%d\n", case_name, rocketnet_posix_cell_names[kind], ipv6 ? "ipv6" : "ipv4",
            received, expected, closed_port, rocketnet_posix_cell_so_error( &cell ) );
    fflush( stdout );
    rocketnet_check( received == expected );
    rocketnet_posix_cell_check_no_weather( case_name, &cell, weather_before );

    rocketnet_platform_socket_destroy( live );
    rocketnet_posix_cell_close( &cell );
}

// the client sends to a closed server port, then the server binds it: the
// client's later datagrams arrive, and the server's replies reach the client
// through the client's recvmmsg pump in one drain.
static void rocketnet_posix_cell_client_server_gone( int kind, bool ipv6 )
{
    const char * case_name = "posix_client_server_gone";
    const uint64_t weather_before = rocketnet_platform_receive_weather_skipped();
    const char * host = rocketnet_posix_cell_host( ipv6 );

    int client_port = 0;
    rocketnet_platform_socket_t * client = rocketnet_posix_bind_any( host, &client_port );

    // the server's port is released while the client sends into it, then
    // bound again. in a parallel suite another process can bind the released
    // port first; the case then takes a fresh port and starts again, and
    // prints how many times it did
    int server_port = 0;
    int port_taken = 0;
    rocketnet_posix_cell_t cell;
    for ( ;; )
    {
        rocketnet_platform_socket_destroy( rocketnet_posix_bind_any( host, &server_port ) );

        for ( int i = 0; i < 100; i++ )
            rocketnet_posix_peer_send( client, host, server_port, (uint8_t) i );
        rocketnet_posix_settle();

        const int opened = rocketnet_posix_cell_try_open( &cell, kind, ipv6, server_port, case_name );
        if ( opened == 1 )
            break;
        if ( opened == 0 )
        {
            rocketnet_platform_socket_destroy( client );
            return;
        }
        port_taken++;
        rocketnet_check( port_taken < ROCKETNET_POSIX_PORT_TAKEN_LIMIT );
    }

    for ( int i = 0; i < 4; i++ )
        rocketnet_posix_peer_send( client, host, server_port, (uint8_t) ( 0x40 + i ) );
    rocketnet_posix_settle();
    const int received = rocketnet_posix_cell_drain( &cell, &client_port, 1, 4, 5.0 );

    rocketnet_address_t to;
    rocketnet_check( rocketnet_address_parse( &to, host ) );
    to.port = (uint16_t) client_port;
    for ( int i = 0; i < 4; i++ )
        rocketnet_posix_cell_send( &cell, &to, 0x50 + i );
    rocketnet_posix_cell_flush( &cell );
    rocketnet_posix_settle();

    static rocketnet_address_t from[8];
    static int packet_bytes[8];
    static uint8_t packet_data[8 * ROCKETNET_POSIX_CELL_BYTES];
    int replies = 0;
    for ( ;; )
    {
        const int got = rocketnet_platform_socket_receive_packets_mmsg( client, from, packet_data, ROCKETNET_POSIX_CELL_BYTES, packet_bytes, 8 );
        if ( got == 0 )
            break;
        replies += rocketnet_posix_count_ports( from, got, &server_port, 1 );
    }

    printf( "%s cell=%s %s received=%d/4 replies=%d/4 port_taken=%d\n", case_name, rocketnet_posix_cell_names[kind], ipv6 ? "ipv6" : "ipv4", received, replies, port_taken );
    fflush( stdout );
    rocketnet_check( received == 4 );
    rocketnet_check( replies == 4 );
    rocketnet_posix_cell_check_no_weather( case_name, &cell, weather_before );

    rocketnet_posix_cell_close( &cell );
    rocketnet_platform_socket_destroy( client );
}

static void rocketnet_posix_linux_cells( void (*cell_case)( int kind, bool ipv6 ) )
{
    for ( int kind = 0; kind < ROCKETNET_POSIX_NUM_CELLS; kind++ )
        for ( int family = 0; family < 2; family++ )
            cell_case( kind, family == 1 );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// one server socket, three live clients, one child client killed mid-stream.
// drain the platform socket until the first empty receive, counting all
// datagrams and those from the given ports. the cases read while they send,
// so a receive buffer at the kernel default (212992 bytes) never overflows.
static int rocketnet_posix_drain_ports( rocketnet_platform_socket_t * socket, const int * ports, int num_ports, int * received )
{
    static rocketnet_address_t from_many[512];
    static int packet_bytes[512];
    static uint8_t packet_data[512 * 64];
    int matched = 0;
    for ( ;; )
    {
        const int got = rocketnet_platform_socket_receive_packets( socket, from_many, packet_data, 64, packet_bytes, 512 );
        if ( got == 0 )
            break;
        *received += got;
        for ( int i = 0; i < got; i++ )
        {
            for ( int p = 0; p < num_ports; p++ )
            {
                if ( from_many[i].port == ports[p] )
                {
                    matched++;
                    break;
                }
            }
        }
    }
    return matched;
}

// after the kill the server keeps sending to the dead address while the live
// clients exchange traffic; every live datagram must still arrive and the
// receive arm must still drain behind the ICMP-class error.
void rocketnet_test_posix_hard_killed_client()
{
    int server_port = 0;
    rocketnet_platform_socket_t * server = rocketnet_posix_bind_any( "127.0.0.1", &server_port );
    rocketnet_check( server );

    const char * pump_at_create = rocketnet_platform_socket_pump( server );

    rocketnet_platform_socket_t * live[3];
    int live_port[3] = { 0, 0, 0 };
    for ( int i = 0; i < 3; i++ )
    {
        live[i] = rocketnet_posix_bind_any( "127.0.0.1", &live_port[i] );
        rocketnet_posix_send_to( live[i], "127.0.0.1", server_port, 0x10 + i );
    }
    rocketnet_posix_settle();

    pid_t child = 0;
    rocketnet_posix_start_child( &child, "127.0.0.1", server_port );
    rocketnet_check( child > 0 );
    rocketnet_platform_sleep( 0.2 );

    // the server reads the child's heartbeats (and the live hello packets)
    uint8_t data[64];
    rocketnet_address_t from;
    rocketnet_address_t dead_address;
    memset( &dead_address, 0, sizeof(dead_address) );
    bool have_dead = false;

    // at least a second, and until the child's first heartbeat: a child
    // started on a loaded host can take longer than a second to send it
    const double warmup = rocketnet_platform_time() + 1.0;
    const double warmup_limit = rocketnet_platform_time() + 10.0;
    while ( rocketnet_platform_time() < warmup || ( !have_dead && rocketnet_platform_time() < warmup_limit ) )
    {
        while ( rocketnet_platform_socket_receive_packet( server, &from, data, sizeof(data) ) > 0 )
        {
            if ( from.port != live_port[0] && from.port != live_port[1] && from.port != live_port[2] )
            {
                dead_address = from;
                have_dead = true;
            }
        }
        rocketnet_platform_sleep( 0.002 );
    }
    rocketnet_check( have_dead );

    // kill without a disconnect packet
    rocketnet_check( kill( child, SIGKILL ) == 0 );
    int status = 0;
    rocketnet_check( waitpid( child, &status, 0 ) == child );
    rocketnet_check( WIFSIGNALED( status ) );
    rocketnet_check( WTERMSIG( status ) == SIGKILL );

    // the server keeps sending to the dead peer (the stack answers with ICMP)
    // while the live clients exchange traffic
    int expected = 0;
    int received = 0;
    int live_received = 0;
    for ( int i = 0; i < 200; i++ )
    {
        uint8_t dead_packet[32];
        memset( dead_packet, i, sizeof(dead_packet) );
        rocketnet_platform_socket_send_packet( server, &dead_address, dead_packet, sizeof(dead_packet) );

        for ( int c = 0; c < 3; c++ )
        {
            rocketnet_posix_send_to( live[c], "127.0.0.1", server_port, (uint8_t) ( i + c ) );
            expected++;
        }
        if ( ( i % 20 ) == 19 )
        {
            rocketnet_posix_settle();
            live_received += rocketnet_posix_drain_ports( server, live_port, 3, &received );
        }
    }
    rocketnet_posix_settle();

    static rocketnet_address_t from_many[512];
    static int packet_bytes[512];
    static uint8_t packet_data[512 * 64];
    const double drain_deadline = rocketnet_platform_time() + 5.0;
    while ( live_received < expected && rocketnet_platform_time() < drain_deadline )
    {
        const int got = rocketnet_platform_socket_receive_packets( server, from_many, packet_data, 64, packet_bytes, 512 );
        if ( got == 0 )
        {
            rocketnet_posix_settle();
            continue;
        }
        // a heartbeat the child sent just before SIGKILL can still arrive
        // after the warmup drain; it is not a live-client datagram and says
        // nothing about the drain, so count only the live ports.
        for ( int i = 0; i < got; i++ )
        {
            received++;
            for ( int c = 0; c < 3; c++ )
            {
                if ( from_many[i].port == live_port[c] )
                {
                    live_received++;
                    break;
                }
            }
        }
    }

    printf( "posix_hard_killed_client received=%d live=%d expected=%d\n", received, live_received, expected );
    rocketnet_check( live_received == expected );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( server ), pump_at_create ) == 0 );

    printf( "posix_hard_killed_client pump=%s live=%d/%d dead_port=%d\n", pump_at_create, live_received, expected, dead_address.port );

    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_destroy( live[i] );
    rocketnet_platform_socket_destroy( server );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    rocketnet_posix_linux_cells( rocketnet_posix_cell_hard_killed_client );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
}

// the server sends 1000 datagrams to a port no socket holds while live clients
// exchange traffic: no stall, no early drain.
void rocketnet_test_posix_send_to_closed_port()
{
    int server_port = 0;
    rocketnet_platform_socket_t * server = rocketnet_posix_bind_any( "127.0.0.1", &server_port );
    rocketnet_check( server );

    const char * pump_at_create = rocketnet_platform_socket_pump( server );

    int live_port = 0;
    rocketnet_platform_socket_t * live = rocketnet_posix_bind_any( "127.0.0.1", &live_port );

    const int closed_port = rocketnet_posix_free_port();
    rocketnet_address_t closed;
    rocketnet_address_parse( &closed, "127.0.0.1" );
    closed.port = closed_port;

    int expected = 0;
    int received = 0;
    int ignored = 0;
    for ( int i = 0; i < 1000; i++ )
    {
        uint8_t dead_packet[32];
        memset( dead_packet, i, sizeof(dead_packet) );
        rocketnet_platform_socket_send_packet( server, &closed, dead_packet, sizeof(dead_packet) );

        rocketnet_posix_send_to( live, "127.0.0.1", server_port, (uint8_t) i );
        expected++;

        if ( ( i % 50 ) == 49 )
        {
            rocketnet_posix_settle();
            received += rocketnet_posix_drain_ports( server, &live_port, 1, &ignored );
        }
    }
    rocketnet_posix_settle();

    static rocketnet_address_t from_many[1100];
    static int packet_bytes[1100];
    static uint8_t packet_data[1100 * 64];
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( received < expected && rocketnet_platform_time() < deadline )
    {
        const int got = rocketnet_platform_socket_receive_packets( server, from_many, packet_data, 64, packet_bytes, 1100 );
        if ( got == 0 )
        {
            rocketnet_posix_settle();
            continue;
        }
        received += got;
    }

    printf( "posix_send_to_closed_port received=%d expected=%d\n", received, expected );
    rocketnet_check( received == expected );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( server ), pump_at_create ) == 0 );
    printf( "posix_send_to_closed_port pump=%s live=%d/%d closed_port=%d\n", pump_at_create, received, expected, closed_port );

    rocketnet_platform_socket_destroy( live );
    rocketnet_platform_socket_destroy( server );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    rocketnet_posix_linux_cells( rocketnet_posix_cell_send_to_closed_port );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
}

// a client sends to a server port that is closed, then a server binds that
// port; the client's later datagram is delivered. the send-side weather never
// tears the client down.
void rocketnet_test_posix_client_server_gone()
{
    int client_port = 0;
    rocketnet_platform_socket_t * client = rocketnet_posix_bind_any( "127.0.0.1", &client_port );
    rocketnet_check( client );

    const int server_port = rocketnet_posix_free_port();

    for ( int i = 0; i < 100; i++ )
        rocketnet_posix_send_to( client, "127.0.0.1", server_port, (uint8_t) i );
    rocketnet_posix_settle();

    rocketnet_platform_socket_t * server = rocketnet_posix_bind( "127.0.0.1", server_port );
    rocketnet_check( server );

    for ( int i = 0; i < 4; i++ )
        rocketnet_posix_send_to( client, "127.0.0.1", server_port, (uint8_t) ( 0x40 + i ) );
    rocketnet_posix_settle();

    static rocketnet_address_t from_many[8];
    static int packet_bytes[8];
    static uint8_t packet_data[8 * 64];
    int received = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( received < 4 && rocketnet_platform_time() < deadline )
    {
        const int got = rocketnet_platform_socket_receive_packets( server, from_many, packet_data, 64, packet_bytes, 8 );
        if ( got == 0 )
        {
            rocketnet_posix_settle();
            continue;
        }
        received += got;
    }
    rocketnet_check( received == 4 );
    printf( "posix_client_server_gone received=%d\n", received );

    rocketnet_platform_socket_destroy( server );
    rocketnet_platform_socket_destroy( client );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    rocketnet_posix_linux_cells( rocketnet_posix_cell_client_server_gone );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
// the injected-errno seam: every ICMP-class and buffer errno must be skipped,
// with the datagram behind it still delivered in the same drain, and the msgx
// pump must stay up. an unknown errno still pauses msgx.
static void rocketnet_posix_seam_one( const char * pump, int error, int expect_skip )
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "127.0.0.1" );
    address.port = 0;
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024, pump );
    rocketnet_check( socket );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );

    rocketnet_posix_send_to( socket, "127.0.0.1", address.port, 0x21 );
    rocketnet_posix_send_to( socket, "127.0.0.1", address.port, 0x22 );
    rocketnet_check( rocketnet_posix_wait_readable( socket, 2.0 ) );
    rocketnet_platform_sleep( 0.05 );

    if ( strcmp( pump, "msgx" ) == 0 )
        rocketnet_platform_set_recvmsg_x_error_for_testing( error );
    else
        rocketnet_platform_set_recvmsg_error_for_testing( error );

    static rocketnet_address_t from[4];
    static int packet_bytes[4];
    static uint8_t packet_data[4 * 64];
    const int got = rocketnet_platform_socket_receive_packets( socket, from, packet_data, 64, packet_bytes, 4 );

    if ( expect_skip )
    {
        rocketnet_check( got == 2 );
        rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );
    }
    else
    {
        rocketnet_check( got == 0 );
    }

    rocketnet_platform_socket_destroy( socket );
}

// an errno outside the receive-weather set is a real refusal: msgx pauses
// (the pump reports portable) and the same call still finishes per packet, so
// the datagrams already queued are delivered rather than lost.
static void rocketnet_posix_seam_unknown()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "127.0.0.1" );
    address.port = 0;
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024, "msgx" );
    rocketnet_check( socket );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), "msgx" ) == 0 );

    rocketnet_posix_send_to( socket, "127.0.0.1", address.port, 0x31 );
    rocketnet_posix_send_to( socket, "127.0.0.1", address.port, 0x32 );
    rocketnet_check( rocketnet_posix_wait_readable( socket, 2.0 ) );
    rocketnet_platform_sleep( 0.05 );

    rocketnet_platform_set_recvmsg_x_error_for_testing( EPROTO );

    static rocketnet_address_t from[4];
    static int packet_bytes[4];
    static uint8_t packet_data[4 * 64];
    const int got = rocketnet_platform_socket_receive_packets( socket, from, packet_data, 64, packet_bytes, 4 );

    rocketnet_check( got == 2 );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), "portable" ) == 0 );
    printf( "seam pump=msgx errno=EPROTO paused\n" );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_posix_hard_killed_receive_errno_seam()
{
    const int errors[] = { ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN, ENETDOWN, ENOBUFS, EINTR };
    const int count = (int) ( sizeof(errors) / sizeof(errors[0]) );
    const char * pumps[] = { "portable", "msgx" };
    for ( int p = 0; p < 2; p++ )
    {
        for ( int i = 0; i < count; i++ )
        {
            rocketnet_posix_seam_one( pumps[p], errors[i], 1 );
            printf( "seam pump=%s errno=%d skipped\n", pumps[p], errors[i] );
        }
        // EAGAIN ends the drain: it is not weather to skip
        rocketnet_posix_seam_one( pumps[p], EAGAIN, 0 );
    }
    // only msgx has a pump to pause
    rocketnet_posix_seam_unknown();
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
// the injected-errno seam on Linux: recvfrom (the portable arm), recvmmsg (the
// client pump and the gso worker, both tiers) and the io_uring receive CQE
// res. every ICMP-class and buffer errno, and EINTR, is skipped with the
// datagrams behind it still delivered; the weather errnos are counted.

static const int rocketnet_posix_seam_errors[] = { ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN, ENETDOWN, ENOBUFS, EINTR };
#define ROCKETNET_POSIX_SEAM_ERRORS ( (int) ( sizeof(rocketnet_posix_seam_errors) / sizeof(rocketnet_posix_seam_errors[0]) ) )

// the socket arms: the datagrams are queued before the error, and one drain
// call must deliver both behind it
static void rocketnet_posix_linux_seam_socket( bool mmsg, int error, bool expect_skip )
{
    int port = 0;
    rocketnet_platform_socket_t * socket = rocketnet_posix_bind_any( "127.0.0.1", &port );
    rocketnet_posix_peer_send( socket, "127.0.0.1", port, 0x21 );
    rocketnet_posix_peer_send( socket, "127.0.0.1", port, 0x22 );
    rocketnet_check( rocketnet_posix_wait_readable( socket, 2.0 ) );
    rocketnet_posix_settle();

    const uint64_t before = rocketnet_platform_receive_weather_skipped();
    if ( mmsg )
        rocketnet_platform_set_recvmmsg_error_for_testing( error );
    else
        rocketnet_platform_set_recvmsg_error_for_testing( error );

    static rocketnet_address_t from[4];
    static int packet_bytes[4];
    static uint8_t packet_data[4 * ROCKETNET_POSIX_CELL_BYTES];
    const int got = mmsg ? rocketnet_platform_socket_receive_packets_mmsg( socket, from, packet_data, ROCKETNET_POSIX_CELL_BYTES, packet_bytes, 4 )
                         : rocketnet_platform_socket_receive_packets( socket, from, packet_data, ROCKETNET_POSIX_CELL_BYTES, packet_bytes, 4 );
    const uint64_t counted = rocketnet_platform_receive_weather_skipped() - before;
    printf( "seam arm=%s errno=%d got=%d counted=%llu\n", mmsg ? "recvmmsg" : "recvfrom", error, got, (unsigned long long) counted );
    fflush( stdout );

    if ( expect_skip )
    {
        rocketnet_check( got == 2 );
        rocketnet_check( counted == ( error == EINTR ? 0u : 1u ) );
    }
    else
    {
        // EAGAIN is the end of a drain, not weather: this call ends, and the
        // next one reads what is queued
        rocketnet_check( got == 0 );
        rocketnet_check( counted == 0 );
        const int again = mmsg ? rocketnet_platform_socket_receive_packets_mmsg( socket, from, packet_data, ROCKETNET_POSIX_CELL_BYTES, packet_bytes, 4 )
                               : rocketnet_platform_socket_receive_packets( socket, from, packet_data, ROCKETNET_POSIX_CELL_BYTES, packet_bytes, 4 );
        rocketnet_check( again == 2 );
    }

    rocketnet_platform_socket_destroy( socket );
}

// the gso worker: the error reaches the worker's next recvmmsg; the worker
// must count it and keep receiving
static void rocketnet_posix_linux_seam_gso( int kind, int error )
{
    rocketnet_posix_cell_t cell;
    rocketnet_check( rocketnet_posix_cell_open( &cell, kind, false, 0, "posix_hard_killed_receive_errno_seam" ) );
    int peer_port = 0;
    rocketnet_platform_socket_t * peer = rocketnet_posix_bind_any( "127.0.0.1", &peer_port );

    const uint64_t before = rocketnet_platform_receive_weather_skipped();
    rocketnet_platform_set_recvmmsg_error_for_testing( error );
    const double deadline = rocketnet_platform_time() + 2.0;
    if ( error == EINTR )
        rocketnet_platform_sleep( 0.05 );
    else
        while ( rocketnet_platform_receive_weather_skipped() == before && rocketnet_platform_time() < deadline )
            rocketnet_platform_sleep( 0.001 );
    rocketnet_platform_set_recvmmsg_error_for_testing( 0 );

    rocketnet_posix_peer_send( peer, "127.0.0.1", cell.port, 0x31 );
    rocketnet_posix_peer_send( peer, "127.0.0.1", cell.port, 0x32 );
    const int got = rocketnet_posix_cell_drain( &cell, &peer_port, 1, 2, 2.0 );
    const uint64_t counted = rocketnet_platform_receive_weather_skipped() - before;
    printf( "seam arm=%s errno=%d got=%d counted=%llu\n", rocketnet_posix_cell_names[kind], error, got, (unsigned long long) counted );
    fflush( stdout );
    rocketnet_check( got == 2 );
    rocketnet_check( counted == ( error == EINTR ? 0u : 1u ) );

    rocketnet_platform_socket_destroy( peer );
    rocketnet_posix_cell_close( &cell );
}

// the io_uring receive CQE: the multishot ends with res -error while two
// datagrams queue behind it; the one receive_packets that reaps the error must
// re-arm and deliver both in the same update
static void rocketnet_posix_linux_seam_uring( int error )
{
    rocketnet_posix_cell_t cell;
    if ( !rocketnet_posix_cell_open( &cell, ROCKETNET_POSIX_CELL_URING, false, 0, "posix_hard_killed_receive_errno_seam" ) )
        return;
    int peer_port = 0;
    rocketnet_platform_socket_t * peer = rocketnet_posix_bind_any( "127.0.0.1", &peer_port );
    rocketnet_posix_settle();
    static rocketnet_address_t from[ROCKETNET_POSIX_CELL_MAX];
    rocketnet_posix_cell_receive( &cell, from );

    uint64_t counters_before[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( cell.uring, counters_before );
    const uint64_t before = rocketnet_platform_receive_weather_skipped();
    rocketnet_server_uring_inject_recv_error_for_testing( cell.uring, error );

    rocketnet_posix_peer_send( peer, "127.0.0.1", cell.port, 0x41 );
    rocketnet_posix_peer_send( peer, "127.0.0.1", cell.port, 0x42 );
    rocketnet_posix_settle();

    const int got = rocketnet_posix_count_ports( from, rocketnet_posix_cell_receive( &cell, from ), &peer_port, 1 );
    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( cell.uring, counters );
    const uint64_t counted = rocketnet_platform_receive_weather_skipped() - before;
    const uint64_t rearms = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS] - counters_before[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS];
    printf( "seam arm=io_uring errno=%d got=%d counted=%llu rearms=%llu\n", error, got, (unsigned long long) counted, (unsigned long long) rearms );
    fflush( stdout );
    rocketnet_check( got == 2 );
    rocketnet_check( rearms == 1 );
    // ENOBUFS on a multishot is the empty buffer ring, counted by the driver
    rocketnet_check( counted == ( ( error == EINTR || error == ENOBUFS ) ? 0u : 1u ) );

    rocketnet_platform_socket_destroy( peer );
    rocketnet_posix_cell_close( &cell );
}

void rocketnet_test_posix_hard_killed_receive_errno_seam()
{
    for ( int i = 0; i < ROCKETNET_POSIX_SEAM_ERRORS; i++ )
    {
        const int error = rocketnet_posix_seam_errors[i];
        rocketnet_posix_linux_seam_socket( false, error, true );
        rocketnet_posix_linux_seam_socket( true, error, true );
        rocketnet_posix_linux_seam_gso( ROCKETNET_POSIX_CELL_GSO, error );
        rocketnet_posix_linux_seam_gso( ROCKETNET_POSIX_CELL_GSO_PLAIN, error );
        rocketnet_posix_linux_seam_uring( error );
    }
    rocketnet_posix_linux_seam_socket( false, EAGAIN, false );
    rocketnet_posix_linux_seam_socket( true, EAGAIN, false );
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#endif // #if defined(__APPLE__) || defined(__linux__)
