/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_SOCKET_H
#define ROCKETNET_SERVER_SOCKET_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"
#include "rocketnet_packet_batch.h"

struct rocketnet_server_socket_t;

#define ROCKETNET_SERVER_SOCKET_STOPPED    0
#define ROCKETNET_SERVER_SOCKET_RUNNING    1
#define ROCKETNET_SERVER_SOCKET_STOPPING   2

// The non-mutating pre-flight (#106, ruled on the #110 review): checks the
// machine IS provisioned for the XDP data plane and refuses by name when it
// is not -- each unmet condition logs the knob, the wanted value, and
// scripts/rocketnet-provision, whose header carries the authoritative
// condition list both this check and that script derive from. Read-only by
// contract: it may create and close probe descriptors (an AF_XDP socket
// never bound, a BPF program never attached) but never changes machine
// state. create() runs it first and refuses without it; the transport ladder
// and the test suite call it directly so a refusal is named without ever
// touching the committing path.

bool rocketnet_server_socket_preflight( const char * public_address_string, int num_queues );

// The commit barrier: a process that must never commit an XDP data plane
// forbids the committing create outright -- the test suite installs this
// once at its start, structurally, so every present and future test is
// safe by default; the pre-flight stays reachable (non-mutating by
// contract) and create refuses before its first act. allow_commit is the
// deliberate opt-out; nothing in the suite calls it.

void rocketnet_server_socket_forbid_commit();

void rocketnet_server_socket_allow_commit();

// the R4 interrupt-line matcher, pure string logic exported for the
// fixture test: does this /proc/interrupts line belong to this interface?
// bounded-token semantics -- '-' allowed before (transport-prefixed names
// like i40e-ens1f0-TxRx-0), never a bare substring ("lo" must not match
// "velocity0").

bool rocketnet_server_socket_interrupt_line_matches_interface( const char * line, const char * interface_name );

// The filter value configures the kernel prefilter: only packets carrying it
// reach the AF_XDP rings. It is the same value the protocol object checks, at the same epoch.
//
// create is VERIFY AND BIND only: the pre-flight above, then the library's
// own per-socket acts -- building and loading its BPF program, attaching
// it, AF_XDP binds, the memlock rlimit. Machine provisioning (xdp-loader
// clearing, SMT, napi tuning, ethtool -L queue counts, isolcpus guidance)
// lives in scripts/rocketnet-provision, run deliberately by the operator;
// create never mutates machine state.

// Requested XDP component bytes (userspace queues + UMEM), excluding kernel
// bookkeeping, thread stacks and allocator overhead; no privileged operations.
uint64_t rocketnet_server_socket_memory_required(int num_queues);

struct rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * server_address, const uint8_t * packet_filter, int num_xdp_queues = 7, int num_os_cpus = 1, const char * second_server_address = NULL, int * create_status = NULL );

void rocketnet_server_socket_destroy( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_update( struct rocketnet_server_socket_t * server_socket );

// The replay high-water writer: the server socket is the only writer of the
// pinned server_xdp_replay_map. These calls are main thread only. Forget and
// mark append to the socket's pending buffer; a full buffer refuses the entry
// and the next commit adds the refusal to the commit failure count. Commit
// deletes every forget, then updates every mark, then resets the buffer,
// keeping every forget the kernel refused (anything but a missing key) to be
// tried first in the next commit. A failed syscall adds to the commit failure
// count and logs one debug line naming the call, never a key. Forget returns
// false when the pending buffer is full, so the walk keeps its slot record.

bool rocketnet_server_socket_replay_forget( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address );

// The transport accepted receive-view packet view_index for a connection:
// its source MAC becomes the reply MAC for its address (rocketnet_xdp_frame.h,
// the reply-MAC rule). Main thread only, while that view is current.

void rocketnet_server_socket_client_accepted( struct rocketnet_server_socket_t * server_socket, int view_index );

void rocketnet_server_socket_replay_mark( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address, uint32_t high_water );

void rocketnet_server_socket_replay_commit( struct rocketnet_server_socket_t * server_socket );

uint64_t rocketnet_server_socket_replay_commit_failures( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_stop( struct rocketnet_server_socket_t * server_socket );
bool rocketnet_server_socket_stopped( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_state( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_num_queues( struct rocketnet_server_socket_t * server_socket );

// The kernel prefilter's counters, summed across CPUs into an array of
// ROCKETNET_SERVER_XDP_NUM_COUNTERS values -- what the XDP program dropped
// and why, and what it redirected, before userspace ever saw a byte.

void rocketnet_server_socket_xdp_counters( struct rocketnet_server_socket_t * server_socket, uint64_t * counters );

// send packets (staged, one copy each way)

uint8_t * rocketnet_server_socket_start_packet( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_socket_finish_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_socket_abort_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_socket_send_packets( struct rocketnet_server_socket_t * server_socket );

// receive packets (staged, one copy each way)

void rocketnet_server_socket_receive_packets( struct rocketnet_server_socket_t * server_socket );



struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket );

#endif // #ifndef ROCKETNET_SERVER_SOCKET_H
