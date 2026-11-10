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
//                 is the gateway. An IPv4 multipath default route answers
//                 with its first next hop, as /proc/net/route (netstat -rn)
//                 shows it.
//   RTM_GETNEIGH  no dump: the one neighbour entry of the family on this
//                 interface whose NDA_DST is the gateway (ip neigh get), in
//                 a state ip(8) shows (reachable, stale, delay, probe or
//                 permanent) and carrying a 6-byte NDA_LLADDR: that is the
//                 gateway's MAC. Nothing resolves a missing entry (ENOENT).
//
// The periodic refresh runs on the main update thread, so it never dumps
// while it knows the gateway: it asks that gateway's neighbour entry
// directly, and dumps the routes again only when no gateway is known or
// the direct query does not find the entry.
//
// The parse functions are pure: one received datagram of a dump in, an
// outcome out, no socket and no allocation (test_unit_xdp_gateway_netlink.cpp
// feeds them canned replies). The dump functions are the thin socket wrapper
// around them: a fixed-size receive buffer, SO_RCVTIMEO, a bounded number of
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
#define ROCKETNET_GATEWAY_NETLINK_MALFORMED     3   // a message or attribute overruns its bounds, an answer has the wrong length, or the datagram was truncated
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

// the periodic refresh of one family's gateway MAC. With *gateway_known, the
// neighbour entry of gateway is asked directly, and FOUND is the answer.
// Otherwise, or when that query does not answer FOUND, the route dump runs:
// its DONE (no default route) is DONE with *gateway_known false, any other
// failure is its outcome and keeps gateway; a route found writes gateway,
// sets *gateway_known, and the neighbour query's outcome is the answer.

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
};

int rocketnet_gateway_netlink_route_with( const struct rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                          uint8_t * gateway, int * error );

int rocketnet_gateway_netlink_neighbor_with( const struct rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                             const uint8_t * gateway, uint8_t * mac, int * error );

int rocketnet_gateway_netlink_refresh_with( const struct rocketnet_gateway_netlink_system_t * system, int family, int interface_index,
                                            uint8_t * gateway, bool * gateway_known, uint8_t * mac, int * error );

// the outcome's name for a log line

const char * rocketnet_gateway_netlink_result_name( int result );

#endif // ROCKETNET_SDK_ROCKETNET_GATEWAY_NETLINK_H
