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
    if ( !rocketnet_init( NULL ) )
        return 1;

    const int free_port = rocketnet_posix_free_port();
    rocketnet_platform_socket_t * socket = rocketnet_posix_bind( "127.0.0.1", free_port );
    if ( !socket )
        return 1;

    printf( "posix-hard-killed-child %d\n", free_port );
    fflush( stdout );

    const double deadline = rocketnet_platform_time() + 30.0;
    int tag = 0;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_posix_send_to( socket, host, port, tag++ );
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

// one server socket, three live clients, one child client killed mid-stream.
// after the kill the server keeps sending to the dead address while the live
// clients exchange traffic; every live datagram must still arrive and the
// receive arm must still drain behind the ICMP-class error.
void rocketnet_test_posix_hard_killed_client()
{
    const int server_port = rocketnet_posix_free_port();
    rocketnet_platform_socket_t * server = rocketnet_posix_bind( "127.0.0.1", server_port );
    rocketnet_check( server );

    const char * pump_at_create = rocketnet_platform_socket_pump( server );

    rocketnet_platform_socket_t * live[3];
    const int live_port[3] = { rocketnet_posix_free_port(), rocketnet_posix_free_port(), rocketnet_posix_free_port() };
    for ( int i = 0; i < 3; i++ )
    {
        live[i] = rocketnet_posix_bind( "127.0.0.1", live_port[i] );
        rocketnet_check( live[i] );
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

    const double warmup = rocketnet_platform_time() + 1.0;
    while ( rocketnet_platform_time() < warmup )
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
        if ( ( i % 20 ) == 0 )
            rocketnet_posix_settle();
    }
    rocketnet_posix_settle();

    int received = 0;
    static rocketnet_address_t from_many[512];
    static int packet_bytes[512];
    static uint8_t packet_data[512 * 64];
    const double drain_deadline = rocketnet_platform_time() + 5.0;
    while ( received < expected && rocketnet_platform_time() < drain_deadline )
    {
        const int got = rocketnet_platform_socket_receive_packets( server, from_many, packet_data, 64, packet_bytes, 512 );
        if ( got == 0 )
        {
            rocketnet_posix_settle();
            continue;
        }
        received += got;
    }

    rocketnet_check( received == expected );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( server ), pump_at_create ) == 0 );

    printf( "posix_hard_killed_client pump=%s live=%d/%d dead_port=%d\n", pump_at_create, received, expected, dead_address.port );

    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_destroy( live[i] );
    rocketnet_platform_socket_destroy( server );
}

// the server sends 1000 datagrams to a port no socket holds while live clients
// exchange traffic: no stall, no early drain.
void rocketnet_test_posix_send_to_closed_port()
{
    const int server_port = rocketnet_posix_free_port();
    rocketnet_platform_socket_t * server = rocketnet_posix_bind( "127.0.0.1", server_port );
    rocketnet_check( server );

    const char * pump_at_create = rocketnet_platform_socket_pump( server );

    const int live_port = rocketnet_posix_free_port();
    rocketnet_platform_socket_t * live = rocketnet_posix_bind( "127.0.0.1", live_port );
    rocketnet_check( live );

    const int closed_port = rocketnet_posix_free_port();
    rocketnet_address_t closed;
    rocketnet_address_parse( &closed, "127.0.0.1" );
    closed.port = closed_port;

    int expected = 0;
    for ( int i = 0; i < 1000; i++ )
    {
        uint8_t dead_packet[32];
        memset( dead_packet, i, sizeof(dead_packet) );
        rocketnet_platform_socket_send_packet( server, &closed, dead_packet, sizeof(dead_packet) );

        rocketnet_posix_send_to( live, "127.0.0.1", server_port, (uint8_t) i );
        expected++;

        if ( ( i % 50 ) == 0 )
            rocketnet_posix_settle();
    }
    rocketnet_posix_settle();

    static rocketnet_address_t from_many[1100];
    static int packet_bytes[1100];
    static uint8_t packet_data[1100 * 64];
    int received = 0;
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

    rocketnet_check( received == expected );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( server ), pump_at_create ) == 0 );
    printf( "posix_send_to_closed_port pump=%s live=%d/%d closed_port=%d\n", pump_at_create, received, expected, closed_port );

    rocketnet_platform_socket_destroy( live );
    rocketnet_platform_socket_destroy( server );
}

// a client sends to a server port that is closed, then a server binds that
// port; the client's later datagram is delivered. the send-side weather never
// tears the client down.
void rocketnet_test_posix_client_server_gone()
{
    rocketnet_platform_socket_t * client = rocketnet_posix_bind( "127.0.0.1", rocketnet_posix_free_port() );
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
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
// the injected-errno seam: every ICMP-class and buffer errno must be skipped,
// with the datagram behind it still delivered in the same drain, and the msgx
// pump must stay up. an unknown errno still pauses msgx.
static void rocketnet_posix_seam_one( const char * pump, int error, int expect_skip )
{
    const int port = rocketnet_posix_free_port();
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "127.0.0.1" );
    address.port = port;
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
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#endif // #if defined(__APPLE__) || defined(__linux__)
