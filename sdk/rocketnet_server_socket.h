/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_SOCKET_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_SOCKET_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"
#include "rocketnet_packet_batch.h"

struct rocketnet_server_socket_t;

#if defined( __cplusplus )
#include <atomic>

// Claims the next staging slot of an XDP send buffer. A claim at or past limit
// (or from a counter that wrapped negative) is refused and counted in refused.
// Returns the slot index or -1.
inline int rocketnet_xdp_send_claim( std::atomic<int> & num_packets, int limit, std::atomic<uint64_t> & refused )
{
    const int index = num_packets.fetch_add( 1 );
    if ( index < 0 || index >= limit )
    {
        refused.fetch_add( 1, std::memory_order_relaxed );
        return -1;
    }

    return index;
}
#endif // #if defined( __cplusplus )

#define ROCKETNET_SERVER_SOCKET_STOPPED    0
#define ROCKETNET_SERVER_SOCKET_RUNNING    1
#define ROCKETNET_SERVER_SOCKET_STOPPING   2

// The non-mutating pre-flight (ruled on review): checks the
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

#if ROCKETNET_DEV
void rocketnet_server_socket_allow_commit();
#endif // #if ROCKETNET_DEV

// Development-only, non-mutating pre-flight result control. The functional
// suite uses it to drive the production pin and ladder decisions through
// both pre-flight answers on a host that cannot provision either way; the
// control changes only the answer, never an attachment. Not declared in a
// production build, and absent from its archive.

#if defined( ROCKETNET_DEV ) && ROCKETNET_DEV && defined( ROCKETNET_XDP ) && ROCKETNET_XDP

void rocketnet_server_socket_set_preflight_test_result( int result );   // 0 unavailable, 1 available

void rocketnet_server_socket_clear_preflight_test_result();             // natural

#endif // #if defined( ROCKETNET_DEV ) && ROCKETNET_DEV && defined( ROCKETNET_XDP ) && ROCKETNET_XDP

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
uint64_t rocketnet_server_socket_memory_required( int num_queues );

struct rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * server_address, const uint8_t * packet_filter, int num_xdp_queues = 7, int num_os_cpus = 1, const char * second_server_address = NULL, int * create_status = NULL );

void rocketnet_server_socket_destroy( struct rocketnet_server_socket_t * server_socket );

// The replay high-water writer: the server socket is the only writer of the
// pinned server_xdp_replay_map. These calls are main thread only. Forget and
// mark append to the socket's pending buffer; a full buffer refuses the entry
// and the next commit adds the refusal to the commit failure count. Commit
// deletes every forget, then updates every mark, then resets the buffer,
// keeping every forget the kernel refused (anything but a missing key) to be
// tried first in the next commit, unless a mark of the same key applied in
// that commit superseded it: the entry is then the newer session's, and the
// stale forget is dropped (proofs/tla/RNXdpReplay.tla, RetryNeverDeletesNewerMark).
// A failed syscall adds to the commit failure
// count and logs one debug line naming the call, never a key. A mark the
// kernel refuses is kept too and applied again in the next commit, unless a
// newer mark of its key replaces it or a queued forget of its key drops it
// (MarkOwedUntilApplied). Until a new session's first mark (a zero) is
// applied, send_packets holds that session's keep-alives, so it is not
// greeted over an older session's entry (HoldGreetingUntilMarked). Forget and
// mark return false when the pending buffer is full, so the walk keeps its
// slot record and asks again.

bool rocketnet_server_socket_replay_forget( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address );

// The transport accepted receive-view packet view_index for a connection:
// its source MAC becomes the reply MAC for its address (rocketnet_xdp_frame.h,
// the reply-MAC rule). Main thread only, while that view is current.

void rocketnet_server_socket_client_accepted( struct rocketnet_server_socket_t * server_socket, int view_index );

bool rocketnet_server_socket_replay_mark( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address, uint32_t high_water );

void rocketnet_server_socket_replay_commit( struct rocketnet_server_socket_t * server_socket );

uint64_t rocketnet_server_socket_replay_commit_failures( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_stop( struct rocketnet_server_socket_t * server_socket );
bool rocketnet_server_socket_stopped( struct rocketnet_server_socket_t * server_socket );

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

#if ROCKETNET_DEV
// test_xdp's cases: they run the staging entry points without kernel resources
bool rocketnet_server_socket_test_buffer_exchange();
bool rocketnet_server_socket_test_inactive_flow();
bool rocketnet_server_socket_test_replay_marks();
bool rocketnet_server_socket_test_send_failures_without_mutex();
bool rocketnet_server_socket_test_reply_mac_refreshed_at_finish();
#endif // #if ROCKETNET_DEV

struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket );

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_SOCKET_H
