/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_IOCP_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_IOCP_H

#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_packet_batch.h"
#include "rocketnet_wire.h"
#include "rocketnet_network_threads.h"

// Windows server transport. The public transport API is unchanged. The send
// side is lanes, the XDP shape: each lane owns its staging, its pool of send
// operations, and a client's packets always stage in the
// lane its address hashes to (rocketnet_server_iocp_send_lane), so one lane
// carries every packet of a client, in order. Fill threads acquire and seal
// directly into the lane's transport-owned cold frames. Flush try-publishes
// one of each lane's three compact generation-tagged index banks to that
// lane's send thread (the thread that owns the lane, lane l to thread l % threads), which alone cuts the bank into runs of one destination
// (grouped by the send gather, sdk/rocketnet_send_gather.h), coalesces and posts its
// UDP sends; only completions return frames to the lane's bounded
// free-reference queue.
// Receive workers dequeue completions in batches, refuse what the stateless
// prefilter names before a datagram takes a bank slot, and commit their bank
// to a three-buffer shared receive handoff (sdk/rocketnet_receive_handoff.h).
// Completion work never runs under the publication mutex; the main receive
// call tries that mutex once and reads each worker's committed datagrams
// without waiting for it.
// Refused receive posts enter a bounded generation-tagged retry queue; a
// worker drains only positions below the enqueue ticket captured at the start
// of its pass.

// The completion workers are the server config's num_iocp_completion_workers
// (default ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS, 2, on a specific bind
// address; one on the wildcard when nothing was asked, said in a warning).
// One worker is one socket bound exclusively. More than one is as many endpoints:
// each worker has its own socket and completion port, the socket given the
// worker's processor with SIO_CPU_AFFINITY before bind and all bound to the
// same specific address and port, and the worker pinned to that processor.
// RSS hands each flow to one processor, so a client's datagrams stay with
// one worker and arrive in order. A refused SIO_CPU_AFFINITY is a named
// refusal at create, never a quiet fallback; several workers refuse the
// wildcard and IPv4-mapped addresses. Windows has no exclusive form of a
// port shared by processor: a socket of another process that also sets
// SIO_CPU_AFFINITY can join the port and receive the flows RSS hands its
// processor (a plain or SO_REUSEADDR bind is refused, measured).
#define ROCKETNET_IOCP_MAX_WORKERS ROCKETNET_IOCP_MAX_COMPLETION_WORKERS

// The send threads. A send thread takes the ready banks of the lanes it owns
// (lane l belongs to thread l % threads), groups and posts them. There are
// num_iocp_send_threads of them (default ROCKETNET_IOCP_DEFAULT_SEND_THREADS,
// or as many as there are lanes when there are fewer; an asked count above the
// lanes is refused), each pinned to a core of its own after the completion
// workers' cores (rocketnet_server_iocp_send_placement). The default is the
// smallest measured to hold the space game's peak (1000 players, 15 packets
// of 1200 bytes each per 60 Hz frame, 900k packets a second, the game threads
// half busy, 2 completion workers): 60 frames a second with every player whole.
// ROCKETNET_IOCP_MAX_SEND_THREADS is in sdk/rocketnet_network_threads.h.
#define ROCKETNET_IOCP_SEND_THREADS ROCKETNET_IOCP_DEFAULT_SEND_THREADS

// The send staging is sized by rate. The lanes together stage
// ROCKETNET_IOCP_SEND_PACKETS_PER_CLIENT for every sized client: 32 at 1000
// clients is 8 lanes of 4096, twice the 17 a client's share of 10 Gb/s of
// 1200-byte packets is per 60 Hz update, so the frames a flush posts while the
// last flush's sends complete fit too. Lanes are of ROCKETNET_IOCP_LANE_PACKETS
// unless a lane must be larger: the protocol's send reserve
// (rocketnet_server_transport_send_reserves) holds every client slot not
// connected in every lane, so a lane holds every sized client plus
// ROCKETNET_IOCP_LANE_HEADROOM (the handshake and disconnect allowance, 80 by
// default, and fill), or the reserve would leave it no fill at all. Packet
// handles keep sixteen bits for the operation index, so every lane's
// operations together are at most ROCKETNET_IOCP_MAX_OPERATIONS; past that,
// larger lanes are fewer: 4 lanes of 16384 at 10000 clients, 6.5 frames a
// client, the most sixteen bits allow.
#define ROCKETNET_IOCP_LANE_PACKETS 4096
#define ROCKETNET_IOCP_LANE_HEADROOM 1024
#define ROCKETNET_IOCP_MAX_LANES 16
#define ROCKETNET_IOCP_MAX_OPERATIONS 65536
#define ROCKETNET_IOCP_SEND_PACKETS_PER_CLIENT 32

ROCKETNET_STATIC_ASSERT( ROCKETNET_IOCP_MAX_LANES * ROCKETNET_IOCP_LANE_PACKETS <= ROCKETNET_IOCP_MAX_OPERATIONS &&
                             ROCKETNET_IOCP_MAX_OPERATIONS == UINT16_MAX + 1,
                         "every lane's send operations fit the packet handle's sixteen index bits" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_CLIENTS + ROCKETNET_IOCP_LANE_HEADROOM <= ROCKETNET_IOCP_MAX_OPERATIONS,
                         "one lane holds every client slot and the headroom within sixteen index bits" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_IOCP_SEND_THREADS >= 1 && ROCKETNET_IOCP_SEND_THREADS <= ROCKETNET_IOCP_MAX_SEND_THREADS &&
                             ROCKETNET_IOCP_MAX_SEND_THREADS == ROCKETNET_IOCP_MAX_LANES,
                         "a send thread owns at least one lane: the threads are at most the lanes there can be" );

// Which send thread owns which lane: lane l belongs to thread l % threads, and
// thread t takes lanes t, t + threads, ... in order. The transport's send
// threads, the model (proofs/tla/RNIocpSendLanes.tla, ThreadLanes) and the
// trace harness (tests/tla_trace_iocp_lanes.cpp) share one copy of this
// arithmetic: these two inline functions and rocketnet_iocp_send_thread_lanes
// in sdk/rocketnet_server_iocp_windows.cpp, the walk the send threads take at
// create.
//
// The send threads there are for lanes lanes: the most there can be, or as
// many as there are lanes when there are fewer. Any thread.
static inline int rocketnet_server_iocp_send_threads( int lanes )
{
    return lanes < ROCKETNET_IOCP_SEND_THREADS ? lanes : ROCKETNET_IOCP_SEND_THREADS;
}

// The send threads for lanes lanes when num_iocp_send_threads asked for
// asked: zero is the default above; a count from 1 up to the lanes is that
// count; 0 back is a refusal (below 0, above the lanes, or past
// ROCKETNET_IOCP_MAX_SEND_THREADS): a thread with no lane would never post.
// Any thread.
static inline int rocketnet_server_iocp_send_threads_for( int asked, int lanes )
{
    if ( asked == 0 )
    {
        return rocketnet_server_iocp_send_threads( lanes );
    }
    if ( asked < 0 || asked > lanes || asked > ROCKETNET_IOCP_MAX_SEND_THREADS )
    {
        return 0;
    }
    return asked;
}

// the thread that owns lane l. Any thread.
static inline int rocketnet_server_iocp_send_thread_of_lane( int lane, int threads )
{
    return lane % threads;
}

// one lane's staging and operation pool for sized clients (a power of two):
// a fair share of the rate, at most ROCKETNET_IOCP_LANE_PACKETS unless every
// client slot plus the headroom needs more (above 3072 clients), and never
// less than that. A function of the sized
// count, so a unit test reaches every build's sizes.
static inline int rocketnet_server_iocp_lane_size_for( int sized )
{
    const int packets = ROCKETNET_IOCP_SEND_PACKETS_PER_CLIENT * sized;
    int lanes = ( packets + ROCKETNET_IOCP_LANE_PACKETS - 1 ) / ROCKETNET_IOCP_LANE_PACKETS;
    lanes = lanes < 1 ? 1 : ( lanes > ROCKETNET_IOCP_MAX_LANES ? ROCKETNET_IOCP_MAX_LANES : lanes );
    int size = rocketnet_power_of_two_at_least( ( packets + lanes - 1 ) / lanes );
    size = size < ROCKETNET_IOCP_LANE_PACKETS ? size : ROCKETNET_IOCP_LANE_PACKETS;
    const int floor = rocketnet_power_of_two_at_least( sized + ROCKETNET_IOCP_LANE_HEADROOM );
    return size > floor ? size : floor;
}

// the lanes for sized clients: enough for the rate in lanes of the size
// above, at most ROCKETNET_IOCP_MAX_LANES and at most the lanes of that size
// sixteen index bits hold
static inline int rocketnet_server_iocp_lanes_for( int sized )
{
    const int packets = ROCKETNET_IOCP_SEND_PACKETS_PER_CLIENT * sized;
    const int size = rocketnet_server_iocp_lane_size_for( sized );
    int lanes = ( packets + ROCKETNET_IOCP_LANE_PACKETS - 1 ) / ROCKETNET_IOCP_LANE_PACKETS;
    lanes = lanes > ROCKETNET_IOCP_MAX_LANES ? ROCKETNET_IOCP_MAX_LANES : lanes;
    lanes = lanes > ROCKETNET_IOCP_MAX_OPERATIONS / size ? ROCKETNET_IOCP_MAX_OPERATIONS / size : lanes;
    return lanes < 1 ? 1 : lanes;
}

static inline int rocketnet_server_iocp_send_lanes( int max_clients )
{
    return rocketnet_server_iocp_lanes_for( rocketnet_server_sized_clients( max_clients ) );
}

// one lane's staging and operation pool (4096 at 1000 clients)
static inline int rocketnet_server_iocp_send_queue_size( int max_clients )
{
    return rocketnet_server_iocp_lane_size_for( rocketnet_server_sized_clients( max_clients ) );
}

// A completion worker's receive bank is sized by rate, not by the send
// staging: the workers' banks together hold one update's receive budget
// (rocketnet_server_receive_packets_per_update, 16000 at 1000 clients), so a
// flood the worker prefilter cannot name is still taken at the rate the main
// thread processes. Both are sized at create.
#define ROCKETNET_IOCP_RECEIVES_PER_WORKER 128

static inline int rocketnet_server_iocp_receive_queue_size( int max_clients, int workers )
{
    const int budget = rocketnet_server_receive_packets_per_update( max_clients );
    return workers > 1 ? budget / workers : budget;
}

// The receive budget twice send, at the smallest sizing count: the process
// batch holds sixteen packets per sized client and a lane stages at most
// ROCKETNET_IOCP_LANE_PACKETS, so the batch holds at least two lanes' frames
// at every count.
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_IOCP_LANE_PACKETS,
                         "receive budget twice send: server process batch holds at least twice an IOCP lane's staging" );

// UDP segmentation offload (USO) on send and UDP receive offload (URO) on
// receive, Windows 11 and Server 2022 or later. Each is a capability create
// probes on the bound socket; a refusal leaves the per-datagram path. A USO
// run is at most this many frames and this many bytes; a URO receive buffer
// holds this many bytes, the coalesced maximum the socket is given.
#define ROCKETNET_IOCP_USO_MAX_SEGMENTS 64
#define ROCKETNET_IOCP_USO_MAX_SUPER_BYTES 65000
#define ROCKETNET_IOCP_URO_BUFFER_BYTES 16384
#define ROCKETNET_IOCP_URO_MAX_SEGMENTS 64

#if ROCKETNET_DEV
// Development create flags: skip a probe, so the fallback path is testable
// on a host that has the feature. Production does not carry these controls.
#define ROCKETNET_IOCP_DISABLE_USO 1
#define ROCKETNET_IOCP_DISABLE_URO 2
// every send thread's pin is refused by Windows (a group that does not exist)
#define ROCKETNET_IOCP_REFUSE_SEND_PINS 4
// every completion worker's pin is refused by Windows (a group that does not exist)
#define ROCKETNET_IOCP_REFUSE_WORKER_PINS 8
#endif // #if ROCKETNET_DEV

// rocketnet_server_iocp_features bits
#define ROCKETNET_IOCP_FEATURE_USO 1
#define ROCKETNET_IOCP_FEATURE_URO 2

struct rocketnet_server_iocp_t;

// bind_address is updated with the actual port. IPv6 sockets accept both
// families and normalize mapped IPv4 sources.
// max_clients is the server's configured client count: it sizes the send
// queue, the receive banks, the process view and the socket receive buffer
// request.
rocketnet_server_iocp_t * rocketnet_server_iocp_create( void * context,
                                                        rocketnet_address_t * bind_address, int num_workers, int max_clients
#if ROCKETNET_DEV
    // Development constructor controls: fail after socket, port, first worker
    // or first receive, and optionally force the per-datagram fallback paths.
                                                        ,
                                                        int fail_stage = 0, int flags = 0
#endif // #if ROCKETNET_DEV
                                                        ,
                                                        int * status = NULL );

// create with num_send_threads send threads: zero is the default
// (rocketnet_server_iocp_send_threads), a count from 1 up to the send lanes is
// that count, anything else is refused with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE
// (rocketnet_server_iocp_send_threads_for). rocketnet_server_iocp_create is
// this with zero.
rocketnet_server_iocp_t * rocketnet_server_iocp_create_threads( void * context, rocketnet_address_t * bind_address, int num_workers,
                                                                int num_send_threads, int max_clients
#if ROCKETNET_DEV
                                                                ,
                                                                int fail_stage = 0, int flags = 0
#endif // #if ROCKETNET_DEV
                                                                ,
                                                                int * status = NULL );
void rocketnet_server_iocp_destroy( rocketnet_server_iocp_t * iocp );
void rocketnet_server_iocp_stop( rocketnet_server_iocp_t * iocp );
bool rocketnet_server_iocp_stopped( rocketnet_server_iocp_t * iocp );

uint8_t * rocketnet_server_iocp_start_packet( rocketnet_server_iocp_t * iocp,
                                              const rocketnet_address_t * to, uint64_t * packet_id );
void rocketnet_server_iocp_finish_packet( rocketnet_server_iocp_t * iocp,
                                          uint64_t packet_id, uint8_t * frame, int packet_bytes );
void rocketnet_server_iocp_abort_packet( rocketnet_server_iocp_t * iocp,
                                         uint64_t packet_id, uint8_t * frame );
void rocketnet_server_iocp_send_packets( rocketnet_server_iocp_t * iocp );
// the frames start_packet may still take before the next flush, over every
// lane: each lane's free send operations, at most its producer bank's free
// slots. Main thread.
int rocketnet_server_iocp_send_room( rocketnet_server_iocp_t * iocp );
// The send lanes, the driver's send queue operations: the lane count (fixed
// at create, any thread), the lane a destination stages in (pure, any
// thread; the lane start_packet uses for it), and one lane's room (main
// thread).
int rocketnet_server_iocp_send_lane_count( rocketnet_server_iocp_t * iocp );
int rocketnet_server_iocp_send_lane( rocketnet_server_iocp_t * iocp, const rocketnet_address_t * to );
int rocketnet_server_iocp_send_lane_room( rocketnet_server_iocp_t * iocp, int lane );
// true once nothing is staged in any lane and every send operation is back in
// its lane's free queue: no bank waits for a send worker and no posted send
// waits for its completion. Main thread.
bool rocketnet_server_iocp_send_idle( rocketnet_server_iocp_t * iocp );
void rocketnet_server_iocp_receive_packets( rocketnet_server_iocp_t * iocp );
rocketnet_server_socket_process_packets_t * rocketnet_server_iocp_process_packets( rocketnet_server_iocp_t * iocp );

// The iocp transport's counters are rows of the public table: the
// ROCKETNET_COUNTER_IOCP_* rows of sdk/rocketnet_counters.h, contiguous from
// ROCKETNET_COUNTER_IOCP_SEND_COMPLETED. Any thread: writes those rows of
// counters, an array of ROCKETNET_NUM_COUNTERS values indexed by public
// counter, cumulative for the transport's life; every other value is left as
// it was. The row ROCKETNET_COUNTER_IOCP_PIN_REFUSALS, outside that run, is
// written too: the thread pins Windows refused, the completion workers' and the
// send threads' together.
#define ROCKETNET_SERVER_IOCP_FIRST_COUNTER ROCKETNET_COUNTER_IOCP_SEND_COMPLETED
#define ROCKETNET_SERVER_IOCP_COUNTER_ROWS  ( ROCKETNET_COUNTER_IOCP_USO_FALLBACKS - ROCKETNET_COUNTER_IOCP_SEND_COMPLETED + 1 )

// RECEIVE_DROPPED counts every datagram Windows completed that was not
// banked, so the datagrams Windows delivered = RECEIVE_COMPLETED +
// RECEIVE_DROPPED (a failed URO completion counts one: its datagram count is
// unknown). Its reasons: RECEIVE_TOO_SHORT and RECEIVE_TOO_LARGE for the
// datagrams shorter than ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES and past
// ROCKETNET_RECEIVE_CEILING (the size hook), and the worker
// prefilter's and the full bank's reasons in the drop totals below.

void rocketnet_server_iocp_counters( rocketnet_server_iocp_t * iocp, uint64_t * counters );

// Turns on the completion workers' prefilter: from the next completion batch
// each received datagram of ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES to
// ROCKETNET_RECEIVE_CEILING bytes passes
// rocketnet_receive_prefilter (sdk/rocketnet_receive_prefilter.h) before it
// may take a bank slot, so garbage cannot evict a player's datagram. The
// filter is the server's configured value, fixed for the transport's life:
// one call after create, main thread. Without the call every datagram is
// banked and the protocol filters it on main, as before.
void rocketnet_server_iocp_set_packet_filter( rocketnet_server_iocp_t * iocp, const uint8_t * packet_filter );

// The cumulative drops by reason, ROCKETNET_RECEIVE_NUM_DROPS values in
// rocketnet_receive_drop_t order (ACCEPT 0): the prefilter's refusals and
// ROCKETNET_RECEIVE_DROP_BANK_FULL, a datagram the prefilter accepted (or a
// datagram with the prefilter off) that found no room in its worker's bank.
// The driver's receive_drop_totals operation. Main thread.
void rocketnet_server_iocp_receive_drop_totals( rocketnet_server_iocp_t * iocp, uint64_t * values );
int rocketnet_server_iocp_pending( rocketnet_server_iocp_t * iocp );
// The ROCKETNET_IOCP_FEATURE_* bits in use now. USO clears itself when a live
// segmented send is refused.
int rocketnet_server_iocp_features( rocketnet_server_iocp_t * iocp );
// where a completion worker runs, as the worker found it: PINNED to *mask in
// *group, NOT_ASKED (one worker, or the development knob "none"), REFUSED
// (SetThreadGroupAffinity failed; *error is its Win32 error) or PENDING (not
// reached yet, or no such worker). Create waits for every worker's answer
// and logs it; a start can read it here.
enum
{
    ROCKETNET_IOCP_PLACEMENT_PENDING = 0,
    ROCKETNET_IOCP_PLACEMENT_PINNED = 1,
    ROCKETNET_IOCP_PLACEMENT_NOT_ASKED = 2,
    ROCKETNET_IOCP_PLACEMENT_REFUSED = 3,
};
int rocketnet_server_iocp_worker_placement( rocketnet_server_iocp_t * iocp, int worker, uint64_t * mask, int * group, unsigned long * error );

// The send threads and where each runs, as the thread found it when it pinned
// itself. The count is num_iocp_send_threads (default ROCKETNET_IOCP_SEND_THREADS,
// or the lanes when there are fewer). Thread t is on the core after the last completion worker's, plus
// t (the same in-order rule the workers follow), one logical processor of it,
// so the core's other logical processors stay free. The placement is PINNED to
// *mask in *group, REFUSED (SetThreadGroupAffinity failed; *error is its Win32
// error; the thread runs wherever Windows puts it, is warned by name, counted
// in rocketnet_server_iocp_send_pin_refusals and never reported as pinned) or
// PENDING (not reached yet, or no such thread). Create waits for every
// thread's answer and logs it.
int rocketnet_server_iocp_send_thread_count( rocketnet_server_iocp_t * iocp );
int rocketnet_server_iocp_send_placement( rocketnet_server_iocp_t * iocp, int thread, uint64_t * mask, int * group, unsigned long * error );
// the send threads whose pin was refused, since create
uint64_t rocketnet_server_iocp_send_pin_refusals( rocketnet_server_iocp_t * iocp );

#if ROCKETNET_DEV
// A development test control: flags every later create ORs into its own (the
// ROCKETNET_IOCP_DISABLE_* bits), so a functional case can run the whole server
// transport on the per-datagram path. Zero, the default, changes nothing.
void rocketnet_server_iocp_test_force_flags( int flags );

uintptr_t rocketnet_server_iocp_test_socket( rocketnet_server_iocp_t * iocp );
// TX tests can hold worker service or the short publication mutex separately.
void rocketnet_server_iocp_test_hold_tx( rocketnet_server_iocp_t * iocp, bool hold );
int rocketnet_server_iocp_test_ready_tx_banks( rocketnet_server_iocp_t * iocp );
// Published banks are fully serviced; outstanding kernel sends are separate.
bool rocketnet_server_iocp_test_tx_idle( rocketnet_server_iocp_t * iocp );
// Hold/release pauses completion work through a test-only atomic gate.
void rocketnet_server_iocp_test_hold_completions( rocketnet_server_iocp_t * iocp, bool hold );
// Hold/release the short publication mutexes on one thread. The main receive
// call must return promptly with an empty view while another thread holds them.
void rocketnet_server_iocp_test_hold_publication( rocketnet_server_iocp_t * iocp, bool hold );
int rocketnet_server_iocp_test_ready_banks( rocketnet_server_iocp_t * iocp, int worker_index );
bool rocketnet_server_iocp_test_receive_view_published( rocketnet_server_iocp_t * iocp, const uint8_t * view );
bool rocketnet_server_iocp_test_stopping( rocketnet_server_iocp_t * iocp );
int rocketnet_server_iocp_test_packet_bytes( rocketnet_server_iocp_t * iocp, uint64_t packet_id );
struct rocketnet_server_iocp_test_queue_stats_t
{
    uint64_t free_pushes;
    uint64_t free_pops;
    uint64_t retry_pushes;
    uint64_t retry_pops;
    uint64_t queue_probes;
    int free_count;
    int retry_count;
    int live_run_refs;
};
void rocketnet_server_iocp_test_queue_stats( rocketnet_server_iocp_t * iocp,
                                             rocketnet_server_iocp_test_queue_stats_t * stats );

// The fault seam. The next count events at a fault point fail with status, as
// the host fails them: a send or receive completion reports status as its
// completion status; a send or receive post is refused before the Winsock
// call, with status as its error, and starts no I/O. Any thread may arm it.
enum rocketnet_server_iocp_test_fault_t
{
    ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION,
    ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION,
    ROCKETNET_IOCP_TEST_FAULT_SEND_POST,
    ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST,
    ROCKETNET_IOCP_TEST_NUM_FAULTS
};
void rocketnet_server_iocp_test_fail_next( rocketnet_server_iocp_t * iocp, int fault, int count, int status );
int rocketnet_server_iocp_test_faults_left( rocketnet_server_iocp_t * iocp, int fault );

// The send lanes' free queues alone, for the trace harness (layer
// iocp-lanes): no socket, no thread. Acquire returns the operation index
// taken from the destination's lane, -1 when that lane is empty; release
// returns false when the lane the index names is full.
struct rocketnet_server_iocp_test_lanes_t;
rocketnet_server_iocp_test_lanes_t * rocketnet_server_iocp_test_lanes_create( int lanes, int send_queue_size );
void rocketnet_server_iocp_test_lanes_destroy( rocketnet_server_iocp_test_lanes_t * test );
int rocketnet_server_iocp_test_lanes_acquire( rocketnet_server_iocp_test_lanes_t * test, const rocketnet_address_t * to, int * lane_out );
bool rocketnet_server_iocp_test_lanes_release( rocketnet_server_iocp_test_lanes_t * test, int index );
int rocketnet_server_iocp_test_lanes_free( const rocketnet_server_iocp_test_lanes_t * test, int lane, int * indexes, int max_indexes );
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_IOCP_H
