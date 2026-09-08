/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#ifndef ROCKETNET_ROUTE_H
#define ROCKETNET_ROUTE_H
#include <stdint.h>
#include <string.h>

// IPv4 values use octet significance, independent of host memory order.
struct rocketnet_ifforward4_t
{
    uint32_t if_index;
    uint32_t forward_dest;
    uint32_t forward_mask;
    uint32_t forward_metric;
};

static uint32_t rocketnet_route_ip( const uint8_t * ip )
{
    return (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) | (uint32_t(ip[2]) << 8) | uint32_t(ip[3]);
}

static bool better_match( const rocketnet_ifforward4_t * a, const rocketnet_ifforward4_t * b, uint32_t ip )
{
    if ( !a || (ip & a->forward_mask) != (a->forward_dest & a->forward_mask) )
        return false;
    if ( !b || (ip & b->forward_mask) != (b->forward_dest & b->forward_mask) )
        return true;
    // A longer contiguous most-significant-bit mask has a greater value.
    if ( a->forward_mask != b->forward_mask )
        return a->forward_mask > b->forward_mask;
    return a->forward_metric < b->forward_metric;
}

#if defined(__linux__)
#include <sys/socket.h>
#include <linux/rtnetlink.h>

static bool parse_kernel_route( const struct nlmsghdr * msg_ptr, rocketnet_ifforward4_t * out )
{
    if ( msg_ptr->nlmsg_len < NLMSG_LENGTH(sizeof(rtmsg)) || msg_ptr->nlmsg_type != RTM_NEWROUTE )
        return false;
    const struct rtmsg * route_entry = (const struct rtmsg*) NLMSG_DATA(msg_ptr);
    if ( route_entry->rtm_family != AF_INET || route_entry->rtm_dst_len > 32 || route_entry->rtm_type != RTN_UNICAST )
        return false;

    rocketnet_ifforward4_t result = {};
    const unsigned prefix = route_entry->rtm_dst_len;
    result.forward_mask = prefix == 0 ? 0 : (0xffffffffu << (32 - prefix));
    uint32_t table = route_entry->rtm_table;
    bool has_destination = prefix == 0;
    int remaining = RTM_PAYLOAD(msg_ptr);
    for ( const struct rtattr * attribute = (const struct rtattr*) RTM_RTA(route_entry);
          RTA_OK(attribute, remaining); attribute = RTA_NEXT(attribute, remaining) )
    {
        switch ( attribute->rta_type )
        {
            case RTA_DST:
                if ( RTA_PAYLOAD(attribute) != 4 ) return false;
                result.forward_dest = rocketnet_route_ip((const uint8_t*) RTA_DATA(attribute));
                has_destination = true;
                break;
            case RTA_OIF:
                if ( RTA_PAYLOAD(attribute) != 4 ) return false;
                memcpy(&result.if_index, RTA_DATA(attribute), 4);
                break;
            case RTA_PRIORITY:
                // RTA_METRICS is a nested attribute set, not route priority.
                // https://www.kernel.org/doc/html/latest/netlink/specs/rt-route.html
                if ( RTA_PAYLOAD(attribute) != 4 ) return false;
                memcpy(&result.forward_metric, RTA_DATA(attribute), 4);
                break;
            case RTA_TABLE:
                if ( RTA_PAYLOAD(attribute) != 4 ) return false;
                memcpy(&table, RTA_DATA(attribute), 4);
                break;
        }
    }
    if ( remaining != 0 || table != RT_TABLE_MAIN || !result.if_index || !has_destination )
        return false;
    *out = result;
    return true;
}
#endif
#endif
