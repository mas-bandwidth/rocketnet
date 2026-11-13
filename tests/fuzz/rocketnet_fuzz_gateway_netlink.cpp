/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP transport's rtnetlink reply parsers: rocketnet_gateway_netlink_parse_route
// and rocketnet_gateway_netlink_parse_neighbor (sdk/rocketnet_gateway_netlink.h),
// which read each datagram of the kernel's RTM_GETROUTE dump and RTM_GETNEIGH
// reply to find the gateway and its MAC. The parsers are compiled only where
// ROCKETNET_XDP is (Linux), exactly where test_unit_xdp_gateway_netlink.cpp
// runs its cases; elsewhere this target compiles, its seed generator writes
// the seeds, and an input does nothing.
//
// INPUT: [query][datagram...]
//   query bit 0: 0 the route dump, 1 the neighbour reply
//   query bit 1: 0 AF_INET, 1 AF_INET6
//   datagram:    one received datagram, at most 32 KiB (the dump's receive
//                buffer, ROCKETNET_GATEWAY_NETLINK_BUFFER_BYTES; a larger one
//                arrives MSG_TRUNC and is refused before it is parsed), and
//                exactly as long as the input, so a read past its end trips
//                AddressSanitizer
// The request is the unit cases' one: sequence 1, port id 4242, interface 3;
// the neighbour query asks for 10.0.0.1 or fe80::1.
//
// THE ORACLES, each from the contract in sdk/rocketnet_gateway_netlink.h:
//   - the outcome is MORE, FOUND, DONE, MALFORMED or ERROR, never SYSTEM
//     (a parse reads no socket)
//   - *error is a positive errno exactly on ERROR, and 0 otherwise
//   - FOUND writes exactly the family's address size of gateway (4 or 16),
//     or the 6-byte MAC, and the bytes written are bytes of the datagram
//   - every other outcome writes nothing
//
// THE SEED GENERATOR: ROCKETNET_FUZZ_SEED_DIR=<dir> writes the datagrams the
// unit cases build (a default route found, on another interface, a device
// route, a multipath route, a refused gateway size, another table, a cloned
// route, NLMSG_ERROR, INT_MIN, an interrupted dump, a truncated message, an
// attribute past its message, another request's reply, an acknowledgement,
// and the neighbour entries found, on another interface, without a usable
// address or state), laid out as the Linux kernel writes them in native byte
// order. Where the parsers are compiled in, each is replayed and must reach
// its named outcome, else the generator exits 1 printing INCOMPLETE.

#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portable_crt.h"

#if ROCKETNET_XDP

#include "rocketnet_gateway_netlink.h"

#include <errno.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

#endif // #if ROCKETNET_XDP

// the Linux layout the seeds are written in; where the parsers are compiled
// in, each value is held to the uapi header's

#define FUZZ_NLMSG_HDRLEN           16
#define FUZZ_NLMSG_ERROR            2
#define FUZZ_NLMSG_DONE             3
#define FUZZ_NLM_F_MULTI            0x02
#define FUZZ_NLM_F_DUMP_INTR        0x10
#define FUZZ_RTM_NEWROUTE           24
#define FUZZ_RTM_NEWNEIGH           28
#define FUZZ_RTMSG_BYTES            12
#define FUZZ_NDMSG_BYTES            12
#define FUZZ_RTATTR_BYTES           4
#define FUZZ_RTNEXTHOP_BYTES        8
#define FUZZ_RTA_DST                1
#define FUZZ_RTA_OIF                4
#define FUZZ_RTA_GATEWAY            5
#define FUZZ_RTA_MULTIPATH          9
#define FUZZ_RTA_TABLE              15
#define FUZZ_NDA_DST                1
#define FUZZ_NDA_LLADDR             2
#define FUZZ_RT_TABLE_MAIN          254
#define FUZZ_RT_TABLE_LOCAL         255
#define FUZZ_RTN_UNICAST            1
#define FUZZ_RTM_F_CLONED           0x200
#define FUZZ_NUD_REACHABLE          0x02
#define FUZZ_NUD_FAILED             0x20
#define FUZZ_AF_INET                2
#define FUZZ_AF_INET6               10
#define FUZZ_EPERM                  1
#define FUZZ_ENOENT                 2
#define FUZZ_ENOBUFS                105

#if ROCKETNET_XDP
static_assert( NLMSG_HDRLEN == FUZZ_NLMSG_HDRLEN && NLMSG_ERROR == FUZZ_NLMSG_ERROR && NLMSG_DONE == FUZZ_NLMSG_DONE, "the seeds' netlink header is the kernel's" );
static_assert( NLM_F_MULTI == FUZZ_NLM_F_MULTI && NLM_F_DUMP_INTR == FUZZ_NLM_F_DUMP_INTR, "the seeds' netlink flags are the kernel's" );
static_assert( RTM_NEWROUTE == FUZZ_RTM_NEWROUTE && RTM_NEWNEIGH == FUZZ_RTM_NEWNEIGH, "the seeds' message types are the kernel's" );
static_assert( sizeof( struct rtmsg ) == FUZZ_RTMSG_BYTES && sizeof( struct ndmsg ) == FUZZ_NDMSG_BYTES, "the seeds' message bodies are the kernel's" );
static_assert( sizeof( struct rtattr ) == FUZZ_RTATTR_BYTES && sizeof( struct rtnexthop ) == FUZZ_RTNEXTHOP_BYTES, "the seeds' attribute records are the kernel's" );
static_assert( RTA_DST == FUZZ_RTA_DST && RTA_OIF == FUZZ_RTA_OIF && RTA_GATEWAY == FUZZ_RTA_GATEWAY && RTA_MULTIPATH == FUZZ_RTA_MULTIPATH && RTA_TABLE == FUZZ_RTA_TABLE,
               "the seeds' route attributes are the kernel's" );
static_assert( NDA_DST == FUZZ_NDA_DST && NDA_LLADDR == FUZZ_NDA_LLADDR, "the seeds' neighbour attributes are the kernel's" );
static_assert( RT_TABLE_MAIN == FUZZ_RT_TABLE_MAIN && RT_TABLE_LOCAL == FUZZ_RT_TABLE_LOCAL && RTN_UNICAST == FUZZ_RTN_UNICAST && RTM_F_CLONED == FUZZ_RTM_F_CLONED,
               "the seeds' route fields are the kernel's" );
static_assert( NUD_REACHABLE == FUZZ_NUD_REACHABLE && NUD_FAILED == FUZZ_NUD_FAILED, "the seeds' neighbour states are the kernel's" );
static_assert( AF_INET == FUZZ_AF_INET && AF_INET6 == FUZZ_AF_INET6, "the seeds' families are the kernel's" );
static_assert( EPERM == FUZZ_EPERM && ENOENT == FUZZ_ENOENT && ENOBUFS == FUZZ_ENOBUFS, "the seeds' errnos are the kernel's" );
#endif // #if ROCKETNET_XDP

// the request the unit cases make

#define FUZZ_NETLINK_SEQUENCE       1
#define FUZZ_NETLINK_PORT_ID        4242
#define FUZZ_SERVER_INTERFACE       3
#define FUZZ_OTHER_INTERFACE        4

// ROCKETNET_GATEWAY_NETLINK_BUFFER_BYTES in sdk/rocketnet_gateway_netlink_linux.cpp

#define FUZZ_NETLINK_BUFFER_BYTES   32768

static const uint8_t fuzz_gateway_ipv4[4] = { 10, 0, 0, 1 };
static const uint8_t fuzz_gateway_ipv4_other[4] = { 10, 0, 0, 12 };
static const uint8_t fuzz_subnet_ipv4[4] = { 10, 0, 0, 0 };
static const uint8_t fuzz_gateway_ipv6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01 };
static const uint8_t fuzz_gateway_ipv6_other[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02 };
static const uint8_t fuzz_subnet_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t fuzz_gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

#define FUZZ_NETLINK_IGNORED -1     // the input names no datagram, or the parsers are compiled out

#if ROCKETNET_XDP

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
    {
        return;
    }
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

static bool fuzz_bytes_inside( const uint8_t * data, size_t bytes, const uint8_t * value, size_t value_bytes )
{
    for ( size_t i = 0; i + value_bytes <= bytes; ++i )
    {
        if ( memcmp( data + i, value, value_bytes ) == 0 )
        {
            return true;
        }
    }

    return false;
}

static bool fuzz_all( const uint8_t * data, size_t bytes, uint8_t value )
{
    for ( size_t i = 0; i < bytes; ++i )
    {
        if ( data[i] != value )
        {
            return false;
        }
    }

    return true;
}

#endif // #if ROCKETNET_XDP

static int fuzz_netlink_run( const uint8_t * data, size_t size )
{
    if ( size < 1 || size - 1 > (size_t) FUZZ_NETLINK_BUFFER_BYTES )
    {
        return FUZZ_NETLINK_IGNORED;
    }

#if ROCKETNET_XDP
    const bool neighbor = ( data[0] & 1 ) != 0;
    const int family = ( data[0] & 2 ) ? AF_INET6 : AF_INET;
    const size_t address_bytes = family == AF_INET ? 4 : 16;
    const uint8_t * datagram = data + 1;
    const size_t bytes = size - 1;

    // the output with room past what FOUND may write, all of it a sentinel

    uint8_t output[24];
    memset( output, 0xEE, sizeof( output ) );
    int error = -12345;

    int result;
    size_t written;
    if ( neighbor )
    {
        const uint8_t * gateway = family == AF_INET ? fuzz_gateway_ipv4 : fuzz_gateway_ipv6;
        result = rocketnet_gateway_netlink_parse_neighbor( datagram, bytes, FUZZ_NETLINK_SEQUENCE, FUZZ_NETLINK_PORT_ID,
                                                           family, FUZZ_SERVER_INTERFACE, gateway, output, &error );
        written = 6;
    }
    else
    {
        result = rocketnet_gateway_netlink_parse_route( datagram, bytes, FUZZ_NETLINK_SEQUENCE, FUZZ_NETLINK_PORT_ID,
                                                        family, FUZZ_SERVER_INTERFACE, output, &error );
        written = address_bytes;
    }

    rocketnet_fuzz_require( result == ROCKETNET_GATEWAY_NETLINK_MORE || result == ROCKETNET_GATEWAY_NETLINK_FOUND ||
                                result == ROCKETNET_GATEWAY_NETLINK_DONE || result == ROCKETNET_GATEWAY_NETLINK_MALFORMED ||
                                result == ROCKETNET_GATEWAY_NETLINK_ERROR,
                            "a parse answers MORE, FOUND, DONE, MALFORMED or ERROR" );

    if ( result == ROCKETNET_GATEWAY_NETLINK_ERROR )
    {
        rocketnet_fuzz_require( error > 0, "ERROR carries a positive errno" );
    }
    else
    {
        rocketnet_fuzz_require( error == 0, "every outcome but ERROR leaves the errno 0" );
    }

    if ( result == ROCKETNET_GATEWAY_NETLINK_FOUND )
    {
        rocketnet_fuzz_require( fuzz_bytes_inside( datagram, bytes, output, written ), "FOUND writes bytes the datagram holds" );
        rocketnet_fuzz_require( fuzz_all( output + written, sizeof( output ) - written, 0xEE ), "FOUND writes only the answer's size" );
    }
    else
    {
        rocketnet_fuzz_require( fuzz_all( output, sizeof( output ), 0xEE ), "every outcome but FOUND writes nothing" );
    }

    return result;
#else // #if ROCKETNET_XDP
    (void) data;
    return FUZZ_NETLINK_IGNORED;
#endif // #if ROCKETNET_XDP
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>
// -----------------------------------------------------------------------

#define FUZZ_NETLINK_SEED_MAX 1024

// the expected outcomes, ROCKETNET_GATEWAY_NETLINK_* where it is compiled

enum
{
    FUZZ_MORE = 0,
    FUZZ_FOUND = 1,
    FUZZ_DONE = 2,
    FUZZ_MALFORMED = 3,
    FUZZ_ERROR = 4,
};

#if ROCKETNET_XDP
static_assert( ROCKETNET_GATEWAY_NETLINK_MORE == FUZZ_MORE && ROCKETNET_GATEWAY_NETLINK_FOUND == FUZZ_FOUND && ROCKETNET_GATEWAY_NETLINK_DONE == FUZZ_DONE &&
                   ROCKETNET_GATEWAY_NETLINK_MALFORMED == FUZZ_MALFORMED && ROCKETNET_GATEWAY_NETLINK_ERROR == FUZZ_ERROR,
               "the seeds' outcomes are the parser's" );
#endif // #if ROCKETNET_XDP

struct fuzz_datagram_t
{
    uint8_t bytes[FUZZ_NETLINK_SEED_MAX];
    size_t size;
};

static void fuzz_append( fuzz_datagram_t * d, const void * data, size_t bytes )
{
    if ( d->size + bytes > sizeof( d->bytes ) )
    {
        abort();
    }
    memcpy( d->bytes + d->size, data, bytes );
    d->size += bytes;
}

static void fuzz_append_u8( fuzz_datagram_t * d, uint8_t value )
{
    fuzz_append( d, &value, sizeof( value ) );
}

static void fuzz_append_u16( fuzz_datagram_t * d, uint16_t value )
{
    fuzz_append( d, &value, sizeof( value ) );
}

static void fuzz_append_u32( fuzz_datagram_t * d, uint32_t value )
{
    fuzz_append( d, &value, sizeof( value ) );
}

static void fuzz_pad( fuzz_datagram_t * d )
{
    while ( d->size % 4 )
    {
        fuzz_append_u8( d, 0 );
    }
}

// a message header; fuzz_end writes its length

static size_t fuzz_begin( fuzz_datagram_t * d, uint16_t type, uint16_t flags, uint32_t sequence = FUZZ_NETLINK_SEQUENCE )
{
    const size_t start = d->size;
    fuzz_append_u32( d, 0 );
    fuzz_append_u16( d, type );
    fuzz_append_u16( d, flags );
    fuzz_append_u32( d, sequence );
    fuzz_append_u32( d, FUZZ_NETLINK_PORT_ID );
    return start;
}

static void fuzz_end( fuzz_datagram_t * d, size_t start )
{
    const uint32_t length = (uint32_t) ( d->size - start );
    memcpy( d->bytes + start, &length, sizeof( length ) );
    fuzz_pad( d );
}

static void fuzz_attribute( fuzz_datagram_t * d, uint16_t type, const void * data, size_t bytes )
{
    fuzz_append_u16( d, (uint16_t) ( FUZZ_RTATTR_BYTES + bytes ) );
    fuzz_append_u16( d, type );
    fuzz_append( d, data, bytes );
    fuzz_pad( d );
}

static void fuzz_attribute_u32( fuzz_datagram_t * d, uint16_t type, uint32_t value )
{
    fuzz_attribute( d, type, &value, sizeof( value ) );
}

static size_t fuzz_address_bytes( int family )
{
    return family == FUZZ_AF_INET ? 4 : 16;
}

// one route as the kernel dumps it: an rtmsg, RTA_TABLE, RTA_DST when not a
// default route, RTA_GATEWAY and RTA_OIF when given

struct fuzz_route_t
{
    int family = FUZZ_AF_INET;
    uint8_t destination_length = 0;
    const uint8_t * destination = NULL;
    uint8_t table = FUZZ_RT_TABLE_MAIN;
    uint32_t flags = 0;
    int output_interface = FUZZ_SERVER_INTERFACE;   // 0: no RTA_OIF
    const uint8_t * gateway = NULL;                 // NULL: no RTA_GATEWAY
    size_t gateway_bytes = 0;                       // 0: the family's address size
    uint16_t message_flags = FUZZ_NLM_F_MULTI;
    uint32_t sequence = FUZZ_NETLINK_SEQUENCE;
};

static void fuzz_route( fuzz_datagram_t * d, const fuzz_route_t & route )
{
    const size_t start = fuzz_begin( d, FUZZ_RTM_NEWROUTE, route.message_flags, route.sequence );
    fuzz_append_u8( d, (uint8_t) route.family );      // rtm_family
    fuzz_append_u8( d, route.destination_length );   // rtm_dst_len
    fuzz_append_u8( d, 0 );                          // rtm_src_len
    fuzz_append_u8( d, 0 );                          // rtm_tos
    fuzz_append_u8( d, route.table );                // rtm_table
    fuzz_append_u8( d, 3 );                          // rtm_protocol: RTPROT_BOOT
    fuzz_append_u8( d, 0 );                          // rtm_scope: RT_SCOPE_UNIVERSE
    fuzz_append_u8( d, FUZZ_RTN_UNICAST );           // rtm_type
    fuzz_append_u32( d, route.flags );               // rtm_flags
    fuzz_attribute_u32( d, FUZZ_RTA_TABLE, route.table );
    if ( route.destination )
    {
        fuzz_attribute( d, FUZZ_RTA_DST, route.destination, fuzz_address_bytes( route.family ) );
    }

    if ( route.gateway )
    {
        fuzz_attribute( d, FUZZ_RTA_GATEWAY, route.gateway, route.gateway_bytes ? route.gateway_bytes : fuzz_address_bytes( route.family ) );
    }

    if ( route.output_interface )
    {
        fuzz_attribute_u32( d, FUZZ_RTA_OIF, (uint32_t) route.output_interface );
    }

    fuzz_end( d, start );
}

// a default route with two next hops and no RTA_OIF

static void fuzz_multipath_route( fuzz_datagram_t * d, int family, int first_interface, const uint8_t * first_gateway,
                                  int second_interface, const uint8_t * second_gateway )
{
    const size_t address_bytes = fuzz_address_bytes( family );
    const size_t start = fuzz_begin( d, FUZZ_RTM_NEWROUTE, FUZZ_NLM_F_MULTI );
    const uint8_t message[FUZZ_RTMSG_BYTES] = { (uint8_t) family, 0, 0, 0, FUZZ_RT_TABLE_MAIN, 0, 0, FUZZ_RTN_UNICAST, 0, 0, 0, 0 };
    fuzz_append( d, message, sizeof( message ) );
    fuzz_attribute_u32( d, FUZZ_RTA_TABLE, FUZZ_RT_TABLE_MAIN );

    fuzz_datagram_t hops;
    hops.size = 0;
    const int interfaces[2] = { first_interface, second_interface };
    const uint8_t * gateways[2] = { first_gateway, second_gateway };
    for ( int i = 0; i < 2; i++ )
    {
        fuzz_append_u16( &hops, (uint16_t) ( FUZZ_RTNEXTHOP_BYTES + FUZZ_RTATTR_BYTES + address_bytes ) );
        fuzz_append_u8( &hops, 0 );
        fuzz_append_u8( &hops, 0 );
        fuzz_append_u32( &hops, (uint32_t) interfaces[i] );
        fuzz_append_u16( &hops, (uint16_t) ( FUZZ_RTATTR_BYTES + address_bytes ) );
        fuzz_append_u16( &hops, FUZZ_RTA_GATEWAY );
        fuzz_append( &hops, gateways[i], address_bytes );
    }

    fuzz_attribute( d, FUZZ_RTA_MULTIPATH, hops.bytes, hops.size );
    fuzz_end( d, start );
}

// one neighbour entry: an ndmsg, NDA_DST and NDA_LLADDR when given

static void fuzz_neighbor( fuzz_datagram_t * d, int family, int interface_index, uint16_t state, const uint8_t * destination,
                           const uint8_t * lladdr, size_t lladdr_bytes )
{
    const size_t start = fuzz_begin( d, FUZZ_RTM_NEWNEIGH, 0 );
    fuzz_append_u8( d, (uint8_t) family );           // ndm_family
    fuzz_append_u8( d, 0 );                          // ndm_pad1
    fuzz_append_u16( d, 0 );                         // ndm_pad2
    fuzz_append_u32( d, (uint32_t) interface_index ); // ndm_ifindex
    fuzz_append_u16( d, state );                     // ndm_state
    fuzz_append_u8( d, 0 );                          // ndm_flags
    fuzz_append_u8( d, FUZZ_RTN_UNICAST );           // ndm_type
    if ( destination )
    {
        fuzz_attribute( d, FUZZ_NDA_DST, destination, fuzz_address_bytes( family ) );
    }

    if ( lladdr )
    {
        fuzz_attribute( d, FUZZ_NDA_LLADDR, lladdr, lladdr_bytes );
    }

    fuzz_end( d, start );
}

// NLMSG_DONE carrying the dump's result, and NLMSG_ERROR carrying an
// nlmsgerr (the code and the request's header)

static void fuzz_done( fuzz_datagram_t * d, int32_t code = 0, uint32_t sequence = FUZZ_NETLINK_SEQUENCE )
{
    const size_t start = fuzz_begin( d, FUZZ_NLMSG_DONE, FUZZ_NLM_F_MULTI, sequence );
    fuzz_append( d, &code, sizeof( code ) );
    fuzz_end( d, start );
}

static void fuzz_error( fuzz_datagram_t * d, int32_t code )
{
    const size_t start = fuzz_begin( d, FUZZ_NLMSG_ERROR, 0 );
    fuzz_append( d, &code, sizeof( code ) );
    const uint8_t request[FUZZ_NLMSG_HDRLEN] = { 0 };
    fuzz_append( d, request, sizeof( request ) );
    fuzz_end( d, start );
}

// the routes a family's table lists ahead of its default route on this
// interface: a subnet route on this interface and a default route on
// another interface

static void fuzz_routes_before_default( fuzz_datagram_t * d, int family )
{
    const bool ipv4 = family == FUZZ_AF_INET;

    fuzz_route_t subnet;
    subnet.family = family;
    subnet.destination_length = ipv4 ? 24 : 64;
    subnet.destination = ipv4 ? fuzz_subnet_ipv4 : fuzz_subnet_ipv6;
    subnet.gateway = ipv4 ? fuzz_gateway_ipv4_other : fuzz_gateway_ipv6_other;
    fuzz_route( d, subnet );

    fuzz_route_t elsewhere;
    elsewhere.family = family;
    elsewhere.output_interface = FUZZ_OTHER_INTERFACE;
    elsewhere.gateway = ipv4 ? fuzz_gateway_ipv4_other : fuzz_gateway_ipv6_other;
    fuzz_route( d, elsewhere );
}

enum
{
    FUZZ_SEED_ROUTE_IPV4_FOUND = 0,
    FUZZ_SEED_ROUTE_IPV6_FOUND,
    FUZZ_SEED_ROUTE_NOT_ON_THIS_INTERFACE,
    FUZZ_SEED_ROUTE_DEVICE_ROUTE,
    FUZZ_SEED_ROUTE_MORE,
    FUZZ_SEED_ROUTE_EMPTY_TABLE,
    FUZZ_SEED_ROUTE_IPV4_MULTIPATH,
    FUZZ_SEED_ROUTE_IPV6_MULTIPATH,
    FUZZ_SEED_ROUTE_GATEWAY_WRONG_SIZE,
    FUZZ_SEED_ROUTE_OTHER_TABLE,
    FUZZ_SEED_ROUTE_CLONED,
    FUZZ_SEED_ROUTE_ERROR,
    FUZZ_SEED_ROUTE_ERROR_INT_MIN,
    FUZZ_SEED_ROUTE_DONE_ERRNO,
    FUZZ_SEED_ROUTE_INTERRUPTED,
    FUZZ_SEED_ROUTE_TRUNCATED,
    FUZZ_SEED_ROUTE_ATTRIBUTE_PAST_END,
    FUZZ_SEED_ROUTE_OTHER_REQUEST,
    FUZZ_SEED_ROUTE_ACK,
    FUZZ_SEED_NEIGHBOR_IPV4_FOUND,
    FUZZ_SEED_NEIGHBOR_IPV6_FOUND,
    FUZZ_SEED_NEIGHBOR_WRONG_INTERFACE,
    FUZZ_SEED_NEIGHBOR_NO_LLADDR,
    FUZZ_SEED_NEIGHBOR_LLADDR_WRONG_SIZE,
    FUZZ_SEED_NEIGHBOR_FAILED_STATE,
    FUZZ_SEED_NEIGHBOR_OTHER_GATEWAY,
    FUZZ_SEED_NEIGHBOR_ENOENT,
};

struct fuzz_netlink_seed_t
{
    const char * name;
    int kind;
    uint8_t query;
    int outcome;
};

static const fuzz_netlink_seed_t fuzz_netlink_seeds[] = {
    { "route_ipv4_found", FUZZ_SEED_ROUTE_IPV4_FOUND, 0, FUZZ_FOUND },
    { "route_ipv6_found", FUZZ_SEED_ROUTE_IPV6_FOUND, 2, FUZZ_FOUND },
    { "route_not_on_this_interface", FUZZ_SEED_ROUTE_NOT_ON_THIS_INTERFACE, 0, FUZZ_DONE },
    { "route_device_route", FUZZ_SEED_ROUTE_DEVICE_ROUTE, 0, FUZZ_DONE },
    { "route_more", FUZZ_SEED_ROUTE_MORE, 0, FUZZ_MORE },
    { "route_empty_table", FUZZ_SEED_ROUTE_EMPTY_TABLE, 0, FUZZ_DONE },
    { "route_ipv4_multipath_second_hop", FUZZ_SEED_ROUTE_IPV4_MULTIPATH, 0, FUZZ_FOUND },
    { "route_ipv6_multipath_second_hop", FUZZ_SEED_ROUTE_IPV6_MULTIPATH, 2, FUZZ_FOUND },
    { "route_gateway_wrong_size", FUZZ_SEED_ROUTE_GATEWAY_WRONG_SIZE, 0, FUZZ_MALFORMED },
    { "route_other_table", FUZZ_SEED_ROUTE_OTHER_TABLE, 0, FUZZ_DONE },
    { "route_cloned", FUZZ_SEED_ROUTE_CLONED, 2, FUZZ_DONE },
    { "route_error", FUZZ_SEED_ROUTE_ERROR, 0, FUZZ_ERROR },
    { "route_error_int_min", FUZZ_SEED_ROUTE_ERROR_INT_MIN, 0, FUZZ_MALFORMED },
    { "route_done_errno", FUZZ_SEED_ROUTE_DONE_ERRNO, 0, FUZZ_ERROR },
    { "route_interrupted", FUZZ_SEED_ROUTE_INTERRUPTED, 0, FUZZ_ERROR },
    { "route_truncated", FUZZ_SEED_ROUTE_TRUNCATED, 0, FUZZ_MALFORMED },
    { "route_attribute_past_end", FUZZ_SEED_ROUTE_ATTRIBUTE_PAST_END, 0, FUZZ_MALFORMED },
    { "route_other_request", FUZZ_SEED_ROUTE_OTHER_REQUEST, 0, FUZZ_DONE },
    { "route_ack", FUZZ_SEED_ROUTE_ACK, 0, FUZZ_MORE },
    { "neighbor_ipv4_found", FUZZ_SEED_NEIGHBOR_IPV4_FOUND, 1, FUZZ_FOUND },
    { "neighbor_ipv6_found", FUZZ_SEED_NEIGHBOR_IPV6_FOUND, 3, FUZZ_FOUND },
    { "neighbor_wrong_interface", FUZZ_SEED_NEIGHBOR_WRONG_INTERFACE, 1, FUZZ_DONE },
    { "neighbor_no_lladdr", FUZZ_SEED_NEIGHBOR_NO_LLADDR, 1, FUZZ_DONE },
    { "neighbor_lladdr_wrong_size", FUZZ_SEED_NEIGHBOR_LLADDR_WRONG_SIZE, 3, FUZZ_DONE },
    { "neighbor_failed_state", FUZZ_SEED_NEIGHBOR_FAILED_STATE, 1, FUZZ_DONE },
    { "neighbor_other_gateway", FUZZ_SEED_NEIGHBOR_OTHER_GATEWAY, 1, FUZZ_DONE },
    { "neighbor_enoent", FUZZ_SEED_NEIGHBOR_ENOENT, 1, FUZZ_ERROR },
};

static void fuzz_netlink_seed_build( const fuzz_netlink_seed_t * seed, fuzz_datagram_t * d )
{
    // the datagram alone, so every pad is relative to its start; the
    // generator puts the query byte in front

    d->size = 0;

    const int family = ( seed->query & 2 ) ? FUZZ_AF_INET6 : FUZZ_AF_INET;
    const bool ipv4 = family == FUZZ_AF_INET;
    const uint8_t * gateway = ipv4 ? fuzz_gateway_ipv4 : fuzz_gateway_ipv6;
    const uint8_t * other = ipv4 ? fuzz_gateway_ipv4_other : fuzz_gateway_ipv6_other;

    fuzz_route_t route;
    route.family = family;
    route.gateway = gateway;

    switch ( seed->kind )
    {
        case FUZZ_SEED_ROUTE_IPV4_FOUND:
        case FUZZ_SEED_ROUTE_IPV6_FOUND:
            fuzz_routes_before_default( d, family );
            fuzz_route( d, route );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_NOT_ON_THIS_INTERFACE:
            fuzz_routes_before_default( d, family );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_DEVICE_ROUTE:
        {
            route.gateway = NULL;
            fuzz_route( d, route );
            fuzz_route_t elsewhere;
            elsewhere.family = family;
            elsewhere.output_interface = FUZZ_OTHER_INTERFACE;
            elsewhere.gateway = gateway;
            fuzz_route( d, elsewhere );
            fuzz_done( d );
        }
        break;
        case FUZZ_SEED_ROUTE_MORE:
        {
            fuzz_route_t subnet;
            subnet.family = family;
            subnet.destination_length = 8;
            subnet.destination = fuzz_subnet_ipv4;
            subnet.gateway = gateway;
            fuzz_route( d, subnet );
        }
        break;
        case FUZZ_SEED_ROUTE_EMPTY_TABLE:
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_IPV4_MULTIPATH:
        case FUZZ_SEED_ROUTE_IPV6_MULTIPATH:
            fuzz_multipath_route( d, family, FUZZ_OTHER_INTERFACE, other, FUZZ_SERVER_INTERFACE, gateway );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_GATEWAY_WRONG_SIZE:
            route.gateway = fuzz_gateway_ipv6;
            route.gateway_bytes = 16;
            fuzz_route( d, route );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_OTHER_TABLE:
            route.table = FUZZ_RT_TABLE_LOCAL;
            fuzz_route( d, route );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_CLONED:
            route.flags = FUZZ_RTM_F_CLONED;
            fuzz_route( d, route );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_ERROR:
            fuzz_error( d, -FUZZ_EPERM );
            break;
        case FUZZ_SEED_ROUTE_ERROR_INT_MIN:
            fuzz_error( d, INT32_MIN );
            break;
        case FUZZ_SEED_ROUTE_DONE_ERRNO:
            fuzz_done( d, -FUZZ_ENOBUFS );
            break;
        case FUZZ_SEED_ROUTE_INTERRUPTED:
            route.message_flags = FUZZ_NLM_F_MULTI | FUZZ_NLM_F_DUMP_INTR;
            fuzz_route( d, route );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_TRUNCATED:
            fuzz_route( d, route );
            d->size -= 4;                               // the message's length now runs past the datagram
            break;
        case FUZZ_SEED_ROUTE_ATTRIBUTE_PAST_END:
        {
            fuzz_route( d, route );

            // the last attribute (RTA_OIF) claims 4 bytes more than its message holds

            uint16_t length;
            memcpy( &length, d->bytes + d->size - 8, sizeof( length ) );
            length = (uint16_t) ( length + 4 );
            memcpy( d->bytes + d->size - 8, &length, sizeof( length ) );
        }
        break;
        case FUZZ_SEED_ROUTE_OTHER_REQUEST:
            route.sequence = FUZZ_NETLINK_SEQUENCE + 1;
            fuzz_route( d, route );
            fuzz_done( d );
            break;
        case FUZZ_SEED_ROUTE_ACK:
            fuzz_error( d, 0 );
            break;
        case FUZZ_SEED_NEIGHBOR_IPV4_FOUND:
        case FUZZ_SEED_NEIGHBOR_IPV6_FOUND:
            fuzz_neighbor( d, family, FUZZ_SERVER_INTERFACE, FUZZ_NUD_REACHABLE, gateway, fuzz_gateway_mac, 6 );
            break;
        case FUZZ_SEED_NEIGHBOR_WRONG_INTERFACE:
            fuzz_neighbor( d, family, FUZZ_OTHER_INTERFACE, FUZZ_NUD_REACHABLE, gateway, fuzz_gateway_mac, 6 );
            fuzz_done( d );
            break;
        case FUZZ_SEED_NEIGHBOR_NO_LLADDR:
            fuzz_neighbor( d, family, FUZZ_SERVER_INTERFACE, FUZZ_NUD_REACHABLE, gateway, NULL, 0 );
            fuzz_done( d );
            break;
        case FUZZ_SEED_NEIGHBOR_LLADDR_WRONG_SIZE:
        {
            const uint8_t lladdr[8] = { 0x02, 0, 0, 0, 0, 0x01, 0, 0 };
            fuzz_neighbor( d, family, FUZZ_SERVER_INTERFACE, FUZZ_NUD_REACHABLE, gateway, lladdr, sizeof( lladdr ) );
            fuzz_done( d );
        }
        break;
        case FUZZ_SEED_NEIGHBOR_FAILED_STATE:
            fuzz_neighbor( d, family, FUZZ_SERVER_INTERFACE, FUZZ_NUD_FAILED, gateway, fuzz_gateway_mac, 6 );
            fuzz_done( d );
            break;
        case FUZZ_SEED_NEIGHBOR_OTHER_GATEWAY:
            fuzz_neighbor( d, family, FUZZ_SERVER_INTERFACE, FUZZ_NUD_REACHABLE, other, fuzz_gateway_mac, 6 );
            fuzz_done( d );
            break;
        case FUZZ_SEED_NEIGHBOR_ENOENT:
            fuzz_error( d, -FUZZ_ENOENT );
            break;
        default:
            break;
    }
}

static bool fuzz_seed_write( const char * dir, const char * name, const uint8_t * data, size_t bytes )
{
    // ROCKETNET_FUZZ_WITHHOLD=<seed name> skips that seed and reports it
    // unwritten, so the generator ends INCOMPLETE: tools/check_fuzz_seeds.sh
    // runs this control for every generator (VERIFY-5 W4)
    char withheld[256];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_WITHHOLD", withheld, sizeof( withheld ) ) && strcmp( withheld, name ) == 0 )
    {
        fprintf( stderr, "seed generator: withholding %s\n", name );
        return false;
    }

    char path[1024];
    if ( snprintf( path, sizeof( path ), "%s/%s", dir, name ) >= (int) sizeof( path ) )
    {
        fprintf( stderr, "seed generator: the seed path for %s does not fit\n", name );
        return false;
    }

    FILE * file = rocketnet_test_fopen( path, "wb" );
    if ( !file )
    {
        fprintf( stderr, "seed generator: cannot write %s\n", path );
        return false;
    }

    const bool ok = fwrite( data, 1, bytes, file ) == bytes;

    fclose( file );
    if ( !ok )
    {
        fprintf( stderr, "seed generator: cannot write %s\n", path );
    }

    return ok;
}

static bool fuzz_generate( const char * dir )
{
    static fuzz_datagram_t datagram;
    static uint8_t seed[1 + FUZZ_NETLINK_SEED_MAX];
    bool complete = true;

    for ( size_t i = 0; i < sizeof( fuzz_netlink_seeds ) / sizeof( fuzz_netlink_seeds[0] ); ++i )
    {
        const fuzz_netlink_seed_t * entry = &fuzz_netlink_seeds[i];
        fuzz_netlink_seed_build( entry, &datagram );
        if ( datagram.size == 0 )
        {
            fprintf( stderr, "seed generator: no seed for %s\n", entry->name );
            complete = false;
            continue;
        }

        seed[0] = entry->query;
        memcpy( seed + 1, datagram.bytes, datagram.size );
        const size_t bytes = 1 + datagram.size;

#if ROCKETNET_XDP
        if ( fuzz_netlink_run( seed, bytes ) != entry->outcome )
        {
            fprintf( stderr, "seed generator: the %s seed does not reach its outcome\n", entry->name );
            complete = false;
            continue;
        }
#endif // #if ROCKETNET_XDP

        if ( !fuzz_seed_write( dir, entry->name, seed, bytes ) )
        {
            complete = false;
        }
    }

#if ROCKETNET_XDP
    printf( "seed generator: %s\n", complete ? "every seed reaches the outcome it is named for" : "INCOMPLETE" );
#else // #if ROCKETNET_XDP
    printf( "seed generator: %s\n", complete ? "seeds written; the parsers are compiled out here, so none was replayed" : "INCOMPLETE" );
#endif // #if ROCKETNET_XDP
    return complete;
}

// -----------------------------------------------------------------------

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    char seed_dir[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
    {
        exit( fuzz_generate( seed_dir ) ? 0 : 1 );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    (void) fuzz_netlink_run( data, size );
    return 0;
}
