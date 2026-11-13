/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_RIO_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_RIO_H

#include "rocketnet_constants.h"
#include "rocketnet_packet_batch.h"
#include "rocketnet_wire.h"
#include "rocketnet_rio_layout.h"

// The Windows Registered I/O (RIO) server transport. It runs one or more
// lanes (the server's num_xdp_queues, default 1, at most
// ROCKETNET_RIO_MAX_LANES). A lane is one UDP socket opened with
// WSA_FLAG_REGISTERED_IO, one registered region carved by
// sdk/rocketnet_rio_layout.h into send frames, send address slots, receive
// slices and receive address slots, one send completion queue, one receive
// completion queue, one request queue and one I/O worker. One worker's
// RIOSendEx path tops out near 210k datagrams a second and one socket's
// request queue near 650k however many threads drive it, and RIO refuses a
// second request queue on a socket, so the rate past that is more sockets.
//
// With one lane the socket binds with SO_EXCLUSIVEADDRUSE, to any address and
// port. With several, the address must be specific (create refuses 0.0.0.0,
// :: and any IPv4-mapped address with ROCKETNET_STATUS_INVALID_ADDRESS), each
// lane takes a processor (the first logical processor of core i for lane i,
// the processor Windows' default RSS layout gives receive queue i), its socket
// is given that processor with SIO_CPU_AFFINITY before bind, every lane socket
// binds the same address and port, and the lane's worker is pinned to the
// other logical processors of that processor's core (the RSS processor keeps
// the receive DPCs). The adapter's RSS then delivers each flow to the lane on the
// flow's RSS processor (measured on ENA: 256 flows spread over 8 lanes); a
// flow whose RSS processor has no lane goes to the first lane bound. A
// refused SIO_CPU_AFFINITY refuses create (TRANSPORT_BIND_REFUSED, named in
// the log); there is no fallback to a shared or exclusive bind. For an
// explicit port create first binds a plain probe socket there with
// SO_EXCLUSIVEADDRUSE, refusing if anything holds the port, and closes it.
// For port 0, lane 0 binds port 0 and the others join the port it was given:
// the guarantee that nothing else holds that port rests on Windows never
// handing a port-0 bind a held port (checked on every run by
// test_rio_transport_lanes_port_zero_never_held). While the lanes run, a later
// plain or SO_REUSEADDR bind of their address and port is refused, but a
// socket of another process that also sets SIO_CPU_AFFINITY can join the port
// and receive the flows RSS hands its processor: Windows has no exclusive
// form of a port shared by processor (tests/test_iocp.cpp pins all three). A
// frame is staged in the lane its destination address hashes to
// (rocketnet_rio_lane_hash), so one client's frames leave through one lane in
// order. Every lane posts receives and banks what it gets, and
// receive_packets appends every lane's ready banks to one process view.
//
// Each lane's worker runs the stateless receive prefilter
// (sdk/rocketnet_receive_prefilter.h) before a datagram takes a bank slot and
// counts each refusal in its own tally; receive_drop_totals sums them.
//
// Within a lane: RIO queues are unsynchronized and sends and receives share the request
// queue, so after create one background I/O worker makes every RIO call:
// receive dequeue and re-post, send post and commit, send retirement. It
// waits on RIONotify for both completion queues and on a wake from main. The
// main thread makes no RIO call and never waits: it exchanges bank indices
// and phase states with the worker under one try of a short mutex.
//
// Send: three staging phases of rocketnet_server_rio_send_queue_size frame slots.
// start_packet reserves the next slot of the fill phase with one atomic
// fetch_add; send_packets (fill threads joined) hands the fill phase to the
// worker and opens the next phase only when the worker has returned that
// phase idle (every send of it retired; each worker pass retires the send
// completions before it takes a phase, so a phase the kernel has finished is
// idle by the next flush); until then the fill phase stays
// open and start_packet refuses only when it is full. Receive: every
// slice is posted at create; the worker copies each completion into its fill
// bank, re-posts the slice and publishes the bank onto a FIFO of ready banks;
// receive_packets exposes the ready banks that fit the process view, valid
// until the next call. With every bank full the worker stops dequeuing, so
// completions wait in the queue rather than being dropped. Stop requests,
// the worker commits deferred sends, closes the socket and drains both
// queues; stopped reports that drain before destroy frees the region.
// proofs/tla/RNRioWorker.tla models the design; the functions that implement a
// model action cite it.
//
// The transport is built only on Windows (Windows 10 1709 or later for
// RIOSendEx and RIOReceiveEx). Elsewhere the module is not compiled: only
// rocketnet_server_rio_available() is defined, and it is false.

// Each lane's send phase is the server-wide send staging for the configured
// max_clients, and at least ROCKETNET_RIO_MIN_SEND_QUEUE_SIZE (16384 at 1000
// clients): the packets a lane starts per flush. A server of 1000 players at
// 60 Hz writes about 15 packets per player per frame at peak (15000 a frame),
// so two lanes carry 7500 each on average; 16384 holds the uneven share the
// destination hash gives a lane with margin. 4096 refused 558k starts a
// second at 2 lanes. Each lane's registered send region is three phases of
// this many frames. Each lane's posted receives are twice the staging (the
// input is small and arrives on the first-bound or steered lane), not the send
// queue. Both are sized at create.
#define ROCKETNET_RIO_MIN_SEND_QUEUE_SIZE 16384

static inline int rocketnet_server_rio_send_queue_size( int max_clients )
{
    const int staging = rocketnet_server_send_staging_packets( max_clients );
    return staging > ROCKETNET_RIO_MIN_SEND_QUEUE_SIZE ? staging : ROCKETNET_RIO_MIN_SEND_QUEUE_SIZE;
}

static inline int rocketnet_server_rio_receives( int max_clients )
{
    return 2 * rocketnet_server_send_staging_packets( max_clients );
}

// The receive budget twice send, at the smallest sizing count: a phase is the
// staging (at most four packets per sized client) or the floor, and the
// process batch holds sixteen per sized client, so the relation holds at
// every count.
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_SERVER_MIN_SIZED_CLIENTS ),
                         "receive budget twice send: server process batch holds at least twice the RIO send queue" );
#define ROCKETNET_RIO_RECEIVE_SLICE_BYTES 2048

struct rocketnet_server_rio_t;

// lanes: 0 or 1 is one lane; at most ROCKETNET_RIO_MAX_LANES.
// packet_filter: the server's ROCKETNET_FILTER_VALUE_BYTES filter value, copied
// at create; NULL runs no prefilter.
struct rocketnet_server_rio_options_t
{
    int lanes;
    const uint8_t * packet_filter;
};

// bind_address is updated with the actual port. IPv6 binds are dual stack
// and normalize mapped IPv4 sources to IPv4. max_clients is the server's
// configured client count: it sizes each lane's send phases, receive slices
// and banks, the process view, each registered region and each socket
// receive buffer request. options NULL is one lane and no prefilter.
rocketnet_server_rio_t * rocketnet_server_rio_create( void * context,
                                                      rocketnet_address_t * bind_address, int max_clients
#if ROCKETNET_DEV
    // Development constructor control: fail after the socket, RIO table,
    // region, completion queues, request queue, first posted receive or last
    // posted receive (stages 1..7), in lane 0; stage 8 refuses the bind with
    // WSAENOBUFS. A stage plus 16 * L fails lane L instead, once lanes
    // 0..L-1 are made and running.
                                                      ,
                                                      int fail_stage = 0
#endif // #if ROCKETNET_DEV
                                                      ,
                                                      int * status = NULL, const rocketnet_server_rio_options_t * options = NULL );
void rocketnet_server_rio_destroy( rocketnet_server_rio_t * rio );
void rocketnet_server_rio_stop( rocketnet_server_rio_t * rio );
bool rocketnet_server_rio_stopped( rocketnet_server_rio_t * rio );

uint8_t * rocketnet_server_rio_start_packet( rocketnet_server_rio_t * rio,
                                             const rocketnet_address_t * to, uint64_t * packet_id );
void rocketnet_server_rio_finish_packet( rocketnet_server_rio_t * rio,
                                         uint64_t packet_id, uint8_t * frame, int packet_bytes );
void rocketnet_server_rio_abort_packet( rocketnet_server_rio_t * rio,
                                        uint64_t packet_id, uint8_t * frame );
void rocketnet_server_rio_send_packets( rocketnet_server_rio_t * rio );
// the slots start_packet may still take in the open fill phase of the
// fullest lane, so a frame to any destination fits. Main thread.
int rocketnet_server_rio_send_room( rocketnet_server_rio_t * rio );
// the lanes, the lane a destination's frames are staged in, and one lane's
// send_room (the server's per-queue protocol send reserve)
int rocketnet_server_rio_lanes( rocketnet_server_rio_t * rio );
// where a lane's worker runs, as its worker found it: PINNED to *mask in
// *group, NOT_ASKED (one lane, or the development knob "none"), REFUSED
// (SetThreadGroupAffinity failed; *error is its Win32 error) or PENDING (the
// worker has not reached it yet). Create waits for every lane's answer and
// logs it; a start can read it here.
enum
{
    ROCKETNET_RIO_PLACEMENT_PENDING = 0,
    ROCKETNET_RIO_PLACEMENT_PINNED = 1,
    ROCKETNET_RIO_PLACEMENT_NOT_ASKED = 2,
    ROCKETNET_RIO_PLACEMENT_REFUSED = 3,
};
int rocketnet_server_rio_lane_placement( rocketnet_server_rio_t * rio, int lane, uint64_t * mask, int * group, unsigned long * error );
// the lanes whose failed create gave up its bounded drain and leaked its
// socket, registered region and queues to the kernel, in this process
uint64_t rocketnet_server_rio_create_leaks();
// the sockets a failed create left open, in this process: closesocket refused
// on every pass of the drain with nothing posted and no send outstanding, so
// the rest of the lane was freed and only the handle stays
uint64_t rocketnet_server_rio_create_abandoned_sockets();
// the completion queues RIODequeueCompletion reported corrupt, in this
// process: each lane with one never reports stopped and is never freed
uint64_t rocketnet_server_rio_corrupt_queues();
int rocketnet_server_rio_lane_of( rocketnet_server_rio_t * rio, const rocketnet_address_t * to );
int rocketnet_server_rio_lane_send_room( rocketnet_server_rio_t * rio, int lane );
// true once the open fill phase is empty, every other phase is idle and no
// posted send waits for its completion. Main thread; a mutex held by the
// worker reads as not idle.
bool rocketnet_server_rio_send_idle( rocketnet_server_rio_t * rio );
void rocketnet_server_rio_receive_packets( rocketnet_server_rio_t * rio );
rocketnet_server_socket_process_packets_t * rocketnet_server_rio_process_packets( rocketnet_server_rio_t * rio );

enum rocketnet_server_rio_counter_t
{
    ROCKETNET_RIO_COUNTER_PACKETS_SENT,             // send completions with a good status
    ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED,         // datagrams copied into a receive bank
    ROCKETNET_RIO_COUNTER_SEND_FULL,                // start_packet found the fill phase full
    ROCKETNET_RIO_COUNTER_SEND_FAILED,              // a refused start, finish or post, or a failed send completion
    ROCKETNET_RIO_COUNTER_SEND_WAITS,               // flushes that kept the fill phase open: the next phase not yet idle
    ROCKETNET_RIO_COUNTER_RECEIVE_FAILED,           // receive completions with a failed status
    ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE,     // datagrams outside the wire's size bounds
    ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT,        // datagrams below the wire's minimum size
    ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE,        // datagrams past the receive ceiling
    ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW, // datagrams past the fill bank's capacity (the bounded dequeue keeps it zero)
    ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED,    // slices retired by a refused re-post
    ROCKETNET_RIO_COUNTER_COMMITS,                  // RIO_MSG_COMMIT_ONLY calls
    ROCKETNET_RIO_COUNTER_RECEIVE_BANK_WAITS,       // worker passes that found the fill bank full (no free bank): the receive backpressure
    ROCKETNET_RIO_COUNTER_RECEIVE_RAN_DRY,          // worker passes that took a whole lane of receive completions: the kernel had none posted
    ROCKETNET_RIO_COUNTER_RECEIVE_VIEW_WAITS,       // main receive calls that left a ready bank for the next update: the process view was full
    ROCKETNET_RIO_NUM_COUNTERS
};

// summed over every lane
void rocketnet_server_rio_counters( rocketnet_server_rio_t * rio, uint64_t * counters );
// ROCKETNET_RECEIVE_NUM_DROPS values in rocketnet_receive_drop_t order: every
// lane worker's prefilter refusals and bank-full drops, summed. Main thread.
void rocketnet_server_rio_receive_drop_totals( rocketnet_server_rio_t * rio, uint64_t * values );
// posted sends and receives the kernel has not completed or the transport has
// not yet dequeued
int rocketnet_server_rio_pending( rocketnet_server_rio_t * rio );
// the allocator bytes create holds for lanes lanes (as in
// rocketnet_server_rio_options_t), past the registered regions: one per lane,
// each rocketnet_server_rio_registered_region_bytes (sdk/rocketnet_memory.h)
uint64_t rocketnet_server_rio_allocation_bytes( int max_clients, int lanes = 1 );
// the lane count create runs for a lanes option: 1 for 0 or 1, else lanes;
// 0 for a value it refuses (below 0 or past ROCKETNET_RIO_MAX_LANES)
int rocketnet_server_rio_lane_count( int lanes );

#if ROCKETNET_DEV
// lane 0's socket
uintptr_t rocketnet_server_rio_test_socket( rocketnet_server_rio_t * rio );
// the waits between drain passes that failed creates have made in this process
int rocketnet_server_rio_test_create_drain_waits();
// a failed multi-lane create's rollback holds every made lane's sends, so none
// stops and its bounded wait gives up and leaks it (counted)
void rocketnet_server_rio_test_set_hold_rollback( bool hold );
// releases the lane a held rollback leaked, waits for it to stop and frees it
bool rocketnet_server_rio_test_finish_rollback_leak();
// The fault seam, compiled only into the test executable. The next count
// events at a fault point fail with status, as the host fails them: a send or
// receive completion reports status as its completion status; a send or
// receive post is refused before the RIO call, with status as its error, and
// starts no I/O; a commit is refused the same way. Set from the test thread,
// consumed by the I/O worker.
enum rocketnet_server_rio_test_fault_t
{
    ROCKETNET_RIO_TEST_FAULT_SEND_COMPLETION,
    ROCKETNET_RIO_TEST_FAULT_RECEIVE_COMPLETION,
    ROCKETNET_RIO_TEST_FAULT_SEND_POST,
    ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST,
    ROCKETNET_RIO_TEST_FAULT_COMMIT,
    ROCKETNET_RIO_TEST_NUM_FAULTS
};
// the fault seams act on lane 0; hold and handed act on every lane
// lanes every RIO server in this process has made, so a test reads the lanes a
// server it made through any path really has
#define ROCKETNET_SERVER_RIO_TEST_LANES_MADE 1
uint64_t rocketnet_server_rio_test_lanes_made();
// RIO transports this process has created whole (every lane made)
uint64_t rocketnet_server_rio_test_transports_made();
void rocketnet_server_rio_test_fail_next( rocketnet_server_rio_t * rio, int fault, int count, int status );
int rocketnet_server_rio_test_faults_left( rocketnet_server_rio_t * rio, int fault );
// receive slices whose re-post was refused and that wait for the worker's
// next pass to re-post them
int rocketnet_server_rio_test_retry_receives( rocketnet_server_rio_t * rio );
// Hold worker send pickup and stop processing until released.
void rocketnet_server_rio_test_hold_send( rocketnet_server_rio_t * rio, bool hold );
// Observe the FIFO under the mutex while the test holds send pickup.
int rocketnet_server_rio_test_handed_phases( rocketnet_server_rio_t * rio );
// nothing is staged in the fill phase, no handed phase waits for the I/O
// worker and no posted phase has a send outstanding (takes the transport
// mutex: the main test thread only)
bool rocketnet_server_rio_test_send_idle( rocketnet_server_rio_t * rio );
#endif // #if ROCKETNET_DEV

#if defined( _WIN32 )
bool rocketnet_server_rio_available();
#else
inline bool rocketnet_server_rio_available() { return false; }
#endif // #if defined( _WIN32 )

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_RIO_H
