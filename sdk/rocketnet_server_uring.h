/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_URING_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_URING_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"
#include "rocketnet_packet_batch.h"
#include "rocketnet_uring_pin.h"
#include "rocketnet_network_threads.h"
#include "rocketnet_uring_send_queue.h"

// The io_uring server transport, the XDP shape: one worker thread per queue,
// each owning its own io_uring and doing all of its queue's kernel-facing
// send and receive. An SO_REUSEPORT socket group (bound the way the gso
// transport binds its group) shards receives across queues.
//
// Send: start_packet hashes the destination to a queue (the XDP hash,
// rocketnet_send_queue_of) and claims a slot of that queue's producer bank
// of ROCKETNET_URING_SEND_QUEUE_SIZE (rocketnet_uring_send_queue.h);
// send_packets, with the game threads joined, publishes each queue's producer to its
// worker and wakes it (rocketnet_uring_queue_send.h). The worker groups a
// bank's packets by destination in staged order (rocketnet_send_gather.h)
// and sends each run as one SENDMSG, segmented with UDP_SEGMENT when the
// create probe found it; a bank goes back to the game threads only after the last
// completion of its sends (rocketnet_uring_queue_worker.h).
//
// Receive: each worker keeps its socket's multishot RECVMSG armed over a
// provided buffer ring, splits UDP_GRO coalesced receives, runs the
// server's stateless prefilter and banks what passes in its queue's receive
// bank (rocketnet_uring_receive_queue.h); receive_packets only drains the
// banks into the process view. Every datagram a completion carries is
// banked or counted by name.
//
// Main never touches a ring. The models are the modules':
// RNUringQueueBankLayout, RNUringQueueSend, RNSendGatherLayout,
// RNUringQueueWorker, RNUringQueueReceive and RNUringQueueReceiveLayout,
// each bound to its code by a trace check.
//
// The transport is built only when the library is configured with liburing
// (ROCKETNET_IO_URING, Linux). Without it, create logs the refusal and
// returns NULL, and rocketnet_server_uring_available() is false.

struct rocketnet_server_uring_t;

// the transport's geometry, all compile time:

// a queue per worker: the counts are the server's num_io_uring_workers
// (sdk/rocketnet_network_threads.h)
#define ROCKETNET_URING_DEFAULT_QUEUES                                       ROCKETNET_IO_URING_DEFAULT_WORKERS
#define ROCKETNET_URING_MAX_QUEUES                                           ROCKETNET_IO_URING_MAX_WORKERS

// each queue's ring: submission queue entries; the completion queue is
// twice this

#define ROCKETNET_URING_ENTRIES                                           4096

// an upper bound of the kernel's mappings of one ring of this many submission
// entries, for the memory plan (the completion queue is twice that): the
// submission entries 64 bytes each, the submission ring's header and index
// array 4 bytes an entry, the completion ring's header and 16-byte entries,
// each mapping rounded up to whole pages plus a page of header room
// (about 0.39 MB for ROCKETNET_URING_ENTRIES)

static inline uint64_t rocketnet_server_uring_ring_mapped_bound( int entries )
{
    if ( entries < 1 )
    {
        return 0;
    }
    const uint64_t page = 4096;
    const uint64_t sqes = (uint64_t) entries * 64;
    const uint64_t sq_ring = (uint64_t) entries * 4 + 128;
    const uint64_t cq_ring = (uint64_t) entries * 2 * 16 + 128;
    return ( ( sqes + page - 1 ) / page + ( sq_ring + page - 1 ) / page + ( cq_ring + page - 1 ) / page + 3 ) * page;
}

// send staging: per queue, a producer bank of this many frame-stride slots
// (and two more banks the worker sends from while the game threads use the
// producer). A destination always stages in its own queue, so the fullest
// queue bounds what one update may stage to any destination.

#define ROCKETNET_URING_SEND_QUEUE_SIZE                ROCKETNET_URING_SEND_BANK_SLOTS

// a queue's send slots are sized by rate at create: twice the queue's share
// of one frame's packets at the bar (1000 players, 10 Gb/s, 60 Hz: 1.04M
// packets/s is 17.3 a player a frame, so 18), at least
// ROCKETNET_URING_SEND_QUEUE_SIZE and at most the module's
// ROCKETNET_URING_SEND_MAX_BANK_SLOTS. Twice covers the destination hash's
// uneven split over the queues; it is not measured past the bar row. The
// floor alone held only at 8 queues: at 2 queues 4096 slots a queue a frame
// is 491,520 packets/s at 60 Hz, half the line rate.

#define ROCKETNET_URING_SEND_PACKETS_PER_CLIENT_FRAME                        18

static inline int rocketnet_server_uring_send_slots( int max_clients, int num_queues )
{
    if ( max_clients < 1 || num_queues < 1 )
    {
        return ROCKETNET_URING_SEND_QUEUE_SIZE;
    }
    const int64_t frame = (int64_t) ROCKETNET_URING_SEND_PACKETS_PER_CLIENT_FRAME * max_clients;
    const int64_t slots = ( 2 * frame + num_queues - 1 ) / num_queues;
    if ( slots < ROCKETNET_URING_SEND_QUEUE_SIZE )
    {
        return ROCKETNET_URING_SEND_QUEUE_SIZE;
    }
    if ( slots > ROCKETNET_URING_SEND_MAX_BANK_SLOTS )
    {
        return ROCKETNET_URING_SEND_MAX_BANK_SLOTS;
    }
    return (int) slots;
}

// runs a worker keeps in the kernel at once: the ring less room for the
// receive arm, the wake read, the poll and their cancels

#define ROCKETNET_URING_MAX_RUNS_IN_KERNEL                ( ROCKETNET_URING_ENTRIES - 16 )

ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_MAX_QUEUES <= ROCKETNET_URING_SEND_MAX_QUEUES, "every io_uring queue has its send banks" );

// receive: per queue a provided buffer ring of this many buffers of this many
// bytes. one buffer holds the multishot recvmsg header, the reserved name
// area, the control area and the payload, which with UDP_GRO is one
// coalesced receive of up to 64 KiB (the 16-bit UDP payload), so a buffer is
// 66 KiB: 4096 * 66 KiB = 264 MiB per queue, all allocated at create
// whether or not the UDP_GRO probe is granted (a page is resident only once
// the kernel writes it).

#define ROCKETNET_URING_RECV_BUFFERS                                      4096
#define ROCKETNET_URING_RECV_BUFFER_BYTES                          ( 67584 ) // 66 KiB

// the floor of a receive bank (not a send budget): each queue's receive bank,
// and the process view, hold at least this many datagrams whatever the client
// count (2048 and two completions a ring entry)

#define ROCKETNET_URING_RECEIVE_BANK_FLOOR    ( 2048 + ROCKETNET_URING_ENTRIES * 2 )

// the most datagrams one coalesced receive is taken with (NIC GRO's limit,
// the gso transport's bound); each datagram past them is counted in
// RECEIVE_DROPPED_OVERFLOW

#define ROCKETNET_URING_MAX_SEGMENTS                                        64

// a receive bank holds one update's receive budget for the configured
// clients (rocketnet_server_receive_packets_per_update), never less than the
// floor. Each queue's bank is this size, so one queue may take the whole
// update's receives; the process view holds this many, and what one update
// does not take waits in its queue's bank for the next.

static inline int rocketnet_server_uring_bank_packets( int max_clients )
{
    const int budget = rocketnet_server_receive_packets_per_update( max_clients );
    return budget > ROCKETNET_URING_RECEIVE_BANK_FLOOR ? budget : ROCKETNET_URING_RECEIVE_BANK_FLOOR;
}

ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_RECEIVE_BANK_FLOOR > ROCKETNET_URING_MAX_SEGMENTS,
                         "the receive bank holds more than one coalesced receive" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_SEND_QUEUE_SIZE >= 2 * ROCKETNET_DEFAULT_MAX_CLIENTS,
                         "one io_uring queue stages two packets for every client of the default configuration" );

// feature bits reported by rocketnet_server_uring_features: what the create
// probe found and the transport is using now

#define ROCKETNET_SERVER_URING_FEATURE_SEGMENT                          (1<<0)      // UDP_SEGMENT accepted: send runs go out as segmented SENDMSGs
#define ROCKETNET_SERVER_URING_FEATURE_GRO                              (1<<1)      // UDP_GRO enabled on every socket: a receive buffer may hold a coalesced run, split by its stride

// create flags: force a probe result off, so the plain tier is testable on
// kernels where the real probe would say yes (ROCKETNET_SERVER_GSO_DISABLE_SEGMENT
// and ROCKETNET_SERVER_GSO_DISABLE_GRO for the gso transport)

#define ROCKETNET_SERVER_URING_DISABLE_SEGMENT                          (1<<0)
#define ROCKETNET_SERVER_URING_DISABLE_GRO                              (1<<1)

// true when the library was built with liburing and this kernel accepts the
// ring, the opcodes and the provided buffer ring. it does not cover multishot
// receive (Linux 6.0), which the transport also needs and create cannot
// detect: an older kernel refuses each arm, logged once. the probe runs once
// and its answer is kept.

bool rocketnet_server_uring_available();

// the bytes create allocates for a queue count, 0 for a count out of range

uint64_t rocketnet_server_uring_allocation_bytes( int queues, int max_clients );

// bind_address is read for family and port and written back when the port
// was zero, the contract of rocketnet_platform_socket_create. an ipv6 bind is
// dual stack: ipv4 senders arrive as native ipv4 addresses. num_queues <= 0
// selects the default; a count past ROCKETNET_URING_MAX_QUEUES refuses.
// max_clients is the server's configured client count: it sizes the receive
// banks, the receive view and each socket's receive buffer request.
// packet_filter is the server's ROCKETNET_FILTER_VALUE_BYTES filter value,
// copied: each queue worker runs the server's stateless prefilter on every
// datagram before banking it. NULL drives the transport directly (raw
// datagrams): the workers then apply the size pass alone.

struct rocketnet_server_uring_t * rocketnet_server_uring_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int * status = NULL,
                                                                 const uint8_t * packet_filter = NULL );

// create with flags, the DISABLE set above; create is create_flags with 0

struct rocketnet_server_uring_t * rocketnet_server_uring_create_flags( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int * status = NULL,
                                                                       const uint8_t * packet_filter = NULL );

// create with flags and the placement of the queue workers: queue q's worker
// pins itself to cpu num_os_cpus + q and sets SCHED_RR at the minimum, as the
// XDP transport's thread does (rocketnet_uring_pin.h); num_os_cpus below 1
// reads as 1 (cpu 0 is the OS's), which is what create and create_flags use.
// A worker whose pin is refused is confined to the cpus past the OS's where
// the machine has any (never cpu 0 on a machine with more than one cpu). The
// server's config num_os_cpus (default 1) reaches this through the builtin
// driver.

struct rocketnet_server_uring_t * rocketnet_server_uring_create_pinned( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int num_os_cpus,
                                                                        int * status = NULL, const uint8_t * packet_filter = NULL );

// the cpu queue's worker pinned itself to: the cpu once it has, ROCKETNET_URING_PIN_NONE
// when not asked or refused (a warning named the queue and the cpu, and
// rocketnet_server_uring_pin_refusals counted it), ROCKETNET_URING_PIN_PENDING until its thread
// has run. Never the cpu unless the pin succeeded.

int rocketnet_server_uring_worker_cpu( struct rocketnet_server_uring_t * uring, int queue );

// the lowest cpu of queue's worker's affinity mask as the system reports it
// (pthread_getaffinity_np) once the pin or its confine has run: what the
// worker may actually run on, so cpu 0 here means it can run on the OS's core.
// ROCKETNET_URING_PIN_PENDING until its thread has pinned, ROCKETNET_URING_PIN_NONE
// when the mask could not be read.

int rocketnet_server_uring_worker_lowest_cpu( struct rocketnet_server_uring_t * uring, int queue );

// the pins the workers asked for and the system refused (a refused priority is
// rocketnet_platform_thread_priority_refusals)

int rocketnet_server_uring_pin_refusals( struct rocketnet_server_uring_t * uring );

// the packets one receive bank holds: one update's receive budget for the
// transport's max_clients (rocketnet_server_uring_bank_packets)

int rocketnet_server_uring_bank_size( const struct rocketnet_server_uring_t * uring );

// the send slots each queue stages a frame into (rocketnet_server_uring_send_slots
// of the transport's max_clients and queue count)

int rocketnet_server_uring_send_queue_size( const struct rocketnet_server_uring_t * uring );

// the FEATURE bits in use now. a destination that refuses a segmented send
// has its runs resent plain and leaves the bit set; it clears when a queue
// cannot segment at all (rocketnet_server_gso.h's refusal threshold)

int rocketnet_server_uring_features( struct rocketnet_server_uring_t * uring );

// begin_shutdown (the driver's begin_shutdown hook: main thread, when the
// server starts stopping, before its last disconnect batches) tells every
// queue worker shutdown has begun: from then on a socket that stays full
// gets bounded retries, each after the worker's retry wait, and what it
// never takes is abandoned and counted (SEND_DROPPED), so send_idle turns
// true and the server can ask the stop.
//
// stop tells every queue worker to stop: each keeps sending what was
// published, retries a full socket a bounded number of times, cancels its
// receive and exits once nothing of its is in the kernel. stopped is true
// once every worker has exited; it never waits. What the workers banked and
// main never took is counted RECEIVE_DROP_STOPPED then.

void rocketnet_server_uring_begin_shutdown( struct rocketnet_server_uring_t * uring );
void rocketnet_server_uring_stop( struct rocketnet_server_uring_t * uring );
bool rocketnet_server_uring_stopped( struct rocketnet_server_uring_t * uring );

// Requires stopped completion. Joins the workers and frees synchronously.
void rocketnet_server_uring_destroy( struct rocketnet_server_uring_t * uring );

#if ROCKETNET_DEV
// Test seam: stopped answers false until test_resume_stop releases it.
void rocketnet_server_uring_test_stall_destroy( struct rocketnet_server_uring_t * uring );
void rocketnet_server_uring_test_resume_stop( struct rocketnet_server_uring_t * uring );

// development library only. fail_receive has the queue's worker cancel its
// multishot receive and hand the receive module the terminating completion
// with error (negative errno) in place of -ECANCELED, the path a kernel-side
// receive failure takes. fail_sends makes the next count send SQEs any
// worker submits name no socket, so the kernel completes each with a failed
// CQE; 0 ends it.
void rocketnet_server_uring_test_fail_receive( struct rocketnet_server_uring_t * uring, int queue_index, int error );
void rocketnet_server_uring_test_fail_sends( struct rocketnet_server_uring_t * uring, int count );
// main thread: no staged slot waiting and no bank published and not retired
bool rocketnet_server_uring_test_send_idle( struct rocketnet_server_uring_t * uring );
// Test seam: one worker's placement (pin, confine off cpu 0, SCHED_RR) as the
// worker runs it, on queue 0 of a synthetic transport, with fake affinity and
// priority calls: only the refusals are injected, the transport's own code
// counts them and records the queue. No thread, ring or affinity of any
// thread changes. The fake mask is cpus 0 .. cpus - 1, less cpu 0 when
// without_cpu_zero; the pin is refused when refuse_pin, every affinity set
// when refuse_all; online is the online count
// the pin reads. The priority switch is on for the call and restored. The
// public row URING_PIN_REFUSALS is then asked of dispatch, given the io_uring
// io table (uring) and the synthetic transport (its queues hidden from the
// read, so only the transport's own rows answer). Writes that row, whether the
// queue is left on cpu 0 and the SCHED_RR requests made; false when the
// synthetic transport cannot be made or dispatch does not answer, and in a
// build without the transport.
struct rocketnet_server_transport_io_operations_t;
typedef bool rocketnet_server_uring_test_transport_counters_t( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                               uint64_t * values );
bool rocketnet_server_uring_test_place_worker( rocketnet_server_uring_test_transport_counters_t * dispatch, const struct rocketnet_server_transport_io_operations_t * uring,
                                               int num_os_cpus, int online, int cpus, bool without_cpu_zero, bool refuse_pin, bool refuse_all, uint64_t * row,
                                               bool * left_on_core_zero, int * round_robin_requests );
#endif // #if ROCKETNET_DEV

int rocketnet_server_uring_num_queues( struct rocketnet_server_uring_t * uring );

// The io_uring transport's counters are rows of the public table: the
// ROCKETNET_COUNTER_URING_* rows of sdk/rocketnet_counters.h, contiguous from
// ROCKETNET_COUNTER_URING_PACKETS_SENT. Any thread: writes those rows of
// counters, an array of ROCKETNET_NUM_COUNTERS values indexed by public
// counter, every queue's counts summed, cumulative for the transport's life;
// every other value is left as it was, except ROCKETNET_COUNTER_URING_PIN_REFUSALS
// and ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SOCKET, which are not in that run
// and are written too (rocketnet_server_uring_pin_refusals; the datagrams the
// kernel dropped at a queue socket, SO_RXQ_OVFL).
#define ROCKETNET_SERVER_URING_FIRST_COUNTER ROCKETNET_COUNTER_URING_PACKETS_SENT
#define ROCKETNET_SERVER_URING_COUNTER_ROWS  ( ROCKETNET_COUNTER_URING_GRO_CMSG_FAULTS - ROCKETNET_COUNTER_URING_PACKETS_SENT + 1 )
void rocketnet_server_uring_counters( struct rocketnet_server_uring_t * uring, uint64_t * counters );

// send packets. start, finish and abort may run on any number of game
// threads strictly between one send_packets call and the next; send_packets
// runs on the main thread with them joined, publishes each queue's producer
// bank to its worker and wakes it, and returns at once. A queue whose banks
// are all with its worker keeps its producer, and its room, until a later
// send_packets publishes it. the frame handed back is a
// ROCKETNET_FRAME_STRIDE slot: wire bytes begin at ROCKETNET_FRAME_WIRE_OFFSET,
// exactly as the other transports.

uint8_t * rocketnet_server_uring_start_packet( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_uring_finish_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_uring_abort_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_uring_send_packets( struct rocketnet_server_uring_t * uring );

// the slots start_packet may still take in the fullest queue's producer
// bank: a destination's queue is fixed by its address, so this is what any
// start is sure to find. Main thread.
int rocketnet_server_uring_send_room( struct rocketnet_server_uring_t * uring );

// the driver's send queue operations: the queue count, the queue a
// destination stages in, and one queue's producer room (main thread)
int rocketnet_server_uring_send_queues( struct rocketnet_server_uring_t * uring );
int rocketnet_server_uring_send_queue_of( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to );
int rocketnet_server_uring_send_room_queue( struct rocketnet_server_uring_t * uring, int queue );

// true once on every queue nothing is staged and no bank is published and
// not yet retired (every send has its completion). Main thread.
bool rocketnet_server_uring_send_idle( struct rocketnet_server_uring_t * uring );

// receive packets: receive_packets drains every queue's receive bank into
// the process view, at most its capacity, from a rotating first queue;
// process_packets exposes it, valid until the next receive_packets call.

void rocketnet_server_uring_receive_packets( struct rocketnet_server_uring_t * uring );

struct rocketnet_server_socket_process_packets_t * rocketnet_server_uring_process_packets( struct rocketnet_server_uring_t * uring );

// the size drops of every queue (main thread): below the smallest server
// packet, and above the receive ceiling or truncated
void rocketnet_server_uring_receive_drops( struct rocketnet_server_uring_t * uring, uint64_t * too_short, uint64_t * too_large );

// the driver's receive_drop_totals: every queue's prefilter and full-bank
// tally summed by reason, and the datagrams left at stop, cumulative
void rocketnet_server_uring_receive_drop_totals( struct rocketnet_server_uring_t * uring, uint64_t * values );

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_URING_H
