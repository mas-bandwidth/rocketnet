/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONSTANTS_H
#define ROCKETNET_CONSTANTS_H

#include "rocketnet.h"

#define ROCKETNET_FRAME_SIZE                                              2048

#define ROCKETNET_SEND_PACKETS_PER_CLIENT                                   64

#define ROCKETNET_RECEIVE_PACKETS_PER_CLIENT                                16

#if ROCKETNET_XDP

#define ROCKETNET_XDP_NUM_BUFFERS                                            2
#define ROCKETNET_XDP_NUM_FRAMES                                         65536
#define ROCKETNET_XDP_FRAME_SIZE                                          4096  // IMPORTANT: This *must* be 4096 or you will get packet corruption on read!
#define ROCKETNET_XDP_SEND_QUEUE_SIZE                             ( 4096 * 4 )
#define ROCKETNET_XDP_RECV_QUEUE_SIZE                             ( 4096 * 4 )
#define ROCKETNET_XDP_FILL_QUEUE_SIZE                                     2048
#define ROCKETNET_XDP_SEND_BATCH_SIZE                                       32

#else // #if ROCKETNET_XDP

#define ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS              ( ROCKETNET_SEND_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS )

#define ROCKETNET_SERVER_SOCKET_MAX_RECEIVE_PACKETS        ( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS )

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

// NOTE the relationship between these two, because it bit once already.
//
// The process_packets arrays are sized by MAX_PROCESS_PACKETS (16 * 1000 =
// 16000), but the XDP path fills them while draining a queue whose staging
// buffer holds ROCKETNET_XDP_RECV_QUEUE_SIZE entries (4096 * 4 = 16384). They are
// independent numbers, and rocketnet_server_socket_process_direct_packet used to
// bound itself with the QUEUE size while writing into the ARRAYS — a remote
// overflow of 384 entries, whose tail wrote attacker-timed pointers past the
// end of rocketnet_server_socket_t for the caller to dereference.
//
// The guard now uses MAX_PROCESS_PACKETS, so it tracks the arrays it protects
// automatically and the class cannot come back by editing one number.
//
// The remaining consequence is capacity, not safety: because the queue can
// stage more than the arrays hold, a single tick that delivers more than 16000
// packets to one server DROPS the excess. That is correct behaviour under
// flood, and at the design load (1000 clients * 16 packets) it is exactly the
// budget. If you would rather never drop, raise MAX_CLIENTS or
// RECEIVE_PACKETS_PER_CLIENT so the arrays cover a full queue drain.

#define ROCKETNET_NUM_CLIENT_PACKETS                                      1024

#define ROCKETNET_DIRECT_TIMEOUT                                          30.0

#define ROCKETNET_REPLAY_PROTECTION_BUFFER_SIZE                           1024

// The sender's delivery bookkeeping, per the standard's _Acks_/Processing:
// retained state for at least one second of the transport's own maximum
// send rate -- the send rings cap a connection at 64 packets per frame,
// which at 60Hz is 3840 per second, so 4096 covers the second -- sized the
// same way as the replay window and for the same reason. The received ring
// and rtt history follow the space reference's endpoint sizing.

#define ROCKETNET_SENT_PACKET_BUFFER_SIZE                                 4096
#define ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE                             2048
#define ROCKETNET_RTT_HISTORY_SIZE                                        2048

// the ack debt threshold from the standard's timing table: a standalone ack
// goes out at this many eliciting packets received since the last block sent

#define ROCKETNET_ACK_DEBT_THRESHOLD                                      64

// the per-client statistics recompute runs round-robin, a sixth of the
// slots per update, every slot covered -- the space reference's amortization

#define ROCKETNET_CLIENT_STATS_UPDATE_PHASES                              6

#define ROCKETNET_PACKET_LOSS_TRACKER_HISTORY                             1024
#define ROCKETNET_PACKET_LOSS_TRACKER_SAFETY                                30
#define ROCKETNET_SECONDS_BETWEEN_PACKET_LOSS_UPDATES                      0.1

#define ROCKETNET_IPV4_HEADER_BYTES                                         20
#define ROCKETNET_UDP_HEADER_BYTES                                           8
#define ROCKETNET_HEADER_BYTES                                              18

#define ROCKETNET_SOCKET_SEND_BUFFER_SIZE                              1000000
#define ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE                           1000000

#define ROCKETNET_VALUE_TRACKER_HISTORY                                   1024

#endif // #ifndef ROCKETNET_CONSTANTS_H
