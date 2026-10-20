/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    POSIX (macOS and Linux) hard-killed-peer and ICMP-class receive cases. A
    UDP send to a vanished peer comes back as an ICMP port unreachable; on a
    BSD stack a connected socket reports it as ECONNREFUSED to the next
    receive. rocketnet's sockets are unconnected, so these errnos do not
    surface by themselves, but every receive arm must treat the ICMP class
    as skip-and-continue: never fatal, never the end of a drain while
    datagrams remain, never a msgx pause. These cases prove it directly with
    the injected-errno seam and with real SIGKILLed child clients.
*/

#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char **environ;

static void rocketnet_posix_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_posix_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
    }                                                                                                           \
} while(0)

#define ROCKETNET_COUNTERS_CHECK rocketnet_check
#include "../test/counters_expect.h"

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// the ICMP-class and buffer errnos the audit names, plus the transient pair
// the receive arms already special-case. the seam sweep proves each is
// skipped (never a pause, never the end of a drain).

static const int posix_receive_weather_errnos[] =
{
    ECONNREFUSED,
    EHOSTUNREACH,
    ENETUNREACH,
    EHOSTDOWN,
    ENETDOWN,
    ENOBUFS,
    EAGAIN,
    EINTR,
};

#define POSIX_NUM_WEATHER ( (int)( sizeof( posix_receive_weather_errnos ) / sizeof( posix_receive_weather_errnos[0] ) ) )

static const char * posix_expected_pump()
{
    const char * pump = rocketnet_platform_getenv( "ROCKETNET_PUMP" );
    if ( pump && strcmp( pump, "portable" ) == 0 )
        return "portable";
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    return "msgx";
#else
    return "portable";
#endif
}

// ------------------------------------------------------------------
// the injected-errno seam cases: prove every receive-weather errno is
// skipped and the drain continues, on both the msgx and the portable arm,
// and that a genuinely unknown errno still pauses the msgx pump.

#define SEAM_N 24
#define SEAM_STRIDE 64

static void posix_seam_queue_to_self( struct rocketnet_platform_socket_t * sock, const rocketnet_address_t * local )
{
    struct rocketnet_platform_send_t sends[SEAM_N];
    static uint8_t data[SEAM_N][32];
    for ( int i = 0; i < SEAM_N; i++ )
    {
        for ( int j = 0; j < 32; j++ )
            data[i][j] = (uint8_t) ( i + j );
        sends[i].to = local;
        sends[i].packet_data = data[i];
        sends[i].packet_bytes = 32;
    }
    rocketnet_platform_socket_send_packets( sock, sends, SEAM_N );
}

static void posix_seam_drain( struct rocketnet_platform_socket_t * sock, int expect_count )
{
    static rocketnet_address_t from[SEAM_N];
    static int bytes[SEAM_N];
    static uint8_t buf[SEAM_N * SEAM_STRIDE];

    int got = 0;
    double t0 = rocketnet_platform_time();
    while ( got < expect_count && rocketnet_platform_time() - t0 < 5.0 )
    {
        int r = rocketnet_platform_socket_receive_packets( sock, from + got, buf + (size_t) got * SEAM_STRIDE, SEAM_STRIDE, bytes + got, expect_count - got );
        if ( r < 0 )
            rocketnet_check( 0 );
        for ( int i = 0; i < r; i++ )
        {
            rocketnet_check( bytes[got + i] == 32 );
            const uint8_t * p = buf + (size_t) ( got + i ) * SEAM_STRIDE;
            const int index = p[0] & 0x1F;
            rocketnet_check( index < 32 );
            for ( int j = 0; j < 32; j++ )
                rocketnet_check( p[j] == (uint8_t) ( ( index + j ) & 0xFF ) );
        }
        got += r;
    }
    rocketnet_check( got == expect_count );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
void rocketnet_test_seam_receive_weather_skips_msgx()
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_address_parse( &local_address, "127.0.0.1" );

    struct rocketnet_platform_socket_t * sock = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( sock );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( sock ), "msgx" ) == 0 );
    local_address.port = bind_address.port;

    for ( int e = 0; e < POSIX_NUM_WEATHER; e++ )
    {
        int err = posix_receive_weather_errnos[e];
        posix_seam_queue_to_self( sock, &local_address );
        rocketnet_platform_test_inject_recv_errno( err );
        posix_seam_drain( sock, SEAM_N );
        rocketnet_check( strcmp( rocketnet_platform_socket_pump( sock ), "msgx" ) == 0 );
    }

    // a genuinely unknown errno still pauses the msgx pump
    rocketnet_platform_test_inject_recv_errno( EINVAL );
    static rocketnet_address_t from[SEAM_N];
    static int bytes[SEAM_N];
    static uint8_t buf[SEAM_N * SEAM_STRIDE];
    rocketnet_platform_socket_receive_packets( sock, from, buf, SEAM_STRIDE, bytes, SEAM_N );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( sock ), "portable" ) == 0 );

    rocketnet_platform_socket_destroy( sock );
}
#endif // ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_seam_receive_weather_skips_portable()
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_address_parse( &local_address, "127.0.0.1" );

    struct rocketnet_platform_socket_t * sock = rocketnet_platform_socket_create_with_pump( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024, "portable" );
    rocketnet_check( sock );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( sock ), "portable" ) == 0 );
    local_address.port = bind_address.port;

    for ( int e = 0; e < POSIX_NUM_WEATHER; e++ )
    {
        int err = posix_receive_weather_errnos[e];
        posix_seam_queue_to_self( sock, &local_address );
        rocketnet_platform_test_inject_recv_errno( err );
        posix_seam_drain( sock, SEAM_N );
        rocketnet_check( strcmp( rocketnet_platform_socket_pump( sock ), "portable" ) == 0 );
    }

    rocketnet_platform_socket_destroy( sock );
}

// ------------------------------------------------------------------
// the child client mode: a functional case that connects as one client to a
// server named by ROCKETNET_CHILD_SERVER and streams until the parent kills
// it with SIGKILL. run only as a spawned child; with no env it passes.

void rocketnet_test_posix_hard_killed_child_client()
{
    const char * server_env = rocketnet_platform_getenv( "ROCKETNET_CHILD_SERVER" );
    if ( !server_env )
        return;

    const char * connid_env = rocketnet_platform_getenv( "ROCKETNET_CHILD_CONNID" );
    uint64_t connid = connid_env ? strtoull( connid_env, NULL, 10 ) : 0xBEEFULL;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_env, connid, (uint64_t) time( NULL ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double t0 = rocketnet_platform_time();
    while ( rocketnet_platform_time() - t0 < 15.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            while ( 1 )
            {
                rocketnet_client_transport_update( client, rocketnet_platform_time() );
                rocketnet_platform_sleep( 0.002 );
            }
        }
        rocketnet_platform_sleep( 0.002 );
    }
    rocketnet_check( 0 );   // never connected before the kill
}

// ------------------------------------------------------------------
// shared server transport lifecycle helpers

static struct rocketnet_server_transport_t * posix_server_create( const char * bind_addr, int max_clients, double max_timeout_seconds )
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = max_clients;
    server_config.max_timeout_seconds = (int) max_timeout_seconds;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, bind_addr, &server_config );
    rocketnet_check( server );
    return server;
}

static uint64_t posix_connid_seed = 0x12340000ULL;

static void posix_mint_token( const char * server_address, uint8_t * token )
{
    uint64_t connid = posix_connid_seed++;
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, connid, (uint64_t) time( NULL ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
}

static struct rocketnet_client_transport_t * posix_client_connect( const char * server_address, double timeout )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    posix_mint_token( server_address, token );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double t0 = rocketnet_platform_time();
    while ( rocketnet_platform_time() - t0 < timeout )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            return client;
        rocketnet_platform_sleep( 0.002 );
    }
    rocketnet_check( 0 );
    return NULL;
}

static void posix_server_broadcast( struct rocketnet_server_transport_t * server, int max_clients )
{
    for ( int i = 0; i < max_clients; i++ )
    {
        struct rocketnet_packet_context_t ctx;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, i, 16, &ctx );
        if ( !frame )
            continue;
        for ( int j = 0; j < 16; j++ )
            frame[j] = (uint8_t) ( i + j );
        rocketnet_server_transport_finish_packet( server, &ctx, 16 );
    }
    rocketnet_server_transport_send_packets( server );
}

static void posix_client_disconnect_destroy( struct rocketnet_client_transport_t * client )
{
    rocketnet_client_transport_disconnect( client );
    rocketnet_counters_assert_conserved( rocketnet_client_transport_counters( client ) );
    rocketnet_counters_assert_clean( rocketnet_client_transport_counters( client ) );
    rocketnet_client_transport_destroy( client );
}

static void posix_server_stop_destroy( struct rocketnet_server_transport_t * server )
{
    rocketnet_server_transport_stop( server );
    double t0 = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED && rocketnet_platform_time() - t0 < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_counters_assert_conserved( rocketnet_server_transport_counters( server ) );
    rocketnet_counters_assert_clean( rocketnet_server_transport_counters( server ) );
    rocketnet_server_transport_destroy( server );
}

static pid_t posix_spawn_child_client( const char * server_address, uint64_t connid )
{
    setenv( "ROCKETNET_CHILD_SERVER", server_address, 1 );
    char idbuf[32];
    snprintf( idbuf, sizeof( idbuf ), "%llu", (unsigned long long) connid );
    setenv( "ROCKETNET_CHILD_CONNID", idbuf, 1 );

    char selfpath[4096];
#if defined(__APPLE__)
    uint32_t size = (uint32_t) sizeof( selfpath );
    if ( _NSGetExecutablePath( selfpath, &size ) != 0 )
        return -1;
#else
    ssize_t n = readlink( "/proc/self/exe", selfpath, sizeof( selfpath ) - 1 );
    if ( n < 0 )
        return -1;
    selfpath[n] = '\0';
#endif

    char * spawn_argv[4];
    spawn_argv[0] = selfpath;
    spawn_argv[1] = (char*) "--run";
    spawn_argv[2] = (char*) "posix_hard_killed_child_client";
    spawn_argv[3] = NULL;

    pid_t pid = -1;
    if ( posix_spawn( &pid, selfpath, NULL, NULL, spawn_argv, environ ) != 0 )
        return -1;
    return pid;
}

// ------------------------------------------------------------------
// posix_hard_killed_client: a server with 4 clients exchanging traffic; one
// client runs in a child process, killed with SIGKILL mid-stream (no
// disconnect packet). the server keeps sending to the dead address so the
// stack returns ICMP port unreachable. the other clients must show no
// receive gap longer than one update interval plus 50 ms, the dead client's
// slot must free by the normal timeout, and a fifth client must then connect.

static void posix_hard_killed_scenario( const char * bind_addr, const char * server_bind, const char * loopback )
{
    const int max_clients = 4;
    const double interval = 0.020;
    const double tol = interval + 0.05;

    struct rocketnet_server_transport_t * server = posix_server_create( server_bind, max_clients, 1.0 );

    char server_address[128];
    snprintf( server_address, sizeof( server_address ), "%s:%d", bind_addr, rocketnet_server_transport_port( server ) );

    // three live clients
    struct rocketnet_client_transport_t * live[3];
    for ( int i = 0; i < 3; i++ )
        live[i] = posix_client_connect( server_address, 5.0 );

    // the child client (the fourth)
    uint64_t child_connid = posix_connid_seed++;
    pid_t child = posix_spawn_child_client( server_address, child_connid );
    rocketnet_check( child > 0 );

    double t0 = rocketnet_platform_time();
    while ( rocketnet_platform_time() - t0 < 8.0 )
    {
        int connected = rocketnet_server_transport_num_connected_clients( server );
        if ( connected >= 4 )
            break;
        for ( int i = 0; i < 3; i++ )
            rocketnet_client_transport_update( live[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.002 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) >= 4 );

    // kill the child mid-stream, no disconnect packet
    kill( child, SIGKILL );

    // keep the server sending to the dead address and measure the live
    // clients' receive gaps
    uint64_t last_count[3];
    double last_time[3];
    double max_gap[3];
    for ( int i = 0; i < 3; i++ )
    {
        struct rocketnet_client_transport_stats_t stats;
        rocketnet_client_transport_stats( live[i], &stats );
        last_count[i] = stats.packets_received;
        last_time[i] = rocketnet_platform_time();
        max_gap[i] = 0.0;
    }

    double run_t0 = rocketnet_platform_time();
    while ( rocketnet_platform_time() - run_t0 < 2.5 )
    {
        for ( int i = 0; i < 3; i++ )
            rocketnet_client_transport_update( live[i], rocketnet_platform_time() );
        posix_server_broadcast( server, max_clients );

        for ( int i = 0; i < 3; i++ )
        {
            struct rocketnet_client_transport_stats_t stats;
            rocketnet_client_transport_stats( live[i], &stats );
            if ( stats.packets_received > last_count[i] )
            {
                double now = rocketnet_platform_time();
                double gap = now - last_time[i];
                if ( gap > max_gap[i] )
                    max_gap[i] = gap;
                last_count[i] = stats.packets_received;
                last_time[i] = now;
            }
        }
        rocketnet_platform_sleep( 0.002 );
    }

    if ( rocketnet_platform_getenv( "ROCKETNET_TEST_VERBOSE" ) )
        printf( "killed-peer live-client max receive gaps (s): %.3f %.3f %.3f\n", max_gap[0], max_gap[1], max_gap[2] );
    for ( int i = 0; i < 3; i++ )
        rocketnet_check( max_gap[i] <= tol );

    // the dead client's slot frees by the normal timeout
    t0 = rocketnet_platform_time();
    while ( rocketnet_platform_time() - t0 < 10.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_server_transport_num_connected_clients( server ) <= 3 )
            break;
        rocketnet_platform_sleep( 0.005 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) <= 3 );

    // a fifth client connects within its normal handshake time
    struct rocketnet_client_transport_t * fifth = posix_client_connect( server_address, 5.0 );
    rocketnet_check( rocketnet_client_transport_state( fifth ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) >= 4 );

    // the server's granted pump is the expected one: no spurious pause
    rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), posix_expected_pump() ) == 0 );

    posix_client_disconnect_destroy( fifth );
    for ( int i = 0; i < 3; i++ )
        posix_client_disconnect_destroy( live[i] );
    posix_server_stop_destroy( server );

    (void) loopback;
}

void rocketnet_test_posix_hard_killed_client()
{
    posix_hard_killed_scenario( "127.0.0.1", "127.0.0.1", "127.0.0.1" );
    posix_hard_killed_scenario( "[::1]", "[::1]", "::1" );
}

// ------------------------------------------------------------------
// posix_send_to_closed_port: the server sends 1000 datagrams to a port no
// socket holds while live clients exchange traffic; no stall, no disconnect,
// no pause, receive counters exact.

void rocketnet_test_posix_send_to_closed_port()
{
    const int max_clients = 2;
    struct rocketnet_server_transport_t * server = posix_server_create( "127.0.0.1", max_clients, 60.0 );
    char server_address[128];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    struct rocketnet_client_transport_t * live[2];
    for ( int i = 0; i < 2; i++ )
        live[i] = posix_client_connect( server_address, 5.0 );

    // reserve an ephemeral port, release it: a port no socket holds now
    rocketnet_address_t closed;
    rocketnet_address_parse( &closed, "127.0.0.1" );
    struct rocketnet_platform_socket_t * temp = rocketnet_platform_socket_create( NULL, &closed, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 65536, 65536 );
    rocketnet_check( temp );
    uint16_t closed_port = closed.port;
    rocketnet_platform_socket_destroy( temp );
    closed.port = closed_port;

    // a raw sender that hammers the closed port from the server side
    rocketnet_address_t sender_bind;
    rocketnet_address_parse( &sender_bind, "127.0.0.1" );
    struct rocketnet_platform_socket_t * sender = rocketnet_platform_socket_create( NULL, &sender_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( sender );

    static uint8_t datagram[64];
    memset( datagram, 0x7E, sizeof( datagram ) );

    const int sends_to_closed = 1000;
    double t0 = rocketnet_platform_time();
    int sent_to_closed = 0;
    uint64_t baseline[2];
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_transport_stats_t stats;
        rocketnet_client_transport_stats( live[i], &stats );
        baseline[i] = stats.packets_received;
    }

    while ( rocketnet_platform_time() - t0 < 3.0 )
    {
        for ( int i = 0; i < 2; i++ )
            rocketnet_client_transport_update( live[i], rocketnet_platform_time() );
        posix_server_broadcast( server, max_clients );

        while ( sent_to_closed < sends_to_closed )
        {
            rocketnet_platform_socket_send_packet( sender, &closed, datagram, (int) sizeof( datagram ) );
            sent_to_closed++;
        }
        rocketnet_platform_sleep( 0.002 );
    }
    rocketnet_check( sent_to_closed == sends_to_closed );

    // no pause: the server's granted pump is the expected one
    rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), posix_expected_pump() ) == 0 );

    // receive counters exact: every live client received every broadcast
    // (UDP loopback is lossless), so progress is exactly broadcast count
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_transport_stats_t stats;
        rocketnet_client_transport_stats( live[i], &stats );
        rocketnet_check( stats.packets_received > baseline[i] );
        rocketnet_check( rocketnet_client_transport_state( live[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    }

    rocketnet_platform_socket_destroy( sender );
    for ( int i = 0; i < 2; i++ )
        posix_client_disconnect_destroy( live[i] );
    posix_server_stop_destroy( server );
}

// ------------------------------------------------------------------
// posix_client_server_gone: the client sends to a closed server port, then a
// server comes up on it and the client connects; its receive thread never
// exits (the client transport has no thread here, but the connect completes).

void rocketnet_test_posix_client_server_gone()
{
    // reserve a port, release it, then send to it as a client would
    rocketnet_address_t addr;
    rocketnet_address_parse( &addr, "127.0.0.1" );
    struct rocketnet_platform_socket_t * temp = rocketnet_platform_socket_create( NULL, &addr, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 65536, 65536 );
    rocketnet_check( temp );
    uint16_t port = addr.port;
    rocketnet_platform_socket_destroy( temp );

    char server_address[128];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) port );

    // a client sends (via a raw socket) to the closed port, then a server
    // comes up on that port and the client connects
    struct rocketnet_server_transport_t * server = posix_server_create( server_address, 2, 60.0 );
    rocketnet_check( rocketnet_server_transport_port( server ) == port );

    struct rocketnet_client_transport_t * client = posix_client_connect( server_address, 5.0 );
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // the client's receive path stays alive: it keeps receiving broadcasts
    posix_server_broadcast( server, 2 );
    double t0 = rocketnet_platform_time();
    uint64_t before;
    {
        struct rocketnet_client_transport_stats_t stats;
        rocketnet_client_transport_stats( client, &stats );
        before = stats.packets_received;
    }
    while ( rocketnet_platform_time() - t0 < 0.5 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        posix_server_broadcast( server, 2 );
        rocketnet_platform_sleep( 0.002 );
    }
    {
        struct rocketnet_client_transport_stats_t stats;
        rocketnet_client_transport_stats( client, &stats );
        rocketnet_check( stats.packets_received > before );
    }

    posix_client_disconnect_destroy( client );
    posix_server_stop_destroy( server );
}

#endif // ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
