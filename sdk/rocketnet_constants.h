/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CONSTANTS_H
#define ROCKETNET_SDK_ROCKETNET_CONSTANTS_H

#include "rocketnet_internal.h"
#include "rocketnet_history.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"

// The packet size limits, each asserted against its derivation so a change
// to the MTU in rocketnet_wire.h is one line and a failed assert names the
// limit that no longer follows (standard/TRANSPORT.md t8.11). The public
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
                                 ROCKETNET_UDP_HEADER_BYTES + ROCKETNET_MAX_PACKET_BYTES ==
                             1514,
                         "one maximum IPv6 packet is exactly one 1500-MTU Ethernet frame" );

#define ROCKETNET_FRAME_SIZE                                              2048

ROCKETNET_STATIC_ASSERT( ROCKETNET_FRAME_SIZE >= 16 + ROCKETNET_RECEIVE_CEILING,
                         "a send frame holds the 16-byte payload alignment slack and a full receive-ceiling packet" );

#define ROCKETNET_RECEIVE_PACKETS_PER_CLIENT                                16

// The most message packets one connection writes in one update: the top of
// the message config's packets_per_update range (rocketnet_internal.h).
#define ROCKETNET_CONNECTION_MAX_PACKETS_PER_UPDATE                         1024

// A portable or msgx server's send bank holds this many packets per configured
// client (never fewer than rocketnet_server_send_batch_packets), as its receive
// banks hold ROCKETNET_RECEIVE_PACKETS_PER_CLIENT. One worker drains a bank
// with one socket call per packet batch; at about 18 us per packet on macOS at
// 1000 clients, about 80% of that worker at 45,000 packets a second, so a
// burst backs up for tens of milliseconds and three banks of 1000 ran out.
// Those figures are not recorded in docs/runs/, so this size is a chosen
// design value, not a measured result. A bank of 16 per client is chosen to
// hold that backlog: memory is cheap, a packet dropped for want of a bank is
// not.
#define ROCKETNET_SEND_PACKETS_PER_CLIENT                                   16

// The XDP frame size is defined on every platform: the frame codec in
// rocketnet_xdp_frame.h (rocketnet_xdp_receive_descriptor) and its unit cases
// are portable and build in ./bin/test everywhere. The transport's own static
// assert below and in rocketnet_server_socket_linux.cpp pin it where XDP is on.
#define ROCKETNET_XDP_FRAME_SIZE                                          4096  // IMPORTANT: This *must* be 4096 or you will get packet corruption on read!

#if ROCKETNET_XDP

#define ROCKETNET_XDP_NUM_BUFFERS                                            3
#define ROCKETNET_XDP_NUM_FRAMES                                        131072
#define ROCKETNET_XDP_SEND_QUEUE_SIZE                             ( 4096 * 4 )
#define ROCKETNET_XDP_RECV_QUEUE_SIZE                             ( 8192 * 4 )
#define ROCKETNET_XDP_FILL_FRAMES                                 ( 8192 * 4 )
#define ROCKETNET_XDP_FILL_RING_SIZE         ( 2 * ROCKETNET_XDP_FILL_FRAMES )

// each queue's UMEM: the send half (ROCKETNET_XDP_NUM_FRAMES / 2 frames from
// offset 0), then exactly the receive frames put on the fill ring at create
#define ROCKETNET_XDP_UMEM_FRAMES     ( ROCKETNET_XDP_NUM_FRAMES / 2 + ROCKETNET_XDP_FILL_FRAMES )
#define ROCKETNET_XDP_SEND_BATCH_SIZE                                       32

// The send queue is the most packets one queue starts in one update (one
// flush a frame), so it caps a server's send rate at queues x send queue x
// update rate. The owner's shape is 2 XDP threads at 10 Gb/s, 60 Hz
// ("narrow beats wide EVERYWHERE for 10GBPS"): 10 GbE line rate at 1304
// bytes a packet on the wire (a 1200-byte payload, as the measured rows count
// it) is 958.6k packets a second, 7989 starts per queue per update. The send
// queue holds twice that, so a burst update of twice the average still
// starts whole. At 4096 the two queues capped at 491.5k a second, half the
// line, measured (482k).
#define ROCKETNET_XDP_LINE_RATE_QUEUES                                       2
#define ROCKETNET_XDP_LINE_RATE_UPDATE_HZ                                   60
#define ROCKETNET_XDP_LINE_RATE_BITS_PER_SECOND               10000000000ULL
#define ROCKETNET_XDP_LINE_RATE_WIRE_BYTES                                1304

ROCKETNET_STATIC_ASSERT( (unsigned long long) ROCKETNET_XDP_LINE_RATE_QUEUES *(unsigned long long) ROCKETNET_XDP_SEND_QUEUE_SIZE * ROCKETNET_XDP_LINE_RATE_UPDATE_HZ * ROCKETNET_XDP_LINE_RATE_WIRE_BYTES * 8 >=
                             2 * ROCKETNET_XDP_LINE_RATE_BITS_PER_SECOND,
                         "two XDP queues at 60 Hz start twice 10 GbE line rate: the send queue never caps the owner's shape" );

// XDP's own receive sizing: the process batch XDP drains its queues into
// (the socket's process view) holds at least twice one queue's send queue at
// every sizing, whatever the shared per-client receive budget
// (ROCKETNET_RECEIVE_PACKETS_PER_CLIENT) is, which the other transports read
// and XDP never moves. rocketnet_server_xdp_receive_packets_per_update below
// is the larger of the shared budget and this minimum.
#define ROCKETNET_XDP_MIN_RECEIVE_PACKETS_PER_UPDATE            ( 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE )

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_FRAME_SIZE >= ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES +
                                                         4 + ROCKETNET_UDP_HEADER_BYTES + ROCKETNET_RECEIVE_CEILING,
                         "an XDP frame holds ethernet, one 4-byte 802.1Q tag, the larger IP header, UDP and a full receive-ceiling packet" );
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
// slot for every frame the worker holds (proofs/tla/RNXdpReceiveRing.tla,
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

// Per-server sizing. Every server buffer that scales with clients is sized at
// create from the configured max_clients, never from the build's
// ROCKETNET_MAX_CLIENTS, which is only the most a config may ask for: a
// 1000-client server holds the same memory in the default build and in a
// -DROCKETNET_MAX_CLIENTS=10000 build. Each owner allocates its buffers once,
// at create, and its memory plan counts the same bytes from the same
// functions. The one exception is the server packet lease table
// (ROCKETNET_SERVER_PACKET_LEASES below): 131072 64-byte entries, 8 MiB,
// fixed in every build because it already holds two packets per client up to
// the 32768 build ceiling.
//
// The sizing count is the configured max_clients itself, any count from 1 to
// ROCKETNET_MAX_CLIENTS: there is no player-count floor. A structure whose
// own arithmetic needs a minimum keeps it as a count of its own slots, stated
// beside it with its reason (the portable send batch below, the receive budget
// held against it, the IOCP process view, the XDP reply MAC table), and each
// relation between them is proved for every count from 1 up, not at one count.

static inline int rocketnet_server_sized_clients( int max_clients )
{
    if ( max_clients > ROCKETNET_MAX_CLIENTS )
    {
        return ROCKETNET_MAX_CLIENTS;
    }
    return max_clients > 1 ? max_clients : 1;
}

// the smallest power of two at least n, for 1 <= n <= 1 << 30
static inline int rocketnet_power_of_two_at_least( int n )
{
    int p = 1;
    while ( p < n )
    {
        p <<= 1;
    }
    return p;
}

// Portable and msgx buffered batch sizes (the payload ring and the control
// batch): one packet per configured client, and never fewer than
// ROCKETNET_SERVER_MIN_SEND_PACKETS slots. That minimum is one connection's
// default update (packets_per_update, 256, sdk/rocketnet_internal.h): a
// single client can send its default update in one server update. It was the
// ring's fixed size before the ring scaled with clients. The transport's send
// capacity is max_clients with the same 256-slot minimum, so it never passes
// the ring.
#define ROCKETNET_SERVER_MIN_SEND_PACKETS                                  256
#define ROCKETNET_SERVER_MAX_RECEIVE_PACKETS                               256

static inline int rocketnet_server_send_batch_packets( int max_clients )
{
    const int sized = rocketnet_server_sized_clients( max_clients );
    return sized > ROCKETNET_SERVER_MIN_SEND_PACKETS ? sized : ROCKETNET_SERVER_MIN_SEND_PACKETS;
}

// One update reads at most ROCKETNET_RECEIVE_PACKETS_PER_CLIENT packets per
// configured client (16 * 1000 = 16000 at the default), bounding its work,
// and never fewer than ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE: twice
// the send batch's minimum, so the receive budget holds twice the portable
// send batch at every count (the twice-send rule). Every receive batch in one
// update (the socket process batch, the per-update receive budget and the
// payload batch) shares this one bound and one storage cap.
#define ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE   ( 2 * ROCKETNET_SERVER_MIN_SEND_PACKETS )

static inline int rocketnet_server_receive_packets_per_update( int max_clients )
{
    const int budget = ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * rocketnet_server_sized_clients( max_clients );
    return budget > ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE ? budget : ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE;
}

// receive >= 2 x send at every count n from 1: where the send batch is n
// (n >= 256) the budget is at least 16n >= 2n; where it is its 256 minimum the
// budget is at least its own minimum, 512.
ROCKETNET_STATIC_ASSERT( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT >= 2,
                         "receive budget twice send: per-update receive budget holds at least twice the portable send batch" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MIN_SEND_PACKETS,
                         "receive budget twice send: the receive budget's minimum holds twice the send batch's minimum" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE <= ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * 512 &&
                             ROCKETNET_SERVER_MIN_SEND_PACKETS <= 512,
                         "neither minimum reaches a server of 512 or more clients: their sizes are the per-client ones" );

#if ROCKETNET_XDP
// The XDP process batch: the shared per-update budget, raised to XDP's own
// minimum so it holds twice one queue's send queue at every count (the shared
// budget is below it up to 2048 clients). At 1000 clients it is
// 32768 against the shared 16000. Only the XDP socket's process view uses it;
// the payload batch and every other transport's batches stay on the shared
// budget, so a drain past the payload batch counts as a full payload batch.
static inline int rocketnet_server_xdp_receive_packets_per_update( int max_clients )
{
    const int shared = rocketnet_server_receive_packets_per_update( max_clients );
    return shared > ROCKETNET_XDP_MIN_RECEIVE_PACKETS_PER_UPDATE ? shared : ROCKETNET_XDP_MIN_RECEIVE_PACKETS_PER_UPDATE;
}

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_MIN_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE,
                         "receive budget twice send: the XDP process batch holds at least twice the XDP send queue" );
#endif // #if ROCKETNET_XDP

// The disconnect packets one server update may seal (proofs/tla/ServerSession.tla
// SealWaiting, sdk/rocketnet_server_transport.cpp): a disconnect
// ends its slot at once, and a burst this allowance cannot hold whole waits
// for the following updates, so ending every client at once costs each call
// a bounded number of seals, never the population's 10 per client.

#define ROCKETNET_DEFAULT_SERVER_DISCONNECT_PACKETS_PER_UPDATE              64

// The send staging of a transport whose one staging serves every client of
// the server (loopback, IOCP, RIO): two packets for each sized client,
// rounded up to a power of two -- 2048 at 1000 clients. The per-queue rings of
// the multi-queue transports (xdp, gso, io_uring) do not derive from it: a
// server's clients spread over its queues, and a server for more clients runs
// more queues (BUILDING.md, "Maximum clients"). The receive budget holds at
// least twice it: 16 per client against at most 4.

static inline int rocketnet_server_send_staging_packets( int max_clients )
{
    return rocketnet_power_of_two_at_least( 2 * rocketnet_server_sized_clients( max_clients ) );
}

ROCKETNET_STATIC_ASSERT( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT >= 2 * 4,
                         "receive budget twice send: per-update receive budget holds at least twice the server-wide send staging" );

// The server's packet lease table. Insert refuses past half the table, so
// one update leases at most 65536 packets: the server-wide send staging rule,
// two packets for every client, holds at any ROCKETNET_MAX_CLIENTS up to the
// 32768 bound, so the table is fixed rather than derived.

#define ROCKETNET_SERVER_PACKET_LEASES                                  131072

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_PACKET_LEASES / 2 >= 2 * ROCKETNET_MAX_CLIENTS,
                         "one update leases at least two packets for every client" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_PACKET_LEASES / 2 >= ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_MAX_CLIENTS ),
                         "one update leases every packet the largest server-wide send staging holds" );

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

// the most payloads one server update can deliver is the same per-update
// receive bound above: what the receive path can hand over in a single drain,
// so a burst is never silently discarded after its sequence has already been
// consumed. internal -- the public surface iterates payloads by index and
// never sees a capacity.

// NOTE the relationship between these two. The process_packets arrays are
// sized by rocketnet_server_receive_packets_per_update (16 * 1000 = 16000 at
// 1000 clients), but the XDP path fills them while draining a queue whose
// staging buffer holds ROCKETNET_XDP_RECV_QUEUE_SIZE entries (8192 * 4 =
// 32768). On XDP the arrays are sized by
// rocketnet_server_xdp_receive_packets_per_update, at least twice the send
// queue (32768). They are independent numbers, and the drain guard MUST bound by the
// process batch's capacity -- the arrays it protects -- never the queue size,
// or the tail of a full queue writes past the arrays.
//
// The consequence of the correct guard is capacity, not safety: because the
// queue can stage more than the arrays hold, a single tick that delivers
// more than the budget to one server DROPS the excess. That is correct
// behaviour under flood, and at the design load (16 packets per client) it
// is exactly the budget. If you would rather never drop, raise max_clients or
// RECEIVE_PACKETS_PER_CLIENT so the arrays cover a full queue drain.

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
// proofs/tla/RNSentHistory.tla models the classification it drives.

#define ROCKETNET_LOSS_PACKET_THRESHOLD                                   3

// the AEAD integrity limit from the standard's _Replay Protection_: each end
// counts the sealed packets that fail to open on a connection, across all of
// that connection's keys, and disconnects it on reaching this many. 2^36 is
// ChaCha20-Poly1305's forgery bound (RFC 9001 section 6.6, RFC 9147 section 4.5.3).

#define ROCKETNET_INTEGRITY_LIMIT                               ( 1ULL << 36 )

// any accepted eliciting packet makes an ack due on the next update

#define ROCKETNET_ACK_DEBT_THRESHOLD                                       1

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

#endif // ROCKETNET_SDK_ROCKETNET_CONSTANTS_H
