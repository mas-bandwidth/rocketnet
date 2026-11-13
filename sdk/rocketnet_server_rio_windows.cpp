/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_internal.h"
#include "rocketnet_driver.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// RIOSendEx, RIOReceiveEx and the RIO function table are declared for
// Windows 8 and later.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif // #ifndef _WIN32_WINNT

#include "rocketnet_server_rio.h"
#include "rocketnet_rio_layout.h"
#include "rocketnet_platform.h"
#include "rocketnet_receive_prefilter.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <atomic>
#include <new>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// The model: proofs/tla/RNRioWorker.tla (the I/O worker, its bank and phase
// handoffs and stop); proofs/tla/RNRioSendSlotLayout.tla for the slot rules.
// Each function that implements an action names it.

// Three send phases, as every other transport's send handoff has three
// banks: one filling, and up to two with the worker. A flush hands the fill
// phase only when the next phase is idle, so a busy worker delays sends by a
// flush instead of refusing them.
#define ROCKETNET_RIO_SEND_PHASES 3
#define ROCKETNET_RIO_DEQUEUE_BATCH 256

// A create that fails before its worker exists drains inline, and the drain
// is bounded by counting passes, never by seconds. closesocket cancels every
// posted receive; a pass that finds some of those completions still on their
// way waits for the receive queue's notification, at most
// ROCKETNET_RIO_DRAIN_NOTIFY_MILLISECONDS so a lost notification cannot hold
// create. A pass that cannot close the socket has nothing on its way and is
// run again at once. A pass that dequeued a cancelled receive made progress
// and is not counted. After ROCKETNET_RIO_DRAIN_WAITS + 1 passes that made
// none the transport leaks rather than freeing memory the kernel may still
// write, as the iocp client's destroy does.
#define ROCKETNET_RIO_DRAIN_WAITS 10
#define ROCKETNET_RIO_DRAIN_NOTIFY_MILLISECONDS 50
// A multi-lane create that fails at a later lane stops the lanes it made and
// polls each for stopped at most this many times, a millisecond apart (about
// the second server destroy gives its workers); a lane still running then is
// leaked whole, named and counted (rocketnet_server_rio_create_leaks).
#define ROCKETNET_RIO_ROLLBACK_POLLS 1000

static_assert( ROCKETNET_RIO_RECEIVE_SLICE_BYTES >= ROCKETNET_RECEIVE_CEILING + 1, "receive detects oversize datagrams" );

// A whole bank fits the process view: a bank holds twice the staging (at most
// four packets per sized client) and the view sixteen per sized client, so
// it holds at every count from the smallest.
static_assert( 2 * ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_SERVER_MIN_SIZED_CLIENTS ) <= ROCKETNET_SERVER_MIN_RECEIVE_PACKETS_PER_UPDATE,
               "a whole bank fits the process view" );

// The request context packs a slot or slice index in its low 32 bits, so the
// largest build's send queue and receive slices fit it.
static_assert( 2 * ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_MAX_CLIENTS ) <= INT32_MAX, "an index fits the request context" );

// a receive bank: its arrays hold capacity packets (the transport's slices,
// rocketnet_server_rio_receives for the configured max_clients), carved from
// the transport's sized allocation; data is 16-aligned
struct rocketnet_rio_bank_t
{
    uint8_t ( *data )[ROCKETNET_FRAME_STRIDE];
    rocketnet_address_t * from;
    int * packet_bytes;
    int num_packets;
    int capacity;
};

// Receive banks: the worker's fill bank, the ready FIFO, the banks main
// exposed last call and the free banks. Four, as the IOCP workers use.
#define ROCKETNET_RIO_RECEIVE_BANKS 4

// The send phase states, written only under the mutex (proofs/tla/RNRioWorker.tla):
// FILL is open to fill threads, READY is handed to the worker, POSTED is the
// worker's until every send of it completes, IDLE may be opened by main.
#define ROCKETNET_RIO_PHASE_FILL    0
#define ROCKETNET_RIO_PHASE_READY   1
#define ROCKETNET_RIO_PHASE_POSTED  2
#define ROCKETNET_RIO_PHASE_IDLE    3

// Its cache-line alignment can require implicit tail padding on MSVC ARM64.
// Keep C4324 disabled only for this private structure; its layout is unchanged.
#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4324 )
#endif // #if defined( _MSC_VER )
// where a lane's receive path waits, logged per lane at destroy: a worker pass
// that found the fill bank full (no free bank), a pass that took a whole lane of
// receive completions (the kernel had none posted), and a main receive call that
// left a ready bank for the next update (the process view was full)
enum
{
    ROCKETNET_RIO_WAIT_RECEIVE_BANK,
    ROCKETNET_RIO_WAIT_RECEIVE_RAN_DRY,
    ROCKETNET_RIO_WAIT_RECEIVE_VIEW,
    ROCKETNET_RIO_NUM_WAITS
};

struct rocketnet_rio_lane_t
{
    // next and outstanding lead, each starting a cache line (next's line
    // padded out explicitly), so the compiler inserts no alignment padding.
    // next is read by fill threads, which run only while the main thread is
    // outside send_packets. It is 64 bits, as IOCP's num_staged: a fill phase
    // never reserves 2^64 slots, so the count never wraps back onto a slot
    // it handed out.
    alignas( 64 ) std::atomic<uint64_t> next[ROCKETNET_RIO_SEND_PHASES]{};
    uint8_t next_padding[64 - ROCKETNET_RIO_SEND_PHASES * sizeof( std::atomic<uint64_t> )];
    // posted sends of each phase not yet retired: the worker writes, pending reads
    alignas( 64 ) std::atomic<int> outstanding[ROCKETNET_RIO_SEND_PHASES]{};

    void * context;
    SOCKET socket = INVALID_SOCKET;
    bool ipv6 = false;
    // the lane's processor (rocketnet_rio_lane_processors): its socket's
    // SIO_CPU_AFFINITY and its worker's affinity. -1 for a single lane, which
    // binds exclusively and runs where Windows schedules it.
    int processor = -1;
    GROUP_AFFINITY pin = {};
    // what the worker found when it pinned itself
    // (rocketnet_server_rio_lane_placement), and the Win32 error of a refusal
    std::atomic<int> placement{ ROCKETNET_RIO_PLACEMENT_PENDING };
    std::atomic<unsigned long> placement_error{ 0 };
    // the pin refusals this lane's worker met (ROCKETNET_COUNTER_RIO_PIN_REFUSALS): 0 or 1 a lane
    std::atomic<uint64_t> pin_refusals{ 0 };
    // a completion queue of this lane was reported corrupt (named once)
    bool corrupt_reported = false;
    // each send phase's slots (rocketnet_server_rio_send_queue_size), every
    // phase's frames, and the receive slices (rocketnet_server_rio_receives),
    // for the configured max_clients; the one allocation the slot bytes, the
    // banks, the retry list and the process view are carved from
    // (rocketnet_rio_sized_layout)
    int send_queue_size = 0;
    int send_frames = 0;
    int receives = 0;
    void * sized_storage = NULL;
    RIO_EXTENSION_FUNCTION_TABLE rio;
    bool have_table = false;

    uint8_t * region = NULL;
    rocketnet_rio_layout_t layout;
    RIO_BUFFERID buffer_id = RIO_INVALID_BUFFERID;
    RIO_CQ send_cq = RIO_INVALID_CQ;
    RIO_CQ receive_cq = RIO_INVALID_CQ;
    RIO_RQ request_queue = RIO_INVALID_RQ;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    uint64_t socket_generation = 0;
    uint64_t wake_generation = 0;
    uint64_t notify_generation = 0;
    uint64_t buffer_generation = 0;
    uint64_t send_cq_generation = 0;
    uint64_t receive_cq_generation = 0;
    uint64_t request_queue_generation = 0;
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    std::atomic<bool> stopping{ false };

    // The I/O worker owns the request queue and both completion queues: RIO
    // queues are unsynchronized, and sends and receives share the request
    // queue, so one thread issues every RIO call after create. Only bank
    // indices and phase states cross the mutex; no RIO call, copy or wait
    // runs under it, and main only ever tries it.
    rocketnet_platform_thread_t * thread = NULL;
    rocketnet_platform_mutex_t mutex = {};
    HANDLE wake = NULL;                         // auto-reset: main signals the worker
    HANDLE notify = NULL;                       // auto-reset: RIONotify on either completion queue
    std::atomic<bool> exited{ false };

    // send, main side: the model's phase. fill is read by fill threads, which
    // run only while main is outside send_packets.
    std::atomic<int> fill{ 0 };
    int * slot_bytes = NULL;                    // [send_frames], zero for a free, reserved or aborted slot

    // send, under the mutex: the model's state and sready
    int phase_state[ROCKETNET_RIO_SEND_PHASES] = { ROCKETNET_RIO_PHASE_FILL, ROCKETNET_RIO_PHASE_IDLE, ROCKETNET_RIO_PHASE_IDLE };
    int phase_count[ROCKETNET_RIO_SEND_PHASES] = {};      // slots handed with the phase
    int send_ready[ROCKETNET_RIO_SEND_PHASES] = {};
    int send_ready_head = 0;
    int send_ready_count = 0;

    // send, worker only: deferred sends posted and not yet committed
    bool uncommitted = false;

    // receive, under the mutex: the model's free and ready
    int free_banks[ROCKETNET_RIO_RECEIVE_BANKS];
    int free_count = 0;
    int ready_banks[ROCKETNET_RIO_RECEIVE_BANKS];
    int ready_head = 0;
    int ready_count = 0;
    // receive, main only: the banks process_packets exposes until the next call
    int exposed_banks[ROCKETNET_RIO_RECEIVE_BANKS];
    int exposed_count = 0;

    // receive, worker only: the model's fill bank and posted slices (atomic
    // only so pending can read the count). A slice is posted, completed in
    // the queue, or in the retry list after a refused re-post.
    std::atomic<int> posted_receives{ 0 };
    rocketnet_rio_bank_t banks[ROCKETNET_RIO_RECEIVE_BANKS] = {};
    int fill_bank = 0;
    int * retry_slices = NULL;                  // [receives]
    std::atomic<int> num_retry{ 0 };

    std::atomic<uint64_t> counters[ROCKETNET_SERVER_RIO_COUNTER_ROWS]{};
    // where this lane's receive path waited (rocketnet_rio_wait): lane-own
    // diagnostics, logged at destroy, not rows of the public counter table
    std::atomic<uint64_t> waits[ROCKETNET_RIO_NUM_WAITS]{};
    // the datagrams this lane's worker refused by the stateless prefilter
    // before banking, and those it had no room for (BANK_FULL): the worker is
    // the one writer (sdk/rocketnet_receive_prefilter.h)
    // the worker's drop tally, carved from the sized block so this struct
    // holds no over-aligned member (MSVC C4324)
    rocketnet_receive_drop_tally_t * drops = NULL;
    // the server's filter value, copied at create; prefilter is false only in
    // a unit fixture or a transport made without one, which banks every
    // datagram the size bounds pass, as before
    bool prefilter = false;
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = {};
    // this lane's own process view, carved with its banks; view is where
    // receive_packets appends: lane 0's view, shared by every lane of a
    // transport (rocketnet_server_rio_receive_packets), or its own in a
    // fixture
    rocketnet_server_socket_process_packets_t process;
    rocketnet_server_socket_process_packets_t * view = NULL;
#if ROCKETNET_DEV
    bool test_hold_send = false;
    std::atomic<int> test_faults[ROCKETNET_RIO_TEST_NUM_FAULTS]{};
    std::atomic<int> test_fault_status[ROCKETNET_RIO_TEST_NUM_FAULTS]{};
    bool test_try_fails = false;                // main's next tries fail, as under contention
#endif // #if ROCKETNET_DEV
};
#if defined( _MSC_VER )
#pragma warning( pop )
#endif // #if defined( _MSC_VER )

static_assert( offsetof( rocketnet_rio_lane_t, outstanding ) == 64, "the rio's aligned members have no implicit padding" );

// The fault seam (rocketnet_rio_lane_test_fail_next) exists only in the
// test executable. Elsewhere the check is the constant false, so the
// production paths carry no test branch.
#if ROCKETNET_DEV
static bool rocketnet_rio_test_fault( rocketnet_rio_lane_t * rio, int fault, int * status )
{
    int left = rio->test_faults[fault].load( std::memory_order_acquire );
    while ( left > 0 && !rio->test_faults[fault].compare_exchange_weak( left, left - 1, std::memory_order_acq_rel ) )
    {
    }

    if ( left <= 0 )
    {
        return false;
    }
    *status = rio->test_fault_status[fault].load( std::memory_order_relaxed );
    WSASetLastError( *status );

    return true;
}
#define ROCKETNET_RIO_TEST_FAULT( rio, fault, status ) rocketnet_rio_test_fault( rio, fault, status )
#else
#define ROCKETNET_RIO_TEST_FAULT( rio, fault, status ) ( (void) (status), false )
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV

// This table exists only in test builds, compiled into the library like the
// filter-epoch seam. The worker pass substitutes dequeue, receive repost and
// (when set) send post and commit, so the unit seams drive the real worker
// pass and main calls with no socket; production builds call the RIO
// function table directly with no test branch.
typedef ULONG( WINAPI * rocketnet_rio_test_dequeue_fn )( RIO_CQ, PRIORESULT, ULONG );
typedef BOOL( WINAPI * rocketnet_rio_test_receive_fn )( RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF,
                                                        PRIO_BUF, PRIO_BUF, DWORD, PVOID );

typedef int( WINAPI * rocketnet_rio_test_close_fn )( SOCKET );

struct rocketnet_rio_test_function_table_t
{
    rocketnet_rio_test_dequeue_fn dequeue;
    rocketnet_rio_test_receive_fn receive;
    rocketnet_rio_test_receive_fn send;         // NULL: sends use the real table
    rocketnet_rio_test_close_fn close;          // NULL: closesocket
};

static const rocketnet_rio_test_function_table_t * rocketnet_rio_test_functions = NULL;

// The unit seam's stand-in for the wait between two passes of create's
// failure drain: it is told the wait's bound and no time passes.
static void ( *rocketnet_rio_test_drain_wait )( DWORD milliseconds ) = NULL;
static std::atomic<int> rocketnet_rio_test_create_drain_waits{ 0 };

int rocketnet_server_rio_test_create_drain_waits()
{
    return rocketnet_rio_test_create_drain_waits.load( std::memory_order_relaxed );
}

#endif // #if ROCKETNET_DEV

// The worker's RIO calls on its queues, through the test table when a unit
// seam installed one.
static ULONG rocketnet_rio_dequeue( rocketnet_rio_lane_t * rio, RIO_CQ cq, RIORESULT * results, ULONG max_results )
{
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions )
    {
        return rocketnet_rio_test_functions->dequeue( cq, results, max_results );
    }
#endif // #if ROCKETNET_DEV
    return rio->rio.RIODequeueCompletion( cq, results, max_results );
}

// the completion queues RIODequeueCompletion reported corrupt
// (rocketnet_server_rio_corrupt_queues)
static std::atomic<uint64_t> rocketnet_rio_corrupt_queues{ 0 };

// RIO_CORRUPT_CQ: the queue's requests can never be accounted for, so the
// lane never reports stopped and its registered region is never freed (the
// kernel may still write it). Not silent: named once per lane at error and
// counted for the process.
static void rocketnet_rio_corrupt_queue( rocketnet_rio_lane_t * rio, const char * which )
{
    if ( rio->corrupt_reported )
    {
        return;
    }
    rio->corrupt_reported = true;
    const uint64_t corrupt = rocketnet_rio_corrupt_queues.fetch_add( 1, std::memory_order_relaxed ) + 1;
    rocketnet_error( "rio transport: the %s completion queue is corrupt (RIO_CORRUPT_CQ); the lane cannot account for its requests, never reports "
                     "stopped, and its registered region is never freed (corrupt queue %llu in this process)",
                     which, (unsigned long long) corrupt );
}

static BOOL rocketnet_rio_send_call( rocketnet_rio_lane_t * rio, RIO_BUF * data, RIO_BUF * address, DWORD flags, void * request_context )
{
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions && rocketnet_rio_test_functions->send )
    {
        return rocketnet_rio_test_functions->send( rio->request_queue, data, data ? 1 : 0, NULL, address, NULL, NULL, flags, request_context );
    }
#endif // #if ROCKETNET_DEV
    return rio->rio.RIOSendEx( rio->request_queue, data, data ? 1 : 0, NULL, address, NULL, NULL, flags, request_context );
}

static int rocketnet_rio_close_call( rocketnet_rio_lane_t * rio )
{
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions && rocketnet_rio_test_functions->close )
    {
        return rocketnet_rio_test_functions->close( rio->socket );
    }
#endif // #if ROCKETNET_DEV
    return closesocket( rio->socket );
}

#if ROCKETNET_DEV
// the lane seams, defined with the lane operations below
static void rocketnet_rio_lane_test_fail_next( rocketnet_rio_lane_t * rio, int fault, int count, int status );
static int rocketnet_rio_lane_test_faults_left( rocketnet_rio_lane_t * rio, int fault );
static void rocketnet_rio_lane_test_hold_send( rocketnet_rio_lane_t * rio, bool hold );
static int rocketnet_rio_lane_test_handed_phases( rocketnet_rio_lane_t * rio );
static bool rocketnet_rio_lane_test_send_idle( rocketnet_rio_lane_t * rio );
static int rocketnet_rio_lane_test_retry_receives( rocketnet_rio_lane_t * rio );
#endif // #if ROCKETNET_DEV

static void rocketnet_rio_count( rocketnet_rio_lane_t * rio, int counter, uint64_t amount = 1 )
{
    rio->counters[counter - ROCKETNET_SERVER_RIO_FIRST_COUNTER].fetch_add( amount, std::memory_order_relaxed );
}

static void rocketnet_rio_wait( rocketnet_rio_lane_t * rio, int which )
{
    rio->waits[which].fetch_add( 1, std::memory_order_relaxed );
}

// posted sends not yet retired, every phase
static int rocketnet_rio_outstanding_sends( rocketnet_rio_lane_t * rio )
{
    int total = 0;
    for ( int p = 0; p < ROCKETNET_RIO_SEND_PHASES; ++p )
    {
        total += rio->outstanding[p].load( std::memory_order_relaxed );
    }
    return total;
}

// a destination as the SOCKADDR_INET the socket's family needs: a dual
// stack socket sends IPv4 as v4-mapped IPv6
static bool rocketnet_rio_address_write( const rocketnet_address_t * input, bool ipv6, SOCKADDR_INET * out )
{
    rocketnet_address_t address = *input;
    memset( out, 0, sizeof( *out ) );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_address_convert_ipv4_to_ipv6( &address );
    }
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV6 )
    {
        rocketnet_platform_address_to_sockaddr_in6( &address, &out->Ipv6 );
        return true;
    }

    if ( !ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_platform_address_to_sockaddr_in( &address, &out->Ipv4 );
        return true;
    }

    return false;
}

// a source from its SOCKADDR_INET; an IPv4-mapped IPv6 source passes the one
// receive rule, rocketnet_address_accept_received_source: canonicalised to
// IPv4 on the dual-stack socket, refused on any other
static bool rocketnet_rio_address_read( const SOCKADDR_INET * in, bool dual_stack, rocketnet_address_t * address )
{
    if ( in->si_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( &in->Ipv4, address );
        return true;
    }

    if ( in->si_family == AF_INET6 )
    {
        rocketnet_platform_sockaddr_in6_to_address( &in->Ipv6, address );
        return rocketnet_address_accept_received_source( address, dual_stack );
    }

    memset( address, 0, sizeof( *address ) );
    return false;
}

// a send frame index is phase * send_queue_size + slot
static uint8_t * rocketnet_rio_frame( rocketnet_rio_lane_t * rio, int index )
{
    return rio->region + rocketnet_rio_send_frame_offset( &rio->layout, index );
}

static SOCKADDR_INET * rocketnet_rio_send_address( rocketnet_rio_lane_t * rio, int index )
{
    return (SOCKADDR_INET *) ( rio->region + rocketnet_rio_send_address_offset( &rio->layout, index ) );
}

static const SOCKADDR_INET * rocketnet_rio_receive_address( rocketnet_rio_lane_t * rio, int slice )
{
    return (const SOCKADDR_INET *) ( rio->region + rocketnet_rio_receive_address_offset( &rio->layout, slice ) );
}

// one RIO_MSG_COMMIT_ONLY when deferred sends are uncommitted; a refused
// commit leaves them uncommitted for the next try and counts nothing: each
// deferred send still completes once, as PACKETS_SENT or SEND_FAILED
static void rocketnet_rio_commit( rocketnet_rio_lane_t * rio )
{
    if ( !rio->uncommitted )
    {
        return;
    }
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, &injected ) ||
         !rocketnet_rio_send_call( rio, NULL, NULL, RIO_MSG_COMMIT_ONLY, NULL ) )
    {
        rocketnet_warn( "rio transport: send commit refused (Winsock %d); retried", WSAGetLastError() );
        return;
    }

    rio->uncommitted = false;
    rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_COMMITS );
}

// the send frame index of a frame pointer: a pointer that is not the root of
// a send frame lands out of range or off a root and returns -1, a lost
// packet, never an out-of-bounds write
static int rocketnet_rio_frame_index( rocketnet_rio_lane_t * rio, const uint8_t * frame )
{
    if ( !frame || !rio->region )
    {
        return -1;
    }
    const uint8_t * first = rio->region + rio->layout.send_frames_offset;
    if ( frame < first )
    {
        return -1;
    }
    const uint64_t offset = (uint64_t) ( frame - first );
    const uint64_t stride = rocketnet_rio_element_stride( rio->layout.frame_bytes );
    if ( offset % stride != 0 || offset / stride >= (uint64_t) rio->send_frames )
    {
        return -1;
    }
    // only a reserved slot of the open fill phase: a stale frame of a phase
    // the kernel owns is refused, never rewritten under a posted send

    const int index = (int) ( offset / stride );
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const int slot = index - phase * rio->send_queue_size;
    if ( slot < 0 || slot >= rio->send_queue_size ||
         (uint64_t) slot >= rio->next[phase].load( std::memory_order_relaxed ) )
    {
        return -1;
    }
    return index;
}

// posts one receive slice with its address slot (create, and the re-post
// in DequeueReceive)
static bool rocketnet_rio_post_receive( rocketnet_rio_lane_t * rio, int slice )
{
    RIO_BUF data;
    data.BufferId = rio->buffer_id;
    data.Offset = (ULONG) rocketnet_rio_receive_slice_offset( &rio->layout, slice );
    data.Length = ROCKETNET_RIO_RECEIVE_SLICE_BYTES;
    RIO_BUF address;
    address.BufferId = rio->buffer_id;
    address.Offset = (ULONG) rocketnet_rio_receive_address_offset( &rio->layout, slice );
    address.Length = sizeof( SOCKADDR_INET );

    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST, &injected ) )
    {
        return false;
    }
    int use_real_receive = 1;
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions )
    {
        use_real_receive = 0;
        if ( !rocketnet_rio_test_functions->receive( rio->request_queue, &data, 1, NULL, &address, NULL, NULL, 0,
                                                     (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) slice ) ) )
        {
            return false;
        }
    }
#endif
    if ( use_real_receive && !rio->rio.RIOReceiveEx( rio->request_queue, &data, 1, NULL, &address, NULL, NULL, 0,
                                                     (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) slice ) ) )
    {
        // a post refused with an ICMP report of an earlier send is counted
        // with the reset statuses; a refused re-post waits on the retry list in
        // rocketnet_rio_complete_receive
        if ( rocketnet_platform_receive_status_is_reset( WSAGetLastError() ) )
        {
            rocketnet_platform_count_receive_reset();
        }
        return false;
    }

    rio->posted_receives.fetch_add( 1, std::memory_order_relaxed );

    return true;
}

// proofs/tla/RNRioWorker.tla WRetire: one send completion; the slot is the
// transport's again. A failed status counts SEND_FAILED.
static void rocketnet_rio_retire_send( rocketnet_rio_lane_t * rio, const RIORESULT * result )
{
    const uint64_t context = (uint64_t) result->RequestContext;
    const int phase = rocketnet_rio_context_phase( context );
    const int slot = rocketnet_rio_context_index( context );
    if ( rocketnet_rio_context_kind( context ) != ROCKETNET_RIO_CONTEXT_SEND || phase < 0 || phase >= ROCKETNET_RIO_SEND_PHASES ||
         slot < 0 || slot >= rio->send_queue_size )
    {
        rocketnet_error( "rio transport: send completion with a foreign request context %llx", (unsigned long long) context );
        return;
    }

    const int index = phase * rio->send_queue_size + slot;
    LONG status = result->Status;
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_SEND_COMPLETION, &injected ) )
    {
        status = (LONG) injected;
    }
    const bool sent = status == 0 && result->BytesTransferred == (ULONG) rio->slot_bytes[index];
    rocketnet_rio_count( rio, sent ? ROCKETNET_COUNTER_RIO_PACKETS_SENT : ROCKETNET_COUNTER_RIO_SEND_FAILED );
    rio->slot_bytes[index] = 0;
    rio->outstanding[phase].fetch_sub( 1, std::memory_order_relaxed );
}

// proofs/tla/RNRioWorker.tla WRetire: dequeues the send
// completion queue without blocking. Returns the completions retired.
// proofs/tla/RNRioSendSlotLayout.tla Complete: each completion frees its slot.
static int rocketnet_rio_dequeue_send( rocketnet_rio_lane_t * rio )
{
    int total = 0;

    for ( ;; )
    {
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rocketnet_rio_dequeue( rio, rio->send_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
        if ( count == RIO_CORRUPT_CQ )
        {
            rocketnet_rio_corrupt_queue( rio, "send" );
            return total;
        }

        for ( ULONG i = 0; i < count; ++i )
        {
            rocketnet_rio_retire_send( rio, &results[i] );
        }
        total += (int) count;
        if ( count < ROCKETNET_RIO_DEQUEUE_BATCH )
        {
            return total;
        }
    }
}

// proofs/tla/RNRioWorker.tla WClose: closesocket completes every outstanding
// request with an error; the close waits for a successful
// commit of the deferred sends, then for every committed send to complete.
// A deferred send still occupies request-queue capacity after a refused
// commit. Keep the socket open so a later completion check can retry the
// commit before closing the request queue that owns those sends. A
// committed send still in flight at closesocket is cancelled and retires as
// SEND_FAILED, which would lose the shutdown disconnect burst, so each pass
// retires the send completions available and keeps the socket open while
// any send is outstanding.
static bool rocketnet_rio_close_socket( rocketnet_rio_lane_t * rio )
{
    if ( rio->request_queue != RIO_INVALID_RQ && rio->uncommitted )
    {
        rocketnet_rio_commit( rio );
        if ( rio->uncommitted )
        {
            return false;
        }
    }

    if ( rio->socket != INVALID_SOCKET && rocketnet_rio_outstanding_sends( rio ) > 0 )
    {
        if ( rio->have_table && rio->send_cq != RIO_INVALID_CQ )
        {
            rocketnet_rio_dequeue_send( rio );
        }
        if ( rocketnet_rio_outstanding_sends( rio ) > 0 )
        {
            return false;
        }
    }

    if ( rio->socket != INVALID_SOCKET )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        const bool have_request_queue = rio->request_queue != RIO_INVALID_RQ;
        if ( have_request_queue && !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_RIO_RQ,
                                                                           (uintptr_t) rio->request_queue, rio->request_queue_generation ) )
        {
            return false;
        }
        if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) rio->socket, rio->socket_generation ) )
        {
            if ( have_request_queue )
            {
                rocketnet_lifecycle_track_close_end( rio->request_queue_generation, false );
            }
            return false;
        }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( rocketnet_rio_close_call( rio ) != 0 )
        {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            rocketnet_lifecycle_track_close_end( rio->socket_generation, false );
            if ( have_request_queue )
            {
                rocketnet_lifecycle_track_close_end( rio->request_queue_generation, false );
            }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            return false;
        }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        rocketnet_lifecycle_track_close_end( rio->socket_generation, true );
        if ( have_request_queue )
        {
            rocketnet_lifecycle_track_close_end( rio->request_queue_generation, true );
        }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        rio->socket = INVALID_SOCKET;
    }

    return true;
}

static void rocketnet_rio_post_phase( rocketnet_rio_lane_t * rio, int phase, int count );

// proofs/tla/RNRioWorker.tla WStop, WClose, WDrainSend and WDrainReceive: post handed
// and finished fill phases, commit deferred sends, retire send completions
// until no committed send is in flight, close the socket (which cancels every
// posted receive), then dequeue every available completion of each queue, a
// call per full batch. True when nothing is
// kernel owned. Runs on the worker, or inline on create's failure path
// before the worker exists; never waits.
// proofs/tla/RNRioWorker.tla WDrainSend / WDrainReceive: both completion queues are dequeued and
// nothing is re-posted.
static bool rocketnet_rio_stop_pass( rocketnet_rio_lane_t * rio )
{
    int phases[ROCKETNET_RIO_SEND_PHASES];
    int counts[ROCKETNET_RIO_SEND_PHASES];
    int num_phases = 0;

    if ( rio->mutex.ok )
    {
        rocketnet_platform_mutex_acquire( &rio->mutex );
#if ROCKETNET_DEV
        if ( rio->test_hold_send )
        {
            rocketnet_platform_mutex_release( &rio->mutex );
            return false;
        }
#endif // #if ROCKETNET_DEV
        while ( rio->send_ready_count > 0 )
        {
            const int phase = rio->send_ready[rio->send_ready_head];
            rio->send_ready_head = ( rio->send_ready_head + 1 ) % ROCKETNET_RIO_SEND_PHASES;
            rio->send_ready_count--;
            phases[num_phases] = phase;
            counts[num_phases++] = rio->phase_count[phase];
            rio->phase_state[phase] = ROCKETNET_RIO_PHASE_POSTED;
        }

        const int fill = rio->fill.load( std::memory_order_relaxed );
        if ( rio->phase_state[fill] == ROCKETNET_RIO_PHASE_FILL )
        {
            const uint64_t staged = rio->next[fill].load( std::memory_order_relaxed );
            const int count = staged < (uint64_t) rio->send_queue_size ? (int) staged : rio->send_queue_size;
            if ( count > 0 )
            {
                phases[num_phases] = fill;
                counts[num_phases++] = count;
                rio->phase_state[fill] = ROCKETNET_RIO_PHASE_POSTED;
            }
        }

        rocketnet_platform_mutex_release( &rio->mutex );
    }

    for ( int i = 0; i < num_phases; ++i )
    {
        rocketnet_rio_post_phase( rio, phases[i], counts[i] );
    }
    // Closing a UDP socket cancels pending RIO requests. Keep its registered
    // region and queues until their completions have all been observed.

    if ( !rocketnet_rio_close_socket( rio ) )
    {
        return false;
    }
    // Every available completion, in batches until one comes back short, never
    // waiting for new completions.

    if ( rio->have_table && rio->send_cq != RIO_INVALID_CQ )
    {
        for ( ;; )
        {
            RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
            const ULONG count = rocketnet_rio_dequeue( rio, rio->send_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
            if ( count == RIO_CORRUPT_CQ )
            {
                rocketnet_rio_corrupt_queue( rio, "send" );
                return false;
            }
            for ( ULONG i = 0; i < count; ++i )
            {
                rocketnet_rio_retire_send( rio, &results[i] );
            }
            if ( count < ROCKETNET_RIO_DEQUEUE_BATCH )
            {
                break;
            }
        }
    }

    if ( rio->have_table && rio->receive_cq != RIO_INVALID_CQ )
    {
        for ( ;; )
        {
            RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
            const ULONG count = rocketnet_rio_dequeue( rio, rio->receive_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
            if ( count == RIO_CORRUPT_CQ )
            {
                rocketnet_rio_corrupt_queue( rio, "receive" );
                return false;
            }
            // stopping: a datagram that arrived is never banked, so it is
            // counted by name; a receive the close cancelled carries none
            uint64_t stranded = 0;
            for ( ULONG i = 0; i < count; ++i )
            {
                stranded += results[i].Status == 0 && results[i].BytesTransferred > 0 ? 1 : 0;
            }
            if ( stranded )
            {
                rocketnet_receive_drop_tally_count( rio->drops, ROCKETNET_RECEIVE_DROP_STOPPED, stranded );
            }
            rio->posted_receives.fetch_sub( (int) count, std::memory_order_relaxed );
            if ( count < ROCKETNET_RIO_DEQUEUE_BATCH )
            {
                break;
            }
        }
    }

    return rocketnet_rio_outstanding_sends( rio ) == 0 &&
           rio->posted_receives.load( std::memory_order_relaxed ) == 0;
}

// proofs/tla/RNRioWorker.tla Stop: stopping, then the wake. Never waits.
static void rocketnet_rio_lane_stop( rocketnet_rio_lane_t * rio )
{
    if ( !rio || rio->stopping.exchange( true, std::memory_order_acq_rel ) )
    {
        return;
    }
    if ( rio->wake )
    {
        SetEvent( rio->wake );
    }
}

// proofs/tla/RNRioWorker.tla Poll: main polls stopped, which changes nothing.
// The worker reports stopped only after its last drain pass. Before the
// worker exists (create's failure path) the drain runs here, one pass a call.
static bool rocketnet_rio_lane_stopped( rocketnet_rio_lane_t * rio )
{
    if ( !rio || !rio->stopping.load( std::memory_order_acquire ) )
    {
        return false;
    }
    if ( rio->thread )
    {
        return rio->exited.load( std::memory_order_acquire );
    }
    return rocketnet_rio_stop_pass( rio );
}

// proofs/tla/RNRioWorker.tla Destroy: the registered region is freed only after stopped.
// RIOCloseCompletionQueue, RIODeregisterBuffer and rocketnet_free once every request
// is accounted for, then the transport itself is freed and nothing of it runs again.
static void rocketnet_rio_lane_destroy( rocketnet_rio_lane_t * rio )
{
    if ( !rio )
    {
        return;
    }
    const bool stopped = rocketnet_rio_lane_stopped( rio );
    rocketnet_assert( stopped );
    if ( !stopped )
    {
        return;
    }
    void * context = rio->context;

    if ( rio->thread )
    {
        rocketnet_platform_thread_join( rio->thread );
        rocketnet_platform_thread_destroy( rio->thread );
    }

    if ( rio->send_cq != RIO_INVALID_CQ )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_RIO_CQ, (uintptr_t) rio->send_cq, rio->send_cq_generation ) )
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        {
            rio->rio.RIOCloseCompletionQueue( rio->send_cq );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            rocketnet_lifecycle_track_close_end( rio->send_cq_generation, true );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        }
    }

    if ( rio->receive_cq != RIO_INVALID_CQ )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_RIO_CQ, (uintptr_t) rio->receive_cq, rio->receive_cq_generation ) )
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        {
            rio->rio.RIOCloseCompletionQueue( rio->receive_cq );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            rocketnet_lifecycle_track_close_end( rio->receive_cq_generation, true );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        }
    }

    if ( rio->buffer_id != RIO_INVALID_BUFFERID )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_RIO_BUFFER, (uintptr_t) rio->buffer_id, rio->buffer_generation ) )
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        {
            rio->rio.RIODeregisterBuffer( rio->buffer_id );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            rocketnet_lifecycle_track_close_end( rio->buffer_generation, true );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        }
    }

    if ( rio->region )
    {
        rocketnet_free( context, rio->region );
    }
    if ( rio->notify )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->notify, rio->notify_generation ) )
        {
            const BOOL closed = CloseHandle( rio->notify );
            rocketnet_lifecycle_track_close_end( rio->notify_generation, closed != 0 );
        }
#else
        CloseHandle( rio->notify );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

    if ( rio->wake )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->wake, rio->wake_generation ) )
        {
            const BOOL closed = CloseHandle( rio->wake );
            rocketnet_lifecycle_track_close_end( rio->wake_generation, closed != 0 );
        }
#else
        CloseHandle( rio->wake );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

    if ( rio->mutex.ok )
    {
        rocketnet_platform_mutex_destroy( &rio->mutex );
    }
    rocketnet_free( context, rio->sized_storage );
    rio->~rocketnet_rio_lane_t();
    rocketnet_free( context, rio );
}

// The carves of the transport's sized allocation for max_clients: the send
// slots' bytes, the four receive banks, the retry list and the process view.
// Create and the memory plan both take this layout.
struct rocketnet_rio_sized_layout_t
{
    int send_queue_size, send_frames, receives, process_capacity;
    uint64_t slot_bytes, retry_slices, process_from, process_packet_bytes, process_packet_data;
    uint64_t bank_data[ROCKETNET_RIO_RECEIVE_BANKS], bank_from[ROCKETNET_RIO_RECEIVE_BANKS], bank_packet_bytes[ROCKETNET_RIO_RECEIVE_BANKS];
    uint64_t drops;
    uint64_t bytes;
};

static bool rocketnet_rio_sized_layout( int max_clients, rocketnet_rio_sized_layout_t * layout )
{
    memset( layout, 0, sizeof( *layout ) );
    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        return false;
    }

    layout->send_queue_size = rocketnet_server_rio_send_queue_size( max_clients );
    layout->send_frames = ROCKETNET_RIO_SEND_PHASES * layout->send_queue_size;
    layout->receives = rocketnet_server_rio_receives( max_clients );
    layout->process_capacity = rocketnet_server_receive_packets_per_update( max_clients );

    const uint64_t receives = (uint64_t) layout->receives;
    const uint64_t process = (uint64_t) layout->process_capacity;
    rocketnet_carve_t carve = { 0, false };
    layout->slot_bytes = rocketnet_carve( carve, (uint64_t) layout->send_frames, sizeof( int ) );
    layout->retry_slices = rocketnet_carve( carve, receives, sizeof( int ) );
    layout->process_from = rocketnet_carve( carve, process, sizeof( rocketnet_address_t ) );
    layout->process_packet_bytes = rocketnet_carve( carve, process, sizeof( int ) );
    layout->process_packet_data = rocketnet_carve( carve, process, sizeof( uint8_t * ) );
    layout->drops = rocketnet_carve( carve, 1, sizeof( rocketnet_receive_drop_tally_t ) );
    for ( int i = 0; i < ROCKETNET_RIO_RECEIVE_BANKS; ++i )
    {
        layout->bank_data[i] = rocketnet_carve( carve, receives, (uint64_t) ROCKETNET_FRAME_STRIDE );
        layout->bank_from[i] = rocketnet_carve( carve, receives, sizeof( rocketnet_address_t ) );
        layout->bank_packet_bytes[i] = rocketnet_carve( carve, receives, sizeof( int ) );
    }

    layout->bytes = carve.bytes;
    return !carve.failed && carve.bytes <= SIZE_MAX;
}

// Wires the transport to its sized allocation (base, zeroed or not): the
// counts, the slot bytes zeroed, the banks, the retry list and the process
// view, bank 0 filling and every other bank free.
static void rocketnet_rio_sized_bind( rocketnet_rio_lane_t * rio, uint8_t * base, const rocketnet_rio_sized_layout_t * layout )
{
    rio->sized_storage = base;
    rio->send_queue_size = layout->send_queue_size;
    rio->send_frames = layout->send_frames;
    rio->receives = layout->receives;
    rio->slot_bytes = (int *) ( base + layout->slot_bytes );
    memset( rio->slot_bytes, 0, sizeof( int ) * (size_t) layout->send_frames );
    rio->retry_slices = (int *) ( base + layout->retry_slices );
    rio->process.num_packets = 0;
    rio->process.capacity = layout->process_capacity;
    rio->process.from = (rocketnet_address_t *) ( base + layout->process_from );
    rio->process.packet_bytes = (int *) ( base + layout->process_packet_bytes );
    rio->process.packet_data = (uint8_t **) ( base + layout->process_packet_data );
    rio->view = &rio->process;
    rio->drops = new ( base + layout->drops ) rocketnet_receive_drop_tally_t();
    rocketnet_receive_drop_tally_reset( rio->drops );

    // RNRioWorker Init: bank 0 fills, every other bank is free
    // proofs/tla/RNBankFifoLayout.tla Init: the loop pushes banks 1 .. N - 1 on the free stack.

    rio->free_count = 0;
    for ( int i = 0; i < ROCKETNET_RIO_RECEIVE_BANKS; ++i )
    {
        rocketnet_rio_bank_t * bank = &rio->banks[i];
        bank->data = ( uint8_t( * )[ROCKETNET_FRAME_STRIDE] )( base + layout->bank_data[i] );
        bank->from = (rocketnet_address_t *) ( base + layout->bank_from[i] );
        bank->packet_bytes = (int *) ( base + layout->bank_packet_bytes[i] );
        bank->num_packets = 0;
        bank->capacity = layout->receives;
        if ( i > 0 )
        {
            rio->free_banks[rio->free_count++] = i;
        }
    }
}

// The transport: its lanes. Lane 0 binds first and its process view is the
// one view every lane appends to (rocketnet_server_rio_receive_packets).
struct rocketnet_server_rio_t
{
    void * context;
    int num_lanes;
    int lane_slots;                                           // each lane's phases times its phase size
    int receive_start;                                        // the lane receive_packets reads first, rotating
    rocketnet_rio_lane_t * lanes[ROCKETNET_RIO_MAX_LANES];
};


int rocketnet_server_rio_lane_count( int lanes )
{
    if ( lanes < 0 || lanes > ROCKETNET_RIO_MAX_LANES )
    {
        return 0;
    }
    return lanes < 1 ? 1 : lanes;
}

static uint64_t rocketnet_rio_outer_bytes();

uint64_t rocketnet_server_rio_allocation_bytes( int max_clients, int lanes )
{
    const int count = rocketnet_server_rio_lane_count( lanes );
    rocketnet_rio_sized_layout_t sized;
    if ( !count || !rocketnet_rio_sized_layout( max_clients, &sized ) )
    {
        return 0;
    }
    rocketnet_rio_layout_t layout;
    if ( !rocketnet_rio_layout_plan( sized.send_frames, sized.receives, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &layout ) )
    {
        return 0;
    }
    uint64_t lane = rocketnet_memory_allocations( { sizeof( rocketnet_rio_lane_t ), sized.bytes } );
    if ( !lane || !rocketnet_memory_add( lane, 1, rocketnet_platform_thread_allocation_bytes() ) )
    {
        return 0;
    }
    uint64_t bytes = rocketnet_memory_allocations( { rocketnet_rio_outer_bytes() } );
    if ( !bytes || !rocketnet_memory_add( bytes, (uint64_t) count, lane ) )
    {
        return 0;
    }
    return bytes;
}

uint64_t rocketnet_server_rio_registered_region_bytes( int max_clients )
{
    rocketnet_rio_sized_layout_t sized;
    if ( !rocketnet_rio_sized_layout( max_clients, &sized ) )
    {
        return 0;
    }
    rocketnet_rio_layout_t layout;
    if ( !rocketnet_rio_layout_plan( sized.send_frames, sized.receives, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &layout ) )
    {
        return 0;
    }
    return layout.total_bytes;
}

static bool rocketnet_rio_load_table( SOCKET socket, RIO_EXTENSION_FUNCTION_TABLE * table )
{
    GUID guid = WSAID_MULTIPLE_RIO;
    DWORD returned_bytes = 0;
    memset( table, 0, sizeof( *table ) );
    table->cbSize = sizeof( *table );
    return WSAIoctl( socket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &guid, sizeof( guid ), table, sizeof( *table ),
                     &returned_bytes, NULL, NULL ) != SOCKET_ERROR &&
           table->RIOReceiveEx && table->RIOSendEx && table->RIODequeueCompletion && table->RIONotify;
}

bool rocketnet_server_rio_available()
{
    SOCKET socket = WSASocketW( AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_REGISTERED_IO | WSA_FLAG_NO_HANDLE_INHERIT );
    if ( socket == INVALID_SOCKET )
    {
        return false;
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const uint64_t probe_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) socket,
                                                                      "rio extension probe socket" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) socket );
    RIO_EXTENSION_FUNCTION_TABLE table;
    const bool available = rocketnet_rio_load_table( socket, &table );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) socket, probe_generation ) )
    {
        return false;
    }
    const int closed = closesocket( socket );
    rocketnet_lifecycle_track_close_end( probe_generation, closed == 0 );
#else
    closesocket( socket );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return available;
}

static void rocketnet_rio_worker( rocketnet_platform_thread_t *, void * argument );

// the lanes a failed create leaked to the kernel (rocketnet_server_rio_create_leaks)
static std::atomic<uint64_t> rocketnet_rio_create_leaks{ 0 };
// the sockets a failed create left open because closesocket refused them with
// nothing posted (rocketnet_server_rio_create_abandoned_sockets)
static std::atomic<uint64_t> rocketnet_rio_create_abandoned_sockets{ 0 };
#if ROCKETNET_DEV
// development library only: a create rollback holds every made lane's sends
// (so none can stop) and keeps every lane it leaks here for the test to finish
static bool rocketnet_rio_test_hold_rollback = false;
static rocketnet_rio_lane_t * rocketnet_rio_test_rollback_leaked[ROCKETNET_RIO_MAX_LANES];
static int rocketnet_rio_test_num_rollback_leaked = 0;
#endif // #if ROCKETNET_DEV

// proofs/tla/RNRioWorker.tla PassFails on create's inline drain: the wait
// between two passes. Only one thing is worth waiting for: closesocket
// succeeded, which cancelled every posted receive, and some of those
// completions have not reached the receive queue yet. That wait is for the
// queue's own notification, which RIONotify sets at once on a queue that is
// not empty. A pass that failed with the socket still open has nothing on
// its way, so the next pass runs at once.
static void rocketnet_rio_create_drain_wait( rocketnet_rio_lane_t * rio )
{
    if ( rio->socket != INVALID_SOCKET || rio->posted_receives.load( std::memory_order_relaxed ) == 0 )
    {
        return;
    }
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_drain_wait )
    {
        rocketnet_rio_test_drain_wait( ROCKETNET_RIO_DRAIN_NOTIFY_MILLISECONDS );
        return;
    }
    rocketnet_rio_test_create_drain_waits.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    rio->rio.RIONotify( rio->receive_cq ); // WSAEALREADY: an earlier notify is still armed
    WaitForSingleObject( rio->notify, ROCKETNET_RIO_DRAIN_NOTIFY_MILLISECONDS );
}

// proofs/tla/RNRioWorker.tla Stop, then WStop to WDrainReceive inline
// (InlineStop) with PassFails between them: create's failure path, before
// the worker exists. Only a stalled pass counts against the bound, one
// during which posted_receives did not fall (CountStalledOnly): a drain
// that keeps accounting for cancelled receives runs until it has them all,
// however many waits they arrive over. False after
// ROCKETNET_RIO_DRAIN_WAITS + 1 stalled passes (WLeak): something is still
// kernel owned and nothing is coming.
static bool rocketnet_rio_create_failed_drain( rocketnet_rio_lane_t * rio )
{
    rocketnet_rio_lane_stop( rio );

    for ( int stalled = 0;; )
    {
        const int posted = rio->posted_receives.load( std::memory_order_relaxed );
        if ( rocketnet_rio_lane_stopped( rio ) )
        {
            return true;
        }
        if ( rio->posted_receives.load( std::memory_order_relaxed ) >= posted && ++stalled > ROCKETNET_RIO_DRAIN_WAITS )
        {
            // closesocket refused on every pass, with nothing posted and no
            // send outstanding: the kernel owns no request on the region, so
            // only the socket handle is left behind (abandoned, named and
            // counted) and the rest of the lane is freed, never leaked whole
            if ( rio->socket != INVALID_SOCKET && rio->posted_receives.load( std::memory_order_relaxed ) == 0 && rocketnet_rio_outstanding_sends( rio ) == 0 )
            {
                const uint64_t abandoned = rocketnet_rio_create_abandoned_sockets.fetch_add( 1, std::memory_order_relaxed ) + 1;
                rocketnet_error( "rio transport: create failed and closesocket refused %d times with nothing posted; socket %llu is abandoned open, the rest of the lane "
                                 "is freed (abandoned socket %llu in this process)",
                                 stalled, (unsigned long long) rio->socket, (unsigned long long) abandoned );
                rio->socket = INVALID_SOCKET;
                return rocketnet_rio_lane_stopped( rio );
            }
            return false;
        }
        rocketnet_rio_create_drain_wait( rio );
    }
}

// SIO_CPU_AFFINITY (mstcpip.h): a UDP socket given a processor before bind
// may bind an address and port that other such sockets hold, and receives
// the datagrams whose RSS processor is its own. It cannot be combined with
// SO_REUSEADDR or SO_EXCLUSIVEADDRUSE (measured: the second exclusive
// socket's bind is refused, 10013).
#ifndef SIO_CPU_AFFINITY
#define SIO_CPU_AFFINITY _WSAIOW( IOC_VENDOR, 21 )
#endif // #ifndef SIO_CPU_AFFINITY
static_assert( (unsigned long) SIO_CPU_AFFINITY == 0x98000015ul, "SIO_CPU_AFFINITY is the vendor IOCTL 21" );

// One lane: its socket, region, queues and worker. A lane of a transport with
// several takes cpu (rocketnet_rio_lane_processors): its socket is given that
// processor by SIO_CPU_AFFINITY before bind, so every lane binds the same
// address and port and receives the flows RSS hands that processor, and its
// worker is pinned to the other logical processors of that processor's core
// (rocketnet_rio_worker). A refused IOCTL refuses the lane, named in the log and as
// TRANSPORT_BIND_REFUSED; there is no fallback to a shared or exclusive bind.
// cpu NULL is a single-lane transport, bound with SO_EXCLUSIVEADDRUSE as before.
// packet_filter, when not NULL, is copied and the worker prefilters with it.
struct rocketnet_rio_processor_t
{
    int index;                                  // the processor index SIO_CPU_AFFINITY takes
    GROUP_AFFINITY affinity;                    // the same processor for SetThreadGroupAffinity
    GROUP_AFFINITY core;                        // every logical processor of its core
};

static rocketnet_rio_lane_t * rocketnet_rio_lane_create( void * context, rocketnet_address_t * bind_address, int max_clients,
                                                         const rocketnet_rio_processor_t * cpu, const uint8_t * packet_filter
#if ROCKETNET_DEV
                                                         ,
                                                         int fail_stage
#endif // #if ROCKETNET_DEV
                                                         ,
                                                         int * status )
{
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) )
    {
        return NULL;
    }
    rocketnet_rio_sized_layout_t sized;
    if ( !rocketnet_rio_sized_layout( max_clients, &sized ) )
    {
        rocketnet_warn( "rio transport refused: max_clients %d is outside 1..%d", max_clients, ROCKETNET_MAX_CLIENTS );
        return NULL;
    }
    void * storage = rocketnet_malloc( context, sizeof( rocketnet_rio_lane_t ) );
    if ( !storage )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    rocketnet_rio_lane_t * rio = new ( storage ) rocketnet_rio_lane_t();
    rio->context = context;
    if ( packet_filter )
    {
        rio->prefilter = true;
        memcpy( rio->packet_filter, packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
    }
    rio->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    // the address the caller asked for: the bound port is written into
    // bind_address once the socket is bound, and a create that fails after
    // that hands the caller back what it asked for, so the next transport of
    // the ladder never inherits this one's ephemeral port
    const rocketnet_address_t requested_address = *bind_address;

    SOCKADDR_INET address;
    int address_bytes = 0;
    // one update's inbound packets at the server's configured client count
    // (rocketnet_server_transport_receive_buffer_request; a request, and the
    // stack charges only what it holds)

    const int receive_buffer_bytes = rocketnet_server_transport_receive_buffer_request( "rio", max_clients );
    int buffer_bytes = receive_buffer_bytes;
    int granted_receive_bytes = 0;
    int granted_length = sizeof( granted_receive_bytes );
    BOOL enabled = TRUE;
    DWORD v6_only = FALSE;
    int bind_result = 0;
    RIO_NOTIFICATION_COMPLETION notification;
    memset( &notification, 0, sizeof( notification ) );

    {
        uint8_t * base = (uint8_t *) rocketnet_malloc( context, (size_t) sized.bytes );
        if ( !base )
        {
            rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
            goto failed;
        }
        rocketnet_rio_sized_bind( rio, base, &sized );
    }

    if ( !rocketnet_platform_mutex_create( &rio->mutex ) )
    {
        goto failed;
    }
    rio->wake = CreateEventW( NULL, FALSE, FALSE, NULL );
    if ( !rio->wake )
    {
        rocketnet_platform_windows_create_status( status, GetLastError() );
    }
    rio->notify = CreateEventW( NULL, FALSE, FALSE, NULL );
    if ( !rio->notify )
    {
        rocketnet_platform_windows_create_status( status, GetLastError() );
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->wake )
    {
        rio->wake_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->wake, "rio wake event" );
    }
    if ( rio->notify )
    {
        rio->notify_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->notify, "rio notify event" );
    }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !rio->wake || !rio->notify )
    {
        goto failed;
    }
    notification.Type = RIO_EVENT_COMPLETION;
    notification.Event.EventHandle = rio->notify;
    notification.Event.NotifyReset = FALSE;

    if ( !rocketnet_rio_address_write( bind_address, rio->ipv6, &address ) )
    {
        goto failed;
    }
    address_bytes = rio->ipv6 ? (int) sizeof( sockaddr_in6 ) : (int) sizeof( sockaddr_in );

    rio->socket = WSASocketW( rio->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_REGISTERED_IO | WSA_FLAG_NO_HANDLE_INHERIT );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->socket != INVALID_SOCKET )
    {
        rio->socket_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) rio->socket, "rio socket" );
    }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->socket == INVALID_SOCKET )
    {
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
        goto failed;
    }
#if ROCKETNET_DEV
    if ( fail_stage == 1 )
    {
        goto failed;
    }
#endif // #if ROCKETNET_DEV
    if ( cpu )
    {
        // before bind: the lane joins the port by its processor. The worker
        // runs on the other logical processors of that processor's core: the
        // RSS processor itself takes the receive DPCs, and a worker pinned
        // there sent 790k of 1.04M msgs/s with 250k start refusals a second
        // (r05-rioaff-f24-pinrss) where its sibling sent 1.03M with 5-7k
        // (r06-rioaff-f24-pinsibling). A core of one logical processor pins
        // the worker to that processor.
        rio->processor = cpu->index;
        rio->pin = cpu->affinity;
        if ( ( cpu->core.Mask & ~cpu->affinity.Mask ) != 0 )
        {
            rio->pin = cpu->core;
            rio->pin.Mask &= ~cpu->affinity.Mask;
        }
#if ROCKETNET_DEV
        // development library only: ROCKETNET_TEST_RIO_PIN places the lane
        // worker for a measurement: "rss" on the RSS processor, "core" on the
        // whole core, "none" unpinned; unset is the sibling placement above
        {
            char pin[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
            if ( rocketnet_platform_getenv( "ROCKETNET_TEST_RIO_PIN", pin, sizeof( pin ) ) > 0 )
            {
                if ( strcmp( pin, "rss" ) == 0 )
                {
                    rio->pin = cpu->affinity;
                }
                else if ( strcmp( pin, "core" ) == 0 )
                {
                    rio->pin = cpu->core;
                }
                else if ( strcmp( pin, "none" ) == 0 )
                {
                    rio->pin.Mask = 0;
                }
            }
        }
#endif // #if ROCKETNET_DEV
        USHORT processor = (USHORT) cpu->index;
        DWORD returned = 0;
        if ( WSAIoctl( rio->socket, SIO_CPU_AFFINITY, &processor, sizeof( processor ), NULL, 0, &returned, NULL, NULL ) == SOCKET_ERROR )
        {
            // a refusal for want of memory reads OUT_OF_MEMORY, never a bind refusal
            const int error = WSAGetLastError();
            rocketnet_create_status( status, ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
            rocketnet_platform_windows_create_status( status, (unsigned long) error );
            rocketnet_error( "rio transport refused: SIO_CPU_AFFINITY to processor %d was refused (Winsock %d); lanes share the port only through it",
                             cpu->index, error );
            goto failed;
        }
    }
    else if ( setsockopt( rio->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
    {
        goto failed;
    }
    if ( rio->ipv6 && setsockopt( rio->socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &v6_only, sizeof( v6_only ) ) == SOCKET_ERROR )
    {
        goto failed;
    }
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_RCVBUF, (const char *) &buffer_bytes, sizeof( buffer_bytes ) ) == SOCKET_ERROR )
    {
        goto failed;
    }
    if ( getsockopt( rio->socket, SOL_SOCKET, SO_RCVBUF, (char *) &granted_receive_bytes, &granted_length ) == SOCKET_ERROR )
    {
        granted_receive_bytes = 0;
    }
    rocketnet_server_receive_buffer_granted( "rio", receive_buffer_bytes, granted_receive_bytes );
    buffer_bytes = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_SNDBUF, (const char *) &buffer_bytes, sizeof( buffer_bytes ) ) == SOCKET_ERROR )
    {
        goto failed;
    }
    // Match the portable socket's don't-fragment rule, tolerating option
    // refusal: a refusal is a fallback, so it is named at warn.

    if ( rio->ipv6 && setsockopt( rio->socket, IPPROTO_IPV6, IPV6_DONTFRAG, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
    {
        rocketnet_warn( "rio transport: IPV6_DONTFRAG refused (Winsock %d); IPv6 datagrams may be fragmented", WSAGetLastError() );
    }
    if ( setsockopt( rio->socket, IPPROTO_IP, IP_DONTFRAGMENT, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
    {
        rocketnet_warn( "rio transport: IP_DONTFRAGMENT refused (Winsock %d); IPv4 datagrams may be fragmented", WSAGetLastError() );
    }
    // both ICMP reset reports off; a refusal is logged once and the receive
    // path skips the statuses

    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) rio->socket );
    bind_result = bind( rio->socket, (sockaddr *) &address, address_bytes );
#if ROCKETNET_DEV
    if ( fail_stage == 8 )
    {
        bind_result = SOCKET_ERROR;
        WSASetLastError( WSAENOBUFS );
    }
#endif // #if ROCKETNET_DEV
    if ( bind_result == SOCKET_ERROR )
    {
        // a held port or an address this host cannot bind; a refusal for want
        // of buffers (WSAENOBUFS) reads OUT_OF_MEMORY, which ends the ladder
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
        if ( rocketnet_platform_bind_refused( WSAGetLastError() ) )
        {
            rocketnet_create_status( status, ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
        }
        goto failed;
    }
    address_bytes = sizeof( address );
    if ( getsockname( rio->socket, (sockaddr *) &address, &address_bytes ) == SOCKET_ERROR )
    {
        goto failed;
    }

    if ( !rocketnet_rio_load_table( rio->socket, &rio->rio ) )
    {
        goto failed;
    }
    rio->have_table = true;
#if ROCKETNET_DEV
    if ( fail_stage == 2 )
    {
        goto failed;
    }
#endif // #if ROCKETNET_DEV

    // one region, carved only by the layout, registered once

    if ( !rocketnet_rio_layout_plan( rio->send_frames, rio->receives, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &rio->layout ) )
    {
        goto failed;
    }
    // the region comes from the caller's allocator like every other block:
    // RIORegisterBuffer takes any committed memory and locks the pages it
    // spans, so ROCKETNET_ALLOCATION_ALIGNMENT is enough (the layout's
    // sub-regions are offsets from the region's base). It is zeroed once
    // here, before any traffic.

    rio->region = (uint8_t *) rocketnet_malloc( context, (size_t) rio->layout.total_bytes );
    if ( !rio->region )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        goto failed;
    }
    memset( rio->region, 0, (size_t) rio->layout.total_bytes );
    rio->buffer_id = rio->rio.RIORegisterBuffer( (PCHAR) rio->region, (DWORD) rio->layout.total_bytes );
    if ( rio->buffer_id == RIO_INVALID_BUFFERID )
    {
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->buffer_id != RIO_INVALID_BUFFERID )
    {
        rio->buffer_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_BUFFER, (uintptr_t) rio->buffer_id, "rio buffer registration" );
    }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->buffer_id == RIO_INVALID_BUFFERID
#if ROCKETNET_DEV
         || fail_stage == 3
#endif // #if ROCKETNET_DEV
    )
    {
        goto failed;
    }

    // both completion queues notify the worker's one event after RIONotify

    rio->send_cq = rio->rio.RIOCreateCompletionQueue( (DWORD) rio->send_frames, &notification );
    if ( rio->send_cq == RIO_INVALID_CQ )
    {
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->send_cq != RIO_INVALID_CQ )
    {
        rio->send_cq_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_CQ, (uintptr_t) rio->send_cq, "rio send completion queue" );
    }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->send_cq == RIO_INVALID_CQ )
    {
        goto failed;
    }
    rio->receive_cq = rio->rio.RIOCreateCompletionQueue( (DWORD) rio->receives, &notification );
    if ( rio->receive_cq == RIO_INVALID_CQ )
    {
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->receive_cq != RIO_INVALID_CQ )
    {
        rio->receive_cq_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_CQ, (uintptr_t) rio->receive_cq, "rio receive completion queue" );
    }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->receive_cq == RIO_INVALID_CQ
#if ROCKETNET_DEV
         || fail_stage == 4
#endif // #if ROCKETNET_DEV
    )
    {
        goto failed;
    }

    // the request queue holds every send of every phase plus every receive

    rio->request_queue = rio->rio.RIOCreateRequestQueue( rio->socket, (ULONG) rio->receives, 1, (ULONG) rio->send_frames, 1,
                                                         rio->receive_cq, rio->send_cq, NULL );
    if ( rio->request_queue == RIO_INVALID_RQ )
    {
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->request_queue != RIO_INVALID_RQ )
    {
        rio->request_queue_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_RQ, (uintptr_t) rio->request_queue, "rio socket-owned request queue" );
    }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->request_queue == RIO_INVALID_RQ
#if ROCKETNET_DEV
         || fail_stage == 5
#endif // #if ROCKETNET_DEV
    )
    {
        goto failed;
    }

    // proofs/tla/RNRioWorker.tla Init: every receive slice is posted

    for ( int i = 0; i < rio->receives; ++i )
    {
        if ( !rocketnet_rio_post_receive( rio, i ) )
        {
            rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
            goto failed;
        }
#if ROCKETNET_DEV
        if ( fail_stage == 6 )
        {
            goto failed;
        }
#endif // #if ROCKETNET_DEV
    }
#if ROCKETNET_DEV
    if ( fail_stage == 7 )
    {
        goto failed;
    }
#endif // #if ROCKETNET_DEV

    if ( !rocketnet_rio_address_read( &address, rio->ipv6, bind_address ) )
    {
        goto failed;
    }

    // the worker starts last: from here every RIO call is its own

    rio->thread = rocketnet_platform_thread_create( context, rocketnet_rio_worker, rio, status );
    if ( !rio->thread )
    {
        goto failed;
    }
    rocketnet_platform_server_thread_priority( rio->thread );
    return rio;

failed:
#if ROCKETNET_DEV
    rocketnet_warn( "rio transport create failed (%s, Win32 %lu, test stage %d)", rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text, GetLastError(), fail_stage );
#else
    rocketnet_warn( "rio transport create failed (%s, Win32 %lu)", rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text, GetLastError() );
#endif
    *bind_address = requested_address;
    // Partial construction remains private to this synchronous create call.
    if ( !rocketnet_rio_create_failed_drain( rio ) )
    {
        // proofs/tla/RNRioWorker.tla WLeak: the bounded inline drain gave up;
        // the kernel may still write the region, so leak it, never free it.
        // Each leak is counted (rocketnet_server_rio_create_leaks) and named:
        // it is permanent for the process, so repeated late cancellation
        // accumulates them.
        const uint64_t leaks = rocketnet_rio_create_leaks.fetch_add( 1, std::memory_order_relaxed ) + 1;
        rocketnet_error( "rio transport: create failed with %d receives still posted; leaking the lane's socket, its %llu-byte registered region and its "
                         "completion queues to the kernel (create leak %llu in this process)",
                         rio->posted_receives.load( std::memory_order_relaxed ), (unsigned long long) rio->layout.total_bytes, (unsigned long long) leaks );
        return NULL;
    }
    rocketnet_rio_lane_destroy( rio );

    return NULL;
}

// proofs/tla/ServerStopSpread.tla Room: the open fill phase's free slots
static int rocketnet_rio_lane_send_room( rocketnet_rio_lane_t * rio )
{
    if ( rio->stopping.load( std::memory_order_acquire ) )
    {
        return 0;
    }
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const uint64_t used = rio->next[phase].load( std::memory_order_relaxed );
    return used >= (uint64_t) rio->send_queue_size ? 0 : (int) ( (uint64_t) rio->send_queue_size - used );
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: no slot staged, handed or
// posted without its completion
static bool rocketnet_rio_lane_send_idle( rocketnet_rio_lane_t * rio )
{
    const int fill = rio->fill.load( std::memory_order_relaxed );
    if ( rio->next[fill].load( std::memory_order_relaxed ) != 0 || rocketnet_rio_outstanding_sends( rio ) != 0 )
    {
        return false;
    }
    if ( !rocketnet_platform_mutex_try_acquire( &rio->mutex ) )
    {
        return false;
    }
    bool idle = rio->send_ready_count == 0;
    for ( int phase = 0; phase < ROCKETNET_RIO_SEND_PHASES; phase++ )
    {
        if ( phase != fill && rio->phase_state[phase] != ROCKETNET_RIO_PHASE_IDLE )
        {
            idle = false;
        }
    }
    rocketnet_platform_mutex_release( &rio->mutex );

    return idle;
}

// proofs/tla/RNRioWorker.tla Stage, first half: a fill thread takes a slot of the
// open fill phase.
// proofs/tla/RNRioSendSlotLayout.tla Reserve: one fetch_add per call that reaches the reserve, refused or not; a counter that wraps
// within reach hands out a staged slot again (MCRNRioSendSlotLayoutBrokenIntWrap.cfg).
// An address this socket cannot carry is refused before the reserve, as on iocp: it takes no slot, so it is
// counted once, SEND_FAILED, and never also as a full staging when it would have taken the last slot.
static uint8_t * rocketnet_rio_lane_start_packet( rocketnet_rio_lane_t * rio, const rocketnet_address_t * to, uint64_t * packet_id )
{
    if ( rio->stopping.load( std::memory_order_acquire ) )
    {
        return NULL;
    }
    SOCKADDR_INET address;
    if ( !rocketnet_rio_address_write( to, rio->ipv6, &address ) )
    {
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_SEND_FAILED );
        return NULL;
    }
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const uint64_t slot = rio->next[phase].fetch_add( 1, std::memory_order_relaxed );
    if ( slot >= (uint64_t) rio->send_queue_size )
    {
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_SEND_FULL );
        return NULL;
    }

    const int index = phase * rio->send_queue_size + (int) slot;
    rio->slot_bytes[index] = 0;
    *rocketnet_rio_send_address( rio, index ) = address;

    *packet_id = (uint64_t) index;

    return rocketnet_rio_frame( rio, index );
}

// proofs/tla/RNRioWorker.tla Stage, second half: the staged packet is postable.
// proofs/tla/RNRioSendSlotLayout.tla Finish: the reserved slot finished.
static void rocketnet_rio_lane_finish_packet( rocketnet_rio_lane_t * rio, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    // a refused finish loses the packet: the slot keeps zero bytes and the
    // post phase skips it, so the loss is counted here. The frame must be the
    // slot the id names (proofs/tla/RNRioLaneLayout.tla FinishOwn).
    const int index = rocketnet_rio_frame_index( rio, frame );
    if ( index < 0 || (uint64_t) index != packet_id || packet_bytes <= 0 || packet_bytes > ROCKETNET_RECEIVE_CEILING )
    {
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_SEND_FAILED );
        return;
    }
    rio->slot_bytes[index] = packet_bytes;
}

// proofs/tla/RNRioSendSlotLayout.tla Finish: the reserved slot aborted: it carries nothing
// and the send skips it, but its reservation stays taken until the phase is handed off.
static void rocketnet_rio_lane_abort_packet( rocketnet_rio_lane_t * rio, uint64_t packet_id, uint8_t * frame )
{
    const int index = rocketnet_rio_frame_index( rio, frame );
    if ( index < 0 || (uint64_t) index != packet_id )
    {
        return;
    }
    rio->slot_bytes[index] = 0;
}

// proofs/tla/RNRioWorker.tla SendPackets and TryFail, main with fill threads
// joined. One try of the mutex hands a non-empty fill phase to the worker, only when the next phase
// is idle (every send of it retired), and opens that phase to the fill
// threads. A failed try or a next phase not yet idle keeps the fill phase
// open: the fill threads keep appending and the next flush hands it. Main
// never waits and makes no RIO call.
// proofs/tla/RNRioSendSlotLayout.tla SendPackets: count = next[fill] clamped to the queue, the send_ready
// tail % ROCKETNET_RIO_SEND_PHASES, next[next] = 0 and fill = next.
static void rocketnet_rio_lane_send_packets( rocketnet_rio_lane_t * rio )
{
    if ( rio->stopping.load( std::memory_order_acquire ) )
    {
        return;
    }
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const uint64_t staged = rio->next[phase].load( std::memory_order_relaxed );
    if ( staged == 0 )
    {
        return;                  // nothing staged
    }
    const int count = staged < (uint64_t) rio->send_queue_size ? (int) staged : rio->send_queue_size;
#if ROCKETNET_DEV
    if ( rio->test_try_fails )
    {
        return;
    }
#endif // #if ROCKETNET_DEV
    if ( !rocketnet_platform_mutex_try_acquire( &rio->mutex ) )
    {
        return;
    }
    const int next = ( phase + 1 ) % ROCKETNET_RIO_SEND_PHASES;
    const bool hand = rio->phase_state[next] == ROCKETNET_RIO_PHASE_IDLE;
    if ( hand )
    {
        rio->phase_state[phase] = ROCKETNET_RIO_PHASE_READY;
        rio->phase_count[phase] = count;
        rio->send_ready[( rio->send_ready_head + rio->send_ready_count ) % ROCKETNET_RIO_SEND_PHASES] = phase;
        rio->send_ready_count++;
        rio->phase_state[next] = ROCKETNET_RIO_PHASE_FILL;
    }

    rocketnet_platform_mutex_release( &rio->mutex );

    if ( !hand )
    {
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_SEND_WAITS );
        return;
    }

    rio->next[next].store( 0, std::memory_order_relaxed );
    rio->fill.store( next, std::memory_order_relaxed );
    SetEvent( rio->wake );
}

// proofs/tla/RNRioWorker.tla WPost and WCommit: every finished slot of a taken phase by
// RIOSendEx with RIO_MSG_DEFER, then one RIO_MSG_COMMIT_ONLY. Between post
// and completion the kernel owns the slot's frame and its address slot.
// proofs/tla/RNRioSendSlotLayout.tla TakeAndPost: finished slots below the count posted, the rest zeroed.
// proofs/tla/RNRioLaneLayout.tla Send: a lane's worker posts the frames staged in its own lane, oldest first.
static void rocketnet_rio_post_phase( rocketnet_rio_lane_t * rio, int phase, int count )
{
    int posted = 0;

    for ( int slot = 0; slot < count; ++slot )
    {
        const int index = phase * rio->send_queue_size + slot;
        const int bytes = rio->slot_bytes[index];
        if ( bytes <= 0 )
        {
            rio->slot_bytes[index] = 0;
            continue;
        }

        RIO_BUF data;
        data.BufferId = rio->buffer_id;
        data.Offset = (ULONG) ( rocketnet_rio_send_frame_offset( &rio->layout, index ) + ROCKETNET_FRAME_WIRE_OFFSET );
        data.Length = (ULONG) bytes;
        RIO_BUF address;
        address.BufferId = rio->buffer_id;
        address.Offset = (ULONG) rocketnet_rio_send_address_offset( &rio->layout, index );
        address.Length = sizeof( SOCKADDR_INET );

        int injected = 0;
        if ( !ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST, &injected ) &&
             rocketnet_rio_send_call( rio, &data, &address, RIO_MSG_DEFER,
                                      (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, phase, (uint32_t) slot ) ) )
        {
            posted++;
        }
        else
        {
            // an unsuccessful post has no completion: the slot is free again
            rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_SEND_FAILED );
            rio->slot_bytes[index] = 0;
        }
    }

    rio->outstanding[phase].fetch_add( posted, std::memory_order_relaxed );

    if ( posted > 0 )
    {
        rio->uncommitted = true;
    }

    rocketnet_rio_commit( rio );
}

// proofs/tla/RNRioWorker.tla WDequeue for one receive completion: a failed status counts
// RECEIVE_FAILED; a datagram is checked against the size bounds and copied
// into the fill bank (the dequeue never takes more completions than the bank
// has room for); then the slice is re-posted, or waits in the retry list
static void rocketnet_rio_complete_receive( rocketnet_rio_lane_t * rio, const RIORESULT * result )
{
    const uint64_t context = (uint64_t) result->RequestContext;
    const int slice = rocketnet_rio_context_index( context );
    if ( rocketnet_rio_context_kind( context ) != ROCKETNET_RIO_CONTEXT_RECEIVE || slice < 0 || slice >= rio->receives )
    {
        rocketnet_error( "rio transport: receive completion with a foreign request context %llx", (unsigned long long) context );
        return;
    }

    rio->posted_receives.fetch_sub( 1, std::memory_order_relaxed );
    LONG status = result->Status;
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_COMPLETION, &injected ) )
    {
        status = (LONG) injected;
    }
    // an ICMP report of an earlier send (port, host or network unreachable)
    // is counted with the failed receives and the slice is re-posted below,
    // never the end of the drain

    if ( rocketnet_platform_receive_status_is_reset( (int) status ) )
    {
        rocketnet_platform_count_receive_reset();
    }
    if ( status == WSAEMSGSIZE )
    {
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_RECEIVE_DROPPED_SIZE );     // past the slice: oversize
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_RECEIVE_TOO_LARGE );
    }
    else if ( status != 0 )
    {
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_RECEIVE_FAILED );
    }
    else
    {
        const int bytes = (int) result->BytesTransferred;
        rocketnet_address_t from;
        rocketnet_rio_bank_t * bank = &rio->banks[rio->fill_bank];

        if ( bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || bytes > ROCKETNET_RECEIVE_CEILING )
        {
            rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_RECEIVE_DROPPED_SIZE );
            rocketnet_rio_count( rio, bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES ? ROCKETNET_COUNTER_RIO_RECEIVE_TOO_SHORT : ROCKETNET_COUNTER_RIO_RECEIVE_TOO_LARGE );
        }
        else if ( !rocketnet_rio_address_read( rocketnet_rio_receive_address( rio, slice ), rio->ipv6, &from ) )
        {
            rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_RECEIVE_FAILED );
        }
        else
        {
            // proofs/tla/RNRioWorker.tla WDequeue, Refused: the stateless
            // prefilter runs here, on the worker, before the datagram takes a
            // bank slot, so a flood that fails it never evicts a player's
            // packet; each refusal is counted by name in the lane's tally
            const uint8_t * data = rio->region + rocketnet_rio_receive_slice_offset( &rio->layout, slice );
            const rocketnet_receive_drop_t drop = rio->prefilter ? rocketnet_receive_prefilter( data, bytes, rio->packet_filter ) : ROCKETNET_RECEIVE_ACCEPT;
            if ( drop != ROCKETNET_RECEIVE_ACCEPT )
            {
                rocketnet_receive_drop_tally_count( rio->drops, drop );
            }
            else if ( bank->num_packets >= bank->capacity )
            {
                // the dequeue takes at most the bank's room, so this cannot
                // happen; if it ever does, the datagram is counted and dropped,
                // never written past the bank
                rocketnet_assert( bank->num_packets < bank->capacity );
                rocketnet_receive_drop_tally_count( rio->drops, ROCKETNET_RECEIVE_DROP_BANK_FULL );
            }
            else
            {
                const int index = bank->num_packets++;
                bank->from[index] = from;
                bank->packet_bytes[index] = bytes;
                memcpy( bank->data[index], data, (size_t) bytes );
                rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_PACKETS_RECEIVED );
            }
        }
    }

    // after stop nothing is re-posted. A refused re-post waits in the retry
    // list for the next worker pass, so a transient refusal never shrinks
    // the receive depth.

    if ( !rio->stopping.load( std::memory_order_acquire ) && !rocketnet_rio_post_receive( rio, slice ) )
    {
        rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_RECEIVE_REPOST_FAILED );

        const int retry = rio->num_retry.load( std::memory_order_relaxed );
        rocketnet_assert( retry < rio->receives );
        rio->retry_slices[retry] = slice;
        rio->num_retry.store( retry + 1, std::memory_order_relaxed );
    }
}

// proofs/tla/RNRioWorker.tla WDequeue, first half: one more post of each
// slice a refused re-post left in the retry list; a slice refused again stays
// for the next pass and counts
// RECEIVE_REPOST_FAILED again.
static void rocketnet_rio_retry_receives( rocketnet_rio_lane_t * rio )
{
    const int num_retry = rio->num_retry.load( std::memory_order_relaxed );
    if ( num_retry == 0 || rio->stopping.load( std::memory_order_acquire ) )
    {
        return;
    }
    int kept = 0;

    for ( int i = 0; i < num_retry; ++i )
    {
        const int slice = rio->retry_slices[i];
        if ( !rocketnet_rio_post_receive( rio, slice ) )
        {
            rocketnet_rio_count( rio, ROCKETNET_COUNTER_RIO_RECEIVE_REPOST_FAILED );
            rio->retry_slices[kept++] = slice;
        }
    }

    rio->num_retry.store( kept, std::memory_order_relaxed );
}

// proofs/tla/RNRioWorker.tla WDequeue, second half: every available receive completion up
// to the fill bank's room, and at most one bank's worth of completions a pass
// (receives), so a flood the prefilter refuses cannot hold the worker from its
// sends. A full bank leaves the rest in the queue with their slices unposted
// (backpressure), never dropped. Returns the count.
static int rocketnet_rio_dequeue_receive( rocketnet_rio_lane_t * rio )
{
    int total = 0;
    int room = rio->receives - rio->banks[rio->fill_bank].num_packets;
    int budget = rio->receives;
    if ( room == 0 )
    {
        // backpressure: the completions wait in the queue with their slices
        // unposted, so the kernel holds fewer posted receives
        rocketnet_rio_wait( rio, ROCKETNET_RIO_WAIT_RECEIVE_BANK );
    }
    while ( room > 0 && budget > 0 )
    {
        const int limit = room < budget ? room : budget;
        const ULONG batch = limit < ROCKETNET_RIO_DEQUEUE_BATCH ? (ULONG) limit : ROCKETNET_RIO_DEQUEUE_BATCH;
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rocketnet_rio_dequeue( rio, rio->receive_cq, results, batch );
        if ( count == RIO_CORRUPT_CQ )
        {
            rocketnet_rio_corrupt_queue( rio, "receive" );
            break;
        }

        for ( ULONG i = 0; i < count; ++i )
        {
            rocketnet_rio_complete_receive( rio, &results[i] );
        }
        total += (int) count;
        budget -= (int) count;
        // the room left is the bank's: a datagram the prefilter refused took
        // no slot, so a flood of them does not end the pass early
        room = rio->receives - rio->banks[rio->fill_bank].num_packets;
        if ( count < batch )
        {
            break;
        }
    }
    if ( budget == 0 )
    {
        // a whole lane of receives completed before this pass: every slice
        // had been used, so the kernel had none posted for a while
        rocketnet_rio_wait( rio, ROCKETNET_RIO_WAIT_RECEIVE_RAN_DRY );
    }

    return total;
}

// proofs/tla/RNRioWorker.tla WRetireFirst, WTake, WRetire and WPublish: one
// worker pass, with WPost, WCommit and WDequeue in the calls it makes. True
// when it did work.
// proofs/tla/RNBankFifoLayout.tla Publish: the receive ready tail % ROCKETNET_RIO_RECEIVE_BANKS and the free
// stack's top. proofs/tla/RNRioSendSlotLayout.tla TakeAndPost and Retire: the send_ready head % ROCKETNET_RIO_SEND_PHASES
// and a drained posted phase idle again.
static bool rocketnet_rio_worker_pass( rocketnet_rio_lane_t * rio )
{
    bool progress = false;

    // WRetireFirst: a refused commit is retried and every send completion
    // retired before the take, so the take returns every phase the kernel
    // has finished: main's next flush finds its next phase idle unless the
    // kernel really is behind (proofs/tla/RNRioWorker.tla DrainedReturnedAtPost)

    rocketnet_rio_commit( rio );
    if ( rocketnet_rio_outstanding_sends( rio ) > 0 &&
         rocketnet_rio_dequeue_send( rio ) > 0 )
    {
        progress = true;
    }

    // WTake: every drained posted phase back to main as idle, then the
    // oldest handed phase, which main now sees posted

    int phase = -1;
    int count = 0;
    rocketnet_platform_mutex_acquire( &rio->mutex );

    for ( int p = 0; p < ROCKETNET_RIO_SEND_PHASES; ++p )
    {
        if ( rio->phase_state[p] == ROCKETNET_RIO_PHASE_POSTED && rio->outstanding[p].load( std::memory_order_relaxed ) == 0 )
        {
            rio->phase_state[p] = ROCKETNET_RIO_PHASE_IDLE;
        }
    }

    if ( rio->send_ready_count > 0
#if ROCKETNET_DEV
         && !rio->test_hold_send
#endif // #if ROCKETNET_DEV
    )
    {
        phase = rio->send_ready[rio->send_ready_head];
        rio->send_ready_head = ( rio->send_ready_head + 1 ) % ROCKETNET_RIO_SEND_PHASES;
        rio->send_ready_count--;
        rio->phase_state[phase] = ROCKETNET_RIO_PHASE_POSTED;
        count = rio->phase_count[phase];
    }

    rocketnet_platform_mutex_release( &rio->mutex );

    // WPost and WCommit

    if ( phase >= 0 )
    {
        rocketnet_rio_post_phase( rio, phase, count );
        progress = true;
    }

    // WRetire: a refused commit is retried, then every send completion

    rocketnet_rio_commit( rio );
    if ( rocketnet_rio_outstanding_sends( rio ) > 0 &&
         rocketnet_rio_dequeue_send( rio ) > 0 )
    {
        progress = true;
    }

    // WDequeue

    rocketnet_rio_retry_receives( rio );
    if ( rocketnet_rio_dequeue_receive( rio ) > 0 )
    {
        progress = true;
    }

    // WPublish: a non-empty fill bank for a free bank, and every drained
    // posted phase back to main as idle

    bool published = false;
    rocketnet_platform_mutex_acquire( &rio->mutex );

    if ( rio->banks[rio->fill_bank].num_packets > 0 && rio->free_count > 0 )
    {
        rio->ready_banks[( rio->ready_head + rio->ready_count ) % ROCKETNET_RIO_RECEIVE_BANKS] = rio->fill_bank;
        rio->ready_count++;
        rio->fill_bank = rio->free_banks[--rio->free_count];
        published = true;
    }

    for ( int p = 0; p < ROCKETNET_RIO_SEND_PHASES; ++p )
    {
        if ( rio->phase_state[p] == ROCKETNET_RIO_PHASE_POSTED && rio->outstanding[p].load( std::memory_order_relaxed ) == 0 )
        {
            rio->phase_state[p] = ROCKETNET_RIO_PHASE_IDLE;
        }
    }
    rocketnet_platform_mutex_release( &rio->mutex );
    if ( published )
    {
        rio->banks[rio->fill_bank].num_packets = 0;
    }
    return progress;
}

// proofs/tla/RNRioWorker.tla WArm and WWait: nothing progressed. RIONotify on the receive
// queue when the fill bank has room (a full bank waits for main's wake
// alone), and on the send queue while sends are outstanding; RIONotify on a
// non-empty queue sets the event at once. A refused commit or re-post waits
// one millisecond and retries; otherwise the bounded wait tolerates a lost
// hint.
static void rocketnet_rio_idle_wait( rocketnet_rio_lane_t * rio )
{
    const bool sends = rocketnet_rio_outstanding_sends( rio ) > 0;
    if ( rio->banks[rio->fill_bank].num_packets < rio->receives )
    {
        rio->rio.RIONotify( rio->receive_cq );  // WSAEALREADY: an earlier notify is still armed
    }
    if ( sends )
    {
        rio->rio.RIONotify( rio->send_cq );
    }
    const bool retry = rio->uncommitted || rio->num_retry.load( std::memory_order_relaxed ) > 0;
    HANDLE handles[2] = { rio->wake, rio->notify };
    WaitForMultipleObjects( 2, handles, FALSE, retry ? 1 : 50 );
}

// At the worker's exit, after its last stop pass: the datagrams banked but
// never handed to main, in its fill bank and its ready banks (main's receive
// takes nothing once stopping), are counted as RECEIVE_DROP_STOPPED (the public
// row RECEIVE_DROPPED_STOPPED), so a datagram the socket delivered that main
// never took is counted there. The banks
// main was exposed were delivered. Each bank is emptied, so a count is never
// taken twice; the banks keep their places (RNBankFifoLayout has no step
// after stop).
static void rocketnet_rio_drop_unread_banks( rocketnet_rio_lane_t * rio )
{
    uint64_t datagrams = 0;
    rocketnet_platform_mutex_acquire( &rio->mutex );
    datagrams += (uint64_t) rio->banks[rio->fill_bank].num_packets;
    rio->banks[rio->fill_bank].num_packets = 0;
    for ( int k = 0; k < rio->ready_count; ++k )
    {
        const int index = rio->ready_banks[( rio->ready_head + k ) % ROCKETNET_RIO_RECEIVE_BANKS];
        datagrams += (uint64_t) rio->banks[index].num_packets;
        rio->banks[index].num_packets = 0;
    }
    rocketnet_platform_mutex_release( &rio->mutex );
    if ( datagrams )
    {
        rocketnet_receive_drop_tally_count( rio->drops, ROCKETNET_RECEIVE_DROP_STOPPED, datagrams );
    }
}

#if ROCKETNET_DEV
// development library only: the unit seam makes the platform refuse the pin
static bool rocketnet_rio_test_refuse_pin = false;
#endif // #if ROCKETNET_DEV

// a lane's worker runs beside its socket's processor, on that core's other
// logical processors (rocketnet_rio_lane_create); a refused pin leaves it
// where Windows schedules it, and is counted (ROCKETNET_COUNTER_RIO_PIN_REFUSALS)
static void rocketnet_rio_place_worker( rocketnet_rio_lane_t * rio )
{
    if ( rio->processor < 0 || rio->pin.Mask == 0 )
    {
        rio->placement.store( ROCKETNET_RIO_PLACEMENT_NOT_ASKED, std::memory_order_release );
        return;
    }

    bool pinned = false;
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_refuse_pin )
    {
        SetLastError( ERROR_ACCESS_DENIED );
    }
    else
#endif // #if ROCKETNET_DEV
    {
        pinned = SetThreadGroupAffinity( GetCurrentThread(), &rio->pin, NULL ) != FALSE;
    }

    if ( pinned )
    {
        rio->placement.store( ROCKETNET_RIO_PLACEMENT_PINNED, std::memory_order_release );
    }
    else
    {
        rio->placement_error.store( GetLastError(), std::memory_order_relaxed );
        rio->pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        rio->placement.store( ROCKETNET_RIO_PLACEMENT_REFUSED, std::memory_order_release );
    }
}

// proofs/tla/RNRioWorker.tla WTop: the loop reads stopping at the top of every pass
// and turns to the stop pass once it is set.
static void rocketnet_rio_worker( rocketnet_platform_thread_t *, void * argument )
{
    rocketnet_rio_lane_t * rio = (rocketnet_rio_lane_t *) argument;

    rocketnet_rio_place_worker( rio );

    for ( ;; )
    {
        if ( rio->stopping.load( std::memory_order_acquire ) )
        {
            if ( rocketnet_rio_stop_pass( rio ) )
            {
                rocketnet_rio_drop_unread_banks( rio );
                break;
            }
            WaitForSingleObject( rio->notify, 1 );
            continue;
        }

        if ( !rocketnet_rio_worker_pass( rio ) )
        {
            rocketnet_rio_idle_wait( rio );
        }
    }

    rio->exited.store( true, std::memory_order_release );
}

// proofs/tla/RNRioWorker.tla ReceivePackets and TryFail, main: one try of the
// mutex returns the banks exposed last call and exposes the ready banks that fit the process view,
// oldest first; the views stay valid until the next call. A failed try
// exposes nothing new and retains everything main owns. Main wakes the
// worker when it returns banks to an empty free list (a worker with a full
// fill bank may be waiting for one).
// proofs/tla/RNBankFifoLayout.tla Release and Take: the exposed banks back on the free stack, then the
// ready banks that fit from the ready head, MCRNBankFifoLayoutLifoExposed.cfg.
// The lane appends to its view after what earlier lanes appended this call;
// the caller empties the view first (rocketnet_server_rio_receive_packets).
static void rocketnet_rio_lane_receive_packets( rocketnet_rio_lane_t * rio )
{
    if ( rio->stopping.load( std::memory_order_acquire ) )
    {
        return;
    }
#if ROCKETNET_DEV
    if ( rio->test_try_fails )
    {
        return;
    }
#endif // #if ROCKETNET_DEV
    if ( !rocketnet_platform_mutex_try_acquire( &rio->mutex ) )
    {
        return;
    }
    const bool wake = rio->free_count == 0 && rio->exposed_count > 0;
    for ( int j = 0; j < rio->exposed_count; ++j )
    {
        rio->free_banks[rio->free_count++] = rio->exposed_banks[j];
    }
    rio->exposed_count = 0;

    int total = 0;

    while ( rio->ready_count > 0 )
    {
        const int candidate = rio->ready_banks[rio->ready_head];
        if ( rio->view->num_packets + total + rio->banks[candidate].num_packets > rio->view->capacity )
        {
            // the oldest ready bank waits for the next call: the process view
            // (shared by the lanes, filled in lane order) has no room for it
            rocketnet_rio_wait( rio, ROCKETNET_RIO_WAIT_RECEIVE_VIEW );
            break;
        }
        rio->ready_head = ( rio->ready_head + 1 ) % ROCKETNET_RIO_RECEIVE_BANKS;
        rio->ready_count--;
        rio->exposed_banks[rio->exposed_count++] = candidate;
        total += rio->banks[candidate].num_packets;
    }

    rocketnet_platform_mutex_release( &rio->mutex );
    if ( wake )
    {
        SetEvent( rio->wake );
    }
    for ( int j = 0; j < rio->exposed_count; ++j )
    {
        rocketnet_rio_bank_t * bank = &rio->banks[rio->exposed_banks[j]];
        for ( int i = 0; i < bank->num_packets; ++i )
        {
            const int index = rio->view->num_packets++;
            rio->view->from[index] = bank->from[i];
            rio->view->packet_bytes[index] = bank->packet_bytes[i];
            rio->view->packet_data[index] = bank->data[i];
        }
    }
}

#if ROCKETNET_DEV
// The unit fixtures are the default configuration: its send queue and
// receive slices, as constants for the fake kernels' fixed arrays.
#define ROCKETNET_RIO_TEST_STAGING ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_DEFAULT_MAX_CLIENTS )
#define ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE \
    ( ROCKETNET_RIO_TEST_STAGING > ROCKETNET_RIO_MIN_SEND_QUEUE_SIZE ? ROCKETNET_RIO_TEST_STAGING : ROCKETNET_RIO_MIN_SEND_QUEUE_SIZE )
#define ROCKETNET_RIO_TEST_RECEIVES ( 2 * ROCKETNET_RIO_TEST_STAGING )

struct rocketnet_rio_overflow_test_state_t
{
    const RIORESULT * completions;
    ULONG num_completions;
    ULONG next_completion;
    int repost_calls;
    int dequeue_calls;
    int guard_hit;
    int guard_after_calls;
};

static rocketnet_rio_overflow_test_state_t * rocketnet_rio_overflow_test_state = NULL;

// main's receive call on one lane, as rocketnet_server_rio_receive_packets
// makes it: the view emptied, then the lane's banks appended
static void rocketnet_rio_test_receive( rocketnet_rio_lane_t * rio )
{
    rio->view->num_packets = 0;
    rocketnet_rio_lane_receive_packets( rio );
}

static ULONG WINAPI rocketnet_rio_overflow_test_dequeue( RIO_CQ cq, PRIORESULT results, ULONG max_results )
{
    if ( !rocketnet_rio_overflow_test_state || cq != (RIO_CQ) (uintptr_t) 1 )
    {
        return RIO_CORRUPT_CQ;
    }
    if ( rocketnet_rio_overflow_test_state->dequeue_calls >= rocketnet_rio_overflow_test_state->guard_after_calls )
    {
        rocketnet_rio_overflow_test_state->guard_hit = 1;
        return RIO_CORRUPT_CQ;
    }

    rocketnet_rio_overflow_test_state->dequeue_calls++;

    for ( ULONG i = 0; i < max_results; ++i )
    {
        results[i].Status = 0;
        results[i].BytesTransferred = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
        results[i].RequestContext = (ULONGLONG) rocketnet_rio_context(
            ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) ( i % ROCKETNET_RIO_TEST_RECEIVES ) );
    }

    return max_results;
}

static BOOL WINAPI rocketnet_rio_overflow_test_receive( RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF,
                                                        PRIO_BUF, PRIO_BUF, DWORD, PVOID )
{
    if ( !rocketnet_rio_overflow_test_state )
    {
        return FALSE;
    }
    rocketnet_rio_overflow_test_state->repost_calls++;
    return TRUE;
}

// A unit fixture: a transport with its region, banks and mutex but no
// socket, no queues and no worker. The seams drive the real worker pass and
// the real main calls on one thread, in the interleavings they choose. Every
// block comes from the counted allocator and is zeroed, so a fixture that
// does not free what it took shows in the allocator's count.
static void * rocketnet_rio_test_zeroed( size_t bytes )
{
    void * block = rocketnet_malloc( NULL, bytes );
    if ( block )
    {
        memset( block, 0, bytes );
    }
    return block;
}

static rocketnet_rio_lane_t * rocketnet_rio_test_fixture()
{
    void * storage = rocketnet_rio_test_zeroed( sizeof( rocketnet_rio_lane_t ) );
    if ( !storage )
    {
        return NULL;
    }
    rocketnet_rio_lane_t * rio = new ( storage ) rocketnet_rio_lane_t();
    rocketnet_rio_sized_layout_t sized;
    bool ok = rocketnet_rio_sized_layout( ROCKETNET_DEFAULT_MAX_CLIENTS, &sized );
    uint8_t * base = ok ? (uint8_t *) rocketnet_rio_test_zeroed( (size_t) sized.bytes ) : NULL;
    if ( base )
    {
        rocketnet_rio_sized_bind( rio, base, &sized );
    }
    rocketnet_assert( !base || ( rio->send_queue_size == ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE && rio->receives == ROCKETNET_RIO_TEST_RECEIVES ) );
    ok = ok && base && rocketnet_rio_layout_plan( rio->send_frames, rio->receives, ROCKETNET_FRAME_STRIDE, ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &rio->layout );
    rio->region = ok ? (uint8_t *) rocketnet_rio_test_zeroed( (size_t) rio->layout.total_bytes ) : NULL;
    ok = ok && rio->region && rocketnet_platform_mutex_create( &rio->mutex );
    rio->wake = ok ? CreateEventW( NULL, FALSE, FALSE, NULL ) : NULL;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->wake )
    {
        rio->wake_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->wake,
                                                               "rio unit fixture wake event" );
    }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !ok || !rio->wake )
    {
        if ( rio->mutex.ok )
        {
            rocketnet_platform_mutex_destroy( &rio->mutex );
        }
        rocketnet_free( NULL, rio->sized_storage );
        rocketnet_free( NULL, rio->region );
        rio->~rocketnet_rio_lane_t();
        rocketnet_free( NULL, storage );
        return NULL;
    }

    for ( int i = 0; i < rio->receives; ++i )
    {
        SOCKADDR_INET * address = (SOCKADDR_INET *) ( rio->region + rocketnet_rio_receive_address_offset( &rio->layout, i ) );
        address->Ipv4.sin_family = AF_INET;
        address->Ipv4.sin_port = htons( 40000 );
        address->Ipv4.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    }

    rio->receive_cq = (RIO_CQ) (uintptr_t) 1;
    rio->send_cq = (RIO_CQ) (uintptr_t) 2;
    rio->posted_receives.store( rio->receives, std::memory_order_relaxed );

    return rio;
}

static void rocketnet_rio_test_fixture_free( rocketnet_rio_lane_t * rio )
{
    rocketnet_rio_test_functions = NULL;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->wake, rio->wake_generation ) )
    {
        const BOOL closed = CloseHandle( rio->wake );
        rocketnet_lifecycle_track_close_end( rio->wake_generation, closed != 0 );
    }
#else
    CloseHandle( rio->wake );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_platform_mutex_destroy( &rio->mutex );
    rocketnet_free( NULL, rio->sized_storage );
    rocketnet_free( NULL, rio->region );
    rio->~rocketnet_rio_lane_t();
    rocketnet_free( NULL, rio );
}

// A test-only fixture drives the production worker pass and receive_packets
// with an endlessly full fake queue. Its dequeue guard makes an unbounded
// loop return a corrupt-CQ sentinel; the bounded pass returns before it.
// counters holds two arrays of ROCKETNET_NUM_COUNTERS (after the first pass,
// after the second); each gets the rio rows at their public indices.
bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                 int * repost_calls, int * pending_receives, int * guard_hit )
{
    if ( !counters || !packet_counts || !repost_calls || !pending_receives || !guard_hit )
    {
        return false;
    }
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }

    const int calls_per_bank = ( rio->receives + ROCKETNET_RIO_DEQUEUE_BATCH - 1 ) / ROCKETNET_RIO_DEQUEUE_BATCH;
    rocketnet_rio_overflow_test_state_t state = { NULL, 0, 0, 0, 0, 0, 2 * calls_per_bank };
    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_overflow_test_dequeue,
        rocketnet_rio_overflow_test_receive,
        NULL,
        NULL
    };
    rocketnet_rio_overflow_test_state = &state;
    rocketnet_rio_test_functions = &functions;

    rocketnet_rio_worker_pass( rio );
    rocketnet_rio_test_receive( rio );
    packet_counts[0] = rio->process.num_packets;
    for ( int i = 0; i < ROCKETNET_SERVER_RIO_COUNTER_ROWS; ++i )
    {
        counters[ROCKETNET_SERVER_RIO_FIRST_COUNTER + i] = rio->counters[i].load( std::memory_order_relaxed );
    }

    // The queue stays full through the guard. A bounded pass fills one bank
    // and leaves the still-full queue for the next pass.

    rocketnet_rio_worker_pass( rio );
    rocketnet_rio_test_receive( rio );
    packet_counts[1] = rio->process.num_packets;
    for ( int i = 0; i < ROCKETNET_SERVER_RIO_COUNTER_ROWS; ++i )
    {
        counters[ROCKETNET_NUM_COUNTERS + ROCKETNET_SERVER_RIO_FIRST_COUNTER + i] = rio->counters[i].load( std::memory_order_relaxed );
    }
    *repost_calls = state.repost_calls;
    *pending_receives = rio->posted_receives.load( std::memory_order_relaxed );
    *guard_hit = state.guard_hit;

    rocketnet_rio_overflow_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return true;
}

// The handoff seam's fake kernel: an endless receive queue that can be
// paused, and a send queue whose deferred posts complete at commit.
struct rocketnet_rio_handoff_test_state_t
{
    bool receive_paused;
    int receive_dequeues;
    int reposts;
    int deferred;
    ULONGLONG deferred_context[2 * ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE];
    int completed;
    ULONGLONG completed_context[2 * ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE];
    int commits;
};

static rocketnet_rio_handoff_test_state_t * rocketnet_rio_handoff_test_state = NULL;

static ULONG WINAPI rocketnet_rio_handoff_test_dequeue( RIO_CQ cq, PRIORESULT results, ULONG max_results )
{
    rocketnet_rio_handoff_test_state_t * state = rocketnet_rio_handoff_test_state;

    if ( cq == (RIO_CQ) (uintptr_t) 2 )
    {
        ULONG count = 0;
        while ( count < max_results && state->completed > 0 )
        {
            results[count].Status = 0;
            results[count].BytesTransferred = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
            results[count].RequestContext = state->completed_context[--state->completed];
            count++;
        }

        return count;
    }

    if ( state->receive_paused )
    {
        return 0;
    }
    state->receive_dequeues++;

    for ( ULONG i = 0; i < max_results; ++i )
    {
        results[i].Status = 0;
        results[i].BytesTransferred = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
        results[i].RequestContext = (ULONGLONG) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) ( i % ROCKETNET_RIO_TEST_RECEIVES ) );
    }

    return max_results;
}

static BOOL WINAPI rocketnet_rio_handoff_test_receive( RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF, DWORD, PVOID )
{
    rocketnet_rio_handoff_test_state->reposts++;
    return TRUE;
}

static BOOL WINAPI rocketnet_rio_handoff_test_send( RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF, DWORD flags, PVOID context )
{
    rocketnet_rio_handoff_test_state_t * state = rocketnet_rio_handoff_test_state;

    if ( flags & RIO_MSG_COMMIT_ONLY )
    {
        state->commits++;
        while ( state->deferred > 0 )
        {
            state->completed_context[state->completed++] = state->deferred_context[--state->deferred];
        }
        return TRUE;
    }

    state->deferred_context[state->deferred++] = (ULONGLONG) (uintptr_t) context;

    return TRUE;
}

static bool rocketnet_rio_test_stage( rocketnet_rio_lane_t * rio, int count )
{
    rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40001;

    for ( int i = 0; i < count; ++i )
    {
        uint64_t id = 0;
        uint8_t * frame = rocketnet_rio_lane_start_packet( rio, &to, &id );
        if ( !frame )
        {
            return false;
        }
        rocketnet_rio_lane_finish_packet( rio, id, frame, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }

    return true;
}

static bool rocketnet_rio_test_woken( rocketnet_rio_lane_t * rio )
{
    return WaitForSingleObject( rio->wake, 0 ) == WAIT_OBJECT_0;
}

// The RNRioWorker transitions, forced in order on one thread with no socket
// and no clock: receive backpressure with every bank full, a failed main try
// that retains its exposed banks, the wake on a return to an empty free list,
// and the send phase handoff that reopens a phase only when it is idle.
bool rocketnet_server_rio_test_worker_handoff()
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }
    static rocketnet_rio_handoff_test_state_t state;
    memset( &state, 0, sizeof( state ) );

    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_handoff_test_dequeue,
        rocketnet_rio_handoff_test_receive,
        rocketnet_rio_handoff_test_send,
        NULL
    };
    rocketnet_rio_handoff_test_state = &state;
    rocketnet_rio_test_functions = &functions;

    bool valid = true;

    // Receive backpressure: each pass fills one bank and publishes it while a
    // bank is free. The last bank fills with nothing free, and later passes
    // dequeue nothing: completions stay queued, nothing is dropped.

    for ( int pass = 0; pass < ROCKETNET_RIO_RECEIVE_BANKS; ++pass )
    {
        valid &= rocketnet_rio_worker_pass( rio );
    }
    valid &= rio->ready_count == ROCKETNET_RIO_RECEIVE_BANKS - 1 && rio->free_count == 0 &&
             rio->banks[rio->fill_bank].num_packets == rio->receives;
    const int dequeues = state.receive_dequeues;
    valid &= !rocketnet_rio_worker_pass( rio ) && state.receive_dequeues == dequeues;
    valid &= rio->posted_receives.load() == rio->receives && state.reposts == ROCKETNET_RIO_RECEIVE_BANKS * rio->receives;

    // Main exposes the ready banks that fit the process view, oldest first.

    const int fit = rio->process.capacity / rio->receives < ROCKETNET_RIO_RECEIVE_BANKS - 1
                        ? rio->process.capacity / rio->receives
                        : ROCKETNET_RIO_RECEIVE_BANKS - 1;
    const int oldest = rio->ready_banks[rio->ready_head];
    rocketnet_rio_test_receive( rio );
    valid &= rio->exposed_count == fit && rio->exposed_banks[0] == oldest &&
             rio->process.num_packets == fit * rio->receives &&
             rio->process.packet_data[0] == rio->banks[oldest].data[0] && !rocketnet_rio_test_woken( rio );

    // A failed try exposes nothing new and keeps the exposed banks main's.

    rio->test_try_fails = true;
    rocketnet_rio_test_receive( rio );
    valid &= rio->process.num_packets == 0 && rio->exposed_count == fit && rio->free_count == 0;
    rio->test_try_fails = false;

    // Returning banks to an empty free list wakes the worker. Its next pass
    // publishes the full bank, and the pass after fills a free one.

    rocketnet_rio_test_receive( rio );
    valid &= rocketnet_rio_test_woken( rio );
    rocketnet_rio_worker_pass( rio );
    valid &= rio->ready_count == 1 && state.receive_dequeues == dequeues;
    valid &= rocketnet_rio_worker_pass( rio ) && state.receive_dequeues > dequeues;
    state.receive_paused = true;

    for ( int call = 0; call < ROCKETNET_RIO_RECEIVE_BANKS; ++call )
    {
        rocketnet_rio_worker_pass( rio );
        rocketnet_rio_test_receive( rio );
    }

    rocketnet_rio_test_receive( rio );
    valid &= rio->ready_count == 0 && rio->exposed_count == 0 && rio->process.num_packets == 0;
    valid &= rio->counters[ROCKETNET_COUNTER_RIO_PACKETS_RECEIVED - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() ==
             (uint64_t) ( ROCKETNET_RIO_RECEIVE_BANKS + 1 ) * (uint64_t) rio->receives;

    // Send: each flush hands the fill phase and opens the next, idle phase.

    valid &= rocketnet_rio_test_stage( rio, 2 );
    rocketnet_rio_lane_send_packets( rio );
    valid &= rio->fill.load() == 1 && rocketnet_rio_test_woken( rio );
    valid &= rocketnet_rio_test_stage( rio, 3 );
    rocketnet_rio_lane_send_packets( rio );
    valid &= rio->fill.load() == 2 && rio->send_ready_count == 2;

    // Phase 0, the next phase, is still the worker's: the flush keeps phase 2
    // open and the fill threads keep appending to it, nothing refused.

    valid &= rocketnet_rio_test_stage( rio, 1 );
    rocketnet_rio_lane_send_packets( rio );
    valid &= rio->fill.load() == 2 && rio->next[2].load() == 1 &&
             rio->counters[ROCKETNET_COUNTER_RIO_SEND_WAITS - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 1;
    valid &= rocketnet_rio_test_stage( rio, 1 );

    // A failed try keeps the fill phase too.

    rio->test_try_fails = true;
    rocketnet_rio_lane_send_packets( rio );
    rio->test_try_fails = false;
    valid &= rio->fill.load() == 2 && rio->next[2].load() == 2;

    // The worker takes the oldest handed phase, 0, posts and commits it, and
    // returns it idle once its completions are retired. Phase 1 waits handed.

    valid &= rocketnet_rio_worker_pass( rio ) && state.commits == 1 &&
             rio->phase_state[0] == ROCKETNET_RIO_PHASE_IDLE && rio->phase_state[1] == ROCKETNET_RIO_PHASE_READY;
    // Now the flush hands phase 2 with both of its packets and opens phase 0.

    rocketnet_rio_lane_send_packets( rio );
    valid &= rio->fill.load() == 0 && rio->phase_count[2] == 2 && rocketnet_rio_test_stage( rio, 1 );
    valid &= rocketnet_rio_worker_pass( rio ) && state.commits == 2 && rio->phase_state[1] == ROCKETNET_RIO_PHASE_IDLE;
    valid &= rocketnet_rio_worker_pass( rio ) && state.commits == 3 && rio->phase_state[2] == ROCKETNET_RIO_PHASE_IDLE;
    valid &= rio->counters[ROCKETNET_COUNTER_RIO_PACKETS_SENT - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 7 && rocketnet_rio_outstanding_sends( rio ) == 0 &&
             rio->phase_state[0] == ROCKETNET_RIO_PHASE_FILL && rio->counters[ROCKETNET_COUNTER_RIO_SEND_FULL - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 0;

    rocketnet_rio_handoff_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

// The stop seam's fake kernel: a commit moves the deferred sends in flight,
// where they stay until the case delivers them; closesocket cancels every
// send still in flight and every posted receive into the completion queues.
struct rocketnet_rio_stop_test_state_t
{
    int deferred;
    ULONGLONG deferred_context[ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE];
    int in_flight;
    ULONGLONG in_flight_context[ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE];
    int completed;
    ULONGLONG completed_context[ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE];
    LONG completed_status[ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE];
    int cancelled_receives;
    int closes;
    // create's failure drain: closesocket refused every time, or cancelled
    // receives that reach the completion queue only over late_waits waits
    bool refuse_close;
    int late_waits;
    int late_receives;
    int waits;
    DWORD longest_wait_milliseconds;
    // closesocket completes this many posted receives with a datagram (they
    // arrived before the close), the rest cancelled; corrupt_receive makes the
    // receive queue's dequeue report RIO_CORRUPT_CQ
    int arrive_at_close;
    int arrived_receives;
    bool corrupt_receive;
};

static rocketnet_rio_stop_test_state_t * rocketnet_rio_stop_test_state = NULL;

static void rocketnet_rio_stop_test_complete_in_flight( LONG status )
{
    rocketnet_rio_stop_test_state_t * state = rocketnet_rio_stop_test_state;
    while ( state->in_flight > 0 )
    {
        state->completed_context[state->completed] = state->in_flight_context[--state->in_flight];
        state->completed_status[state->completed++] = status;
    }
}

static ULONG WINAPI rocketnet_rio_stop_test_dequeue( RIO_CQ cq, PRIORESULT results, ULONG max_results )
{
    rocketnet_rio_stop_test_state_t * state = rocketnet_rio_stop_test_state;

    ULONG count = 0;
    if ( cq == (RIO_CQ) (uintptr_t) 2 )
    {
        while ( count < max_results && state->completed > 0 )
        {
            state->completed--;
            results[count].Status = state->completed_status[state->completed];
            results[count].BytesTransferred = state->completed_status[state->completed] == 0 ? ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES : 0;
            results[count].RequestContext = state->completed_context[state->completed];
            count++;
        }

        return count;
    }

    if ( state->corrupt_receive )
    {
        return RIO_CORRUPT_CQ;
    }
    while ( count < max_results && state->arrived_receives > 0 )
    {
        state->arrived_receives--;
        results[count].Status = 0;
        results[count].BytesTransferred = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
        results[count].RequestContext = (ULONGLONG) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) count );
        count++;
    }
    while ( count < max_results && state->cancelled_receives > 0 )
    {
        state->cancelled_receives--;
        results[count].Status = (LONG) WSA_OPERATION_ABORTED;
        results[count].BytesTransferred = 0;
        results[count].RequestContext = (ULONGLONG) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) count );
        count++;
    }

    return count;
}

static BOOL WINAPI rocketnet_rio_stop_test_send( RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF, DWORD flags, PVOID context )
{
    rocketnet_rio_stop_test_state_t * state = rocketnet_rio_stop_test_state;

    if ( flags & RIO_MSG_COMMIT_ONLY )
    {
        while ( state->deferred > 0 )
        {
            state->in_flight_context[state->in_flight++] = state->deferred_context[--state->deferred];
        }
        return TRUE;
    }

    state->deferred_context[state->deferred++] = (ULONGLONG) (uintptr_t) context;

    return TRUE;
}

static BOOL WINAPI rocketnet_rio_stop_test_receive( RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF, DWORD, PVOID )
{
    return TRUE;
}

static int WINAPI rocketnet_rio_stop_test_close( SOCKET )
{
    rocketnet_rio_stop_test_state_t * state = rocketnet_rio_stop_test_state;
    state->closes++;
    if ( state->refuse_close )
    {
        return SOCKET_ERROR;
    }
    rocketnet_rio_stop_test_complete_in_flight( (LONG) WSA_OPERATION_ABORTED );
    if ( state->late_waits > 0 )
    {
        state->late_receives = ROCKETNET_RIO_TEST_RECEIVES;
    }
    else
    {
        state->arrived_receives = state->arrive_at_close;
        state->cancelled_receives = ROCKETNET_RIO_TEST_RECEIVES - state->arrive_at_close;
    }
    return 0;
}

// one wait of create's failure drain: an equal share of the late cancelled
// receives reaches the completion queue, the remainder with the last wait
static void rocketnet_rio_stop_test_drain_wait( DWORD milliseconds )
{
    rocketnet_rio_stop_test_state_t * state = rocketnet_rio_stop_test_state;
    state->waits++;
    if ( milliseconds > state->longest_wait_milliseconds )
    {
        state->longest_wait_milliseconds = milliseconds;
    }
    if ( state->late_waits == 0 )
    {
        return;
    }

    const int share = state->late_receives / state->late_waits;
    state->late_receives -= share;
    state->cancelled_receives += share;
    state->late_waits--;
}

// proofs/tla/RNRioWorker.tla WClose under DrainSendsBeforeClose, stepped on one thread
// with a fake kernel and no socket, worker thread or clock: the stop pass
// posts and commits the staged fill phase, keeps the socket open while the
// committed sends are in flight, and closes it only after every send
// completed, so the stop burst retires delivered and none is cancelled.
bool rocketnet_server_rio_test_stop_drains_sends()
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }
    static rocketnet_rio_stop_test_state_t state;
    memset( &state, 0, sizeof( state ) );

    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_stop_test_dequeue,
        rocketnet_rio_stop_test_receive,
        rocketnet_rio_stop_test_send,
        rocketnet_rio_stop_test_close
    };
    rocketnet_rio_stop_test_state = &state;
    rocketnet_rio_test_functions = &functions;
    rio->have_table = true;
    rio->socket = (SOCKET) 7;

    const int burst = 3;
    bool valid = rocketnet_rio_test_stage( rio, burst );
    rocketnet_rio_lane_stop( rio );

    // The pass posts and commits the burst; the sends are in flight, so the
    // socket stays open, on this pass and the next.

    valid &= !rocketnet_rio_stop_pass( rio ) && state.in_flight == burst && rocketnet_rio_outstanding_sends( rio ) == burst;
    valid &= !rocketnet_rio_stop_pass( rio ) && state.closes == 0 && rio->socket != INVALID_SOCKET;

    // The sends complete. The next pass retires them, closes the socket and
    // drains every cancelled receive in that same pass, not one 256 completion
    // batch per 1 ms wait.

    rocketnet_rio_stop_test_complete_in_flight( 0 );
    const bool stopped = rocketnet_rio_stop_pass( rio );
    valid &= stopped && state.closes == 1 && rio->socket == INVALID_SOCKET;
    valid &= rio->counters[ROCKETNET_COUNTER_RIO_PACKETS_SENT - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == (uint64_t) burst &&
             rio->counters[ROCKETNET_COUNTER_RIO_SEND_FAILED - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 0 &&
             rocketnet_rio_outstanding_sends( rio ) == 0 && rio->posted_receives.load() == 0;

    rio->have_table = false;
    rocketnet_rio_stop_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

// proofs/tla/RNRioWorker.tla InlineStop with BoundedStopDrain: create's
// failure drain on one thread with a fake kernel and no clock, every receive
// slice posted and no worker. late_waits is how many waits the cancelled
// receives take to reach the completion queue (0: closesocket queues them
// all); refuse_close refuses closesocket every time. Reports whether the
// drain stopped, the closesocket calls (one a pass until it succeeds), the
// waits between passes and the longest bound any of them was given.
bool rocketnet_server_rio_test_create_failure_drain( int late_waits, bool refuse_close, bool * stopped_out, int * closes_out,
                                                     int * waits_out, int * longest_wait_milliseconds_out )
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }
    static rocketnet_rio_stop_test_state_t state;
    memset( &state, 0, sizeof( state ) );
    state.late_waits = late_waits;
    state.refuse_close = refuse_close;

    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_stop_test_dequeue,
        rocketnet_rio_stop_test_receive,
        rocketnet_rio_stop_test_send,
        rocketnet_rio_stop_test_close
    };
    rocketnet_rio_stop_test_state = &state;
    rocketnet_rio_test_functions = &functions;
    rocketnet_rio_test_drain_wait = rocketnet_rio_stop_test_drain_wait;
    rio->have_table = true;
    rio->socket = (SOCKET) 7;

    *stopped_out = rocketnet_rio_create_failed_drain( rio );
    *closes_out = state.closes;
    *waits_out = state.waits;
    *longest_wait_milliseconds_out = (int) state.longest_wait_milliseconds;

    // a drain that stopped left nothing kernel owned; one that gave up still
    // counts what it could not account for
    const bool valid = *stopped_out == ( rio->socket == INVALID_SOCKET && rio->posted_receives.load() == 0 );

    rio->have_table = false;
    rocketnet_rio_test_drain_wait = NULL;
    rocketnet_rio_stop_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

// create's failure drain when nothing was ever posted (a failure before the
// receives) and closesocket refuses every time: the kernel owns nothing on
// the region, so the drain stops after its bounded passes, abandons only the
// socket handle (counted) and the lane can be freed. Reports whether it
// stopped, the closesocket calls and the abandoned sockets it added.
bool rocketnet_server_rio_test_create_failure_unposted( bool * stopped_out, int * closes_out, uint64_t * abandoned_out )
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }
    static rocketnet_rio_stop_test_state_t state;
    memset( &state, 0, sizeof( state ) );
    state.refuse_close = true;

    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_stop_test_dequeue,
        rocketnet_rio_stop_test_receive,
        rocketnet_rio_stop_test_send,
        rocketnet_rio_stop_test_close
    };
    rocketnet_rio_stop_test_state = &state;
    rocketnet_rio_test_functions = &functions;
    rocketnet_rio_test_drain_wait = rocketnet_rio_stop_test_drain_wait;
    rio->have_table = true;
    rio->socket = (SOCKET) 7;
    rio->posted_receives.store( 0, std::memory_order_relaxed );

    const uint64_t before = rocketnet_rio_create_abandoned_sockets.load();
    const uint64_t leaks_before = rocketnet_rio_create_leaks.load();
    *stopped_out = rocketnet_rio_create_failed_drain( rio );
    *closes_out = state.closes;
    *abandoned_out = rocketnet_rio_create_abandoned_sockets.load() - before;
    const bool valid = rio->socket == INVALID_SOCKET && rocketnet_rio_create_leaks.load() == leaks_before;

    rio->have_table = false;
    rocketnet_rio_test_drain_wait = NULL;
    rocketnet_rio_stop_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

// The stop on one thread with a fake kernel: closesocket completes three
// posted receives with a datagram and cancels the rest, three datagrams wait
// in the fill bank and two in a ready bank main never took. The stop pass
// counts the three completed and the worker's exit the five banked, all as
// RECEIVE_DROP_STOPPED and nothing else; a second exit counts nothing more.
bool rocketnet_server_rio_test_stop_counts_stranded( uint64_t * stopped_out, void ( *driver_totals )( void * transport, uint64_t * values ), uint64_t * driver_stopped_out )
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }
    static rocketnet_rio_stop_test_state_t state;
    memset( &state, 0, sizeof( state ) );
    state.arrive_at_close = 3;

    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_stop_test_dequeue,
        rocketnet_rio_stop_test_receive,
        rocketnet_rio_stop_test_send,
        rocketnet_rio_stop_test_close
    };
    rocketnet_rio_stop_test_state = &state;
    rocketnet_rio_test_functions = &functions;
    rio->have_table = true;
    rio->socket = (SOCKET) 7;

    const int ready = rio->fill_bank == 0 ? 1 : 0;
    rio->banks[rio->fill_bank].num_packets = 3;
    rio->banks[ready].num_packets = 2;
    rio->ready_banks[rio->ready_head] = ready;
    rio->ready_count = 1;

    rocketnet_rio_lane_stop( rio );
    bool valid = rocketnet_rio_stop_pass( rio );
    valid &= rio->drops->values[ROCKETNET_RECEIVE_DROP_STOPPED].load() == 3;
    rocketnet_rio_drop_unread_banks( rio );
    rocketnet_rio_drop_unread_banks( rio );
    *stopped_out = rio->drops->values[ROCKETNET_RECEIVE_DROP_STOPPED].load();
    for ( int i = 0; i < ROCKETNET_RECEIVE_NUM_DROPS; ++i )
    {
        valid &= i == ROCKETNET_RECEIVE_DROP_STOPPED || rio->drops->values[i].load() == 0;
    }

    // the driver's receive_drop_totals (the path the server reads the tally
    // by) over a transport of this one lane
    if ( driver_totals )
    {
        rocketnet_server_rio_t outer = {};
        outer.num_lanes = 1;
        outer.lanes[0] = rio;
        uint64_t values[ROCKETNET_RECEIVE_NUM_DROPS] = {};
        driver_totals( &outer, values );
        *driver_stopped_out = values[ROCKETNET_RECEIVE_DROP_STOPPED];
    }
    valid &= rio->posted_receives.load() == 0;

    rio->ready_count = 0;
    rio->have_table = false;
    rocketnet_rio_stop_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

static void rocketnet_rio_lane_counters( rocketnet_rio_lane_t * rio, uint64_t * counters );

// The lane worker's pin, with no thread and no socket: a lane that asks for no
// placement counts no refusal; one whose pin the platform refuses (the seam)
// counts one, reads REFUSED with the Win32 error, and the count reaches the
// public row through the transport's counters.
bool rocketnet_server_rio_test_pin_refusal( uint64_t * not_asked_out, uint64_t * refused_out, int * placement_out )
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }

    uint64_t counters[ROCKETNET_NUM_COUNTERS] = {};
    rio->processor = -1;
    rocketnet_rio_place_worker( rio );
    rocketnet_rio_lane_counters( rio, counters );
    *not_asked_out = counters[ROCKETNET_COUNTER_RIO_PIN_REFUSALS];
    bool valid = rio->placement.load() == ROCKETNET_RIO_PLACEMENT_NOT_ASKED;

    rio->processor = 0;
    rio->pin.Mask = 1;
    rio->pin.Group = 0;
    rocketnet_rio_test_refuse_pin = true;
    rocketnet_rio_place_worker( rio );
    rocketnet_rio_test_refuse_pin = false;
    rocketnet_rio_lane_counters( rio, counters );
    *refused_out = counters[ROCKETNET_COUNTER_RIO_PIN_REFUSALS];
    *placement_out = rio->placement.load();
    valid &= rio->placement_error.load() == (unsigned long) ERROR_ACCESS_DENIED;

    rocketnet_rio_test_fixture_free( rio );
    return valid;
}

// A receive completion queue that reports RIO_CORRUPT_CQ at stop: the lane
// never reports stopped (its requests cannot be accounted for), and the
// corruption is named once and counted once however many passes see it.
bool rocketnet_server_rio_test_corrupt_queue( bool * stopped_out, uint64_t * corrupt_out )
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }
    static rocketnet_rio_stop_test_state_t state;
    memset( &state, 0, sizeof( state ) );
    state.corrupt_receive = true;

    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_stop_test_dequeue,
        rocketnet_rio_stop_test_receive,
        rocketnet_rio_stop_test_send,
        rocketnet_rio_stop_test_close
    };
    rocketnet_rio_stop_test_state = &state;
    rocketnet_rio_test_functions = &functions;
    rio->have_table = true;
    rio->socket = (SOCKET) 7;

    const uint64_t before = rocketnet_rio_corrupt_queues.load();
    rocketnet_rio_lane_stop( rio );
    bool stopped = false;
    for ( int pass = 0; pass < 3; ++pass )
    {
        stopped |= rocketnet_rio_stop_pass( rio );
    }
    *stopped_out = stopped;
    *corrupt_out = rocketnet_rio_corrupt_queues.load() - before;
    const bool valid = rio->socket == INVALID_SOCKET;

    // the fixture is the test's: its queue is not the kernel's, so it is freed
    rio->posted_receives.store( 0 );
    rio->have_table = false;
    rocketnet_rio_stop_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

static int rocketnet_rio_accounting_test_asserts = 0;

static void rocketnet_rio_accounting_test_assert( const char *, const char *, const char *, int )
{
    rocketnet_rio_accounting_test_asserts++;
}

// Send accounting on one thread with a fake kernel: each lost packet counts
// SEND_FAILED once and each sent packet PACKETS_SENT once. A finish with a
// length out of range loses its packet and counts it; refused commits count
// nothing, since the deferred sends they leave complete later.
bool rocketnet_server_rio_test_send_accounting()
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }
    static rocketnet_rio_handoff_test_state_t state;
    memset( &state, 0, sizeof( state ) );

    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_handoff_test_dequeue,
        rocketnet_rio_handoff_test_receive,
        rocketnet_rio_handoff_test_send,
        NULL
    };
    rocketnet_rio_handoff_test_state = &state;
    rocketnet_rio_test_functions = &functions;
    state.receive_paused = true;

    bool valid = true;

    rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40001;

    // Two finishes out of range, below one byte and past the ceiling: each
    // packet is lost and counted, and the post phase skips its slot.

    rocketnet_assert_function_t saved_assert = rn_assert_function;
    rn_assert_function = rocketnet_rio_accounting_test_assert;
    const int lengths[2] = { 0, ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int i = 0; i < 2; ++i )
    {
        uint64_t id = 0;
        uint8_t * frame = rocketnet_rio_lane_start_packet( rio, &to, &id );
        valid &= frame != NULL;
        if ( frame )
        {
            rocketnet_rio_lane_finish_packet( rio, id, frame, lengths[i] );
        }
        valid &= rio->counters[ROCKETNET_COUNTER_RIO_SEND_FAILED - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == (uint64_t) ( i + 1 );
    }
    rn_assert_function = saved_assert;

    // Three good packets beside them, and a commit refused on the next four
    // tries. The phase posts three sends and the worker retries the commit
    // until it is taken; nothing is counted until the sends complete.

    valid &= rocketnet_rio_test_stage( rio, 3 );
    rocketnet_rio_lane_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, 4, WSAENOBUFS );
    rocketnet_rio_lane_send_packets( rio );

    for ( int pass = 0; pass < 8 && rocketnet_rio_lane_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT ) > 0; ++pass )
    {
        rocketnet_rio_worker_pass( rio );
        valid &= rio->uncommitted && state.commits == 0;
        valid &= rio->counters[ROCKETNET_COUNTER_RIO_SEND_FAILED - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 2;
    }

    valid &= rocketnet_rio_lane_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT ) == 0;
    for ( int pass = 0; pass < 4 && rocketnet_rio_outstanding_sends( rio ) > 0; ++pass )
    {
        rocketnet_rio_worker_pass( rio );
    }

    valid &= state.commits == 1 && !rio->uncommitted && rocketnet_rio_outstanding_sends( rio ) == 0;
    valid &= rio->counters[ROCKETNET_COUNTER_RIO_COMMITS - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 1;
    valid &= rio->counters[ROCKETNET_COUNTER_RIO_PACKETS_SENT - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 3;
    valid &= rio->counters[ROCKETNET_COUNTER_RIO_SEND_FAILED - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 2;
    valid &= rocketnet_rio_accounting_test_asserts == ( ROCKETNET_ASSERTS ? 2 : 0 );    // each refused finish asserts where asserts are compiled in

    rocketnet_rio_handoff_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

#endif // #if ROCKETNET_DEV

// proofs/tla/RNRioWorker.tla ReceivePackets: the views of the exposed bank
static rocketnet_server_socket_process_packets_t * rocketnet_rio_lane_process_packets( rocketnet_rio_lane_t * rio )
{
    return &rio->process;
}

static void rocketnet_rio_lane_counters( rocketnet_rio_lane_t * rio, uint64_t * counters )
{
    for ( int i = 0; i < ROCKETNET_SERVER_RIO_COUNTER_ROWS; ++i )
    {
        counters[ROCKETNET_SERVER_RIO_FIRST_COUNTER + i] = rio->counters[i].load( std::memory_order_relaxed );
    }
    counters[ROCKETNET_COUNTER_RIO_PIN_REFUSALS] = rio->pin_refusals.load( std::memory_order_relaxed );
}

static int rocketnet_rio_lane_pending( rocketnet_rio_lane_t * rio )
{
    return rocketnet_rio_outstanding_sends( rio ) + rio->posted_receives.load( std::memory_order_relaxed );
}

#if ROCKETNET_DEV
static uintptr_t rocketnet_rio_lane_test_socket( rocketnet_rio_lane_t * rio )
{
    return (uintptr_t) rio->socket;
}

static void rocketnet_rio_lane_test_fail_next( rocketnet_rio_lane_t * rio, int fault, int count, int status )
{
    rocketnet_assert( fault >= 0 && fault < ROCKETNET_RIO_TEST_NUM_FAULTS && count >= 0 );
    rio->test_fault_status[fault].store( status, std::memory_order_relaxed );
    rio->test_faults[fault].store( count, std::memory_order_release );
}

static int rocketnet_rio_lane_test_faults_left( rocketnet_rio_lane_t * rio, int fault )
{
    return rio->test_faults[fault].load( std::memory_order_acquire );
}

static void rocketnet_rio_lane_test_hold_send( rocketnet_rio_lane_t * rio, bool hold )
{
    rocketnet_platform_mutex_acquire( &rio->mutex );
    rio->test_hold_send = hold;
    rocketnet_platform_mutex_release( &rio->mutex );
    if ( !hold )
    {
        SetEvent( rio->wake );
    }
}

static int rocketnet_rio_lane_test_handed_phases( rocketnet_rio_lane_t * rio )
{
    rocketnet_platform_mutex_acquire( &rio->mutex );

    const int count = rio->send_ready_count;
    rocketnet_platform_mutex_release( &rio->mutex );
    return count;
}

static bool rocketnet_rio_lane_test_send_idle( rocketnet_rio_lane_t * rio )
{
    rocketnet_platform_mutex_acquire( &rio->mutex );

    const int phase = rio->fill.load( std::memory_order_relaxed );
    bool idle = rio->send_ready_count == 0 && rio->next[phase].load( std::memory_order_relaxed ) == 0;
    for ( int p = 0; p < ROCKETNET_RIO_SEND_PHASES; ++p )
    {
        idle = idle && rio->phase_state[p] != ROCKETNET_RIO_PHASE_POSTED;
    }
    rocketnet_platform_mutex_release( &rio->mutex );

    return idle;
}

static int rocketnet_rio_lane_test_retry_receives( rocketnet_rio_lane_t * rio )
{
    return rio->num_retry.load( std::memory_order_relaxed );
}
// A fill phase that has taken 2^32 - 1 reservations without a flush refuses
// the next two as full: the reservation count does not wrap back onto a slot
// it handed out.
bool rocketnet_server_rio_test_reservation_wrap()
{
    rocketnet_rio_lane_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
    {
        return false;
    }

    rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40001;

    typedef decltype( rio->next[0].load() ) rocketnet_rio_test_count_t;
    const int phase = rio->fill.load( std::memory_order_relaxed );
    bool valid = rocketnet_rio_lane_send_room( rio ) == rio->send_queue_size;
    rio->next[phase].store( (uint64_t) rio->send_queue_size - 1, std::memory_order_relaxed );
    valid &= rocketnet_rio_lane_send_room( rio ) == 1;
    rio->next[phase].store( (uint64_t) rio->send_queue_size, std::memory_order_relaxed );
    valid &= rocketnet_rio_lane_send_room( rio ) == 0;
    rio->next[phase].store( (rocketnet_rio_test_count_t) 0xFFFFFFFFull, std::memory_order_relaxed );
    valid &= rocketnet_rio_lane_send_room( rio ) == 0;

    uint64_t id = 0;
    valid &= rocketnet_rio_lane_start_packet( rio, &to, &id ) == NULL;
    valid &= rocketnet_rio_lane_send_room( rio ) == 0;
    valid &= rocketnet_rio_lane_start_packet( rio, &to, &id ) == NULL;
    valid &= rocketnet_rio_lane_send_room( rio ) == 0;
    valid &= rio->counters[ROCKETNET_COUNTER_RIO_SEND_FULL - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load() == 2;

    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

#endif // #if ROCKETNET_DEV

static uint64_t rocketnet_rio_outer_bytes()
{
    return sizeof( rocketnet_server_rio_t );
}

// The unspecified address, 0.0.0.0 or ::.
static bool rocketnet_rio_address_is_wildcard( const rocketnet_address_t * address )
{
    if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        return address->data.ip == 0;
    }
    for ( int i = 0; i < 8; ++i )
    {
        if ( address->data.ipv6[i] != 0 )
        {
            return false;
        }
    }
    return true;
}

// Binds a plain socket to the address with SO_EXCLUSIVEADDRUSE and closes
// it: refused when anything already holds the address and port, so lanes
// that share the port by SIO_CPU_AFFINITY never join a socket someone else
// bound first. Never called for port 0.
static bool rocketnet_rio_exclusive_probe( const rocketnet_address_t * bind_address, int * status )
{
    const bool ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    SOCKADDR_INET address;
    if ( !rocketnet_rio_address_write( bind_address, ipv6, &address ) )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_INVALID_ADDRESS );
        rocketnet_warn( "rio transport refused: the address for the exclusive probe could not be written as a socket address" );
        return false;
    }
    const int address_bytes = ipv6 ? (int) sizeof( sockaddr_in6 ) : (int) sizeof( sockaddr_in );
    SOCKET probe = WSASocketW( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT );
    if ( probe == INVALID_SOCKET )
    {
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
        return false;
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const uint64_t generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) probe, "rio exclusive probe socket" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    BOOL enabled = TRUE;
    DWORD v6_only = FALSE;
    const char * failed = NULL;
    if ( setsockopt( probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
    {
        failed = "SO_EXCLUSIVEADDRUSE was refused";
    }
    else if ( ipv6 && setsockopt( probe, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &v6_only, sizeof( v6_only ) ) == SOCKET_ERROR )
    {
        failed = "IPV6_V6ONLY was refused";
    }
    else if ( bind( probe, (sockaddr *) &address, address_bytes ) == SOCKET_ERROR )
    {
        failed = "the bind was refused: another socket holds the address and port";
    }
    const bool bound = failed == NULL;
    if ( !bound )
    {
        const int error = WSAGetLastError();
        rocketnet_create_status( status, ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
        rocketnet_platform_windows_create_status( status, (unsigned long) error );
        rocketnet_warn( "rio transport refused: the exclusive probe on port %d failed (Winsock %d): %s", bind_address->port, error, failed );
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) probe, generation ) )
    {
        const int closed = closesocket( probe );
        rocketnet_lifecycle_track_close_end( generation, closed == 0 );
    }
#else
    closesocket( probe );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return bound;
}

// The processor of each lane: the first logical processor of each physical
// core in processor-index order, then the cores' other logical processors in
// order, wrapping past the last (two sockets may share a processor). Windows'
// default RSS layout (base processor 0, one processor per core: on the ENA
// adapter Get-NetAdapterRss lists 0, 2, 4, ...) hands receive queue i to the
// first processor of core i, so with as many lanes as queues each queue's
// flows reach the lane on its processor. A flow whose RSS processor has no
// lane is delivered to the first lane bound (measured), so a different RSS
// layout costs spread, never datagrams. The processor index SIO_CPU_AFFINITY
// takes is the group's offset (the active processors of the groups before it)
// plus the processor's number in its group. False only when Windows cannot
// list its processors.
static bool rocketnet_rio_lane_processors( void * context, int lanes, rocketnet_rio_processor_t * out, int * status )
{
    DWORD bytes = 0;
    if ( GetLogicalProcessorInformationEx( RelationProcessorCore, NULL, &bytes ) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 )
    {
        rocketnet_platform_windows_create_status( status, GetLastError() );
        rocketnet_error( "rio transport refused: the processors could not be listed for the lanes (Win32 %lu)", GetLastError() );
        return false;
    }
    const DWORD logical = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
    uint8_t * info = (uint8_t *) rocketnet_malloc( context, (size_t) bytes );
    rocketnet_rio_processor_t * order = (rocketnet_rio_processor_t *) rocketnet_malloc( context, sizeof( rocketnet_rio_processor_t ) * ( (size_t) logical + 1 ) );
    if ( !info || !order )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_free( context, order );
        rocketnet_free( context, info );
        return false;
    }
    DWORD error = 0;
    bool ok = logical > 0;
    if ( ok && !GetLogicalProcessorInformationEx( RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) info, &bytes ) )
    {
        ok = false;
        error = GetLastError();
    }
    int count = 0;
    // pass 0 takes each core's first logical processor, pass 1 the others
    for ( int pass = 0; ok && pass < 2; ++pass )
    {
        const int first = count;
        for ( DWORD offset = 0; offset < bytes; )
        {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX * entry = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ( info + offset );
            if ( entry->Size == 0 )
            {
                break;
            }
            offset += entry->Size;
            if ( entry->Relationship != RelationProcessorCore || entry->Processor.GroupCount == 0 )
            {
                continue;
            }
            const GROUP_AFFINITY * mask = &entry->Processor.GroupMask[0];
            int group_offset = 0;
            for ( WORD g = 0; g < mask->Group; ++g )
            {
                group_offset += (int) GetActiveProcessorCount( g );
            }
            bool seen_first = false;
            for ( int bit = 0; bit < (int) ( sizeof( KAFFINITY ) * 8 ); ++bit )
            {
                if ( !( mask->Mask & ( (KAFFINITY) 1 << bit ) ) )
                {
                    continue;
                }
                const bool take = pass == 0 ? !seen_first : seen_first;
                seen_first = true;
                if ( !take || count >= (int) logical )
                {
                    continue;
                }
                rocketnet_rio_processor_t * p = &order[count++];
                p->index = group_offset + bit;
                memset( &p->affinity, 0, sizeof( p->affinity ) );
                p->affinity.Group = mask->Group;
                p->affinity.Mask = (KAFFINITY) 1 << bit;
                memset( &p->core, 0, sizeof( p->core ) );
                p->core.Group = mask->Group;
                p->core.Mask = mask->Mask;
                if ( pass == 0 )
                {
                    break;
                }
            }
        }
        // each pass in processor-index order
        for ( int i = first + 1; i < count; ++i )
        {
            const rocketnet_rio_processor_t key = order[i];
            int j = i - 1;
            while ( j >= first && order[j].index > key.index )
            {
                order[j + 1] = order[j];
                --j;
            }
            order[j + 1] = key;
        }
    }
    const bool listed = ok;
    ok = ok && count > 0;
    for ( int i = 0; ok && i < lanes; ++i )
    {
        out[i] = order[i % count];
    }
    if ( ok && lanes > count )
    {
        // a fallback: named at warn, and each lane's processor is readable
        // after start (rocketnet_server_rio_lane_placement)
        rocketnet_warn( "rio transport: %d lanes asked for %d logical processors; lanes %d..%d share processors with lanes 0..%d", lanes, count, count,
                        lanes - 1, lanes - 1 - count );
    }
    if ( !ok )
    {
        if ( error != 0 )
        {
            rocketnet_platform_windows_create_status( status, error );
            rocketnet_error( "rio transport refused: the processors could not be listed for the lanes (Win32 %lu)", error );
        }
        else
        {
            // no API failed: the system reported no active processor, or no core with a usable processor
            rocketnet_error( "rio transport refused: no processor to place the lanes on (%lu active, %d cores listed%s)", (unsigned long) logical, count,
                             listed ? "" : ", none read" );
        }
    }
    rocketnet_free( context, order );
    rocketnet_free( context, info );
    return ok;
}

#if ROCKETNET_DEV
static std::atomic<uint64_t> rocketnet_rio_lanes_made{ 0 };
static std::atomic<uint64_t> rocketnet_rio_transports_made{ 0 };

uint64_t rocketnet_server_rio_test_lanes_made()
{
    return rocketnet_rio_lanes_made.load( std::memory_order_relaxed );
}

uint64_t rocketnet_server_rio_test_transports_made()
{
    return rocketnet_rio_transports_made.load( std::memory_order_relaxed );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
// development library only: reads a test layout "R0,R1,../W0,W1,..", processor
// numbers in group 0, into rss and workers (each up to max). False when a
// character is not a digit, comma or the one slash, or a number is past the
// group's processors.
static bool rocketnet_windows_test_layout_read( const char * text, int * rss, int * num_rss, int * workers, int * num_workers, int max )
{
    const int group_processors = (int) GetActiveProcessorCount( 0 );
    *num_rss = 0;
    *num_workers = 0;
    bool after_slash = false;
    const char * p = text;
    while ( *p )
    {
        if ( *p == '/' && !after_slash )
        {
            after_slash = true;
            ++p;
            continue;
        }
        if ( *p == ',' )
        {
            ++p;
            continue;
        }
        if ( *p < '0' || *p > '9' )
        {
            return false;
        }
        int value = 0;
        while ( *p >= '0' && *p <= '9' )
        {
            if ( value > 1000 )
            {
                return false;
            }
            value = value * 10 + ( *p - '0' );
            ++p;
        }
        if ( value >= group_processors || value >= (int) ( sizeof( KAFFINITY ) * 8 ) )
        {
            return false;
        }
        if ( !after_slash && *num_rss < max )
        {
            rss[( *num_rss )++] = value;
        }
        else if ( after_slash && *num_workers < max )
        {
            workers[( *num_workers )++] = value;
        }
    }
    return true;
}
#endif // #if ROCKETNET_DEV

static void rocketnet_rio_free( rocketnet_server_rio_t * rio )
{
    void * context = rio->context;
    rio->~rocketnet_server_rio_t();
    rocketnet_free( context, rio );
}

rocketnet_server_rio_t * rocketnet_server_rio_create( void * context, rocketnet_address_t * bind_address, int max_clients
#if ROCKETNET_DEV
                                                      ,
                                                      int fail_stage
#endif // #if ROCKETNET_DEV
                                                      ,
                                                      int * status, const rocketnet_server_rio_options_t * options )
{
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) )
    {
        return NULL;
    }
    int requested = options ? options->lanes : 0;
#if ROCKETNET_DEV
    // development library only: ROCKETNET_TEST_RIO_LANES=N (2 .. 32) runs every
    // RIO server that leaves the lane count to the transport (no options, or 0
    // lanes: num_xdp_queues unset) on N lanes, for a whole process, so a suite
    // whose servers come from many paths runs on lanes. One lane asked for
    // explicitly stays one. A wildcard bind keeps its one lane: lanes refuse
    // the wildcard, and that refusal has its own test.
    if ( requested == 0 && !rocketnet_rio_address_is_wildcard( bind_address ) )
    {
        char test_lanes[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
        if ( rocketnet_platform_getenv( "ROCKETNET_TEST_RIO_LANES", test_lanes, sizeof( test_lanes ) ) > 0 )
        {
            const int n = atoi( test_lanes );
            if ( n >= 2 && n <= ROCKETNET_RIO_MAX_LANES )
            {
                requested = n;
            }
        }
    }
#endif // #if ROCKETNET_DEV
    int lanes = rocketnet_server_rio_lane_count( requested );
    if ( !lanes )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
        rocketnet_warn( "rio transport refused: %d lanes is outside 1..%d", requested, ROCKETNET_RIO_MAX_LANES );
        return NULL;
    }
#if ROCKETNET_DEV
    // development library only: ROCKETNET_TEST_RIO_LANES_PLANT set makes one
    // lane whatever was asked, the plant control of a hand run on four lanes;
    // every check of the lanes actually made must then fail
    {
        char plant[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
        if ( rocketnet_platform_getenv( "ROCKETNET_TEST_RIO_LANES_PLANT", plant, sizeof( plant ) ) > 0 )
        {
            lanes = 1;
        }
    }
#endif // #if ROCKETNET_DEV
    // More than one lane shares the port by SIO_CPU_AFFINITY, which has no
    // exclusive form, so the lanes take a specific local address: a squatter
    // on that exact address and port is refused by the probe below, and a
    // later plain bind of the wildcard at that port (which Windows allows,
    // measured) is less specific than the lanes. One lane binds exclusively
    // and takes either.
    if ( lanes > 1 && rocketnet_rio_address_is_wildcard( bind_address ) )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_INVALID_ADDRESS );
        rocketnet_warn( "rio transport refused: %d lanes need a specific local address; set bind_ipv4_address or bind_ipv6_address in the server config", lanes );
        return NULL;
    }
    // An IPv4-mapped address (::ffff:a.b.c.d, ::ffff:0.0.0.0 among them)
    // gives lane 0 a dual-stack socket whose bound name comes back as IPv4,
    // so the other lanes would bind IPv4 sockets beside it, on the IPv4
    // wildcard for ::ffff:0.0.0.0. The lanes take the IPv4 address itself.
    rocketnet_address_t unmapped = *bind_address;
    if ( lanes > 1 && unmapped.type == ROCKETNET_ADDRESS_IPV6 && rocketnet_address_is_ipv4_in_ipv6( &unmapped ) )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_INVALID_ADDRESS );
        rocketnet_warn( "rio transport refused: %d lanes need a specific local address, not an IPv4-mapped one; set bind_ipv4_address in the server config", lanes );
        return NULL;
    }
    const uint8_t * packet_filter = options ? options->packet_filter : NULL;
    const rocketnet_address_t asked_address = *bind_address;
    void * storage = rocketnet_malloc( context, sizeof( rocketnet_server_rio_t ) );
    if ( !storage )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    rocketnet_server_rio_t * rio = new ( storage ) rocketnet_server_rio_t();
    rio->context = context;

    // An explicit port is probed. Port 0 is not: lane 0 binds it with
    // SIO_CPU_AFFINITY and Windows hands it a port no socket holds
    // (test_rio_transport_lanes_port_zero_never_held checks it on every run),
    // so there is no gap between a probe and the lanes; lane 0 writes the port
    // it was given to bind_address and the other lanes join. Once lane 0 is
    // bound, a later plain or SO_REUSEADDR bind of the lanes' address and port
    // is refused (measured, 10048 and 10013), but a socket of another process
    // that also sets SIO_CPU_AFFINITY can join the port and receive the flows
    // RSS hands its processor: Windows offers no exclusive form of a port
    // shared by processor (SO_EXCLUSIVEADDRUSE refuses the second lane).
    if ( lanes > 1 && bind_address->port != 0 && !rocketnet_rio_exclusive_probe( bind_address, status ) )
    {
        rocketnet_rio_free( rio );
        return NULL;
    }
    rocketnet_rio_processor_t processors[ROCKETNET_RIO_MAX_LANES];
    if ( lanes > 1 && !rocketnet_rio_lane_processors( context, lanes, processors, status ) )
    {
        rocketnet_rio_free( rio );
        return NULL;
    }
#if ROCKETNET_DEV
    // development library only: ROCKETNET_TEST_RIO_LAYOUT="R0,R1,../W0,W1,.."
    // places lane i's socket on processor Ri (SIO_CPU_AFFINITY: the processor
    // RSS hands its flows) and its worker on processor Wi alone, processor
    // group 0, for a measurement in the layout a game runs: the NIC's receive
    // processing on cores the operator chose (Set-NetAdapterRss), the network
    // threads on cores of their own. A list shorter than the lanes, or a
    // processor past group 0's, leaves the default placement and says so.
    {
        char layout[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
        if ( lanes > 1 && rocketnet_platform_getenv( "ROCKETNET_TEST_RIO_LAYOUT", layout, sizeof( layout ) ) > 0 )
        {
            int rss[ROCKETNET_RIO_MAX_LANES];
            int workers[ROCKETNET_RIO_MAX_LANES];
            int num_rss = 0;
            int num_workers = 0;
            if ( !rocketnet_windows_test_layout_read( layout, rss, &num_rss, workers, &num_workers, ROCKETNET_RIO_MAX_LANES ) || num_rss < lanes ||
                 num_workers < lanes )
            {
                rocketnet_warn( "rio transport: ROCKETNET_TEST_RIO_LAYOUT \"%s\" does not place %d lanes in group 0; default placement", layout, lanes );
            }
            else
            {
                for ( int i = 0; i < lanes; ++i )
                {
                    processors[i].index = rss[i];
                    memset( &processors[i].affinity, 0, sizeof( processors[i].affinity ) );
                    processors[i].affinity.Group = 0;
                    processors[i].affinity.Mask = (KAFFINITY) 1 << rss[i];
                    // the lane's pin is core less affinity: the worker's processor alone
                    memset( &processors[i].core, 0, sizeof( processors[i].core ) );
                    processors[i].core.Group = 0;
                    processors[i].core.Mask = processors[i].affinity.Mask | ( (KAFFINITY) 1 << workers[i] );
                }
            }
        }
    }
#endif // #if ROCKETNET_DEV

    for ( int i = 0; i < lanes; ++i )
    {
        rio->lanes[i] = rocketnet_rio_lane_create( context, bind_address, max_clients, lanes > 1 ? &processors[i] : NULL, packet_filter
#if ROCKETNET_DEV
                                                   ,
                                                   ( fail_stage >> 4 ) == i ? ( fail_stage & 15 ) : 0
#endif // #if ROCKETNET_DEV
                                                   ,
                                                   status );
        if ( !rio->lanes[i] )
        {
            // the lanes already made stop and drain before they are freed;
            // a lane whose own create failed has drained or leaked itself.
            // A made lane stops on its worker's path
            // (proofs/tla/RNRioWorker.tla StopCompletesUnlessRefused): it
            // finishes unless the kernel refuses every later close, so the
            // wait here is bounded as server destroy's is
            // (ROCKETNET_RIO_ROLLBACK_POLLS polls of a millisecond); a lane
            // still running then is leaked whole, named and counted, never
            // freed under its worker.
            for ( int j = 0; j < i; ++j )
            {
#if ROCKETNET_DEV
                if ( rocketnet_rio_test_hold_rollback )
                {
                    rocketnet_rio_lane_test_hold_send( rio->lanes[j], true );
                }
#endif // #if ROCKETNET_DEV
                rocketnet_rio_lane_stop( rio->lanes[j] );
            }
            for ( int j = 0; j < i; ++j )
            {
                int polls = 0;
                while ( !rocketnet_rio_lane_stopped( rio->lanes[j] ) && polls < ROCKETNET_RIO_ROLLBACK_POLLS )
                {
                    rocketnet_platform_sleep( 0.001 );
                    ++polls;
                }
                if ( rocketnet_rio_lane_stopped( rio->lanes[j] ) )
                {
                    rocketnet_rio_lane_destroy( rio->lanes[j] );
                    continue;
                }
                const uint64_t leaks = rocketnet_rio_create_leaks.fetch_add( 1, std::memory_order_relaxed ) + 1;
                rocketnet_error( "rio transport: create failed at lane %d and lane %d did not stop within %d polls; leaking it whole, its worker still running "
                                 "(create leak %llu in this process)",
                                 i, j, ROCKETNET_RIO_ROLLBACK_POLLS, (unsigned long long) leaks );
#if ROCKETNET_DEV
                rocketnet_rio_test_rollback_leaked[rocketnet_rio_test_num_rollback_leaked++] = rio->lanes[j];
#endif // #if ROCKETNET_DEV
            }
            // lane 0 wrote the port it was given into bind_address: the next
            // transport of the ladder gets the address the caller asked for
            *bind_address = asked_address;
            rocketnet_rio_free( rio );
            return NULL;
        }
        rio->num_lanes = i + 1;
#if ROCKETNET_DEV
        rocketnet_rio_lanes_made.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    }

#if ROCKETNET_DEV
    rocketnet_rio_transports_made.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    rio->lane_slots = rio->lanes[0]->send_frames;
    for ( int i = 1; i < lanes; ++i )
    {
        rio->lanes[i]->view = &rio->lanes[0]->process;
    }

    rocketnet_info( "rio transport: port %d, %d lane%s (one socket and I/O worker each), %d receive slices and %d send frames per phase per lane, %llu registered bytes per lane%s",
                    bind_address->port, lanes, lanes > 1 ? "s" : "", rio->lanes[0]->receives, rio->lanes[0]->send_queue_size,
                    (unsigned long long) rio->lanes[0]->layout.total_bytes, packet_filter ? ", receive prefilter on" : "" );
    // each lane's placement as its worker found it: create waits (at most a
    // second per lane) for every worker to answer, and says what happened
    for ( int i = 0; lanes > 1 && i < lanes; ++i )
    {
        rocketnet_rio_lane_t * lane = rio->lanes[i];
        for ( int waits = 0; waits < 1000 && lane->placement.load( std::memory_order_acquire ) == ROCKETNET_RIO_PLACEMENT_PENDING; ++waits )
        {
            rocketnet_platform_sleep( 0.001 );
        }
        const int placement = lane->placement.load( std::memory_order_acquire );
        if ( placement == ROCKETNET_RIO_PLACEMENT_PINNED )
        {
            rocketnet_info( "rio transport: lane %d socket on processor %d (SIO_CPU_AFFINITY), worker pinned to mask 0x%llx in group %d", i, lane->processor,
                            (unsigned long long) lane->pin.Mask, (int) lane->pin.Group );
        }
        else if ( placement == ROCKETNET_RIO_PLACEMENT_REFUSED )
        {
            rocketnet_warn( "rio transport: lane %d socket on processor %d (SIO_CPU_AFFINITY), worker NOT pinned: SetThreadGroupAffinity to mask 0x%llx in group %d refused (Win32 %lu)",
                            i, lane->processor, (unsigned long long) lane->pin.Mask, (int) lane->pin.Group, lane->placement_error.load( std::memory_order_relaxed ) );
        }
        else if ( placement == ROCKETNET_RIO_PLACEMENT_NOT_ASKED )
        {
            rocketnet_info( "rio transport: lane %d socket on processor %d (SIO_CPU_AFFINITY), worker not pinned (not asked)", i, lane->processor );
        }
        else
        {
            rocketnet_warn( "rio transport: lane %d socket on processor %d (SIO_CPU_AFFINITY), worker placement not reported within a second", i, lane->processor );
        }
    }
    return rio;
}

void rocketnet_server_rio_stop( rocketnet_server_rio_t * rio )
{
    if ( !rio )
    {
        return;
    }
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        rocketnet_rio_lane_stop( rio->lanes[i] );
    }
}

// every lane drained
bool rocketnet_server_rio_stopped( rocketnet_server_rio_t * rio )
{
    if ( !rio )
    {
        return false;
    }
    bool stopped = true;
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        stopped = rocketnet_rio_lane_stopped( rio->lanes[i] ) && stopped;
    }
    return stopped;
}

void rocketnet_server_rio_destroy( rocketnet_server_rio_t * rio )
{
    if ( !rio )
    {
        return;
    }
    const bool stopped = rocketnet_server_rio_stopped( rio );
    rocketnet_assert( stopped );
    if ( !stopped )
    {
        return;
    }
    // what each lane received and where its receive path waited, so a run
    // that lost datagrams before the worker says which limit ran dry
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        const rocketnet_rio_lane_t * lane = rio->lanes[i];
        rocketnet_info( "rio transport: lane %d received %llu, %d receives posted, fill bank full on %llu passes, a whole lane of receives taken on %llu "
                        "passes, ready banks left for the next update %llu times",
                        i, (unsigned long long) lane->counters[ROCKETNET_COUNTER_RIO_PACKETS_RECEIVED - ROCKETNET_SERVER_RIO_FIRST_COUNTER].load(), lane->receives,
                        (unsigned long long) lane->waits[ROCKETNET_RIO_WAIT_RECEIVE_BANK].load(),
                        (unsigned long long) lane->waits[ROCKETNET_RIO_WAIT_RECEIVE_RAN_DRY].load(),
                        (unsigned long long) lane->waits[ROCKETNET_RIO_WAIT_RECEIVE_VIEW].load() );
    }
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        rocketnet_rio_lane_destroy( rio->lanes[i] );
    }
    rocketnet_rio_free( rio );
}

int rocketnet_server_rio_lanes( rocketnet_server_rio_t * rio )
{
    return rio->num_lanes;
}

int rocketnet_server_rio_lane_placement( rocketnet_server_rio_t * rio, int lane, uint64_t * mask, int * group, unsigned long * error )
{
    if ( !rio || lane < 0 || lane >= rio->num_lanes || !rio->lanes[lane] )
    {
        return ROCKETNET_RIO_PLACEMENT_PENDING;
    }
    const rocketnet_rio_lane_t * l = rio->lanes[lane];
    const int placement = l->placement.load( std::memory_order_acquire );
    if ( mask )
    {
        *mask = (uint64_t) l->pin.Mask;
    }
    if ( group )
    {
        *group = (int) l->pin.Group;
    }
    if ( error )
    {
        *error = l->placement_error.load( std::memory_order_relaxed );
    }
    return placement;
}

uint64_t rocketnet_server_rio_create_leaks()
{
    return rocketnet_rio_create_leaks.load( std::memory_order_relaxed );
}

uint64_t rocketnet_server_rio_create_abandoned_sockets()
{
    return rocketnet_rio_create_abandoned_sockets.load( std::memory_order_relaxed );
}

uint64_t rocketnet_server_rio_corrupt_queues()
{
    return rocketnet_rio_corrupt_queues.load( std::memory_order_relaxed );
}

#if ROCKETNET_DEV
// development library only: hold every made lane's sends in a failed create's
// rollback, so no made lane can stop and the bounded wait gives up
void rocketnet_server_rio_test_set_hold_rollback( bool hold )
{
    rocketnet_rio_test_hold_rollback = hold;
}

// development library only: lets the lane a held rollback leaked stop (its
// hold released), waits for each as long as the rollback would and frees it.
// True when there was at least one and every one was freed.
bool rocketnet_server_rio_test_finish_rollback_leak()
{
    const int num = rocketnet_rio_test_num_rollback_leaked;
    rocketnet_rio_test_num_rollback_leaked = 0;
    bool all = num > 0;
    for ( int i = 0; i < num; ++i )
    {
        rocketnet_rio_lane_t * lane = rocketnet_rio_test_rollback_leaked[i];
        rocketnet_rio_test_rollback_leaked[i] = NULL;
        rocketnet_rio_lane_test_hold_send( lane, false );
        SetEvent( lane->wake );
        for ( int polls = 0; polls < ROCKETNET_RIO_ROLLBACK_POLLS && !rocketnet_rio_lane_stopped( lane ); ++polls )
        {
            rocketnet_platform_sleep( 0.001 );
        }
        if ( rocketnet_rio_lane_stopped( lane ) )
        {
            rocketnet_rio_lane_destroy( lane );
        }
        else
        {
            all = false;
        }
    }
    return all;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNRioLaneLayout.tla LaneOf
int rocketnet_server_rio_lane_of( rocketnet_server_rio_t * rio, const rocketnet_address_t * to )
{
    if ( rio->num_lanes <= 1 )
    {
        return 0;
    }
    const int bytes = to->type == ROCKETNET_ADDRESS_IPV6 ? 16 : 4;
    return rocketnet_rio_lane_of( rocketnet_rio_lane_hash( to->type, to->port, (const uint8_t *) &to->data, bytes ), rio->num_lanes );
}

int rocketnet_server_rio_lane_send_room( rocketnet_server_rio_t * rio, int lane )
{
    if ( lane < 0 || lane >= rio->num_lanes )
    {
        return 0;
    }
    return rocketnet_rio_lane_send_room( rio->lanes[lane] );
}

// the room of the fullest lane: a frame to any destination fits
int rocketnet_server_rio_send_room( rocketnet_server_rio_t * rio )
{
    int room = rocketnet_rio_lane_send_room( rio->lanes[0] );
    for ( int i = 1; i < rio->num_lanes; ++i )
    {
        const int lane = rocketnet_rio_lane_send_room( rio->lanes[i] );
        room = lane < room ? lane : room;
    }
    return room;
}

bool rocketnet_server_rio_send_idle( rocketnet_server_rio_t * rio )
{
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        if ( !rocketnet_rio_lane_send_idle( rio->lanes[i] ) )
        {
            return false;
        }
    }
    return true;
}

// proofs/tla/RNRioLaneLayout.tla Start and Encode: the frame is staged in the lane its
// destination hashes to; the id carries the lane
uint8_t * rocketnet_server_rio_start_packet( rocketnet_server_rio_t * rio, const rocketnet_address_t * to, uint64_t * packet_id )
{
    const int lane = rocketnet_server_rio_lane_of( rio, to );
    uint64_t index = 0;
    uint8_t * frame = rocketnet_rio_lane_start_packet( rio->lanes[lane], to, &index );
    if ( frame )
    {
        *packet_id = rocketnet_rio_lane_packet_id( lane, (int) index, rio->lane_slots );
    }
    return frame;
}

// proofs/tla/RNRioLaneLayout.tla Decode and Foreign: an id past the last lane is refused
// and counted on lane 0, as a lane counts a frame outside its region
void rocketnet_server_rio_finish_packet( rocketnet_server_rio_t * rio, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    int lane = 0;
    int index = 0;
    if ( !rocketnet_rio_lane_packet_decode( packet_id, rio->num_lanes, rio->lane_slots, &lane, &index ) )
    {
        rocketnet_rio_count( rio->lanes[0], ROCKETNET_COUNTER_RIO_SEND_FAILED );
        return;
    }
    rocketnet_rio_lane_finish_packet( rio->lanes[lane], (uint64_t) index, frame, packet_bytes );
}

// proofs/tla/RNRioLaneLayout.tla Finish: finish_packet (above) and abort_packet act on the lane and slot the id names
void rocketnet_server_rio_abort_packet( rocketnet_server_rio_t * rio, uint64_t packet_id, uint8_t * frame )
{
    int lane = 0;
    int index = 0;
    if ( !rocketnet_rio_lane_packet_decode( packet_id, rio->num_lanes, rio->lane_slots, &lane, &index ) )
    {
        return;
    }
    rocketnet_rio_lane_abort_packet( rio->lanes[lane], (uint64_t) index, frame );
}

void rocketnet_server_rio_send_packets( rocketnet_server_rio_t * rio )
{
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        rocketnet_rio_lane_send_packets( rio->lanes[i] );
    }
}

// The view is emptied, then each lane appends the ready banks that fit,
// starting one lane later each call so no lane's banks always wait behind
// the others when the view fills.
void rocketnet_server_rio_receive_packets( rocketnet_server_rio_t * rio )
{
    rocketnet_server_socket_process_packets_t * view = &rio->lanes[0]->process;
    view->num_packets = 0;
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        rocketnet_rio_lane_receive_packets( rio->lanes[( rio->receive_start + i ) % rio->num_lanes] );
    }
    rio->receive_start = ( rio->receive_start + 1 ) % rio->num_lanes;
}

rocketnet_server_socket_process_packets_t * rocketnet_server_rio_process_packets( rocketnet_server_rio_t * rio )
{
    return rocketnet_rio_lane_process_packets( rio->lanes[0] );
}

void rocketnet_server_rio_counters( rocketnet_server_rio_t * rio, uint64_t * counters )
{
    rocketnet_rio_lane_counters( rio->lanes[0], counters );
    for ( int i = 1; i < rio->num_lanes; ++i )
    {
        uint64_t lane[ROCKETNET_NUM_COUNTERS];
        rocketnet_rio_lane_counters( rio->lanes[i], lane );
        for ( int c = 0; c < ROCKETNET_SERVER_RIO_COUNTER_ROWS; ++c )
        {
            counters[ROCKETNET_SERVER_RIO_FIRST_COUNTER + c] += lane[ROCKETNET_SERVER_RIO_FIRST_COUNTER + c];
        }
        counters[ROCKETNET_COUNTER_RIO_PIN_REFUSALS] += lane[ROCKETNET_COUNTER_RIO_PIN_REFUSALS];
    }
}

void rocketnet_server_rio_receive_drop_totals( rocketnet_server_rio_t * rio, uint64_t * values )
{
    for ( int i = 0; i < ROCKETNET_RECEIVE_NUM_DROPS; ++i )
    {
        values[i] = 0;
    }
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        rocketnet_receive_drop_tally_sum( rio->lanes[i]->drops, 1, values );
    }
}

int rocketnet_server_rio_pending( rocketnet_server_rio_t * rio )
{
    int pending = 0;
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        pending += rocketnet_rio_lane_pending( rio->lanes[i] );
    }
    return pending;
}

#if ROCKETNET_DEV
uintptr_t rocketnet_server_rio_test_socket( rocketnet_server_rio_t * rio )
{
    return rocketnet_rio_lane_test_socket( rio->lanes[0] );
}

void rocketnet_server_rio_test_fail_next( rocketnet_server_rio_t * rio, int fault, int count, int status )
{
    rocketnet_rio_lane_test_fail_next( rio->lanes[0], fault, count, status );
}

int rocketnet_server_rio_test_faults_left( rocketnet_server_rio_t * rio, int fault )
{
    return rocketnet_rio_lane_test_faults_left( rio->lanes[0], fault );
}

void rocketnet_server_rio_test_hold_send( rocketnet_server_rio_t * rio, bool hold )
{
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        rocketnet_rio_lane_test_hold_send( rio->lanes[i], hold );
    }
}

int rocketnet_server_rio_test_handed_phases( rocketnet_server_rio_t * rio )
{
    int handed = 0;
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        handed += rocketnet_rio_lane_test_handed_phases( rio->lanes[i] );
    }
    return handed;
}

bool rocketnet_server_rio_test_send_idle( rocketnet_server_rio_t * rio )
{
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        if ( !rocketnet_rio_lane_test_send_idle( rio->lanes[i] ) )
        {
            return false;
        }
    }
    return true;
}

int rocketnet_server_rio_test_retry_receives( rocketnet_server_rio_t * rio )
{
    int retry = 0;
    for ( int i = 0; i < rio->num_lanes; ++i )
    {
        retry += rocketnet_rio_lane_test_retry_receives( rio->lanes[i] );
    }
    return retry;
}
#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

int rocketnet_server_rio_windows_cpp_dummy = 0;
