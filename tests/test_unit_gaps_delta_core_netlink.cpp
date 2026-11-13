/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Mutation gaps (delta, core): XDP gateway discovery's parsers
// (sdk/rocketnet_gateway_netlink_linux.cpp) over canned rtnetlink datagrams,
// built here byte by byte from the uapi structs, and the route query over a
// scripted fake netlink system. No socket, no netlink, no real time.
//
//   - the nexthop ids a route dump records: each distinct id once, in dump
//     order, at most eight; each recorded id asked once after the dump, a
//     later one after an earlier one that does not resolve
//   - a nexthop object with no NHA_ID is id 0, never the id asked
//   - the link parser: a one-byte (empty) string attribute, an IFLA_LINK of
//     INT_MAX, and VLAN data read only for kind "vlan"
//   - the VLAN classifier: a lower device of any kind but "" or "veth" refuses

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#if ROCKETNET_XDP

#include "rocketnet_gateway_netlink.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <vector>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/nexthop.h>

static const uint32_t gaps_netlink_sequence = 1;
static const uint32_t gaps_netlink_port_id = 4242;
static const int gaps_server_interface = 3;
static const int gaps_other_interface = 4;
static const uint8_t gaps_gateway_ipv4[4] = { 10, 0, 0, 1 };

struct gaps_datagram_t
{
    std::vector<uint8_t> bytes;
};

static void gaps_append( gaps_datagram_t * datagram, const void * data, size_t bytes )
{
    const uint8_t * p = (const uint8_t *) data;
    datagram->bytes.insert( datagram->bytes.end(), p, p + bytes );
}

static void gaps_pad( gaps_datagram_t * datagram )
{
    while ( datagram->bytes.size() % 4 )
    {
        datagram->bytes.push_back( 0 );
    }
}

static size_t gaps_begin( gaps_datagram_t * datagram, uint16_t type, uint16_t flags )
{
    const size_t start = datagram->bytes.size();
    struct nlmsghdr header;
    memset( &header, 0, sizeof( header ) );
    header.nlmsg_type = type;
    header.nlmsg_flags = flags;
    header.nlmsg_seq = gaps_netlink_sequence;
    header.nlmsg_pid = gaps_netlink_port_id;
    gaps_append( datagram, &header, sizeof( header ) );
    return start;
}

static void gaps_end( gaps_datagram_t * datagram, size_t start )
{
    const uint32_t length = (uint32_t) ( datagram->bytes.size() - start );
    memcpy( datagram->bytes.data() + start, &length, sizeof( length ) );
    gaps_pad( datagram );
}

static void gaps_attribute( gaps_datagram_t * datagram, uint16_t type, const void * data, size_t bytes )
{
    struct rtattr attribute;
    attribute.rta_len = (unsigned short) RTA_LENGTH( bytes );
    attribute.rta_type = type;
    gaps_append( datagram, &attribute, sizeof( attribute ) );
    gaps_append( datagram, data, bytes );
    gaps_pad( datagram );
}

static void gaps_attribute_u32( gaps_datagram_t * datagram, uint16_t type, uint32_t value )
{
    gaps_attribute( datagram, type, &value, sizeof( value ) );
}

static void gaps_done( gaps_datagram_t * datagram )
{
    const size_t start = gaps_begin( datagram, NLMSG_DONE, NLM_F_MULTI );
    const int code = 0;
    gaps_append( datagram, &code, sizeof( code ) );
    gaps_end( datagram, start );
}

// one default route of the main table that names a nexthop object by id

static void gaps_nexthop_route( gaps_datagram_t * datagram, uint32_t id )
{
    const size_t start = gaps_begin( datagram, RTM_NEWROUTE, NLM_F_MULTI );
    struct rtmsg message;
    memset( &message, 0, sizeof( message ) );
    message.rtm_family = AF_INET;
    message.rtm_table = RT_TABLE_MAIN;
    message.rtm_protocol = RTPROT_BOOT;
    message.rtm_scope = RT_SCOPE_UNIVERSE;
    message.rtm_type = RTN_UNICAST;
    gaps_append( datagram, &message, sizeof( message ) );
    gaps_attribute_u32( datagram, RTA_TABLE, RT_TABLE_MAIN );
    gaps_attribute_u32( datagram, RTA_NH_ID, id );
    gaps_end( datagram, start );
}

static gaps_datagram_t gaps_nexthop_dump( const std::vector<uint32_t> & ids )
{
    gaps_datagram_t datagram;
    for ( size_t i = 0; i < ids.size(); i++ )
    {
        gaps_nexthop_route( &datagram, ids[i] );
    }
    gaps_done( &datagram );
    return datagram;
}

// a nexthop object: IPv4, a gateway on the given interface; with_id false
// leaves out NHA_ID

static gaps_datagram_t gaps_nexthop_reply( uint32_t id, int output_interface, bool with_id = true )
{
    gaps_datagram_t datagram;
    const size_t start = gaps_begin( &datagram, RTM_NEWNEXTHOP, 0 );
    struct nhmsg message;
    memset( &message, 0, sizeof( message ) );
    message.nh_family = AF_INET;
    message.nh_protocol = RTPROT_BOOT;
    gaps_append( &datagram, &message, sizeof( message ) );
    if ( with_id )
    {
        gaps_attribute_u32( &datagram, NHA_ID, id );
    }
    gaps_attribute_u32( &datagram, NHA_OIF, (uint32_t) output_interface );
    gaps_attribute( &datagram, NHA_GATEWAY, gaps_gateway_ipv4, sizeof( gaps_gateway_ipv4 ) );
    gaps_end( &datagram, start );
    return datagram;
}

// ---------------------------------------------------------------------------
// the fake netlink system: replies are a script read in order across dumps,
// every request is kept

struct gaps_fake_netlink_t
{
    std::vector<gaps_datagram_t> script;
    std::vector<gaps_datagram_t> requests;
    size_t next = 0;
};

static gaps_fake_netlink_t * gaps_fake = NULL;

static int gaps_fake_socket( int, int, int )
{
    return 101;
}

static int gaps_fake_setsockopt( int, int, int, const void *, socklen_t )
{
    return 0;
}

static int gaps_fake_bind( int, const struct sockaddr *, socklen_t )
{
    return 0;
}

static int gaps_fake_getsockname( int, struct sockaddr * address, socklen_t * address_bytes )
{
    struct sockaddr_nl local;
    memset( &local, 0, sizeof( local ) );
    local.nl_family = AF_NETLINK;
    local.nl_pid = gaps_netlink_port_id;
    memcpy( address, &local, sizeof( local ) );
    *address_bytes = sizeof( local );
    return 0;
}

static ssize_t gaps_fake_sendto( int, const void * data, size_t bytes, int, const struct sockaddr *, socklen_t )
{
    gaps_datagram_t request;
    gaps_append( &request, data, bytes );
    gaps_fake->requests.push_back( request );
    return (ssize_t) bytes;
}

static ssize_t gaps_fake_recvmsg( int, struct msghdr * message, int )
{
    if ( gaps_fake->next >= gaps_fake->script.size() )
    {
        errno = EAGAIN;
        return -1;
    }

    const gaps_datagram_t & datagram = gaps_fake->script[gaps_fake->next++];
    memcpy( message->msg_iov[0].iov_base, datagram.bytes.data(), datagram.bytes.size() );
    struct sockaddr_nl kernel;
    memset( &kernel, 0, sizeof( kernel ) );
    kernel.nl_family = AF_NETLINK;
    memcpy( message->msg_name, &kernel, sizeof( kernel ) );
    message->msg_namelen = sizeof( kernel );
    message->msg_flags = 0;
    return (ssize_t) datagram.bytes.size();
}

static int gaps_fake_close( int )
{
    return 0;
}

static rocketnet_gateway_netlink_system_t gaps_fake_system()
{
    rocketnet_gateway_netlink_system_t system;
    memset( &system, 0, sizeof( system ) );
    system.socket = gaps_fake_socket;
    system.setsockopt = gaps_fake_setsockopt;
    system.bind = gaps_fake_bind;
    system.getsockname = gaps_fake_getsockname;
    system.sendto = gaps_fake_sendto;
    system.recvmsg = gaps_fake_recvmsg;
    system.close = gaps_fake_close;
    return system;
}

static int gaps_route_query( gaps_fake_netlink_t * fake, uint8_t * gateway, int * error )
{
    const rocketnet_gateway_netlink_system_t system = gaps_fake_system();
    gaps_fake = fake;
    memset( gateway, 0xEE, 16 );
    *error = -1;
    const int result = rocketnet_gateway_netlink_route_with( &system, AF_INET, gaps_server_interface, gateway, error );
    gaps_fake = NULL;
    return result;
}

// the ids the RTM_GETNEXTHOP requests after the dump named, in order

static std::vector<uint32_t> gaps_nexthop_ids_asked( const gaps_fake_netlink_t & fake )
{
    std::vector<uint32_t> ids;
    for ( size_t i = 0; i < fake.requests.size(); i++ )
    {
        struct nlmsghdr header;
        memcpy( &header, fake.requests[i].bytes.data(), sizeof( header ) );
        if ( header.nlmsg_type != RTM_GETNEXTHOP )
        {
            continue;
        }
        uint32_t id = 0;
        memcpy( &id, fake.requests[i].bytes.data() + NLMSG_SPACE( sizeof( struct nhmsg ) ) + RTA_LENGTH( 0 ), sizeof( id ) );
        ids.push_back( id );
    }
    return ids;
}

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:342:13:negate_condition and 342:40:relational:!=
//   (a second distinct id is taken for a duplicate and never recorded)
// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:340:55:arithmetic:-- (the duplicate walk runs below the array)
// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:1018:50:arithmetic:-- (the query walk runs below the array)
//
// Two default routes name objects 7 and 9. Object 7 is on another interface,
// so no gateway; object 9 is on the server's: its gateway is the answer, found
// by asking 7 and then 9.

static void test_gaps_delta_core_netlink_later_nexthop_id_resolves()
{
    gaps_fake_netlink_t fake;
    fake.script.push_back( gaps_nexthop_dump( { 7, 9 } ) );
    fake.script.push_back( gaps_nexthop_reply( 7, gaps_other_interface ) );
    fake.script.push_back( gaps_nexthop_reply( 9, gaps_server_interface ) );

    uint8_t gateway[16];
    int error = 0;
    const int result = gaps_route_query( &fake, gateway, &error );
    const bool second_nexthop_object_gives_the_gateway = result == ROCKETNET_GATEWAY_NETLINK_FOUND && memcmp( gateway, gaps_gateway_ipv4, 4 ) == 0;
    rocketnet_unit_check( second_nexthop_object_gives_the_gateway );

    const std::vector<uint32_t> asked = gaps_nexthop_ids_asked( fake );
    const bool each_nexthop_id_asked_once_in_dump_order = asked.size() == 2 && asked[0] == 7 && asked[1] == 9;
    rocketnet_unit_check( each_nexthop_id_asked_once_in_dump_order );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_later_nexthop_id_resolves );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:340:23:off_by_one:1 (the duplicate walk skips the first id)
// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:340:28:relational:> (no duplicate walk)
//
// Routes naming 7, 7, 9, 7: two distinct objects, each asked once.

static void test_gaps_delta_core_netlink_duplicate_nexthop_ids_asked_once()
{
    gaps_fake_netlink_t fake;
    fake.script.push_back( gaps_nexthop_dump( { 7, 7, 9, 7 } ) );
    fake.script.push_back( gaps_nexthop_reply( 7, gaps_other_interface ) );
    fake.script.push_back( gaps_nexthop_reply( 9, gaps_server_interface ) );

    uint8_t gateway[16];
    int error = 0;
    const int result = gaps_route_query( &fake, gateway, &error );

    const std::vector<uint32_t> asked = gaps_nexthop_ids_asked( fake );
    const bool duplicate_nexthop_ids_asked_once = asked.size() == 2 && asked[0] == 7 && asked[1] == 9;
    rocketnet_unit_check( duplicate_nexthop_ids_asked_once );
    const bool gateway_found_after_duplicates = result == ROCKETNET_GATEWAY_NETLINK_FOUND && memcmp( gateway, gaps_gateway_ipv4, 4 ) == 0;
    rocketnet_unit_check( gateway_found_after_duplicates );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_duplicate_nexthop_ids_asked_once );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:340:28:relational:<= (the duplicate walk reads one
//   past the ids recorded, a zeroed slot)
//
// The walk compares only the ids recorded: the first route's id 0 meets no
// recorded id, so it is recorded and asked like any other.

static void test_gaps_delta_core_netlink_first_nexthop_id_compared_only_with_recorded_ids()
{
    gaps_fake_netlink_t fake;
    fake.script.push_back( gaps_nexthop_dump( { 0 } ) );
    fake.script.push_back( gaps_nexthop_reply( 0, gaps_server_interface ) );

    uint8_t gateway[16];
    int error = 0;
    const int result = gaps_route_query( &fake, gateway, &error );

    const std::vector<uint32_t> asked = gaps_nexthop_ids_asked( fake );
    const bool first_id_recorded_though_it_equals_an_unused_slot = asked.size() == 1 && asked[0] == 0;
    rocketnet_unit_check( first_id_recorded_though_it_equals_an_unused_slot );
    rocketnet_unit_check( result == ROCKETNET_GATEWAY_NETLINK_FOUND );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_first_nexthop_id_compared_only_with_recorded_ids );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:348:37:relational:<= (a ninth id written past the array)
//
// Nine distinct ids: the first eight are recorded and asked, the ninth is not.
// None resolves, so the answer is DONE, no gateway.

static void test_gaps_delta_core_netlink_nexthop_ids_bounded_at_eight()
{
    gaps_fake_netlink_t fake;
    fake.script.push_back( gaps_nexthop_dump( { 11, 12, 13, 14, 15, 16, 17, 18, 19 } ) );
    for ( uint32_t id = 11; id <= 19; id++ )
    {
        fake.script.push_back( gaps_nexthop_reply( id, gaps_other_interface ) );
    }

    uint8_t gateway[16];
    int error = 0;
    const int result = gaps_route_query( &fake, gateway, &error );

    const std::vector<uint32_t> asked = gaps_nexthop_ids_asked( fake );
    bool first_eight_in_order = asked.size() == 8;
    for ( size_t i = 0; first_eight_in_order && i < asked.size(); i++ )
    {
        first_eight_in_order = asked[i] == 11 + (uint32_t) i;
    }
    const bool only_the_first_eight_nexthop_ids_asked = first_eight_in_order;
    rocketnet_unit_check( only_the_first_eight_nexthop_ids_asked );
    rocketnet_unit_check( result == ROCKETNET_GATEWAY_NETLINK_DONE );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_nexthop_ids_bounded_at_eight );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:424:19:off_by_one:1 (an object without NHA_ID read as id 1)
//
// The route names object 1; the reply carries no NHA_ID, so it is not object
// 1, and there is no gateway.

static void test_gaps_delta_core_netlink_nexthop_without_id_is_not_the_asked_object()
{
    gaps_fake_netlink_t fake;
    fake.script.push_back( gaps_nexthop_dump( { 1 } ) );
    fake.script.push_back( gaps_nexthop_reply( 1, gaps_server_interface, false ) );

    uint8_t gateway[16];
    int error = 0;
    const int result = gaps_route_query( &fake, gateway, &error );
    const bool object_without_nha_id_is_no_answer_for_id_1 = result == ROCKETNET_GATEWAY_NETLINK_DONE;
    rocketnet_unit_check( object_without_nha_id_is_no_answer_for_id_1 );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_nexthop_without_id_is_not_the_asked_object );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:397:23:off_by_one:1 (parse_route's context filled with 1s)
//
// parse_route over a dump of one nexthop-object route: the route names no
// gateway of its own, so the datagram is the dump's end with no answer. Its
// context starts zeroed: with the record count filled with 1s the duplicate
// walk runs off the context (a crash, or the ASan unit legs' stack overflow).

static void test_gaps_delta_core_netlink_parse_route_context_starts_zeroed()
{
    const gaps_datagram_t datagram = gaps_nexthop_dump( { 0x5A5A1234u } );
    uint8_t gateway[16];
    memset( gateway, 0xEE, sizeof( gateway ) );
    int error = -1;
    const int result = rocketnet_gateway_netlink_parse_route( datagram.bytes.data(), datagram.bytes.size(), gaps_netlink_sequence, gaps_netlink_port_id,
                                                             AF_INET, gaps_server_interface, gateway, &error );
    const bool nexthop_route_parses_to_the_dump_end = result == ROCKETNET_GATEWAY_NETLINK_DONE;
    rocketnet_unit_check( nexthop_route_parses_to_the_dump_end );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_parse_route_context_starts_zeroed );

// ---------------------------------------------------------------------------
// the link parser and the VLAN classifier

struct gaps_link_t
{
    const char * kind = NULL;           // NULL: no IFLA_LINKINFO
    int64_t link = -1;                  // -1: no IFLA_LINK
    int vlan_id = -1;                   // -1: no IFLA_INFO_DATA
};

static int gaps_parse_link( const gaps_link_t & link, rocketnet_gateway_netlink_link_t * out )
{
    gaps_datagram_t datagram;
    const size_t start = gaps_begin( &datagram, RTM_NEWLINK, 0 );
    struct ifinfomsg message;
    memset( &message, 0, sizeof( message ) );
    message.ifi_family = AF_UNSPEC;
    message.ifi_index = gaps_server_interface;
    gaps_append( &datagram, &message, sizeof( message ) );
    gaps_attribute( &datagram, IFLA_IFNAME, "eth0", 5 );
    if ( link.link >= 0 )
    {
        gaps_attribute_u32( &datagram, IFLA_LINK, (uint32_t) link.link );
    }
    if ( link.kind )
    {
        gaps_datagram_t info;
        gaps_attribute( &info, IFLA_INFO_KIND, link.kind, strlen( link.kind ) + 1 );
        if ( link.vlan_id >= 0 )
        {
            gaps_datagram_t data;
            const uint16_t vlan_id = (uint16_t) link.vlan_id;
            gaps_attribute( &data, IFLA_VLAN_ID, &vlan_id, sizeof( vlan_id ) );
            gaps_attribute( &info, IFLA_INFO_DATA, data.bytes.data(), data.bytes.size() );
        }
        gaps_attribute( &datagram, IFLA_LINKINFO, info.bytes.data(), info.bytes.size() );
    }
    gaps_end( &datagram, start );

    int error = 0;
    return rocketnet_gateway_netlink_parse_link( datagram.bytes.data(), datagram.bytes.size(), gaps_netlink_sequence, gaps_netlink_port_id,
                                                 gaps_server_interface, out, &error );
}

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:548:36:off_by_one:1 (a one-byte string refused)
//
// IFLA_INFO_KIND of one byte, the NUL alone: an empty kind, read as "".

static void test_gaps_delta_core_netlink_link_empty_kind_reads_as_empty()
{
    gaps_link_t link;
    link.kind = "";
    rocketnet_gateway_netlink_link_t out;
    const bool one_byte_kind_is_the_empty_kind = gaps_parse_link( link, &out ) == ROCKETNET_GATEWAY_NETLINK_FOUND && out.kind[0] == '\0';
    rocketnet_unit_check( one_byte_kind_is_the_empty_kind );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_link_empty_kind_reads_as_empty );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:596:87:relational:>= (IFLA_LINK of INT_MAX refused)
//
// IFLA_LINK of INT_MAX is an index; one past it is malformed.

static void test_gaps_delta_core_netlink_link_index_int_max_accepted()
{
    rocketnet_gateway_netlink_link_t out;
    gaps_link_t link;
    link.link = INT_MAX;
    const bool link_index_int_max_read = gaps_parse_link( link, &out ) == ROCKETNET_GATEWAY_NETLINK_FOUND && out.link_index == INT_MAX;
    rocketnet_unit_check( link_index_int_max_read );

    link.link = (int64_t) INT_MAX + 1;
    rocketnet_unit_check( gaps_parse_link( link, &out ) == ROCKETNET_GATEWAY_NETLINK_MALFORMED );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_link_index_int_max_accepted );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:621:48:boolean:|| (VLAN data read for another kind)
//
// A veth (or a device with no kind) whose IFLA_INFO_DATA holds a type-1
// attribute is no VLAN: its vlan_id stays 0. The same data on kind "vlan" is
// the VLAN ID.

static void test_gaps_delta_core_netlink_link_vlan_data_only_for_vlan_kind()
{
    rocketnet_gateway_netlink_link_t out;
    gaps_link_t link;
    link.kind = "veth";
    link.vlan_id = 100;
    const bool veth_info_data_is_not_a_vlan_id = gaps_parse_link( link, &out ) == ROCKETNET_GATEWAY_NETLINK_FOUND && out.vlan_id == 0;
    rocketnet_unit_check( veth_info_data_is_not_a_vlan_id );

    link.kind = "";
    const bool kindless_info_data_is_not_a_vlan_id = gaps_parse_link( link, &out ) == ROCKETNET_GATEWAY_NETLINK_FOUND && out.vlan_id == 0;
    rocketnet_unit_check( kindless_info_data_is_not_a_vlan_id );

    link.kind = "vlan";
    rocketnet_unit_check( gaps_parse_link( link, &out ) == ROCKETNET_GATEWAY_NETLINK_FOUND && out.vlan_id == 100 );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_link_vlan_data_only_for_vlan_kind );

// mutation gap: delta sdk/rocketnet_gateway_netlink_linux.cpp:725:22:off_by_one:1 (a one-letter lower kind passed)
//
// An 802.1Q VLAN on a lower device: none ("") and "veth" carry it; any other
// kind refuses, a one-letter one included.

static void test_gaps_delta_core_netlink_vlan_lower_kind_refuses_unless_none_or_veth()
{
    rocketnet_gateway_netlink_link_t device;
    memset( &device, 0, sizeof( device ) );
    device.index = gaps_server_interface;
    memcpy( device.kind, "vlan", sizeof( "vlan" ) );
    device.link_index = 2;
    device.vlan_id = 100;
    device.vlan_protocol = 0x8100;

    rocketnet_gateway_netlink_link_t lower;
    memset( &lower, 0, sizeof( lower ) );
    lower.index = 2;
    const char * reason = NULL;

    rocketnet_unit_check( rocketnet_xdp_link_classify( &device, &lower, &reason ) == ROCKETNET_XDP_LINK_VLAN );
    memcpy( lower.kind, "veth", sizeof( "veth" ) );
    rocketnet_unit_check( rocketnet_xdp_link_classify( &device, &lower, &reason ) == ROCKETNET_XDP_LINK_VLAN );

    memcpy( lower.kind, "x", sizeof( "x" ) );
    const bool one_letter_lower_kind_refused = rocketnet_xdp_link_classify( &device, &lower, &reason ) == ROCKETNET_XDP_LINK_REFUSED && reason != NULL;
    rocketnet_unit_check( one_letter_lower_kind_refused );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_netlink_vlan_lower_kind_refuses_unless_none_or_veth );

#endif // #if ROCKETNET_XDP
