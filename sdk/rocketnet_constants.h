/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONSTANTS_H
#define ROCKETNET_CONSTANTS_H

#include "rocketnet_internal.h"
#include "rocketnet_history.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"

// The packet size limits, each asserted against its derivation so a change
// to the MTU in rocketnet_wire.h is one line and a failed assert names the
// limit that no longer follows (STANDARD.md _Packet sizes_). The public
// header carries its values as written numbers, since it cannot see the
// internal constants; these asserts hold them to the derivation.

ROCKETNET_STATIC_ASSERT( ROCKETNET_RECEIVE_PACKET_BYTES == ROCKETNET_RECEIVE_CEILING,
                         "the receive packet size is the common send and receive ceiling" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PACKET_BYTES == ROCKETNET_SEND_BUDGET,
                         "the public IPv6 packet size is the send budget: the IPv6 UDP payload of the link MTU" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SEND_BUDGET == ROCKETNET_RECEIVE_CEILING,
                         "a sender's largest packet is one every receiver accepts" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ==
                         ( ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES ) / 8 * 8,
                         "the payload capacity is the send budget less the overhead and the minimum ack block, rounded down to eight" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_RECEIVE_PAYLOAD_BYTES ==
                         ROCKETNET_MAX_PACKET_PAYLOAD_BYTES,
                         "the receive payload bound matches the sender capacity" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MESSAGE_SINGLE_BYTES == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1,
                         "the SINGLE message is the payload capacity less its form byte" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES +
                         ROCKETNET_UDP_HEADER_BYTES + ROCKETNET_MAX_PACKET_BYTES == 1514,
                         "one maximum IPv6 packet is exactly one 1500-MTU Ethernet frame" );

#define ROCKETNET_FRAME_SIZE                                              2048

ROCKETNET_STATIC_ASSERT( ROCKETNET_FRAME_SIZE >= 16 + ROCKETNET_RECEIVE_CEILING,
                         "a send frame holds the 16-byte payload alignment slack and a full receive-ceiling packet" );

#define ROCKETNET_RECEIVE_PACKETS_PER_CLIENT                                16

// The XDP frame size is defined on every platform: the frame codec in
// rocketnet_xdp_frame.h (rocketnet_xdp_receive_descriptor) and its unit cases
// are portable and build in ./bin/test everywhere. The transport's own static
// assert below and in rocketnet_server_socket_linux.cpp pin it where XDP is on.
#define ROCKETNET_XDP_FRAME_SIZE                                          4096  // IMPORTANT: This *must* be 4096 or you will get packet corruption on read!

#if ROCKETNET_XDP

#define ROCKETNET_XDP_NUM_BUFFERS                                            3
#define ROCKETNET_XDP_NUM_FRAMES                                         65536
#define ROCKETNET_XDP_SEND_QUEUE_SIZE                             ( 1024 * 4 )
#define ROCKETNET_XDP_RECV_QUEUE_SIZE                             ( 4096 * 4 )
#define ROCKETNET_XDP_FILL_QUEUE_SIZE                                     2048
#define ROCKETNET_XDP_SEND_BATCH_SIZE                                       32

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FRAME_SIZE >= ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES +
                         ROCKETNET_UDP_HEADER_BYTES + ROCKETNET_RECEIVE_CEILING,
                         "an XDP frame holds ethernet, the larger IP header, UDP and a full receive-ceiling packet" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FRAME_SIZE == 4096,
                         "the XDP UMEM frame the transport registers is 4096 bytes" );

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_RECV_QUEUE_SIZE >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE,
                         "receive budget twice send: XDP receive queue holds at least twice the send queue" );

#endif // #if ROCKETNET_XDP

#define ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS        ( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS )

#if ROCKETNET_XDP
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE,
                         "receive budget twice send: server process batch holds at least twice the XDP send queue" );
#endif

// One update reads at most ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS packets (16 * 1000 = 16000), bounding its work. This equals the payload-batch capacity; batches share one bound and one storage cap.
#define ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE   ( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS )

// Portable and msgx buffered batch sizes. The per-update receive budget
// holds at least twice the send batch. The transport uses max_clients with
// the historical 256-slot floor, bounded by the shared maximum below.
#define ROCKETNET_SERVER_MIN_SEND_PACKETS                                  256
#define ROCKETNET_SERVER_MAX_SEND_PACKETS                       ROCKETNET_MAX_CLIENTS
#define ROCKETNET_SERVER_MAX_RECEIVE_PACKETS                               256

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS,
                         "receive budget twice send: per-update receive budget holds at least twice the portable send batch" );

// the send frame layout. every slot a transport hands out is a 16-aligned
// root; the wire packet begins at the wire offset inside it and the payload
// begins at the payload offset -- the wire offset plus the 10-byte sealed
// header plus the 7-byte minimum ack block -- itself a multiple of 16, so
// the pointer a caller writes payload at is always aligned for any
// word-based serializer. the 15 bytes before the wire offset are
// memory-only slack, never sent. with the minimum block -- everything
// received, the common case on a clean link -- the payload seals exactly
// where the game wrote it; a block carrying loss ranges moves the payload
// up inside the frame by the extra block bytes before sealing, bounded by
// the send budget.

#define ROCKETNET_FRAME_WIRE_OFFSET                             15
#define ROCKETNET_FRAME_PAYLOAD_OFFSET                          ( ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES )     // 32
#define ROCKETNET_FRAME_STRIDE                                  ( ( ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING + 15 ) / 16 * 16 )                                              // 1472

ROCKETNET_STATIC_ASSERT( ROCKETNET_FRAME_PAYLOAD_OFFSET % 16 == 0, "the payload root is 16-aligned inside a 16-aligned frame" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_FRAME_STRIDE % 16 == 0, "every frame slot root is 16-aligned" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_FRAME_STRIDE >= ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING,
                         "a frame slot holds the slack and a full receive-ceiling packet" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_FRAME_STRIDE <= ROCKETNET_FRAME_SIZE, "a frame slot fits a send frame" );

// the most payloads one server update can deliver: what the receive path can
// hand over in a single drain, so a burst is never silently discarded after
// its sequence has already been consumed. internal -- the public surface
// iterates payloads by index and never sees a capacity.

#define ROCKETNET_MAX_PAYLOADS_PER_UPDATE                  ( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS )

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE == ROCKETNET_MAX_PAYLOADS_PER_UPDATE,
                         "all receive batches in one update share the payload batch capacity" );

// NOTE the relationship between these two. The process_packets arrays are
// sized by MAX_PROCESS_PACKETS (16 * 1000 = 16000), but the XDP path fills
// them while draining a queue whose staging buffer holds
// ROCKETNET_XDP_RECV_QUEUE_SIZE entries (4096 * 4 = 16384). They are
// independent numbers, and the drain guard MUST bound by
// MAX_PROCESS_PACKETS — the arrays it protects — never the queue size, or
// the tail of a full queue writes past the arrays.
//
// The consequence of the correct guard is capacity, not safety: because the
// queue can stage more than the arrays hold, a single tick that delivers
// more than 16000 packets to one server DROPS the excess. That is correct
// behaviour under flood, and at the design load (1000 clients * 16 packets)
// it is exactly the budget. If you would rather never drop, raise
// MAX_CLIENTS or RECEIVE_PACKETS_PER_CLIENT so the arrays cover a full
// queue drain.

// the send arena: how many over-wire-capacity payloads one fill phase can
// stage per transport. each slot is the configured maximum payload, spent
// at create; exhaustion drops at start_packet on SEND_ARENA_FULL, the
// arena sibling of the ring counter.

#define ROCKETNET_SEND_ARENA_SLOTS                                          16

#define ROCKETNET_NUM_CLIENT_PACKETS                                      1024

// Packet-count history budget, shared with receive-side ACK/replay history.
// There is no time-based expiration, send quota or ACK cadence assumption.
#define ROCKETNET_SENT_PACKET_BUFFER_SIZE                ROCKETNET_HISTORY_PACKETS
#define ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE                             2048
#define ROCKETNET_RTT_HISTORY_SIZE                                        2048

// the packet threshold of RFC 9002 section 6.1.1 (kPacketThreshold): a sent
// packet is known lost once the peer has acknowledged a sequence at least
// this many above it. sequence based, so it needs no clock and fits the
// sequence-distance windows of the standard's _Replay window sizing_.
// tla/RNSentHistory.tla models the classification it drives.

#define ROCKETNET_LOSS_PACKET_THRESHOLD                                   3

// the AEAD integrity limit from the standard's _Replay Protection_: each end
// counts the sealed packets that fail to open on a connection, across all of
// that connection's keys, and disconnects it on reaching this many. 2^36 is
// ChaCha20-Poly1305's forgery bound, the number QUIC and DTLS 1.3 name.

#define ROCKETNET_INTEGRITY_LIMIT                               ( 1ULL << 36 )

// Any accepted eliciting packet makes an ack due on the next update.

#define ROCKETNET_ACK_DEBT_THRESHOLD                                       1

// The adaptive resend delay: it follows the measured round trip.

#define ROCKETNET_MESSAGE_RESEND_DELAY_SCALE_PERCENT                          200
#define ROCKETNET_MESSAGE_RESEND_DELAY_VARIANCE_SCALE                           4
#define ROCKETNET_MESSAGE_RESEND_DELAY_GRANULARITY_MS                          30
// Retained for historical benchmark comparisons; it does not cap live resends.
#define ROCKETNET_MESSAGE_RESEND_INTERVAL_MS                              100

// Bound per-client ACK statistics publication to this many slot visits per
// server update. Free slots consume a visit but never touch cold endpoints.
#define ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE                           20

#define ROCKETNET_PACKET_LOSS_TRACKER_HISTORY                             1024
#define ROCKETNET_PACKET_LOSS_TRACKER_SAFETY                                30

// ROCKETNET_IPV4_HEADER_BYTES and ROCKETNET_UDP_HEADER_BYTES live in
// rocketnet_wire.h, beside the MTU they derive the packet size from

#define ROCKETNET_SOCKET_SEND_BUFFER_SIZE                              1000000
#define ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE                           1000000

#define ROCKETNET_VALUE_TRACKER_HISTORY                                   1024

#endif // #ifndef ROCKETNET_CONSTANTS_H
