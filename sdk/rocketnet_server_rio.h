/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#pragma once

#include "rocketnet_constants.h"
#include "rocketnet_packet_batch.h"
#include "rocketnet_wire.h"

// The Windows Registered I/O (RIO) server transport. One UDP socket opened with
// WSA_FLAG_REGISTERED_IO, one registered region carved by
// sdk/rocketnet_rio_layout.h into send frames, send address slots, receive
// slices and receive address slots, one send completion queue, one receive
// completion queue and one request queue.
//
// RIO queues are unsynchronized and sends and receives share the request
// queue, so after create one background I/O worker makes every RIO call:
// receive dequeue and re-post, send post and commit, send retirement. It
// waits on RIONotify for both completion queues and on a wake from main. The
// main thread makes no RIO call and never waits: it exchanges bank indices
// and phase states with the worker under one try of a short mutex.
//
// Send: three staging phases of ROCKETNET_RIO_SEND_QUEUE_SIZE frame slots.
// start_packet reserves the next slot of the fill phase with one atomic
// fetch_add; send_packets (fill threads joined) hands the fill phase to the
// worker and opens the next phase only when the worker has returned that
// phase idle (every send of it retired); until then the fill phase stays
// open and start_packet refuses only when it is full. Receive: every
// slice is posted at create; the worker copies each completion into its fill
// bank, re-posts the slice and publishes the bank onto a FIFO of ready banks;
// receive_packets exposes the ready banks that fit the process view, valid
// until the next call. With every bank full the worker stops dequeuing, so
// completions wait in the queue rather than being dropped. Stop requests,
// the worker commits deferred sends, closes the socket and drains both
// queues; stopped reports that drain before destroy frees the region.
// tla/RNRioWorker.tla models the design (tla/RNRioDriver.tla the earlier
// caller-driven one); the functions that implement a model action cite it.
//
// The transport is built only on Windows (Windows 10 1709 or later for
// RIOSendEx and RIOReceiveEx). Elsewhere the module is not compiled: only
// rocketnet_server_rio_available() is defined, and it is false.

// One socket serves every client: each send phase is the server-wide send
// staging (2048 at 1000 clients) and the posted receives are twice it.
#define ROCKETNET_RIO_SEND_QUEUE_SIZE ROCKETNET_SERVER_SEND_STAGING_PACKETS
#define ROCKETNET_RIO_RECEIVES ( 2 * ROCKETNET_SERVER_SEND_STAGING_PACKETS )

ROCKETNET_STATIC_ASSERT( ROCKETNET_RIO_RECEIVES >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE,
                         "receive budget twice send: RIO receives hold at least twice the send queue" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE,
                         "receive budget twice send: server process batch holds at least twice the RIO send queue" );
#define ROCKETNET_RIO_RECEIVE_SLICE_BYTES 2048

struct rocketnet_server_rio_t;

// bind_address is updated with the actual port. IPv6 binds are dual stack
// and normalize mapped IPv4 sources to IPv4.
rocketnet_server_rio_t * rocketnet_server_rio_create( void * context,
    rocketnet_address_t * bind_address
#if ROCKETNET_DEV
    // Development constructor control: fail after the socket, RIO table,
    // region, completion queues, request queue or first posted receive.
    , int fail_stage = 0
#endif
    );
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
void rocketnet_server_rio_receive_packets( rocketnet_server_rio_t * rio );
rocketnet_server_socket_process_packets_t * rocketnet_server_rio_process_packets( rocketnet_server_rio_t * rio );

enum rocketnet_server_rio_counter_t
{
    ROCKETNET_RIO_COUNTER_PACKETS_SENT,             // send completions with a good status
    ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED,         // datagrams copied into a receive bank
    ROCKETNET_RIO_COUNTER_SEND_FULL,                // start_packet found the fill phase full
    ROCKETNET_RIO_COUNTER_SEND_FAILED,              // a refused post or a failed send completion
    ROCKETNET_RIO_COUNTER_SEND_WAITS,               // flushes that kept the fill phase open: the next phase not yet idle
    ROCKETNET_RIO_COUNTER_RECEIVE_FAILED,           // receive completions with a failed status
    ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE,     // datagrams outside the wire's size bounds
    ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT,        // datagrams below the wire's minimum size
    ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE,        // datagrams past the receive ceiling
    ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW, // datagrams past the fill bank's capacity (the bounded dequeue keeps it zero)
    ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED,    // slices retired by a refused re-post
    ROCKETNET_RIO_COUNTER_COMMITS,                  // RIO_MSG_COMMIT_ONLY calls
    ROCKETNET_RIO_NUM_COUNTERS
};

void rocketnet_server_rio_counters( rocketnet_server_rio_t * rio, uint64_t * counters );
// posted sends and receives the kernel has not completed or the transport has
// not yet dequeued
int rocketnet_server_rio_pending( rocketnet_server_rio_t * rio );
uint64_t rocketnet_server_rio_allocation_bytes();

#if ROCKETNET_DEV && defined(ROCKETNET_RIO_TESTS)
uintptr_t rocketnet_server_rio_test_socket( rocketnet_server_rio_t * rio );
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
#endif

#if defined(_WIN32)
bool rocketnet_server_rio_available();
#else
inline bool rocketnet_server_rio_available() { return false; }
#endif
