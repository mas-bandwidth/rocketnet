/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// XDP's gateway discovery over canned rtnetlink replies. The server asks the
// kernel for an RTM_GETROUTE dump and then the gateway's RTM_GETNEIGH entry
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
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <utility>
#include <vector>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/neighbour.h>
#include <linux/nexthop.h>

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
// RTA_GATEWAY of the family's address size, or of gateway_bytes when it is
// not zero

static void netlink_multipath_route( netlink_datagram_t * datagram, int family, int first_interface, const uint8_t * first_gateway, int second_interface, const uint8_t * second_gateway,
                                     size_t gateway_bytes = 0 )
{
    const size_t address_bytes = gateway_bytes != 0 ? gateway_bytes : ( family == AF_INET ? 4 : 16 );
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

        // a multipath hop on this interface with the wrong size refuses the
        // same way, never read as no route

        netlink_datagram_t multipath;
        netlink_multipath_route( &multipath, family, server_interface, gateway_ipv6, other_interface, gateway_ipv6, family == AF_INET ? 16 : 4 );
        netlink_done( &multipath );
        check( parse_route( multipath, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
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

// an error code of INT_MIN, the one negative int with no positive
// counterpart, is no errno: NLMSG_ERROR and NLMSG_DONE carrying it refuse

static void test_xdp_transport_gateway_netlink_error_code_int_min()
{
    uint8_t gateway[16];
    int error = 0;

    netlink_datagram_t refused;
    netlink_error( &refused, INT_MIN );
    check( parse_route( refused, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    uint8_t mac[6];
    check( parse_neighbor( refused, AF_INET, gateway_ipv4, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    netlink_datagram_t done;
    netlink_done( &done, INT_MIN );
    check( parse_route( done, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
    check( parse_neighbor( done, AF_INET, gateway_ipv4, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
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

// ---------------------------------------------------------------------------
// whole dumps over a fake kernel: the socket calls go through a table, the
// replies are a script of datagrams read in order across dumps, and nothing
// opens a socket

struct fake_netlink_t
{
    std::vector<netlink_datagram_t> script;
    std::vector<netlink_datagram_t> requests;       // every request sent, in order
    size_t next = 0;
    int sockets = 0;
    int closes = 0;
    socklen_t local_bytes = sizeof( struct sockaddr_nl );
    bool nonblocking = false;                       // the last socket opened with SOCK_NONBLOCK
    int blocking_receives = 0;                      // reads that would wait for the kernel: a blocking socket, no MSG_DONTWAIT

    // kernel: each request is answered from the tables below, as the kernel
    // answers it, instead of from the script: the IPv4 default route through
    // route_gateway, and a stale (usable) neighbour entry for each of
    // neighbors, any other entry ENOENT
    bool kernel = false;
    const uint8_t * route_gateway = NULL;
    std::vector<std::pair<const uint8_t *, const uint8_t *>> neighbors;    // IPv4 address, MAC
    size_t answered = 0;
};

static fake_netlink_t * fake_netlink = NULL;

static netlink_datagram_t fake_kernel_answer( const netlink_datagram_t & request )
{
    netlink_datagram_t answer;
    struct nlmsghdr header;
    memcpy( &header, request.bytes.data(), sizeof( header ) );
    if ( header.nlmsg_type == RTM_GETROUTE )
    {
        netlink_route_t route;
        route.gateway = fake_netlink->route_gateway;
        netlink_route( &answer, route );
        netlink_done( &answer );
        return answer;
    }

    const uint8_t * asked = request.bytes.data() + NLMSG_SPACE( sizeof( struct ndmsg ) ) + RTA_LENGTH( 0 );
    for ( size_t i = 0; i < fake_netlink->neighbors.size(); i++ )
    {
        if ( memcmp( asked, fake_netlink->neighbors[i].first, 4 ) == 0 )
        {
            netlink_neighbor_t neighbor;
            neighbor.state = NUD_STALE;
            neighbor.destination = fake_netlink->neighbors[i].first;
            neighbor.lladdr = fake_netlink->neighbors[i].second;
            netlink_neighbor( &answer, neighbor );
            return answer;
        }
    }

    netlink_error( &answer, -ENOENT );
    return answer;
}

static int fake_netlink_socket( int, int type, int )
{
    fake_netlink->nonblocking = ( type & SOCK_NONBLOCK ) != 0;
    fake_netlink->sockets++;
    return 100 + fake_netlink->sockets;
}

static int fake_netlink_setsockopt( int, int, int, const void *, socklen_t )
{
    return 0;
}

static int fake_netlink_bind( int, const struct sockaddr *, socklen_t )
{
    return 0;
}

static int fake_netlink_getsockname( int, struct sockaddr * address, socklen_t * address_bytes )
{
    struct sockaddr_nl local;
    memset( &local, 0, sizeof( local ) );
    local.nl_family = AF_NETLINK;
    local.nl_pid = netlink_port_id;
    memcpy( address, &local, sizeof( local ) );
    *address_bytes = fake_netlink->local_bytes;
    return 0;
}

static ssize_t fake_netlink_sendto( int, const void * data, size_t bytes, int, const struct sockaddr *, socklen_t )
{
    netlink_datagram_t request;
    netlink_append( &request, data, bytes );
    fake_netlink->requests.push_back( request );
    return (ssize_t) bytes;
}

static ssize_t fake_netlink_recvmsg( int, struct msghdr * message, int flags )
{
    if ( !fake_netlink->nonblocking && ( flags & MSG_DONTWAIT ) == 0 )
    {
        fake_netlink->blocking_receives++;
    }

    netlink_datagram_t datagram;
    if ( fake_netlink->kernel )
    {
        if ( fake_netlink->answered < fake_netlink->requests.size() )
        {
            datagram = fake_kernel_answer( fake_netlink->requests[fake_netlink->answered++] );
        }
    }
    else if ( fake_netlink->next < fake_netlink->script.size() )
    {
        datagram = fake_netlink->script[fake_netlink->next++];
    }

    // an empty datagram: nothing queued for this read

    if ( datagram.bytes.empty() )
    {
        errno = EAGAIN;
        return -1;
    }

    memcpy( message->msg_iov[0].iov_base, datagram.bytes.data(), datagram.bytes.size() );
    struct sockaddr_nl kernel;
    memset( &kernel, 0, sizeof( kernel ) );
    kernel.nl_family = AF_NETLINK;
    memcpy( message->msg_name, &kernel, sizeof( kernel ) );
    message->msg_namelen = sizeof( kernel );
    message->msg_flags = 0;
    return (ssize_t) datagram.bytes.size();
}

static int fake_netlink_close( int )
{
    fake_netlink->closes++;
    return 0;
}

// the table field by field, every other field zero: the fake's sockets take
// only the flags the code under test adds

static rocketnet_gateway_netlink_system_t fake_netlink_system_table()
{
    rocketnet_gateway_netlink_system_t system;
    memset( &system, 0, sizeof( system ) );
    system.socket = fake_netlink_socket;
    system.setsockopt = fake_netlink_setsockopt;
    system.bind = fake_netlink_bind;
    system.getsockname = fake_netlink_getsockname;
    system.sendto = fake_netlink_sendto;
    system.recvmsg = fake_netlink_recvmsg;
    system.close = fake_netlink_close;
    return system;
}

static const rocketnet_gateway_netlink_system_t fake_netlink_system = fake_netlink_system_table();

static netlink_datagram_t interrupted_route_datagram()
{
    netlink_datagram_t datagram = default_route_datagram();
    struct nlmsghdr header;
    memcpy( &header, datagram.bytes.data(), sizeof( header ) );
    header.nlmsg_flags = (uint16_t) ( header.nlmsg_flags | NLM_F_DUMP_INTR );
    memcpy( datagram.bytes.data(), &header, sizeof( header ) );
    return datagram;
}

// a dump the kernel marks interrupted is asked again on a fresh socket: two
// interrupted dumps, then a clean one, find the gateway

static void test_xdp_transport_gateway_netlink_interrupted_dump_retried()
{
    fake_netlink_t fake;
    fake_netlink = &fake;
    fake.script.push_back( interrupted_route_datagram() );
    fake.script.push_back( interrupted_route_datagram() );
    fake.script.push_back( default_route_datagram() );

    uint8_t gateway[16];
    memset( gateway, 0, sizeof( gateway ) );
    int error = -1;
    check( rocketnet_gateway_netlink_route_with( &fake_netlink_system, AF_INET, server_interface, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );
    check( fake.sockets == 3 && fake.closes == 3 && fake.next == 3 );

    fake_netlink = NULL;
}

// the retries are bounded: a table that changes under every dump refuses
// with EINTR after ROCKETNET_GATEWAY_NETLINK_DUMP_ATTEMPTS dumps

static void test_xdp_transport_gateway_netlink_interrupted_dump_bounded()
{
    fake_netlink_t fake;
    fake_netlink = &fake;
    for ( int i = 0; i < ROCKETNET_GATEWAY_NETLINK_DUMP_ATTEMPTS; i++ )
    {
        fake.script.push_back( interrupted_route_datagram() );
    }
    fake.script.push_back( default_route_datagram() );

    uint8_t gateway[16];
    int error = -1;
    check( rocketnet_gateway_netlink_route_with( &fake_netlink_system, AF_INET, server_interface, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
    check( error == EINTR );
    check( fake.sockets == ROCKETNET_GATEWAY_NETLINK_DUMP_ATTEMPTS && fake.closes == fake.sockets );
    check( fake.next == (size_t) ROCKETNET_GATEWAY_NETLINK_DUMP_ATTEMPTS );

    fake_netlink = NULL;
}

// any other kernel error is the answer: it is not asked again

static void test_xdp_transport_gateway_netlink_error_not_retried()
{
    fake_netlink_t fake;
    fake_netlink = &fake;
    netlink_datagram_t refused;
    netlink_error( &refused, -EPERM );
    fake.script.push_back( refused );
    fake.script.push_back( default_route_datagram() );

    uint8_t gateway[16];
    int error = -1;
    check( rocketnet_gateway_netlink_route_with( &fake_netlink_system, AF_INET, server_interface, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
    check( error == EPERM );
    check( fake.sockets == 1 && fake.closes == 1 );

    fake_netlink = NULL;
}

// a local address of the wrong size after every call succeeded is SYSTEM with
// EPROTO, never whatever errno an earlier call left behind

static void test_xdp_transport_gateway_netlink_short_local_address()
{
    fake_netlink_t fake;
    fake_netlink = &fake;
    fake.local_bytes = sizeof( struct sockaddr_nl ) - 2;
    fake.script.push_back( default_route_datagram() );

    uint8_t gateway[16];
    int error = -1;
    errno = 0;
    check( rocketnet_gateway_netlink_route_with( &fake_netlink_system, AF_INET, server_interface, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_SYSTEM );
    check( error == EPROTO );
    check( fake.sockets == 1 && fake.closes == 1 && fake.next == 0 );

    uint8_t mac[6];
    error = -1;
    errno = 0;
    check( rocketnet_gateway_netlink_neighbor_with( &fake_netlink_system, AF_INET, server_interface, gateway_ipv4, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_SYSTEM );
    check( error == EPROTO );

    fake_netlink = NULL;
}

// ---------------------------------------------------------------------------
// the neighbour entry asked directly, and the periodic refresh

// a request is a dump when it carries NLM_F_DUMP

static bool request_is_dump( const netlink_datagram_t & request, uint16_t * type )
{
    struct nlmsghdr header;
    memcpy( &header, request.bytes.data(), sizeof( header ) );
    *type = header.nlmsg_type;
    return ( header.nlmsg_flags & NLM_F_DUMP ) == NLM_F_DUMP;
}

static int fake_dumps( const fake_netlink_t & fake )
{
    int dumps = 0;
    for ( size_t i = 0; i < fake.requests.size(); i++ )
    {
        uint16_t type = 0;
        if ( request_is_dump( fake.requests[i], &type ) )
        {
            dumps++;
        }
    }
    return dumps;
}

static netlink_datagram_t neighbor_reply( int family, uint16_t state = NUD_REACHABLE )
{
    netlink_datagram_t datagram;
    netlink_neighbor_t neighbor;
    neighbor.family = family;
    neighbor.state = state;
    neighbor.destination = family == AF_INET ? gateway_ipv4 : gateway_ipv6;
    neighbor.lladdr = state == NUD_INCOMPLETE ? NULL : gateway_mac;
    netlink_neighbor( &datagram, neighbor );
    return datagram;
}

// the gateway's neighbour entry is one RTM_GETNEIGH for that entry (the
// interface and NDA_DST named, no NLM_F_DUMP), answered by the one reply:
// the kernel's whole neighbour table is never walked for it

static void check_neighbor_asked_directly( int family )
{
    const uint8_t * gateway = family == AF_INET ? gateway_ipv4 : gateway_ipv6;
    const size_t address_bytes = netlink_address_bytes( family );

    fake_netlink_t fake;
    fake_netlink = &fake;
    fake.script.push_back( neighbor_reply( family ) );

    uint8_t mac[6];
    memset( mac, 0, sizeof( mac ) );
    int error = -1;
    check( rocketnet_gateway_netlink_neighbor_with( &fake_netlink_system, family, server_interface, gateway, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( mac, gateway_mac, 6 ) == 0 );
    check( fake.requests.size() == 1 && fake.sockets == 1 && fake.closes == 1 );

    const netlink_datagram_t & request = fake.requests[0];
    check( request.bytes.size() >= NLMSG_SPACE( sizeof( struct ndmsg ) ) + RTA_SPACE( address_bytes ) );
    uint16_t type = 0;
    check( !request_is_dump( request, &type ) );
    check( type == RTM_GETNEIGH );

    struct nlmsghdr header;
    memcpy( &header, request.bytes.data(), sizeof( header ) );
    check( header.nlmsg_flags == NLM_F_REQUEST );
    check( header.nlmsg_len == request.bytes.size() );

    struct ndmsg message;
    memcpy( &message, request.bytes.data() + NLMSG_HDRLEN, sizeof( message ) );
    check( message.ndm_family == family );
    check( message.ndm_ifindex == server_interface );
    check( message.ndm_state == 0 && message.ndm_flags == 0 && message.ndm_type == 0 );

    struct rtattr attribute;
    memcpy( &attribute, request.bytes.data() + NLMSG_SPACE( sizeof( struct ndmsg ) ), sizeof( attribute ) );
    check( attribute.rta_type == NDA_DST );
    check( attribute.rta_len == RTA_LENGTH( address_bytes ) );
    check( memcmp( request.bytes.data() + NLMSG_SPACE( sizeof( struct ndmsg ) ) + RTA_LENGTH( 0 ), gateway, address_bytes ) == 0 );

    fake_netlink = NULL;
}

static void test_xdp_transport_gateway_netlink_neighbor_asked_directly()
{
    check_neighbor_asked_directly( AF_INET );
    check_neighbor_asked_directly( AF_INET6 );
}

// no entry for the gateway (the kernel answers ENOENT), or an entry with no
// usable link-layer address, is DONE at once: nothing waits for a
// datagram that will not come

static void test_xdp_transport_gateway_netlink_neighbor_missing_entry()
{
    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        netlink_datagram_t missing;
        netlink_error( &missing, -ENOENT );
        fake.script.push_back( missing );

        uint8_t mac[6];
        int error = -1;
        check( rocketnet_gateway_netlink_neighbor_with( &fake_netlink_system, AF_INET, server_interface, gateway_ipv4, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
        check( error == 0 );
        check( fake.next == 1 && fake.sockets == 1 && fake.closes == 1 );
    }

    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        fake.script.push_back( neighbor_reply( AF_INET, NUD_INCOMPLETE ) );

        uint8_t mac[6];
        int error = -1;
        check( rocketnet_gateway_netlink_neighbor_with( &fake_netlink_system, AF_INET, server_interface, gateway_ipv4, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
        check( error == 0 );
        check( fake.next == 1 && fake.closes == 1 );
    }

    fake_netlink = NULL;
}

// the periodic refresh, every period: the route dump, then the route's
// gateway's neighbour entry asked directly, whether or not a gateway is
// known, as create discovers it

static void test_xdp_transport_gateway_netlink_refresh_reads_the_route_every_period()
{
    uint8_t gateway[16];
    memset( gateway, 0, sizeof( gateway ) );
    bool gateway_known = false;
    uint8_t mac[6];
    int error = -1;

    // the first refresh knows no gateway: the route dump, then the neighbour

    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        fake.script.push_back( default_route_datagram() );
        fake.script.push_back( neighbor_reply( AF_INET ) );
        memset( mac, 0, sizeof( mac ) );
        check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
        check( gateway_known );
        check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );
        check( memcmp( mac, gateway_mac, 6 ) == 0 );
        check( fake.requests.size() == 2 && fake_dumps( fake ) == 1 );
    }

    // every refresh after it, the gateway known: the route dump again, then
    // the neighbour

    for ( int i = 0; i < 3; i++ )
    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        fake.script.push_back( default_route_datagram() );
        fake.script.push_back( neighbor_reply( AF_INET ) );
        memset( mac, 0, sizeof( mac ) );
        check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
        check( gateway_known );
        check( memcmp( mac, gateway_mac, 6 ) == 0 );
        check( fake.requests.size() == 2 && fake_dumps( fake ) == 1 );
        uint16_t type = 0;
        check( request_is_dump( fake.requests[0], &type ) && type == RTM_GETROUTE );
        check( !request_is_dump( fake.requests[1], &type ) && type == RTM_GETNEIGH );
    }

    // the route's gateway has no usable entry: DONE with the gateway known,
    // which the server counts a failed refresh

    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        netlink_datagram_t missing;
        netlink_error( &missing, -ENOENT );
        fake.script.push_back( default_route_datagram() );
        fake.script.push_back( missing );
        check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
        check( gateway_known );
        check( fake.requests.size() == 2 && fake_dumps( fake ) == 1 );
    }

    // no default route any more: DONE, and no gateway is known

    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        netlink_datagram_t empty;
        netlink_done( &empty );
        fake.script.push_back( empty );
        check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
        check( !gateway_known );
        check( fake.requests.size() == 1 );
    }

    // a failed route dump is its outcome: the gateway bytes are not written,
    // and no MAC is

    {
        memcpy( gateway, gateway_ipv4, 4 );
        memcpy( mac, other_mac, 6 );
        fake_netlink_t fake;
        fake_netlink = &fake;
        netlink_datagram_t refused;
        netlink_error( &refused, -EPERM );
        fake.script.push_back( refused );
        check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
        check( error == EPERM );
        check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );
        check( memcmp( mac, other_mac, 6 ) == 0 );
        check( fake.requests.size() == 1 );
    }

    fake_netlink = NULL;
}

// the refresh runs inside the update, so it never makes a blocking netlink
// receive: every socket it opens is non-blocking, and the answer the kernel
// has already queued is read as before
static void test_xdp_transport_gateway_netlink_refresh_never_blocks()
{
    fake_netlink_t fake;
    fake_netlink = &fake;
    fake.script.push_back( default_route_datagram() );
    fake.script.push_back( neighbor_reply( AF_INET ) );

    uint8_t gateway[16];
    memset( gateway, 0, sizeof( gateway ) );
    memcpy( gateway, gateway_ipv4, 4 );
    bool gateway_known = true;
    uint8_t mac[6];
    memset( mac, 0, sizeof( mac ) );
    int error = -1;
    check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( memcmp( mac, gateway_mac, 6 ) == 0 );
    check( fake.nonblocking );
    check( fake.blocking_receives == 0 );
    check( fake.sockets == 2 && fake.closes == 2 );

    fake_netlink = NULL;
}

// nothing queued when the refresh reads (EAGAIN) is a failed query, where a
// blocking read would have waited for its timeout: the route dump failing
// EAGAIN mid-reply, or the neighbour query finding nothing queued, fails the
// refresh SYSTEM with EAGAIN and writes no MAC, so the server keeps the
// gateway MAC in use; nothing waited for
static void test_xdp_transport_gateway_netlink_refresh_eagain_fails_and_keeps_the_gateway()
{
    uint8_t gateway[16];
    memset( gateway, 0, sizeof( gateway ) );
    memcpy( gateway, gateway_ipv4_other, 4 );
    bool gateway_known = true;
    uint8_t mac[6];
    memcpy( mac, other_mac, 6 );
    int error = -1;

    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        netlink_datagram_t other_route;
        netlink_route_t route;
        route.destination_length = 24;
        route.destination = subnet_ipv4;
        netlink_route( &other_route, route );
        fake.script.push_back( other_route );
        fake.script.push_back( netlink_datagram_t() );
        fake.script.push_back( default_route_datagram() );

        check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_SYSTEM );
        check( error == EAGAIN );
        check( memcmp( gateway, gateway_ipv4_other, 4 ) == 0 );
        check( memcmp( mac, other_mac, 6 ) == 0 );
        check( fake.requests.size() == 1 && fake_dumps( fake ) == 1 );
        check( fake.next == 2 );
        check( fake.sockets == 1 && fake.closes == 1 );
        check( fake.blocking_receives == 0 );
    }

    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        fake.script.push_back( default_route_datagram() );
        fake.script.push_back( netlink_datagram_t() );

        error = -1;
        check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_SYSTEM );
        check( error == EAGAIN );
        check( memcmp( mac, other_mac, 6 ) == 0 );
        check( fake.requests.size() == 2 && fake_dumps( fake ) == 1 );
        check( fake.sockets == 2 && fake.closes == 2 );
        check( fake.blocking_receives == 0 );
    }

    fake_netlink = NULL;
}

// the default route moves to a new gateway IP while the old gateway's
// neighbour entry is still in the table and usable (stale): the next refresh
// takes the new gateway and its MAC, as v1.0.0's refresh did by reading the
// route every period. The fake answers each request as the kernel would.
static void test_xdp_transport_gateway_netlink_refresh_follows_a_gateway_ip_change()
{
    fake_netlink_t fake;
    fake_netlink = &fake;
    fake.kernel = true;
    fake.route_gateway = gateway_ipv4;
    fake.neighbors.push_back( std::make_pair( gateway_ipv4, gateway_mac ) );
    fake.neighbors.push_back( std::make_pair( gateway_ipv4_other, other_mac ) );

    uint8_t gateway[16];
    memset( gateway, 0, sizeof( gateway ) );
    bool gateway_known = false;
    uint8_t mac[6];
    memset( mac, 0, sizeof( mac ) );
    int error = -1;
    check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( gateway_known && memcmp( gateway, gateway_ipv4, 4 ) == 0 );
    check( memcmp( mac, gateway_mac, 6 ) == 0 );

    // the router is replaced: the route names the other gateway, and the old
    // gateway's entry stays usable

    fake.route_gateway = gateway_ipv4_other;
    check( rocketnet_gateway_netlink_refresh_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( gateway_known && memcmp( gateway, gateway_ipv4_other, 4 ) == 0 );
    check( memcmp( mac, other_mac, 6 ) == 0 );
    check( fake.blocking_receives == 0 );

    fake_netlink = NULL;
}

// create-time discovery records the route's gateway IP and marks it known;
// no default route records none
static void test_xdp_transport_gateway_netlink_discover_records_the_gateway()
{
    uint8_t gateway[16];
    memset( gateway, 0, sizeof( gateway ) );
    bool gateway_known = false;
    uint8_t mac[6];
    int error = -1;

    // create: the route dump finds the gateway, then its neighbour answers

    {
        fake_netlink_t fake;
        fake_netlink = &fake;
        fake.script.push_back( default_route_datagram() );
        fake.script.push_back( neighbor_reply( AF_INET ) );
        memset( mac, 0, sizeof( mac ) );
        check( rocketnet_gateway_netlink_discover_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
        check( gateway_known );
        check( memcmp( gateway, gateway_ipv4, 4 ) == 0 );
        check( memcmp( mac, gateway_mac, 6 ) == 0 );
        check( fake.requests.size() == 2 && fake_dumps( fake ) == 1 );
    }

    // no default route at create: no gateway IP is recorded

    {
        gateway_known = true;
        netlink_datagram_t empty;
        netlink_done( &empty );
        fake_netlink_t fake;
        fake_netlink = &fake;
        fake.script.push_back( empty );
        check( rocketnet_gateway_netlink_discover_with( &fake_netlink_system, AF_INET, server_interface, gateway, &gateway_known, mac, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
        check( !gateway_known );
    }

    fake_netlink = NULL;
}

// ---------------------------------------------------------------------------
// the link: the RTM_NEWLINK reply to an RTM_GETLINK, as the kernel writes it
// (rtnl_fill_ifinfo: IFLA_IFNAME, IFLA_LINK, IFLA_LINK_NETNSID, and the
// IFLA_LINKINFO nest of IFLA_INFO_KIND and, for a VLAN device, the
// IFLA_INFO_DATA nest of IFLA_VLAN_ID in host order and IFLA_VLAN_PROTOCOL
// big endian), and what the XDP transport does with each device

struct netlink_link_t
{
    int index = server_interface;
    const char * name = "eth0.100";
    size_t name_bytes = 0;              // 0: strlen + 1 (the kernel's NUL)
    int link = -1;                      // -1: no IFLA_LINK
    size_t link_bytes = 4;
    bool netnsid = false;
    const char * kind = NULL;           // NULL: no IFLA_LINKINFO
    int vlan_id = -1;                   // -1: no IFLA_VLAN_ID
    size_t vlan_id_bytes = 2;
    int vlan_protocol = -1;             // -1: no IFLA_VLAN_PROTOCOL
    size_t vlan_protocol_bytes = 2;
    uint32_t sequence = netlink_sequence;
};

static void netlink_link( netlink_datagram_t * datagram, const netlink_link_t & link )
{
    const size_t start = netlink_begin( datagram, RTM_NEWLINK, 0, link.sequence );
    struct ifinfomsg message;
    memset( &message, 0, sizeof( message ) );
    message.ifi_family = AF_UNSPEC;
    message.ifi_index = link.index;
    netlink_append( datagram, &message, sizeof( message ) );

    if ( link.name )
    {
        netlink_attribute( datagram, IFLA_IFNAME, link.name, link.name_bytes ? link.name_bytes : strlen( link.name ) + 1 );
    }
    if ( link.link >= 0 )
    {
        const uint32_t value = (uint32_t) link.link;
        netlink_attribute( datagram, IFLA_LINK, &value, link.link_bytes );
    }
    if ( link.netnsid )
    {
        netlink_attribute_u32( datagram, IFLA_LINK_NETNSID, 1 );
    }
    if ( link.kind )
    {
        netlink_datagram_t info;
        netlink_attribute( &info, IFLA_INFO_KIND, link.kind, strlen( link.kind ) + 1 );
        if ( link.vlan_id >= 0 || link.vlan_protocol >= 0 )
        {
            netlink_datagram_t data;
            if ( link.vlan_id >= 0 )
            {
                const uint16_t vlan_id = (uint16_t) link.vlan_id;
                uint8_t bytes[4] = { 0, 0, 0, 0 };
                memcpy( bytes, &vlan_id, sizeof( vlan_id ) );
                netlink_attribute( &data, IFLA_VLAN_ID, bytes, link.vlan_id_bytes );
            }
            if ( link.vlan_protocol >= 0 )
            {
                const uint8_t protocol[4] = { (uint8_t) ( link.vlan_protocol >> 8 ), (uint8_t) link.vlan_protocol, 0, 0 };
                netlink_attribute( &data, IFLA_VLAN_PROTOCOL, protocol, link.vlan_protocol_bytes );
            }
            netlink_attribute( &info, IFLA_INFO_DATA, data.bytes.data(), data.bytes.size() );
        }
        netlink_attribute( datagram, IFLA_LINKINFO, info.bytes.data(), info.bytes.size() );
    }
    netlink_end( datagram, start );
}

static netlink_link_t vlan_link()
{
    netlink_link_t link;
    link.link = 2;
    link.kind = "vlan";
    link.vlan_id = 100;
    link.vlan_protocol = 0x8100;
    return link;
}

static int parse_link( const netlink_link_t & link, rocketnet_gateway_netlink_link_t * out )
{
    netlink_datagram_t datagram;
    netlink_link( &datagram, link );
    int error = 0;
    return rocketnet_gateway_netlink_parse_link( datagram.bytes.data(), datagram.bytes.size(), netlink_sequence, netlink_port_id, server_interface, out, &error );
}

static void test_xdp_transport_link_parse_reads_the_device()
{
    rocketnet_gateway_netlink_link_t link;

    // a VLAN device

    check( parse_link( vlan_link(), &link ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( link.index == server_interface );
    check( strcmp( link.name, "eth0.100" ) == 0 );
    check( strcmp( link.kind, "vlan" ) == 0 );
    check( link.link_index == 2 && !link.link_netns );
    check( link.vlan_id == 100 && link.vlan_protocol == 0x8100 );

    // an 802.1ad one in another namespace

    netlink_link_t ad = vlan_link();
    ad.vlan_protocol = 0x88a8;
    ad.netnsid = true;
    check( parse_link( ad, &link ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( link.vlan_protocol == 0x88a8 && link.link_netns );

    // a physical NIC: no link info, and IFLA_LINK naming itself is no lower device

    netlink_link_t nic;
    nic.name = "eth0";
    nic.link = server_interface;
    check( parse_link( nic, &link ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( strcmp( link.name, "eth0" ) == 0 && link.kind[0] == '\0' );
    check( link.link_index == 0 && link.vlan_id == 0 && link.vlan_protocol == 0 );

    // a bond: its kind, and no VLAN fields read from it

    netlink_link_t bond;
    bond.name = "bond0";
    bond.kind = "bond";
    check( parse_link( bond, &link ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( strcmp( link.kind, "bond" ) == 0 && link.vlan_id == 0 );

    // another device's reply is not this one's

    netlink_link_t other = vlan_link();
    other.index = other_interface;
    check( parse_link( other, &link ) == ROCKETNET_GATEWAY_NETLINK_MORE );
    netlink_link_t stale = vlan_link();
    stale.sequence = netlink_sequence + 1;
    check( parse_link( stale, &link ) == ROCKETNET_GATEWAY_NETLINK_MORE );
}

// every attribute past its bounds or of the wrong size is MALFORMED
static void test_xdp_transport_link_parse_refuses_malformed_attributes()
{
    rocketnet_gateway_netlink_link_t link;

    netlink_link_t no_name = vlan_link();
    no_name.name = NULL;
    check( parse_link( no_name, &link ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    netlink_link_t unterminated = vlan_link();
    unterminated.name_bytes = strlen( unterminated.name );
    check( parse_link( unterminated, &link ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    netlink_link_t long_name = vlan_link();
    long_name.name = "abcdefghijklmnop";        // 16 characters: no room for the NUL in IFNAMSIZ
    check( parse_link( long_name, &link ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
    long_name.name = "abcdefghijklmno";         // 15: the longest name
    check( parse_link( long_name, &link ) == ROCKETNET_GATEWAY_NETLINK_FOUND );

    netlink_link_t long_kind = vlan_link();
    long_kind.kind = "abcdefghijklmnop";
    check( parse_link( long_kind, &link ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    netlink_link_t short_link = vlan_link();
    short_link.link_bytes = 2;
    check( parse_link( short_link, &link ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    netlink_link_t wide_id = vlan_link();
    wide_id.vlan_id_bytes = 4;
    check( parse_link( wide_id, &link ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );

    netlink_link_t short_protocol = vlan_link();
    short_protocol.vlan_protocol_bytes = 1;
    check( parse_link( short_protocol, &link ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
}

static rocketnet_gateway_netlink_link_t classify_link( const char * kind, int link_index = 0, int vlan_id = 0, int vlan_protocol = 0, bool link_netns = false )
{
    rocketnet_gateway_netlink_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.index = server_interface;
    snprintf( link.kind, sizeof( link.kind ), "%s", kind );
    link.link_index = link_index;
    link.vlan_id = vlan_id;
    link.vlan_protocol = vlan_protocol;
    link.link_netns = link_netns;
    return link;
}

static int classify( const rocketnet_gateway_netlink_link_t & device, const rocketnet_gateway_netlink_link_t * lower, const char * reason_has = NULL )
{
    const char * reason = "unset";
    const int result = rocketnet_xdp_link_classify( &device, lower, &reason );
    if ( result == ROCKETNET_XDP_LINK_REFUSED )
    {
        check( reason != NULL && reason_has != NULL && strstr( reason, reason_has ) != NULL );
    }
    else
    {
        check( reason == NULL );
    }
    return result;
}

// what the transport attaches to: the device itself (PLAIN), the NIC under
// one 802.1Q VLAN device (VLAN), or nothing, each refusal named
static void test_xdp_transport_link_classify_names_every_refusal()
{
    const rocketnet_gateway_netlink_link_t nic = classify_link( "" );
    const rocketnet_gateway_netlink_link_t veth = classify_link( "veth" );
    const rocketnet_gateway_netlink_link_t vlan = classify_link( "vlan", 2, 100, 0x8100 );

    // plain: a NIC, veth, and any kind the transport does not know

    check( classify( nic, NULL ) == ROCKETNET_XDP_LINK_PLAIN );
    check( classify( veth, NULL ) == ROCKETNET_XDP_LINK_PLAIN );
    check( classify( classify_link( "dummy" ), NULL ) == ROCKETNET_XDP_LINK_PLAIN );

    // one 802.1Q VLAN on a NIC or veth, VLAN IDs 1 and 4094

    check( classify( vlan, &nic ) == ROCKETNET_XDP_LINK_VLAN );
    check( classify( vlan, &veth ) == ROCKETNET_XDP_LINK_VLAN );
    check( classify( classify_link( "vlan", 2, 1, 0x8100 ), &nic ) == ROCKETNET_XDP_LINK_VLAN );
    check( classify( classify_link( "vlan", 2, 4094, 0x8100 ), &nic ) == ROCKETNET_XDP_LINK_VLAN );

    // the address on a stacked device

    const char * stacked[] = { "bond", "bridge", "team", "macvlan", "macvtap", "ipvlan", "ipvtap" };
    for ( size_t i = 0; i < sizeof( stacked ) / sizeof( stacked[0] ); i++ )
    {
        check( classify( classify_link( stacked[i] ), NULL, "stacked on other devices" ) == ROCKETNET_XDP_LINK_REFUSED );
    }

    // 802.1ad, and a VLAN with no protocol read

    check( classify( classify_link( "vlan", 2, 100, 0x88a8 ), &nic, "not 802.1Q" ) == ROCKETNET_XDP_LINK_REFUSED );
    check( classify( classify_link( "vlan", 2, 100, 0 ), &nic, "not 802.1Q" ) == ROCKETNET_XDP_LINK_REFUSED );

    // VLAN IDs 0 and 4095

    check( classify( classify_link( "vlan", 2, 0, 0x8100 ), &nic, "not 1 to 4094" ) == ROCKETNET_XDP_LINK_REFUSED );
    check( classify( classify_link( "vlan", 2, 4095, 0x8100 ), &nic, "not 1 to 4094" ) == ROCKETNET_XDP_LINK_REFUSED );

    // the parent: in another namespace, unnamed, or not read

    check( classify( classify_link( "vlan", 2, 100, 0x8100, true ), &nic, "not a device in this network namespace" ) == ROCKETNET_XDP_LINK_REFUSED );
    check( classify( classify_link( "vlan", 0, 100, 0x8100 ), &nic, "not a device in this network namespace" ) == ROCKETNET_XDP_LINK_REFUSED );
    check( classify( vlan, NULL, "not a device in this network namespace" ) == ROCKETNET_XDP_LINK_REFUSED );

    // QinQ: a VLAN on a VLAN, either protocol

    const rocketnet_gateway_netlink_link_t outer_ad = classify_link( "vlan", 1, 200, 0x88a8 );
    check( classify( vlan, &vlan, "QinQ" ) == ROCKETNET_XDP_LINK_REFUSED );
    check( classify( vlan, &outer_ad, "QinQ" ) == ROCKETNET_XDP_LINK_REFUSED );

    // a VLAN on a bond, a bridge, a macvlan or any other kind

    const char * lowers[] = { "bond", "bridge", "team", "macvlan", "ipvlan", "vxlan" };
    for ( size_t i = 0; i < sizeof( lowers ) / sizeof( lowers[0] ); i++ )
    {
        const rocketnet_gateway_netlink_link_t lower = classify_link( lowers[i] );
        check( classify( vlan, &lower, "stacked or virtual device" ) == ROCKETNET_XDP_LINK_REFUSED );
    }
}

// the query: one RTM_GETLINK for the index, not a dump, answered by the one
// reply; the kernel's ENODEV is an ERROR
static void test_xdp_transport_link_query_asks_the_index()
{
    fake_netlink_t fake;
    fake_netlink = &fake;

    netlink_datagram_t reply;
    netlink_link( &reply, vlan_link() );
    fake.script.push_back( reply );

    rocketnet_gateway_netlink_link_t link;
    int error = -1;
    check( rocketnet_gateway_netlink_link_with( &fake_netlink_system, server_interface, &link, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( link.vlan_id == 100 && link.link_index == 2 );
    check( fake.requests.size() == 1 && fake.sockets == 1 && fake.closes == 1 );

    const netlink_datagram_t & request = fake.requests[0];
    uint16_t type = 0;
    check( !request_is_dump( request, &type ) );
    check( type == RTM_GETLINK );
    check( request.bytes.size() == NLMSG_LENGTH( sizeof( struct ifinfomsg ) ) );
    struct ifinfomsg message;
    memcpy( &message, request.bytes.data() + NLMSG_HDRLEN, sizeof( message ) );
    check( message.ifi_family == AF_UNSPEC && message.ifi_index == server_interface );

    netlink_datagram_t gone;
    netlink_error( &gone, -ENODEV );
    fake.script.push_back( gone );
    check( rocketnet_gateway_netlink_link_with( &fake_netlink_system, server_interface, &link, &error ) == ROCKETNET_GATEWAY_NETLINK_ERROR );
    check( error == ENODEV );

    fake_netlink = NULL;
}

REGISTER_UNIT_TEST( test_xdp_transport_link_parse_reads_the_device );
REGISTER_UNIT_TEST( test_xdp_transport_link_parse_refuses_malformed_attributes );
REGISTER_UNIT_TEST( test_xdp_transport_link_classify_names_every_refusal );
REGISTER_UNIT_TEST( test_xdp_transport_link_query_asks_the_index );
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
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_error_code_int_min );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_multipart_with_done );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_skips_other_requests );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_interrupted_dump_retried );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_interrupted_dump_bounded );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_error_not_retried );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_short_local_address );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_neighbor_asked_directly );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_neighbor_missing_entry );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_refresh_reads_the_route_every_period );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_refresh_never_blocks );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_refresh_eagain_fails_and_keeps_the_gateway );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_refresh_follows_a_gateway_ip_change );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_discover_records_the_gateway );

// ---------------------------------------------------------------------------
// a default route that names a nexthop object (RTA_NH_ID, the form
// "ip route add default nhid N" makes) carries no RTA_OIF or RTA_GATEWAY of
// its own: the gateway and interface are the nexthop object's, asked with
// RTM_GETNEXTHOP and NHA_ID (ip nexthop get id N). An object that cannot be
// resolved (on another interface, a group, a blackhole, no gateway, or
// none) is the no-gateway path: DONE, as with no default route.

static const uint32_t nexthop_id = 7;

static netlink_datagram_t nexthop_route_datagram( int family )
{
    netlink_datagram_t datagram;
    const size_t start = netlink_begin( &datagram, RTM_NEWROUTE, NLM_F_MULTI );
    struct rtmsg message;
    memset( &message, 0, sizeof( message ) );
    message.rtm_family = (unsigned char) family;
    message.rtm_table = RT_TABLE_MAIN;
    message.rtm_protocol = RTPROT_BOOT;
    message.rtm_scope = RT_SCOPE_UNIVERSE;
    message.rtm_type = RTN_UNICAST;
    netlink_append( &datagram, &message, sizeof( message ) );
    netlink_attribute_u32( &datagram, RTA_TABLE, RT_TABLE_MAIN );
    netlink_attribute_u32( &datagram, RTA_NH_ID, nexthop_id );
    netlink_end( &datagram, start );
    netlink_done( &datagram );
    return datagram;
}

struct netlink_nexthop_t
{
    int family = AF_INET;
    uint32_t id = nexthop_id;
    int output_interface = server_interface;       // 0: no NHA_OIF
    const uint8_t * gateway = gateway_ipv4;         // NULL: no NHA_GATEWAY
    bool group = false;                             // an NHA_GROUP of one member
    bool blackhole = false;
};

static netlink_datagram_t nexthop_reply( const netlink_nexthop_t & nexthop )
{
    netlink_datagram_t datagram;
    const size_t start = netlink_begin( &datagram, RTM_NEWNEXTHOP, 0 );
    struct nhmsg message;
    memset( &message, 0, sizeof( message ) );
    message.nh_family = (unsigned char) nexthop.family;
    message.nh_protocol = RTPROT_BOOT;
    netlink_append( &datagram, &message, sizeof( message ) );
    netlink_attribute_u32( &datagram, NHA_ID, nexthop.id );
    if ( nexthop.group )
    {
        struct nexthop_grp member;
        memset( &member, 0, sizeof( member ) );
        member.id = nexthop.id + 1;
        netlink_attribute( &datagram, NHA_GROUP, &member, sizeof( member ) );
    }
    if ( nexthop.blackhole )
    {
        netlink_attribute( &datagram, NHA_BLACKHOLE, NULL, 0 );
    }
    if ( nexthop.output_interface )
    {
        netlink_attribute_u32( &datagram, NHA_OIF, (uint32_t) nexthop.output_interface );
    }
    if ( nexthop.gateway )
    {
        netlink_attribute( &datagram, NHA_GATEWAY, nexthop.gateway, netlink_address_bytes( nexthop.family ) );
    }
    netlink_end( &datagram, start );
    return datagram;
}

static int nexthop_route_query( fake_netlink_t * fake, int family, uint8_t * gateway, int * error )
{
    fake_netlink = fake;
    memset( gateway, 0xEE, 16 );
    *error = -1;
    const int result = rocketnet_gateway_netlink_route_with( &fake_netlink_system, family, server_interface, gateway, error );
    fake_netlink = NULL;
    return result;
}

// the second request is RTM_GETNEXTHOP, not a dump, naming the route's id

static void check_nexthop_asked( const fake_netlink_t & fake )
{
    check( fake.requests.size() == 2 );
    struct nlmsghdr header;
    memcpy( &header, fake.requests[1].bytes.data(), sizeof( header ) );
    check( header.nlmsg_type == RTM_GETNEXTHOP );
    check( ( header.nlmsg_flags & NLM_F_DUMP ) == 0 );
    struct rtattr attribute;
    memcpy( &attribute, fake.requests[1].bytes.data() + NLMSG_SPACE( sizeof( struct nhmsg ) ), sizeof( attribute ) );
    check( attribute.rta_type == NHA_ID );
    check( attribute.rta_len == RTA_LENGTH( 4 ) );
    uint32_t id = 0;
    memcpy( &id, fake.requests[1].bytes.data() + NLMSG_SPACE( sizeof( struct nhmsg ) ) + RTA_LENGTH( 0 ), sizeof( id ) );
    check( id == nexthop_id );
}

static void check_nexthop_resolved( int family )
{
    const uint8_t * want = family == AF_INET ? gateway_ipv4 : gateway_ipv6;
    netlink_nexthop_t nexthop;
    nexthop.family = family;
    nexthop.gateway = want;

    fake_netlink_t fake;
    fake.script.push_back( nexthop_route_datagram( family ) );
    fake.script.push_back( nexthop_reply( nexthop ) );

    uint8_t gateway[16];
    int error = 0;
    check( nexthop_route_query( &fake, family, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_FOUND );
    check( error == 0 );
    check( memcmp( gateway, want, netlink_address_bytes( family ) ) == 0 );
    check_nexthop_asked( fake );
}

static void test_xdp_transport_gateway_netlink_ipv4_nexthop_object_resolved()
{
    check_nexthop_resolved( AF_INET );
}

static void test_xdp_transport_gateway_netlink_ipv6_nexthop_object_resolved()
{
    check_nexthop_resolved( AF_INET6 );
}

static void check_nexthop_unresolved( const netlink_datagram_t & reply )
{
    fake_netlink_t fake;
    fake.script.push_back( nexthop_route_datagram( AF_INET ) );
    fake.script.push_back( reply );

    uint8_t gateway[16];
    int error = -1;
    check( nexthop_route_query( &fake, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_DONE );
    check( error == 0 );
    check_nexthop_asked( fake );
}

static void test_xdp_transport_gateway_netlink_nexthop_object_unresolved_is_no_gateway()
{
    netlink_nexthop_t other;
    other.output_interface = other_interface;
    check_nexthop_unresolved( nexthop_reply( other ) );

    netlink_nexthop_t group;
    group.group = true;
    group.output_interface = 0;
    group.gateway = NULL;
    check_nexthop_unresolved( nexthop_reply( group ) );

    netlink_nexthop_t blackhole;
    blackhole.blackhole = true;
    blackhole.output_interface = 0;
    blackhole.gateway = NULL;
    check_nexthop_unresolved( nexthop_reply( blackhole ) );

    netlink_nexthop_t no_gateway;
    no_gateway.gateway = NULL;
    check_nexthop_unresolved( nexthop_reply( no_gateway ) );

    netlink_nexthop_t other_family;
    other_family.family = AF_INET6;
    other_family.gateway = gateway_ipv6;
    check_nexthop_unresolved( nexthop_reply( other_family ) );

    netlink_datagram_t missing;
    netlink_error( &missing, -ENOENT );
    check_nexthop_unresolved( missing );

    netlink_datagram_t unsupported;
    netlink_error( &unsupported, -EOPNOTSUPP );
    check_nexthop_unresolved( unsupported );
}

// a gateway of the wrong size in the nexthop object refuses, as in a route

static void test_xdp_transport_gateway_netlink_nexthop_gateway_wrong_size_refuses()
{
    netlink_datagram_t reply;
    const size_t start = netlink_begin( &reply, RTM_NEWNEXTHOP, 0 );
    struct nhmsg message;
    memset( &message, 0, sizeof( message ) );
    message.nh_family = AF_INET;
    netlink_append( &reply, &message, sizeof( message ) );
    netlink_attribute_u32( &reply, NHA_ID, nexthop_id );
    netlink_attribute_u32( &reply, NHA_OIF, (uint32_t) server_interface );
    netlink_attribute( &reply, NHA_GATEWAY, gateway_ipv6, 16 );
    netlink_end( &reply, start );

    fake_netlink_t fake;
    fake.script.push_back( nexthop_route_datagram( AF_INET ) );
    fake.script.push_back( reply );

    uint8_t gateway[16];
    int error = -1;
    check( nexthop_route_query( &fake, AF_INET, gateway, &error ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
}

REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv4_nexthop_object_resolved );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_ipv6_nexthop_object_resolved );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_nexthop_object_unresolved_is_no_gateway );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_netlink_nexthop_gateway_wrong_size_refuses );

#endif // #if ROCKETNET_XDP
