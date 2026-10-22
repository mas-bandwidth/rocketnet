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
// slices and receive address slots, one polled send completion queue, one
// polled receive completion queue and one request queue. Everything runs on
// the main thread; there is no worker thread.
//
// Send: two staging phases of ROCKETNET_RIO_SEND_QUEUE_SIZE frame slots.
// start_packet reserves the next slot of the fill phase with one atomic
// fetch_add; send_packets (fill threads joined) retires completed sends,
// posts every finished slot of the fill phase with RIO_MSG_DEFER and one
// commit, and switches the fill phase to the other phase once that phase has
// no outstanding send. Receive: every slice is posted at create;
// receive_packets copies each completion into the fill bank of two receive
// banks, re-posts the slice, then swaps the banks. Stop commits deferred sends
// and closes the socket; stopped drains both completion queues before destroy
// frees the registered region.
// tla/RNRioDriver.tla models the design; the functions that implement a
// model action cite it.
//
// The transport is built only on Windows (Windows 10 1709 or later for
// RIOSendEx and RIOReceiveEx). Elsewhere the module is not compiled: only
// rocketnet_server_rio_available() is defined, and it is false.

#define ROCKETNET_RIO_SEND_QUEUE_SIZE 2048
#define ROCKETNET_RIO_RECEIVES 4096

ROCKETNET_STATIC_ASSERT( ROCKETNET_RIO_RECEIVES >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE,
                         "receive budget twice send: RIO receives hold at least twice the send queue" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE,
                         "receive budget twice send: server process batch holds at least twice the RIO send queue" );
#define ROCKETNET_RIO_RECEIVE_SLICE_BYTES 2048

struct rocketnet_server_rio_t;

// bind_address is updated with the actual port. IPv6 binds are dual stack
// and normalize mapped IPv4 sources to IPv4. fail_stage is a constructor test
// control: 1 after the socket, 2 after the RIO function table, 3 after the
// region is registered, 4 after the completion queues, 5 after the request
// queue, 6 after the first posted receive. Production passes zero.
rocketnet_server_rio_t * rocketnet_server_rio_create( void * context,
    rocketnet_address_t * bind_address, int fail_stage = 0 );
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
    ROCKETNET_RIO_COUNTER_SEND_WAITS,               // empty polls waiting for the other phase to drain
    ROCKETNET_RIO_COUNTER_RECEIVE_FAILED,           // receive completions with a failed status
    ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE,     // datagrams outside the wire's size bounds
    ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW, // datagrams past the fill bank's capacity
    ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED,    // slices retired by a refused re-post
    ROCKETNET_RIO_COUNTER_COMMITS,                  // RIO_MSG_COMMIT_ONLY calls
    ROCKETNET_RIO_NUM_COUNTERS
};

void rocketnet_server_rio_counters( rocketnet_server_rio_t * rio, uint64_t * counters );
// posted sends and receives the kernel has not completed or the transport has
// not yet dequeued
int rocketnet_server_rio_pending( rocketnet_server_rio_t * rio );
uint64_t rocketnet_server_rio_allocation_bytes();
// Historical diagnostic retained for internal consumers. Pending shutdowns
// now retain their caller-owned handle until completion rather than leaking.
uint64_t rocketnet_server_rio_leaked_regions();

#if defined(ROCKETNET_RIO_TESTS)
// The fault seam, compiled only into the test executable. The next count
// events at a fault point fail with status, as the host fails them: a send or
// receive completion reports status as its completion status; a send or
// receive post is refused before the RIO call, with status as its error, and
// starts no I/O; a commit is refused the same way. Main thread only.
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
// receive slices whose re-post was refused and that wait for the next
// receive_packets to re-post them
int rocketnet_server_rio_test_retry_receives( rocketnet_server_rio_t * rio );
#endif

#if defined(_WIN32)
bool rocketnet_server_rio_available();
#else
inline bool rocketnet_server_rio_available() { return false; }
#endif
