/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_ROUTE_H
#define ROCKETNET_SDK_ROCKETNET_ROUTE_H
#include <stdint.h>
#include <string.h>

/**
 * @brief Stores one IPv4 forwarding route.
 *
 * IPv4 addresses use octet significance, independent of host byte order.
 * `forward_dest` and `forward_mask` use that same numeric representation;
 * `forward_metric` is the route priority, where a smaller value wins among
 * equally specific matching routes. `if_index` is the outgoing interface
 * index. The fields are plain values owned by the caller.
 */
struct rocketnet_ifforward4_t
{
    /** @brief Outgoing interface index. */
    uint32_t if_index;
    /** @brief Destination address in IPv4 octet significance. */
    uint32_t forward_dest;
    /** @brief Destination mask in IPv4 octet significance. */
    uint32_t forward_mask;
    /** @brief Route priority; smaller values win ties. */
    uint32_t forward_metric;
};

/**
 * @brief Converts four IPv4 octets to a host-order numeric address.
 *
 * The returned value is `(ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]`,
 * independent of host byte order. The caller supplies a non-NULL pointer to
 * at least four readable bytes; the function does not validate or retain it.
 * @param ip Input IPv4 address octets.
 * @return The address in IPv4 octet significance.
 */
static inline uint32_t rocketnet_route_ip( const uint8_t * ip )
{
    return ( uint32_t( ip[0] ) << 24 ) | ( uint32_t( ip[1] ) << 16 ) | ( uint32_t( ip[2] ) << 8 ) | uint32_t( ip[3] );
}

/**
 * @brief Reports whether route `a` is preferable to route `b` for an IPv4 address.
 *
 * A route matches when its destination masked by `forward_mask` equals `ip`
 * masked by the same value. A matching route beats a missing or nonmatching
 * route; when both match, the numerically larger mask wins, then the smaller
 * metric. This function does not check that masks are contiguous. It reads
 * the route values only for the call and does not modify them.
 * @param a Candidate route; NULL or a nonmatching route loses.
 * @param b Current best route; NULL or a nonmatching route loses to a matching `a`.
 * @param ip IPv4 address in octet significance.
 * @return True when `a` is preferred, otherwise false.
 */
static inline bool better_match( const rocketnet_ifforward4_t * a, const rocketnet_ifforward4_t * b, uint32_t ip )
{
    if ( !a || ( ip & a->forward_mask ) != ( a->forward_dest & a->forward_mask ) )
        return false;
    if ( !b || ( ip & b->forward_mask ) != ( b->forward_dest & b->forward_mask ) )
        return true;
    // The implementation compares masks numerically; contiguous prefixes sort by length.

    if ( a->forward_mask != b->forward_mask )
        return a->forward_mask > b->forward_mask;
    return a->forward_metric < b->forward_metric;
}

#if defined( __linux__ )
#include <sys/socket.h>
#include <linux/rtnetlink.h>

// The const-correct spellings of NLMSG_DATA, RTM_RTA, RTA_NEXT and RTA_DATA:
// the same offsets, without the macros' casts through char *, which drop the
// const of a borrowed message. RTA_NEXT's stride is RTA_ALIGN( rta_len ),
// at most 0xFFFF + 3, so it fits an int; RTA_OK has already checked rta_len
// against the bytes that remain before this is called.
static inline const uint8_t * rocketnet_route_attribute_data( const struct rtattr * attribute )
{
    return (const uint8_t *) attribute + RTA_LENGTH( 0 );
}

static inline const struct rtattr * rocketnet_route_attribute_next( const struct rtattr * attribute, int * remaining )
{
    const int stride = (int) RTA_ALIGN( attribute->rta_len );
    *remaining -= stride;
    return (const struct rtattr *) ( (const uint8_t *) attribute + stride );
}

/**
 * @brief Parses one supported main-table IPv4 unicast route message.
 *
 * Accepts an RTM_NEWROUTE message for AF_INET with a prefix length at most
 * 32, a nonzero output interface, a destination attribute when the prefix is
 * nonzero, and RT_TABLE_MAIN. A default route has an implicit zero destination.
 * Unknown attributes are ignored. Attribute lengths and message bounds are
 * checked; repeated recognized attributes use the last value. `out` is written
 * only on success. The input message and output object are borrowed for this
 * call, and both pointers must be non-NULL and valid.
 * @param msg_ptr Netlink route message to inspect.
 * @param out Receives the parsed route on success; unchanged on refusal.
 * @return True for a supported route, false for an unsupported or malformed message.
 */
static inline bool parse_kernel_route( const struct nlmsghdr * msg_ptr, rocketnet_ifforward4_t * out )
{
    if ( msg_ptr->nlmsg_len < NLMSG_LENGTH( sizeof( rtmsg ) ) || msg_ptr->nlmsg_type != RTM_NEWROUTE )
        return false;
    const struct rtmsg * route_entry = (const struct rtmsg *) ( (const uint8_t *) msg_ptr + NLMSG_HDRLEN );
    if ( route_entry->rtm_family != AF_INET || route_entry->rtm_dst_len > 32 || route_entry->rtm_type != RTN_UNICAST )
        return false;

    rocketnet_ifforward4_t result = {};
    const unsigned prefix = route_entry->rtm_dst_len;
    result.forward_mask = prefix == 0 ? 0 : ( 0xffffffffu << ( 32 - prefix ) );

    uint32_t table = route_entry->rtm_table;
    bool has_destination = prefix == 0;

    // RTM_PAYLOAD: the length check above keeps this at least zero, and a
    // length past INT_MAX goes negative, which RTA_OK refuses and the
    // remaining != 0 check below rejects.

    int remaining = (int) ( msg_ptr->nlmsg_len - NLMSG_SPACE( sizeof( rtmsg ) ) );
    for ( const struct rtattr * attribute = (const struct rtattr *) ( (const uint8_t *) route_entry + NLMSG_ALIGN( sizeof( rtmsg ) ) );
          RTA_OK( attribute, remaining ); attribute = rocketnet_route_attribute_next( attribute, &remaining ) )
    {
        switch ( attribute->rta_type )
        {
            case RTA_DST:
                if ( RTA_PAYLOAD( attribute ) != 4 )
                    return false;
                result.forward_dest = rocketnet_route_ip( rocketnet_route_attribute_data( attribute ) );
                has_destination = true;
                break;
            case RTA_OIF:
                if ( RTA_PAYLOAD( attribute ) != 4 )
                    return false;
                memcpy( &result.if_index, rocketnet_route_attribute_data( attribute ), 4 );
                break;
            case RTA_PRIORITY:
                // RTA_METRICS is a nested attribute set, not route priority.
                // https://www.kernel.org/doc/html/latest/netlink/specs/rt-route.html
                if ( RTA_PAYLOAD( attribute ) != 4 )
                    return false;
                memcpy( &result.forward_metric, rocketnet_route_attribute_data( attribute ), 4 );
                break;
            case RTA_TABLE:
                if ( RTA_PAYLOAD( attribute ) != 4 )
                    return false;
                memcpy( &table, rocketnet_route_attribute_data( attribute ), 4 );
                break;
        }
    }

    if ( remaining != 0 || table != RT_TABLE_MAIN || !result.if_index || !has_destination )
        return false;
    *out = result;

    return true;
}
#endif // #if defined( __linux__ )
#endif // ROCKETNET_SDK_ROCKETNET_ROUTE_H
