/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// XDP's IPv4 gateway discovery over captured command text. The server reads
// `netstat -rn` and `ip neigh show` through popen line by line and hands each
// line to a pure parser (sdk/rocketnet_server_socket_linux.cpp,
// rocketnet_server_socket_ipv4_route_line_gateway and
// rocketnet_server_socket_ipv4_neigh_line_mac); the first line a parser
// accepts wins. These cases feed the parsers captured text only: no popen,
// no socket, no netlink.
//
// The parsers match whole whitespace-separated tokens: the route is the
// default route (destination and genmask 0.0.0.0, flags U and G) whose Iface
// column is the interface, and the neighbor is the line whose first token is
// the gateway address and whose dev token is the interface. A neighbor line
// for 10.0.0.12 never answers for 10.0.0.1, eth10 never answers for eth1, and
// a non-default UG route listed first never wins.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#define check rocketnet_unit_check

#if ROCKETNET_XDP

#include <arpa/inet.h>

bool rocketnet_server_socket_ipv4_route_line_gateway( const char * line, const char * interface_name, char * gateway_ip_string, size_t gateway_ip_size );
bool rocketnet_server_socket_ipv4_neigh_line_mac( const char * line, const char * gateway_ip_string, const char * interface_name, char * mac_address_string );

// walks captured text one line at a time as the server's fgets loop does;
// the first line the parser accepts wins. false when no line is accepted.

static bool gateway_from_netstat( const char * text, const char * interface_name, char * gateway, size_t gateway_size )
{
    gateway[0] = '\0';
    char line[1024];
    const char * cursor = text;
    while ( *cursor )
    {
        const char * end = strchr( cursor, '\n' );
        const size_t length = end ? (size_t) ( end - cursor + 1 ) : strlen( cursor );
        check( length < sizeof(line) );
        memcpy( line, cursor, length );
        line[length] = '\0';
        cursor += length;
        if ( rocketnet_server_socket_ipv4_route_line_gateway( line, interface_name, gateway, gateway_size ) )
            return true;
    }
    return false;
}

static bool mac_from_neigh( const char * text, const char * gateway, const char * interface_name, char * mac )
{
    mac[0] = '\0';
    char line[1024];
    const char * cursor = text;
    while ( *cursor )
    {
        const char * end = strchr( cursor, '\n' );
        const size_t length = end ? (size_t) ( end - cursor + 1 ) : strlen( cursor );
        check( length < sizeof(line) );
        memcpy( line, cursor, length );
        line[length] = '\0';
        cursor += length;
        if ( rocketnet_server_socket_ipv4_neigh_line_mac( line, gateway, interface_name, mac ) )
            return true;
    }
    return false;
}

static const char * netstat_header =
    "Kernel IP routing table\n"
    "Destination     Gateway         Genmask         Flags   MSS Window  irtt Iface\n";

static void xdp_gateway_parse_matches_the_default_route_and_its_neighbor()
{
    char netstat[2048];
    snprintf( netstat, sizeof(netstat), "%s%s", netstat_header,
        "0.0.0.0         10.0.0.1        0.0.0.0         UG        0 0          0 eth1\n"
        "10.0.0.0        0.0.0.0         255.255.255.0   U         0 0          0 eth1\n" );

    char gateway[INET_ADDRSTRLEN + 1];
    check( gateway_from_netstat( netstat, "eth1", gateway, sizeof(gateway) ) );
    check( strcmp( gateway, "10.0.0.1" ) == 0 );

    // no default route on this interface: nothing is chosen

    check( !gateway_from_netstat( netstat, "eth0", gateway, sizeof(gateway) ) );

    const char * neigh =
        "10.0.0.7 dev eth1 FAILED\n"
        "fe80::1 dev eth1 lladdr 02:00:00:00:00:99 router STALE\n"
        "10.0.0.1 dev eth1 lladdr 02:00:00:00:00:01 REACHABLE\n";

    char mac[18];
    check( mac_from_neigh( neigh, "10.0.0.1", "eth1", mac ) );
    check( strcmp( mac, "02:00:00:00:00:01" ) == 0 );

    // a gateway with no neighbor entry, or one with no lladdr, is not found

    check( !mac_from_neigh( neigh, "10.0.0.2", "eth1", mac ) );
    check( !mac_from_neigh( neigh, "10.0.0.7", "eth1", mac ) );
}

static void xdp_gateway_parse_neighbor_address_is_a_whole_token()
{
    // 10.0.0.1 is a substring of 10.0.0.12, listed first

    const char * neigh =
        "10.0.0.12 dev eth1 lladdr 02:00:00:00:00:12 REACHABLE\n"
        "10.0.0.1 dev eth1 lladdr 02:00:00:00:00:01 REACHABLE\n";

    char mac[18];
    check( mac_from_neigh( neigh, "10.0.0.1", "eth1", mac ) );
    check( strcmp( mac, "02:00:00:00:00:01" ) == 0 );
}

static void xdp_gateway_parse_interface_is_a_whole_token()
{
    // eth1 is a substring of eth10, listed first, in both tables

    char netstat[2048];
    snprintf( netstat, sizeof(netstat), "%s%s", netstat_header,
        "0.0.0.0         192.168.10.1    0.0.0.0         UG        0 0          0 eth10\n"
        "0.0.0.0         10.0.0.1        0.0.0.0         UG        0 0          0 eth1\n" );

    char gateway[INET_ADDRSTRLEN + 1];
    check( gateway_from_netstat( netstat, "eth1", gateway, sizeof(gateway) ) );
    check( strcmp( gateway, "10.0.0.1" ) == 0 );

    const char * neigh =
        "10.0.0.1 dev eth10 lladdr 02:00:00:00:10:01 STALE\n"
        "10.0.0.1 dev eth1 lladdr 02:00:00:00:00:01 REACHABLE\n";

    char mac[18];
    check( mac_from_neigh( neigh, "10.0.0.1", "eth1", mac ) );
    check( strcmp( mac, "02:00:00:00:00:01" ) == 0 );
}

static void xdp_gateway_parse_only_the_default_route_wins()
{
    // a non-default UG route (and a UGH host route) listed before the default

    char netstat[2048];
    snprintf( netstat, sizeof(netstat), "%s%s", netstat_header,
        "172.16.0.0      10.0.0.254      255.255.0.0     UG        0 0          0 eth1\n"
        "198.51.100.7    10.0.0.253      255.255.255.255 UGH       0 0          0 eth1\n"
        "0.0.0.0         10.0.0.1        0.0.0.0         UG        0 0          0 eth1\n" );

    char gateway[INET_ADDRSTRLEN + 1];
    check( gateway_from_netstat( netstat, "eth1", gateway, sizeof(gateway) ) );
    check( strcmp( gateway, "10.0.0.1" ) == 0 );
}

#else

static void xdp_gateway_parse_matches_the_default_route_and_its_neighbor()
{
    printf( "xdp_gateway_parse_matches_the_default_route_and_its_neighbor [SKIPPED AF_XDP is Linux only]\n" );
    fflush( stdout );
}

static void xdp_gateway_parse_neighbor_address_is_a_whole_token()
{
    printf( "xdp_gateway_parse_neighbor_address_is_a_whole_token [SKIPPED AF_XDP is Linux only]\n" );
    fflush( stdout );
}

static void xdp_gateway_parse_interface_is_a_whole_token()
{
    printf( "xdp_gateway_parse_interface_is_a_whole_token [SKIPPED AF_XDP is Linux only]\n" );
    fflush( stdout );
}

static void xdp_gateway_parse_only_the_default_route_wins()
{
    printf( "xdp_gateway_parse_only_the_default_route_wins [SKIPPED AF_XDP is Linux only]\n" );
    fflush( stdout );
}

#endif

REGISTER_UNIT_TEST( xdp_gateway_parse_matches_the_default_route_and_its_neighbor );
REGISTER_UNIT_TEST( xdp_gateway_parse_neighbor_address_is_a_whole_token );
REGISTER_UNIT_TEST( xdp_gateway_parse_interface_is_a_whole_token );
REGISTER_UNIT_TEST( xdp_gateway_parse_only_the_default_route_wins );
