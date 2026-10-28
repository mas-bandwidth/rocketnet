/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// XDP's gateway discovery over canned rtnetlink replies. The server asks the
// kernel for an RTM_GETROUTE dump and then an RTM_GETNEIGH dump
// (sdk/rocketnet_gateway_netlink.h) and hands each received datagram to a
// pure parser. These cases build the datagrams here, byte by byte from the
// uapi structs (nlmsghdr, rtmsg, ndmsg, rtattr, rtnexthop, nlmsgerr), and
// feed them to the parsers: no socket, no netlink, no real time.
//
// The route parser takes the first default route (destination length 0,
// unicast, main table, not cloned) of the family whose output interface is
// the server's; the neighbour parser takes the family's entry on that
// interface whose destination is the gateway, in a usable state, with a
// 6-byte link-layer address. Anything that overruns its bounds refuses.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#define check rocketnet_unit_check

#if ROCKETNET_XDP

#include "rocketnet_gateway_netlink.h"

#include <errno.h>
#include <string.h>
#include <vector>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

// the request's sequence number and the port id the kernel assigned this
// socket; replies carry both

static const uint32_t netlink_sequence = 1;
static const uint32_t netlink_port_id = 4242;

static const int server_interface = 3;
static const int other_interface = 4;

static const uint8_t gateway_ipv4[4] = { 10, 0, 0, 1 };
static const uint8_t gateway_ipv4_other[4] = { 10, 0, 0, 12 };
static const uint8_t subnet_ipv4[4] = { 10, 0, 0, 0 };
static const uint8_t gateway_ipv6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01 };
static const uint8_t gateway_ipv6_other[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02 };
static const uint8_t subnet_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t other_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 };

// ---------------------------------------------------------------------------
// building datagrams

struct netlink_datagram_t
{
    std::vector<uint8_t> bytes;
};

static void netlink_append( netlink_datagram_t * datagram, const void * data, size_t bytes )
{
    const uint8_t * p = (const uint8_t *) data;
    datagram->bytes.insert( datagram->bytes.end(), p, p + bytes );
}

static void netlink_pad( netlink_datagram_t * datagram )
{
    while ( datagram->bytes.size() % 4 )
    {
        datagram->bytes.push_back( 0 );
    }
}

// a message header at the end of the datagram; netlink_end writes its length

static size_t netlink_begin( netlink_datagram_t * datagram, uint16_t type, uint16_t flags, uint32_t sequence = netlink_sequence, uint32_t port_id = netlink_port_id )
{
    const size_t start = datagram->bytes.size();
    struct nlmsghdr header;
    memset( &header, 0, sizeof( header ) );
    header.nlmsg_type = type;
    header.nlmsg_flags = flags;
    header.nlmsg_seq = sequence;
    header.nlmsg_pid = port_id;
    netlink_append( datagram, &header, sizeof( header ) );

    return start;
}

static void netlink_end( netlink_datagram_t * datagram, size_t start )
{
    const uint32_t length = (uint32_t) ( datagram->bytes.size() - start );
    memcpy( datagram->bytes.data() + start, &length, sizeof( length ) );
    netlink_pad( datagram );
}

static void netlink_attribute( netlink_datagram_t * datagram, uint16_t type, const void * data, size_t bytes )
{
    struct rtattr attribute;
    attribute.rta_len = (unsigned short) RTA_LENGTH( bytes );
    attribute.rta_type = type;
    netlink_append( datagram, &attribute, sizeof( attribute ) );
    netlink_append( datagram, data, bytes );
    netlink_pad( datagram );
}

static void netlink_attribute_u32( netlink_datagram_t * datagram, uint16_t type, uint32_t value )
{
    netlink_attribute( datagram, type, &value, sizeof( value ) );
}

// one route as the kernel dumps it: an rtmsg and RTA_TABLE, RTA_DST when
// not a default route, RTA_OIF and RTA_GATEWAY when given

struct netlink_route_t
{
    int family = AF_INET;
    unsigned char destination_length = 0;
    const uint8_t * destination = NULL;
    uint32_t table = RT_TABLE_MAIN;
    int rtm_table = -1;                             // -1: the table, or RT_TABLE_COMPAT past 255, as the kernel writes it
    unsigned char type = RTN_UNICAST;
    unsigned int flags = 0;
    int output_interface = server_interface;       // 0: no RTA_OIF
    const uint8_t * gateway = NULL;                 // NULL: no RTA_GATEWAY
    size_t gateway_bytes = 0;                       // 0: the family's address size
    uint32_t sequence = netlink_sequence;
    uint32_t port_id = netlink_port_id;
};

static size_t netlink_address_bytes( int family )
{
    return family == AF_INET ? 4 : 16;
}

static void netlink_route( netlink_datagram_t * datagram, const netlink_route_t & route )
{
    const size_t start = netlink_begin( datagram, RTM_NEWROUTE, NLM_F_MULTI, route.sequence, route.port_id );
    struct rtmsg message;
    memset( &message, 0, sizeof( message ) );
    message.rtm_family = (unsigned char) route.family;
    message.rtm_dst_len = route.destination_length;
    message.rtm_table = (unsigned char) ( route.rtm_table >= 0 ? (uint32_t) route.rtm_table : route.table < 256 ? route.table
                                                                                                                : (uint32_t) RT_TABLE_COMPAT );
    message.rtm_protocol = RTPROT_BOOT;
    message.rtm_scope = RT_SCOPE_UNIVERSE;
    message.rtm_type = route.type;
    message.rtm_flags = route.flags;
    netlink_append( datagram, &message, sizeof( message ) );
    netlink_attribute_u32( datagram, RTA_TABLE, route.table );
    if ( route.destination )
    {
        netlink_attribute( datagram, RTA_DST, route.destination, netlink_address_bytes( route.family ) );
    }

    if ( route.gateway )
    {
        netlink_attribute( datagram, RTA_GATEWAY, route.gateway, route.gateway_bytes ? route.gateway_bytes : netlink_address_bytes( route.family ) );
    }

    if ( route.output_interface )
    {
        netlink_attribute_u32( datagram, RTA_OIF, (uint32_t) route.output_interface );
    }

    netlink_end( datagram, start );
}

// one neighbour entry: an ndmsg, NDA_DST and NDA_LLADDR when given

struct netlink_neighbor_t
{
    int family = AF_INET;
    int interface_index = server_interface;
    uint16_t state = NUD_REACHABLE;
    const uint8_t * destination = NULL;
    const uint8_t * lladdr = NULL;                  // NULL: no NDA_LLADDR
    size_t lladdr_bytes = 6;
};

static void netlink_neighbor( netlink_datagram_t * datagram, const netlink_neighbor_t & neighbor )
{
    const size_t start = netlink_begin( datagram, RTM_NEWNEIGH, NLM_F_MULTI );
    struct ndmsg message;
    memset( &message, 0, sizeof( message ) );
    message.ndm_family = (unsigned char) neighbor.family;
    message.ndm_ifindex = neighbor.interface_index;
    message.ndm_state = neighbor.state;
    message.ndm_type = RTN_UNICAST;
    netlink_append( datagram, &message, sizeof( message ) );

    if ( neighbor.destination )
    {
        netlink_attribute( datagram, NDA_DST, neighbor.destination, netlink_address_bytes( neighbor.family ) );
    }

    if ( neighbor.lladdr )
    {
        netlink_attribute( datagram, NDA_LLADDR, neighbor.lladdr, neighbor.lladdr_bytes );
    }

    netlink_end( datagram, start );
}

// NLMSG_DONE carrying the dump's result

static void netlink_done( netlink_datagram_t * datagram, int code = 0, uint32_t sequence = netlink_sequence, uint32_t port_id = netlink_port_id )
{
    const size_t start = netlink_begin( datagram, NLMSG_DONE, NLM_F_MULTI, sequence, port_id );
    netlink_append( datagram, &code, sizeof( code ) );
    netlink_end( datagram, start );
}

static void netlink_error( netlink_datagram_t * datagram, int code )
{
    const size_t start = netlink_begin( datagram, NLMSG_ERROR, 0 );
    struct nlmsgerr error;
    memset( &error, 0, sizeof( error ) );
    error.error = code;
    netlink_append( datagram, &error, sizeof( error ) );
    netlink_end( datagram, start );
}

static int parse_route( const netlink_datagram_t & datagram, int family, uint8_t * gateway, int * error )
{
    memset( gateway, 0xEE, 16 );
    return rocketnet_gateway_netlink_parse_route( datagram.bytes.data(), datagram.bytes.size(), netlink_sequence, netlink_port_id,
                                                  family, server_interface, gateway, error );
}

static int parse_neighbor( const netlink_datagram_t & datagram, int family, const uint8_t * gateway, uint8_t * mac, int * error )
{
    memset( mac, 0xEE, 6 );
    return rocketnet_gateway_netlink_parse_neighbor( datagram.bytes.data(), datagram.bytes.size(), netlink_sequence, netlink_port_id,
                                                     family, server_interface, gateway, mac, error );
}

// ---------------------------------------------------------------------------
// the default route

// the routes a family's table lists ahead of its default route on this
// interface: a subnet route on this interface and a default route on
// another interface

static void route_table_before_default( netlink_datagram_t * datagram, int family )
{
    const bool ipv4 = family == AF_INET;

    netlink_route_t subnet;
    subnet.family = family;
    subnet.destination_length = ipv4 ? 24 : 64;
    subnet.destination = ipv4 ? subnet_ipv4 : subnet_ipv6;
    subnet.gateway = ipv4 ? gateway_ipv4_other : gateway_ipv6_other;
    netlink_route( datagram, subnet );

    netlink_route_t elsewhere;
    elsewhere.family = family;
    elsewhere.output_interface = other_interface;
    elsewhere.gateway = ipv4 ? gateway_ipv4_other : gateway_ipv6_other;
    netlink_route( datagram, elsewhere );
}

static void check_default_route_found( int family )
{
    const bool ipv4 = family == AF_INET;
    const uint8_t * want = ipv4 ? gateway_ipv4 : gateway_ipv6;

    netlink_datagram_t datagram;
    route_table_before_default( &datagram, family );

    netlink_route_t route;
    route.family = family;
    route.gateway = want;
    netlink_route( &datagram, route );
    netlink_done( &datagram );

    uint8_t gateway[16];
    int error = -1;
    check( parse_route( datagram, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, want, netlink_address_bytes( family ) ) == 0 );
    check( error == 0 );
}

static void check_default_route_not_on_this_interface( int family )
{
    const bool ipv4 = family == AF_INET;

    netlink_datagram_t datagram;
    route_table_before_default( &datagram, family );
    netlink_done( &datagram );

    uint8_t gateway[16];
    int error = -1;
    check( parse_route( datagram, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
    check( error == 0 );

    // nor does a default route with no gateway (a device route) on this
    // interface answer

    netlink_datagram_t device;
    netlink_route_t route;
    route.family = family;
    netlink_route( &device, route );

    netlink_route_t elsewhere;
    elsewhere.family = family;
    elsewhere.output_interface = other_interface;
    elsewhere.gateway = ipv4 ? gateway_ipv4 : gateway_ipv6;
    netlink_route( &device, elsewhere );
    netlink_done( &device );
    check( parse_route( device, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
}

static void check_no_default_route( int family )
{
    const bool ipv4 = family == AF_INET;

    netlink_route_t subnet;
    subnet.family = family;
    subnet.destination_length = ipv4 ? 8 : 48;
    subnet.destination = ipv4 ? subnet_ipv4 : subnet_ipv6;
    subnet.gateway = ipv4 ? gateway_ipv4 : gateway_ipv6;

    // the dump's first datagram holds no answer and no NLMSG_DONE: read on

    netlink_datagram_t first;
    netlink_route( &first, subnet );

    uint8_t gateway[16];
    int error = -1;
    check( parse_route( first, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MORE );

    // the empty table: only NLMSG_DONE

    netlink_datagram_t empty;
    netlink_done( &empty );
    check( parse_route( empty, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );

    netlink_datagram_t last;
    netlink_route( &last, subnet );
    netlink_done( &last );
    check( parse_route( last, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
}

static void test_xdp_transport_gateway_netlink_ipv4_default_route_found()
{
    check_default_route_found( AF_INET );
}

static void test_xdp_transport_gateway_netlink_ipv4_default_route_not_on_this_interface()
{
    check_default_route_not_on_this_interface( AF_INET );
}

static void test_xdp_transport_gateway_netlink_ipv4_no_default_route()
{
    check_no_default_route( AF_INET );
}

static void test_xdp_transport_gateway_netlink_ipv6_default_route_found()
{
    check_default_route_found( AF_INET6 );
}

static void test_xdp_transport_gateway_netlink_ipv6_default_route_not_on_this_interface()
{
    check_default_route_not_on_this_interface( AF_INET6 );
}

static void test_xdp_transport_gateway_netlink_ipv6_no_default_route()
{
    check_no_default_route( AF_INET6 );
}

// what ip(8) and /proc/net/route never listed as this interface's default
// route: another family's, another table's (local, a policy table past 255
// carried only in RTA_TABLE), a route that is not unicast, a cloned route.
// Listed ahead of the real one, none of them wins.

static void test_xdp_transport_gateway_netlink_only_the_main_unicast_default_route_wins()
{
    for ( int f = 0; f < 2; f++ )
    {
        const int family = f == 0 ? AF_INET : AF_INET6;
        const bool ipv4 = family == AF_INET;
        const uint8_t * other = ipv4 ? gateway_ipv4_other : gateway_ipv6_other;
        const uint8_t * want = ipv4 ? gateway_ipv4 : gateway_ipv6;

        netlink_datagram_t datagram;

        netlink_route_t family_route;
        family_route.family = ipv4 ? AF_INET6 : AF_INET;
        family_route.gateway = ipv4 ? gateway_ipv6_other : gateway_ipv4_other;
        netlink_route( &datagram, family_route );

        netlink_route_t local;
        local.family = family;
        local.table = RT_TABLE_LOCAL;
        local.gateway = other;
        netlink_route( &datagram, local );

        netlink_route_t policy;
        policy.family = family;
        policy.table = 1000;
        policy.gateway = other;
        netlink_route( &datagram, policy );

        // RTA_TABLE names the table when it is present, whatever the
        // rtmsg's one-byte field says

        netlink_route_t disagree;
        disagree.family = family;
        disagree.table = 1000;
        disagree.rtm_table = RT_TABLE_MAIN;
        disagree.gateway = other;
        netlink_route( &datagram, disagree );

        netlink_route_t unreachable;
        unreachable.family = family;
        unreachable.type = RTN_UNREACHABLE;
        unreachable.gateway = other;
        netlink_route( &datagram, unreachable );

        netlink_route_t cloned;
        cloned.family = family;
        cloned.flags = RTM_F_CLONED;
        cloned.gateway = other;
        netlink_route( &datagram, cloned );

        netlink_route_t route;
        route.family = family;
        route.gateway = want;
        netlink_route( &datagram, route );
        netlink_done( &datagram );

        uint8_t gateway[16];
        int error = -1;
        check( parse_route( datagram, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
        check( memcmp( gateway, want, netlink_address_bytes( family ) ) == 0 );
    }
}

// a multipath default route: two next hops, each an rtnexthop plus an
// RTA_GATEWAY of the family's address size

static void netlink_multipath_route( netlink_datagram_t * datagram, int family, int first_interface, const uint8_t * first_gateway, int second_interface, const uint8_t * second_gateway )
{
    const size_t address_bytes = family == AF_INET ? 4 : 16;
    const size_t start = netlink_begin( datagram, RTM_NEWROUTE, NLM_F_MULTI );
    struct rtmsg message;
    memset( &message, 0, sizeof( message ) );
    message.rtm_family = (unsigned char) family;
    message.rtm_table = RT_TABLE_MAIN;
    message.rtm_type = RTN_UNICAST;
    netlink_append( datagram, &message, sizeof( message ) );
    netlink_attribute_u32( datagram, RTA_TABLE, RT_TABLE_MAIN );

    std::vector<uint8_t> hops;
    const int interfaces[2] = { first_interface, second_interface };
    const uint8_t * gateways[2] = { first_gateway, second_gateway };

    for ( int i = 0; i < 2; i++ )
    {
        struct rtnexthop hop;
        memset( &hop, 0, sizeof( hop ) );
        hop.rtnh_len = (unsigned short) ( sizeof( hop ) + RTA_LENGTH( address_bytes ) );
        hop.rtnh_ifindex = interfaces[i];
        const uint8_t * h = (const uint8_t *) &hop;
        hops.insert( hops.end(), h, h + sizeof( hop ) );

        struct rtattr attribute;
        attribute.rta_len = (unsigned short) RTA_LENGTH( address_bytes );
        attribute.rta_type = RTA_GATEWAY;
        const uint8_t * a = (const uint8_t *) &attribute;
        hops.insert( hops.end(), a, a + sizeof( attribute ) );
        hops.insert( hops.end(), gateways[i], gateways[i] + address_bytes );
    }

    netlink_attribute( datagram, RTA_MULTIPATH, hops.data(), hops.size() );
    netlink_end( datagram, start );
}

static void test_xdp_transport_gateway_netlink_ipv4_multipath_first_next_hop()
{
    uint8_t gateway[16];
    int error = -1;

    netlink_datagram_t ours;
    netlink_multipath_route( &ours, AF_INET, server_interface, gateway_ipv4, other_interface, gateway_ipv4_other );
    netlink_done( &ours );
    check( parse_route( ours, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );

    // this interface as the second next hop: that hop is the route

    netlink_datagram_t second;
    netlink_multipath_route( &second, AF_INET, other_interface, gateway_ipv4_other, server_interface, gateway_ipv4 );
    netlink_done( &second );
    check( parse_route( second, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );
}

static void test_xdp_transport_gateway_netlink_ipv6_multipath_found()
{
    uint8_t gateway[16];
    int error = -1;
    netlink_datagram_t datagram;
    netlink_multipath_route( &datagram, AF_INET6, other_interface, gateway_ipv6_other, server_interface, gateway_ipv6 );
    netlink_done( &datagram );
    check( parse_route( datagram, AF_INET6, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv6, 16 ) == 0 );
}

static void test_xdp_transport_gateway_netlink_ipv4_multipath_later_next_hop()
{
    uint8_t gateway[16];
    int error = -1;
    netlink_datagram_t datagram;
    netlink_multipath_route( &datagram, AF_INET, other_interface, gateway_ipv4_other, server_interface, gateway_ipv4 );
    netlink_done( &datagram );
    check( parse_route( datagram, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );
}

// a gateway attribute that is not the family's address size refuses

static void test_xdp_transport_gateway_netlink_gateway_wrong_size_refuses()
{
    for ( int f = 0; f < 2; f++ )
    {
        const int family = f == 0 ? AF_INET : AF_INET6;
        netlink_datagram_t datagram;
        netlink_route_t route;
        route.family = family;
        route.gateway = gateway_ipv6;
        route.gateway_bytes = family == AF_INET ? 16 : 4;
        netlink_route( &datagram, route );
        netlink_done( &datagram );

        uint8_t gateway[16];
        int error = -1;
        check( parse_route( datagram, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
    }
}

// ---------------------------------------------------------------------------
// the gateway's neighbour entry

static void check_neighbor_found( int family )
{
    const bool ipv4 = family == AF_INET;
    const uint8_t * gateway = ipv4 ? gateway_ipv4 : gateway_ipv6;

    // ahead of it: another address on this interface, whose bytes start
    // like the gateway's (10.0.0.12 ahead of 10.0.0.1)

    netlink_datagram_t datagram;
    netlink_neighbor_t other;
    other.family = family;
    other.destination = ipv4 ? gateway_ipv4_other : gateway_ipv6_other;
    other.lladdr = other_mac;
    netlink_neighbor( &datagram, other );

    netlink_neighbor_t neighbor;
    neighbor.family = family;
    neighbor.destination = gateway;
    neighbor.lladdr = gateway_mac;
    neighbor.state = NUD_STALE;
    netlink_neighbor( &datagram, neighbor );
    netlink_done( &datagram );

    uint8_t mac[6];
    int error = -1;
    check( parse_neighbor( datagram, family, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( mac, gateway_mac, 6 ) == 0 );

    // every state ip(8) listed with an lladdr answers

    const uint16_t states[5] = { NUD_REACHABLE, NUD_STALE, NUD_DELAY, NUD_PROBE, NUD_PERMANENT };

    for ( int i = 0; i < 5; i++ )
    {
        netlink_datagram_t one;
        neighbor.state = states[i];
        netlink_neighbor( &one, neighbor );
        netlink_done( &one );
        check( parse_neighbor( one, family, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
        check( memcmp( mac, gateway_mac, 6 ) == 0 );
    }
}

static void test_xdp_transport_gateway_netlink_neighbor_lladdr_found()
{
    check_neighbor_found( AF_INET );
    check_neighbor_found( AF_INET6 );
}

static void test_xdp_transport_gateway_netlink_neighbor_wrong_interface()
{
    for ( int f = 0; f < 2; f++ )
    {
        const int family = f == 0 ? AF_INET : AF_INET6;
        const uint8_t * gateway = family == AF_INET ? gateway_ipv4 : gateway_ipv6;

        netlink_datagram_t datagram;
        netlink_neighbor_t neighbor;
        neighbor.family = family;
        neighbor.interface_index = other_interface;
        neighbor.destination = gateway;
        neighbor.lladdr = other_mac;
        netlink_neighbor( &datagram, neighbor );
        netlink_done( &datagram );

        uint8_t mac[6];
        int error = -1;
        check( parse_neighbor( datagram, family, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );

        // then the entry on this interface, after it in the same dump

        netlink_datagram_t both;
        netlink_neighbor( &both, neighbor );
        neighbor.interface_index = server_interface;
        neighbor.lladdr = gateway_mac;
        netlink_neighbor( &both, neighbor );
        netlink_done( &both );
        check( parse_neighbor( both, family, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
        check( memcmp( mac, gateway_mac, 6 ) == 0 );
    }
}

// an entry with no link-layer address, or one that is not 6 bytes, is passed
// over: ip(8) printed no 17-character lladdr for it

static void test_xdp_transport_gateway_netlink_neighbor_no_lladdr()
{
    for ( int f = 0; f < 2; f++ )
    {
        const int family = f == 0 ? AF_INET : AF_INET6;
        const uint8_t * gateway = family == AF_INET ? gateway_ipv4 : gateway_ipv6;

        netlink_datagram_t datagram;
        netlink_neighbor_t neighbor;
        neighbor.family = family;
        neighbor.destination = gateway;
        netlink_neighbor( &datagram, neighbor );
        neighbor.lladdr = other_mac;
        neighbor.lladdr_bytes = 4;
        netlink_neighbor( &datagram, neighbor );
        netlink_done( &datagram );

        uint8_t mac[6];
        int error = -1;
        check( parse_neighbor( datagram, family, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
    }
}

// the states with no usable address, and NOARP, which ip(8) neigh show does
// not list: passed over even when an address is attached

static void test_xdp_transport_gateway_netlink_neighbor_failed_state()
{
    const uint16_t states[4] = { NUD_FAILED, NUD_INCOMPLETE, NUD_NOARP, NUD_NONE };

    for ( int f = 0; f < 2; f++ )
    {
        const int family = f == 0 ? AF_INET : AF_INET6;
        const uint8_t * gateway = family == AF_INET ? gateway_ipv4 : gateway_ipv6;
        for ( int i = 0; i < 4; i++ )
        {
            netlink_datagram_t datagram;
            netlink_neighbor_t neighbor;
            neighbor.family = family;
            neighbor.state = states[i];
            neighbor.destination = gateway;
            neighbor.lladdr = other_mac;
            netlink_neighbor( &datagram, neighbor );
            netlink_done( &datagram );

            uint8_t mac[6];
            int error = -1;
            check( parse_neighbor( datagram, family, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
        }
    }
}

// a neighbour entry of the other family never answers, even one whose
// destination is this family's size and the gateway's bytes

static void test_xdp_transport_gateway_netlink_neighbor_other_family()
{
    for ( int f = 0; f < 2; f++ )
    {
        const int family = f == 0 ? AF_INET : AF_INET6;
        const uint8_t * gateway = family == AF_INET ? gateway_ipv4 : gateway_ipv6;

        netlink_datagram_t datagram;
        const size_t start = netlink_begin( &datagram, RTM_NEWNEIGH, NLM_F_MULTI );
        struct ndmsg message;
        memset( &message, 0, sizeof( message ) );
        message.ndm_family = (unsigned char) ( family == AF_INET ? AF_INET6 : AF_INET );
        message.ndm_ifindex = server_interface;
        message.ndm_state = NUD_REACHABLE;
        netlink_append( &datagram, &message, sizeof( message ) );
        netlink_attribute( &datagram, NDA_DST, gateway, netlink_address_bytes( family ) );
        netlink_attribute( &datagram, NDA_LLADDR, other_mac, 6 );
        netlink_end( &datagram, start );
        netlink_done( &datagram );

        uint8_t mac[6];
        int error = -1;
        check( parse_neighbor( datagram, family, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
    }
}

// ---------------------------------------------------------------------------
// malformed and out-of-band replies

static netlink_datagram_t default_route_datagram()
{
    netlink_datagram_t datagram;
    netlink_route_t route;
    route.gateway = gateway_ipv4;
    netlink_route( &datagram, route );
    return datagram;
}

// a datagram cut inside a message, or inside a header, refuses; a
// message length past the datagram refuses

static void test_xdp_transport_gateway_netlink_truncated_message()
{
    const netlink_datagram_t whole = default_route_datagram();
    uint8_t gateway[16];
    int error = -1;

    for ( size_t cut = 1; cut < whole.bytes.size(); cut++ )
    {
        netlink_datagram_t datagram = whole;
        datagram.bytes.resize( cut );
        check( parse_route( datagram, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
    }

    // the length field claims four bytes more than the datagram holds

    netlink_datagram_t longer = whole;
    uint32_t length;
    memcpy( &length, longer.bytes.data(), sizeof( length ) );
    length += 4;
    memcpy( longer.bytes.data(), &length, sizeof( length ) );
    check( parse_route( longer, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    // a length shorter than the header itself

    netlink_datagram_t shorter = whole;
    length = sizeof( struct nlmsghdr ) - 1;
    memcpy( shorter.bytes.data(), &length, sizeof( length ) );
    check( parse_route( shorter, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    // a message with no room for its rtmsg

    netlink_datagram_t bodyless;
    const size_t start = netlink_begin( &bodyless, RTM_NEWROUTE, NLM_F_MULTI );
    netlink_end( &bodyless, start );
    check( parse_route( bodyless, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
}

// an attribute whose rta_len runs past the end of its message refuses, even
// when the datagram has bytes after it; so does one shorter than its header

static void test_xdp_transport_gateway_netlink_attribute_past_the_end()
{
    uint8_t gateway[16];
    int error = -1;

    // the gateway attribute is the second of three (RTA_TABLE, RTA_GATEWAY,
    // RTA_OIF): its rta_len sits 8 bytes after the end of RTA_TABLE

    const size_t gateway_attribute = NLMSG_HDRLEN + NLMSG_ALIGN( sizeof( struct rtmsg ) ) + RTA_LENGTH( 4 );

    netlink_datagram_t past = default_route_datagram();
    netlink_done( &past );

    const unsigned short too_long = 200;
    memcpy( past.bytes.data() + gateway_attribute, &too_long, sizeof( too_long ) );
    check( parse_route( past, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    netlink_datagram_t tiny = default_route_datagram();
    const unsigned short too_short = 2;
    memcpy( tiny.bytes.data() + gateway_attribute, &too_short, sizeof( too_short ) );
    check( parse_route( tiny, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    // the same in a neighbour reply: the NDA_LLADDR length past the end

    netlink_datagram_t neighbor_past;
    netlink_neighbor_t neighbor;
    neighbor.destination = gateway_ipv4;
    neighbor.lladdr = gateway_mac;
    netlink_neighbor( &neighbor_past, neighbor );

    const size_t lladdr_attribute = NLMSG_HDRLEN + NLMSG_ALIGN( sizeof( struct ndmsg ) ) + RTA_LENGTH( 4 );
    memcpy( neighbor_past.bytes.data() + lladdr_attribute, &too_long, sizeof( too_long ) );

    uint8_t mac[6];
    check( parse_neighbor( neighbor_past, AF_INET, gateway_ipv4, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
}

// NLMSG_ERROR refuses with the kernel's errno; an acknowledgement (error 0)
// carries no answer and the walk goes on

static void test_xdp_transport_gateway_netlink_error_message()
{
    uint8_t gateway[16];
    int error = 0;

    netlink_datagram_t refused;
    netlink_error( &refused, -EPERM );
    check( parse_route( refused, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
    check( error == EPERM );

    uint8_t mac[6];
    error = 0;
    check( parse_neighbor( refused, AF_INET, gateway_ipv4, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
    check( error == EPERM );

    netlink_datagram_t acknowledged;
    netlink_error( &acknowledged, 0 );

    netlink_route_t route;
    route.gateway = gateway_ipv4;
    netlink_route( &acknowledged, route );
    check( parse_route( acknowledged, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );

    // an NLMSG_ERROR too short for its error code refuses

    netlink_datagram_t cut;
    const size_t start = netlink_begin( &cut, NLMSG_ERROR, 0 );
    netlink_end( &cut, start );
    check( parse_route( cut, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    // a dump the kernel ended with an errno in NLMSG_DONE, and a dump the
    // kernel marked interrupted, refuse

    netlink_datagram_t done_error;
    netlink_done( &done_error, -ENOBUFS );
    check( parse_route( done_error, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
    check( error == ENOBUFS );

    netlink_datagram_t interrupted = default_route_datagram();
    struct nlmsghdr header;
    memcpy( &header, interrupted.bytes.data(), sizeof( header ) );
    header.nlmsg_flags = (uint16_t) ( header.nlmsg_flags | NLM_F_DUMP_INTR );
    memcpy( interrupted.bytes.data(), &header, sizeof( header ) );
    check( parse_route( interrupted, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
    check( error == EINTR );
}

// a dump in several datagrams: each ahead of the answer reads MORE, and the
// one carrying NLMSG_DONE ends it

static void test_xdp_transport_gateway_netlink_multipart_with_done()
{
    uint8_t gateway[16];
    int error = -1;

    netlink_datagram_t first;
    route_table_before_default( &first, AF_INET );
    check( parse_route( first, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MORE );

    netlink_datagram_t second;
    netlink_route_t route;
    route.gateway = gateway_ipv4;
    netlink_route( &second, route );
    netlink_done( &second );
    check( parse_route( second, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );

    netlink_datagram_t end;
    netlink_done( &end );
    check( parse_route( end, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );

    // NLMSG_DONE ends the dump: a route after it in the datagram is not read

    netlink_datagram_t after;
    netlink_done( &after );
    netlink_route( &after, route );
    check( parse_route( after, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
}

// messages for another request (another sequence number or port id) are not
// this dump's: neither their answer nor their NLMSG_DONE counts

static void test_xdp_transport_gateway_netlink_skips_other_requests()
{
    uint8_t gateway[16];
    int error = -1;

    netlink_datagram_t datagram;
    netlink_route_t stale;
    stale.gateway = gateway_ipv4_other;
    stale.sequence = netlink_sequence + 1;
    netlink_route( &datagram, stale );

    netlink_route_t foreign;
    foreign.gateway = gateway_ipv4_other;
    foreign.port_id = netlink_port_id + 1;
    netlink_route( &datagram, foreign );
    netlink_done( &datagram, 0, netlink_sequence + 1 );
    netlink_done( &datagram, 0, netlink_sequence, netlink_port_id + 1 );
    check( parse_route( datagram, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MORE );

    netlink_route_t route;
    route.gateway = gateway_ipv4;
    netlink_route( &datagram, route );
    check( parse_route( datagram, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );
}

REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv4_default_route_found );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv4_default_route_not_on_this_interface );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv4_no_default_route );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv6_default_route_found );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv6_default_route_not_on_this_interface );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv6_no_default_route );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_only_the_main_unicast_default_route_wins );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv4_multipath_first_next_hop );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv6_multipath_found );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv4_multipath_later_next_hop );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_gateway_wrong_size_refuses );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_neighbor_lladdr_found );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_neighbor_wrong_interface );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_neighbor_no_lladdr );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_neighbor_failed_state );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_neighbor_other_family );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_truncated_message );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_attribute_past_the_end );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_error_message );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_multipart_with_done );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_skips_other_requests );

#endif // #if ROCKETNET_XDP
