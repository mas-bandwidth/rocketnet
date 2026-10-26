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

// A portable or msgx server's send bank holds this many packets per configured
// client (never fewer than ROCKETNET_SERVER_MAX_SEND_PACKETS), as its receive
// banks hold ROCKETNET_RECEIVE_PACKETS_PER_CLIENT. One worker drains a bank
// with one socket call per packet batch, measured at about 18 us per packet on
// macOS at 1000 clients: about 80% of that worker at 45,000 packets a second,
// so a burst backs up for tens of milliseconds and three banks of 1000 ran
// out. A bank of 16 per client holds that backlog: memory is cheap, a packet
// dropped for want of a bank is not.
#define ROCKETNET_SEND_PACKETS_PER_CLIENT                                   16

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
#define ROCKETNET_XDP_FILL_FRAMES                                 ( 4096 * 4 )
#define ROCKETNET_XDP_FILL_RING_SIZE         ( 2 * ROCKETNET_XDP_FILL_FRAMES )
// each queue's UMEM: the send half (ROCKETNET_XDP_NUM_FRAMES / 2 frames from
// offset 0), then exactly the receive frames put on the fill ring at create
#define ROCKETNET_XDP_UMEM_FRAMES     ( ROCKETNET_XDP_NUM_FRAMES / 2 + ROCKETNET_XDP_FILL_FRAMES )
#define ROCKETNET_XDP_SEND_BATCH_SIZE                                       32

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FRAME_SIZE >= ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES +
                         ROCKETNET_UDP_HEADER_BYTES + ROCKETNET_RECEIVE_CEILING,
                         "an XDP frame holds ethernet, the larger IP header, UDP and a full receive-ceiling packet" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FRAME_SIZE == 4096,
                         "the XDP UMEM frame the transport registers is 4096 bytes" );

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_RECV_QUEUE_SIZE >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE,
                         "receive budget twice send: XDP receive queue holds at least twice the send queue" );

// The XDP ring geometry is per queue and does not derive from
// ROCKETNET_MAX_CLIENTS: the TX queue is a hash of the destination
// (rocketnet_server_socket_send_queue), so a server's clients spread over its
// queues, and a build for more clients runs more queues (BUILDING.md,
// "Maximum clients"). The most queues (ROCKETNET_XDP_MAX_QUEUES, 1024) stage
// two packets for every client at any ROCKETNET_MAX_CLIENTS.

// The fill ring holds twice the receive frames the server puts on it. The
// kernel reads the fill ring through a cached batch and publishes its consumer
// index only when that batch is used up (xskq_cons_peek_addr_unchecked,
// net/xdp/xsk_queue.h), so a frame it took stays in a fill ring slot until
// then. With a ring of exactly the frames, the worker cannot hand back a frame
// until the kernel has used its whole batch, and the kernel then finds the fill
// ring empty and drops ("rx fill ring empty"). Twice the frames always has a
// slot for every frame the worker holds (tla/RNXdpReceiveRing.tla,
// PendingReturnedEachPass). The frames fit the RX ring and the worker's
// pending return, so neither can overflow while every frame is out.

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FILL_RING_SIZE >= 2 * ROCKETNET_XDP_FILL_FRAMES,
                         "the fill ring has a slot for every receive frame beyond the kernel's unpublished batch" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FILL_FRAMES <= ROCKETNET_XDP_RECV_QUEUE_SIZE,
                         "every receive frame fits the RX ring and the worker's pending return at once" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FILL_FRAMES <= ROCKETNET_XDP_NUM_FRAMES / 2,
                         "the receive frames come from the receive half of the UMEM" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_UMEM_FRAMES <= ROCKETNET_XDP_NUM_FRAMES,
                         "the UMEM holds the send half and only the receive frames the fill ring is given" );
ROCKETNET_STATIC_ASSERT( (long long) ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE <= 0x7fffffffLL,
                         "a queue's UMEM size in bytes fits the int the transport allocates it with" );

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

// The disconnect packets one server update may seal (tla/ServerSession.tla
// DrainDisconnectBurst, sdk/rocketnet_server_transport.cpp): a disconnect
// ends its slot at once, and a burst this allowance cannot hold whole waits
// for the following updates, so ending every client at once costs each call
// a bounded number of seals, never the population's 10 per client.

#define ROCKETNET_DEFAULT_SERVER_DISCONNECT_PACKETS_PER_UPDATE              64

// The send staging of a transport whose one staging serves every client of
// the server (loopback, IOCP, RIO): two packets for each client the build
// holds, rounded up to a power of two -- 2048 at the default 1000 clients.
// The per-queue rings of the multi-queue transports (xdp, gso, io_uring) do
// not derive from it: a server's clients spread over its queues, and a build
// for more clients runs more queues (BUILDING.md, "Maximum clients").

#define ROCKETNET_SERVER_SEND_STAGING_PACKETS             ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_MAX_CLIENTS )

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_SEND_STAGING_PACKETS >= 2 * ROCKETNET_MAX_CLIENTS,
                         "the server-wide send staging holds two packets for every client" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_SEND_STAGING_PACKETS,
                         "receive budget twice send: per-update receive budget holds at least twice the server-wide send staging" );

// The server's packet lease table. Insert refuses past half the table, so
// one update leases at most 65536 packets: the server-wide send staging rule,
// two packets for every client, holds at any ROCKETNET_MAX_CLIENTS up to the
// 32768 bound, so the table is fixed rather than derived.

#define ROCKETNET_SERVER_PACKET_LEASES                                  131072

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_PACKET_LEASES / 2 >= 2 * ROCKETNET_MAX_CLIENTS,
                         "one update leases at least two packets for every client" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_PACKET_LEASES / 2 >= ROCKETNET_SERVER_SEND_STAGING_PACKETS,
                         "one update leases every packet the server-wide send staging holds" );

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
// packet is known lost once the receiver has acknowledged a sequence at least
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

// ROCKETNET_IPV4_HEADER_BYTES and ROCKETNET_UDP_HEADER_BYTES live in
// rocketnet_wire.h, beside the MTU they derive the packet size from

#define ROCKETNET_SOCKET_SEND_BUFFER_SIZE                              1000000
#define ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE                           1000000

// A server socket's receive request holds one update's inbound packets at
// max_clients (rocketnet_server_receive_buffer_bytes, USAGE.md "Receive
// buffer sizing"): max_clients x the per-client receive rate x the longest
// update interval x the margin, never below ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE
// and capped at INT_MAX. The longest interval is the slowest supported update
// rate; the margin covers the kernel's per-packet accounting, which charges
// more than the packet bytes.
#define ROCKETNET_SERVER_RECEIVE_MIN_UPDATE_HZ                              10
#define ROCKETNET_SERVER_RECEIVE_BUFFER_MARGIN                               2

#define ROCKETNET_VALUE_TRACKER_HISTORY                                   1024

#endif // #ifndef ROCKETNET_CONSTANTS_H
