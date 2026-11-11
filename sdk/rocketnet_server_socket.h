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
#include <errno.h>

// Claims the next staging slot of an XDP send buffer. A claim at or past limit
// is refused and counted in refused. The count is 64 bits: a buffer never takes
// 2^64 claims without a flush, so it never wraps back onto a slot it handed
// out. Returns the slot index or -1.
inline int rocketnet_xdp_send_claim( std::atomic<uint64_t> & num_packets, int limit, std::atomic<uint64_t> & refused )
{
    const uint64_t index = num_packets.fetch_add( 1 );
    if ( index >= (uint64_t) limit )
    {
        refused.fetch_add( 1, std::memory_order_relaxed );
        return -1;
    }

    return (int) index;
}

// An AF_XDP TX wakeup (sendto on the socket fd) fails hard when no retry can
// clear it: the interface is down (ENETDOWN), the device is gone (ENXIO), or
// the descriptor is no longer valid (EBADF). A queue whose last wakeup failed
// so cannot drain its TX ring, so the server stop must not wait for it.
inline bool rocketnet_xdp_send_wakeup_hard_error( int error )
{
    return error == ENETDOWN || error == ENXIO || error == EBADF;
}

// The stop drain's answer for one XDP queue: released once its producer
// buffer is empty and its handoff and worker hold no work (handoff_idle: no
// buffer waits, none is claimed, and the TX ring's consumer has taken every
// descriptor the worker put on it), or at once when the worker reported a
// hard wakeup error or a stalled TX ring (rocketnet_xdp_send_stall_pass).
// Waiting on such a queue would keep rocketnet_server_stopped false forever
// (proofs/tla/RNXdpSendDrain.tla SendIdle).
inline bool rocketnet_xdp_send_queue_released( int hard_send_error, bool stalled, bool buffer_empty, bool handoff_idle )
{
    if ( rocketnet_xdp_send_wakeup_hard_error( hard_send_error ) || stalled )
        return true;
    return buffer_empty && handoff_idle;
}

// The worker's watch for a TX ring that has stopped draining with no hard
// errno (a zero-copy queue whose link lost carrier while the interface stays
// up, a copy-mode queue whose device queue stays stopped), run once per pass.
// pending: the worker holds frames the ring's consumer has not taken (its
// batch, a claimed buffer, or descriptors on the ring). passes counts the
// passes since the consumer index last moved while frames were pending. Once
// it reaches bound during a stop (draining), stalled is set and the call
// returns true, once per stall, for the worker to count and log what it
// abandons; the consumer moving again, or nothing pending, clears it
// (proofs/tla/RNXdpSendDrain.tla Pass).
struct rocketnet_xdp_send_stall_t
{
    uint32_t consumer;
    uint32_t passes;
};

inline bool rocketnet_xdp_send_stall_pass( rocketnet_xdp_send_stall_t & stall, uint32_t consumer, bool pending, bool draining, uint32_t bound, std::atomic<bool> & stalled )
{
    if ( !pending || consumer != stall.consumer )
    {
        stall.consumer = consumer;
        stall.passes = 0;
        if ( stalled.load( std::memory_order_relaxed ) )
            stalled.store( false, std::memory_order_relaxed );
        return false;
    }
    if ( stall.passes < bound )
        stall.passes++;
    if ( stall.passes < bound || !draining || stalled.load( std::memory_order_relaxed ) )
        return false;
    stalled.store( true, std::memory_order_relaxed );
    return true;
}

// The bound of that watch, in worker passes. A healthy NIC takes descriptors
// from its 4096-entry TX ring at every NAPI poll; the longest healthy pause is
// an 802.3x PAUSE of 65535 quanta, 33.6 ms at 1 Gb/s, or a few interrupt
// moderation intervals. A pass is at least one poll call and the ring reads:
// at a floor of 100 ns a pass, 2^24 passes are at least 1.68 s, fifty times
// that pause; at a more usual 1 us a pass the stop gives up about 17 s after
// the ring stopped. Chosen, not measured. Passes, not wakeup attempts: a
// zero-copy driver that clears the ring's need-wakeup flag is never woken.
#define ROCKETNET_XDP_SEND_STALL_PASSES ( 1u << 24 )

// The worker's accounting of one TX wakeup that failed with error, into
// failures, which the public SEND_FAILED counter sums (unit: datagrams).
// EBUSY is one datagram lost: in copy mode the kernel answers it when the
// device dropped the frame it had just taken off the TX ring ("SKB completed
// but not sent", xsk_generic_xmit in net/xdp/xsk.c, Linux 5.15 and 6.8), so
// each one counts. A hard error loses nothing by itself, the descriptors stay
// on the ring, and it repeats on every wakeup while it lasts: it counts once,
// when it differs from the one kept in hard_error for the stop drain (a
// wakeup that succeeds clears that). EAGAIN and ENOBUFS are back-pressure:
// the descriptors stay queued for the next wakeup, and they count nothing.
// Any other errno is counted by the worker once per run, when it reports it
// (rocketnet_xdp_send_wakeup_unexpected).
inline void rocketnet_xdp_send_wakeup_failed( int error, std::atomic<uint64_t> & failures, std::atomic<int> & hard_error )
{
    if ( error == EBUSY )
    {
        failures.fetch_add( 1, std::memory_order_relaxed );
        return;
    }
    if ( !rocketnet_xdp_send_wakeup_hard_error( error ) )
        return;
    if ( hard_error.load( std::memory_order_relaxed ) == error )
        return;
    failures.fetch_add( 1, std::memory_order_relaxed );
    hard_error.store( error, std::memory_order_relaxed );
}

// A TX wakeup that succeeded shows the ring can drain: a hard error kept from
// an interface that went down and came back up no longer holds, and the stop
// drain waits for this queue again. The store is skipped while there is none.
inline void rocketnet_xdp_send_wakeup_succeeded( std::atomic<int> & hard_error )
{
    if ( hard_error.load( std::memory_order_relaxed ) != 0 )
        hard_error.store( 0, std::memory_order_relaxed );
}

// A TX wakeup errno that is neither back-pressure (EAGAIN, ENOBUFS), a lost
// datagram (EBUSY) nor hard: EINVAL or EOPNOTSUPP from a driver, say. The
// worker counts it in failures and logs it once per contiguous run.
inline bool rocketnet_xdp_send_wakeup_unexpected( int error )
{
    return error != 0 && error != EAGAIN && error != ENOBUFS && error != EBUSY && !rocketnet_xdp_send_wakeup_hard_error( error );
}

// The worker logs a hard or unexpected wakeup failure once per contiguous run
// of one errno: report when it is either and differs from last_error, and
// remember every attempt's errno (0 for success) so the next run reports
// again.
inline bool rocketnet_xdp_send_wakeup_report( int error, int & last_error )
{
    const bool report = ( rocketnet_xdp_send_wakeup_hard_error( error ) || rocketnet_xdp_send_wakeup_unexpected( error ) ) && error != last_error;
    last_error = error;
    return report;
}

// The frames the kernel dropped on one AF_XDP socket's receive path after the
// XDP program redirected them, from its XDP_STATISTICS answer of bytes bytes
// (struct xdp_statistics in linux/if_xdp.h). rx_dropped counts a frame with no
// fill ring frame to take it, or larger than a umem frame; rx_ring_full, the
// fourth field (bytes 24 to 31), a frame with no room on the RX ring. A kernel
// that answers with only the first three fields (24 bytes, before Linux 5.9)
// counts both in rx_dropped. rx_fill_ring_empty_descs is not a frame count:
// it counts failed fill ring reads, and in copy mode the frame such a read
// failed for counts in rx_dropped too. The answer is the socket's total
// since bind, so each read replaces the last.
inline uint64_t rocketnet_xdp_socket_dropped( uint64_t rx_dropped, uint64_t rx_ring_full, size_t bytes )
{
    return rx_dropped + ( bytes >= 32 ? rx_ring_full : 0 );
}

// The worker reads its socket's XDP_STATISTICS once per this many passes of
// its loop, and on its final pass: a count of passes, so the read adds no
// clock to the loop. A pass is at least one poll call, so this is about a
// read every few milliseconds at most; chosen, not measured.
#define ROCKETNET_XDP_STATISTICS_PASSES 4096
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

bool rocketnet_server_socket_preflight( const char * public_address_string, int num_queues, int max_clients );

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
// it, AF_XDP binds, and raising the process RLIMIT_MEMLOCK to RLIM_INFINITY
// (which neither destroy nor a failed create restores). Machine provisioning
// (xdp-loader clearing, SMT, napi tuning, ethtool -L queue counts, isolcpus
// guidance) lives in scripts/rocketnet-provision, run deliberately by the
// operator; create never mutates host-wide machine configuration.

// Requested XDP component bytes (userspace queues, the parts sized from the
// server's configured max_clients, and UMEM), excluding kernel bookkeeping,
// thread stacks and allocator overhead; no privileged operations.
uint64_t rocketnet_server_socket_memory_required( int num_queues, int max_clients );

// max_clients is the server's configured client count: it sizes the receive
// view, the reply MAC table, the replay marks and the kernel replay map.
struct rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * server_address, const uint8_t * packet_filter, int max_clients, int num_xdp_queues = 7, int num_os_cpus = 1, const char * second_server_address = NULL, int * create_status = NULL );

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

// the frames start_packet takes for any destination before the next flush:
// the fewest free slots of any queue's producer buffer, since a destination
// hashes to one queue. Main thread.
int rocketnet_server_socket_send_room( struct rocketnet_server_socket_t * server_socket );

// true once every queue's producer buffer is empty, its worker has copied and
// recycled every buffer it was handed, and the TX ring's consumer has taken
// every descriptor the worker put on it; a queue whose worker reported a hard
// wakeup error, or a TX ring stalled during the stop, is given up on
// (rocketnet_xdp_send_queue_released). Main thread; a send mutex held by a
// worker reads as not idle.
bool rocketnet_server_socket_send_idle( struct rocketnet_server_socket_t * server_socket );

// the stop has begun: each queue's worker watches its TX ring for a stall
// from now on (rocketnet_xdp_send_stall_pass). Main thread.
void rocketnet_server_socket_begin_shutdown( struct rocketnet_server_socket_t * server_socket );

// receive packets (staged, one copy each way)

void rocketnet_server_socket_receive_packets( struct rocketnet_server_socket_t * server_socket );

#if ROCKETNET_DEV
// test_xdp's cases: they run the staging entry points without kernel resources
bool rocketnet_server_socket_test_buffer_exchange();
bool rocketnet_server_socket_test_inactive_flow();
bool rocketnet_server_socket_test_replay_marks();
bool rocketnet_server_socket_test_send_failures_without_mutex();
bool rocketnet_server_socket_test_send_idle_hard_error();
// the unit suite's case: dispatch is rocketnet_server_transport_io_receive_frames_dropped
// and xdp the xdp operations table, as the transport's counter read uses them
typedef bool rocketnet_server_socket_test_frames_dropped_t( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                            uint64_t * dropped );
bool rocketnet_server_socket_test_receive_dropped( rocketnet_server_socket_test_frames_dropped_t * dispatch,
                                                   const struct rocketnet_server_transport_io_operations_t * xdp );
bool rocketnet_server_socket_test_reply_mac_refreshed_at_finish();
bool rocketnet_server_socket_test_gateway_refresh();
bool rocketnet_server_socket_test_worker_stack();
#endif // #if ROCKETNET_DEV

struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket );

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_SOCKET_H
