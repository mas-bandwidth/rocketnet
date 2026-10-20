#include <new>
/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"

#if ROCKETNET_XDP

#include "rocketnet_server_socket.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"
#include "rocketnet_xdp_frame.h"
#include "rocketnet_driver.h"
#include "rocketnet_hash.h"

#include <ifaddrs.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>   // mkdtemp (POSIX declares it in stdlib.h; unistd.h is the portable belt-and-braces)

#include <rocketnet_server_xdp.h>
#include "rocketnet_pending_frames.h"
#include "rocketnet_send_handoff.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_xdp_replay_marks.h"
#include "rocketnet_memory.h"
#include "rocketnet_xdp_free_list.h"
#include "rocketnet_xdp_socket_config.h"

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <atomic>

// XDP umem frame size must match libxdp expectation
static_assert( ROCKETNET_XDP_FRAME_SIZE == 4096, "ROCKETNET_XDP_FRAME_SIZE must be 4096" );

struct alignas(64) rocketnet_server_socket_send_packet_info_t
{
    rocketnet_address_t to;
    size_t packet_bytes;
    uint8_t destination_mac[ETH_ALEN];  // chosen at start by the reply-MAC rule
};

// IMPORTANT: The 4096 byte padding walls in these structs are load-bearing. Each field
// between walls is touched by a different thread, and without the walls those fields
// would share cache lines and false sharing would destroy multithreaded send and
// receive performance. Nothing goes wrong if you remove them -- it just gets slow.
// Do not remove them.

struct rocketnet_server_socket_send_buffer_t
{
    uint8_t padding_0[4096];

    std::atomic<int> num_packets{0};

    uint8_t padding_1[4096];

    int packet_start_index;

    uint8_t padding_2[4096];

    rocketnet_server_socket_send_packet_info_t packet_info[ROCKETNET_XDP_SEND_QUEUE_SIZE];

    uint8_t padding_3[4096];

    alignas(16) uint8_t packet_data[ROCKETNET_XDP_SEND_QUEUE_SIZE][ROCKETNET_XDP_FRAME_SIZE];

    uint8_t padding_4[4096];
};

struct rocketnet_server_socket_receive_buffer_t
{
    uint8_t padding_0[4096];

    std::atomic<bool> ready{0};

    uint8_t padding_1[4096];

    uint32_t num_packets;
    uint8_t eth[ROCKETNET_XDP_RECV_QUEUE_SIZE][ETH_ALEN];
    rocketnet_address_t from[ROCKETNET_XDP_RECV_QUEUE_SIZE];
    size_t packet_bytes[ROCKETNET_XDP_RECV_QUEUE_SIZE];
    uint8_t packet_data[ROCKETNET_XDP_RECV_QUEUE_SIZE][ROCKETNET_XDP_FRAME_SIZE];

    uint8_t padding_2[4096];
};

struct rocketnet_server_xdp_socket_t
{
    uint8_t padding_0[4096];

    int queue;
    int num_os_cpus;

    uint8_t padding_1[4096];

    void * buffer;
    struct xsk_umem * umem;
    struct xsk_ring_cons receive_queue;
    struct xsk_ring_prod send_queue;
    struct xsk_ring_cons complete_queue;
    struct xsk_ring_prod fill_queue;
    struct xsk_socket * xsk;
    bool zero_copy;

    uint8_t padding_2[4096];

    std::atomic<bool> quit{0};

    uint8_t padding_4[4096];

    uint64_t receive_counter;

    // frames the receive loop dropped, by reason (rocketnet_xdp_frame_receive);
    // written by this queue's xdp thread under receive_mutex
    struct rocketnet_xdp_receive_drops_t receive_drops;

    // frames the flush did not send because the codec refused to build them
    // (a destination of the family this server does not serve); written by
    // this queue's xdp thread under send_mutex
    uint64_t send_refused_build;

    uint8_t padding_5[4096];

    rocketnet_send_handoff_t<ROCKETNET_XDP_NUM_BUFFERS> send_handoff;

    uint8_t padding_6[4096];

    rocketnet_platform_thread_t * xdp_thread;

    rocketnet_platform_mutex_t send_mutex;
    rocketnet_platform_mutex_t receive_mutex;

    int num_free_receive_frames;
    uint64_t receive_frames[ROCKETNET_XDP_NUM_FRAMES/2];

    int num_free_send_frames;
    uint64_t send_frames[ROCKETNET_XDP_NUM_FRAMES/2];
    std::atomic<int> send_flow_index{-1};

    uint8_t server_ethernet_address[ETH_ALEN];
    uint8_t gateway_ethernet_address[ETH_ALEN];
    rocketnet_address_t server_address;     // the public address and port (host-order port): its family is the family this socket serves

    struct rocketnet_server_socket_receive_buffer_t receive_buffer[ROCKETNET_XDP_NUM_BUFFERS];

    struct rocketnet_server_socket_send_buffer_t send_buffer[ROCKETNET_XDP_NUM_BUFFERS];

    uint8_t padding_7[4096];
};

struct rocketnet_server_socket_t
{
    void * context;
    int state;
    rocketnet_address_t public_address;
    uint64_t server_id;
    uint64_t match_id;

    uint8_t server_ethernet_address[ETH_ALEN];
    uint8_t gateway_ethernet_address[ETH_ALEN];
    bool gateway_known;

    // the reply-MAC rule (rocketnet_xdp_frame.h): learned from the source MAC
    // of each packet the transport accepts for a connection (peer_accepted),
    // written on the main thread while the transport processes a receive
    // view, read by start_packet; fill threads only read it, and they are
    // joined before each receive
    struct rocketnet_xdp_reply_macs_t reply_macs;
    uint8_t process_eth[ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS][ETH_ALEN];

    int interface_index;
    struct xdp_program * program;
    bool attached_native;
    bool attached_skb;
    char pin_root[160];                 // this INSTANCE's own bpffs directory, named by a random token with its identity metadata encoded: created by this create, adopted never
    bool pin_root_created;              // set only when this instance's mkdir succeeded -- destroy removes nothing it did not make
    int num_pinned;                     // the pin paths recorded BY CONSTRUCTION at mkdir (their names are deterministic) -- destroy unlinks exactly these
    char pinned_paths[4][200];
    int config_map_fd;
    int socket_map_fd;
    int stats_map_fd;
    int replay_map_fd;
    struct rocketnet_xdp_replay_marks_t * replay_marks;     // pending forgets and marks, this socket owns it
    uint64_t replay_commit_failures;

    std::atomic<uint64_t> packet_id{0};

    int num_queues;
    rocketnet_server_xdp_socket_t * socket;

    rocketnet_server_socket_process_packets_t process_packets;
};

void rocketnet_server_socket_destroy( rocketnet_server_socket_t * server );

static bool get_interface_mac_address( const char * interface_name, uint8_t * mac_address ) 
{
    char path[256];
    snprintf( path, sizeof(path), "/sys/class/net/%s/address", interface_name );
    FILE * file = fopen(path, "r");
    if ( !file )
    {
        return false;
    }

    // xx.xx.xx.xx.xx.xx

    char mac_address_string[18];

    if ( fgets( mac_address_string, sizeof(mac_address_string), file ) == NULL ) 
    {
        fclose( file );
        return false;
    }

    mac_address_string[2] = 0;
    mac_address_string[5] = 0;
    mac_address_string[8] = 0;
    mac_address_string[11] = 0;
    mac_address_string[14] = 0;
    mac_address_string[17] = 0;

    mac_address[0] = (uint8_t) strtol( mac_address_string + 0, NULL, 16 );
    mac_address[1] = (uint8_t) strtol( mac_address_string + 3, NULL, 16 );
    mac_address[2] = (uint8_t) strtol( mac_address_string + 6, NULL, 16 );
    mac_address[3] = (uint8_t) strtol( mac_address_string + 9, NULL, 16 );
    mac_address[4] = (uint8_t) strtol( mac_address_string + 12, NULL, 16 );
    mac_address[5] = (uint8_t) strtol( mac_address_string + 15, NULL, 16 );

    fclose( file );

    return true;
}

// an interface name goes into an ip(8) command line below, so it is held to
// the characters interface names use; anything else refuses

static bool rocketnet_server_socket_interface_name_is_plain( const char * interface_name )
{
    if ( !interface_name || interface_name[0] == '\0' || strlen( interface_name ) >= IFNAMSIZ )
        return false;
    for ( const char * c = interface_name; *c; c++ )
    {
        const bool plain = ( *c >= 'a' && *c <= 'z' ) || ( *c >= 'A' && *c <= 'Z' ) || ( *c >= '0' && *c <= '9' ) ||
                           *c == '-' || *c == '_' || *c == '.' || *c == '@';
        if ( !plain )
            return false;
    }
    return true;
}

// the IPv6 gateway, the twin of the IPv4 discovery below: the default route's
// next hop on this interface from `ip -6 route show default dev <if>`, then
// its link-layer address from `ip -6 neigh show dev <if>`, matched on the
// whole first token (one IPv6 address is a substring of others). No default
// route on the interface returns true with *found_route false, as IPv4 does;
// an unparseable next hop or no neighbor entry with an lladdr refuses.

static bool get_gateway_mac_address_ipv6( const char * interface_name, char * mac_address_string, bool * found_route )
{
    *found_route = false;

    if ( !rocketnet_server_socket_interface_name_is_plain( interface_name ) )
    {
        rocketnet_error( "ipv6 gateway discovery: interface name is not plain" );
        return false;
    }

    char command[256];
    snprintf( command, sizeof(command), "ip -6 route show default dev %s 2>/dev/null", interface_name );

    char gateway_ip_string[INET6_ADDRSTRLEN + 1];
    gateway_ip_string[0] = '\0';

    FILE * file = popen( command, "r" );
    if ( !file )
        return false;
    char route_buffer[1024];
    while ( fgets( route_buffer, sizeof(route_buffer), file ) != NULL )
    {
        if ( strncmp( route_buffer, "default ", 8 ) != 0 )
            continue;
        const char * via = strstr( route_buffer, " via " );
        if ( !via )
            continue;
        via += 5;
        const size_t length = strcspn( via, " \t\r\n" );
        if ( length == 0 || length > INET6_ADDRSTRLEN )
            continue;
        memcpy( gateway_ip_string, via, length );
        gateway_ip_string[length] = '\0';
        break;
    }
    pclose( file );

    if ( gateway_ip_string[0] == '\0' )
        return true;

    struct in6_addr gateway_address;
    if ( inet_pton( AF_INET6, gateway_ip_string, &gateway_address ) != 1 )
    {
        rocketnet_error( "ipv6 gateway discovery: next hop %s is not an ipv6 address", gateway_ip_string );
        return false;
    }

    *found_route = true;

    snprintf( command, sizeof(command), "ip -6 neigh show dev %s 2>/dev/null", interface_name );

    bool found_mac_address = false;

    file = popen( command, "r" );
    if ( !file )
        return false;
    char neigh_buffer[1024];
    while ( fgets( neigh_buffer, sizeof(neigh_buffer), file ) != NULL )
    {
        const size_t length = strcspn( neigh_buffer, " \t\r\n" );
        if ( length != strlen( gateway_ip_string ) || memcmp( neigh_buffer, gateway_ip_string, length ) != 0 )
            continue;
        const char * lladdr = strstr( neigh_buffer, " lladdr " );
        if ( !lladdr )
            continue;
        lladdr += 8;
        if ( strcspn( lladdr, " \t\r\n" ) != 17 )
            continue;
        memcpy( mac_address_string, lladdr, 17 );
        mac_address_string[17] = 0;
        found_mac_address = true;
        break;
    }
    pclose( file );

    if ( !found_mac_address )
    {
        rocketnet_error( "ipv6 gateway discovery: no neighbor entry with a link-layer address for %s on %s", gateway_ip_string, interface_name );
        return false;
    }

    return true;
}

// the next whitespace-separated token of a command's output line, copied
// into token: false at the end of the line, or when the token does not fit
// (no column the IPv4 gateway parsers read is that long)

static bool rocketnet_server_socket_next_token( const char ** cursor, char * token, size_t token_size )
{
    const char * start = *cursor + strspn( *cursor, " \t\r\n" );
    const size_t length = strcspn( start, " \t\r\n" );
    *cursor = start + length;
    if ( length == 0 || length >= token_size )
        return false;
    memcpy( token, start, length );
    token[length] = '\0';
    return true;
}

// the IPv4 gateway from one `netstat -rn` line, pure string logic over the
// command's text (test_unit_xdp_gateway_parse.cpp drives it). The line is
// accepted only when it is the default route on this interface, every column
// compared as a whole token: Destination and Genmask are 0.0.0.0, Flags
// holds U and G, and the last column (Iface) is the interface name. Its
// Gateway column is copied out. A non-default UG route, a longer interface
// name that contains this one (eth10 for eth1) and the header lines refuse.

bool rocketnet_server_socket_ipv4_route_line_gateway( const char * line, const char * interface_name, char * gateway_ip_string, size_t gateway_ip_size )
{
    rocketnet_assert( line );
    rocketnet_assert( interface_name );
    rocketnet_assert( gateway_ip_string );

    // Destination Gateway Genmask Flags MSS Window irtt Iface

    char columns[8][IFNAMSIZ + INET_ADDRSTRLEN];
    const char * cursor = line;
    for ( int i = 0; i < 8; i++ )
    {
        if ( !rocketnet_server_socket_next_token( &cursor, columns[i], sizeof(columns[i]) ) )
            return false;
    }
    if ( cursor[strspn( cursor, " \t\r\n" )] != '\0' )
        return false;

    if ( strcmp( columns[0], "0.0.0.0" ) != 0 || strcmp( columns[2], "0.0.0.0" ) != 0 )
        return false;
    if ( !strchr( columns[3], 'U' ) || !strchr( columns[3], 'G' ) )
        return false;
    if ( strcmp( columns[7], interface_name ) != 0 )
        return false;
    if ( strlen( columns[1] ) >= gateway_ip_size )
        return false;

    strcpy( gateway_ip_string, columns[1] );
    return true;
}

// the gateway's link-layer address from one `ip neigh show` line, pure
// string logic (test_unit_xdp_gateway_parse.cpp drives it), as the IPv6
// discovery above matches its neighbor: the first token is the gateway
// address as a whole token (10.0.0.12 never answers for 10.0.0.1), the token
// after "dev" is the interface name as a whole token, and the token after
// "lladdr" is 17 characters long, copied into mac_address_string (18 bytes).
// A line with no lladdr (FAILED, INCOMPLETE) refuses.

bool rocketnet_server_socket_ipv4_neigh_line_mac( const char * line, const char * gateway_ip_string, const char * interface_name, char * mac_address_string )
{
    rocketnet_assert( line );
    rocketnet_assert( gateway_ip_string );
    rocketnet_assert( interface_name );
    rocketnet_assert( mac_address_string );

    char token[INET6_ADDRSTRLEN + 1];
    const char * cursor = line;
    if ( !rocketnet_server_socket_next_token( &cursor, token, sizeof(token) ) || strcmp( token, gateway_ip_string ) != 0 )
        return false;

    bool interface_matches = false;
    char mac[18];
    mac[0] = '\0';
    while ( rocketnet_server_socket_next_token( &cursor, token, sizeof(token) ) )
    {
        if ( strcmp( token, "dev" ) == 0 )
        {
            if ( !rocketnet_server_socket_next_token( &cursor, token, sizeof(token) ) )
                return false;
            interface_matches = strcmp( token, interface_name ) == 0;
        }
        else if ( strcmp( token, "lladdr" ) == 0 )
        {
            if ( !rocketnet_server_socket_next_token( &cursor, token, sizeof(token) ) || strlen( token ) != 17 )
                return false;
            memcpy( mac, token, 18 );
        }
    }

    if ( !interface_matches || mac[0] == '\0' )
        return false;

    memcpy( mac_address_string, mac, 18 );
    return true;
}

// false refuses create (a malformed, zero or multicast MAC, or a discovery
// that could not complete). true with *found false: this interface has no
// default route for the server's family, the one case the server starts
// without a gateway. address_type picks the discovery: the IPv4 routing
// table and neighbors, or the IPv6 ones.

static bool get_gateway_mac_address( const char * interface_name, int address_type, uint8_t * mac_address, bool * found )
{
    memset( mac_address, 0, 6 );
    *found = false;

    char mac_address_string[18];
    memset( mac_address_string, 0, sizeof(mac_address_string) );

    // a parameter override under the #106 loud-override law: not a config
    // table field (it overrides a discovered value, not a declared one),
    // but it reads through the platform wrapper, logs what it displaces
    // (the address discovery it preempts), and STRICT-PARSES like every
    // other env value -- a malformed MAC refuses loudly rather than
    // silently becoming 00:00:00:00:00:00 through strtol

    const char * gateway_ethernet_address_env = rocketnet_platform_getenv( "ROCKETNET_GATEWAY_ETHERNET_ADDRESS" );
    if ( gateway_ethernet_address_env && gateway_ethernet_address_env[0] != '\0' )
    {
        bool valid = strlen( gateway_ethernet_address_env ) == 17;
        for ( int i = 0; valid && i < 17; i++ )
        {
            const char c = gateway_ethernet_address_env[i];
            if ( ( i % 3 ) == 2 )
                valid = c == ':' || c == '.';
            else
                valid = ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
        }

        if ( !valid )
        {
            rocketnet_error( "could not parse ROCKETNET_GATEWAY_ETHERNET_ADDRESS=\"%s\" as a mac address (xx:xx:xx:xx:xx:xx)", gateway_ethernet_address_env );
            return false;
        }

        rocketnet_info( "server config override ROCKETNET_GATEWAY_ETHERNET_ADDRESS=%s (displaces gateway discovery)", gateway_ethernet_address_env );

        strncpy( mac_address_string, gateway_ethernet_address_env, 17 );
        mac_address_string[17] = 0;
    }
    else if ( address_type == ROCKETNET_ADDRESS_IPV6 )
    {
        bool found_route = false;
        if ( !get_gateway_mac_address_ipv6( interface_name, mac_address_string, &found_route ) )
            return false;
        if ( !found_route )
            return true;
    }
    else
    {
        // the gateway address: the first `netstat -rn` line that is the
        // default route on this interface, matched on whole columns by
        // rocketnet_server_socket_ipv4_route_line_gateway

        char gateway_ip_string[INET_ADDRSTRLEN + 1];
        gateway_ip_string[0] = '\0';

        FILE * file = popen( "netstat -rn", "r" );
        if ( !file )
            return false;
        char netstat_buffer[1024];
        while ( fgets( netstat_buffer, sizeof(netstat_buffer), file ) != NULL )
        {
            if ( rocketnet_server_socket_ipv4_route_line_gateway( netstat_buffer, interface_name, gateway_ip_string, sizeof(gateway_ip_string) ) )
                break;
        }
        pclose( file );

        if ( gateway_ip_string[0] == '\0' )
        {
            return true;
        }

        // parse the address and make sure it's a valid ipv4

        rocketnet_address_t address;
        if ( !rocketnet_address_parse( &address, gateway_ip_string ) || address.type != ROCKETNET_ADDRESS_IPV4 )
        {
            return false;
        }

        // its ethernet address: the first `ip neigh show` line whose address
        // and dev are the gateway and this interface as whole tokens and that
        // carries an lladdr, by rocketnet_server_socket_ipv4_neigh_line_mac

        bool found_mac_address = false;

        file = popen( "ip neigh show", "r" );
        if ( !file )
            return false;
        char ip_buffer[1024];
        while ( fgets( ip_buffer, sizeof(ip_buffer), file ) != NULL )
        {
            if ( rocketnet_server_socket_ipv4_neigh_line_mac( ip_buffer, gateway_ip_string, interface_name, mac_address_string ) )
            {
                found_mac_address = true;
                break;
            }
        }
        pclose( file );

        if ( !found_mac_address )
        {
            return false;
        }
    }

    mac_address_string[2] = 0;
    mac_address_string[5] = 0;
    mac_address_string[8] = 0;
    mac_address_string[11] = 0;
    mac_address_string[14] = 0;
    mac_address_string[17] = 0;

    mac_address[0] = (uint8_t) strtol( mac_address_string + 0, NULL, 16 );
    mac_address[1] = (uint8_t) strtol( mac_address_string + 3, NULL, 16 );
    mac_address[2] = (uint8_t) strtol( mac_address_string + 6, NULL, 16 );
    mac_address[3] = (uint8_t) strtol( mac_address_string + 9, NULL, 16 );
    mac_address[4] = (uint8_t) strtol( mac_address_string + 12, NULL, 16 );
    mac_address[5] = (uint8_t) strtol( mac_address_string + 15, NULL, 16 );

    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };
    if ( ( mac_address[0] & 1 ) || memcmp( mac_address, zero_mac, 6 ) == 0 )
    {
        rocketnet_error( "gateway mac address %s is zero or multicast", mac_address_string );
        return false;
    }

    *found = true;
    return true;
}

#define INVALID_FRAME UINT64_MAX

static uint64_t alloc_send_frame( rocketnet_server_xdp_socket_t * socket )
{
    uint64_t frame = INVALID_FRAME;
    if ( socket->num_free_send_frames > 0 )
    {
        socket->num_free_send_frames--;
        frame = socket->send_frames[socket->num_free_send_frames];
        socket->send_frames[socket->num_free_send_frames] = INVALID_FRAME;
    }
    return frame;
}

static void free_send_frame( rocketnet_server_xdp_socket_t * socket, uint64_t frame )
{
    const uint64_t region_begin = 0;
    const uint64_t region_end = (uint64_t)ROCKETNET_XDP_NUM_FRAMES / 2 * ROCKETNET_XDP_FRAME_SIZE;
    if ( !rocketnet_xdp_free_list_push( socket->send_frames, &socket->num_free_send_frames, ROCKETNET_XDP_NUM_FRAMES / 2, frame, region_begin, region_end ) )
    {
        rocketnet_error( "xdp send frame dropped: %llu", (unsigned long long)frame );
    }
}

static uint64_t alloc_receive_frame( rocketnet_server_xdp_socket_t * socket )
{
    uint64_t frame = INVALID_FRAME;
    if ( socket->num_free_receive_frames > 0 )
    {
        socket->num_free_receive_frames--;
        frame = socket->receive_frames[socket->num_free_receive_frames];
        socket->receive_frames[socket->num_free_receive_frames] = INVALID_FRAME;
    }
    return frame;
}

static void rocketnet_server_socket_xdp_thread_function( void * data );

// Removes the private build directory created by mkdtemp. The path is always
// one this process made, never a glob and never the caller's CWD, so an
// unguarded rm -rf on it is safe in a way the code this replaced was not.
// Still refuses anything that does not look like our own mkdtemp template --
// an rm -rf built by snprintf deserves one cheap assertion in front of it.
static void rocketnet_server_socket_remove_workdir( const char * workdir )
{
    if ( workdir == NULL || strncmp( workdir, "/var/tmp/rocketnet_xdp_", 23 ) != 0 )
    {
        rocketnet_error( "refusing to remove unexpected work directory" );
        return;
    }

    char command[1024];
    snprintf( command, sizeof(command), "rm -rf %s", workdir );
    FILE * file = popen( command, "r" );
    if ( file )
    {
        char buffer[1024];
        while ( fgets( buffer, sizeof(buffer), file ) != NULL ) {}
        pclose( file );
    }
}

// the public address as the 16 network-order bytes the XDP config carries
// (the rocketnet address stores IPv6 as host-order 16-bit groups)

static void rocketnet_server_socket_ipv6_bytes( const rocketnet_address_t * address, uint8_t * bytes )
{
    for ( int i = 0; i < 8; i++ )
    {
        bytes[i * 2] = (uint8_t) ( address->data.ipv6[i] >> 8 );
        bytes[i * 2 + 1] = (uint8_t) ( address->data.ipv6[i] & 0xFF );
    }
}

// a server's public address is global: an IPv6 link-local address
// (fe80::/10) names no route off the link, so the pre-flight and create
// refuse it by name

static bool rocketnet_server_socket_ipv6_link_local( const rocketnet_address_t * address )
{
    return address->type == ROCKETNET_ADDRESS_IPV6 && ( address->data.ipv6[0] & 0xFFC0 ) == 0xFE80;
}

// the network interface carrying the public address, read-only -- shared by
// the pre-flight and create so they cannot disagree about which interface
// the driver would use. IPv4 matches an AF_INET address, IPv6 an AF_INET6
// address, each on an UP interface.

static bool rocketnet_server_socket_find_interface( const rocketnet_address_t * public_address, char * interface_name, size_t interface_name_size, unsigned int * interface_index )
{
    interface_name[0] = '\0';
    *interface_index = 0;

    if ( public_address->type != ROCKETNET_ADDRESS_IPV4 && public_address->type != ROCKETNET_ADDRESS_IPV6 )
        return false;

    uint32_t public_address_ipv4 = 0;
    uint8_t public_address_ipv6[16];
    memset( public_address_ipv6, 0, sizeof(public_address_ipv6) );
    if ( public_address->type == ROCKETNET_ADDRESS_IPV4 )
        memcpy( &public_address_ipv4, public_address->data.ipv4, 4 );     // network order, as sin_addr holds it
    else
        rocketnet_server_socket_ipv6_bytes( public_address, public_address_ipv6 );

    struct ifaddrs * addrs;
    if ( getifaddrs( &addrs ) != 0 )
        return false;

    bool found = false;

    for ( struct ifaddrs * iap = addrs; iap != NULL; iap = iap->ifa_next )
    {
        if ( !iap->ifa_addr || !( iap->ifa_flags & IFF_UP ) )
            continue;

        bool match = false;

        if ( public_address->type == ROCKETNET_ADDRESS_IPV4 && iap->ifa_addr->sa_family == AF_INET )
        {
            const struct sockaddr_in * sa = (const struct sockaddr_in*) iap->ifa_addr;
            match = sa->sin_addr.s_addr == public_address_ipv4;
        }
        else if ( public_address->type == ROCKETNET_ADDRESS_IPV6 && iap->ifa_addr->sa_family == AF_INET6 )
        {
            const struct sockaddr_in6 * sa = (const struct sockaddr_in6*) iap->ifa_addr;
            match = memcmp( sa->sin6_addr.s6_addr, public_address_ipv6, 16 ) == 0;
        }

        if ( match )
        {
            strncpy( interface_name, iap->ifa_name, interface_name_size - 1 );
            interface_name[interface_name_size - 1] = '\0';
            *interface_index = if_nametoindex( iap->ifa_name );
            found = *interface_index != 0;
            break;
        }
    }

    freeifaddrs( addrs );

    return found;
}

// read one small sysfs value; returns false when the file does not exist

static bool rocketnet_server_socket_read_sysfs( const char * path, char * value, size_t value_size )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
        return false;
    value[0] = '\0';
    if ( fgets( value, (int) value_size, file ) )
    {
        char * newline = strchr( value, '\n' );
        if ( newline )
            *newline = '\0';
    }
    fclose( file );
    return true;
}

// THE COMMIT BARRIER: a process that must never commit an XDP data plane
// -- the test suite installs this once, structurally, for every present
// and future test -- forbids the committing create outright. the
// pre-flight stays reachable (it is non-mutating by contract); create
// refuses at the top, before its first act. allow exists for a caller
// that deliberately opts back in; nothing in the suite does.

static std::atomic<bool> rocketnet_server_socket_commit_forbidden_flag( false );

void rocketnet_server_socket_forbid_commit()
{
    rocketnet_server_socket_commit_forbidden_flag.store( true );
}

void rocketnet_server_socket_allow_commit()
{
    rocketnet_server_socket_commit_forbidden_flag.store( false );
}

// cpu list helpers for the boot-tier checks: "1-7,9" style lists as
// /sys/devices/system/cpu/isolated and /proc/irq/N/smp_affinity_list
// print them

static bool rocketnet_server_socket_cpu_in_list( const char * list, int cpu )
{
    const char * cursor = list;
    while ( *cursor )
    {
        int first = -1;
        int last = -1;
        char * end = NULL;
        first = (int) strtol( cursor, &end, 10 );
        if ( end == cursor )
            break;
        cursor = end;
        last = first;
        if ( *cursor == '-' )
        {
            cursor++;
            last = (int) strtol( cursor, &end, 10 );
            if ( end == cursor )
                break;
            cursor = end;
        }
        if ( cpu >= first && cpu <= last )
            return true;
        if ( *cursor == ',' )
            cursor++;
    }
    return false;
}

static int rocketnet_server_socket_count_cpu_list( const char * list )
{
    int count = 0;
    const char * cursor = list;
    while ( *cursor )
    {
        int first = -1;
        int last = -1;
        char * end = NULL;
        first = (int) strtol( cursor, &end, 10 );
        if ( end == cursor )
            break;
        cursor = end;
        last = first;
        if ( *cursor == '-' )
        {
            cursor++;
            last = (int) strtol( cursor, &end, 10 );
            if ( end == cursor )
                break;
            cursor = end;
        }
        if ( last >= first )
            count += last - first + 1;
        if ( *cursor == ',' )
            cursor++;
    }
    return count;
}

// The R4 interrupt-line matcher, one boundary definition implemented
// twice (this function and the script's awk form, held to the same case
// set by the fixture test here and the script's selftest): the interface
// name matches as a bounded token -- preceded by line start, whitespace,
// or '-' (driver-prefixed names like i40e-ens1f0-TxRx-0, the exact
// family B3 tells operators to build) and followed by '-', ':', '@',
// whitespace, or end. pure string logic, exported for the fixture test.
// accepted asymmetries with the script's form, recorded: this reader
// splits lines past its 4096-byte buffer where awk is unbounded (real
// /proc/interrupts lines are far shorter); '\r' rides this after-set for
// fgets robustness but is unreachable through awk (no CRLF procfs); the
// empty interface name refuses here and is unreachable in the script
// (empty arguments refuse at the door).

bool rocketnet_server_socket_interrupt_line_matches_interface( const char * line, const char * interface_name )
{
    rocketnet_assert( line );
    rocketnet_assert( interface_name );

    const size_t name_length = strlen( interface_name );
    if ( name_length == 0 )
        return false;

    const char * match = strstr( line, interface_name );
    while ( match )
    {
        const char before = match == line ? ' ' : match[-1];
        const char after = match[name_length];
        const bool before_ok = before == ' ' || before == '\t' || before == '-';
        const bool after_ok = after == '-' || after == ':' || after == '@' || after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '\0';
        if ( before_ok && after_ok )
            return true;
        match = strstr( match + 1, interface_name );
    }
    return false;
}

// a kernel command line token, bounded: preceded by start or space,
// followed by space or end -- "irqaffinity=0" must not match
// "irqaffinity=0-3" or "irqaffinity=012"

static bool rocketnet_server_socket_cmdline_has_token( const char * cmdline, const char * token )
{
    const size_t token_length = strlen( token );
    const char * match = strstr( cmdline, token );
    while ( match )
    {
        const char before = match == cmdline ? ' ' : match[-1];
        const char after = match[token_length];
        if ( ( before == ' ' ) && ( after == ' ' || after == '\n' || after == '\0' ) )
            return true;
        match = strstr( match + 1, token );
    }
    return false;
}

// THE PRE-FLIGHT (#106): non-mutating, checking the two-tier condition
// list in scripts/rocketnet-provision's header -- the single documented
// source this check, that script and BUILDING.md all derive from. Every
// unmet condition names the knob, the wanted value, and the script;
// nothing here changes machine state (probe descriptors are created and
// closed, never bound, never attached). All conditions that can be
// checked from here are checked, so one run names every errand at once
// rather than one per attempt.

uint64_t rocketnet_xdp_umem_bytes(int queues)
{
    if (queues < 1 || queues > ROCKETNET_XDP_MAX_QUEUES) return 0;
    uint64_t bytes = 0;
    if (!rocketnet_memory_add(bytes, (uint64_t)queues,
            (uint64_t)ROCKETNET_XDP_NUM_FRAMES * ROCKETNET_XDP_FRAME_SIZE) || bytes > SIZE_MAX)
        return 0;
    return bytes;
}

uint64_t rocketnet_server_xdp_allocation_bytes(int queues)
{
    if (queues < 1 || queues > ROCKETNET_XDP_MAX_QUEUES) return 0;
    uint64_t bytes = rocketnet_memory_allocations({sizeof(rocketnet_server_socket_t),
        (uint64_t)queues * sizeof(rocketnet_server_xdp_socket_t)});
    if (!bytes || !rocketnet_memory_add(bytes, (uint64_t)queues, rocketnet_platform_thread_allocation_bytes())) return 0;
    return bytes <= SIZE_MAX ? bytes : 0;
}

uint64_t rocketnet_server_socket_memory_required(int num_queues)
{
    if (num_queues < 1 || num_queues > ROCKETNET_XDP_MAX_QUEUES) return 0;
    const uint64_t umem = rocketnet_xdp_umem_bytes(num_queues);
    uint64_t bytes = sizeof(rocketnet_server_socket_t);
    if (!umem ||
        !rocketnet_memory_add(bytes, (uint64_t)num_queues, sizeof(rocketnet_server_xdp_socket_t)) ||
        !rocketnet_memory_add(bytes, 1, umem) || bytes > SIZE_MAX)
        return 0;
    return bytes;
}

static bool rocketnet_server_socket_interface_is_veth( const char * interface_name )
{
    // the driver name the kernel reports for the interface (ETHTOOL_GDRVINFO)
    const int probe_fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( probe_fd < 0 )
        return false;
    struct ethtool_drvinfo info;
    memset( &info, 0, sizeof(info) );
    info.cmd = ETHTOOL_GDRVINFO;
    struct ifreq ifr;
    memset( &ifr, 0, sizeof(ifr) );
    strncpy( ifr.ifr_name, interface_name, sizeof(ifr.ifr_name) - 1 );
    ifr.ifr_data = (char*) &info;
    const bool veth = ioctl( probe_fd, SIOCETHTOOL, &ifr ) == 0 && strcmp( info.driver, "veth" ) == 0;
    close( probe_fd );
    return veth;
}

bool rocketnet_server_socket_preflight( const char * public_address_string, int num_queues )
{
    rocketnet_assert( public_address_string );
    rocketnet_assert( num_queues >= 1 );

    const uint64_t memory_bytes = rocketnet_server_socket_memory_required(num_queues);
    const uint64_t memory_budget = (uint64_t)ROCKETNET_XDP_MEMORY_MIB * 1024 * 1024;
    if (!memory_bytes || memory_bytes > memory_budget)
    {
        rocketnet_warn("xdp geometry refused: requires %llu bytes, component budget %llu bytes",
            (unsigned long long)memory_bytes, (unsigned long long)memory_budget);
        return false;
    }
    rocketnet_info("xdp userspace queues and UMEM require %llu bytes before kernel and thread overhead",
        (unsigned long long)memory_bytes);

    // R1: root. the data plane requires euid 0 -- the same refusal create
    // itself would give, checked here so the ladder can name it without
    // touching anything.

    if ( geteuid() != 0 )
    {
        rocketnet_warn( "xdp pre-flight: not root (R1 of scripts/rocketnet-provision). the XDP data plane requires euid 0" );
        return false;
    }

    bool provisioned = true;

    // R2: an UP interface carries the public address

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) ||
         ( public_address.type != ROCKETNET_ADDRESS_IPV4 && public_address.type != ROCKETNET_ADDRESS_IPV6 ) )
    {
        rocketnet_warn( "xdp pre-flight: public address %s is not a parseable ipv4 or ipv6 address (R2 of scripts/rocketnet-provision)", public_address_string );
        return false;
    }

    if ( rocketnet_server_socket_ipv6_link_local( &public_address ) )
    {
        rocketnet_warn( "xdp pre-flight: public address %s is ipv6 link-local (fe80::/10); a server's public address is global (R2 of scripts/rocketnet-provision)", public_address_string );
        return false;
    }

    char interface_name[1024];
    unsigned int interface_index = 0;
    if ( !rocketnet_server_socket_find_interface( &public_address, interface_name, sizeof(interface_name), &interface_index ) )
    {
        rocketnet_warn( "xdp pre-flight: no UP interface carries the public address (R2 of scripts/rocketnet-provision)" );
        return false;
    }

    // R3: the exclusive xdp slot -- no program of anyone's attached to the
    // interface. read-only query; the library never unloads anyone (the
    // script does the clearing, deliberately).

    {
        unsigned int attached_program_id = 0;
        const int query_result = bpf_xdp_query_id( (int) interface_index, 0, &attached_program_id );
        if ( query_result != 0 )
        {
            // silence must never verify: an unanswerable exclusivity query
            // is an unmet condition, not a pass

            // -errno is libbpf 1.0's uniform error convention, which is
            // the tree's documented floor (BUILDING.md) -- pre-1.0 libbpf
            // returned -1 with errno set and would misreport here

            rocketnet_warn( "xdp pre-flight: could not query %s for an attached XDP program (%s) (R3)", interface_name, strerror( -query_result ) );
            provisioned = false;
        }
        else if ( attached_program_id != 0 )
        {
            rocketnet_warn( "xdp pre-flight: an XDP program (id %u) is already attached to %s (R3): run scripts/rocketnet-provision %s %d", attached_program_id, interface_name, interface_name, num_queues );
            provisioned = false;
        }
    }

    // B1: the boot tier's kernel command line -- irqaffinity=0 exactly,
    // isolcpus= and nohz_full= with values (nosmt's effect is verified by
    // state under R6). grub changes, so the script can only verify these
    // too; the errand is a reboot, named as one.

    char value[4096];
    if ( rocketnet_server_socket_read_sysfs( "/proc/cmdline", value, sizeof(value) ) )
    {
        if ( !rocketnet_server_socket_cmdline_has_token( value, "irqaffinity=0" ) )
        {
            rocketnet_warn( "xdp pre-flight: kernel command line lacks irqaffinity=0 -- exactly that token (B1): grub + reboot, per scripts/rocketnet-provision" );
            provisioned = false;
        }

        const char * ranged_tokens[2] = { "isolcpus=", "nohz_full=" };
        for ( int i = 0; i < 2; i++ )
        {
            // left-bounded like every other token: "xisolcpus=1-7" is not
            // the parameter. accepted-unreachable divergence, recorded: a
            // command line carrying the same token twice (one valid, one
            // malformed) can grade differently here than in the script --
            // this check takes the first left-bounded occurrence, the
            // script's shell pattern answers for the line as a whole.

            const char * token = strstr( value, ranged_tokens[i] );
            while ( token && token != value && token[-1] != ' ' )
                token = strstr( token + 1, ranged_tokens[i] );

            if ( !token || token[strlen( ranged_tokens[i] )] == '\0' || token[strlen( ranged_tokens[i] )] == ' ' )
            {
                rocketnet_warn( "xdp pre-flight: kernel command line lacks %s<range> (B1): grub + reboot, per scripts/rocketnet-provision", ranged_tokens[i] );
                provisioned = false;
            }
        }
    }
    else
    {
        // unreadable is unmet, exactly as the script's empty read fails
        // every B1 token -- silence must never verify

        rocketnet_warn( "xdp pre-flight: /proc/cmdline is unreadable (B1): cannot verify the boot tier" );
        provisioned = false;
    }

    // B2: the isolated cpu set exists and is non-empty -- isolcpus= taken
    // effect. an absent file is the same errand as an empty one, in both
    // this check and the script.

    char isolated[1024];
    bool have_isolated = rocketnet_server_socket_read_sysfs( "/sys/devices/system/cpu/isolated", isolated, sizeof(isolated) ) && isolated[0] != '\0';
    if ( !have_isolated )
    {
        rocketnet_warn( "xdp pre-flight: no isolated cpus (B2): B1's isolcpus= has not taken effect (grub + reboot, per scripts/rocketnet-provision)" );
        provisioned = false;
    }

    // R4's scaling clause: one queue wants one isolated core

    if ( have_isolated && rocketnet_server_socket_count_cpu_list( isolated ) < num_queues )
    {
        rocketnet_warn( "xdp pre-flight: only %d isolated cpus for %d queues (R4 scales with B1): one queue wants one isolated core", rocketnet_server_socket_count_cpu_list( isolated ), num_queues );
        provisioned = false;
    }

    // R4: each of the interface's queue IRQs is pinned to exactly one cpu
    // FROM THE ISOLATED SET, no cpu doubled -- a range or comma is the
    // scheduler default, a non-isolated cpu (0 included, which
    // irqaffinity=0 parks everything on) is not a pin, and two queues on
    // one core is not one-queue-one-core. interfaces with no per-queue
    // IRQs (VMs) pass. the interface name matches as a whole token, never
    // a substring ("lo" must not match "velocity0").

    {
        FILE * interrupts = fopen( "/proc/interrupts", "r" );
        if ( !interrupts )
        {
            // silence must never verify: an unreadable interrupt table is
            // an unmet condition, not a pass

            rocketnet_warn( "xdp pre-flight: /proc/interrupts is unreadable (R4): cannot verify irq pinning" );
            provisioned = false;
        }
        else
        {
            int matched_irqs = 0;

            uint8_t cpu_seen[1024];
            memset( cpu_seen, 0, sizeof(cpu_seen) );

            char line[4096];
            while ( fgets( line, sizeof(line), interrupts ) )
            {
                if ( !rocketnet_server_socket_interrupt_line_matches_interface( line, interface_name ) )
                    continue;

                int irq = 0;
                if ( sscanf( line, " %d:", &irq ) != 1 )
                    continue;

                matched_irqs++;

                char affinity_path[256];
                snprintf( affinity_path, sizeof(affinity_path), "/proc/irq/%d/smp_affinity_list", irq );

                char affinity[256];
                if ( !rocketnet_server_socket_read_sysfs( affinity_path, affinity, sizeof(affinity) ) )
                {
                    rocketnet_warn( "xdp pre-flight: irq %d affinity is unreadable (R4): cannot verify its pin", irq );
                    provisioned = false;
                    continue;
                }

                if ( strchr( affinity, '-' ) || strchr( affinity, ',' ) )
                {
                    rocketnet_warn( "xdp pre-flight: irq %d affinity is '%s', want a single isolated cpu (R4): run scripts/rocketnet-provision %s %d", irq, affinity, interface_name, num_queues );
                    provisioned = false;
                    continue;
                }

                const int cpu = atoi( affinity );

                if ( have_isolated && !rocketnet_server_socket_cpu_in_list( isolated, cpu ) )
                {
                    rocketnet_warn( "xdp pre-flight: irq %d is pinned to cpu %d, which is not in the isolated set %s (R4): run scripts/rocketnet-provision %s %d", irq, cpu, isolated, interface_name, num_queues );
                    provisioned = false;
                }

                if ( cpu < 0 || cpu >= (int) sizeof(cpu_seen) )
                {
                    // outside the checkable range is unmet, never silently
                    // skipped

                    rocketnet_warn( "xdp pre-flight: irq %d affinity cpu %d is outside the checkable range (R4)", irq, cpu );
                    provisioned = false;
                }
                else
                {
                    if ( cpu_seen[cpu] )
                    {
                        rocketnet_warn( "xdp pre-flight: two of %s's IRQs are pinned to cpu %d (R4): one queue, one core -- run scripts/rocketnet-provision %s %d", interface_name, cpu, interface_name, num_queues );
                        provisioned = false;
                    }
                    cpu_seen[cpu] = 1;
                }
            }
            fclose( interrupts );

            if ( matched_irqs == 0 )
            {
                // HONESTY over a false rationale: some drivers' IRQ names
                // never carry the netdev name (mlx5-class), and VMs may
                // have no per-queue vectors at all -- R4 cannot be
                // verified BY NAME here, and saying so is different from
                // claiming there is nothing to pin

                rocketnet_info( "xdp pre-flight: no /proc/interrupts line names %s (R4): either this NIC has no per-queue vectors, or its transport's irq naming does not embed the interface name (mlx5-class) -- irq pinning cannot be verified by name here; pin and verify via the NIC transport's own tooling", interface_name );
            }
            else if ( have_isolated && matched_irqs > rocketnet_server_socket_count_cpu_list( isolated ) )
            {
                // the same refusal the script gives a round robin that
                // would wrap: more queue irqs than isolated cores IS the
                // doubling

                rocketnet_warn( "xdp pre-flight: %d of %s's IRQs but only %d isolated cpus (R4): one queue, one core -- grow the isolated range or shrink the queue count", matched_irqs, interface_name, rocketnet_server_socket_count_cpu_list( isolated ) );
                provisioned = false;
            }
        }
    }

    // R6: smt off (absent knob passes; belt-and-braces under B1's nosmt)

    if ( rocketnet_server_socket_read_sysfs( "/sys/devices/system/cpu/smt/control", value, sizeof(value) ) )
    {
        if ( strcmp( value, "off" ) != 0 && strcmp( value, "forceoff" ) != 0 && strcmp( value, "notsupported" ) != 0 && strcmp( value, "notimplemented" ) != 0 )
        {
            rocketnet_warn( "xdp pre-flight: smt is '%s', want off (R6): run scripts/rocketnet-provision %s %d", value, interface_name, num_queues );
            provisioned = false;
        }
    }

    // R5: the busy-poll pair (absent knobs pass)

    char path[2048];
    snprintf( path, sizeof(path), "/sys/class/net/%s/napi_defer_hard_irqs", interface_name );
    if ( rocketnet_server_socket_read_sysfs( path, value, sizeof(value) ) && strcmp( value, "2" ) != 0 )
    {
        rocketnet_warn( "xdp pre-flight: napi_defer_hard_irqs is %s, want 2 (R5): run scripts/rocketnet-provision %s %d", value, interface_name, num_queues );
        provisioned = false;
    }

    snprintf( path, sizeof(path), "/sys/class/net/%s/gro_flush_timeout", interface_name );
    if ( rocketnet_server_socket_read_sysfs( path, value, sizeof(value) ) && strcmp( value, "200000" ) != 0 )
    {
        rocketnet_warn( "xdp pre-flight: gro_flush_timeout is %s, want 200000 (R5): run scripts/rocketnet-provision %s %d", value, interface_name, num_queues );
        provisioned = false;
    }

    // R7: the NIC's combined channel count equals the configured queue
    // count (a NIC without channel support passes)

    {
        int probe_fd = socket( AF_INET, SOCK_DGRAM, 0 );
        if ( probe_fd < 0 )
        {
            // silence must never verify

            rocketnet_warn( "xdp pre-flight: could not open a probe socket for the channel query (%s) (R7)", strerror( errno ) );
            provisioned = false;
        }
        else
        {
            struct ethtool_channels channels;
            memset( &channels, 0, sizeof(channels) );
            channels.cmd = ETHTOOL_GCHANNELS;

            struct ifreq ifr;
            memset( &ifr, 0, sizeof(ifr) );
            strncpy( ifr.ifr_name, interface_name, sizeof(ifr.ifr_name) - 1 );
            ifr.ifr_data = (char*) &channels;

            const int ethtool_result = ioctl( probe_fd, SIOCETHTOOL, &ifr );
            if ( ethtool_result != 0 && ( errno == EOPNOTSUPP || errno == ENOTSUP ) )
            {
                // the honest pass, stated like its R4 sibling: no channel
                // surface means the count cannot be wrong, and saying so
                // is different from silence

                rocketnet_info( "xdp pre-flight: %s has no channel surface (R7 passes: the transport does not support channel queries)", interface_name );
            }
            else if ( ethtool_result != 0 )
            {
                rocketnet_warn( "xdp pre-flight: the channel query on %s failed (%s) (R7): cannot verify the queue count", interface_name, strerror( errno ) );
                provisioned = false;
            }
            else if ( channels.combined_count == 0 && (int) channels.rx_count == num_queues && (int) channels.tx_count == num_queues &&
                      rocketnet_server_socket_interface_is_veth( interface_name ) )
            {
                // veth reports separate rx and tx channels and never combined
                // ones, so the queue count is carried in both: the kernel
                // tier's on-link case binds the server on a veth pair

                rocketnet_info( "xdp pre-flight: %s has %u rx and %u tx channels and no combined ones (R7 passes: the queue count is %d)", interface_name, channels.rx_count, channels.tx_count, num_queues );
            }
            else
            {
                if ( (int) channels.combined_count != num_queues )
                {
                    rocketnet_warn( "xdp pre-flight: combined queue count is %u, want %d (R7): run scripts/rocketnet-provision %s %d", channels.combined_count, num_queues, interface_name, num_queues );
                    provisioned = false;
                }
            }
            close( probe_fd );
        }
    }

    // K1, kernel capability, per the ruling's checklist: an AF_XDP socket
    // created and closed without a bind...

    {
        int xdp_fd = socket( AF_XDP, SOCK_RAW, 0 );
        if ( xdp_fd < 0 )
        {
            rocketnet_warn( "xdp pre-flight: kernel refused an AF_XDP socket (%s) (K1). this kernel cannot run the XDP data plane", strerror( errno ) );
            provisioned = false;
        }
        else
        {
            close( xdp_fd );
        }
    }

    // ...and a BPF program loaded and closed without an attach

    {
        struct bpf_insn instructions[2];
        memset( instructions, 0, sizeof(instructions) );
        instructions[0].code = BPF_ALU64 | BPF_MOV | BPF_K;         // r0 = XDP_PASS
        instructions[0].imm = 2;
        instructions[1].code = BPF_JMP | BPF_EXIT;

        int program_fd = bpf_prog_load( BPF_PROG_TYPE_XDP, "rocketnet_pf", "GPL", instructions, 2, NULL );
        if ( program_fd < 0 )
        {
            rocketnet_warn( "xdp pre-flight: kernel refused a BPF program load (%s) (K1). this kernel cannot run the XDP data plane", strerror( errno ) );
            provisioned = false;
        }
        else
        {
            close( program_fd );
        }
    }

    return provisioned;
}

rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * public_address_string, const uint8_t * filter_value, int num_queues, int num_os_cpus )
{
    rocketnet_assert( num_queues >= 1 );
    rocketnet_assert( public_address_string );
    rocketnet_assert( filter_value );

    // the committing create refuses outright in a process that forbade it
    // (the test suite's structural barrier) -- before its first act, so no
    // path below this line is reachable there

    if ( rocketnet_server_socket_commit_forbidden_flag.load() )
    {
        rocketnet_warn( "xdp create refused: this process forbids committing an XDP data plane" );
        return NULL;
    }

    // the queue and cpu counts, and since the #110 review the public
    // address too, arrive from the transport config table (with loud
    // override logging and deprecated aliases handled there) -- no env is
    // read here, so the address the pre-flight verified is the address
    // this create binds, always

    rocketnet_info( "server public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        rocketnet_error( "server could not parse public address" );
        return NULL;
    }

    // one server, one public address, one family: IPv4 or IPv6, the family
    // of the address it is created with (DRIVERS.md, AF_XDP)

    if ( public_address.type != ROCKETNET_ADDRESS_IPV4 && public_address.type != ROCKETNET_ADDRESS_IPV6 )
    {
        rocketnet_error( "server public address is neither ipv4 nor ipv6" );
        return NULL;
    }

    if ( rocketnet_server_socket_ipv6_link_local( &public_address ) )
    {
        rocketnet_error( "server public address is ipv6 link-local (fe80::/10); a server's public address is global" );
        return NULL;
    }

    // VERIFY, then bind (#106): the non-mutating pre-flight refuses unless
    // the machine is provisioned -- provisioning itself lives in
    // scripts/rocketnet-provision, run deliberately by the operator, and
    // nothing below this point changes machine state. everything from here
    // is the library's own per-socket work: its BPF program built, loaded
    // and attached, AF_XDP binds, the memlock rlimit.

    if ( !rocketnet_server_socket_preflight( public_address_string, num_queues ) )
    {
        rocketnet_error( "xdp pre-flight failed: this machine is not provisioned for the XDP data plane (see scripts/rocketnet-provision)" );
        return NULL;
    }

    rocketnet_server_socket_t * server_socket = (rocketnet_server_socket_t*) rocketnet_malloc( context, sizeof(rocketnet_server_socket_t) );
    if ( !server_socket )
        return NULL;

    new ( server_socket ) rocketnet_server_socket_t();

    server_socket->context = context;

    server_socket->replay_marks = (struct rocketnet_xdp_replay_marks_t*) rocketnet_malloc( context, sizeof(struct rocketnet_xdp_replay_marks_t) );
    if ( !server_socket->replay_marks )
    {
        rocketnet_error( "server could not allocate the replay marks buffer" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }
    rocketnet_xdp_replay_marks_reset( server_socket->replay_marks );

    // find the network interface that matches the public address -- the
    // same read-only lookup the pre-flight just passed

    char interface_name[1024];
    unsigned int interface_index = 0;
    if ( !rocketnet_server_socket_find_interface( &public_address, interface_name, sizeof(interface_name), &interface_index ) )
    {
        rocketnet_error( "server could not find any network interface matching public address" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    server_socket->interface_index = (int) interface_index;

    rocketnet_info( "server network interface is %s", interface_name );

    server_socket->num_queues = num_queues;

    // look up the ethernet address of the network interface

    if ( !get_interface_mac_address( interface_name, server_socket->server_ethernet_address ) )
    {
        rocketnet_error( "server could not get mac address of network interface" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    rocketnet_info( "server ethernet address is %02x.%02x.%02x.%02x.%02x.%02x", 
        server_socket->server_ethernet_address[0], 
        server_socket->server_ethernet_address[1], 
        server_socket->server_ethernet_address[2], 
        server_socket->server_ethernet_address[3], 
        server_socket->server_ethernet_address[4], 
        server_socket->server_ethernet_address[5] 
    );

    // look up the gateway ethernet address for the network interface

    if ( !get_gateway_mac_address( interface_name, public_address.type, server_socket->gateway_ethernet_address, &server_socket->gateway_known ) )
    {
        rocketnet_error( "server could not get gateway mac address" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    if ( server_socket->gateway_known )
    {
        rocketnet_info( "gateway ethernet address is %02x.%02x.%02x.%02x.%02x.%02x",
            server_socket->gateway_ethernet_address[0],
            server_socket->gateway_ethernet_address[1],
            server_socket->gateway_ethernet_address[2],
            server_socket->gateway_ethernet_address[3],
            server_socket->gateway_ethernet_address[4],
            server_socket->gateway_ethernet_address[5]
        );
    }
    else
    {
        rocketnet_warn( "server found no gateway on %s: replies go only to peers learned on link, and a reply to an unlearned address is refused", interface_name );
    }

    // replies go to the source MAC learned from each answered peer, else
    // to the gateway

    rocketnet_xdp_reply_macs_reset( &server_socket->reply_macs, server_socket->gateway_known ? server_socket->gateway_ethernet_address : NULL );

    // STATE IDENTITY (re-derived twice on the #110 gate): the identity
    // input contains NOTHING process-derived -- a random 64-bit token
    // names the root, because pid+sequence fails deterministically in
    // containers (pid 1 always, the sequence resetting every restart).
    // the root's NAME carries the instance metadata a sweep needs to
    // judge staleness -- pid, process start time, and the host boot id --
    // because bpffs holds no regular files, and a directory name is
    // atomic with the mkdir that claims it:
    //
    //     rocketnet_xdp_<token16>_<pid>_<starttime>_<bootid32>
    //
    // (underscore-delimited: bpffs refuses '.' in names)
    //
    // the root is created by THIS create's own mkdir and never adopted
    // (an existing path -- astronomically unlikely under a random token --
    // refuses loudly rather than letting LIBBPF_PIN_BY_NAME resurrect
    // someone else's state). the four pin paths under it are recorded BY
    // CONSTRUCTION here, at mkdir, since their names are deterministic:
    // every failure path from here on unlinks exactly the recorded paths
    // and removes the root, so no create outcome -- grant, refusal, or
    // mid-create failure -- leaves state behind. only a crash can, and
    // the operator's deliberate sweep judges those by the metadata in the
    // name.

    {
        const uint64_t token = rocketnet_random_uint64();

        char boot_id[64] = "unknown";
        {
            char raw[128];
            if ( rocketnet_server_socket_read_sysfs( "/proc/sys/kernel/random/boot_id", raw, sizeof(raw) ) )
            {
                int j = 0;
                for ( int i = 0; raw[i] != '\0' && j < (int) sizeof(boot_id) - 1; i++ )
                    if ( raw[i] != '-' )
                        boot_id[j++] = raw[i];
                boot_id[j] = '\0';
            }
        }

        uint64_t start_time = 0;
        {
            char stat_line[1024];
            if ( rocketnet_server_socket_read_sysfs( "/proc/self/stat", stat_line, sizeof(stat_line) ) )
            {
                const char * cursor = strrchr( stat_line, ')' );        // comm may contain spaces; fields resume after the last ')'
                if ( cursor )
                {
                    cursor++;
                    for ( int field = 0; field < 19 && cursor; field++ )        // state is field 3 overall; starttime is field 22 = 19 fields later
                        cursor = strchr( cursor + 1, ' ' );
                    if ( cursor )
                        start_time = strtoull( cursor + 1, NULL, 10 );
                }
            }
        }

        snprintf( server_socket->pin_root, sizeof(server_socket->pin_root), "/sys/fs/bpf/rocketnet_xdp_%016" PRIx64 "_%d_%" PRIu64 "_%s",
                  token, (int) getpid(), start_time, boot_id );
    }

    if ( mkdir( server_socket->pin_root, 0700 ) != 0 )
    {
        // each errno is its own operator errand, named as one

        if ( errno == EEXIST )
            rocketnet_error( "pin root %s already exists: stale state is never adopted -- sweep deliberately with scripts/rocketnet-provision sweep, or remove it by hand", server_socket->pin_root );
        else if ( errno == ENOENT )
            rocketnet_error( "could not create pin root %s: bpffs is not mounted (mount -t bpf bpf /sys/fs/bpf)", server_socket->pin_root );
        else
            rocketnet_error( "could not create pin root %s (%s)", server_socket->pin_root, strerror( errno ) );

        server_socket->pin_root[0] = '\0';
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    server_socket->pin_root_created = true;

    // the pins recorded by construction, at the moment the root exists

    {
        static const char * pinned_map_names[4] = { "server_xdp_config_map", "server_xdp_socket_map", "server_xdp_stats_map", "server_xdp_replay_map" };
        for ( int i = 0; i < 4; i++ )
        {
            snprintf( server_socket->pinned_paths[i], sizeof(server_socket->pinned_paths[0]), "%s/%s", server_socket->pin_root, pinned_map_names[i] );
        }
        server_socket->num_pinned = 4;
    }

    // Build the server_xdp program in a PRIVATE directory.
    //
    // It must not build in whatever directory the process inherited. There is
    // no chdir anywhere in this library, so any glob or removal would expand
    // wherever the server was started from -- a source tree, or a directory
    // two servers share and race over server_xdp.o in. A launcher that cds
    // somewhere safe first cannot be relied on: the build lives in the
    // library, not the launcher.
    //
    // mkdtemp gives a fresh 0700 directory per process. Nothing needs
    // pre-clearing (it starts empty), there is no race, and cleanup is
    // rm -rf on a path this process created rather than a glob over files it
    // did not put there.

    char workdir[] = "/var/tmp/rocketnet_xdp_XXXXXX";
    if ( mkdtemp( workdir ) == NULL )
    {
        rocketnet_error( "could not create a temporary directory to build server_xdp" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    // write out source tar.gz for server_xdp program
    {
        char tarball[1024];
        snprintf( tarball, sizeof(tarball), "%s/server_xdp_source.tar.gz", workdir );

        FILE * file = fopen( tarball, "wb" );
        if ( !file )
        {
            rocketnet_error( "could not open %s for writing", tarball );
            rocketnet_server_socket_remove_workdir( workdir );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }

        fwrite( rocketnet_server_xdp_tar_gz, sizeof(rocketnet_server_xdp_tar_gz), 1, file );

        fclose( file );
    }

    // unzip source and build server_xdp.o, entirely inside the private directory
    {
        char command[2048];
        snprintf( command, sizeof(command),
                  "cd %s && tar -zxf server_xdp_source.tar.gz && make server_xdp.o", workdir );
        FILE * file = popen( command, "r" );
        if ( file )
        {
            char buffer[1024];
            while ( fgets( buffer, sizeof(buffer), file ) != NULL ) {}
            pclose( file );
        }
    }

    // load the server_xdp program and attach it to the network interface

    rocketnet_info( "loading server_xdp..." );

    {
        // the open opts carry this instance's pin root, so LIBBPF_PIN_BY_NAME
        // pins land under it -- never under a name another instance shares

        struct bpf_object_open_opts open_opts;
        memset( &open_opts, 0, sizeof(open_opts) );
        open_opts.sz = sizeof(open_opts);
        open_opts.pin_root_path = server_socket->pin_root;

        char object[1024];
        snprintf( object, sizeof(object), "%s/server_xdp.o", workdir );
        server_socket->program = xdp_program__open_file( object, "server_xdp", &open_opts );
    }

    // Clean up after ourselves. This happens AFTER the load, unlike the old
    // sequence -- the old cleanup could run first only because it left
    // server_xdp.o behind in the shared directory for the loader to find.
    // Unlinking a file libxdp still holds open is fine on Linux.
    rocketnet_server_socket_remove_workdir( workdir );

    if ( libxdp_get_error( server_socket->program ) )
    {
        rocketnet_error( "could not load server_xdp program" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    rocketnet_info( "server_xdp loaded successfully." );

    rocketnet_info( "attaching server_xdp to network interface %s", interface_name );

    int ret = xdp_program__attach( server_socket->program, server_socket->interface_index, XDP_MODE_NATIVE, 0 );
    if ( ret == 0 )
    {
        server_socket->attached_native = true;
    } 
    else
    {
        rocketnet_info( "falling back to skb mode..." );
        ret = xdp_program__attach( server_socket->program, server_socket->interface_index, XDP_MODE_SKB, 0 );
        if ( ret == 0 )
        {
            server_socket->attached_skb = true;
        }
        else
        {
            rocketnet_error( "failed to attach server_xdp program to interface %s", interface_name );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }
    }

    // allow unlimited locking of memory, so all memory needed for packet buffers can be locked

    struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };

    if ( setrlimit( RLIMIT_MEMLOCK, &rlim ) ) 
    {
        rocketnet_error( "server could not setrlimit" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    // get file descriptors for maps so we can communicate with the server_xdp
    // program running in kernel space -- under this instance's own pin root

    // the SAME recorded paths destroy will unlink: one construction, one
    // bound, no re-derivation to drift

    server_socket->config_map_fd = bpf_obj_get( server_socket->pinned_paths[0] );
    if ( server_socket->config_map_fd <= 0 )
    {
        rocketnet_error( "server could not get config map: %s", strerror(errno) );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    server_socket->socket_map_fd = bpf_obj_get( server_socket->pinned_paths[1] );
    if ( server_socket->socket_map_fd <= 0 )
    {
        rocketnet_error( "server could not get socket map: %s", strerror(errno) );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    server_socket->stats_map_fd = bpf_obj_get( server_socket->pinned_paths[2] );
    if ( server_socket->stats_map_fd <= 0 )
    {
        rocketnet_error( "server could not get stats map: %s", strerror(errno) );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    server_socket->replay_map_fd = bpf_obj_get( server_socket->pinned_paths[3] );
    if ( server_socket->replay_map_fd <= 0 )
    {
        rocketnet_error( "server could not get replay map: %s", strerror(errno) );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    // configure the kernel prefilter: the destination it accepts and the
    // per-title filter value and epoch. until this write the program passes
    // untouched, so a server that fails before this point never black-holed
    // the box's other traffic.

    {
        struct rocketnet_server_xdp_config_t config;
        memset( &config, 0, sizeof(config) );
        // the server's own family gets its public address. the other
        // family's slot gets that family's loopback address (127.0.0.1, ::1)
        // rather than zero, because zero accepts any destination: a NIC never
        // delivers a loopback destination (the stack drops it as martian), so
        // the program redirects only this server's family and passes the
        // other family's traffic to the kernel untouched

        if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
        {
            memcpy( &config.public_address, public_address.data.ipv4, 4 );     // network order
            config.public_address_ipv6[15] = 1;
        }
        else
        {
            config.public_address = htonl( INADDR_LOOPBACK );
            rocketnet_server_socket_ipv6_bytes( &public_address, config.public_address_ipv6 );
        }
        config.public_port = rocketnet_platform_htons( public_address.port );
        memcpy( config.filter_value, filter_value, ROCKETNET_FILTER_VALUE_BYTES );
        config.filter_epoch = ROCKETNET_FILTER_EPOCH_UNKEYED;

        __u32 key = 0;
        if ( bpf_map_update_elem( server_socket->config_map_fd, &key, &config, BPF_ANY ) != 0 )
        {
            rocketnet_error( "server could not write config map: %s", strerror(errno) );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }
    }

    // initialize server xdp sockets (one socket per-NIC queue)

    server_socket->socket = (rocketnet_server_xdp_socket_t*) rocketnet_malloc( server_socket->context, num_queues * sizeof(rocketnet_server_xdp_socket_t) );
    if ( server_socket->socket == NULL )
    {
        rocketnet_error( "server could not allocate sockets" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    // Construct every queue before any resource acquisition can fail.
    for ( int queue = 0; queue < num_queues; queue++ )
        new ( &server_socket->socket[queue] ) rocketnet_server_xdp_socket_t();

    for ( int queue = 0; queue < num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

        socket->queue = queue;
        socket->num_os_cpus = num_os_cpus;

        // allocate umem

        const int buffer_size = ROCKETNET_XDP_NUM_FRAMES * ROCKETNET_XDP_FRAME_SIZE;

        if ( posix_memalign( &socket->buffer, getpagesize(), buffer_size ) ) 
        {
            rocketnet_allocation_refused();
            rocketnet_error( "server could not allocate buffer" );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }

        // create umem and xdp socket: zero copy first, copy if the driver refuses the zero-copy bind.
        // A refused bind leaves the umem's rings set, so the fallback starts from a fresh umem.

        int result = 0;

        for ( int attempt = 0; attempt < 2; attempt++ )
        {
            const bool zero_copy = attempt == 0;

            struct xsk_umem_config umem_config;
            rocketnet_xdp_umem_config( &umem_config );

            result = xsk_umem__create( &socket->umem, socket->buffer, buffer_size, &socket->fill_queue, &socket->complete_queue, &umem_config );
            if ( result )
            {
                rocketnet_error( "server could not create umem: %s (errno %d)", strerror( -result ), -result );
                rocketnet_server_socket_destroy( server_socket );
                return NULL;
            }

            struct xsk_socket_config xsk_config;
            rocketnet_xdp_socket_config( &xsk_config, ROCKETNET_XDP_RECV_QUEUE_SIZE, ROCKETNET_XDP_SEND_QUEUE_SIZE, zero_copy );

            result = xsk_socket__create( &socket->xsk, interface_name, queue, socket->umem, &socket->receive_queue, &socket->send_queue, &xsk_config );
            if ( result == 0 )
                break;

            socket->xsk = NULL;

            if ( !zero_copy )
            {
                rocketnet_error( "server could not create xsk socket for queue %d: %s (errno %d)", queue, strerror( -result ), -result );
                rocketnet_server_socket_destroy( server_socket );
                return NULL;
            }

            rocketnet_info( "server xdp queue %d refused the zero copy bind (%s), binding in copy mode", queue, strerror( -result ) );

            xsk_umem__delete( socket->umem );
            socket->umem = NULL;
        }

        // record the mode the kernel bound

        struct xdp_options xdp_options;
        memset( &xdp_options, 0, sizeof(xdp_options) );
        socklen_t xdp_options_length = sizeof(xdp_options);
        if ( getsockopt( xsk_socket__fd( socket->xsk ), SOL_XDP, XDP_OPTIONS, &xdp_options, &xdp_options_length ) != 0 )
        {
            rocketnet_error( "server could not read the xdp options for queue %d", queue );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }

        socket->zero_copy = ( xdp_options.flags & XDP_OPTIONS_ZEROCOPY ) != 0;

        // configure the xdp socket to receive packets from the xdp program

        __u32 key = queue;
        __u32 value = xsk_socket__fd( socket->xsk );

        if ( bpf_map_update_elem( server_socket->socket_map_fd, &key, &value, BPF_ANY ) < 0 ) 
        {
            rocketnet_error( "server failed to add xdp socket for queue %d to map", queue );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }

        // copy across data needed by the socket to send packets

        memcpy( socket->server_ethernet_address, server_socket->server_ethernet_address, ETH_ALEN );
        memcpy( socket->gateway_ethernet_address, server_socket->gateway_ethernet_address, ETH_ALEN );
        socket->server_address = public_address;

        // initialize send frame allocator

        rocketnet_assert( ROCKETNET_XDP_SEND_QUEUE_SIZE <= ROCKETNET_XDP_NUM_FRAMES / 4 );
        rocketnet_assert( ROCKETNET_XDP_RECV_QUEUE_SIZE <= ROCKETNET_XDP_NUM_FRAMES / 4 );

        for ( int j = 0; j < ROCKETNET_XDP_NUM_FRAMES / 2; j++ )
        {
            socket->send_frames[j] = j * ROCKETNET_XDP_FRAME_SIZE;
        }

        socket->num_free_send_frames = ROCKETNET_XDP_NUM_FRAMES / 2;

        // make sure we start off with the correct send buffer index

        socket->send_flow_index.store( socket->send_handoff.acquire(), std::memory_order_relaxed );

        // initialize receive frame allocator

        for ( int j = 0; j < ROCKETNET_XDP_NUM_FRAMES / 2; j++ )
        {
            socket->receive_frames[j] = ( ROCKETNET_XDP_NUM_FRAMES / 2 + j ) * ROCKETNET_XDP_FRAME_SIZE;
        }

        socket->num_free_receive_frames = ROCKETNET_XDP_NUM_FRAMES / 2;

        // populate fill ring for packets to be received in
        {
            rocketnet_assert( ROCKETNET_XDP_FILL_QUEUE_SIZE <= ROCKETNET_XDP_RECV_QUEUE_SIZE );

            uint32_t index;
            int result = xsk_ring_prod__reserve( &socket->fill_queue, ROCKETNET_XDP_FILL_QUEUE_SIZE, &index );
            if ( result != ROCKETNET_XDP_FILL_QUEUE_SIZE )
            {
                rocketnet_error( "server failed to populate fill queue: %d", result );
                rocketnet_server_socket_destroy( server_socket );
                return NULL;
            }

            uint64_t frames[ROCKETNET_XDP_FILL_QUEUE_SIZE];
            for ( int i = 0; i < ROCKETNET_XDP_FILL_QUEUE_SIZE; i++ ) 
            {
                frames[i] = alloc_receive_frame( socket );
                if ( frames[i] == INVALID_FRAME )
                {
                    rocketnet_error( "server could not allocate receive frame for fill queue" );
                    rocketnet_server_socket_destroy( server_socket );
                    return NULL;
                }
            }

            for ( int i = 0; i < ROCKETNET_XDP_FILL_QUEUE_SIZE; i++ ) 
            {
                uint64_t * frame = (uint64_t*) xsk_ring_prod__fill_addr( &socket->fill_queue, index + i );
                rocketnet_assert( frame );
                *frame = frames[i];
            }

            xsk_ring_prod__submit( &socket->fill_queue, ROCKETNET_XDP_FILL_QUEUE_SIZE );
        }

        // create send mutex

        if ( !rocketnet_platform_mutex_create( &socket->send_mutex ) )
        {
            rocketnet_error( "failed to create send mutex for queue %d", queue );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }

        // create receive mutex

        if ( !rocketnet_platform_mutex_create( &socket->receive_mutex ) )
        {
            rocketnet_error( "failed to create receive mutex for queue %d", queue );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }

        // start xdp thread

        rocketnet_info( "starting xdp thread for socket queue %d", socket->queue );

        socket->xdp_thread = rocketnet_platform_thread_create( server_socket->context, rocketnet_server_socket_xdp_thread_function, socket );
        if ( !socket->xdp_thread )
        {
            rocketnet_error( "server could not create xdp thread %d", queue );
            rocketnet_server_socket_destroy( server_socket );
            return NULL;
        }
    }

    // the bound mode, once per server: zero copy or copy (each queue's mode is on its socket)

    int zero_copy_queues = 0;
    for ( int queue = 0; queue < num_queues; queue++ )
    {
        if ( server_socket->socket[queue].zero_copy )
            zero_copy_queues++;
    }

    if ( zero_copy_queues == num_queues )
        rocketnet_info( "server xdp sockets bound in zero copy mode on %s", interface_name );
    else if ( zero_copy_queues == 0 )
        rocketnet_info( "server xdp sockets bound in copy mode on %s", interface_name );
    else
        rocketnet_info( "server xdp sockets bound in zero copy mode on %d of %d queues and copy mode on the rest on %s", zero_copy_queues, num_queues, interface_name );

    // the server has started successfully

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server started on %s [xdp]", rocketnet_address_to_string( &public_address, address_string ) );

    server_socket->public_address = public_address;
    server_socket->state = ROCKETNET_SERVER_SOCKET_RUNNING;
    server_socket->server_id = rocketnet_hash_string( public_address_string );
    server_socket->match_id = rocketnet_random_uint64();

    rocketnet_info( "server id is %016" PRIx64, server_socket->server_id );
    rocketnet_info( "match id is %016" PRIx64, server_socket->match_id );

    return server_socket;
}

void rocketnet_server_socket_destroy( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( server_socket->state == ROCKETNET_SERVER_SOCKET_STOPPED );        // IMPORTANT: Please stop the server and wait until state is ROCKETNET_SERVER_SOCKET_STOPPED before destroying it

    if ( server_socket->program != NULL )
    {
        if ( server_socket->attached_native )
        {
            xdp_program__detach( server_socket->program, server_socket->interface_index, XDP_MODE_NATIVE, 0 );
        }
        if ( server_socket->attached_skb )
        {
            xdp_program__detach( server_socket->program, server_socket->interface_index, XDP_MODE_SKB, 0 );
        }
        xdp_program__close( server_socket->program );
    }

    if ( server_socket->config_map_fd > 0 )
        close( server_socket->config_map_fd );
    if ( server_socket->socket_map_fd > 0 )
        close( server_socket->socket_map_fd );
    if ( server_socket->stats_map_fd > 0 )
        close( server_socket->stats_map_fd );
    if ( server_socket->replay_map_fd > 0 )
        close( server_socket->replay_map_fd );

    rocketnet_free( server_socket->context, server_socket->replay_marks );

    // this instance's bpffs state goes with it: the paths recorded by
    // construction at mkdir -- so EVERY create outcome that reaches
    // destroy, mid-create failures included, unlinks them all (a pin that
    // never came to exist unlinks as a harmless ENOENT) -- and only the
    // directory this instance's own mkdir made. a directory that will not
    // empty is refused loudly, naming the operator's sweep.

    for ( int i = 0; i < server_socket->num_pinned; i++ )
        unlink( server_socket->pinned_paths[i] );

    if ( server_socket->pin_root_created && rmdir( server_socket->pin_root ) != 0 )
        rocketnet_warn( "could not remove pin root %s (%s): sweep deliberately with scripts/rocketnet-provision sweep", server_socket->pin_root, strerror( errno ) );

    // the queue array is allocated late in create, so a create that failed
    // before it exists reaches here with num_queues already set and the
    // array still null -- walk it only when it is real

    for ( int i = 0; server_socket->socket != NULL && i < server_socket->num_queues; i++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[i];

        // stop xdp thread

        if ( socket->xdp_thread )
        {
            socket->quit = true;
            rocketnet_platform_thread_join( socket->xdp_thread );
            rocketnet_platform_thread_destroy( socket->xdp_thread );
        }

        // destroy mutexes

        rocketnet_platform_mutex_destroy( &socket->send_mutex );
        rocketnet_platform_mutex_destroy( &socket->receive_mutex );

        // destroy xdp socket

        if ( socket->xsk )
        {
            xsk_socket__delete( socket->xsk );
        }

        if ( socket->umem )
        {
            xsk_umem__delete( socket->umem );
        }

        free( socket->buffer );
        socket->~rocketnet_server_xdp_socket_t();
    }

    rocketnet_free( server_socket->context, server_socket->socket );

    void * allocation_context = server_socket->context;
    server_socket->~rocketnet_server_socket_t();
    rocketnet_clear_and_free( allocation_context, server_socket, sizeof(rocketnet_server_socket_t) );
}

void rocketnet_server_socket_update( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    if ( server_socket->state == ROCKETNET_SERVER_SOCKET_STOPPING )
    {
        server_socket->state = ROCKETNET_SERVER_SOCKET_STOPPED;
    }
}

void rocketnet_server_socket_stop( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    server_socket->state = ROCKETNET_SERVER_SOCKET_STOPPING;
}

int rocketnet_server_socket_state( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return server_socket->state;
}

static void rocketnet_server_socket_acquire_send_buffer( rocketnet_server_xdp_socket_t * socket );

uint8_t * rocketnet_server_socket_start_packet_internal( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, int queue, const rocketnet_address_t * to, const uint8_t * destination_mac )
{
    rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

    int flow_index = socket->send_flow_index.load( std::memory_order_acquire );
    if ( flow_index < 0 )
    {
        // Slow path after both generations were queued. A worker may have
        // retired one since the last flush; recovery needs no extra update.
        rocketnet_platform_mutex_acquire( &socket->send_mutex );
        rocketnet_server_socket_acquire_send_buffer( socket );
        flow_index = socket->send_flow_index.load( std::memory_order_relaxed );
        rocketnet_platform_mutex_release( &socket->send_mutex );
        if ( flow_index < 0 ) return NULL;
    }
    rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[flow_index];

    int packet_index = send_buffer->num_packets.fetch_add(1);

    // the lower bound matters too: an app that stages forever without ever
    // flushing wraps this counter negative after ~2^31 packets, and a
    // negative index here is an out-of-bounds write, not a dropped packet

    if ( packet_index >= ROCKETNET_XDP_SEND_QUEUE_SIZE || packet_index < 0 )
    {
        rocketnet_warn( "sent too many packets!" );
        return NULL;
    }

    send_buffer->packet_info[packet_index].to = *to;
    send_buffer->packet_info[packet_index].packet_bytes = 0;
    memcpy( send_buffer->packet_info[packet_index].destination_mac, destination_mac, ETH_ALEN );

    return send_buffer->packet_data[packet_index];
}

uint8_t * rocketnet_server_socket_start_packet( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    // the reply-MAC rule (rocketnet_xdp_frame.h), before a send slot is
    // taken: a response the transport reserves while processing a receive
    // view passes that view's own from[] entry, which names the frame it
    // answers, used only with no gateway. no MAC (no gateway, nothing
    // learned, no frame answered) is a refusal the transport counts
    // (ROCKETNET_DRIVER_PACKET_UNREACHABLE)

    const uint8_t * answered_mac = NULL;
    const uintptr_t view_begin = (uintptr_t) &server_socket->process_packets.from[0];
    const uintptr_t view_end = (uintptr_t) &server_socket->process_packets.from[server_socket->process_packets.num_packets];
    const uintptr_t to_pointer = (uintptr_t) to;
    if ( to_pointer >= view_begin && to_pointer < view_end )
        answered_mac = server_socket->process_eth[( to_pointer - view_begin ) / sizeof( rocketnet_address_t )];

    uint64_t refused = 0;
    const uint8_t * destination_mac = rocketnet_xdp_reply_mac( &server_socket->reply_macs, to, answered_mac, &refused );
    if ( !destination_mac )
    {
        *packet_id = ROCKETNET_DRIVER_PACKET_UNREACHABLE;
        return NULL;
    }

    *packet_id = server_socket->packet_id.fetch_add(1);

    const int queue = *packet_id % server_socket->num_queues;

    return rocketnet_server_socket_start_packet_internal( server_socket, *packet_id, queue, to, destination_mac );
}

void rocketnet_server_socket_finish_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    const int queue = packet_id % server_socket->num_queues;

    rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

    const int flow_index = socket->send_flow_index.load( std::memory_order_relaxed );
    if ( flow_index < 0 )
        return;
    rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[flow_index];

    ptrdiff_t offset = ( packet_data - send_buffer->packet_data[0] );

    const int packet_index = (int) ( offset / ROCKETNET_XDP_FRAME_SIZE );

    // derived from pointer arithmetic against the CURRENT staging buffer: a
    // pointer from before the last flip lands wildly out of range here, and
    // failing closed turns a contract violation into a lost packet instead
    // of an out-of-bounds write

    if ( packet_index < 0 || packet_index >= ROCKETNET_XDP_SEND_QUEUE_SIZE )
        return;

    // the packet is complete wire bytes as the protocol wrote them; nothing
    // is prepended here -- the xdp thread adds the ethernet, ip and udp
    // headers when it copies the packet into its umem frame

    send_buffer->packet_info[packet_index].packet_bytes = packet_bytes;
}

void rocketnet_server_socket_abort_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( server_socket );

    const int queue = packet_id % server_socket->num_queues;

    rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

    const int flow_index = socket->send_flow_index.load( std::memory_order_relaxed );
    if ( flow_index < 0 )
        return;
    rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[flow_index];

    ptrdiff_t offset = ( packet_data - send_buffer->packet_data[0] );

    const int packet_index = (int) ( offset / ROCKETNET_XDP_FRAME_SIZE );

    if ( packet_index < 0 || packet_index >= ROCKETNET_XDP_SEND_QUEUE_SIZE )
        return;

    send_buffer->packet_info[packet_index].packet_bytes = 0;
}

// With send_mutex held. Only an unowned producer generation can acquire;
// publish its index after resetting the retired buffer for concurrent fills.
static void rocketnet_server_socket_acquire_send_buffer( rocketnet_server_xdp_socket_t * socket )
{
    if ( socket->send_flow_index.load( std::memory_order_relaxed ) >= 0 ) return;
    const int index = socket->send_handoff.acquire();
    if ( index >= 0 )
    {
        auto * buffer = &socket->send_buffer[index];
        buffer->num_packets = 0;
        buffer->packet_start_index = 0;
        socket->send_flow_index.store( index, std::memory_order_release );
    }
}

void rocketnet_server_socket_send_packets( struct rocketnet_server_socket_t * server_socket )
{
    for ( int queue = 0; queue < server_socket->num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];
        rocketnet_platform_mutex_acquire( &socket->send_mutex );
        const int index = socket->send_flow_index.load( std::memory_order_relaxed );
        if ( index >= 0 && socket->send_buffer[index].num_packets > 0 )
        {
            // RNDriverSendStaging.Publish: retain the last producer buffer.
            // A busy worker leaves this generation staged for the next
            // transport send_packets call (Publish retry rule).
            if ( socket->send_handoff.publish() )
                socket->send_flow_index.store( -1, std::memory_order_relaxed );
        }
        rocketnet_server_socket_acquire_send_buffer( socket );
        rocketnet_platform_mutex_release( &socket->send_mutex );
    }
}


#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif

// The replay high-water writer. Main thread only, like the rest of the server
// update path. Forget and mark append to the socket's pending buffer; commit
// applies them to the pinned map and resets the buffer, failures counted and
// named by call, never by key.

void rocketnet_server_socket_replay_forget( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( address );
    rocketnet_assert( server_socket->replay_marks );
    rocketnet_xdp_replay_marks_forget( server_socket->replay_marks, address );
    rocketnet_xdp_reply_macs_forget( &server_socket->reply_macs, address );
}

void rocketnet_server_socket_peer_accepted( struct rocketnet_server_socket_t * server_socket, int view_index )
{
    rocketnet_assert( server_socket );
    if ( view_index < 0 || view_index >= server_socket->process_packets.num_packets )
        return;
    rocketnet_xdp_reply_macs_learn( &server_socket->reply_macs, &server_socket->process_packets.from[view_index], server_socket->process_eth[view_index] );
}

void rocketnet_server_socket_replay_mark( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address, uint32_t high_water )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( address );
    rocketnet_assert( server_socket->replay_marks );
    rocketnet_xdp_replay_marks_mark( server_socket->replay_marks, address, high_water );
}

static void rocketnet_server_socket_replay_commit_failed( struct rocketnet_server_socket_t * server_socket, const char * call )
{
    server_socket->replay_commit_failures++;
    rocketnet_debug( "replay commit: %s failed (%s)", call, strerror( errno ) );
}

void rocketnet_server_socket_replay_commit( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( server_socket->replay_marks );

    struct rocketnet_xdp_replay_marks_t * marks = server_socket->replay_marks;

    // tla/RNXdpReplay.tla, Commit: every forget is applied, then every mark,
    // in one commit; this is the only writer of the kernel map.

    if ( marks->forget_count > 0 )
    {
        struct bpf_map_batch_opts opts;
        memset( &opts, 0, sizeof(opts) );
        opts.sz = sizeof(opts);
        __u32 count = marks->forget_count;
        if ( bpf_map_delete_batch( server_socket->replay_map_fd, marks->forgets, &count, &opts ) != 0 )
        {
            // a missing key is not a failure; an older kernel refuses the
            // batch call, so walk the forgets one syscall at a time

            if ( errno == ENOENT || errno == EINVAL || errno == ENOTSUPP )
            {
                for ( uint32_t i = 0; i < marks->forget_count; i++ )
                {
                    if ( bpf_map_delete_elem( server_socket->replay_map_fd, &marks->forgets[i] ) != 0 && errno != ENOENT )
                        rocketnet_server_socket_replay_commit_failed( server_socket, "bpf_map_delete_elem" );
                }
            }
            else
            {
                rocketnet_server_socket_replay_commit_failed( server_socket, "bpf_map_delete_batch" );
            }
        }
    }

    if ( marks->mark_count > 0 )
    {
        struct bpf_map_batch_opts opts;
        memset( &opts, 0, sizeof(opts) );
        opts.sz = sizeof(opts);
        opts.elem_flags = BPF_ANY;
        __u32 count = marks->mark_count;
        if ( bpf_map_update_batch( server_socket->replay_map_fd, marks->mark_keys, marks->mark_values, &count, &opts ) != 0 )
        {
            if ( errno == EINVAL || errno == ENOTSUPP )
            {
                for ( uint32_t i = 0; i < marks->mark_count; i++ )
                {
                    if ( bpf_map_update_elem( server_socket->replay_map_fd, &marks->mark_keys[i], &marks->mark_values[i], BPF_ANY ) != 0 )
                        rocketnet_server_socket_replay_commit_failed( server_socket, "bpf_map_update_elem" );
                }
            }
            else
            {
                rocketnet_server_socket_replay_commit_failed( server_socket, "bpf_map_update_batch" );
            }
        }
    }

    rocketnet_xdp_replay_marks_reset( marks );
}

uint64_t rocketnet_server_socket_replay_commit_failures( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return server_socket->replay_commit_failures;
}

#if defined(ROCKETNET_XDP_TESTS)
// Exercise the native staging entry points without creating kernel resources.
bool rocketnet_server_socket_test_inactive_generation()
{
    void * server_storage = rocketnet_malloc(NULL, sizeof(rocketnet_server_socket_t));
    void * socket_storage = rocketnet_malloc(NULL, sizeof(rocketnet_server_xdp_socket_t));
    if (!server_storage || !socket_storage)
    {
        rocketnet_free(NULL, socket_storage);
        rocketnet_free(NULL, server_storage);
        return false;
    }
    auto * server = new (server_storage) rocketnet_server_socket_t();
    auto * socket = new (socket_storage) rocketnet_server_xdp_socket_t();
    server->num_queues = 1;
    server->socket = socket;
    static const uint8_t test_gateway_mac[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
    rocketnet_xdp_reply_macs_reset(&server->reply_macs, test_gateway_mac);
    rocketnet_platform_mutex_create(&socket->send_mutex);
    rocketnet_server_socket_acquire_send_buffer(socket);
    rocketnet_address_t to = {};
    to.type = ROCKETNET_ADDRESS_IPV4;
    uint8_t * packets[ROCKETNET_XDP_NUM_BUFFERS] = {};
    uint64_t ids[ROCKETNET_XDP_NUM_BUFFERS] = {};
    bool valid = true;
    for (int generation = 0; generation < ROCKETNET_XDP_NUM_BUFFERS; ++generation)
    {
        packets[generation] = rocketnet_server_socket_start_packet(server, &to, &ids[generation]);
        if (!packets[generation]) { valid = false; break; }
        rocketnet_server_socket_finish_packet(server, ids[generation], packets[generation], 64 + generation);
        valid &= socket->send_buffer[generation].packet_info[0].packet_bytes == size_t(64 + generation);
        uint64_t aborted_id;
        uint8_t * aborted = rocketnet_server_socket_start_packet(server, &to, &aborted_id);
        if (!aborted) { valid = false; break; }
        rocketnet_server_socket_finish_packet(server, aborted_id, aborted, 32);
        rocketnet_server_socket_abort_packet(server, aborted_id, aborted);
        valid &= socket->send_buffer[generation].packet_info[1].packet_bytes == 0;
        // Construct the legacy inactive state for the defensive entry-point
        // checks below; normal publish retains the final producer buffer.
        socket->send_handoff.queued[generation] = generation;
        socket->send_handoff.count++;
        socket->send_handoff.filling = -1;
        socket->send_flow_index.store(-1);
        rocketnet_server_socket_acquire_send_buffer(socket);
    }
    valid &= socket->send_flow_index.load() == -1;
    if (valid)
    {
        // Both generations belong to the worker. The sanitizer negative control
        // detects inactive-buffer pointer formation; the length assertions
        // below separately check preservation of published packet metadata.
        uint64_t refused_id;
        valid &= rocketnet_server_socket_start_packet(server, &to, &refused_id) == NULL;
        rocketnet_server_socket_finish_packet(server, ids[0], packets[0], 17);
        rocketnet_server_socket_abort_packet(server, ids[1], packets[1]);
        for (int generation = 0; generation < ROCKETNET_XDP_NUM_BUFFERS; ++generation)
            valid &= socket->send_buffer[generation].packet_info[0].packet_bytes == size_t(64 + generation);
        rocketnet_server_socket_send_packets(server);
        valid &= socket->send_flow_index.load() == -1;
        socket->send_handoff.retire(); // No worker exists; retire one copied generation.
        uint64_t id;
        uint8_t * packet = rocketnet_server_socket_start_packet(server, &to, &id);
        valid &= packet == packets[0];
        if (packet)
        {
            rocketnet_server_socket_finish_packet(server, id, packet, 100);
            valid &= socket->send_buffer[0].packet_info[0].packet_bytes == 100;
        }
    }
    rocketnet_platform_mutex_destroy(&socket->send_mutex);
    socket->~rocketnet_server_xdp_socket_t();
    server->~rocketnet_server_socket_t();
    rocketnet_free(NULL, socket_storage);
    rocketnet_free(NULL, server_storage);
    return valid;
}

// Exercise the replay writer's pending buffer and its commit failure path
// without a map or a kernel resource: replay_map_fd -1 refuses every syscall.
bool rocketnet_server_socket_test_replay_marks()
{
    void * server_storage = rocketnet_malloc(NULL, sizeof(rocketnet_server_socket_t));
    if (!server_storage)
        return false;
    auto * server = new (server_storage) rocketnet_server_socket_t();
    server->replay_marks = (struct rocketnet_xdp_replay_marks_t*) rocketnet_malloc(NULL, sizeof(struct rocketnet_xdp_replay_marks_t));
    if (!server->replay_marks)
    {
        server->~rocketnet_server_socket_t();
        rocketnet_free(NULL, server_storage);
        return false;
    }
    rocketnet_xdp_replay_marks_reset(server->replay_marks);
    server->replay_map_fd = -1;

    rocketnet_address_t a = {};
    a.type = ROCKETNET_ADDRESS_IPV4;
    a.data.ipv4[0] = 198;
    a.data.ipv4[1] = 51;
    a.data.ipv4[2] = 100;
    a.data.ipv4[3] = 7;
    a.port = 50000;
    rocketnet_address_t b = a;
    b.port = 50001;

    rocketnet_server_socket_replay_forget(server, &a);
    rocketnet_server_socket_replay_mark(server, &a, 41);
    rocketnet_server_socket_replay_mark(server, &b, 7);

    bool valid = server->replay_marks->forget_count == 1;
    valid &= server->replay_marks->mark_count == 2;
    const uint64_t failures_before = rocketnet_server_socket_replay_commit_failures(server);

    rocketnet_server_socket_replay_commit(server);

    valid &= server->replay_marks->forget_count == 0;
    valid &= server->replay_marks->mark_count == 0;
    valid &= rocketnet_server_socket_replay_commit_failures(server) > failures_before;

    rocketnet_server_socket_destroy(server);
    return valid;
}
#endif

void rocketnet_server_socket_process_staged_packet( rocketnet_server_socket_t * server_socket, uint8_t * eth, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes )
{
    if ( packet_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
        return;

    // Bound by the ARRAY, not by the queue depth. These are different numbers:
    // ROCKETNET_XDP_RECV_QUEUE_SIZE is 4096*4 = 16384 (how deep one queue's staging
    // buffer is), while process_packets.from/packet_bytes/packet_data are sized
    // ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS = ROCKETNET_RECEIVE_PACKETS_PER_CLIENT *
    // ROCKETNET_MAX_CLIENTS = 16 * 1000 = 16000. Guarding on the larger one let 384
    // entries write past the end of all three arrays — and since packet_data is
    // the last member of rocketnet_server_socket_t, the tail of that overrun put
    // attacker-timed POINTERS past the allocation, which the caller then
    // dereferences. num_packets accumulates across every queue and both
    // off-buffers before it is reset in receive_packets, so one flooded queue
    // reaches it. The portable path was always bounded correctly.
    if ( server_socket->process_packets.num_packets >= ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS )
        return;

    const int index = server_socket->process_packets.num_packets++;

    // the packet is complete wire bytes as they left the client; the
    // protocol's own reading order takes it from here

    server_socket->process_packets.from[index] = *from;
    server_socket->process_packets.packet_data[index] = packet_data;
    server_socket->process_packets.packet_bytes[index] = packet_bytes;
    memcpy( server_socket->process_eth[index], eth, ETH_ALEN );
}

static void pin_thread_to_cpu( int cpu ) 
{
    int num_cpus = sysconf( _SC_NPROCESSORS_ONLN );
    rocketnet_assert( cpu >= 0 );
    rocketnet_assert( cpu < num_cpus );

    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    CPU_SET( cpu, &cpuset );

    pthread_t current_thread = pthread_self();    

    const int result = pthread_setaffinity_np( current_thread, sizeof(cpu_set_t), &cpuset );
    if ( result != 0 )
    {
        char call[64];
        snprintf( call, sizeof( call ), "pthread_setaffinity_np cpu %d", cpu );
        rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_AFFINITY, "thread affinity", call, strerror( result ), "the thread runs on any cpu" );
    }
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return &server_socket->process_packets;
}

int rocketnet_server_socket_num_queues( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return server_socket->num_queues;
}

void rocketnet_server_socket_xdp_counters( struct rocketnet_server_socket_t * server_socket, uint64_t * counters )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( counters );

    memset( counters, 0, ROCKETNET_SERVER_XDP_NUM_COUNTERS * sizeof(uint64_t) );

    // the stats map is per-CPU: one value struct per possible CPU, summed here

    const int num_cpus = libbpf_num_possible_cpus();
    if ( num_cpus <= 0 )
        return;

    struct rocketnet_server_xdp_stats_t * values = (struct rocketnet_server_xdp_stats_t*) alloca( num_cpus * sizeof(struct rocketnet_server_xdp_stats_t) );

    __u32 key = 0;
    if ( bpf_map_lookup_elem( server_socket->stats_map_fd, &key, values ) != 0 )
        return;

    for ( int cpu = 0; cpu < num_cpus; cpu++ )
    {
        for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        {
            counters[i] += values[cpu].counters[i];
        }
    }
}

void rocketnet_server_socket_receive_packets( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    server_socket->process_packets.num_packets = 0;

    for ( int queue = 0; queue < server_socket->num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

        rocketnet_platform_mutex_acquire( &socket->receive_mutex );
        {
            socket->receive_counter++;
        }
        rocketnet_platform_mutex_release( &socket->receive_mutex );

        const int on_buffer = ( socket->receive_counter ) % ROCKETNET_XDP_NUM_BUFFERS;

        for ( int j = 0; j < ROCKETNET_XDP_NUM_BUFFERS; j++ )
        {
            if ( j == on_buffer )
                continue;

            rocketnet_server_socket_receive_buffer_t * receive_buffer = &socket->receive_buffer[j];

            for ( int i = 0; i < receive_buffer->num_packets; i++ )
            {
                uint8_t * eth = receive_buffer->eth[i];
                rocketnet_address_t from = receive_buffer->from[i];
                uint8_t * packet_data = receive_buffer->packet_data[i];
                const int packet_bytes = receive_buffer->packet_bytes[i];

                rocketnet_assert( packet_bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );

                rocketnet_server_socket_process_staged_packet( server_socket, eth, &from, packet_data, packet_bytes );
            }

            receive_buffer->num_packets = 0;
        }
    }        
}

static void rocketnet_server_socket_xdp_thread_function( void * data )
{
    rocketnet_server_xdp_socket_t * socket = (rocketnet_server_xdp_socket_t*) data;

    rocketnet_assert( socket );

    pin_thread_to_cpu( socket->num_os_cpus + socket->queue );

    // flow_server_socket_linux.cpp:1374-1381: pin, then SCHED_RR at the
    // minimum on itself. the priority call follows the off switch; the pin
    // is the queue's cpu and always runs
    if ( rocketnet_platform_thread_priority_enabled() )
    {
        struct sched_param param;
        param.sched_priority = sched_get_priority_min( SCHED_RR );
        const int result = pthread_setschedparam( pthread_self(), SCHED_RR, &param );
        if ( result != 0 )
        {
            char call[64];
            snprintf( call, sizeof( call ), "pthread_setschedparam SCHED_RR %d", param.sched_priority );
            rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_SCHED, "thread priority", call, strerror( result ), "the thread runs at its default priority" );
        }
    }

    rocketnet_info( "pinning xdp thread %d to CPU %d", socket->queue, socket->num_os_cpus + socket->queue );

    struct pollfd fds[1];
    fds[0].fd = xsk_socket__fd( socket->xsk );
    fds[0].events = POLLIN;

    int num_batch_packets = 0;
    int batch_frames[ROCKETNET_XDP_SEND_BATCH_SIZE];
    int batch_packet_bytes[ROCKETNET_XDP_SEND_BATCH_SIZE];

    rocketnet_pending_frames_t<ROCKETNET_XDP_RECV_QUEUE_SIZE> pending_return;
    auto return_pending_frames = [&]()
    {
        return pending_return.flush(
            [&](uint32_t count, uint32_t * index) { return xsk_ring_prod__reserve(&socket->fill_queue, count, index); },
            [&](uint32_t index, uint64_t frame) { *xsk_ring_prod__fill_addr(&socket->fill_queue, index) = frame; },
            [&](uint32_t count) { xsk_ring_prod__submit(&socket->fill_queue, count); });
    };
    bool reported_frame_pressure = false;

    while ( !socket->quit )
    {
        // busy poll the driver

        poll( fds, 1, 0 );

        // return pending frames to the fill queue

        // libxdp reserves the entire requested count or zero. A failed
        // reservation retains the entire batch for a later iteration.
        return_pending_frames();

        // see if there are any packets to receive

        uint32_t receive_index;
        
        uint32_t num_packets = xsk_ring_cons__peek( &socket->receive_queue, (uint32_t)pending_return.available(), &receive_index );

        if ( num_packets > 0 )
        {
            rocketnet_platform_mutex_acquire( &socket->receive_mutex );

            const int on_index = socket->receive_counter % ROCKETNET_XDP_NUM_BUFFERS;

            rocketnet_server_socket_receive_buffer_t * __restrict__ receive_buffer = &socket->receive_buffer[on_index];

            // receive packets

            // How many descriptors this pass actually took. The loop below can
            // stop early when the staging buffer fills, and everything after
            // this point -- the release, the refill, the pending-return spill --
            // must cover exactly that many, not the number peeked. Releasing
            // more would hand the kernel uninitialized stack as umem addresses:
            // rejected (leaking the frame forever) or in range and aliasing a
            // LIVE frame, which is the packet corruption the shouty comment at
            // rocketnet_constants.h:22 warns about.
            //
            // Descriptors left unconsumed stay in the ring and come back on the
            // next peek, which is the correct AF_XDP behaviour when a consumer
            // cannot keep up.
            uint32_t num_consumed = 0;

            uint64_t frame[ROCKETNET_XDP_RECV_QUEUE_SIZE];

            for ( uint32_t i = 0; i < num_packets; i++ ) 
            {
                if ( receive_buffer->num_packets >= ROCKETNET_XDP_RECV_QUEUE_SIZE )
                    break;

                const struct xdp_desc * __restrict__ desc = xsk_ring_cons__rx_desc( &socket->receive_queue, receive_index + i );

                frame[num_consumed] = desc->addr;
                num_consumed++;

                uint8_t * __restrict__ packet_data = (uint8_t*)socket->buffer + desc->addr;

                // the codec dispatches on the ethertype: IPv4 at its
                // offsets, IPv6 at the 40-byte fixed header, the from-address
                // in the family it read. a frame it does not take (another
                // ethertype, an IPv6 extension header, the family this server
                // does not serve, a frame too short for any server packet) is
                // counted by reason in receive_drops, never parsed at the
                // wrong offsets

                rocketnet_address_t from;
                const uint8_t * payload_data = NULL;
                int payload_bytes = 0;

                if ( rocketnet_xdp_frame_receive( packet_data, (size_t) desc->len, socket->server_address.type, &from, &payload_data, &payload_bytes, &socket->receive_drops ) )
                {
                    const int index = receive_buffer->num_packets;

                    receive_buffer->from[index] = from;

                    memcpy( receive_buffer->eth[index], packet_data + ETH_ALEN, ETH_ALEN );      // the ethernet source

                    memcpy( receive_buffer->packet_data[index], payload_data, payload_bytes );

                    receive_buffer->packet_bytes[index] = payload_bytes;

                    receive_buffer->num_packets++;
                }
            }

            rocketnet_platform_mutex_release( &socket->receive_mutex );

            xsk_ring_cons__release( &socket->receive_queue, num_consumed );

            // The peek was bounded by available pending storage, so every
            // consumed frame remains owned until a full fill reservation.
            const bool retained = pending_return.append(frame, num_consumed);
            rocketnet_assert(retained);
            if (!retained)
            {
                rocketnet_error("xdp pending frame ownership invariant failed");
                socket->quit = true;
                break;
            }
            return_pending_frames();
        }

        // mark any completed send packet frames as free to be reused

        uint32_t complete_index;

        unsigned int num_completed = xsk_ring_cons__peek( &socket->complete_queue, XSK_RING_CONS__DEFAULT_NUM_DESCS, &complete_index );

        if ( num_completed > 0 )
        {
            for ( int i = 0; i < num_completed; i++ )
            {
                uint64_t frame = *xsk_ring_cons__comp_addr( &socket->complete_queue, complete_index + i );
                free_send_frame( socket, frame );
            }

            xsk_ring_cons__release( &socket->complete_queue, num_completed );
        }

        // if we don't already have a batch of packets to send, get one

        if ( num_batch_packets == 0 )
        {
            rocketnet_platform_mutex_acquire( &socket->send_mutex );

            const int on_index = socket->send_handoff.front();
            if ( on_index >= 0 )
            {

                rocketnet_server_socket_send_buffer_t * __restrict__ send_buffer = &socket->send_buffer[on_index];

                // IMPORTANT: clamp num packets because it's an atomic increment across multiple threads so it can go over max if unlucky

                int num_packets = send_buffer->num_packets;

                if ( num_packets > ROCKETNET_XDP_SEND_QUEUE_SIZE )
                {
                    num_packets = ROCKETNET_XDP_SEND_QUEUE_SIZE;
                }

                rocketnet_assert( num_packets >= 0 );
                rocketnet_assert( num_packets <= ROCKETNET_XDP_SEND_QUEUE_SIZE );

                // grab a batch of non-zero byte packets from the send buffer

                const int start_index = send_buffer->packet_start_index;

                rocketnet_assert( start_index >= 0 );
                rocketnet_assert( start_index <= ROCKETNET_XDP_SEND_QUEUE_SIZE );

                for ( int i = start_index; i < num_packets; i++ )
                {
                    if ( num_batch_packets >= ROCKETNET_XDP_SEND_BATCH_SIZE )
                        break;

                    if ( send_buffer->packet_info[i].packet_bytes > 0 )
                    {
                        int frame = alloc_send_frame( socket );

                        if ( frame == INVALID_FRAME )
                        {
                            // Leave packet_start_index untouched. Completions in
                            // the next iteration may release frames; never kill
                            // the host or lose the staged packet on pressure.
                            if (!reported_frame_pressure)
                                rocketnet_error("xdp send frames exhausted; retaining staged packets for retry");
                            reported_frame_pressure = true;
                            break;
                        }
                        reported_frame_pressure = false;

                        uint8_t * packet_data = (uint8_t*)socket->buffer + frame;

                        const int payload_bytes = send_buffer->packet_info[i].packet_bytes;

                        // the server's own address is the source, in the
                        // family it serves; the codec builds IPv4 or IPv6
                        // (with the mandatory UDP checksum) and refuses a
                        // destination of the other family, which is counted
                        // and never sent as a malformed frame

                        const int header_bytes = rocketnet_xdp_frame_header_bytes( send_buffer->packet_info[i].to.type );
                        memcpy( packet_data + header_bytes, send_buffer->packet_data[i] + ROCKETNET_FRAME_WIRE_OFFSET, payload_bytes );

                        const int frame_bytes = rocketnet_xdp_frame_build( packet_data, ROCKETNET_XDP_FRAME_SIZE, socket->server_ethernet_address, send_buffer->packet_info[i].destination_mac, &socket->server_address, &send_buffer->packet_info[i].to, payload_bytes );
                        if ( frame_bytes <= 0 )
                        {
                            free_send_frame( socket, frame );
                            socket->send_refused_build++;
                        }
                        else
                        {
                            batch_frames[num_batch_packets] = frame;
                            batch_packet_bytes[num_batch_packets] = frame_bytes;
                            num_batch_packets++;
                        }
                    }

                    send_buffer->packet_start_index++;
                }

                // UMEM now owns every copied packet. Only now may the producer
                // reset/reuse this staging generation, even across repeated flushes.
                if ( send_buffer->packet_start_index == num_packets )
                    socket->send_handoff.retire();
            }

            rocketnet_platform_mutex_release( &socket->send_mutex );
        }

        // actually send the packets

        if ( num_batch_packets > 0 )
        {
            // reserve entries in the send queue

            uint32_t send_queue_index;
            int send_packets = xsk_ring_prod__reserve( &socket->send_queue, num_batch_packets, &send_queue_index );

            // All-or-zero: zero keeps the batch intact for retry.
            if ( send_packets == num_batch_packets )
            {
                // setup descriptors for packets in batch to be sent

                for ( int i = 0; i < send_packets; i++ )
                {
                    struct xdp_desc * __restrict__ desc = xsk_ring_prod__tx_desc( &socket->send_queue, send_queue_index + i );
                    desc->addr = batch_frames[i];
                    desc->len = batch_packet_bytes[i];
                }

                // submit send queue to driver

                xsk_ring_prod__submit( &socket->send_queue, send_packets );

                // batch has been sent

                num_batch_packets = 0;
            }
        }

        if ( xsk_ring_prod__needs_wakeup( &socket->send_queue ) )
        {
            sendto( xsk_socket__fd( socket->xsk ), NULL, 0, MSG_DONTWAIT, NULL, 0 );
        }

        // IMPORTANT: Sleep so the driver gets all the time it wants. Otherwise, it can hitch us

        rocketnet_platform_sleep( 0.0 );
    }
}

#else // #if ROCKETNET_XDP

int rocketnet_server_socket_linux_cpp_dummy = 0;

#endif // #if ROCKETNET_XDP
