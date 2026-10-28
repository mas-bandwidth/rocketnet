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

// Windows server transport. The public transport API is unchanged. Fill threads
// acquire and seal directly into transport-owned cold frames. Flush try-publishes
// one of three compact generation-tagged index banks to a dedicated TX worker.
// That worker alone coalesces and posts UDP sends; only completions return
// frames to the bounded free-reference queue.
// Receive workers dequeue completions in batches and publish private four-bank
// buffers. Completion work never runs under the publication mutex; the main
// receive call swaps bank ownership without waiting for that mutex. Refused
// receive posts enter a bounded generation-tagged retry queue; a worker drains
// only positions below the enqueue ticket captured at the start of its pass.

// One completion worker by default, plus one dedicated TX worker. Completion
// workers race on the one completion port and each
// publishes its own buffer, so more than one reorders a client's datagrams
// at the drain, which the reliable layer answers with resends.
#define ROCKETNET_IOCP_DEFAULT_WORKERS 1
#define ROCKETNET_IOCP_MAX_WORKERS 32
// The send frames are shared by all workers, so they are the server-wide send
// staging (2048 at 1000 clients); each worker's receive staging is twice it.
#define ROCKETNET_IOCP_SEND_QUEUE_SIZE ROCKETNET_SERVER_SEND_STAGING_PACKETS
#define ROCKETNET_IOCP_RECEIVES_PER_WORKER 128
#define ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE ( 2 * ROCKETNET_SERVER_SEND_STAGING_PACKETS )

ROCKETNET_STATIC_ASSERT( ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE >= 2 * ROCKETNET_IOCP_SEND_QUEUE_SIZE,
                         "receive budget twice send: IOCP receive queue holds at least twice the send queue" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_IOCP_SEND_QUEUE_SIZE,
                         "receive budget twice send: server process batch holds at least twice the IOCP send queue" );

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
// families and normalize mapped IPv4 sources. Zero workers selects the default.
rocketnet_server_iocp_t * rocketnet_server_iocp_create( void * context,
                                                        rocketnet_address_t * bind_address, int num_workers
#if ROCKETNET_DEV
    // Development constructor controls: fail after socket, port, first worker
    // or first receive, and optionally force the per-datagram fallback paths.
                                                        ,
                                                        int fail_stage = 0, int flags = 0
#endif // #if ROCKETNET_DEV
);
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

void rocketnet_server_iocp_counters( rocketnet_server_iocp_t * iocp, uint64_t * counters );
int rocketnet_server_iocp_pending( rocketnet_server_iocp_t * iocp );
// The ROCKETNET_IOCP_FEATURE_* bits in use now. USO clears itself when a live
// segmented send is refused.
int rocketnet_server_iocp_features( rocketnet_server_iocp_t * iocp );

#if ROCKETNET_DEV
// A development test control: flags every later create ORs into its own (the
// ROCKETNET_IOCP_DISABLE_* bits), so a functional case can run the whole server
// transport on the per-datagram path. Zero, the default, changes nothing.
void rocketnet_server_iocp_test_force_flags( int flags );
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV && defined( ROCKETNET_IOCP_TESTS )
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
#endif // #if ROCKETNET_DEV && defined( ROCKETNET_IOCP_TESTS )

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_IOCP_H
