/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    A hard-killed client or an ICMP error never stalls or degrades a receive
    (TRANSPORTS.md, ICMP errors). A UDP send to a vanished client comes back as
    ICMP port unreachable; a BSD stack reports it to the next receive on a
    connected socket as ECONNREFUSED. These cases run per server transport
    cell (macOS: portable and msgx; Linux: portable, gso and io_uring),
    per address family, per client transport (macOS: portable and msgx;
    Linux: portable, the one client transport there):

      posix_hard_killed_client  one of four clients runs in a child process
                                (a child mode of this binary started by
                                posix_spawn) and is killed with SIGKILL
                                mid-stream; the server keeps sending to it,
                                the other three continue bidirectional receive
                                progress, the dead slot times out and frees,
                                and a fifth connects.
      posix_send_to_closed_port the server sends 1000 datagrams to a port no
                                socket holds while live clients exchange
                                traffic; no stall, no disconnect, no pause.
      posix_client_server_gone  a client sends to a closed server port, then a
                                server comes up on it and the client connects.

    and the receive seam cases inject each errno into the next recvmsg_x and
    recvmsg return and prove the drain continues and msgx stays up (macOS
    only: the seam is the msgx arm's).

    What a cell proves on Linux. Linux reports ICMP port unreachable to an
    unconnected UDP socket only when IP_RECVERR is set, and no server socket
    sets it: every server socket of the portable, gso and io_uring transports
    is unconnected, so the kernel takes each ICMP answer to a send to the
    killed child or the closed port and reports nothing to the server. The
    server cells there prove that sends drawing ICMP port unreachable (to a
    SIGKILLed client mid-stream, 1000 to a closed port, the closed-port sends
    staged on the transport's own sockets) never stall or degrade the other
    clients, that the dead slot times out and frees by the normal timeout,
    and that a fifth client connects. The client socket is connected, and
    Linux reports ICMP errors to a connected UDP socket without IP_RECVERR:
    posix_client_server_gone proves the portable client's receive takes each
    ECONNREFUSED as weather and connects once the server comes up. The io_uring
    cells register only where rocketnet_server_uring_available() is true.

    The xdp cell (Linux, ROCKETNET_XDP) runs the same three kinds, IPv4 and
    IPv6, with the portable client. It attaches XDP and makes a network
    namespace and a veth pair, so it is registered only where the xdp
    lifecycle cells are (rocketnet_test_posix_xdp_offered: root inside a
    virtme-ng guest, or root with ROCKETNET_TEST_XDP_VETH=1), as
    functional/posix_lifecycle_stress.cpp's xdp cells and
    rocketnet_test_xdp_veth_runs_here gate theirs. Each case makes its own
    pair (rnxdphk<n>, n = kind * 2 + family, so cases running side by side
    never share one): the server binds the near end on a fixed port, and every
    client, the SIGKILLed child included, opens its socket in the far end's
    namespace (the child is spawned from inside it). A send to the closed port
    goes out of the server's own AF_XDP queue to a port no socket holds on the
    far end, which answers ICMP port unreachable; that address is learned from
    no accepted packet, and the pair has no gateway, so the closed-port case
    names the far end's MAC as ROCKETNET_GATEWAY_ETHERNET_ADDRESS (the reply
    MAC rule, sdk/rocketnet_xdp_frame.h) for its server's create. The pair is
    removed at the case's end and at exit.

    The macOS and Linux cells live here; the file builds on every POSIX
    platform, and other platforms register no cells of their own here.
*/

#include "../tests/native_shutdown.h"
#include "../tests/time_scale.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#include <mach-o/dyld.h>
#else
#include <sys/random.h>
#include "rocketnet_server_gso.h"
#include "rocketnet_server_uring.h"
#if ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#include <fcntl.h>
#include <sched.h>
#include <sys/vfs.h>
#endif
#endif

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
#include "../tests/counters_expect.h"

// The server's update interval and the original receive-gap reference: one
// interval plus 50 ms. Keep reporting it for comparison, but do not make it a
// correctness bound: the portable server now hands send and receive work to
// background threads, whose scheduling latency is not an API guarantee.

static const double POSIX_HK_TICK = 1.0 / 60.0;
static const double POSIX_HK_GAP_REFERENCE = POSIX_HK_TICK + 0.050;

// Completion guard for connecting the test client and server.
static const double POSIX_HK_HANDSHAKE_SECONDS = 5.0;

#define POSIX_HK_CHILD_FLAG "--posix-hard-killed-child"

struct posix_hk_cell_t
{
    const char * server;        // the server transport: portable | msgx (macOS), portable | gso | io_uring (Linux)
    bool ipv6;
    const char * client;        // the client transport: portable | msgx (macOS), portable (Linux)
    int xdp_pair;               // the xdp cell's veth pair, kind * 2 + family; -1 on every other cell
};

static bool posix_hk_is_xdp( const posix_hk_cell_t & cell )
{
    return cell.xdp_pair >= 0;
}

// ---------------------------------------------------------------------------
// the xdp cell's veth pair: the server on the near end, the clients in the
// far end's namespace

#define POSIX_HK_XDP_PORT 40430

static void posix_hk_xdp_host( const posix_hk_cell_t & cell, bool far_end, char * out, size_t bytes )
{
    if ( cell.ipv6 )
        snprintf( out, bytes, "2001:db8:%x::%d", (unsigned) ( 0x90 + cell.xdp_pair ), far_end ? 2 : 1 );
    else
        snprintf( out, bytes, "10.77.%d.%d", 10 + cell.xdp_pair, far_end ? 2 : 1 );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

static int posix_hk_xdp_made = -1;          // the pair made in this process, or -1
static char posix_hk_xdp_netns[32] = "";    // the far end's namespace while a pair is up

static void posix_hk_xdp_names( int pair, char * netns, char * near_end, char * far_end, size_t bytes )
{
    snprintf( netns, bytes, "rnxdphk%d", pair );
    snprintf( near_end, bytes, "rnxdphk%d0", pair );
    snprintf( far_end, bytes, "rnxdphk%d1", pair );
}

static void posix_hk_xdp_remove( int pair )
{
    char netns[32], near_end[32], far_end[32];
    posix_hk_xdp_names( pair, netns, near_end, far_end, sizeof(netns) );
    char command[256];
    // deleting the namespace takes the far end, and with it the pair
    snprintf( command, sizeof(command), "ip netns del %s 2>/dev/null; ip link del %s 2>/dev/null", netns, near_end );
    if ( system( command ) != 0 )
    {
        // already gone
    }
}

static void posix_hk_xdp_teardown()
{
    if ( posix_hk_xdp_made >= 0 )
    {
        posix_hk_xdp_remove( posix_hk_xdp_made );
        posix_hk_xdp_made = -1;
        posix_hk_xdp_netns[0] = '\0';
    }
    rocketnet_server_socket_forbid_commit();
}

static void posix_hk_xdp_run( const char * command )
{
    const int result = system( command );
    if ( result != 0 )
        printf( "posix hard killed command failed (%d): %s\n", result, command );
    posix_hard_killed_require( result == 0 );
}

static void posix_hk_xdp_setup( const posix_hk_cell_t & cell )
{
    if ( posix_hk_xdp_made == cell.xdp_pair )
        return;
    static bool registered = false;
    if ( !registered )
    {
        atexit( posix_hk_xdp_teardown );
        registered = true;
    }
    if ( posix_hk_xdp_made >= 0 )
        posix_hk_xdp_teardown();

    // a guest boots with an empty bpffs mount point
    struct statfs bpffs;
    if ( statfs( "/sys/fs/bpf", &bpffs ) != 0 || (uint64_t) bpffs.f_type != (uint64_t) 0xcafe4a11 )
        posix_hk_xdp_run( "mount -t bpf bpf /sys/fs/bpf" );

    posix_hk_xdp_remove( cell.xdp_pair );

    char netns[32], near_end[32], far_end[32];
    posix_hk_xdp_names( cell.xdp_pair, netns, near_end, far_end, sizeof(netns) );
    char near_host[64], far_host[64];
    posix_hk_xdp_host( cell, false, near_host, sizeof(near_host) );
    posix_hk_xdp_host( cell, true, far_host, sizeof(far_host) );

    posix_hk_xdp_made = cell.xdp_pair;
    char command[512];
    snprintf( command, sizeof(command), "ip netns add %s", netns );
    posix_hk_xdp_run( command );
    snprintf( command, sizeof(command), "ip link add %s mtu 1500 type veth peer name %s mtu 1500", near_end, far_end );
    posix_hk_xdp_run( command );
    snprintf( command, sizeof(command), "ip link set %s netns %s", far_end, netns );
    posix_hk_xdp_run( command );
    if ( cell.ipv6 )
    {
        // no link-local address and no duplicate address detection: neighbor
        // discovery runs between the two global addresses at once
        snprintf( command, sizeof(command), "sysctl -qw net.ipv6.conf.%s.addr_gen_mode=1", near_end );
        posix_hk_xdp_run( command );
        snprintf( command, sizeof(command), "ip netns exec %s sysctl -qw net.ipv6.conf.%s.addr_gen_mode=1", netns, far_end );
        posix_hk_xdp_run( command );
        snprintf( command, sizeof(command), "ip addr add %s/64 dev %s nodad", near_host, near_end );
        posix_hk_xdp_run( command );
        snprintf( command, sizeof(command), "ip -n %s addr add %s/64 dev %s nodad", netns, far_host, far_end );
        posix_hk_xdp_run( command );
    }
    else
    {
        snprintf( command, sizeof(command), "ip addr add %s/24 dev %s", near_host, near_end );
        posix_hk_xdp_run( command );
        snprintf( command, sizeof(command), "ip -n %s addr add %s/24 dev %s", netns, far_host, far_end );
        posix_hk_xdp_run( command );
    }
    snprintf( command, sizeof(command), "ip link set %s up", near_end );
    posix_hk_xdp_run( command );
    // the busy-poll pair the xdp pre-flight verifies
    snprintf( command, sizeof(command), "echo 2 > /sys/class/net/%s/napi_defer_hard_irqs", near_end );
    posix_hk_xdp_run( command );
    snprintf( command, sizeof(command), "echo 200000 > /sys/class/net/%s/gro_flush_timeout", near_end );
    posix_hk_xdp_run( command );
    snprintf( command, sizeof(command), "ip -n %s link set lo up", netns );
    posix_hk_xdp_run( command );
    snprintf( command, sizeof(command), "ip -n %s link set %s up", netns, far_end );
    posix_hk_xdp_run( command );

    snprintf( posix_hk_xdp_netns, sizeof(posix_hk_xdp_netns), "%s", netns );
    rocketnet_server_socket_allow_commit();
}

// the far end's MAC, xx:xx:xx:xx:xx:xx, into out (18 bytes)

static void posix_hk_xdp_far_mac( const posix_hk_cell_t & cell, char * out, size_t bytes )
{
    char netns[32], near_end[32], far_end[32];
    posix_hk_xdp_names( cell.xdp_pair, netns, near_end, far_end, sizeof(netns) );
    char command[256];
    snprintf( command, sizeof(command), "ip netns exec %s cat /sys/class/net/%s/address", netns, far_end );
    FILE * file = popen( command, "r" );
    posix_hard_killed_require( file );
    const bool read = fgets( out, (int) bytes, file ) != NULL;
    pclose( file );
    posix_hard_killed_require( read );
    out[strcspn( out, "\r\n" )] = '\0';
    posix_hard_killed_require( strlen( out ) == 17 );
}

// the calling thread enters the far end's namespace while a pair is up: a
// socket opened, or a process spawned, before the leave lives there.
// returns the descriptor the leave restores, or -1 with no pair up

static int posix_hk_far_enter()
{
    if ( posix_hk_xdp_netns[0] == '\0' )
        return -1;
    char path[96];
    snprintf( path, sizeof(path), "/var/run/netns/%s", posix_hk_xdp_netns );
    const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
    const int far_ns = open( path, O_RDONLY );
    posix_hard_killed_require( self_ns >= 0 && far_ns >= 0 );
    posix_hard_killed_require( setns( far_ns, CLONE_NEWNET ) == 0 );
    close( far_ns );
    return self_ns;
}

static void posix_hk_far_leave( int self_ns )
{
    if ( self_ns < 0 )
        return;
    posix_hard_killed_require( setns( self_ns, CLONE_NEWNET ) == 0 );
    close( self_ns );
}

#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

static int posix_hk_far_enter()
{
    return -1;
}

static void posix_hk_far_leave( int )
{
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

// the transports whose server owns one platform socket the case reads its
// live pump from; gso and io_uring own a socket group and no single socket

static bool posix_hk_server_has_socket( const posix_hk_cell_t & cell )
{
    return strcmp( cell.server, "portable" ) == 0 || strcmp( cell.server, "msgx" ) == 0;
}

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
    if ( posix_hk_is_xdp( cell ) )
    {
        char host[64];
        posix_hk_xdp_host( cell, false, host, sizeof(host) );
        if ( cell.ipv6 )
            snprintf( out, bytes, "[%s]:%d", host, port );
        else
            snprintf( out, bytes, "%s:%d", host, port );
        return;
    }
    if ( cell.ipv6 )
        snprintf( out, bytes, "[::1]:%d", port );
    else
        snprintf( out, bytes, "127.0.0.1:%d", port );
}

// the server is the cell's transport: by name always, and, on a transport
// with one platform socket, by that socket's live pump too

static void posix_hk_require_server_io( struct rocketnet_server_transport_t * server, const posix_hk_cell_t & cell )
{
    posix_hard_killed_require( strcmp( rocketnet_server_transport_io_name( server ), cell.server ) == 0 );
    if ( !posix_hk_server_has_socket( cell ) )
        return;
    struct rocketnet_platform_socket_t * socket = rocketnet_server_transport_platform_socket_for_testing( server );
    posix_hard_killed_require( socket );
    posix_hard_killed_require( strcmp( rocketnet_platform_socket_pump( socket ), posix_hk_expected_server_pump( cell ) ) == 0 );
}

// what the report lines name as the server's pump: the socket's live pump,
// or the transport's name where there is no single socket

static const char * posix_hk_server_pump( struct rocketnet_server_transport_t * server, const posix_hk_cell_t & cell )
{
    if ( !posix_hk_server_has_socket( cell ) )
        return rocketnet_server_transport_io_name( server );
    return rocketnet_platform_socket_pump( rocketnet_server_transport_platform_socket_for_testing( server ) );
}

// Create the cell server with its canonical transport pin.

static struct rocketnet_server_transport_t * posix_hk_server_create( const posix_hk_cell_t & cell, int port, int max_clients )
{
    posix_hk_setenv( "ROCKETNET_SERVER_TRANSPORT", cell.server );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;

    char address[64];
    if ( posix_hk_is_xdp( cell ) )
    {
        // the near end of the case's pair, on its fixed port; one queue, on
        // the pair's one channel
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
        posix_hk_xdp_setup( cell );
#endif
        posix_hk_server_address( cell, port == 0 ? POSIX_HK_XDP_PORT + cell.xdp_pair : port, address, sizeof(address) );
        config.num_xdp_queues = 1;
        config.num_os_cpus = 1;
    }
    else if ( port == 0 )
        snprintf( address, sizeof(address), "%s", cell.ipv6 ? "[::1]" : "127.0.0.1" );
    else
        posix_hk_server_address( cell, port, address, sizeof(address) );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, address, &config );
    posix_hard_killed_require( server );
    posix_hk_require_server_io( server, cell );
    return server;
}

static struct rocketnet_client_transport_t * posix_hk_client_create( const char * server_address, uint64_t client_id )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    posix_hard_killed_require( rocketnet_write_test_connect_token( server_address, client_id, (uint64_t) time( NULL ), token, sizeof(token) ) == ROCKETNET_STATUS_OK );
    // on the xdp cell the client's socket and threads open in the far end's
    // namespace
    const int self_ns = posix_hk_far_enter();
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
    posix_hk_far_leave( self_ns );
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
    rn_native_server_transport_stop_and_destroy( server );
}

static void posix_hk_client_disconnect_and_destroy( struct rocketnet_client_transport_t * client )
{
    rocketnet_client_transport_disconnect( client );
    rocketnet_counters_assert_conserved( rocketnet_client_transport_counters( client ) );
    rn_native_client_transport_stop_and_destroy( client );
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
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    uint32_t size = sizeof(buffer);
    if ( _NSGetExecutablePath( buffer, &size ) != 0 )
        return std::string();
    return std::string( buffer );
#else
    const ssize_t bytes = readlink( "/proc/self/exe", buffer, sizeof(buffer) - 1 );
    if ( bytes <= 0 )
        return std::string();
    buffer[bytes] = '\0';
    return std::string( buffer );
#endif
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
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
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
    // on the xdp cell the child is spawned from inside the far end's
    // namespace, so its socket opens there too
    const int self_ns = posix_hk_far_enter();
    const int spawned = posix_spawn( &child, self.c_str(), NULL, NULL, child_argv, environ );
    posix_hk_far_leave( self_ns );
    posix_hard_killed_require( spawned == 0 );

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
    for ( size_t i = 0; i < 3; i++ )
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

    uint64_t received_up_at_kill[3];
    uint64_t received_down_at_kill[3];
    for ( size_t i = 0; i < 3; i++ )
    {
        received_up_at_kill[i] = live[i].received_up;
        received_down_at_kill[i] = live[i].received_down;
    }

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
        posix_hard_killed_require( tick_start - killed_at < ROCKETNET_FUNCTIONAL_CASE_TARGET_SECONDS );
        posix_hk_sleep_rest_of_tick( tick_start );
    }

    const double timeout_seen = freed_at - killed_at;
    double worst_gap = 0.0;
    for ( size_t i = 0; i < 3; i++ )
    {
        if ( live[i].max_gap_up > worst_gap ) worst_gap = live[i].max_gap_up;
        if ( live[i].max_gap_down > worst_gap ) worst_gap = live[i].max_gap_down;
    }
    printf( "posix_hard_killed_client server=%s family=%s client=%s: %llu sends to the dead slot, slot freed %.2fs after the kill, worst live receive gap %.1f ms (reference %.1f ms), pump %s\n",
            cell.server, cell.ipv6 ? "ipv6" : "ipv4", cell.client, (unsigned long long) sent_to_slot[dead_slot], timeout_seen,
            worst_gap * 1000.0, POSIX_HK_GAP_REFERENCE * 1000.0, posix_hk_server_pump( server, cell ) );
    for ( size_t i = 0; i < 3; i++ )
        printf( "  live client %d slot %d: after kill up +%llu down +%llu, up gap %.1f ms down gap %.1f ms\n", (int) i, live[i].slot,
                (unsigned long long) ( live[i].received_up - received_up_at_kill[i] ),
                (unsigned long long) ( live[i].received_down - received_down_at_kill[i] ),
                live[i].max_gap_up * 1000.0, live[i].max_gap_down * 1000.0 );

    posix_hard_killed_require( sent_to_slot[dead_slot] > 0 );
    // Exact timeout boundaries are covered with simulated time in the unit tier.
    for ( size_t i = 0; i < 3; i++ )
    {
        posix_hard_killed_require( rocketnet_client_transport_state( live[i].client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        posix_hard_killed_require( live[i].received_up > received_up_at_kill[i] );
        posix_hard_killed_require( live[i].received_down > received_down_at_kill[i] );
    }
    posix_hk_require_server_io( server, cell );
    posix_hard_killed_require( rocketnet_server_transport_num_connected_clients( server ) == 3 );

    // a fifth client connects into the freed slot within its handshake time

    std::vector<posix_hk_live_t> fifth( 1 );
    posix_hk_live_init( fifth[0], posix_hk_client_create( server_address, 0x6D000ULL ) );
    const double fifth_start = rocketnet_platform_time();
    posix_hk_connect_all( server, fifth, POSIX_HK_HANDSHAKE_SECONDS );
    printf( "  fifth client connected in %.2fs into slot %d\n", rocketnet_platform_time() - fifth_start, fifth[0].slot );
    posix_hard_killed_require( rocketnet_server_transport_num_connected_clients( server ) == 4 );

    posix_hk_client_disconnect_and_destroy( fifth[0].client );
    for ( size_t i = 0; i < 3; i++ )
        posix_hk_client_disconnect_and_destroy( live[i].client );
    posix_hk_server_stop_and_destroy( server );
}

// a port no socket holds: bind one, close it. it is drawn below the
// ephemeral range (net.inet.ip.portrange.first, 49152 on macOS;
// net.ipv4.ip_local_port_range, 32768 by default on Linux), so a socket
// another case binds to port 0 is never granted it between this close and
// the bind posix_client_server_gone makes on it; an ephemeral port released
// here was, and the server's create then failed on a port in use.

static int posix_hk_closed_port( bool ipv6 )
{
    for ( int attempt = 0; attempt < 1000; attempt++ )
    {
        rocketnet_address_t address;
        rocketnet_address_parse( &address, ipv6 ? "[::1]" : "127.0.0.1" );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        address.port = (uint16_t) ( 20000 + arc4random_uniform( 20000 ) );
#else
        uint32_t draw = 0;
        posix_hard_killed_require( getrandom( &draw, sizeof(draw), 0 ) == (ssize_t) sizeof(draw) );
        address.port = (uint16_t) ( 20000 + draw % 12000 );
#endif
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

// count datagrams to the closed port, sent from the server's own sockets:
// the one platform socket of portable and msgx, or the socket group of gso
// and io_uring, staged through the transport instance's own start, finish
// and send phase on this, the main thread, between the transport's calls

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
void * rocketnet_server_transport_io_name_for_testing( struct rocketnet_server_transport_t * server_transport );
#endif

static void posix_hk_server_send_to( struct rocketnet_server_transport_t * server, const posix_hk_cell_t & cell, const rocketnet_address_t * to, const uint8_t * data, int bytes, int count )
{
    if ( posix_hk_server_has_socket( cell ) )
    {
        struct rocketnet_platform_socket_t * socket = rocketnet_server_transport_platform_socket_for_testing( server );
        for ( int i = 0; i < count; i++ )
            rocketnet_platform_socket_send_packet( socket, to, data, bytes );
        return;
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    void * instance = rocketnet_server_transport_io_name_for_testing( server );
    posix_hard_killed_require( instance );
#if ROCKETNET_XDP
    if ( posix_hk_is_xdp( cell ) )
    {
        struct rocketnet_server_socket_t * server_socket = (struct rocketnet_server_socket_t*) instance;
        for ( int i = 0; i < count; i++ )
        {
            uint64_t packet_id = 0;
            uint8_t * frame = rocketnet_server_socket_start_packet( server_socket, to, &packet_id );
            posix_hard_killed_require( frame );
            memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, data, (size_t) bytes );
            rocketnet_server_socket_finish_packet( server_socket, packet_id, frame, bytes );
        }
        rocketnet_server_socket_send_packets( server_socket );
        return;
    }
#endif
    const bool gso = strcmp( cell.server, "gso" ) == 0;
    posix_hard_killed_require( gso || strcmp( cell.server, "io_uring" ) == 0 );
    for ( int i = 0; i < count; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = gso ? rocketnet_server_gso_start_packet( (struct rocketnet_server_gso_t*) instance, to, &packet_id )
                              : rocketnet_server_uring_start_packet( (struct rocketnet_server_uring_t*) instance, to, &packet_id );
        posix_hard_killed_require( frame );
        memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, data, (size_t) bytes );
        if ( gso )
            rocketnet_server_gso_finish_packet( (struct rocketnet_server_gso_t*) instance, packet_id, frame, bytes );
        else
            rocketnet_server_uring_finish_packet( (struct rocketnet_server_uring_t*) instance, packet_id, frame, bytes );
    }
    if ( gso )
        rocketnet_server_gso_send_packets( (struct rocketnet_server_gso_t*) instance );
    else
        rocketnet_server_uring_send_packets( (struct rocketnet_server_uring_t*) instance );
#else
    (void) to; (void) data; (void) bytes; (void) count;
    posix_hard_killed_require( false );
#endif
}

static void posix_send_to_closed_port( const posix_hk_cell_t & cell )
{
    posix_hk_setenv( "ROCKETNET_CLIENT_TRANSPORT", cell.client );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    // the xdp server sends to the closed port, an address no accepted packet
    // taught it, through the far end's MAC, named as the gateway for this
    // create only (see the header)
    if ( posix_hk_is_xdp( cell ) )
    {
        posix_hk_xdp_setup( cell );
        char far_mac[32];
        posix_hk_xdp_far_mac( cell, far_mac, sizeof(far_mac) );
        posix_hk_setenv( "ROCKETNET_GATEWAY_ETHERNET_ADDRESS", far_mac );
    }
#endif
    struct rocketnet_server_transport_t * server = posix_hk_server_create( cell, 0, 8 );
    if ( posix_hk_is_xdp( cell ) )
        posix_hk_setenv( "ROCKETNET_GATEWAY_ETHERNET_ADDRESS", NULL );
    char server_address[64];
    posix_hk_server_address( cell, rocketnet_server_transport_port( server ), server_address, sizeof(server_address) );

    std::vector<posix_hk_live_t> live( 3 );
    for ( size_t i = 0; i < 3; i++ )
        posix_hk_live_init( live[i], posix_hk_client_create( server_address, 0x6E000ULL + (uint64_t) i ) );
    posix_hk_connect_all( server, live, POSIX_HK_HANDSHAKE_SECONDS );

    // let the handshake's own traffic settle, then count from zero

    posix_hk_quiesce( server, live );
    for ( size_t i = 0; i < 3; i++ )
    {
        live[i].sent_up = live[i].received_up = live[i].sent_down = live[i].received_down = 0;
        live[i].last_up = live[i].last_down = 0.0;
    }

    // the closed port: on loopback, or on the xdp cell the far end of its
    // pair, where only the clients' ephemeral ports are held
    rocketnet_address_t closed;
    if ( posix_hk_is_xdp( cell ) )
    {
        char far_host[64];
        posix_hk_xdp_host( cell, true, far_host, sizeof(far_host) );
        posix_hard_killed_require( rocketnet_address_parse( &closed, far_host ) );
    }
    else
        rocketnet_address_parse( &closed, cell.ipv6 ? "[::1]" : "127.0.0.1" );
    closed.port = (uint16_t) posix_hk_closed_port( cell.ipv6 );

    // 1000 datagrams to the closed port, 20 a tick, across 50 ticks of live
    // traffic, on the server's own sockets

    uint8_t junk[64];
    memset( junk, 0xAB, sizeof(junk) );
    int sent_closed = 0;
    while ( sent_closed < 1000 )
    {
        const double tick_start = rocketnet_platform_time();
        posix_hk_server_send_to( server, cell, &closed, junk, (int) sizeof(junk), 20 );
        sent_closed += 20;
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
    for ( size_t i = 0; i < 3; i++ )
    {
        if ( live[i].max_gap_up > worst_gap ) worst_gap = live[i].max_gap_up;
        if ( live[i].max_gap_down > worst_gap ) worst_gap = live[i].max_gap_down;
    }
    printf( "posix_send_to_closed_port server=%s family=%s client=%s: %d datagrams to closed port %d, worst live receive gap %.1f ms (reference %.1f ms), pump %s\n",
            cell.server, cell.ipv6 ? "ipv6" : "ipv4", cell.client, sent_closed, closed.port, worst_gap * 1000.0, POSIX_HK_GAP_REFERENCE * 1000.0,
            posix_hk_server_pump( server, cell ) );
    for ( size_t i = 0; i < 3; i++ )
        printf( "  live client %d slot %d: up %llu/%llu down %llu/%llu\n", (int) i, live[i].slot,
                (unsigned long long) live[i].received_up, (unsigned long long) live[i].sent_up,
                (unsigned long long) live[i].received_down, (unsigned long long) live[i].sent_down );

    for ( size_t i = 0; i < 3; i++ )
    {
        posix_hard_killed_require( rocketnet_client_transport_state( live[i].client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        posix_hard_killed_require( live[i].sent_up > 0 && live[i].received_up == live[i].sent_up );
        posix_hard_killed_require( live[i].sent_down > 0 && live[i].received_down == live[i].sent_down );
    }
    posix_hard_killed_require( rocketnet_server_transport_num_connected_clients( server ) == 3 );
    posix_hk_require_server_io( server, cell );

    for ( size_t i = 0; i < 3; i++ )
        posix_hk_client_disconnect_and_destroy( live[i].client );
    posix_hk_server_stop_and_destroy( server );
}

static void posix_client_server_gone( const posix_hk_cell_t & cell )
{
    posix_hk_setenv( "ROCKETNET_CLIENT_TRANSPORT", cell.client );
    // on the xdp cell the pair comes up first, with no server on it, and the
    // port is the cell's fixed one on the near end, which no socket holds
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    if ( posix_hk_is_xdp( cell ) )
        posix_hk_xdp_setup( cell );
#endif
    const int port = posix_hk_is_xdp( cell ) ? POSIX_HK_XDP_PORT + cell.xdp_pair : posix_hk_closed_port( cell.ipv6 );
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
    posix_hk_require_server_io( server, cell );

    posix_hk_client_disconnect_and_destroy( live[0].client );
    posix_hk_server_stop_and_destroy( server );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// ---------------------------------------------------------------------------
// the receive seam: each errno injected into the next recvmsg_x (the msgx
// arm) and recvmsg (the portable arm) return; macOS only, since the seam is
// the msgx arm's and SO_NUMRCVPKT is Darwin's

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

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    rocketnet_log_function( posix_seam_log, NULL );

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

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// ---------------------------------------------------------------------------
// the case table: three case kinds per cell, then the seam cases (macOS)

static const char * posix_hk_kinds[] = { "test_posix_hard_killed_client", "test_posix_send_to_closed_port", "test_posix_client_server_gone" };

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

static const char * posix_hk_server_names[] = { "portable", "msgx" };
static const char * posix_hk_client_names[] = { "portable", "msgx" };

struct posix_seam_case_t
{
    const char * name;
    int error;
};

static const posix_seam_case_t posix_seam_cases[] =
{
    { "test_posix_receive_seam_econnrefused", ECONNREFUSED },
    { "test_posix_receive_seam_ehostunreach", EHOSTUNREACH },
    { "test_posix_receive_seam_enetunreach", ENETUNREACH },
    { "test_posix_receive_seam_ehostdown", EHOSTDOWN },
    { "test_posix_receive_seam_enetdown", ENETDOWN },
    { "test_posix_receive_seam_enobufs", ENOBUFS },
    { "test_posix_receive_seam_eagain", EAGAIN },
    { "test_posix_receive_seam_eintr", EINTR },
    { "test_posix_receive_seam_unknown_errno_pauses_msgx", 0 },
};

#define POSIX_HK_NUM_SEAM_CASES ( (int) ( sizeof(posix_seam_cases) / sizeof(posix_seam_cases[0]) ) )

#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// the server transports in registration order; posix_hk_servers below keeps
// the ones this host offers

static const char * posix_hk_server_names[] = { "portable", "gso", "io_uring", "xdp" };
static const char * posix_hk_client_names[] = { "portable" };

#define POSIX_HK_NUM_SEAM_CASES 0

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#define POSIX_HK_MAX_SERVERS ( (int) ( sizeof(posix_hk_server_names) / sizeof(posix_hk_server_names[0]) ) )
#define POSIX_HK_NUM_CLIENTS ( (int) ( sizeof(posix_hk_client_names) / sizeof(posix_hk_client_names[0]) ) )
#define POSIX_HK_MAX_CELL_CASES ( 3 * POSIX_HK_MAX_SERVERS * 2 * POSIX_HK_NUM_CLIENTS )

// the server transports registered on this host: every name, except
// io_uring where rocketnet_server_uring_available() is false, and xdp where
// rocketnet_test_posix_xdp_offered() is false (root inside a virtme-ng
// guest, or root with ROCKETNET_TEST_XDP_VETH=1), as the xdp lifecycle
// cells are gated

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
bool rocketnet_test_posix_xdp_offered();
#endif

static const char * posix_hk_servers[POSIX_HK_MAX_SERVERS];

static int posix_hk_num_servers()
{
    static int count = -1;
    if ( count >= 0 )
        return count;
    count = 0;
    for ( int i = 0; i < POSIX_HK_MAX_SERVERS; i++ )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
        if ( strcmp( posix_hk_server_names[i], "io_uring" ) == 0 && !rocketnet_server_uring_available() )
            continue;
        if ( strcmp( posix_hk_server_names[i], "xdp" ) == 0 && !rocketnet_test_posix_xdp_offered() )
            continue;
#endif
        posix_hk_servers[count++] = posix_hk_server_names[i];
    }
    return count;
}

static int posix_hk_num_cell_cases()
{
    return 3 * posix_hk_num_servers() * 2 * POSIX_HK_NUM_CLIENTS;
}

static char posix_hk_names[POSIX_HK_MAX_CELL_CASES][96];

static void posix_hk_cell_of( int index, int * kind, posix_hk_cell_t * cell )
{
    // index = ( ( kind * servers + server ) * 2 + family ) * clients + client
    const int servers = posix_hk_num_servers();
    const int client = index % POSIX_HK_NUM_CLIENTS;
    const int family = ( index / POSIX_HK_NUM_CLIENTS ) % 2;
    const int server = ( index / ( 2 * POSIX_HK_NUM_CLIENTS ) ) % servers;
    *kind = index / ( 2 * POSIX_HK_NUM_CLIENTS * servers );
    cell->server = posix_hk_servers[server];
    cell->ipv6 = family == 1;
    cell->client = posix_hk_client_names[client];
    cell->xdp_pair = strcmp( cell->server, "xdp" ) == 0 ? *kind * 2 + family : -1;
}

int rocketnet_posix_hard_killed_client_test_count()
{
    return posix_hk_num_cell_cases() + POSIX_HK_NUM_SEAM_CASES;
}

const char * rocketnet_posix_hard_killed_client_test_name( int index )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    if ( index >= posix_hk_num_cell_cases() )
        return posix_seam_cases[index - posix_hk_num_cell_cases()].name;
#endif
    int kind = 0;
    posix_hk_cell_t cell;
    posix_hk_cell_of( index, &kind, &cell );
    snprintf( posix_hk_names[index], sizeof(posix_hk_names[index]), "%s_%s_%s_client_%s", posix_hk_kinds[kind], cell.server, cell.ipv6 ? "ipv6" : "ipv4", cell.client );
    return posix_hk_names[index];
}

// the suite group (tests/suite_runner.h, group_of) of a case name: the cell
// cases measure receive gaps and handshake times against the wall clock, so
// they run as group 1, after every other case and the sibling executables
// (group 0) have finished, never beside the loopback load those make; they
// run in parallel with each other. the seam cases measure no time.
//
// the 1000-cycle connect/disconnect cases (functional/posix_lifecycle_stress.cpp)
// hold 1024 IPv4 loopback ports while they cycle, and Darwin gives a
// dual-stack IPv6 socket in another process a port one of them holds: that
// socket then never hears IPv4 datagrams to it. Beside them,
// test_transport_dual_stack_ipv4_client and test_server_serves_both_families
// failed to connect. On macOS they run as group 2, alone, after group 1.

int rocketnet_posix_hard_killed_client_test_group( const char * name )
{
    for ( size_t i = 0; i < 3; i++ )
        if ( strncmp( name, posix_hk_kinds[i], strlen( posix_hk_kinds[i] ) ) == 0 )
            return 1;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    if ( strncmp( name, "test_posix_connect_disconnect_1000", strlen( "test_posix_connect_disconnect_1000" ) ) == 0 )
        return 2;
#endif
    return 0;
}

void rocketnet_posix_hard_killed_client_test_run( int index )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    if ( index >= posix_hk_num_cell_cases() )
    {
        const posix_seam_case_t & seam = posix_seam_cases[index - posix_hk_num_cell_cases()];
        if ( seam.error == 0 )
            posix_seam_unknown_errno_pauses_msgx();
        else
            posix_seam_weather( seam.error );
        return;
    }
#endif
    int kind = 0;
    posix_hk_cell_t cell;
    posix_hk_cell_of( index, &kind, &cell );
    if ( kind == 0 )
        posix_hard_killed_client( cell );
    else if ( kind == 1 )
        posix_send_to_closed_port( cell );
    else
        posix_client_server_gone( cell );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    if ( posix_hk_is_xdp( cell ) )
        posix_hk_xdp_teardown();
#endif
}

#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

int rocketnet_posix_hard_killed_child_main( int, char ** )
{
    return 2;
}

int rocketnet_posix_hard_killed_client_test_count()
{
    return 0;
}

const char * rocketnet_posix_hard_killed_client_test_name( int )
{
    return "";
}

void rocketnet_posix_hard_killed_client_test_run( int )
{
}

int rocketnet_posix_hard_killed_client_test_group( const char * )
{
    return 0;
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
