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

// the receive ceiling: fixed for every receiver, whatever its send budget

#define ROCKETNET_RECEIVE_CEILING               1426

// the send budget: a sender never emits a packet larger than this. 1232 is
// the only value safe with no knowledge of the network (IPv6 minimum MTU
// less headers); the standard lets a deployment that knows its paths raise
// it, to at most the receive ceiling.

#define ROCKETNET_SEND_BUDGET                   1232

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
// either compiler: 4 + 2 + 4 + 1 + 1 = 12 bytes, no implicit holes.

struct rocketnet_server_xdp_config_t
{
    uint32_t public_address;                    // big endian; 0 = accept any destination address
    uint16_t public_port;                       // big endian
    uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES];
    uint8_t filter_epoch;                       // the only epoch this server accepts; 0 in 1.0
    uint8_t pad[1];
};

// The kernel filter's counters, incremented in the XDP program per verdict
// and read back by userspace -- the same counter discipline as the rest of
// the library, running through the kernel. The map is per-CPU; userspace
// sums across CPUs. For packets on the server's port,
// received == redirected + the five drop reasons.

#define ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED                 0
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL          1
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE          2
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER       3
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE       4
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH        5
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_MARTIAN_SOURCE     6
#define ROCKETNET_SERVER_XDP_NUM_COUNTERS                       7

struct rocketnet_server_xdp_stats_t
{
    uint64_t counters[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
};

#endif // #ifndef ROCKETNET_WIRE_H
