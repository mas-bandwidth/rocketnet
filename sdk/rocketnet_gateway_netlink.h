/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_GATEWAY_NETLINK_H
#define ROCKETNET_SDK_ROCKETNET_GATEWAY_NETLINK_H

// the XDP transport's gateway discovery through rtnetlink, the Linux routing
// API (iproute2's ip(8) is a client of the same socket): no process is
// started and nothing is read from PATH. Two dumps answer it, each on a
// fresh NETLINK_ROUTE socket at create:
//
//   RTM_GETROUTE  the default route (destination length 0, unicast, main
//                 table, not cloned) of the family whose output interface is
//                 this interface: its RTA_GATEWAY is the gateway. An IPv4
//                 multipath default route answers with its first next hop,
//                 as /proc/net/route (netstat -rn) did.
//   RTM_GETNEIGH  the family's neighbour entry on this interface whose
//                 NDA_DST is the gateway, in a state ip(8) shows (reachable,
//                 stale, delay, probe or permanent) and carrying a 6-byte
//                 NDA_LLADDR: that is the gateway's MAC. Nothing resolves a
//                 missing entry.
//
// The parse functions are pure: one received datagram of a dump in, an
// outcome out, no socket and no allocation (test_unit_xdp_gateway_netlink.cpp
// feeds them canned replies). The dump functions are the thin socket wrapper
// around them: a fixed-size receive buffer, SO_RCVTIMEO, a bounded number of
// reads, and the reply's sequence number, port id and sender checked.

#include <stddef.h>
#include <stdint.h>

#define ROCKETNET_GATEWAY_NETLINK_MORE          0   // nothing found in this datagram; the dump continues
#define ROCKETNET_GATEWAY_NETLINK_FOUND         1   // the answer is written
#define ROCKETNET_GATEWAY_NETLINK_DONE          2   // NLMSG_DONE: the dump ended with no answer
#define ROCKETNET_GATEWAY_NETLINK_MALFORMED     3   // a message or attribute overruns its bounds, an answer has the wrong length, or the datagram was truncated
#define ROCKETNET_GATEWAY_NETLINK_ERROR         4   // the kernel answered NLMSG_ERROR, or the dump was interrupted; *error holds the errno
#define ROCKETNET_GATEWAY_NETLINK_SYSTEM        5   // the socket failed, timed out, or the read bound ran out; *error holds the errno

// one received datagram of an RTM_GETROUTE dump: FOUND writes the gateway
// (4 bytes for AF_INET, 16 for AF_INET6) of the first default route of family
// whose output interface is interface_index. Messages whose sequence or port
// id is not the request's are skipped.

int rocketnet_gateway_netlink_parse_route( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                           int family, int interface_index, uint8_t * gateway, int * error );

// one received datagram of an RTM_GETNEIGH dump: FOUND writes the 6-byte
// link-layer address of the first usable neighbour entry of family on
// interface_index whose destination is gateway.

int rocketnet_gateway_netlink_parse_neighbor( const uint8_t * data, size_t bytes, uint32_t sequence, uint32_t port_id,
                                              int family, int interface_index, const uint8_t * gateway, uint8_t * mac, int * error );

// the dumps themselves, over a NETLINK_ROUTE socket opened and closed inside
// the call: the outcome of the first datagram that is not MORE, or SYSTEM

int rocketnet_gateway_netlink_route( int family, int interface_index, uint8_t * gateway, int * error );

int rocketnet_gateway_netlink_neighbor( int family, int interface_index, const uint8_t * gateway, uint8_t * mac, int * error );

// the outcome's name for a log line

const char * rocketnet_gateway_netlink_result_name( int result );

#endif // #ifndef ROCKETNET_SDK_ROCKETNET_GATEWAY_NETLINK_H
