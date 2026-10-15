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
// state. create() runs it first and refuses without it; the driver ladder
// and the test suite call it directly so a refusal is named without ever
// touching the committing path.

bool rocketnet_server_socket_preflight( const char * public_address_string, int num_queues );

// The commit barrier: a process that must never commit an XDP data plane
// forbids the committing create outright -- the test suite installs this
// once at its start, structurally, so every present and future test is
// safe by default; the pre-flight stays reachable (non-mutating by
// contract) and create refuses before its first act. allow_commit is the
// deliberate opt-out; only the xdp veth cases in the functional suite call
// it, and only when the operator opts in (ROCKETNET_TEST_XDP_VETH=1, root).

void rocketnet_server_socket_forbid_commit();

void rocketnet_server_socket_allow_commit();

bool rocketnet_server_socket_commit_forbidden();

// the R4 interrupt-line matcher, pure string logic exported for the
// fixture test: does this /proc/interrupts line belong to this interface?
// bounded-token semantics -- '-' allowed before (driver-prefixed names
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

struct rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * server_address, const uint8_t * filter_value, int num_xdp_queues = 7, int num_os_cpus = 1 );

void rocketnet_server_socket_destroy( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_update( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_stop( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_state( struct rocketnet_server_socket_t * server_socket );

uint64_t rocketnet_server_socket_id( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_num_queues( struct rocketnet_server_socket_t * server_socket );

// The kernel prefilter's counters, summed across CPUs into an array of
// ROCKETNET_SERVER_XDP_NUM_COUNTERS values -- what the XDP program dropped
// and why, and what it redirected, before userspace ever saw a byte.

void rocketnet_server_socket_xdp_counters( struct rocketnet_server_socket_t * server_socket, uint64_t * counters );

// The driver's userspace counters, summed across queues: what the workers
// handed up and sent, and every frame the XDP program redirected but
// userspace refused, by reason -- so a redirect the driver cannot use is
// counted and named, never hidden. One server serves one family (the
// family of its public address): a frame or a destination of the other
// family is refused and counted here.

#define ROCKETNET_SERVER_SOCKET_COUNTER_RECEIVED                    0   // frames parsed and handed to the transport
#define ROCKETNET_SERVER_SOCKET_COUNTER_RECEIVE_REFUSED_PARSE       1   // the frame codec refused it, or it is shorter than any server packet
#define ROCKETNET_SERVER_SOCKET_COUNTER_RECEIVE_REFUSED_FAMILY      2   // a frame of the family this server does not serve
#define ROCKETNET_SERVER_SOCKET_COUNTER_SENT                        3   // frames built and queued for transmit
#define ROCKETNET_SERVER_SOCKET_COUNTER_SEND_REFUSED_FAMILY         4   // a destination of the family this server does not serve
#define ROCKETNET_SERVER_SOCKET_COUNTER_SEND_REFUSED_BUILD          5   // the frame codec refused to build it
#define ROCKETNET_SERVER_SOCKET_NUM_COUNTERS                        6

void rocketnet_server_socket_driver_counters( struct rocketnet_server_socket_t * server_socket, uint64_t * counters );

const char * rocketnet_server_socket_counter_name( int counter );

// Both counter sets of the most recently created server socket in this
// process that is not yet destroyed: the kernel prefilter's
// (ROCKETNET_SERVER_XDP_NUM_COUNTERS values) and the driver's
// (ROCKETNET_SERVER_SOCKET_NUM_COUNTERS values). False when there is none.
// The functional suite reads a transport's xdp driver through this.

bool rocketnet_server_socket_live_counters( uint64_t * xdp_counters, uint64_t * driver_counters );

// send packets (zero copy)

uint8_t * rocketnet_server_socket_start_packet( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_socket_finish_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_socket_abort_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_socket_send_packets( struct rocketnet_server_socket_t * server_socket );

// receive packets (zero copy)

void rocketnet_server_socket_receive_packets( struct rocketnet_server_socket_t * server_socket );



struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket );

#endif // #ifndef ROCKETNET_SERVER_SOCKET_H
