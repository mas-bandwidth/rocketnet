/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONSTANTS_H
#define ROCKETNET_CONSTANTS_H

#include "rocketnet.h"
#include "rocketnet_history.h"

#define ROCKETNET_FRAME_SIZE                                              2048

#define ROCKETNET_RECEIVE_PACKETS_PER_CLIENT                                16

#if ROCKETNET_XDP

#define ROCKETNET_XDP_NUM_BUFFERS                                            2
#define ROCKETNET_XDP_NUM_FRAMES                                         65536
#define ROCKETNET_XDP_FRAME_SIZE                                          4096  // IMPORTANT: This *must* be 4096 or you will get packet corruption on read!
#define ROCKETNET_XDP_SEND_QUEUE_SIZE                             ( 4096 * 4 )
#define ROCKETNET_XDP_RECV_QUEUE_SIZE                             ( 4096 * 4 )
#define ROCKETNET_XDP_FILL_QUEUE_SIZE                                     2048
#define ROCKETNET_XDP_SEND_BATCH_SIZE                                       32

#endif // #if ROCKETNET_XDP

#define ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS        ( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS )

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
#define ROCKETNET_FRAME_PAYLOAD_OFFSET                          32          // wire offset + 10 header + 7 minimum block
#define ROCKETNET_FRAME_STRIDE                                  1456        // 16-aligned; holds 15 slack + a full receive-ceiling wire packet

// the most payloads one server update can deliver: what the receive path can
// hand over in a single drain, so a burst is never silently discarded after
// its sequence has already been consumed. internal -- the public surface
// iterates payloads by index and never sees a capacity.

#define ROCKETNET_MAX_PAYLOADS_PER_UPDATE                  ( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS )

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

// the AEAD integrity limit from the standard's _Replay Protection_: each end
// counts the sealed packets that fail to open on a connection, across all of
// that connection's keys, and disconnects it on reaching this many. 2^36 is
// ChaCha20-Poly1305's forgery bound, the number QUIC and DTLS 1.3 name.

#define ROCKETNET_INTEGRITY_LIMIT                               ( 1ULL << 36 )

// the ack debt threshold from the standard's timing table: a standalone ack
// goes out at this many eliciting packets received since the last block sent

#define ROCKETNET_ACK_DEBT_THRESHOLD                                      64

// the per-client statistics recompute runs round-robin, a sixth of the
// slots per update, every slot covered -- the space reference's amortization

#define ROCKETNET_CLIENT_STATS_UPDATE_PHASES                              6

#define ROCKETNET_PACKET_LOSS_TRACKER_HISTORY                             1024
#define ROCKETNET_PACKET_LOSS_TRACKER_SAFETY                                30

#define ROCKETNET_IPV4_HEADER_BYTES                                         20
#define ROCKETNET_UDP_HEADER_BYTES                                           8

#define ROCKETNET_SOCKET_SEND_BUFFER_SIZE                              1000000
#define ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE                           1000000

#define ROCKETNET_VALUE_TRACKER_HISTORY                                   1024

#endif // #ifndef ROCKETNET_CONSTANTS_H
