/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_IOCP_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_IOCP_H

#include "rocketnet_constants.h"
#include "rocketnet_packet_batch.h"
#include "rocketnet_wire.h"

// Windows server transport. The public transport API is unchanged. The send
// side is lanes, the XDP shape: each lane owns its staging, its pool of send
// operations and its TX worker, and a client's packets always stage in the
// lane its address hashes to (rocketnet_server_iocp_send_lane), so one lane
// carries every packet of a client, in order. Fill threads acquire and seal
// directly into the lane's transport-owned cold frames. Flush try-publishes
// one of each lane's three compact generation-tagged index banks to that
// lane's TX worker, which alone cuts the bank into runs of one destination
// (grouped by the send gather, sdk/rocketnet_send_gather.h), coalesces and posts its
// UDP sends; only completions return frames to the lane's bounded
// free-reference queue.
// Receive workers dequeue completions in batches, refuse what the stateless
// prefilter names before a datagram takes a bank slot, and publish private
// four-bank buffers. Completion work never runs under the publication mutex;
// the main receive call swaps bank ownership without waiting for that mutex.
// Refused receive posts enter a bounded generation-tagged retry queue; a
// worker drains only positions below the enqueue ticket captured at the start
// of its pass.

// One completion worker by default, on one socket bound exclusively. More
// than one worker (the server config's num_xdp_queues) is as many endpoints:
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
#define ROCKETNET_IOCP_DEFAULT_WORKERS 1
#define ROCKETNET_IOCP_MAX_WORKERS 32

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

// one lane's staging and operation pool for sized clients (a power of two):
// a fair share of the rate, at most ROCKETNET_IOCP_LANE_PACKETS, and never
// less than every client slot plus the headroom. A function of the sized
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

enum rocketnet_server_iocp_counter_t
{
    ROCKETNET_IOCP_SEND_COMPLETED,
    ROCKETNET_IOCP_SEND_FAILED,
    ROCKETNET_IOCP_RECEIVE_COMPLETED,
    ROCKETNET_IOCP_RECEIVE_DROPPED,
    ROCKETNET_IOCP_RECEIVE_TOO_SHORT,
    ROCKETNET_IOCP_RECEIVE_TOO_LARGE,
    ROCKETNET_IOCP_RECEIVE_REPOST_FAILED,
    ROCKETNET_IOCP_COMPLETION_BATCHES,
    ROCKETNET_IOCP_USO_SENDS,               // posts that carried more than one frame
    ROCKETNET_IOCP_URO_BATCHES,             // receive completions split into more than one segment
    ROCKETNET_IOCP_USO_FALLBACKS,           // live USO sends refused and resent frame by frame
    ROCKETNET_IOCP_NUM_COUNTERS
};

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
