/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_WIRE_H
#define ROCKETNET_SDK_ROCKETNET_WIRE_H

#include <stdint.h>

// The wire constants shared by every reader of raw packets: the sdk, the
// tests, and the server_xdp kernel program, which is compiled as BPF C on
// the target machine and cannot include anything else from the sdk. Only
// preprocessor definitions and plain inline helpers live here, so the one
// file serves all three -- the kernel filter and the userspace prefilter
// can never drift apart.

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

// The IP and UDP header bytes on one datagram, from the remote endpoint's address family:
// 28 for IPv4 and 48 for IPv6. The values are the rocketnet_address_t.type
// families from rocketnet_layers.h (1 IPv4, 2 IPv6); an address with no family
// has no headers.

static inline int rocketnet_wire_header_bytes( int address_type )
{
    if ( address_type == 1 )    // ROCKETNET_ADDRESS_IPV4
        return ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
    if ( address_type == 2 )    // ROCKETNET_ADDRESS_IPV6
        return ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
    return 0;
}

// The receive ceiling matches the send budget for both address families.

#define ROCKETNET_RECEIVE_CEILING               ROCKETNET_SEND_BUDGET

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

// Every handshake packet fits one datagram on the smaller family's budget:
// 1500 - 40 - 8 = 1452 UDP payload bytes over IPv6 (ROCKETNET_MAX_PACKET_BYTES
// in rocketnet.h; written out here because this header stands alone in the
// XDP program). A token change that would push the connection request past
// one IPv6 datagram fails to compile.

#define ROCKETNET_HANDSHAKE_MAX_WIRE_BYTES              1452    // ( 1500 - 40 - 8 )

#ifdef __cplusplus
#define ROCKETNET_WIRE_STATIC_ASSERT( condition, message ) static_assert( condition, message )
#elif defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 201112L
#define ROCKETNET_WIRE_STATIC_ASSERT( condition, message ) _Static_assert( condition, message )
#else // Before C11, a negative array size refuses the build without language extensions.
#define ROCKETNET_WIRE_STATIC_ASSERT_JOIN2( a, b ) a##b
#define ROCKETNET_WIRE_STATIC_ASSERT_JOIN( a, b ) ROCKETNET_WIRE_STATIC_ASSERT_JOIN2( a, b )
#define ROCKETNET_WIRE_STATIC_ASSERT( condition, message ) typedef char ROCKETNET_WIRE_STATIC_ASSERT_JOIN( rocketnet_wire_static_assert_, __LINE__ )[( condition ) ? 1 : -1]
#endif

ROCKETNET_WIRE_STATIC_ASSERT( ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES <= ROCKETNET_HANDSHAKE_MAX_WIRE_BYTES, "the connection request fits one IPv6 datagram" );
ROCKETNET_WIRE_STATIC_ASSERT( ROCKETNET_CONNECTION_DENIED_WIRE_BYTES <= ROCKETNET_HANDSHAKE_MAX_WIRE_BYTES, "the connection denied fits one IPv6 datagram" );
ROCKETNET_WIRE_STATIC_ASSERT( ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES <= ROCKETNET_HANDSHAKE_MAX_WIRE_BYTES, "the connection challenge fits one IPv6 datagram" );
ROCKETNET_WIRE_STATIC_ASSERT( ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES <= ROCKETNET_HANDSHAKE_MAX_WIRE_BYTES, "the connection response fits one IPv6 datagram" );
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
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES];
    uint8_t filter_epoch;                       // the only epoch this server accepts; 0 in 1.0
    uint8_t pad[5];                             // zero; fills offsets 11 to 15, up to the IPv6 destination
    uint8_t public_address_ipv6[16];            // network order; all zero = accept any IPv6 destination address
};

#ifdef __cplusplus
static_assert( sizeof( struct rocketnet_server_xdp_config_t ) == 32, "the XDP config map value is 32 bytes" );
static_assert( __builtin_offsetof( struct rocketnet_server_xdp_config_t, pad ) == 11, "the XDP config pad follows the epoch" );
static_assert( __builtin_offsetof( struct rocketnet_server_xdp_config_t, public_address_ipv6 ) == 16, "the XDP config IPv6 destination is 4-byte aligned" );
#elif defined( __GNUC__ ) || defined( __clang__ ) // C: __builtin_offsetof is a GNU form; MSVC C checks none of this
ROCKETNET_WIRE_STATIC_ASSERT( sizeof( struct rocketnet_server_xdp_config_t ) == 32, "the XDP config map value is 32 bytes" );
ROCKETNET_WIRE_STATIC_ASSERT( __builtin_offsetof( struct rocketnet_server_xdp_config_t, pad ) == 11, "the XDP config pad follows the epoch" );
ROCKETNET_WIRE_STATIC_ASSERT( __builtin_offsetof( struct rocketnet_server_xdp_config_t, public_address_ipv6 ) == 16, "the XDP config IPv6 destination is 4-byte aligned" );
#endif // #ifdef __cplusplus

// The kernel replay map's key and value, in the same shared-wire discipline
// as the config above: one definition serves the BPF program and userspace,
// and the padding is explicit so the layout cannot depend on either compiler.
// The key is 4 + 2 + 16 = 20 bytes with no implicit holes; the value 8 bytes.
// The port and address are network order; an IPv4 source occupies the first 4
// address bytes with the rest zero.

struct rocketnet_server_xdp_replay_key_t
{
    uint8_t family;                     // 4 or 6
    uint8_t pad;                        // zero; fills offset 1, up to the port
    uint16_t port;                      // network order, the UDP source port
    uint8_t address[16];                // network order; IPv4 in the first 4 bytes, the rest zero
};

struct rocketnet_server_xdp_replay_value_t
{
    uint32_t high_water;                // the highest sequence the map has marked
    uint32_t pad;                       // zero; keeps the value at 8 bytes
};

#ifdef __cplusplus
static_assert( sizeof( struct rocketnet_server_xdp_replay_key_t ) == 20, "the XDP replay key is 20 bytes" );
static_assert( __builtin_offsetof( struct rocketnet_server_xdp_replay_key_t, port ) == 2, "the XDP replay port is at offset 2" );
static_assert( __builtin_offsetof( struct rocketnet_server_xdp_replay_key_t, address ) == 4, "the XDP replay address is at offset 4" );
static_assert( sizeof( struct rocketnet_server_xdp_replay_value_t ) == 8, "the XDP replay value is 8 bytes" );
#elif defined( __GNUC__ ) || defined( __clang__ ) // C: __builtin_offsetof is a GNU form; MSVC C checks none of this
ROCKETNET_WIRE_STATIC_ASSERT( sizeof( struct rocketnet_server_xdp_replay_key_t ) == 20, "the XDP replay key is 20 bytes" );
ROCKETNET_WIRE_STATIC_ASSERT( __builtin_offsetof( struct rocketnet_server_xdp_replay_key_t, port ) == 2, "the XDP replay port is at offset 2" );
ROCKETNET_WIRE_STATIC_ASSERT( __builtin_offsetof( struct rocketnet_server_xdp_replay_key_t, address ) == 4, "the XDP replay address is at offset 4" );
ROCKETNET_WIRE_STATIC_ASSERT( sizeof( struct rocketnet_server_xdp_replay_value_t ) == 8, "the XDP replay value is 8 bytes" );
#endif // #ifdef __cplusplus

// The kernel filter's counters, incremented in the XDP program per verdict
// and read back by userspace -- the same counter discipline as the rest of
// the library, running through the kernel. The map is per-CPU; userspace
// sums across CPUs. For packets on the server's port,
// received == redirected + the nine drop reasons. DROPPED_REPLAYED counts
// the session packets dropped because their sequence is
// ROCKETNET_XDP_REPLAY_WINDOW or more behind the replay map's high-water for
// their source address and port. PASSED_IPV6_EXTENSION
// counts the IPv6 frames the program hands to the kernel stack because their
// next header is an extension header (hop-by-hop, routing, destination
// options, AH, ESP) and the walk of the chain did not reach UDP to this
// server (another port, ESP, a non-first fragment, a chain past the walk's
// bound, or the frame end), on any port. RocketNet assumes a 1500-byte MTU path, so an IP fragment for
// the server is hostile or misconfigured. DROPPED_FIRST_FRAGMENT counts the
// first fragments the program drops: IPv4 with more-fragments set and
// offset 0 (ihl 5, protocol UDP), or IPv6 whose next header is a fragment
// header at offset 0 with more-fragments set and UDP behind it, directly or
// through the bounded extension-header walk, whose UDP header is in the
// frame and names the configured address and port.
// Without its first fragment the kernel never reassembles the datagram and
// drops the rest at its reassembly timeout. PASSED_FRAGMENT counts every
// other IP fragment, which the program hands to the kernel stack for
// reassembly: non-first fragments on any port, and first fragments to
// another port or address, to an unconfigured program, or whose UDP header
// is not in the frame. Both checks run before the size, epoch, filter and
// replay checks, so a fragment never counts as any other drop.
// DROPPED_NO_SOCKET counts the packets that passed every check but whose
// receive queue has no AF_XDP socket in the XSKMAP (bpf_redirect_map
// refuses the empty slot and the program drops the packet).
// DROPPED_IP_OPTIONS_OR_EXTENSION counts the datagrams to the configured
// address and port that the program drops because IPv4 options (ihl 6 to
// 15) or IPv6 extension headers sit between the IP header and UDP: RocketNet
// never sends either, and in XDP mode no kernel socket listens on the port.
// The program walks an IPv6 extension chain a bounded number of headers to
// find the UDP header; an extension-header frame it does not drop is counted
// in PASSED_IPV6_EXTENSION as before, on any port.

#define ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED                 0
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL          1
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE          2
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER       3
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE       4
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH        5
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION      6
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED           7
#define ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT            8
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT     9
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_NO_SOCKET          10
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IP_OPTIONS_OR_EXTENSION 11
#define ROCKETNET_SERVER_XDP_NUM_COUNTERS                       12

struct rocketnet_server_xdp_stats_t
{
    uint64_t counters[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
};

#endif // ROCKETNET_SDK_ROCKETNET_WIRE_H
