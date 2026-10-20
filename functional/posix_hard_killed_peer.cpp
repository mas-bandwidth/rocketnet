/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    A hard-killed peer or an ICMP error never stalls or degrades a receive
    (DRIVERS.md, ICMP errors). A UDP send to a vanished peer comes back as
    ICMP port unreachable; a BSD stack reports it to the next receive on a
    connected socket as ECONNREFUSED. These cases run per server transport
    cell (portable, msgx, and msgx forced portable by ROCKETNET_PUMP=portable),
    per address family, per client transport:

      posix_hard_killed_client  one of four clients runs in a child process
                                (a child mode of this binary started by
                                posix_spawn) and is killed with SIGKILL
                                mid-stream; the server keeps sending to it,
                                the other three see no receive gap, the dead
                                slot times out and frees, a fifth connects.
      posix_send_to_closed_port the server sends 1000 datagrams to a port no
                                socket holds while live clients exchange
                                traffic; no stall, no disconnect, no pause.
      posix_client_server_gone  a client sends to a closed server port, then a
                                server comes up on it and the client connects.

    and the receive seam cases inject each errno into the next recvmsg_x and
    recvmsg return and prove the drain continues and msgx stays up.

    The macOS cells live here; the file builds on every POSIX platform, and
    other platforms register no cells of their own here.
*/

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <mach-o/dyld.h>

#include <string>
#include <vector>

extern char ** environ;

#define ROCKETNET_COUNTERS_CHECK posix_hard_killed_require
static void posix_hard_killed_check( bool condition, const char * expression, const char * function, int line )
{
    if ( condition )
        return;
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", expression, function, __FILE__, line );
    fflush( stdout );
    exit( 1 );
}
#define posix_hard_killed_require( condition ) posix_hard_killed_check( (condition), #condition, __FUNCTION__, __LINE__ )
#include "../test/counters_expect.h"

// the server's update interval, and the receive-gap bound DONE-WHEN names:
// one interval plus 50 ms

static const double POSIX_HK_TICK = 1.0 / 60.0;
static const double POSIX_HK_GAP_BOUND = POSIX_HK_TICK + 0.050;

// the development token's timeout (rocketnet_write_dev_connect_token) and the
// server config's handshake deadline: the normal times the dead slot frees
// in and the fifth client connects in

static const double POSIX_HK_CLIENT_TIMEOUT = 10.0;
static const double POSIX_HK_HANDSHAKE_SECONDS = 5.0;

#define POSIX_HK_CHILD_FLAG "--posix-hard-killed-child"

struct posix_hk_cell_t
{
    const char * server;        // portable | msgx | msgx_pump_portable
    bool ipv6;
    const char * client;        // portable | msgx
};

static void posix_hk_setenv( const char * name, const char * value )
{
    if ( value )
        setenv( name, value, 1 );
    else
        unsetenv( name );
}

static const char * posix_hk_expected_server_pump( const posix_hk_cell_t & cell )
{
    return strcmp( cell.server, "msgx" ) == 0 ? "msgx" : "portable";
}

static void posix_hk_server_address( const posix_hk_cell_t & cell, int port, char * out, size_t bytes )
{
    if ( cell.ipv6 )
        snprintf( out, bytes, "[::1]:%d", port );
    else
        snprintf( out, bytes, "127.0.0.1:%d", port );
}

// create the cell's server: the transport pinned by env, as the CI matrix
// pins it. msgx_pump_portable asks for msgx under ROCKETNET_PUMP=portable,
// which the msgx rung refuses before binding, so the ladder falls to the
// portable pump; the override is cleared once the server holds its socket

static struct rocketnet_server_transport_t * posix_hk_server_create( const posix_hk_cell_t & cell, int port, int max_clients )
{
    const bool pump_portable = strcmp( cell.server, "msgx_pump_portable" ) == 0;
    posix_hk_setenv( "ROCKETNET_SERVER_TRANSPORT", pump_portable ? "msgx,portable" : cell.server );
    posix_hk_setenv( "ROCKETNET_PUMP", pump_portable ? "portable" : NULL );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;

    char address[64];
    if ( port == 0 )
        snprintf( address, sizeof(address), "%s", cell.ipv6 ? "[::1]" : "127.0.0.1" );
    else
        posix_hk_server_address( cell, port, address, sizeof(address) );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, address, &config );
    posix_hk_setenv( "ROCKETNET_PUMP", NULL );
    posix_hard_killed_require( server );

    const char * expected_driver = strcmp( cell.server, "msgx" ) == 0 ? "msgx" : "portable";
    posix_hard_killed_require( strcmp( rocketnet_server_transport_driver( server ), expected_driver ) == 0 );
    struct rocketnet_platform_socket_t * socket = rocketnet_server_transport_platform_socket_for_testing( server );
    posix_hard_killed_require( socket );
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( socket ), posix_hk_expected_server_pump( cell ) ) == 0 );
    return server;
}

static struct rocketnet_client_transport_t * posix_hk_client_create( const char * server_address, uint64_t client_id )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    posix_hard_killed_require( rocketnet_write_dev_connect_token( server_address, client_id, (uint64_t) time( NULL ), token, sizeof(token) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
    posix_hard_killed_require( client );
    return client;
}

static void posix_hk_server_stop_and_destroy( struct rocketnet_server_transport_t * server )
{
    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
    }
    rocketnet_counters_assert_conserved( rocketnet_server_transport_counters( server ) );
    rocketnet_server_transport_destroy( server );
}

static void posix_hk_client_disconnect_and_destroy( struct rocketnet_client_transport_t * client )
{
    rocketnet_client_transport_disconnect( client );
    rocketnet_counters_assert_conserved( rocketnet_client_transport_counters( client ) );
    rocketnet_client_transport_destroy( client );
}

// one tracked in-process client: its slot, what it sent and received, and
// the longest receive gap on each side of the wire

struct posix_hk_live_t
{
    struct rocketnet_client_transport_t * client;
    int slot;
    uint64_t sent_up;
    uint64_t received_up;       // at the server, from this client's slot
    uint64_t sent_down;
    uint64_t received_down;     // at this client
    double last_up;
    double last_down;
    double max_gap_up;
    double max_gap_down;
};

static void posix_hk_live_init( posix_hk_live_t & live, struct rocketnet_client_transport_t * client )
{
    memset( &live, 0, sizeof(live) );
    live.client = client;
    live.slot = -1;
}

// one server update interval of traffic: every live client drains, the
// server drains and sends one payload down to every connected slot (a dead
// one included, until it times out), then every connected live client sends
// one payload up. gaps are measured only while measure is set.

static void posix_hk_tick( struct rocketnet_server_transport_t * server, std::vector<posix_hk_live_t> & live, bool measure, uint64_t * sent_to_slot_total )
{
    const double now = rocketnet_platform_time();
    uint8_t payload[32];

    for ( size_t i = 0; i < live.size(); i++ )
    {
        rocketnet_client_transport_update( live[i].client, now );
        const int num = rocketnet_client_transport_num_payloads( live[i].client );
        for ( int p = 0; p < num; p++ )
        {
            int bytes = 0;
            uint8_t * data = rocketnet_client_transport_payload( live[i].client, p, &bytes );
            if ( !data || bytes != (int) sizeof(payload) || data[0] != 0xD0 )
                continue;
            live[i].received_down++;
            if ( measure && live[i].last_down > 0.0 && now - live[i].last_down > live[i].max_gap_down )
                live[i].max_gap_down = now - live[i].last_down;
            live[i].last_down = now;
        }
    }

    rocketnet_server_transport_update( server, now );

    const int num = rocketnet_server_transport_num_payloads( server );
    for ( int p = 0; p < num; p++ )
    {
        int slot = -1;
        int bytes = 0;
        uint8_t * data = rocketnet_server_transport_payload( server, p, &slot, &bytes );
        if ( !data || bytes != (int) sizeof(payload) || data[0] != 0x0U )
            continue;
        for ( size_t i = 0; i < live.size(); i++ )
        {
            if ( live[i].slot != slot )
                continue;
            live[i].received_up++;
            if ( measure && live[i].last_up > 0.0 && now - live[i].last_up > live[i].max_gap_up )
                live[i].max_gap_up = now - live[i].last_up;
            live[i].last_up = now;
        }
    }

    for ( int slot = 0; slot < ROCKETNET_MAX_CLIENTS; slot++ )
    {
        if ( !rocketnet_server_transport_is_client_connected( server, slot ) )
        {
            if ( slot >= 8 )
                break;
            continue;
        }
        memset( payload, 0, sizeof(payload) );
        payload[0] = 0xD0;
        payload[1] = (uint8_t) slot;
        rocketnet_server_transport_send_packet( server, slot, payload, (int) sizeof(payload) );
        if ( sent_to_slot_total )
            sent_to_slot_total[slot]++;
        for ( size_t i = 0; i < live.size(); i++ )
            if ( live[i].slot == slot )
                live[i].sent_down++;
    }
    rocketnet_server_transport_send_packets( server );

    // the live clients send up last, so each payload has the rest of the tick
    // to reach the server's receive thread before the next
    // rocketnet_server_transport_update takes it, as the server's payloads
    // down have before the next client update. sent just ahead of the
    // server's update, a payload the receive thread had not yet read waited a
    // second update, a gap of two intervals with no receive at fault.

    for ( size_t i = 0; i < live.size(); i++ )
    {
        if ( rocketnet_client_transport_state( live[i].client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            memset( payload, 0, sizeof(payload) );
            payload[0] = 0x0U;
            payload[1] = (uint8_t) i;
            rocketnet_client_transport_send_packet( live[i].client, payload, (int) sizeof(payload) );
            rocketnet_client_transport_send_packets( live[i].client );
            live[i].sent_up++;
            if ( live[i].slot < 0 )
                live[i].slot = rocketnet_client_transport_client_index( live[i].client );
        }
    }

    // a gap still open counts too: a receive that stopped outright is the
    // longest gap of all, not an absent one

    if ( measure )
    {
        const double after = rocketnet_platform_time();
        for ( size_t i = 0; i < live.size(); i++ )
        {
            if ( live[i].last_up > 0.0 && after - live[i].last_up > live[i].max_gap_up )
                live[i].max_gap_up = after - live[i].last_up;
            if ( live[i].last_down > 0.0 && after - live[i].last_down > live[i].max_gap_down )
                live[i].max_gap_down = after - live[i].last_down;
        }
    }
}

static void posix_hk_sleep_rest_of_tick( double tick_start )
{
    const double spent = rocketnet_platform_time() - tick_start;
    if ( spent < POSIX_HK_TICK )
        rocketnet_platform_sleep( POSIX_HK_TICK - spent );
}

// run ticks until every live client is connected and has its slot

static void posix_hk_connect_all( struct rocketnet_server_transport_t * server, std::vector<posix_hk_live_t> & live, double deadline_seconds )
{
    const double start = rocketnet_platform_time();
    for ( ;; )
    {
        const double tick_start = rocketnet_platform_time();
        posix_hk_tick( server, live, false, NULL );
        bool all = true;
        for ( size_t i = 0; i < live.size(); i++ )
            if ( rocketnet_client_transport_state( live[i].client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED || live[i].slot < 0 )
                all = false;
        if ( all )
            return;
        posix_hard_killed_require( rocketnet_platform_time() - start < deadline_seconds );
        posix_hk_sleep_rest_of_tick( tick_start );
    }
}

// drain in-flight traffic without sending, so sent and received counts can
// be compared exactly

static void posix_hk_quiesce( struct rocketnet_server_transport_t * server, std::vector<posix_hk_live_t> & live )
{
    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 0.25 )
    {
        const double now = rocketnet_platform_time();
        for ( size_t i = 0; i < live.size(); i++ )
        {
            rocketnet_client_transport_update( live[i].client, now );
            const int num = rocketnet_client_transport_num_payloads( live[i].client );
            for ( int p = 0; p < num; p++ )
            {
                int bytes = 0;
                uint8_t * data = rocketnet_client_transport_payload( live[i].client, p, &bytes );
                if ( data && bytes == 32 && data[0] == 0xD0 )
                    live[i].received_down++;
            }
        }
        rocketnet_server_transport_update( server, now );
        const int num = rocketnet_server_transport_num_payloads( server );
        for ( int p = 0; p < num; p++ )
        {
            int slot = -1;
            int bytes = 0;
            uint8_t * data = rocketnet_server_transport_payload( server, p, &slot, &bytes );
            if ( !data || bytes != 32 || data[0] != 0x0U )
                continue;
            for ( size_t i = 0; i < live.size(); i++ )
                if ( live[i].slot == slot )
                    live[i].received_up++;
        }
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.005 );
    }
}

static std::string posix_hk_self_path()
{
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if ( _NSGetExecutablePath( buffer, &size ) != 0 )
        return std::string();
    return std::string( buffer );
}

// ---------------------------------------------------------------------------
// the child client: a functional process in child mode, connected to the
// server named on its command line, exchanging traffic until it is killed.
// it never disconnects; it ends itself only if its parent is gone or after a
// minute, so a failed parent never leaves it running.

int rocketnet_posix_hard_killed_child_main( int argc, char ** argv )
{
    if ( argc < 4 )
        return 2;
    setvbuf( stdout, NULL, _IOLBF, 0 );
    rocketnet_set_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK )
        return 1;
    const pid_t parent = getppid();
    struct rocketnet_client_transport_t * client = posix_hk_client_create( argv[2], strtoull( argv[3], NULL, 10 ) );
    const double start = rocketnet_platform_time();
    uint8_t payload[32];
    while ( rocketnet_platform_time() - start < 60.0 && getppid() == parent )
    {
        const double tick_start = rocketnet_platform_time();
        rocketnet_client_transport_update( client, tick_start );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            memset( payload, 0, sizeof(payload) );
            payload[1] = 0xFF;
            rocketnet_client_transport_send_packet( client, payload, (int) sizeof(payload) );
            rocketnet_client_transport_send_packets( client );
        }
        else if ( rocketnet_client_transport_state( client ) < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        {
            return 3;
        }
        posix_hk_sleep_rest_of_tick( tick_start );
    }
    return 0;
}

// ---------------------------------------------------------------------------

static void posix_hard_killed_client( const posix_hk_cell_t & cell )
{
    posix_hk_setenv( "ROCKETNET_CLIENT_TRANSPORT", cell.client );

    struct rocketnet_server_transport_t * server = posix_hk_server_create( cell, 0, 8 );
    struct rocketnet_platform_socket_t * server_socket = rocketnet_server_transport_platform_socket_for_testing( server );
    char server_address[64];
    posix_hk_server_address( cell, rocketnet_server_transport_port( server ), server_address, sizeof(server_address) );

    // the child first, so its slot is the only one connected when it lands

    const std::string self = posix_hk_self_path();
    posix_hard_killed_require( !self.empty() );
    char client_id_text[32];
    snprintf( client_id_text, sizeof(client_id_text), "%llu", (unsigned long long) ( 0x6B000ULL + (uint64_t) getpid() * 16 ) );
    std::vector<char> arg0( self.begin(), self.end() ); arg0.push_back( '\0' );
    char flag[] = POSIX_HK_CHILD_FLAG;
    char * child_argv[] = { arg0.data(), flag, server_address, client_id_text, NULL };
    pid_t child = 0;
    posix_hard_killed_require( posix_spawn( &child, self.c_str(), NULL, NULL, child_argv, environ ) == 0 );

    std::vector<posix_hk_live_t> none;
    int dead_slot = -1;
    const double child_start = rocketnet_platform_time();
    while ( dead_slot < 0 )
    {
        const double tick_start = rocketnet_platform_time();
        posix_hk_tick( server, none, false, NULL );
        for ( int slot = 0; slot < 8; slot++ )
            if ( rocketnet_server_transport_is_client_connected( server, slot ) )
                dead_slot = slot;
        if ( dead_slot < 0 && rocketnet_platform_time() - child_start > POSIX_HK_HANDSHAKE_SECONDS )
        {
            kill( child, SIGKILL );
            waitpid( child, NULL, 0 );
            posix_hard_killed_require( dead_slot >= 0 );
        }
        posix_hk_sleep_rest_of_tick( tick_start );
    }

    std::vector<posix_hk_live_t> live( 3 );
    for ( int i = 0; i < 3; i++ )
        posix_hk_live_init( live[i], posix_hk_client_create( server_address, 0x6C000ULL + (uint64_t) i ) );
    posix_hk_connect_all( server, live, POSIX_HK_HANDSHAKE_SECONDS );
    posix_hard_killed_require( rocketnet_server_transport_num_connected_clients( server ) == 4 );

    // a second of steady traffic, then the hard kill: no disconnect packet

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        const double tick_start = rocketnet_platform_time();
        posix_hk_tick( server, live, true, NULL );
        posix_hk_sleep_rest_of_tick( tick_start );
    }

    posix_hard_killed_require( kill( child, SIGKILL ) == 0 );
    int status = 0;
    posix_hard_killed_require( waitpid( child, &status, 0 ) == child );
    posix_hard_killed_require( WIFSIGNALED( status ) && WTERMSIG( status ) == SIGKILL );
    const double killed_at = rocketnet_platform_time();

    // the server keeps sending to the dead slot until the normal timeout
    // frees it; the stack answers each send with ICMP port unreachable

    uint64_t sent_to_slot[ROCKETNET_MAX_CLIENTS];
    memset( sent_to_slot, 0, sizeof(sent_to_slot) );
    double freed_at = 0.0;
    while ( freed_at == 0.0 )
    {
        const double tick_start = rocketnet_platform_time();
        posix_hk_tick( server, live, true, sent_to_slot );
        if ( !rocketnet_server_transport_is_client_connected( server, dead_slot ) )
            freed_at = rocketnet_platform_time();
        posix_hard_killed_require( tick_start - killed_at < POSIX_HK_CLIENT_TIMEOUT + 3.0 );
        posix_hk_sleep_rest_of_tick( tick_start );
    }

    const double timeout_seen = freed_at - killed_at;
    double worst_gap = 0.0;
    for ( int i = 0; i < 3; i++ )
    {
        if ( live[i].max_gap_up > worst_gap ) worst_gap = live[i].max_gap_up;
        if ( live[i].max_gap_down > worst_gap ) worst_gap = live[i].max_gap_down;
    }
    printf( "posix_hard_killed_client server=%s family=%s client=%s: %llu sends to the dead slot, slot freed %.2fs after the kill, worst live receive gap %.1f ms (bound %.1f ms), pump %s\n",
            cell.server, cell.ipv6 ? "ipv6" : "ipv4", cell.client, (unsigned long long) sent_to_slot[dead_slot], timeout_seen,
            worst_gap * 1000.0, POSIX_HK_GAP_BOUND * 1000.0, rocketnet_platform_socket_pump( server_socket ) );
    for ( int i = 0; i < 3; i++ )
        printf( "  live client %d slot %d: up gap %.1f ms down gap %.1f ms\n", i, live[i].slot, live[i].max_gap_up * 1000.0, live[i].max_gap_down * 1000.0 );

    posix_hard_killed_require( sent_to_slot[dead_slot] > 0 );
    posix_hard_killed_require( timeout_seen >= POSIX_HK_CLIENT_TIMEOUT - 1.0 );
    posix_hard_killed_require( timeout_seen <= POSIX_HK_CLIENT_TIMEOUT + 2.0 );
    for ( int i = 0; i < 3; i++ )
    {
        posix_hard_killed_require( rocketnet_client_transport_state( live[i].client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        posix_hard_killed_require( live[i].max_gap_up <= POSIX_HK_GAP_BOUND );
        posix_hard_killed_require( live[i].max_gap_down <= POSIX_HK_GAP_BOUND );
    }
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( server_socket ), posix_hk_expected_server_pump( cell ) ) == 0 );
    posix_hard_killed_require( rocketnet_server_transport_num_connected_clients( server ) == 3 );

    // a fifth client connects into the freed slot within its handshake time

    std::vector<posix_hk_live_t> fifth( 1 );
    posix_hk_live_init( fifth[0], posix_hk_client_create( server_address, 0x6D000ULL ) );
    const double fifth_start = rocketnet_platform_time();
    posix_hk_connect_all( server, fifth, POSIX_HK_HANDSHAKE_SECONDS );
    printf( "  fifth client connected in %.2fs into slot %d\n", rocketnet_platform_time() - fifth_start, fifth[0].slot );
    posix_hard_killed_require( rocketnet_server_transport_num_connected_clients( server ) == 4 );

    posix_hk_client_disconnect_and_destroy( fifth[0].client );
    for ( int i = 0; i < 3; i++ )
        posix_hk_client_disconnect_and_destroy( live[i].client );
    posix_hk_server_stop_and_destroy( server );
}

// a port no socket holds: bind one, close it. it is drawn below the
// ephemeral range (net.inet.ip.portrange.first, 49152 on macOS), so a socket
// another case binds to port 0 is never granted it between this close and
// the bind posix_client_server_gone makes on it; an ephemeral port released
// here was, and the server's create then failed on a port in use.

static int posix_hk_closed_port( bool ipv6 )
{
    for ( int attempt = 0; attempt < 1000; attempt++ )
    {
        rocketnet_address_t address;
        rocketnet_address_parse( &address, ipv6 ? "[::1]" : "127.0.0.1" );
        address.port = (uint16_t) ( 20000 + arc4random_uniform( 20000 ) );
        struct rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, 65536, 65536, "portable" );
        if ( !socket )
            continue;
        const int port = address.port;
        rocketnet_platform_socket_destroy( socket );
        return port;
    }
    posix_hard_killed_require( false );
    return 0;
}

static void posix_send_to_closed_port( const posix_hk_cell_t & cell )
{
    posix_hk_setenv( "ROCKETNET_CLIENT_TRANSPORT", cell.client );

    struct rocketnet_server_transport_t * server = posix_hk_server_create( cell, 0, 8 );
    struct rocketnet_platform_socket_t * server_socket = rocketnet_server_transport_platform_socket_for_testing( server );
    char server_address[64];
    posix_hk_server_address( cell, rocketnet_server_transport_port( server ), server_address, sizeof(server_address) );

    std::vector<posix_hk_live_t> live( 3 );
    for ( int i = 0; i < 3; i++ )
        posix_hk_live_init( live[i], posix_hk_client_create( server_address, 0x6E000ULL + (uint64_t) i ) );
    posix_hk_connect_all( server, live, POSIX_HK_HANDSHAKE_SECONDS );

    // let the handshake's own traffic settle, then count from zero

    posix_hk_quiesce( server, live );
    for ( int i = 0; i < 3; i++ )
    {
        live[i].sent_up = live[i].received_up = live[i].sent_down = live[i].received_down = 0;
        live[i].last_up = live[i].last_down = 0.0;
    }

    rocketnet_address_t closed;
    rocketnet_address_parse( &closed, cell.ipv6 ? "[::1]" : "127.0.0.1" );
    closed.port = (uint16_t) posix_hk_closed_port( cell.ipv6 );

    // 1000 datagrams to the closed port, 20 a tick, across 50 ticks of live
    // traffic, on the server's own socket

    uint8_t junk[64];
    memset( junk, 0xAB, sizeof(junk) );
    int sent_closed = 0;
    while ( sent_closed < 1000 )
    {
        const double tick_start = rocketnet_platform_time();
        for ( int i = 0; i < 20; i++, sent_closed++ )
            rocketnet_platform_socket_send_packet( server_socket, &closed, junk, (int) sizeof(junk) );
        posix_hk_tick( server, live, true, NULL );
        posix_hk_sleep_rest_of_tick( tick_start );
    }
    for ( int t = 0; t < 30; t++ )
    {
        const double tick_start = rocketnet_platform_time();
        posix_hk_tick( server, live, true, NULL );
        posix_hk_sleep_rest_of_tick( tick_start );
    }
    posix_hk_quiesce( server, live );

    double worst_gap = 0.0;
    for ( int i = 0; i < 3; i++ )
    {
        if ( live[i].max_gap_up > worst_gap ) worst_gap = live[i].max_gap_up;
        if ( live[i].max_gap_down > worst_gap ) worst_gap = live[i].max_gap_down;
    }
    printf( "posix_send_to_closed_port server=%s family=%s client=%s: %d datagrams to closed port %d, worst live receive gap %.1f ms (bound %.1f ms), pump %s\n",
            cell.server, cell.ipv6 ? "ipv6" : "ipv4", cell.client, sent_closed, closed.port, worst_gap * 1000.0, POSIX_HK_GAP_BOUND * 1000.0,
            rocketnet_platform_socket_pump( server_socket ) );
    for ( int i = 0; i < 3; i++ )
        printf( "  live client %d slot %d: up %llu/%llu down %llu/%llu\n", i, live[i].slot,
                (unsigned long long) live[i].received_up, (unsigned long long) live[i].sent_up,
                (unsigned long long) live[i].received_down, (unsigned long long) live[i].sent_down );

    for ( int i = 0; i < 3; i++ )
    {
        posix_hard_killed_require( rocketnet_client_transport_state( live[i].client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        posix_hard_killed_require( live[i].sent_up > 0 && live[i].received_up == live[i].sent_up );
        posix_hard_killed_require( live[i].sent_down > 0 && live[i].received_down == live[i].sent_down );
        posix_hard_killed_require( live[i].max_gap_up <= POSIX_HK_GAP_BOUND );
        posix_hard_killed_require( live[i].max_gap_down <= POSIX_HK_GAP_BOUND );
    }
    posix_hard_killed_require( rocketnet_server_transport_num_connected_clients( server ) == 3 );
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( server_socket ), posix_hk_expected_server_pump( cell ) ) == 0 );

    for ( int i = 0; i < 3; i++ )
        posix_hk_client_disconnect_and_destroy( live[i].client );
    posix_hk_server_stop_and_destroy( server );
}

static void posix_client_server_gone( const posix_hk_cell_t & cell )
{
    posix_hk_setenv( "ROCKETNET_CLIENT_TRANSPORT", cell.client );

    const int port = posix_hk_closed_port( cell.ipv6 );
    char server_address[64];
    posix_hk_server_address( cell, port, server_address, sizeof(server_address) );

    // the client starts its handshake against a closed port: every request
    // it sends draws ICMP port unreachable

    std::vector<posix_hk_live_t> live( 1 );
    posix_hk_live_init( live[0], posix_hk_client_create( server_address, 0x6F000ULL ) );
    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        rocketnet_client_transport_update( live[0].client, rocketnet_platform_time() );
        posix_hard_killed_require( rocketnet_client_transport_state( live[0].client ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
        rocketnet_platform_sleep( POSIX_HK_TICK );
    }

    // the server comes up on that port; the client connects through the
    // same receive thread that took the errors

    struct rocketnet_server_transport_t * server = posix_hk_server_create( cell, port, 8 );
    struct rocketnet_platform_socket_t * server_socket = rocketnet_server_transport_platform_socket_for_testing( server );
    const double server_start = rocketnet_platform_time();
    posix_hk_connect_all( server, live, POSIX_HK_HANDSHAKE_SECONDS );
    const double connected_in = rocketnet_platform_time() - server_start;

    for ( int t = 0; t < 30; t++ )
    {
        const double tick_start = rocketnet_platform_time();
        posix_hk_tick( server, live, true, NULL );
        posix_hk_sleep_rest_of_tick( tick_start );
    }
    printf( "posix_client_server_gone server=%s family=%s client=%s: port %d closed for 1.0s, connected %.2fs after the server came up, down %llu up %llu\n",
            cell.server, cell.ipv6 ? "ipv6" : "ipv4", cell.client, port, connected_in,
            (unsigned long long) live[0].received_down, (unsigned long long) live[0].received_up );
    posix_hard_killed_require( rocketnet_client_transport_state( live[0].client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    posix_hard_killed_require( live[0].received_down > 0 && live[0].received_up > 0 );
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( server_socket ), posix_hk_expected_server_pump( cell ) ) == 0 );

    posix_hk_client_disconnect_and_destroy( live[0].client );
    posix_hk_server_stop_and_destroy( server );
}

// ---------------------------------------------------------------------------
// the receive seam: each errno injected into the next recvmsg_x (the msgx
// arm) and recvmsg (the portable arm) return

#define POSIX_SEAM_PACKETS 10
#define POSIX_SEAM_BYTES 64
#define POSIX_SEAM_STRIDE 1500

static struct rocketnet_platform_socket_t * posix_seam_socket_with_queue( const char * pump )
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "127.0.0.1" );
    struct rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, 1024 * 1024, 1024 * 1024, pump );
    posix_hard_killed_require( socket );
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );

    uint8_t data[POSIX_SEAM_BYTES];
    for ( int i = 0; i < POSIX_SEAM_PACKETS; i++ )
    {
        memset( data, i, sizeof(data) );
        rocketnet_platform_socket_send_packet( socket, &address, data, (int) sizeof(data) );
    }

    // every datagram queued before the receive under test runs: counted in
    // datagrams (SO_NUMRCVPKT), since a byte count can reach the total while
    // the last datagram is still in flight

    const double start = rocketnet_platform_time();
    for ( ;; )
    {
        int queued = 0;
        socklen_t queued_bytes = sizeof(queued);
        posix_hard_killed_require( getsockopt( socket->handle, SOL_SOCKET, SO_NUMRCVPKT, &queued, &queued_bytes ) == 0 );
        if ( queued >= POSIX_SEAM_PACKETS )
            break;
        posix_hard_killed_require( rocketnet_platform_time() - start < 2.0 );
        rocketnet_platform_sleep( 0.001 );
    }
    return socket;
}

static int posix_seam_warnings = 0;

static void posix_seam_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( level == ROCKETNET_LOG_LEVEL_WARN && strstr( message, "msgx pump pauses" ) != NULL )
        posix_seam_warnings++;
}

static void posix_seam_receive_errno( int error, const char * pump )
{
    struct rocketnet_platform_socket_t * socket = posix_seam_socket_with_queue( pump );

    rocketnet_address_t from[32];
    int bytes[32];
    static uint8_t data[32 * POSIX_SEAM_STRIDE];

    rocketnet_platform_inject_receive_errno_for_testing( error );
    int received = rocketnet_platform_socket_receive_packets( socket, from, data, POSIX_SEAM_STRIDE, bytes, 32 );

    // the arm consumed the injection: nothing is left pending

    rocketnet_platform_inject_receive_errno_for_testing( 0 );

    int total = received;
    if ( error == EAGAIN )
    {
        // EAGAIN is the drain's end: this call delivers nothing, never
        // pauses, and the next call delivers every queued datagram
        posix_hard_killed_require( received == 0 );
        total = rocketnet_platform_socket_receive_packets( socket, from, data, POSIX_SEAM_STRIDE, bytes, 32 );
    }

    printf( "posix_receive_seam errno=%d (%s) pump=%s: first call %d of %d, total %d, pump after %s\n", error, strerror( error ), pump,
            received, POSIX_SEAM_PACKETS, total, rocketnet_platform_socket_pump( socket ) );

    posix_hard_killed_require( total == POSIX_SEAM_PACKETS );
    for ( int i = 0; i < total; i++ )
    {
        posix_hard_killed_require( bytes[i] == POSIX_SEAM_BYTES );
        posix_hard_killed_require( data[(size_t) i * POSIX_SEAM_STRIDE] == (uint8_t) i );
    }
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

static void posix_seam_weather( int error )
{
    posix_seam_receive_errno( error, "msgx" );
    posix_seam_receive_errno( error, "portable" );
}

static void posix_seam_unknown_errno_pauses_msgx()
{
    // a genuinely unknown errno still pauses msgx: the call finishes on the
    // per-packet loop without losing a datagram, the pump reads portable
    // until the pause expires, and the pause logs once

    rocketnet_set_log_level( ROCKETNET_LOG_LEVEL_WARN );
    rocketnet_set_log_callback( posix_seam_log, NULL );

    struct rocketnet_platform_socket_t * socket = posix_seam_socket_with_queue( "msgx" );
    rocketnet_address_t from[32];
    int bytes[32];
    static uint8_t data[32 * POSIX_SEAM_STRIDE];

    rocketnet_platform_inject_receive_errno_for_testing( EPROTOTYPE );
    const int received = rocketnet_platform_socket_receive_packets( socket, from, data, POSIX_SEAM_STRIDE, bytes, 32 );
    printf( "posix_receive_seam errno=%d (%s) pump=msgx: received %d of %d, pump after %s, pause warnings %d\n", EPROTOTYPE, strerror( EPROTOTYPE ),
            received, POSIX_SEAM_PACKETS, rocketnet_platform_socket_pump( socket ), posix_seam_warnings );
    posix_hard_killed_require( received == POSIX_SEAM_PACKETS );
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( socket ), "portable" ) == 0 );
    posix_hard_killed_require( posix_seam_warnings == 1 );

    // while paused the batch arm is not called: another receive runs the
    // per-packet loop and logs no second pause

    uint8_t one[POSIX_SEAM_BYTES];
    memset( one, 0x5A, sizeof(one) );
    rocketnet_address_t self;
    rocketnet_address_parse( &self, "127.0.0.1" );
    self.port = from[0].port;
    rocketnet_platform_socket_send_packet( socket, &self, one, (int) sizeof(one) );
    int got = 0;
    const double start = rocketnet_platform_time();
    while ( got == 0 && rocketnet_platform_time() - start < 2.0 )
        got = rocketnet_platform_socket_receive_packets( socket, from, data, POSIX_SEAM_STRIDE, bytes, 32 );
    posix_hard_killed_require( got == 1 );
    posix_hard_killed_require( posix_seam_warnings == 1 );

    rocketnet_platform_socket_destroy( socket );
}

// ---------------------------------------------------------------------------
// the case table: three case kinds per cell, then the seam cases

static const char * posix_hk_servers[] = { "portable", "msgx", "msgx_pump_portable" };
static const char * posix_hk_clients[] = { "portable", "msgx" };
static const char * posix_hk_kinds[] = { "posix_hard_killed_client", "posix_send_to_closed_port", "posix_client_server_gone" };

struct posix_seam_case_t
{
    const char * name;
    int error;
};

static const posix_seam_case_t posix_seam_cases[] =
{
    { "posix_receive_seam_econnrefused", ECONNREFUSED },
    { "posix_receive_seam_ehostunreach", EHOSTUNREACH },
    { "posix_receive_seam_enetunreach", ENETUNREACH },
    { "posix_receive_seam_ehostdown", EHOSTDOWN },
    { "posix_receive_seam_enetdown", ENETDOWN },
    { "posix_receive_seam_enobufs", ENOBUFS },
    { "posix_receive_seam_eagain", EAGAIN },
    { "posix_receive_seam_eintr", EINTR },
    { "posix_receive_seam_unknown_errno_pauses_msgx", 0 },
};

#define POSIX_HK_NUM_CELL_CASES ( 3 * 2 * 2 * 3 )
#define POSIX_HK_NUM_SEAM_CASES ( (int) ( sizeof(posix_seam_cases) / sizeof(posix_seam_cases[0]) ) )

static char posix_hk_names[POSIX_HK_NUM_CELL_CASES][96];

static void posix_hk_cell_of( int index, int * kind, posix_hk_cell_t * cell )
{
    // index = ( ( kind * 3 + server ) * 2 + family ) * 2 + client
    const int client = index % 2;
    const int family = ( index / 2 ) % 2;
    const int server = ( index / 4 ) % 3;
    *kind = index / 12;
    cell->server = posix_hk_servers[server];
    cell->ipv6 = family == 1;
    cell->client = posix_hk_clients[client];
}

int rocketnet_posix_hard_killed_peer_test_count()
{
    return POSIX_HK_NUM_CELL_CASES + POSIX_HK_NUM_SEAM_CASES;
}

const char * rocketnet_posix_hard_killed_peer_test_name( int index )
{
    if ( index >= POSIX_HK_NUM_CELL_CASES )
        return posix_seam_cases[index - POSIX_HK_NUM_CELL_CASES].name;
    int kind = 0;
    posix_hk_cell_t cell;
    posix_hk_cell_of( index, &kind, &cell );
    snprintf( posix_hk_names[index], sizeof(posix_hk_names[index]), "%s_%s_%s_client_%s", posix_hk_kinds[kind], cell.server, cell.ipv6 ? "ipv6" : "ipv4", cell.client );
    return posix_hk_names[index];
}

// the suite group (test/suite_runner.h, group_of) of a case name: the cell
// cases measure receive gaps and handshake times against the wall clock, so
// they run as group 1, after every other case and the sibling executables
// (group 0) have finished, never beside the loopback load those make; they
// run in parallel with each other. the seam cases measure no time.

int rocketnet_posix_hard_killed_peer_test_group( const char * name )
{
    for ( int i = 0; i < 3; i++ )
        if ( strncmp( name, posix_hk_kinds[i], strlen( posix_hk_kinds[i] ) ) == 0 )
            return 1;
    return 0;
}

void rocketnet_posix_hard_killed_peer_test_run( int index )
{
    if ( index >= POSIX_HK_NUM_CELL_CASES )
    {
        const posix_seam_case_t & seam = posix_seam_cases[index - POSIX_HK_NUM_CELL_CASES];
        if ( seam.error == 0 )
            posix_seam_unknown_errno_pauses_msgx();
        else
            posix_seam_weather( seam.error );
        return;
    }
    int kind = 0;
    posix_hk_cell_t cell;
    posix_hk_cell_of( index, &kind, &cell );
    if ( kind == 0 )
        posix_hard_killed_client( cell );
    else if ( kind == 1 )
        posix_send_to_closed_port( cell );
    else
        posix_client_server_gone( cell );
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

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
void rocketnet_server_gso_test_hold_receive( struct rocketnet_server_gso_t * gso, bool hold );
bool rocketnet_server_gso_test_receive_held( struct rocketnet_server_gso_t * gso );
void rocketnet_server_gso_test_receive_passes( struct rocketnet_server_gso_t * gso, uint64_t * error_pass, uint64_t * data_pass );

static void rocketnet_posix_linux_seam_gso( int kind, int error )
{
    rocketnet_posix_cell_t cell;
    rocketnet_check( rocketnet_posix_cell_open( &cell, kind, false, 0, "posix_hard_killed_receive_errno_seam" ) );
    int peer_port = 0;
    rocketnet_platform_socket_t * peer = rocketnet_posix_bind_any( "127.0.0.1", &peer_port );

    rocketnet_server_gso_test_hold_receive( cell.gso, true );
    const double pause_deadline = rocketnet_platform_time() + 2.0;
    while ( !rocketnet_server_gso_test_receive_held( cell.gso ) && rocketnet_platform_time() < pause_deadline )
        rocketnet_platform_sleep( 0.001 );
    rocketnet_check( rocketnet_server_gso_test_receive_held( cell.gso ) );

    rocketnet_posix_peer_send( peer, "127.0.0.1", cell.port, 0x31 );
    rocketnet_posix_peer_send( peer, "127.0.0.1", cell.port, 0x32 );
    rocketnet_posix_settle();
    const uint64_t before = rocketnet_platform_receive_weather_skipped();
    rocketnet_platform_set_recvmmsg_error_for_testing( error );
    rocketnet_server_gso_test_hold_receive( cell.gso, false );

    const int got = rocketnet_posix_cell_drain( &cell, &peer_port, 1, 2, 2.0 );
    const uint64_t counted = rocketnet_platform_receive_weather_skipped() - before;
    uint64_t error_pass = 0, data_pass = 0;
    rocketnet_server_gso_test_receive_passes( cell.gso, &error_pass, &data_pass );
    printf( "seam gso error_pass=%llu data_pass=%llu\n", (unsigned long long) error_pass, (unsigned long long) data_pass );
    fflush( stdout );
    rocketnet_check( error_pass != 0 );
    rocketnet_check( data_pass == error_pass );

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

// on linux every cell is a static case of its own (the three base names and
// the xdp cells in functional/rocketnet_functional_tests.cpp); the generated
// per-cell table is macos's, so this table is empty here.
int rocketnet_posix_hard_killed_peer_test_count()
{
    return 0;
}

const char * rocketnet_posix_hard_killed_peer_test_name( int )
{
    return "";
}

void rocketnet_posix_hard_killed_peer_test_run( int )
{
}

int rocketnet_posix_hard_killed_peer_test_group( const char * )
{
    return 0;
}

#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

int rocketnet_posix_hard_killed_child_main( int, char ** )
{
    return 2;
}

int rocketnet_posix_hard_killed_peer_test_count()
{
    return 0;
}

const char * rocketnet_posix_hard_killed_peer_test_name( int )
{
    return "";
}

void rocketnet_posix_hard_killed_peer_test_run( int )
{
}

int rocketnet_posix_hard_killed_peer_test_group( const char * )
{
    return 0;
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
