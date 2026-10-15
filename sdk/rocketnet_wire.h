/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_WIRE_H
#define ROCKETNET_WIRE_H

#include <stdint.h>

// The wire constants shared by every reader of raw packets: the sdk, the
// tests, and the server_xdp kernel program, which is compiled as BPF C on
// the target machine and cannot include anything else from the sdk. Only
// preprocessor definitions live here, so the one file serves all three --
// the kernel filter and the userspace prefilter can never drift apart.

// packet types

#define ROCKETNET_CONNECTION_REQUEST_PACKET     0
#define ROCKETNET_CONNECTION_DENIED_PACKET      1
#define ROCKETNET_CONNECTION_CHALLENGE_PACKET   2
#define ROCKETNET_CONNECTION_RESPONSE_PACKET    3
#define ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET  4
#define ROCKETNET_CONNECTION_PAYLOAD_PACKET     5
#define ROCKETNET_CONNECTION_DISCONNECT_PACKET  6
#define ROCKETNET_CONNECTION_COOKIE_PACKET      7
#define ROCKETNET_CONNECTION_ACK_PACKET         8
#define ROCKETNET_CONNECTION_FRAGMENT_PACKET    9   // payload fragment, per the standard's _Fragmentation_

// types 10 through 255 are reserved and ignored by a conforming receiver

// the prefilter header: 6 bytes on the front of every packet -- the packet
// type, the filter epoch, and a 4-byte filter value whose meaning the epoch
// decides. The geometry never varies.

#define ROCKETNET_PREFILTER_BYTES               6
#define ROCKETNET_FILTER_VALUE_BYTES            4

// the filter epoch selects what produced the filter value: 0 is the unkeyed
// per-title constant, which is the whole of 1.0. Generations 1 through 255 are
// keyed MACs under a magic distributed by a backend -- specified in the
// standard, deliberately not implemented here, and no default magic exists.

#define ROCKETNET_FILTER_EPOCH_UNKEYED          0

// the published test filter value, embedded so development works out of the
// box at epoch 0. production titles get their own constant from the per-title
// generator. there is no default magic and none is published: see the standard.

#define ROCKETNET_TEST_FILTER_VALUE             { 0xb7, 0x59, 0x01, 0x00 }

// The packet size, from the one number it derives from: the MTU of a plain
// 1500-byte link, less the IP header and the 8-byte UDP header. The two
// address families give two sizes (RFC 8200 section 5 forbids in-transit
// fragmentation on IPv6; RFC 8085 section 3.2 asks a UDP sender to stay
// inside the path MTU), and each limit takes the one its job needs.

#define ROCKETNET_LINK_MTU_BYTES                1500
#define ROCKETNET_ETHERNET_HEADER_BYTES         14
#define ROCKETNET_IPV4_HEADER_BYTES             20
#define ROCKETNET_IPV6_HEADER_BYTES             40
#define ROCKETNET_UDP_HEADER_BYTES              8

// the receive ceiling: fixed for every receiver, whatever its send budget --
// anything one 1500-MTU IPv4 datagram can carry, 1472 bytes

#define ROCKETNET_RECEIVE_CEILING               ( ROCKETNET_LINK_MTU_BYTES - ROCKETNET_IPV4_HEADER_BYTES - ROCKETNET_UDP_HEADER_BYTES )

// the send budget: a sender never emits a packet larger than this. It is the
// smaller family's packet, the IPv6 UDP payload of a 1500-byte MTU, 1452
// bytes, so one budget serves both families: a maximum packet crosses a
// 1500-MTU path over IPv4 with 20 bytes spare and over IPv6 exactly at the
// limit. A path below MTU 1500 (an IPv6 tunnel at the 1280 minimum, PPPoE at
// 1492) drops a maximum-size IPv6 packet.

#define ROCKETNET_SEND_BUDGET                   ( ROCKETNET_LINK_MTU_BYTES - ROCKETNET_IPV6_HEADER_BYTES - ROCKETNET_UDP_HEADER_BYTES )

// The ack block, at the front of every session type's sealed plaintext:
// largest received (u32) + range count (1 byte) + first range length (u16)
// is the 7-byte minimum; each loss event adds a 4-byte (gap, length) entry,
// capped at 255 entries.

#define ROCKETNET_ACK_BLOCK_MIN_BYTES           7
#define ROCKETNET_ACK_BLOCK_MAX_RANGES          255
#define ROCKETNET_ACK_BLOCK_MAX_BYTES           ( ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * ROCKETNET_ACK_BLOCK_MAX_RANGES )

// Wire sizes, from the standard's packet size table. Every handshake type
// has a fixed total size; the session types have a range, because the ack
// block and the payload vary -- each keeps its own hard minimum here, and
// they share the one receive ceiling.

#define ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES         710
#define ROCKETNET_CONNECTION_DENIED_WIRE_BYTES          34
#define ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES       66
#define ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES        50
#define ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES  41  // 6 + 4 + 7 + 8 + 16
#define ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES     34  // 6 + 4 + 7 + 1 + 16
#define ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES  33  // 6 + 4 + 7 + 0 + 16
#define ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES          38
#define ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES         33  // 6 + 4 + 7 + 0 + 16
#define ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES    40  // 6 + 4 + 7 + 6 + 1 + 16

// The server-inbound rule, usable from BPF and from tests alike: the types
// a server ever receives, each at its exact wire size (the session types at
// their ranges). DENIED, CHALLENGE and COOKIE travel server to client only,
// and everything from 10 up is reserved -- a conforming server refuses them
// all, so the kernel filter drops them before they cost a ring slot. This
// mirrors the refusal ladder in rocketnet_server_protocol.cpp; the
// equivalence test in rocketnet_tests.cpp holds the two together.

#define ROCKETNET_WIRE_SERVER_PACKET_VALID( packet_type, packet_bytes )                                                 \
    ( ( (packet_type) == ROCKETNET_CONNECTION_REQUEST_PACKET     && (packet_bytes) == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES     ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_RESPONSE_PACKET    && (packet_bytes) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES    ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET  && (packet_bytes) >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES \
                                                                 && (packet_bytes) <= ROCKETNET_RECEIVE_CEILING                   ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_PAYLOAD_PACKET     && (packet_bytes) >= ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES    \
                                                                 && (packet_bytes) <= ROCKETNET_RECEIVE_CEILING                   ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_DISCONNECT_PACKET  && (packet_bytes) >= ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES \
                                                                 && (packet_bytes) <= ROCKETNET_RECEIVE_CEILING                   ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_ACK_PACKET         && (packet_bytes) >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES        \
                                                                 && (packet_bytes) <= ROCKETNET_RECEIVE_CEILING                   ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_FRAGMENT_PACKET    && (packet_bytes) >= ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES   \
                                                                 && (packet_bytes) <= ROCKETNET_RECEIVE_CEILING                   ) )

// the smallest packet a server can ever accept, for the cheap early bound

#define ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES      ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES

// The kernel filter's configuration, written by userspace at server start
// and read per-packet in the XDP program. One definition serves both sides
// of the map, and the padding is explicit so the layout cannot depend on
// either compiler: 4 + 2 + 4 + 1 + 5 + 16 = 32 bytes, no implicit holes.
// The IPv6 destination sits at offset 16 so the program reads it as four
// aligned 32-bit words.

struct rocketnet_server_xdp_config_t
{
    uint32_t public_address;                    // big endian; 0 = accept any destination address
    uint16_t public_port;                       // big endian
    uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES];
    uint8_t filter_epoch;                       // the only epoch this server accepts; 0 in 1.0
    uint8_t pad[5];                             // zero; fills offsets 11 to 15, up to the IPv6 destination
    uint8_t public_address_ipv6[16];            // network order; all zero = accept any IPv6 destination address
};

#ifdef __cplusplus
static_assert( sizeof(struct rocketnet_server_xdp_config_t) == 32, "the XDP config map value is 32 bytes" );
static_assert( __builtin_offsetof( struct rocketnet_server_xdp_config_t, pad ) == 11, "the XDP config pad follows the epoch" );
static_assert( __builtin_offsetof( struct rocketnet_server_xdp_config_t, public_address_ipv6 ) == 16, "the XDP config IPv6 destination is 4-byte aligned" );
#else // #ifdef __cplusplus
_Static_assert( sizeof(struct rocketnet_server_xdp_config_t) == 32, "the XDP config map value is 32 bytes" );
_Static_assert( __builtin_offsetof( struct rocketnet_server_xdp_config_t, pad ) == 11, "the XDP config pad follows the epoch" );
_Static_assert( __builtin_offsetof( struct rocketnet_server_xdp_config_t, public_address_ipv6 ) == 16, "the XDP config IPv6 destination is 4-byte aligned" );
#endif // #ifdef __cplusplus

// The kernel filter's counters, read back by userspace -- the same counter
// discipline as the rest of the library, running through the kernel. The map
// is per-CPU; userspace sums across CPUs.
//
// RECEIVED counts every frame the program sees, on entry. Every exit of the
// program then raises exactly one other counter: REDIRECTED, a DROPPED_*
// reason, a prefilter or type reason, or a PASSED_* reason. So, by
// construction,
//
//     RECEIVED == REDIRECTED + every DROPPED_* + every prefilter and type
//                 counter + every PASSED_*
//
// and a frame never leaves the program uncounted.
//
// Frames that are not for this server pass to the kernel stack, each under
// the condition the program observed: a frame shorter than an Ethernet header,
// an ethertype that is not IP, an IPv4 header with options or a protocol that
// is not UDP, a truncated UDP header, a filter that is not configured, a
// destination port or address that is not the server's, an IPv6 next header
// that is an extension header (the program never walks that chain, so it
// cannot say whether such a frame was for this server, and counts it on any
// port), and an IPv6 next header that is neither UDP nor an extension. A
// truncated IPv4 or IPv6 header drops.
//
// Frames for the server's port and address run the userspace refusal ladder,
// in its order and under its names (STANDARD.md, _Counters_, the prefilter's
// four reasons; and _Reading packets_ for the order): PREFILTER_TOO_SHORT
// below the 6-byte prefilter header, PREFILTER_TOO_LARGE above the receive
// ceiling, PREFILTER_WRONG_EPOCH, PREFILTER_WRONG_VALUE, then RESERVED_TYPE
// for a type above 9, WRONG_DIRECTION for denied, challenge and cookie, and
// then the type's own WRONG_SIZE counter for a server-inbound type outside
// its wire size. A frame that passes the ladder redirects into the AF_XDP
// socket for its queue (REDIRECTED), or drops with DROPPED_REDIRECT_NO_SOCKET
// when that queue has no socket.

#define ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED                         0
#define ROCKETNET_SERVER_XDP_COUNTER_RECEIVED                           1
#define ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_TOO_SHORT                2
#define ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_TOO_LARGE                3
#define ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_EPOCH              4
#define ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_VALUE              5
#define ROCKETNET_SERVER_XDP_COUNTER_RESERVED_TYPE                      6
#define ROCKETNET_SERVER_XDP_COUNTER_WRONG_DIRECTION                    7
#define ROCKETNET_SERVER_XDP_COUNTER_CONNECTION_REQUEST_WRONG_SIZE      8
#define ROCKETNET_SERVER_XDP_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE     9
#define ROCKETNET_SERVER_XDP_COUNTER_KEEP_ALIVE_WRONG_SIZE              10
#define ROCKETNET_SERVER_XDP_COUNTER_PAYLOAD_WRONG_SIZE                 11
#define ROCKETNET_SERVER_XDP_COUNTER_DISCONNECT_WRONG_SIZE              12
#define ROCKETNET_SERVER_XDP_COUNTER_ACK_WRONG_SIZE                     13
#define ROCKETNET_SERVER_XDP_COUNTER_FRAGMENT_WRONG_SIZE                14
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV4_SHORT_HEADER          15
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV6_SHORT_HEADER          16
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REDIRECT_NO_SOCKET         17
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_SHORT_ETHERNET              18
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_NOT_IP                      19
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV4_NOT_PLAIN_UDP          20
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_UDP_SHORT_HEADER            21
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_NOT_CONFIGURED              22
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION           23
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION              24
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_NOT_UDP                25
#define ROCKETNET_SERVER_XDP_NUM_COUNTERS                               26

// One row per counter, in index order: the define, the name, and the class.
// "arrival" is RECEIVED, "refusal" is every drop, "expected" is REDIRECTED and
// every pass. The prefilter and type rows carry the userspace counter's own
// name, so the two report in the same shape. Host builds only; the kernel
// program has no use for strings.

#if !defined(__BPF__)

#define ROCKETNET_SERVER_XDP_COUNTER_TABLE( X )                                                                     \
    X( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED,                       "REDIRECTED",                       "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_RECEIVED,                         "RECEIVED",                         "arrival"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_TOO_SHORT,              "PREFILTER_TOO_SHORT",              "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_TOO_LARGE,              "PREFILTER_TOO_LARGE",              "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_EPOCH,            "PREFILTER_WRONG_EPOCH",            "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_VALUE,            "PREFILTER_WRONG_VALUE",            "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_RESERVED_TYPE,                    "RESERVED_TYPE",                    "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_WRONG_DIRECTION,                  "WRONG_DIRECTION",                  "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_CONNECTION_REQUEST_WRONG_SIZE,    "CONNECTION_REQUEST_WRONG_SIZE",    "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE,   "CONNECTION_RESPONSE_WRONG_SIZE",   "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_KEEP_ALIVE_WRONG_SIZE,            "KEEP_ALIVE_WRONG_SIZE",            "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PAYLOAD_WRONG_SIZE,               "PAYLOAD_WRONG_SIZE",               "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_DISCONNECT_WRONG_SIZE,            "DISCONNECT_WRONG_SIZE",            "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_ACK_WRONG_SIZE,                   "ACK_WRONG_SIZE",                   "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_FRAGMENT_WRONG_SIZE,              "FRAGMENT_WRONG_SIZE",              "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV4_SHORT_HEADER,        "DROPPED_IPV4_SHORT_HEADER",        "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV6_SHORT_HEADER,        "DROPPED_IPV6_SHORT_HEADER",        "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REDIRECT_NO_SOCKET,       "DROPPED_REDIRECT_NO_SOCKET",       "refusal"  ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_SHORT_ETHERNET,            "PASSED_SHORT_ETHERNET",            "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_NOT_IP,                    "PASSED_NOT_IP",                    "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV4_NOT_PLAIN_UDP,        "PASSED_IPV4_NOT_PLAIN_UDP",        "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_UDP_SHORT_HEADER,          "PASSED_UDP_SHORT_HEADER",          "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_NOT_CONFIGURED,            "PASSED_NOT_CONFIGURED",            "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION,         "PASSED_OTHER_DESTINATION",         "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION,            "PASSED_IPV6_EXTENSION",            "expected" ) \
    X( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_NOT_UDP,              "PASSED_IPV6_NOT_UDP",              "expected" )

#define ROCKETNET_SERVER_XDP_COUNTER_TABLE_ROW_COUNT( define, name, kind ) + 1

#ifdef __cplusplus
static_assert( 0 ROCKETNET_SERVER_XDP_COUNTER_TABLE( ROCKETNET_SERVER_XDP_COUNTER_TABLE_ROW_COUNT ) == ROCKETNET_SERVER_XDP_NUM_COUNTERS, "the XDP counter table has one row per counter" );
#else // #ifdef __cplusplus
_Static_assert( 0 ROCKETNET_SERVER_XDP_COUNTER_TABLE( ROCKETNET_SERVER_XDP_COUNTER_TABLE_ROW_COUNT ) == ROCKETNET_SERVER_XDP_NUM_COUNTERS, "the XDP counter table has one row per counter" );
#endif // #ifdef __cplusplus

#endif // #if !defined(__BPF__)

struct rocketnet_server_xdp_stats_t
{
    uint64_t counters[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
};

#endif // #ifndef ROCKETNET_WIRE_H
