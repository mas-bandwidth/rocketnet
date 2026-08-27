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

// types 8 through 255 are reserved and ignored by a conforming receiver

// the prefilter header: 4 bytes on the front of every packet. the first byte
// is the packet type and carries the type and nothing else; the other three
// are the per-title tag, an arbitrary constant agreed by value.

#define ROCKETNET_PREFILTER_BYTES               4
#define ROCKETNET_TITLE_TAG_BYTES               3

// the published test title tag, embedded so development works out of the box.
// production titles get their own tag from the per-title generator.

#define ROCKETNET_TEST_TITLE_TAG                { 0xb7, 0x59, 0x01 }

// the receive ceiling: fixed for every receiver, whatever its send budget

#define ROCKETNET_RECEIVE_CEILING               1400

// Wire sizes, from the standard's packet size table. Every type has a fixed
// total size except the payload, which has a range.

#define ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES     708
#define ROCKETNET_CONNECTION_DENIED_WIRE_BYTES      32
#define ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES   64
#define ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES    48
#define ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES  32
#define ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES 25
#define ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES  24
#define ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES      36

// The server-inbound rule, usable from BPF and from tests alike: the five
// types a server ever receives, each at its exact wire size (the payload at
// its range). DENIED, CHALLENGE and COOKIE travel server to client only, and
// everything from 8 up is reserved -- a conforming server refuses them all,
// so the kernel filter drops them before they cost a ring slot. This mirrors
// the refusal ladder in rocketnet_server_protocol.cpp; the equivalence test
// in rocketnet_tests.cpp holds the two together.

#define ROCKETNET_WIRE_SERVER_PACKET_VALID( packet_type, packet_bytes )                                                 \
    ( ( (packet_type) == ROCKETNET_CONNECTION_REQUEST_PACKET     && (packet_bytes) == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES     ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_RESPONSE_PACKET    && (packet_bytes) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES    ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET  && (packet_bytes) == ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES  ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_PAYLOAD_PACKET     && (packet_bytes) >= ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES    \
                                                                 && (packet_bytes) <= ROCKETNET_RECEIVE_CEILING                   ) || \
      ( (packet_type) == ROCKETNET_CONNECTION_DISCONNECT_PACKET  && (packet_bytes) == ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES  ) )

// the smallest packet a server can ever accept, for the cheap early bound

#define ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES      ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES

// The kernel filter's configuration, written by userspace at server start
// and read per-packet in the XDP program. One definition serves both sides
// of the map, and the padding is explicit so the layout cannot depend on
// either compiler: 4 + 2 + 3 + 3 = 12 bytes, no implicit holes.

struct rocketnet_server_xdp_config_t
{
    uint32_t public_address;                    // big endian; 0 = accept any destination address
    uint16_t public_port;                       // big endian
    uint8_t title_tag[ROCKETNET_TITLE_TAG_BYTES];
    uint8_t pad[3];
};

// The kernel filter's counters, incremented in the XDP program per verdict
// and read back by userspace -- the same counter discipline as the rest of
// the library, running through the kernel. The map is per-CPU; userspace
// sums across CPUs. For packets on the server's port,
// received == redirected + the four drop reasons.

#define ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED                 0
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL          1
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE          2
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_TAG          3
#define ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE       4
#define ROCKETNET_SERVER_XDP_NUM_COUNTERS                       5

struct rocketnet_server_xdp_stats_t
{
    uint64_t counters[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
};

#endif // #ifndef ROCKETNET_WIRE_H
