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
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE,
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

// a full socket keeps the TX worker's bank for the next update; this is how
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
// by the thread that sends (the gso TX worker, the io_uring main thread). A
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
    int destinations;               // refusals that filled an empty entry: distinct refusing destinations, up to the table size
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

// remembers a refusing destination. true when segmentation should now turn
// off for the transport: the threshold of distinct destinations reached with
// no segmented send ever accepted
static inline bool rocketnet_segment_refused_add( struct rocketnet_segment_refused_t * refused, const struct rocketnet_address_t * address )
{
    struct rocketnet_address_t * entry = &refused->destination[rocketnet_segment_refused_entry( address )];
    if ( entry->type == ROCKETNET_ADDRESS_NONE )
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

// counters, readable any time from the main thread; indexes into the array
// rocketnet_server_gso_counters fills

#define ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT                    0
#define ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED               1
#define ROCKETNET_SERVER_GSO_COUNTER_SEND_SYSCALLS                  2
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_SYSCALLS               3
#define ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED                   4       // packets not sent: refused for their destination (never resent), or a full socket's tail left after shutdown's bounded retries
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE           5       // segment outside [server min, receive ceiling]
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_OVERFLOW       6       // segments of one GRO receive past the staging's room: more than ROCKETNET_GSO_RECV_MAX_SEGMENTS
#define ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS                      7       // sendmmsg messages that carried a UDP_SEGMENT cmsg
#define ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES                    8       // receive buffers that arrived GRO-coalesced (more than one segment)
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SOURCE         9       // source address family the boundary could not convert
#define ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_ERRORS                10       // recvmmsg answered -1 with an errno other than EAGAIN; the worker goes back to poll
#define ROCKETNET_SERVER_GSO_NUM_COUNTERS                          11

// bind_address is read for family and port and written back when the port
// was zero, the same contract as rocketnet_platform_socket_create. an ipv6
// bind is dual stack: ipv4 senders arrive as native ipv4 addresses,
// normalized exactly once at this boundary per the standard's law.
// num_queues <= 0 selects the default. max_clients is the server's configured
// client count, which sizes the receive view and each socket's receive
// buffer request. flags is the DISABLE set above, zero for production.

struct rocketnet_server_gso_t * rocketnet_server_gso_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int * status = NULL );

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
// The send seams below run on a fixture with no socket and no thread.
void rocketnet_server_gso_test_force_create_flags( int flags );
void rocketnet_server_gso_test_fail_receives( struct rocketnet_server_gso_t * gso, int count, int error );
void rocketnet_server_gso_test_fail_sends( struct rocketnet_server_gso_t * gso, int count, int error );
// Observe publication without consuming it; returns false on lock contention.
bool rocketnet_server_gso_test_receive_ready( struct rocketnet_server_gso_t * gso, int queue_index );
// Main thread: no staged packet, no ready bank, no bank held by the TX worker.
bool rocketnet_server_gso_test_send_idle( struct rocketnet_server_gso_t * gso );
// Deterministic ownership/full/contended/stop and retained-RX-tail regression.
bool rocketnet_server_gso_test_worker_handoff();
// A refused sendmmsg: an errno other than a full socket's drops the one
// refused message and sends the rest.
bool rocketnet_server_gso_test_send_refusals();
// A full socket mid-flush, on a fake socket: the flush stops at the first
// unsent message, drops nothing, and the next attempt sends the rest in
// order without counting an earlier dropped run twice.
bool rocketnet_server_gso_test_send_full_keeps_rest();
// A full socket after stop. The TX worker's EAGAIN retry must notice
// send_quit and leave, so the transport reports stopped. No socket, no sleep.
bool rocketnet_server_gso_test_send_retry_quit();
// One receive drain fed endless discards returns within its read budget.
bool rocketnet_server_gso_test_drain_budget();
bool rocketnet_server_gso_test_drain_room();

#if defined( __linux__ )
struct mmsghdr;

// No socket, no thread and no clock. num_queues receive queues and the real
// per-update budget. NULL when the fixture cannot be built. set_send is the
// sendmmsg seam. set_features installs the probe bits. flush_staged publishes
// the staged bank and sends it on this thread.
struct rocketnet_server_gso_t * rocketnet_server_gso_test_open( int num_queues );
void rocketnet_server_gso_test_close( struct rocketnet_server_gso_t * gso );
void rocketnet_server_gso_test_set_send( struct rocketnet_server_gso_t * gso,
                                         int ( *send )( struct mmsghdr * messages, unsigned int count, void * user ),
                                         void * user );
void rocketnet_server_gso_test_set_features( struct rocketnet_server_gso_t * gso, int features );
bool rocketnet_server_gso_test_flush_staged( struct rocketnet_server_gso_t * gso );
// Publish packets on one queue, each byte set to marker. false when a ready
// buffer already waits, or the queue index or count is out of range.
bool rocketnet_server_gso_test_publish_marked( struct rocketnet_server_gso_t * gso, int queue_index, uint8_t marker, uint32_t packets );
#endif // #if defined( __linux__ )
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_GSO_H
