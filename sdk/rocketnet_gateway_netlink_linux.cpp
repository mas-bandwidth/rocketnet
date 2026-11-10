/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"

#if ROCKETNET_XDP

#include "rocketnet_gateway_netlink.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>
#include <netinet/in.h>

// every header and attribute is copied out of the byte buffer with memcpy
// before it is read, so nothing depends on the buffer's alignment. The walks
// keep the NLMSG_OK and RTA_OK rules with size_t arithmetic: a header must
// fit in the bytes left, its length must cover at least the header, and its
// length must not run past the bytes left. Anything else is MALFORMED.

// the fields of one attribute walk the parsers read: the first occurrence of
// each type below ROCKETNET_GATEWAY_NETLINK_MAX_ATTRIBUTE wins

#define ROCKETNET_GATEWAY_NETLINK_MAX_ATTRIBUTE 32

struct rocketnet_gateway_netlink_attributes_t
{
    const uint8_t * value[ROCKETNET_GATEWAY_NETLINK_MAX_ATTRIBUTE];
    size_t bytes[ROCKETNET_GATEWAY_NETLINK_MAX_ATTRIBUTE];
};

static bool rocketnet_gateway_netlink_walk_attributes( const uint8_t * data, size_t bytes, rocketnet_gateway_netlink_attributes_t * attributes )
{
    memset( attributes, 0, sizeof( *attributes ) );

    size_t offset = 0;
    while ( offset < bytes )
    {
        struct rtattr attribute;
        if ( bytes - offset < sizeof( attribute ) )
            return false;

        memcpy( &attribute, data + offset, sizeof( attribute ) );
        if ( attribute.rta_len < sizeof( attribute ) || attribute.rta_len > bytes - offset )
            return false;

        const unsigned int type = (unsigned int) ( attribute.rta_type & NLA_TYPE_MASK );
        if ( type < ROCKETNET_GATEWAY_NETLINK_MAX_ATTRIBUTE && attributes->value[type] == NULL )
        {
            attributes->value[type] = data + offset + RTA_LENGTH( 0 );
            attributes->bytes[type] = attribute.rta_len - RTA_LENGTH( 0 );
        }

        offset += RTA_ALIGN( attribute.rta_len );
    }

    return true;
}

static bool rocketnet_gateway_netlink_u32( const rocketnet_gateway_netlink_attributes_t * attributes, unsigned int type, uint32_t * value )
{
    if ( attributes->value[type] == NULL || attributes->bytes[type] != sizeof( uint32_t ) )
        return false;

    memcpy( value, attributes->value[type], sizeof( uint32_t ) );

    return true;
}

static size_t rocketnet_gateway_netlink_address_bytes( int family )
{
    return family == AF_INET ? 4 : 16;
}

// one message of the wanted type, its fixed body (rtmsg or ndmsg) and its
// attributes: MORE when it is not the answer, FOUND when it is, MALFORMED

typedef int ( *rocketnet_gateway_netlink_match_t )( const uint8_t * body, size_t body_bytes, const rocketnet_gateway_netlink_attributes_t * attributes, void * context );

static int rocketnet_gateway_netlink_parse( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                            uint16_t message_type, size_t body_bytes,
                                            rocketnet_gateway_netlink_match_t match, void * context, int * error )
{
    // the walk of one received datagram, shared by both dumps

    rocketnet_assert( data || bytes == 0 );
    rocketnet_assert( error );

    *error = 0;
    size_t offset = 0;
    while ( offset < bytes )
    {
        struct nlmsghdr header;
        if ( bytes - offset < sizeof( header ) )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        memcpy( &header, data + offset, sizeof( header ) );
        if ( header.nlmsg_len < sizeof( header ) || header.nlmsg_len > bytes - offset )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        const uint8_t * payload = data + offset + NLMSG_HDRLEN;
        const size_t payload_bytes = header.nlmsg_len > NLMSG_HDRLEN ? header.nlmsg_len - NLMSG_HDRLEN : 0;

        // a message that is not the reply to this request is skipped

        if ( header.nlmsg_seq == sequence && header.nlmsg_pid == port_id )
        {
            if ( header.nlmsg_flags & NLM_F_DUMP_INTR )
            {
                // the table changed while the kernel dumped it

                *error = EINTR;
                return ROCKETNET_GATEWAY_NETLINK_ERROR;
            }

            if ( header.nlmsg_type == NLMSG_ERROR )
            {
                int code;
                if ( payload_bytes < sizeof( code ) )
                    return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

                memcpy( &code, payload, sizeof( code ) );
                if ( code != 0 )
                {
                    *error = code < 0 ? -code : code;
                    return ROCKETNET_GATEWAY_NETLINK_ERROR;
                }

                // an acknowledgement carries no answer
            }
            else if ( header.nlmsg_type == NLMSG_DONE )
            {
                // a dump's NLMSG_DONE carries the dump's result: negative is an errno

                if ( payload_bytes >= sizeof( int ) )
                {
                    int code;
                    memcpy( &code, payload, sizeof( code ) );
                    if ( code < 0 )
                    {
                        *error = -code;
                        return ROCKETNET_GATEWAY_NETLINK_ERROR;
                    }
                }

                return ROCKETNET_GATEWAY_NETLINK_DONE;
            }
            else if ( header.nlmsg_type == message_type )
            {
                if ( payload_bytes < NLMSG_ALIGN( body_bytes ) )
                    return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

                rocketnet_gateway_netlink_attributes_t attributes;
                if ( !rocketnet_gateway_netlink_walk_attributes( payload + NLMSG_ALIGN( body_bytes ), payload_bytes - NLMSG_ALIGN( body_bytes ), &attributes ) )
                    return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

                const int result = match( payload, body_bytes, &attributes, context );
                if ( result != ROCKETNET_GATEWAY_NETLINK_MORE )
                    return result;
            }
        }

        offset += NLMSG_ALIGN( header.nlmsg_len );
    }

    return ROCKETNET_GATEWAY_NETLINK_MORE;
}

// ---------------------------------------------------------------------------
// the default route

struct rocketnet_gateway_netlink_route_context_t
{
    int family;
    int interface_index;
    uint8_t * gateway;
};

static int rocketnet_gateway_netlink_multipath_on_interface( const uint8_t * data, size_t bytes, int family, int want_interface,
                                                             int * interface_index, const uint8_t ** gateway, size_t * gateway_bytes )
{
    // every next hop of a multipath default route (RTA_MULTIPATH: rtnexthop
    // records, each followed by its own attributes), for either family. The
    // first hop whose interface is this one and whose RTA_GATEWAY is the
    // family's address size is the route. A short or oversized record is
    // malformed. No hop on this interface is not this interface's route.

    const size_t address_bytes = rocketnet_gateway_netlink_address_bytes( family );

    size_t offset = 0;
    while ( offset < bytes )
    {
        if ( bytes - offset < sizeof( struct rtnexthop ) )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        struct rtnexthop next_hop;
        memcpy( &next_hop, data + offset, sizeof( next_hop ) );
        const size_t raw = (size_t) next_hop.rtnh_len;
        if ( raw < sizeof( next_hop ) || raw > bytes - offset )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        // the record's attributes start RTNH_ALIGN( sizeof(next_hop) ) in, which
        // is sizeof(next_hop): the record is already a multiple of RTNH_ALIGNTO

        static_assert( sizeof( struct rtnexthop ) % RTNH_ALIGNTO == 0, "an rtnexthop record ends aligned" );

        rocketnet_gateway_netlink_attributes_t attributes;
        if ( !rocketnet_gateway_netlink_walk_attributes( data + offset + sizeof( next_hop ), raw - sizeof( next_hop ), &attributes ) )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        if ( next_hop.rtnh_ifindex == want_interface && attributes.value[RTA_GATEWAY] != NULL && attributes.bytes[RTA_GATEWAY] == address_bytes )
        {
            *interface_index = next_hop.rtnh_ifindex;
            *gateway = attributes.value[RTA_GATEWAY];
            *gateway_bytes = attributes.bytes[RTA_GATEWAY];
            return ROCKETNET_GATEWAY_NETLINK_FOUND;
        }

        const size_t step = ( raw + (size_t) RTNH_ALIGNTO - 1 ) & ~(size_t) ( RTNH_ALIGNTO - 1 );
        if ( step < raw || step > bytes - offset )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        offset += step;
    }

    return ROCKETNET_GATEWAY_NETLINK_MORE;
}

static int rocketnet_gateway_netlink_match_route( const uint8_t * body, size_t body_bytes, const rocketnet_gateway_netlink_attributes_t * attributes, void * context )
{
    rocketnet_gateway_netlink_route_context_t * route = (rocketnet_gateway_netlink_route_context_t *) context;

    struct rtmsg message;
    rocketnet_assert( body_bytes == sizeof( message ) );
    (void) body_bytes;
    memcpy( &message, body, sizeof( message ) );

    if ( message.rtm_family != route->family || message.rtm_dst_len != 0 || message.rtm_type != RTN_UNICAST || ( message.rtm_flags & RTM_F_CLONED ) )
        return ROCKETNET_GATEWAY_NETLINK_MORE;

    uint32_t table = message.rtm_table;
    if ( attributes->value[RTA_TABLE] && !rocketnet_gateway_netlink_u32( attributes, RTA_TABLE, &table ) )
        return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

    if ( table != RT_TABLE_MAIN )
        return ROCKETNET_GATEWAY_NETLINK_MORE;

    int interface_index = 0;
    const uint8_t * gateway = attributes->value[RTA_GATEWAY];
    size_t gateway_bytes = attributes->bytes[RTA_GATEWAY];

    uint32_t output_interface = 0;
    if ( attributes->value[RTA_OIF] )
    {
        if ( !rocketnet_gateway_netlink_u32( attributes, RTA_OIF, &output_interface ) )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        interface_index = (int) output_interface;
    }
    else if ( attributes->value[RTA_MULTIPATH] )
    {
        // both families: the first next hop on this interface. Hops on
        // another interface are not this route, so the walk keeps going.

        const int hop = rocketnet_gateway_netlink_multipath_on_interface(
            attributes->value[RTA_MULTIPATH], attributes->bytes[RTA_MULTIPATH],
            route->family, route->interface_index, &interface_index, &gateway, &gateway_bytes );
        if ( hop == ROCKETNET_GATEWAY_NETLINK_MALFORMED )
            return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

        if ( hop != ROCKETNET_GATEWAY_NETLINK_FOUND )
            return ROCKETNET_GATEWAY_NETLINK_MORE;
    }

    if ( interface_index != route->interface_index || gateway == NULL )
        return ROCKETNET_GATEWAY_NETLINK_MORE;

    if ( gateway_bytes != rocketnet_gateway_netlink_address_bytes( route->family ) )
        return ROCKETNET_GATEWAY_NETLINK_MALFORMED;

    memcpy( route->gateway, gateway, gateway_bytes );

    return ROCKETNET_GATEWAY_NETLINK_FOUND;
}

int rocketnet_gateway_netlink_parse_route( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                           int family, int interface_index, uint8_t * gateway, int * error )
{
    rocketnet_assert( family == AF_INET || family == AF_INET6 );
    rocketnet_assert( interface_index > 0 );
    rocketnet_assert( gateway );

    rocketnet_gateway_netlink_route_context_t context = { family, interface_index, gateway };
    return rocketnet_gateway_netlink_parse( data, bytes, sequence, port_id, RTM_NEWROUTE, sizeof( struct rtmsg ), rocketnet_gateway_netlink_match_route, &context, error );
}

// ---------------------------------------------------------------------------
// the gateway's neighbour entry

// the states ip(8) neigh show lists (0xFF & ~NUD_NOARP), less the two that
// carry no link-layer address (NUD_INCOMPLETE, NUD_FAILED)

#define ROCKETNET_GATEWAY_NETLINK_USABLE_STATES ( NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE | NUD_PERMANENT )

struct rocketnet_gateway_netlink_neighbor_context_t
{
    int family;
    int interface_index;
    const uint8_t * gateway;
    uint8_t * mac;
};

static int rocketnet_gateway_netlink_match_neighbor( const uint8_t * body, size_t body_bytes, const rocketnet_gateway_netlink_attributes_t * attributes, void * context )
{
    rocketnet_gateway_netlink_neighbor_context_t * neighbor = (rocketnet_gateway_netlink_neighbor_context_t *) context;

    struct ndmsg message;
    rocketnet_assert( body_bytes == sizeof( message ) );
    (void) body_bytes;
    memcpy( &message, body, sizeof( message ) );

    if ( message.ndm_family != neighbor->family || message.ndm_ifindex != neighbor->interface_index )
        return ROCKETNET_GATEWAY_NETLINK_MORE;

    const size_t address_bytes = rocketnet_gateway_netlink_address_bytes( neighbor->family );
    if ( attributes->value[NDA_DST] == NULL || attributes->bytes[NDA_DST] != address_bytes ||
         memcmp( attributes->value[NDA_DST], neighbor->gateway, address_bytes ) != 0 )
        return ROCKETNET_GATEWAY_NETLINK_MORE;

    // an entry for the gateway that ip(8) would not have listed with a
    // 17-character lladdr is passed over, and the walk goes on

    if ( ( message.ndm_state & ROCKETNET_GATEWAY_NETLINK_USABLE_STATES ) == 0 )
        return ROCKETNET_GATEWAY_NETLINK_MORE;

    if ( attributes->value[NDA_LLADDR] == NULL || attributes->bytes[NDA_LLADDR] != 6 )
        return ROCKETNET_GATEWAY_NETLINK_MORE;

    memcpy( neighbor->mac, attributes->value[NDA_LLADDR], 6 );

    return ROCKETNET_GATEWAY_NETLINK_FOUND;
}

int rocketnet_gateway_netlink_parse_neighbor( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                              int family, int interface_index, const uint8_t * gateway, uint8_t * mac, int * error )
{
    rocketnet_assert( family == AF_INET || family == AF_INET6 );
    rocketnet_assert( interface_index > 0 );
    rocketnet_assert( gateway );
    rocketnet_assert( mac );

    rocketnet_gateway_netlink_neighbor_context_t context = { family, interface_index, gateway, mac };
    return rocketnet_gateway_netlink_parse( data, bytes, sequence, port_id, RTM_NEWNEIGH, sizeof( struct ndmsg ), rocketnet_gateway_netlink_match_neighbor, &context, error );
}

// ---------------------------------------------------------------------------
// the socket

// one datagram of a dump fits: the kernel sizes a dump's datagrams to the
// largest buffer recvmsg has offered, capped at 32 KiB
// (net/netlink/af_netlink.c, netlink_dump); a larger datagram arrives
// MSG_TRUNC and refuses

#define ROCKETNET_GATEWAY_NETLINK_BUFFER_BYTES 32768

// a dump of a routing table this many datagrams long refuses rather than
// keeps reading

#define ROCKETNET_GATEWAY_NETLINK_MAX_READS 4096

// each read waits at most this long for the kernel's next datagram

#define ROCKETNET_GATEWAY_NETLINK_TIMEOUT_SECONDS 1

// the request's sequence number; the socket is new for each dump, so the
// port id the kernel assigns at bind is what tells its replies apart

#define ROCKETNET_GATEWAY_NETLINK_SEQUENCE 1

typedef int ( *rocketnet_gateway_netlink_parse_t )( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id, void * context, int * error );

// one query on a fresh socket: RTM_GETROUTE is a dump of the family's
// routes; RTM_GETNEIGH asks for the one neighbour entry of neighbor on
// interface_index (no NLM_F_DUMP), whose single reply is the answer
static int rocketnet_gateway_netlink_dump_once( const rocketnet_gateway_netlink_system_t * system, uint16_t request_type, int family,
                                                int interface_index, const uint8_t * neighbor,
                                                rocketnet_gateway_netlink_parse_t parse, void * context, int * error )
{
    *error = 0;

    const int fd = system->socket( AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE );
    if ( fd < 0 )
    {
        *error = errno;
        return ROCKETNET_GATEWAY_NETLINK_SYSTEM;
    }

    struct timeval timeout;
    memset( &timeout, 0, sizeof( timeout ) );
    timeout.tv_sec = ROCKETNET_GATEWAY_NETLINK_TIMEOUT_SECONDS;

    struct sockaddr_nl local;
    memset( &local, 0, sizeof( local ) );
    local.nl_family = AF_NETLINK;
    socklen_t local_bytes = sizeof( local );

    if ( system->setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof( timeout ) ) != 0 ||
         system->setsockopt( fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof( timeout ) ) != 0 ||
         system->bind( fd, (struct sockaddr *) &local, sizeof( local ) ) != 0 ||
         system->getsockname( fd, (struct sockaddr *) &local, &local_bytes ) != 0 )
    {
        *error = errno;
        system->close( fd );
        return ROCKETNET_GATEWAY_NETLINK_SYSTEM;
    }

    // every call succeeded but the address is not a sockaddr_nl: no errno
    // describes it, so the error is EPROTO

    if ( local_bytes != sizeof( local ) )
    {
        *error = EPROTO;
        system->close( fd );
        return ROCKETNET_GATEWAY_NETLINK_SYSTEM;
    }

    // the request: a header and the family's rtmsg or ndmsg, every other
    // field zero; the route dump is filtered by the parse, the neighbour
    // query names its interface and its NDA_DST

    uint8_t request[NLMSG_SPACE( sizeof( struct rtmsg ) > sizeof( struct ndmsg ) ? sizeof( struct rtmsg ) : sizeof( struct ndmsg ) ) + RTA_SPACE( 16 )];
    memset( request, 0, sizeof( request ) );

    const bool dump = request_type == RTM_GETROUTE;
    const size_t body_bytes = dump ? sizeof( struct rtmsg ) : sizeof( struct ndmsg );
    const size_t address_bytes = rocketnet_gateway_netlink_address_bytes( family );

    struct nlmsghdr header;
    memset( &header, 0, sizeof( header ) );
    header.nlmsg_len = (uint32_t) ( dump ? NLMSG_LENGTH( body_bytes ) : NLMSG_SPACE( body_bytes ) + RTA_LENGTH( address_bytes ) );
    header.nlmsg_type = request_type;
    header.nlmsg_flags = (uint16_t) ( dump ? NLM_F_REQUEST | NLM_F_DUMP : NLM_F_REQUEST );
    header.nlmsg_seq = ROCKETNET_GATEWAY_NETLINK_SEQUENCE;
    memcpy( request, &header, sizeof( header ) );

    if ( dump )
    {
        struct rtmsg message;
        memset( &message, 0, sizeof( message ) );
        message.rtm_family = (unsigned char) family;
        memcpy( request + NLMSG_HDRLEN, &message, sizeof( message ) );
    }
    else
    {
        struct ndmsg message;
        memset( &message, 0, sizeof( message ) );
        message.ndm_family = (unsigned char) family;
        message.ndm_ifindex = interface_index;
        memcpy( request + NLMSG_HDRLEN, &message, sizeof( message ) );

        struct rtattr attribute;
        attribute.rta_len = (unsigned short) RTA_LENGTH( address_bytes );
        attribute.rta_type = NDA_DST;
        memcpy( request + NLMSG_SPACE( body_bytes ), &attribute, sizeof( attribute ) );
        memcpy( request + NLMSG_SPACE( body_bytes ) + RTA_LENGTH( 0 ), neighbor, address_bytes );
    }

    struct sockaddr_nl kernel;
    memset( &kernel, 0, sizeof( kernel ) );
    kernel.nl_family = AF_NETLINK;

    if ( system->sendto( fd, request, header.nlmsg_len, 0, (struct sockaddr *) &kernel, sizeof( kernel ) ) != (ssize_t) header.nlmsg_len )
    {
        *error = errno;
        system->close( fd );
        return ROCKETNET_GATEWAY_NETLINK_SYSTEM;
    }

    alignas( 8 ) uint8_t buffer[ROCKETNET_GATEWAY_NETLINK_BUFFER_BYTES];

    // the read bound running out is SYSTEM with E2BIG

    int result = ROCKETNET_GATEWAY_NETLINK_SYSTEM;
    *error = E2BIG;

    for ( int read = 0; read < ROCKETNET_GATEWAY_NETLINK_MAX_READS; read++ )
    {
        struct sockaddr_nl from;
        memset( &from, 0, sizeof( from ) );

        struct iovec vector;
        vector.iov_base = buffer;
        vector.iov_len = sizeof( buffer );

        struct msghdr message;
        memset( &message, 0, sizeof( message ) );
        message.msg_name = &from;
        message.msg_namelen = sizeof( from );
        message.msg_iov = &vector;
        message.msg_iovlen = 1;

        const ssize_t received = system->recvmsg( fd, &message, 0 );
        if ( received < 0 )
        {
            if ( errno == EINTR )
                continue;

            *error = errno;
            result = ROCKETNET_GATEWAY_NETLINK_SYSTEM;
            break;
        }

        if ( message.msg_flags & MSG_TRUNC )
        {
            *error = EMSGSIZE;
            result = ROCKETNET_GATEWAY_NETLINK_MALFORMED;
            break;
        }

        // only the kernel (port id 0) answers a dump

        if ( message.msg_namelen != sizeof( from ) || from.nl_pid != 0 )
            continue;

        result = parse( buffer, (size_t) received, ROCKETNET_GATEWAY_NETLINK_SEQUENCE, local.nl_pid, context, error );
        if ( result != ROCKETNET_GATEWAY_NETLINK_MORE )
            break;

        // the neighbour query's one reply held no usable entry: nothing more comes

        if ( !dump )
        {
            result = ROCKETNET_GATEWAY_NETLINK_DONE;
            break;
        }

        result = ROCKETNET_GATEWAY_NETLINK_SYSTEM;
        *error = E2BIG;
    }

    system->close( fd );

    return result;
}

// the query, asked again on a fresh socket while it answers interrupted
static int rocketnet_gateway_netlink_dump( const rocketnet_gateway_netlink_system_t * system, uint16_t request_type, int family,
                                           int interface_index, const uint8_t * neighbor,
                                           rocketnet_gateway_netlink_parse_t parse, void * context, int * error )
{
    rocketnet_assert( system );

    int result = ROCKETNET_GATEWAY_NETLINK_SYSTEM;
    for ( int attempt = 0; attempt < ROCKETNET_GATEWAY_NETLINK_DUMP_ATTEMPTS; attempt++ )
    {
        result = rocketnet_gateway_netlink_dump_once( system, request_type, family, interface_index, neighbor, parse, context, error );
        if ( result != ROCKETNET_GATEWAY_NETLINK_ERROR || *error != EINTR )
            break;
    }

    return result;
}

static int rocketnet_gateway_netlink_libc_close( int fd )
{
    return close( fd );
}

static const rocketnet_gateway_netlink_system_t rocketnet_gateway_netlink_libc = {
    socket, setsockopt, bind, getsockname, sendto, recvmsg, rocketnet_gateway_netlink_libc_close
};

static int rocketnet_gateway_netlink_route_parse( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id, void * context, int * error )
{
    const rocketnet_gateway_netlink_route_context_t * query = (const rocketnet_gateway_netlink_route_context_t *) context;
    return rocketnet_gateway_netlink_parse_route( data, bytes, sequence, port_id, query->family, query->interface_index, query->gateway, error );
}

int rocketnet_gateway_netlink_route_with( const rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                          uint8_t * gateway, int * error )
{
    rocketnet_assert( error );
    rocketnet_gateway_netlink_route_context_t query = { family, interface_index, gateway };
    return rocketnet_gateway_netlink_dump( system, RTM_GETROUTE, family, interface_index, NULL, rocketnet_gateway_netlink_route_parse, &query, error );
}

int rocketnet_gateway_netlink_route( int family, int interface_index, uint8_t * gateway, int * error )
{
    return rocketnet_gateway_netlink_route_with( &rocketnet_gateway_netlink_libc, family, interface_index, gateway, error );
}

struct rocketnet_gateway_netlink_neighbor_query_t
{
    int family;
    int interface_index;
    const uint8_t * gateway;
    uint8_t * mac;
};

static int rocketnet_gateway_netlink_neighbor_parse( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id, void * context, int * error )
{
    const rocketnet_gateway_netlink_neighbor_query_t * query = (const rocketnet_gateway_netlink_neighbor_query_t *) context;
    return rocketnet_gateway_netlink_parse_neighbor( data, bytes, sequence, port_id, query->family, query->interface_index, query->gateway, query->mac, error );
}

int rocketnet_gateway_netlink_neighbor_with( const rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                             const uint8_t * gateway, uint8_t * mac, int * error )
{
    rocketnet_assert( error );
    rocketnet_gateway_netlink_neighbor_query_t query = { family, interface_index, gateway, mac };
    const int result = rocketnet_gateway_netlink_dump( system, RTM_GETNEIGH, family, interface_index, gateway, rocketnet_gateway_netlink_neighbor_parse, &query, error );

    // the kernel answers ENOENT when it has no entry for the gateway

    if ( result == ROCKETNET_GATEWAY_NETLINK_ERROR && *error == ENOENT )
    {
        *error = 0;
        return ROCKETNET_GATEWAY_NETLINK_DONE;
    }

    return result;
}

int rocketnet_gateway_netlink_neighbor( int family, int interface_index, const uint8_t * gateway, uint8_t * mac, int * error )
{
    return rocketnet_gateway_netlink_neighbor_with( &rocketnet_gateway_netlink_libc, family, interface_index, gateway, mac, error );
}

int rocketnet_gateway_netlink_refresh_with( const rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                            uint8_t * gateway, bool * gateway_known, uint8_t * mac, int * error )
{
    rocketnet_assert( gateway_known );

    if ( *gateway_known && rocketnet_gateway_netlink_neighbor_with( system, family, interface_index, gateway, mac, error ) == ROCKETNET_GATEWAY_NETLINK_FOUND )
        return ROCKETNET_GATEWAY_NETLINK_FOUND;

    const int route = rocketnet_gateway_netlink_route_with( system, family, interface_index, gateway, error );
    if ( route == ROCKETNET_GATEWAY_NETLINK_DONE )
        *gateway_known = false;
    if ( route != ROCKETNET_GATEWAY_NETLINK_FOUND )
        return route;

    *gateway_known = true;

    return rocketnet_gateway_netlink_neighbor_with( system, family, interface_index, gateway, mac, error );
}

int rocketnet_gateway_netlink_refresh( int family, int interface_index, uint8_t * gateway, bool * gateway_known, uint8_t * mac, int * error )
{
    return rocketnet_gateway_netlink_refresh_with( &rocketnet_gateway_netlink_libc, family, interface_index, gateway, gateway_known, mac, error );
}

const char * rocketnet_gateway_netlink_result_name( int result )
{
    switch ( result )
    {
        case ROCKETNET_GATEWAY_NETLINK_MORE:      return "no answer";
        case ROCKETNET_GATEWAY_NETLINK_FOUND:     return "found";
        case ROCKETNET_GATEWAY_NETLINK_DONE:      return "not found";
        case ROCKETNET_GATEWAY_NETLINK_MALFORMED: return "malformed reply";
        case ROCKETNET_GATEWAY_NETLINK_ERROR:     return "kernel error";
        case ROCKETNET_GATEWAY_NETLINK_SYSTEM:    return "socket error";
        default:                                  return "unknown";
    }
}

#else // #if ROCKETNET_XDP

int rocketnet_gateway_netlink_linux_cpp_dummy = 0;

#endif // #if ROCKETNET_XDP
