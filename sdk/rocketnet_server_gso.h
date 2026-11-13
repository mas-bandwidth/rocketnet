/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_GSO_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_GSO_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_gso_pin.h"
#include "rocketnet_network_threads.h"
#include "rocketnet_wire.h"
#include "rocketnet_packet_batch.h"

// The GSO transport: the best non-root path Linux has, per the
// measurements -- an SO_REUSEPORT socket group with one worker thread
// per socket, the kernel's 4-tuple flow hash sharding RECEIVES across them
// with zero shared state between queues, recvmmsg/sendmmsg batching on both
// sides, UDP_SEGMENT (GSO) on send and UDP_GRO on receive so a batch
// traverses the kernel's UDP path once instead of per packet. Sends are per
// queue as well: a destination maps to one queue by the XDP transport's hash,
// is staged in that queue's banks, and leaves on that queue's socket from that
// queue's worker, which also drains the socket's receives. Caller/worker
// handoffs exchange only bank ownership under short mutexes; batching, copying
// and socket calls stay outside the locks.
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

// a queue per worker: the counts are the server's num_gso_workers
// (sdk/rocketnet_network_threads.h)
#define ROCKETNET_GSO_DEFAULT_QUEUES                                         ROCKETNET_GSO_DEFAULT_WORKERS
#define ROCKETNET_GSO_MAX_QUEUES                                             ROCKETNET_GSO_MAX_WORKERS

// send staging: one phase's worth of slots, the same budget as the xdp
// transport's send queue. each slot is a frame-stride wire packet.

#define ROCKETNET_GSO_SEND_QUEUE_SIZE                                    16384

// the banks of one queue's send staging: the producer, one with the queue's
// worker and one free (proofs/tla/RNBankFifoLayout.tla, whose Init literal is
// right for three banks only)

#define ROCKETNET_GSO_TX_BANKS                                               3

// receive staging per queue per buffer: twice the send staging, gso's own
// size (the shared per-update receive budget does not size it). One update
// takes at most that budget from the queues into the process array; the rest
// waits in the queue buffers, so a single flooded queue fills its pair
// between drains and drops counted -- correct behaviour under flood, ample
// at design load where the reuseport hash spreads flows

#define ROCKETNET_GSO_RECV_QUEUE_SIZE                                    32768

ROCKETNET_STATIC_ASSERT( ROCKETNET_GSO_RECV_QUEUE_SIZE >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE,
                         "receive budget twice send: GSO receive queue holds at least twice the send queue" );

// the send staging is one producer bank per queue, and does not derive from
// ROCKETNET_MAX_CLIENTS: one queue's bank stages two packets for every client
// of the default configuration. A server whose sends need more in one update
// adds queues, or spreads them across updates, as the transport's send room
// reports (rocketnet_server_gso_send_room, the fullest queue's room;
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

// the segments the drain makes room for per GRO message: the kernel's
// UDP_GRO_CNT_MAX, NIC GRO's coalescing limit. a local sender's segmented
// send can coalesce more (UDP_MAX_SEGMENTS is 128 on recent kernels): the
// worker stages the segments that fit and counts the rest in
// RECEIVE_DROPPED_OVERFLOW

#define ROCKETNET_GSO_RECV_MAX_SEGMENTS                                     64

// the messages one recvmmsg of a drain may ask for, so that every segment of
// every message it returns has a slot: the buffer's free slots, divided by
// ROCKETNET_GSO_RECV_MAX_SEGMENTS when GRO is on, at most a batch. 0 when not
// one more message fits: the drain stops and the datagrams wait in the
// socket for the next buffer, never read only to be dropped.

static inline int rocketnet_server_gso_recv_vlen( int num_packets, bool gro )
{
    const int free_slots = num_packets < ROCKETNET_GSO_RECV_QUEUE_SIZE ? ROCKETNET_GSO_RECV_QUEUE_SIZE - num_packets : 0;
    const int messages = gro ? free_slots / ROCKETNET_GSO_RECV_MAX_SEGMENTS : free_slots;
    return messages < ROCKETNET_GSO_RECV_BATCH ? messages : ROCKETNET_GSO_RECV_BATCH;
}

// socket buffers. linux clamps an SO_SNDBUF / SO_RCVBUF request to
// net.core.wmem_max / net.core.rmem_max without an error; SO_SNDBUFFORCE /
// SO_RCVBUFFORCE pass that limit for a process with CAP_NET_ADMIN and are
// refused (EPERM) for any other. every queue socket asks with the plain
// option, reads the grant back, and when the grant is short asks once more
// with the force option. a grant still short is not fatal: the transport
// runs, the kernel drops what arrives past a short receive grant (UDP
// RcvbufErrors) and a short send grant makes sends wait on a full socket, so
// the transport names it at start with the sysctl the operator sets.
//
// rocketnet_server_gso_buffer_step is that decision for one buffer: the
// request, the grant read back (halved: linux reports double), and whether
// the force option was already tried.

#define ROCKETNET_SERVER_GSO_BUFFER_GRANTED                         0       // the grant meets the request
#define ROCKETNET_SERVER_GSO_BUFFER_FORCE                           1       // short, force not tried: ask again with the force option
#define ROCKETNET_SERVER_GSO_BUFFER_SHORT                           2       // short after the force option: name the sysctl at start

static inline int rocketnet_server_gso_buffer_step( int requested_bytes, int granted_bytes, bool force_tried )
{
    if ( granted_bytes >= requested_bytes )
    {
        return ROCKETNET_SERVER_GSO_BUFFER_GRANTED;
    }
    return force_tried ? ROCKETNET_SERVER_GSO_BUFFER_SHORT : ROCKETNET_SERVER_GSO_BUFFER_FORCE;
}

// the start line for one socket buffer (receive or send), written into line
// and always terminated: asked, got, how, and when the grant is short the
// sysctl and the value the operator sets (the request itself: linux compares
// the request, not its doubled charge, against the limit). force_error is
// the errno the force option was refused with, 0 when it was taken or never
// tried. Returns true when the line is a warning (the grant is short).

bool rocketnet_server_gso_buffer_note( char * line, int line_bytes, bool receive, int requested_bytes, int granted_bytes, bool force_tried,
                                       int force_error );

// send batching geometry: sendmmsg messages per call, GSO segments per
// super-datagram (the kernel's UDP_MAX_SEGMENTS is 64), and a total-bytes
// cap keeping every super-datagram inside the 16-bit UDP payload space

#define ROCKETNET_GSO_SEND_BATCH                                           128
#define ROCKETNET_GSO_MAX_SEGMENTS                                          64
#define ROCKETNET_GSO_MAX_SUPER_BYTES                                    65000

// a full socket keeps the queue worker's bank for the next update; this is how
// long the worker waits for main's wake before it tries the socket again.
// before shutdown the worker retries without limit and drops nothing.

#define ROCKETNET_GSO_SEND_RETRY_SECONDS                                 0.001

// once shutdown has begun (rocketnet_server_gso_begin_shutdown, called when
// the server starts to stop and before its disconnect packets drain, or
// rocketnet_server_gso_stop), a full socket is tried this many further times;
// then the worker drops the bank's unsent tail, counts it in SEND_DROPPED and
// returns the bank. a socket that stays full then cannot keep send_idle false
// (the server asks for stop only once it is true) or the transport from
// reporting stopped. each retry waits for main's wake or the retry
// timeout, so a socket that stays full holds a bank at most about 100 ms;
// a transient refusal (a disconnect burst meeting a busy socket) is sent.
// the value is unmeasured: chosen, not derived from a measured drain time.
#define ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT                             100

// feature bits reported by rocketnet_server_gso_features: what the runtime
// probe found and the transport is actually using

#define ROCKETNET_SERVER_GSO_FEATURE_SEGMENT                    (1<<0)      // UDP_SEGMENT accepted: send batches as GSO super-datagrams
#define ROCKETNET_SERVER_GSO_FEATURE_GRO                        (1<<1)      // UDP_GRO enabled: receives arrive coalesced
#define ROCKETNET_SERVER_GSO_FEATURE_REUSEPORT                  (1<<2)      // SO_REUSEPORT group bound: receives shard across queues

// One destination refusing UDP_SEGMENT (EINVAL, EIO or EOPNOTSUPP: its route
// cannot take a segmented send, an IPsec policy say) is that destination's,
// never another's: its refused run is resent plain, and the destination is
// remembered so its later runs leave plain without a refusal. Segmentation
// turns off for the transport only when this many destinations have refused
// and no segmented send has been accepted since create: a kernel or device
// that cannot segment at all. One accepted segmented send shows the transport
// can segment, and from then on no refusal turns it off.
#define ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD                  8

// The refused destinations, a direct-mapped table of this many entries owned
// by the thread that sends (each gso queue worker, the io_uring main thread). A
// destination maps to one entry by its address and is matched on the whole
// address, so a destination that never refused is never sent plain by it. A
// refusing destination that maps to an occupied entry takes it, and the one
// it displaces pays one refusal again on its next run. Fixed size: no
// allocation, and a lookup per run whatever the client count. Modelled in
// proofs/tla/RNSegmentRefusal.tla: Run reads the table, Refuse adds to it.
#define ROCKETNET_SEGMENT_REFUSED_ENTRIES                                64

struct rocketnet_segment_refused_t
{
    struct rocketnet_address_t destination[ROCKETNET_SEGMENT_REFUSED_ENTRIES];     // type ROCKETNET_ADDRESS_NONE: empty
    int destinations;               // refusals of a destination the table did not hold, up to the threshold: never fewer than the distinct refusing destinations
    bool accepted;                  // a segmented send was accepted since create
};

static inline int rocketnet_segment_refused_entry( const struct rocketnet_address_t * address )
{
    uint32_t hash = 2166136261u;
    const int words = address->type == ROCKETNET_ADDRESS_IPV6 ? 8 : 2;
    for ( int i = 0; i < words; i++ )
    {
        hash = ( hash ^ address->data.ipv6[i] ) * 16777619u;
    }
    hash = ( hash ^ address->port ) * 16777619u;
    return (int) ( ( hash ^ ( hash >> 16 ) ) % ROCKETNET_SEGMENT_REFUSED_ENTRIES );
}

static inline bool rocketnet_segment_refused_contains( const struct rocketnet_segment_refused_t * refused, const struct rocketnet_address_t * address )
{
    const struct rocketnet_address_t * entry = &refused->destination[rocketnet_segment_refused_entry( address )];
    return entry->type != ROCKETNET_ADDRESS_NONE && rocketnet_address_equal( entry, address );
}

// remembers a refusing destination the table did not hold. true when
// segmentation should now turn off for the transport: the threshold reached
// with no segmented send ever accepted. Every such refusal counts, a
// displaced destination's again included, so two refusers sharing an entry
// cannot keep the count below the distinct destinations that refused
// (proofs/tla/RNSegmentRefusal.tla Counted, KernelFallback)
static inline bool rocketnet_segment_refused_add( struct rocketnet_segment_refused_t * refused, const struct rocketnet_address_t * address )
{
    struct rocketnet_address_t * entry = &refused->destination[rocketnet_segment_refused_entry( address )];
    if ( refused->destinations < ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD )
    {
        refused->destinations++;
    }
    *entry = *address;
    return !refused->accepted && refused->destinations >= ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
}

// create flags: force a probe result off, so the degraded paths are testable
// on kernels where the real probe would say yes -- the fallback tier must
// never be the untested tier

#define ROCKETNET_SERVER_GSO_DISABLE_SEGMENT                    (1<<0)
#define ROCKETNET_SERVER_GSO_DISABLE_GRO                        (1<<1)

// bind_address is read for family and port and written back when the port
// was zero, the same contract as rocketnet_platform_socket_create. an ipv6
// bind is dual stack: ipv4 senders arrive as native ipv4 addresses,
// normalized exactly once at this boundary per the standard's law.
// num_queues <= 0 selects the default. max_clients is the server's configured
// client count, which sizes the receive view and each socket's receive
// buffer request. flags is the DISABLE set above, zero for production.
// packet_filter is the server's ROCKETNET_FILTER_VALUE_BYTES filter value,
// copied: each queue worker runs the server's stateless prefilter on every
// segment before banking it. NULL drives the transport directly (raw
// datagrams): the worker then applies the size pass alone.

struct rocketnet_server_gso_t * rocketnet_server_gso_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int * status = NULL,
                                                             const uint8_t * packet_filter = NULL );

// the same, and each queue's worker pins itself to cpu num_os_cpus + queue
// (rocketnet_gso_pin.h, the XDP transport's rule) and then sets its own
// SCHED_RR (rocketnet_platform_network_thread_rr_priority) unless the platform
// priority switch is off or it was left on cpu 0 (its pin and the
// confinement after it both failed). A num_os_cpus below 1
// is read as 1 (the OS keeps cpu 0 on Linux), which is what
// rocketnet_server_gso_create passes: the transport driven directly pins from
// cpu 1 too. The server's num_os_cpus (default 1) reaches this through
// the builtin driver. A refused pin (cpu outside the set or not online, or
// affinity refused) is a warning naming the queue and the cpu, is counted in
// rocketnet_server_gso_pin_refusals, and the worker is never reported pinned;
// a refused priority is counted in the platform's priority refusals. A
// refused worker is confined to the online cpus from num_os_cpus up
// (rocketnet_gso_pin_fallback). A confinement that fails (the system refuses
// it, or no cpu but cpu 0 is online) is counted too and leaves the worker on
// a mask that includes cpu 0, with one warning naming the queue and the mask,
// and without SCHED_RR; the worker carries on.

struct rocketnet_server_gso_t * rocketnet_server_gso_create_pinned( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int num_os_cpus,
                                                                    int * status = NULL, const uint8_t * packet_filter = NULL );

// the cpu queue's worker pinned itself to, ROCKETNET_GSO_PIN_NONE if it is
// not pinned (not asked for, or refused: counted), ROCKETNET_GSO_PIN_PENDING
// until its thread has run the pin. Any thread.
int rocketnet_server_gso_worker_cpu( struct rocketnet_server_gso_t * gso, int queue );

// the pins (not priorities) the workers asked for and the system refused,
// and confinements refused after a refused pin. Any thread.
int rocketnet_server_gso_pin_refusals( struct rocketnet_server_gso_t * gso );

// the datagrams the queue workers dropped before banking, by reason
// (ROCKETNET_RECEIVE_NUM_DROPS values in rocketnet_receive_drop_t order),
// cumulative. Main thread.
void rocketnet_server_gso_receive_drop_totals( struct rocketnet_server_gso_t * gso, uint64_t * values );

// the queue count (fixed after create), and one queue's producer room. Any
// thread for the count; the room on the threads send_room is read on (the main
// thread and the game threads between main thread calls, sdk/rocketnet_driver.h).
int rocketnet_server_gso_send_queues( struct rocketnet_server_gso_t * gso );
int rocketnet_server_gso_send_room_queue( struct rocketnet_server_gso_t * gso, int queue );

void rocketnet_server_gso_stop( struct rocketnet_server_gso_t * gso );
bool rocketnet_server_gso_stopped( struct rocketnet_server_gso_t * gso );

// Requires stopped completion. Joins and frees synchronously.
void rocketnet_server_gso_destroy( struct rocketnet_server_gso_t * gso );

#if ROCKETNET_DEV
int rocketnet_server_gso_num_queues( struct rocketnet_server_gso_t * gso );
#endif // #if ROCKETNET_DEV

int rocketnet_server_gso_features( struct rocketnet_server_gso_t * gso );

// The gso transport's counters are rows of the public table: the
// ROCKETNET_COUNTER_GSO_* rows of sdk/rocketnet_counters.h, contiguous from
// ROCKETNET_COUNTER_GSO_PACKETS_SENT. Main thread, any time: writes those rows
// of counters, an array of ROCKETNET_NUM_COUNTERS values indexed by public
// counter, cumulative for the transport's life; every other value is left as
// it was.
#define ROCKETNET_SERVER_GSO_FIRST_COUNTER ROCKETNET_COUNTER_GSO_PACKETS_SENT
#define ROCKETNET_SERVER_GSO_COUNTER_ROWS  ( ROCKETNET_COUNTER_GSO_GRO_CMSG_FAULTS - ROCKETNET_COUNTER_GSO_PACKETS_SENT + 1 )
void rocketnet_server_gso_counters( struct rocketnet_server_gso_t * gso, uint64_t * counters );

// The two tallies with no row of their own (the first is counted by reason
// in the shared prefilter rows, the second is part of the server's send
// failures), cumulative for the transport's life. The first is main thread;
// the second any thread.
uint64_t rocketnet_server_gso_receive_dropped_prefilter( struct rocketnet_server_gso_t * gso );
uint64_t rocketnet_server_gso_send_finish_refused( struct rocketnet_server_gso_t * gso );

// send packets. the phase contract is the transport's: start, finish and
// abort may run on any number of game threads strictly between one
// send_packets call and the next. start stages the packet in the producer bank
// of the queue its destination maps to (rocketnet_server_gso_send_queue) with
// one compare-and-swap on that queue's count; the packet id names the queue
// and the slot. send_packets try-publishes each queue's main-owned bank to
// that queue's worker and wakes it. The worker coalesces same-destination
// equal-size runs and sends on its own socket outside the handoff lock. A
// full/contended FIFO retains the producer for a later send/receive call; stop
// transfers each queue's final producer after its earlier ready banks and
// completion waits for every worker.
// The frame handed back is a ROCKETNET_FRAME_STRIDE slot: wire
// bytes begin at ROCKETNET_FRAME_WIRE_OFFSET, exactly as the other transports.

uint8_t * rocketnet_server_gso_start_packet( struct rocketnet_server_gso_t * gso, const struct rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_gso_finish_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_gso_abort_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_gso_send_packets( struct rocketnet_server_gso_t * gso );

// the queue a destination's packets are staged on and sent from: the XDP
// transport's FNV-1a hash of the address bytes and port, modulo num_queues.
// Pure; any thread.
int rocketnet_server_gso_send_queue( const struct rocketnet_address_t * to, int num_queues );

// the slots start_packet may still take in the fullest queue's producer bank:
// what any destination is sure to find. Main thread.
int rocketnet_server_gso_send_room( struct rocketnet_server_gso_t * gso );

// true once every queue's producer is empty, no bank waits for its worker and
// each worker has returned every bank it took, its sends finished. Main
// thread; a lane mutex held by a worker reads as not idle.
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
// The send seams below run on a fixture with no socket and no thread.
void rocketnet_server_gso_test_force_create_flags( int flags );
void rocketnet_server_gso_test_fail_receives( struct rocketnet_server_gso_t * gso, int count, int error );
void rocketnet_server_gso_test_fail_sends( struct rocketnet_server_gso_t * gso, int count, int error );
// Observe publication without consuming it; returns false on lock contention.
bool rocketnet_server_gso_test_receive_ready( struct rocketnet_server_gso_t * gso, int queue_index );
// Main thread: no staged packet, no ready bank, no bank held by a queue worker.
bool rocketnet_server_gso_test_send_idle( struct rocketnet_server_gso_t * gso );
// Deterministic ownership/full/contended/stop and retained-RX-tail regression.
bool rocketnet_server_gso_test_worker_handoff();
// StopCount: what is still banked at stop is counted RECEIVE_DROP_STOPPED
// once every worker has exited, and main takes nothing after stop.
bool rocketnet_server_gso_test_stop_counts_unreceived();
// A refused sendmmsg: an errno other than a full socket's drops the one
// refused message and sends the rest.
bool rocketnet_server_gso_test_send_refusals();
// A full socket mid-flush, on a fake socket: the flush stops at the first
// unsent message, drops nothing, and the next attempt sends the rest in
// order without counting an earlier dropped run twice.
bool rocketnet_server_gso_test_send_full_keeps_rest();
// A full socket after stop. The queue worker's EAGAIN retry must notice quit
// and leave, so the transport reports stopped. No socket, no sleep.
bool rocketnet_server_gso_test_send_retry_quit();
// Per-queue staging on four queues: each packet on its destination's queue,
// the id naming queue and slot, foreign and unaligned pointers refused, ids
// distinct across a turn-over, every queue's bank sent.
bool rocketnet_server_gso_test_send_queues();
// One receive drain fed endless discards returns within its read budget.
bool rocketnet_server_gso_test_drain_budget();
bool rocketnet_server_gso_test_drain_room();
// Receives whose UDP_GRO control is truncated reach the GRO_CMSG_FAULTS
// counter once each, through the drain, the publish and main's fold.
bool rocketnet_server_gso_test_gro_cmsg_faults();
// The worker's prefilter before banking: a datagram main would refuse on its
// stateless rungs takes no slot and is counted by reason.
bool rocketnet_server_gso_test_drain_prefilter();

#if defined( __linux__ )
struct mmsghdr;

// No socket, no thread and no clock. num_queues receive queues and the real
// per-update budget. NULL when the fixture cannot be built. set_send is the
// sendmmsg seam. set_features installs the probe bits. flush_staged publishes
// every queue's staged bank and sends each on this thread, in queue order;
// false when no queue had a bank or one was not sent whole.
struct rocketnet_server_gso_t * rocketnet_server_gso_test_open( int num_queues );
void rocketnet_server_gso_test_close( struct rocketnet_server_gso_t * gso );
// The trace check's seams (tests/tla_trace_gso.cpp, proofs/tla/RNGsoQueueBankLayout.tla).
// test_open_slots is test_open with each producer bank handing out
// send_slots slots (1..ROCKETNET_GSO_SEND_QUEUE_SIZE) instead of all of
// them, so a walk fills a bank in a few steps; NULL out of range.
// test_send_lane reads one queue's send staging as the lane holds it, and
// test_send_packet_bytes one slot's packet_bytes (-1 out of range).
// test_take_send and test_return_send are the queue worker's take and
// return of a published bank, on the calling thread.
struct rocketnet_server_gso_test_send_lane_t
{
    int slots;                                          // the slots a producer bank hands out
    int producer;
    uint64_t turn;
    int free_banks[ROCKETNET_GSO_TX_BANKS];
    int free_head;
    int free_count;
    int ready_banks[ROCKETNET_GSO_TX_BANKS]; // the published banks, oldest at ready_head
    int ready_head;
    int ready_count;
    int num_packets[ROCKETNET_GSO_TX_BANKS];
    const uint8_t * first[ROCKETNET_GSO_TX_BANKS];      // each bank's slot 0
    size_t bank_bytes;                                  // the bytes of each bank's slots
};
struct rocketnet_server_gso_t * rocketnet_server_gso_test_open_slots( int num_queues, int send_slots );

// The bytes rocketnet_server_gso_test_open holds for num_queues queues (the
// transport and each queue's send banks and receive buffers), for the
// resident-set bound of a case that opens several (tests/suite_runner.h).
uint64_t rocketnet_server_gso_test_open_bytes( int num_queues );
bool rocketnet_server_gso_test_send_lane( struct rocketnet_server_gso_t * gso, int queue_index, struct rocketnet_server_gso_test_send_lane_t * lane );
// One worker's placement (pin, confinement off cpu 0, SCHED_RR) as the
// worker runs it, on queue queue_index of a test_open fixture, with fake
// affinity and priority calls: only the refusals are injected, the
// transport's own code counts them and records the queue. The fake mask is
// cpus 0 .. cpus - 1, less cpu 0 when without_cpu_zero; the pin is refused
// when refuse_pin, every affinity set when refuse_all; online is the online
// count the pin reads. The priority switch is on for the call and restored.
// No thread's affinity or priority changes. Writes whether the queue is left
// on cpu 0 and the SCHED_RR requests made; false for a bad queue index.
bool rocketnet_server_gso_test_place_worker( struct rocketnet_server_gso_t * gso, int queue_index, int num_os_cpus, int online, int cpus, bool without_cpu_zero,
                                             bool refuse_pin, bool refuse_all, bool * left_on_core_zero, int * round_robin_requests );
// Where queue queue_index's running worker may run, read from the thread
// itself (pthread_getaffinity_np): 1 when its mask allows cpu 0, 0 when it
// does not, -1 when there is no such worker or the read failed.
int rocketnet_server_gso_test_worker_allows_cpu0( struct rocketnet_server_gso_t * gso, int queue_index );
int rocketnet_server_gso_test_send_packet_bytes( struct rocketnet_server_gso_t * gso, int queue_index, int bank, int slot );
int rocketnet_server_gso_test_send_slot( struct rocketnet_server_gso_t * gso, uint64_t packet_id, const uint8_t * packet_data, int * queue_out );
int rocketnet_server_gso_test_take_send( struct rocketnet_server_gso_t * gso, int queue_index, bool * final );
void rocketnet_server_gso_test_return_send( struct rocketnet_server_gso_t * gso, int queue_index, int index );
void rocketnet_server_gso_test_set_send( struct rocketnet_server_gso_t * gso,
                                         int ( *send )( struct mmsghdr * messages, unsigned int count, void * user ),
                                         void * user );
// The claim seam: hook runs inside start_packet between its load and its compare-and-swap;
// the losses are the compare-and-swaps that lost, cumulative.
void rocketnet_server_gso_test_set_claim_hook( struct rocketnet_server_gso_t * gso, void ( *hook )( void * user, int queue ), void * user );
int rocketnet_server_gso_test_claim_losses( struct rocketnet_server_gso_t * gso );
void rocketnet_server_gso_test_set_features( struct rocketnet_server_gso_t * gso, int features );
bool rocketnet_server_gso_test_flush_staged( struct rocketnet_server_gso_t * gso );
// Publish packets on one queue, each byte set to marker. false when a ready
// buffer already waits, or the queue index or count is out of range.
bool rocketnet_server_gso_test_publish_marked( struct rocketnet_server_gso_t * gso, int queue_index, uint8_t marker, uint32_t packets );
#endif // #if defined( __linux__ )
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_GSO_H
