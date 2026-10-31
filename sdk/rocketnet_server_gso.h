/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_GSO_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_GSO_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"
#include "rocketnet_packet_batch.h"

// The GSO transport: the best non-root path Linux has, per the
// measurements -- an SO_REUSEPORT socket group with one worker thread
// per socket, the kernel's 4-tuple flow hash sharding RECEIVES across them
// with zero shared state between queues, recvmmsg/sendmmsg batching on both
// sides, UDP_SEGMENT (GSO) on send and UDP_GRO on receive so a batch
// traverses the kernel's UDP path once instead of per packet. A dedicated
// transmit worker consumes owned packet banks and rotates batches across the
// group's sockets. Caller/worker handoffs exchange only bank ownership under
// short mutexes; batching, copying and socket calls stay outside the locks.
//
// Every kernel feature is probed at create and degrades gracefully: no
// UDP_SEGMENT means plain sendmmsg batching, no UDP_GRO means plain recvmmsg,
// and the probe results are readable so a deployment knows which tier it got.
// This transport never needs a capability or a privilege.
//
// Honest framing: this is the solid middle tier for
// hosts without XDP -- it recovers the syscall amortization and the
// single-traversal segmentation, not the skb-allocation avoidance. The
// the highest-scale claims belong to the XDP transport that earns them.

struct rocketnet_server_gso_t;

// the transport's geometry, all compile time:

#ifndef ROCKETNET_GSO_DEFAULT_QUEUES
#define ROCKETNET_GSO_DEFAULT_QUEUES                                         4
#endif // #ifndef ROCKETNET_GSO_DEFAULT_QUEUES

#define ROCKETNET_GSO_MAX_QUEUES                                            32

// send staging: one phase's worth of slots, the same budget as the xdp
// transport's send queue. each slot is a frame-stride wire packet.

#define ROCKETNET_GSO_SEND_QUEUE_SIZE                                     4096

// receive staging per queue per buffer: twice the send staging and half
// the process array, so two queues at full tilt cover a whole drain and
// a single flooded queue fills its pair between drains and drops
// counted -- correct behaviour under flood, ample at design load where
// the reuseport hash spreads flows

#define ROCKETNET_GSO_RECV_QUEUE_SIZE                                     8192

ROCKETNET_STATIC_ASSERT( ROCKETNET_GSO_RECV_QUEUE_SIZE >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE,
                         "receive budget twice send: GSO receive queue holds at least twice the send queue" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE,
                         "receive budget twice send: server process batch holds at least twice the GSO send queue" );

// the send staging is one server-wide producer bank whatever the queue
// count, and does not derive from ROCKETNET_MAX_CLIENTS: it stages two
// packets for every client of the default configuration. A server whose
// sends need more in one update spreads them across updates, as the
// transport's send room reports (rocketnet_server_gso_send_room;
// proofs/tla/ServerStopSpread.tla Room).

ROCKETNET_STATIC_ASSERT( ROCKETNET_GSO_SEND_QUEUE_SIZE >= 2 * ROCKETNET_DEFAULT_MAX_CLIENTS,
                         "the GSO producer bank stages two packets for every client of the default configuration" );

// recvmmsg geometry: how many kernel messages one call drains, and how big
// one message's buffer is. with GRO on, one message is a coalesced run of
// same-flow segments up to the kernel's coalescing budget (~64KB), so the
// buffer takes the maximum; without GRO a message is one datagram and the
// same buffer is simply generous.

#define ROCKETNET_GSO_RECV_BATCH                                            64
#define ROCKETNET_GSO_RECV_BUFFER_BYTES                                  65536

// send batching geometry: sendmmsg messages per call, GSO segments per
// super-datagram (the kernel's UDP_MAX_SEGMENTS is 64), and a total-bytes
// cap keeping every super-datagram inside the 16-bit UDP payload space

#define ROCKETNET_GSO_SEND_BATCH                                           128
#define ROCKETNET_GSO_MAX_SEGMENTS                                          64
#define ROCKETNET_GSO_MAX_SUPER_BYTES                                    65000

// feature bits reported by rocketnet_server_gso_features: what the runtime
// probe found and the transport is actually using

#define ROCKETNET_SERVER_GSO_FEATURE_SEGMENT                    (1<<0)      // UDP_SEGMENT accepted: send batches as GSO super-datagrams
#define ROCKETNET_SERVER_GSO_FEATURE_GRO                        (1<<1)      // UDP_GRO enabled: receives arrive coalesced
#define ROCKETNET_SERVER_GSO_FEATURE_REUSEPORT                  (1<<2)      // SO_REUSEPORT group bound: receives shard across queues

// create flags: force a probe result off, so the degraded paths are testable
// on kernels where the real probe would say yes -- the fallback tier must
// never be the untested tier

#define ROCKETNET_SERVER_GSO_DISABLE_SEGMENT                    (1<<0)
#define ROCKETNET_SERVER_GSO_DISABLE_GRO                        (1<<1)

// counters, readable any time from the main thread; indexes into the array
// rocketnet_server_gso_counters fills

#define ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT                    0
#define ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED               1
#define ROCKETNET_SERVER_GSO_COUNTER_SEND_SYSCALLS                  2
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_SYSCALLS               3
#define ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED                   4       // sendmmsg refused and the packets were dropped, counted never resent
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE           5       // segment outside [server min, receive ceiling]
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_OVERFLOW       6       // staging full: the queue could not keep up
#define ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS                      7       // sendmmsg messages that carried a UDP_SEGMENT cmsg
#define ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES                    8       // receive buffers that arrived GRO-coalesced (more than one segment)
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SOURCE         9       // source address family the boundary could not convert
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_ERRORS                10       // recvmmsg answered -1 with an errno other than EAGAIN; the worker goes back to poll
#define ROCKETNET_SERVER_GSO_NUM_COUNTERS                          11

// bind_address is read for family and port and written back when the port
// was zero, the same contract as rocketnet_platform_socket_create. an ipv6
// bind is dual stack: ipv4 senders arrive as native ipv4 addresses,
// normalized exactly once at this boundary per the standard's law.
// num_queues <= 0 selects the default. flags is the DISABLE set above, zero
// for production.

struct rocketnet_server_gso_t * rocketnet_server_gso_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int flags, int * status = NULL );

void rocketnet_server_gso_stop( struct rocketnet_server_gso_t * gso );
bool rocketnet_server_gso_stopped( struct rocketnet_server_gso_t * gso );

// Requires stopped completion. Joins and frees synchronously.
void rocketnet_server_gso_destroy( struct rocketnet_server_gso_t * gso );

#if ROCKETNET_DEV
int rocketnet_server_gso_num_queues( struct rocketnet_server_gso_t * gso );
#endif // #if ROCKETNET_DEV

int rocketnet_server_gso_features( struct rocketnet_server_gso_t * gso );

void rocketnet_server_gso_counters( struct rocketnet_server_gso_t * gso, uint64_t * counters );

// send packets. the phase contract is the transport's: start, finish and
// abort may run on any number of game threads strictly between one
// send_packets call and the next; send_packets try-publishes the main-owned
// bank to a dedicated transmit worker. The worker coalesces same-destination
// equal-size runs and sends outside the handoff lock. A full/contended FIFO
// retains the producer for a later send/receive call; stop transfers the final
// producer after earlier ready banks and completion waits for the worker.
// The frame handed back is a ROCKETNET_FRAME_STRIDE slot: wire
// bytes begin at ROCKETNET_FRAME_WIRE_OFFSET, exactly as the other transports.

uint8_t * rocketnet_server_gso_start_packet( struct rocketnet_server_gso_t * gso, const struct rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_gso_finish_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_gso_abort_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_gso_send_packets( struct rocketnet_server_gso_t * gso );

// the slots start_packet may still take in the producer bank: the one bank
// producers fill, whatever the queue count. Main thread.
int rocketnet_server_gso_send_room( struct rocketnet_server_gso_t * gso );

// true once the producer is empty, no bank waits for the transmit worker and
// the worker has returned every bank it took, its sends finished. Main
// thread; a send mutex held by the worker reads as not idle.
bool rocketnet_server_gso_send_idle( struct rocketnet_server_gso_t * gso );

// Receive workers stage into per-queue producer/ready/main banks off main.
// receive_packets try-swaps ownership and gathers within the caller budget;
// unconsumed tails stay main-owned until a later receive call. process_packets
// exposes stable views valid until the next receive_packets call.

void rocketnet_server_gso_receive_packets( struct rocketnet_server_gso_t * gso );

struct rocketnet_server_socket_process_packets_t * rocketnet_server_gso_process_packets( struct rocketnet_server_gso_t * gso );

#if ROCKETNET_DEV
// development library only. force_create_flags ORs flags (the DISABLE set)
// into every following create. fail_receives makes the next count recvmmsg
// calls on any worker answer -1 with error without reaching the kernel;
// fail_sends does the same for the next count sendmmsg calls of a flush.
void rocketnet_server_gso_test_force_create_flags( int flags );
void rocketnet_server_gso_test_fail_receives( struct rocketnet_server_gso_t * gso, int count, int error );
void rocketnet_server_gso_test_fail_sends( struct rocketnet_server_gso_t * gso, int count, int error );
// Observe publication without consuming it; returns false on lock contention.
bool rocketnet_server_gso_test_receive_ready( struct rocketnet_server_gso_t * gso, int queue_index );
// Main thread: no staged packet, no ready bank, no bank held by the TX worker.
bool rocketnet_server_gso_test_send_idle( struct rocketnet_server_gso_t * gso );
// Deterministic ownership/full/contended/stop and retained-RX-tail regression.
bool rocketnet_server_gso_test_worker_handoff();
// A refused sendmmsg: a full socket drops the rest of the flush, any other
// errno drops the one refused message and sends the rest.
bool rocketnet_server_gso_test_send_refusals();
// One receive drain fed endless discards returns within its read budget.
bool rocketnet_server_gso_test_drain_budget();
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_GSO_H
