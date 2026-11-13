/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_URING_RECEIVE_QUEUE_H
#define ROCKETNET_SDK_ROCKETNET_URING_RECEIVE_QUEUE_H

// The io_uring server's per-queue receive: one queue's multishot RECVMSG over
// its provided buffer ring, the completion handling, the stateless prefilter
// and the queue's receive bank, run on the queue's worker thread; main only
// drains the bank. The worker, its thread and its ring are the transport's
// (rocketnet_server_uring_linux.cpp); this module never submits, waits or
// peeks: the worker hands it a submission entry to arm and each receive
// completion to handle.
//
// The bank is a ring of slots, single producer (the worker) and single
// consumer (main), with two 64-bit indexes that never wrap:
//
//   tail  the worker's: every slot below it is whole. The worker writes a
//         slot at tail % slots, then stores tail + 1 (release).
//   head  main's: every slot below it main has finished reading. Main stores
//         it (release) at the start of each drain, the end of the slots the
//         drain before exposed.
//
// The worker writes a slot only while tail - head < slots, reading head
// (acquire) again only when its last read says the bank is full, so it never
// writes a slot main has not finished reading. A datagram that finds the bank
// full is dropped and counted (ROCKETNET_RECEIVE_DROP_BANK_FULL): the worker
// never blocks and never overwrites. A drain exposes the slots from head to
// the tail it reads (acquire), at most max of them; they stay valid, and the
// worker writes none of them, until main's next drain.
//
// Every datagram a completion carries is either banked or counted by name:
// the transport's own refusals in this queue's counters, the prefilter's and
// the full bank's in its drop tally (sdk/rocketnet_receive_prefilter.h). The
// datagrams the kernel dropped at the socket before io_uring saw them (its
// receive buffer full, RcvbufErrors) are counted from the SO_RXQ_OVFL control
// message when the transport set that option on the socket: each receive
// carries the socket's drop count when its datagram was queued, so a drop is
// counted at the next datagram the socket delivers.
//
// A multishot that ends (a completion without IORING_CQE_F_MORE: ENOBUFS, a
// cancel, an error) leaves the queue needing an arm, which the worker's next
// pass gives it unless the queue is stopping; an arm that finds no
// submission entry is counted and stays needed.
//
// proofs/tla/RNUringQueueReceive.tla models the handoff between the worker
// and main and the multishot's life; proofs/tla/RNUringQueueReceiveLayout.tla
// models the index arithmetic of the slots and the provided buffer ring at
// capacity 3 and 4. Each function cites the actions it implements.
//
// Linux with kernel uapi headers that have multishot receive
// (IORING_RECV_MULTISHOT); the arm and the buffer ring registration also need
// liburing (ROCKETNET_IO_URING). Elsewhere the functions exist and create
// refuses.

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_receive_prefilter.h"
#include "rocketnet_wire.h"

#include <atomic>
#include <stdint.h>

#if defined( __linux__ )
#include <sys/socket.h>
#endif // #if defined( __linux__ )

struct io_uring;
struct io_uring_sqe;
struct io_uring_buf_ring;

// the kernel's limit on a provided buffer ring's entries
#define ROCKETNET_URING_RECEIVE_MAX_BUFFERS                              32768

// the most slots one queue's bank holds
#define ROCKETNET_URING_RECEIVE_MAX_SLOTS                          ( 1 << 20 )

// the most datagrams taken from one coalesced receive; each one past them is
// counted in DROPPED_OVERFLOW (the gso transport's bound, the kernel's
// UDP_MAX_SEGMENTS)
#define ROCKETNET_URING_RECEIVE_MAX_SEGMENTS                                64

// the control area the multishot reserves in each buffer: one UDP_GRO cmsg
// (an int) and one SO_RXQ_OVFL cmsg (a uint32)
#define ROCKETNET_URING_RECEIVE_CONTROL_BYTES          ( 2 * 32 )

// a queue's counters, cumulative for its life; indexes into the array
// rocketnet_uring_receive_queue_counters fills. One writer, the worker; main
// reads them at any time. Every datagram a completion carries is in BANKED,
// in one DROPPED_* below or in the drop tally.

#define ROCKETNET_URING_RECEIVE_COUNTER_BANKED                               0      // datagrams banked for main (the transport's PACKETS_RECEIVED)
#define ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE                         1      // a datagram outside [server min, receive ceiling], truncated in its buffer, or a short buffer
#define ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE                    2      // the part of DROPPED_SIZE above the receive ceiling or truncated
#define ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE                       3      // a source address the boundary could not convert, or a name truncated
#define ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_NO_BUFFER                    4      // a completion with res >= 0 that names no provided buffer of the queue
#define ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_OVERFLOW                     5      // datagrams of one coalesced receive past ROCKETNET_URING_RECEIVE_MAX_SEGMENTS
#define ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET                       6      // datagrams the kernel dropped at the socket (SO_RXQ_OVFL: RcvbufErrors and the rest of the socket's drops)
#define ROCKETNET_URING_RECEIVE_COUNTER_NO_BUFFERS                           7      // the multishot ended because the provided buffer ring was empty (ENOBUFS)
#define ROCKETNET_URING_RECEIVE_COUNTER_MULTISHOT_ENDED                      8      // completions without IORING_CQE_F_MORE, whatever their result
#define ROCKETNET_URING_RECEIVE_COUNTER_REARMS                               9      // arms after the first: the multishot armed again after it ended
#define ROCKETNET_URING_RECEIVE_COUNTER_REARM_FAILED                        10      // an arm that had no submission entry (the queue still needs one)
#define ROCKETNET_URING_RECEIVE_COUNTER_ERRORS                              11      // a completion with a negative result other than ENOBUFS or ECANCELED (the kernel refused the receive)
#define ROCKETNET_URING_RECEIVE_COUNTER_GRO_BATCHES                         12      // buffers that held more than one datagram (a UDP_GRO coalesced receive)
#define ROCKETNET_URING_RECEIVE_COUNTER_GRO_CMSG_FAULTS                     13      // a receive whose control area was truncated or malformed: taken as one datagram
#define ROCKETNET_URING_RECEIVE_NUM_COUNTERS                                14

struct rocketnet_uring_receive_queue_config_t
{
    int queue_index;                        // the queue's index: its completions' tag (ROCKETNET_URING_TAG_RECV, queue_index, 0)
    int fd;                                 // the queue's socket
    int bgid;                               // the provided buffer ring's group id
    int buffers;                            // provided buffers: a power of two, 1..ROCKETNET_URING_RECEIVE_MAX_BUFFERS
    int buffer_bytes;                       // bytes per buffer: at least the recvmsg header, the name, the control area and ROCKETNET_RECEIVE_CEILING
    int slots;                              // the bank's slots, 1..ROCKETNET_URING_RECEIVE_MAX_SLOTS
    bool ipv6;                              // the socket is ipv6 (dual stack)
    bool gro;                               // UDP_GRO is on: a buffer may hold a coalesced receive
    bool socket_drops;                      // SO_RXQ_OVFL is on: each receive carries the socket's drop count
    const uint8_t * packet_filter;          // the server's packet filter value, ROCKETNET_FILTER_VALUE_BYTES, copied at create
};

struct rocketnet_uring_receive_slot_t
{
    struct rocketnet_address_t from;
    int packet_bytes;
};

struct rocketnet_uring_receive_queue_t
{
    // create's, read-only after it

    void * context;
    struct io_uring * ring;                 // NULL: a fake buffer ring in memory, never registered (the unit suite and the trace walk)
    int queue_index;
    int fd;
    int bgid;
    int buffers;
    int buffer_bytes;
    int slots;
    bool ipv6;
    bool socket_drops;
    bool buf_ring_inverted_resv;            // registered through the inverted resv check (rocketnet_uring_receive_buf_ring_setup)
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES];
    struct io_uring_buf_ring * buf_ring;
#if defined( __linux__ )
    struct msghdr receive_message;          // the multishot's msghdr: the name area, and the control area when GRO or SO_RXQ_OVFL is on
#endif // #if defined( __linux__ )
    struct rocketnet_uring_receive_slot_t * slot;      // slots records
    uint8_t * slot_data;                    // slots * ROCKETNET_FRAME_STRIDE bytes, 16-aligned: a datagram's wire bytes from offset 0
    uint8_t * buffer;                       // buffers * buffer_bytes bytes: the provided buffers
    void * memory;

    // the handoff, each index on its own cache line

    alignas( 64 ) std::atomic<uint64_t> tail;       // the worker stores (release) once a slot is whole; main loads (acquire)
    alignas( 64 ) std::atomic<uint64_t> head;       // main stores (release) once it has finished reading; the worker loads (acquire)

    // the worker's

    alignas( 64 ) uint64_t head_seen;       // the head the worker last loaded
    bool armed;                             // a multishot is in the kernel, or its SQE is prepared
    bool ever_armed;                        // an arm after this one is a re-arm
    bool stopping;                          // never arm again
    bool logged_refused;                    // an ERRORS completion is logged once
    uint32_t socket_drops_seen;             // the last SO_RXQ_OVFL count
    std::atomic<uint64_t> counters[ROCKETNET_URING_RECEIVE_NUM_COUNTERS];      // one writer: relaxed load and store
    struct rocketnet_receive_drop_tally_t tally;   // the prefilter's drops and BANK_FULL

    // main's

    alignas( 64 ) uint64_t exposed;         // the end of the slots the last drain exposed
};

// any thread, the worker not running. NULL when a size is outside its range,
// the buffer ring cannot be registered (the error logged) or the allocation
// is refused. With a ring it registers the queue's provided buffer ring on it
// (group config->bgid) and gives it every buffer; with ring NULL the buffer
// ring is plain memory, given every buffer the same way, that nothing
// registers (the unit suite's and the trace walk's fake kernel).

struct rocketnet_uring_receive_queue_t * rocketnet_uring_receive_queue_create( void * context, struct io_uring * ring, const struct rocketnet_uring_receive_queue_config_t * config );

// the worker not running, and the kernel holding none of its buffers (the
// multishot's last completion handled, or the ring torn down): unregisters
// the buffer ring and frees
void rocketnet_uring_receive_queue_destroy( struct rocketnet_uring_receive_queue_t * queue );

// the bytes create allocates, 0 for a config out of range, and the bytes it
// maps outside the allocator (the registered buffer ring, whole pages)
uint64_t rocketnet_uring_receive_queue_allocation_bytes( const struct rocketnet_uring_receive_queue_config_t * config );
uint64_t rocketnet_uring_receive_queue_mapped_bytes( int buffers );

// the worker's

// true when the multishot ended (or was never armed) and the queue is not
// stopping: the worker's next pass arms it (RNUringQueueReceive Arm)
bool rocketnet_uring_receive_queue_needs_arm( const struct rocketnet_uring_receive_queue_t * queue );

// prepares sqe as the queue's multishot RECVMSG over its buffer ring, tagged
// (ROCKETNET_URING_TAG_RECV, queue_index, 0); the worker submits it. sqe NULL
// is the worker's ring with no submission entry: counted REARM_FAILED,
// returns false and the queue still needs an arm. Called only while
// needs_arm.
bool rocketnet_uring_receive_queue_arm( struct rocketnet_uring_receive_queue_t * queue, struct io_uring_sqe * sqe );

// one completion tagged with the queue's RECV tag. res >= 0 with a buffer:
// the datagram, or each datagram of a coalesced receive, is checked (size,
// source), run through the prefilter and banked, or counted by name; then
// the buffer goes back to the ring. Without IORING_CQE_F_MORE the multishot
// ended: ENOBUFS counted NO_BUFFERS, another error but ECANCELED counted
// ERRORS (logged once), and the queue needs an arm unless it is stopping.
// Returns the datagrams banked (RNUringQueueReceive Complete).
int rocketnet_uring_receive_queue_cqe( struct rocketnet_uring_receive_queue_t * queue, int32_t res, uint32_t flags );

// the transport's stop: the queue never needs an arm again. The worker's;
// the cancel of the multishot is the transport's.
void rocketnet_uring_receive_queue_stop( struct rocketnet_uring_receive_queue_t * queue );

// a multishot is in the kernel (or its SQE prepared): its last completion is
// still to come
bool rocketnet_uring_receive_queue_armed( const struct rocketnet_uring_receive_queue_t * queue );

// main's

// releases the slots the last drain exposed (head = that drain's end), then
// exposes the banked datagrams from head, oldest first, at most max (max 0
// only releases): writes each one's source, bytes and wire pointer at [0, n)
// of from, packet_bytes and packet_data and returns n. The pointers stay
// valid, and the worker writes none of their slots, until the next drain.
// Every update calls it on every queue (RNUringQueueReceive Drain).
int rocketnet_uring_receive_queue_drain( struct rocketnet_uring_receive_queue_t * queue, int max, struct rocketnet_address_t * from, int * packet_bytes, uint8_t ** packet_data );

// banked datagrams no drain has exposed yet: left for the next update
uint64_t rocketnet_uring_receive_queue_waiting( const struct rocketnet_uring_receive_queue_t * queue );

// any thread: writes ROCKETNET_URING_RECEIVE_NUM_COUNTERS values
void rocketnet_uring_receive_queue_counters( const struct rocketnet_uring_receive_queue_t * queue, uint64_t * counters );

// any thread: adds the queue's drop tally into totals[ROCKETNET_RECEIVE_NUM_DROPS]
// (rocketnet_receive_drop_tally_sum), for the transport's receive_drop_totals
void rocketnet_uring_receive_queue_drops( const struct rocketnet_uring_receive_queue_t * queue, uint64_t * totals );

#if ROCKETNET_DEV

// the unit suite's and the trace walk's fake kernel, on a queue with no
// kernel ring. take is the kernel taking a buffer: the id at the ring entry
// *head & (buffers - 1), and *head + 1, or -1 when *head is the ring's tail
// (empty). write fills that buffer as the kernel fills one multishot
// receive: the recvmsg header (out_flags its flags: MSG_TRUNC, MSG_CTRUNC),
// name_bytes of name in the name area, a UDP_GRO cmsg of segment_bytes when
// it is positive and a SO_RXQ_OVFL cmsg of socket_drops when it is nonzero
// (each only when the queue reserved its control area), then the payload.
// Returns the completion's res: every byte written past the buffer's start.

int rocketnet_uring_receive_queue_test_take( const struct rocketnet_uring_receive_queue_t * queue, uint16_t * head );

int rocketnet_uring_receive_queue_test_write( struct rocketnet_uring_receive_queue_t * queue, int buffer_id, const void * name, int name_bytes, const uint8_t * payload,
                                              int payload_bytes, int segment_bytes, uint32_t socket_drops, uint32_t out_flags );

#endif // #if ROCKETNET_DEV

// the provided buffer ring's registration, with the fallback for kernels
// whose resv check is inverted (Ubuntu 24.04 6.8.0-136 to 6.8.0-148): the
// transport's availability probe uses them too. The mapped bytes go in the
// direct allocation count. Need liburing; NULL (error -ENOSYS) without it.
struct io_uring_buf_ring * rocketnet_uring_receive_buf_ring_setup( struct io_uring * ring, unsigned int entries, int bgid, bool * inverted_resv, int * error );
void rocketnet_uring_receive_buf_ring_free( struct io_uring * ring, struct io_uring_buf_ring * buf_ring, unsigned int entries, int bgid, bool inverted_resv );

#endif // ROCKETNET_SDK_ROCKETNET_URING_RECEIVE_QUEUE_H
