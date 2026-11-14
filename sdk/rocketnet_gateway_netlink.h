/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_GATEWAY_NETLINK_H
#define ROCKETNET_SDK_ROCKETNET_GATEWAY_NETLINK_H

// the XDP transport's gateway discovery through rtnetlink, the Linux routing
// API (iproute2's ip(8) is a client of the same socket): no process is
// started and nothing is read from PATH. Two queries answer it, each on a
// fresh NETLINK_ROUTE socket:
//
//   RTM_GETROUTE  a dump of the family's routes: the default route
//                 (destination length 0, unicast, main table, not cloned)
//                 whose output interface is this interface: its RTA_GATEWAY
//                 is the gateway. A multipath default route (either
//                 family) answers with its first next hop on this
//                 interface. A default route that names only a nexthop
//                 object (RTA_NH_ID, as the kernel dumps it when
//                 net.ipv4.nexthop_compat_mode is 0) is answered after the
//                 dump by an RTM_GETNEXTHOP query for that id (ip nexthop
//                 get id N): an object on this interface with an
//                 NHA_GATEWAY is the route. A group, a blackhole, an object
//                 on another interface or without a gateway, or the
//                 kernel refusing the query, is no default route.
//   RTM_GETNEIGH  no dump: the one neighbour entry of the family on this
//                 interface whose NDA_DST is the gateway (ip neigh get), in
//                 a state ip(8) shows (reachable, stale, delay, probe or
//                 permanent) and carrying a 6-byte NDA_LLADDR: that is the
//                 gateway's MAC. Nothing resolves a missing entry (ENOENT).
//
// The periodic refresh makes the same two queries as create, every period,
// so it follows a change of the default route's gateway.
//
// The parse functions are pure: one received datagram of a dump in, an
// outcome out, no socket and no allocation (test_unit_xdp_gateway_netlink.cpp
// feeds them canned replies). The dump functions are the thin socket wrapper
// around them: a fixed-size receive buffer, SO_RCVTIMEO (the refresh's
// sockets are non-blocking instead), a bounded number of
// reads, and the reply's sequence number, port id and sender checked. A dump
// the kernel marks interrupted (NLM_F_DUMP_INTR: the table changed while it
// was dumped) is asked again on a fresh socket, up to
// ROCKETNET_GATEWAY_NETLINK_DUMP_ATTEMPTS dumps in all.

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

#define ROCKETNET_GATEWAY_NETLINK_MORE          0   // nothing found in this datagram; the dump continues
#define ROCKETNET_GATEWAY_NETLINK_FOUND         1   // the answer is written
#define ROCKETNET_GATEWAY_NETLINK_DONE          2   // NLMSG_DONE: the dump ended with no answer
#define ROCKETNET_GATEWAY_NETLINK_MALFORMED     3   // a message or attribute overruns its bounds, an answer has the wrong length, an error code is INT_MIN, or the datagram was truncated
#define ROCKETNET_GATEWAY_NETLINK_ERROR         4   // the kernel answered NLMSG_ERROR, or the dump was interrupted; *error holds the errno
#define ROCKETNET_GATEWAY_NETLINK_SYSTEM        5   // the socket failed, timed out, or the read bound ran out; *error holds the errno

// dumps asked for one query: the first, and a fresh one after each that
// answers interrupted (ERROR with EINTR)

#define ROCKETNET_GATEWAY_NETLINK_DUMP_ATTEMPTS 4

// one received datagram of an RTM_GETROUTE dump: FOUND writes the gateway
// (4 bytes for AF_INET, 16 for AF_INET6) of the first default route of family
// whose output interface is interface_index. Messages whose sequence or port
// id is not the request's are skipped.

int rocketnet_gateway_netlink_parse_route( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                           int family, int interface_index, uint8_t * gateway, int * error );

// one received datagram of an RTM_GETNEIGH reply: FOUND writes the 6-byte
// link-layer address of the first usable neighbour entry of family on
// interface_index whose destination is gateway.

int rocketnet_gateway_netlink_parse_neighbor( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                              int family, int interface_index, const uint8_t * gateway, uint8_t * mac, int * error );

// the queries themselves, over a NETLINK_ROUTE socket opened and closed
// inside the call: the outcome of the first datagram that is not MORE, or
// SYSTEM. The neighbour query's one reply with no usable entry, or the
// kernel's ENOENT, is DONE. A local address of the wrong size after bind is
// SYSTEM with EPROTO.

int rocketnet_gateway_netlink_route( int family, int interface_index, uint8_t * gateway, int * error );

int rocketnet_gateway_netlink_neighbor( int family, int interface_index, const uint8_t * gateway, uint8_t * mac, int * error );

// create-time discovery of one family's gateway MAC, and the gateway IP it
// records for the refresh: the route query writes gateway and sets
// *gateway_known, then the neighbour query asks that gateway's entry. FOUND
// writes mac. DONE with *gateway_known false means no default route; DONE
// with *gateway_known true means the route was found but its neighbour entry
// was not usable. Any other outcome is returned and keeps gateway.

int rocketnet_gateway_netlink_discover( int family, int interface_index, uint8_t * gateway, bool * gateway_known, uint8_t * mac, int * error );

// the periodic refresh of one family's gateway MAC: discover's route query
// and neighbour query, every period, with discover's outcomes, so a change
// of the default route's gateway IP is followed at the next refresh.
// The refresh runs inside the update, so its sockets are non-blocking: a
// reply not yet queued when it is read fails that query SYSTEM with EAGAIN,
// where a blocking read would have waited up to its timeout, and the refresh
// goes on as for any failure. Non-blocking covers the receive only. The
// kernel handles each request inside sendto, building the first part of the
// dump there, and a kernel that takes the rtnl lock for these dumps makes
// sendto wait while another task holds it: under rtnl contention a refresh
// can still hold up the update (roadmap: xdp-gateway-refresh-main-thread).

int rocketnet_gateway_netlink_refresh( int family, int interface_index, uint8_t * gateway, bool * gateway_known, uint8_t * mac, int * error );

// the socket calls a dump makes. The functions above use the C library's;
// the _with forms take a table, so a unit test drives a whole dump over a
// fake kernel with no socket.

struct rocketnet_gateway_netlink_system_t
{
    int ( *socket )( int domain, int type, int protocol );
    int ( *setsockopt )( int fd, int level, int name, const void * value, socklen_t value_bytes );
    int ( *bind )( int fd, const struct sockaddr * address, socklen_t address_bytes );
    int ( *getsockname )( int fd, struct sockaddr * address, socklen_t * address_bytes );
    ssize_t ( *sendto )( int fd, const void * data, size_t bytes, int flags, const struct sockaddr * address, socklen_t address_bytes );
    ssize_t ( *recvmsg )( int fd, struct msghdr * message, int flags );
    int ( *close )( int fd );
    int socket_flags;   // added to each socket's type: SOCK_NONBLOCK for the refresh
};

int rocketnet_gateway_netlink_route_with( const struct rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                          uint8_t * gateway, int * error );

int rocketnet_gateway_netlink_neighbor_with( const struct rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                             const uint8_t * gateway, uint8_t * mac, int * error );

int rocketnet_gateway_netlink_discover_with( const struct rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                             uint8_t * gateway, bool * gateway_known, uint8_t * mac, int * error );

int rocketnet_gateway_netlink_refresh_with( const struct rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                            uint8_t * gateway, bool * gateway_known, uint8_t * mac, int * error );

// the outcome's name for a log line

const char * rocketnet_gateway_netlink_result_name( int result );

// ---------------------------------------------------------------------------
// THE LINK: what the interface that owns the server's address is, from the
// one RTM_NEWLINK reply to an RTM_GETLINK for its index (ip -d link show):
// its name, its link kind (IFLA_INFO_KIND: "" for a device with none, a
// physical NIC), the device it is stacked on (IFLA_LINK) and, for a VLAN
// device, its VLAN ID and tag protocol. The XDP transport reads it at
// pre-flight and create (rocketnet_xdp_link_classify) for the device it
// attaches to.

#define ROCKETNET_GATEWAY_NETLINK_NAME_BYTES    16  // IFNAMSIZ

struct rocketnet_gateway_netlink_link_t
{
    int index;
    char name[ROCKETNET_GATEWAY_NETLINK_NAME_BYTES];    // IFLA_IFNAME
    char kind[ROCKETNET_GATEWAY_NETLINK_NAME_BYTES];    // IFLA_INFO_KIND; "" when there is none
    int link_index;                                     // IFLA_LINK; 0 when absent or the device itself
    bool link_netns;                                    // IFLA_LINK_NETNSID present: the link is in another namespace
    int vlan_id;                                        // IFLA_VLAN_ID of kind "vlan"; 0 otherwise
    int vlan_protocol;                                  // IFLA_VLAN_PROTOCOL of kind "vlan", host order; 0 otherwise
};

// one received datagram of the reply: FOUND fills link from the RTM_NEWLINK
// of interface_index; a name or kind past its bytes, or a VLAN attribute of
// the wrong size, is MALFORMED

int rocketnet_gateway_netlink_parse_link( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                          int interface_index, struct rocketnet_gateway_netlink_link_t * link, int * error );

// the query, over a fresh NETLINK_ROUTE socket as above: FOUND, or the
// failure; a reply with no message for the index is DONE

int rocketnet_gateway_netlink_link( int interface_index, struct rocketnet_gateway_netlink_link_t * link, int * error );

int rocketnet_gateway_netlink_link_with( const struct rocketnet_gateway_netlink_system_t * system, int interface_index,
                                         struct rocketnet_gateway_netlink_link_t * link, int * error );

// What the XDP transport does with the device that owns the address, given
// that device and, when it names one (link_index), the device it is stacked
// on (lower; NULL when it names none):
//
//   PLAIN    no stacking the transport knows: XDP on the device itself, as
//            before VLAN support (a physical NIC, veth, virtio and the rest)
//   VLAN     one 802.1Q VLAN device on a device with no link kind (a
//            physical NIC) or on veth: XDP on lower, frames tagged with
//            vlan_id
//   REFUSED  *reason names what was found: an 802.1ad VLAN, a VLAN on a
//            VLAN (QinQ) or on any other stacked kind, a VLAN with VLAN ID 0
//            or no lower device in this namespace, or the address directly
//            on a bond, bridge, team, macvlan, macvtap, ipvlan or ipvtap

#define ROCKETNET_XDP_LINK_PLAIN    0
#define ROCKETNET_XDP_LINK_VLAN     1
#define ROCKETNET_XDP_LINK_REFUSED  2

int rocketnet_xdp_link_classify( const struct rocketnet_gateway_netlink_link_t * device, const struct rocketnet_gateway_netlink_link_t * lower, const char ** reason );

#endif // ROCKETNET_SDK_ROCKETNET_GATEWAY_NETLINK_H
