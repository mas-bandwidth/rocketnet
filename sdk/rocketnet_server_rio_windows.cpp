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
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <atomic>
#include <new>
#include <stddef.h>
#include <string.h>

// The model: proofs/tla/RNRioWorker.tla (the I/O worker, its bank and phase
// handoffs and stop); proofs/tla/RNRioDriver.tla for the slot and slice rules it
// keeps. Each function that implements an action names it.

// Three send phases, as every other transport's send handoff has three
// banks: one filling, and up to two with the worker. A flush hands the fill
// phase only when the next phase is idle, so a busy worker delays sends by a
// flush instead of refusing them.
#define ROCKETNET_RIO_SEND_PHASES 3
#define ROCKETNET_RIO_DEQUEUE_BATCH 256

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

struct rocketnet_server_rio_t
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

    std::atomic<uint64_t> counters[ROCKETNET_RIO_NUM_COUNTERS]{};
    rocketnet_server_socket_process_packets_t process;
#if ROCKETNET_DEV
    bool test_hold_send = false;
    std::atomic<int> test_faults[ROCKETNET_RIO_TEST_NUM_FAULTS]{};
    std::atomic<int> test_fault_status[ROCKETNET_RIO_TEST_NUM_FAULTS]{};
    bool test_try_fails = false;                // main's next tries fail, as under contention
#if defined( _M_ARM64 )
    // Make the existing tail padding explicit for the MSVC ARM64 build.
    // These bytes preserve the 960-byte DEV layout and its 64-byte alignment.
    uint8_t test_padding[51];
#endif // #if defined( _M_ARM64 )
#endif // #if ROCKETNET_DEV
};

static_assert( offsetof( rocketnet_server_rio_t, outstanding ) == 64, "the rio's aligned members have no implicit padding" );

#if ROCKETNET_DEV && defined( _M_ARM64 )
static_assert( alignof( rocketnet_server_rio_t ) == 64 && sizeof( rocketnet_server_rio_t ) == 960,
               "the rio DEV layout retains its cache-line alignment and size" );
static_assert( offsetof( rocketnet_server_rio_t, test_try_fails ) == 908 && offsetof( rocketnet_server_rio_t, test_padding ) == 909,
               "explicit tail padding follows the final DEV field without moving it" );
static_assert( offsetof( rocketnet_server_rio_t, test_padding ) + sizeof( ( (rocketnet_server_rio_t *) 0 )->test_padding ) == sizeof( rocketnet_server_rio_t ),
               "the rio DEV layout has no implicit tail padding" );
#endif // #if ROCKETNET_DEV && defined( _M_ARM64 )

// The fault seam (rocketnet_server_rio_test_fail_next) exists only in the
// test executable. Elsewhere the check is the constant false, so the
// production paths carry no test branch.
#if ROCKETNET_DEV
static bool rocketnet_rio_test_fault( rocketnet_server_rio_t * rio, int fault, int * status )
{
    int left = rio->test_faults[fault].load( std::memory_order_acquire );
    while ( left > 0 && !rio->test_faults[fault].compare_exchange_weak( left, left - 1, std::memory_order_acq_rel ) )
    {
    }

    if ( left <= 0 )
        return false;
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

#endif // #if ROCKETNET_DEV

// The worker's RIO calls on its queues, through the test table when a unit
// seam installed one.
static ULONG rocketnet_rio_dequeue( rocketnet_server_rio_t * rio, RIO_CQ cq, RIORESULT * results, ULONG max_results )
{
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions )
        return rocketnet_rio_test_functions->dequeue( cq, results, max_results );
#endif // #if ROCKETNET_DEV
    return rio->rio.RIODequeueCompletion( cq, results, max_results );
}

static BOOL rocketnet_rio_send_call( rocketnet_server_rio_t * rio, RIO_BUF * data, RIO_BUF * address, DWORD flags, void * request_context )
{
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions && rocketnet_rio_test_functions->send )
        return rocketnet_rio_test_functions->send( rio->request_queue, data, data ? 1 : 0, NULL, address, NULL, NULL, flags, request_context );
#endif // #if ROCKETNET_DEV
    return rio->rio.RIOSendEx( rio->request_queue, data, data ? 1 : 0, NULL, address, NULL, NULL, flags, request_context );
}

static int rocketnet_rio_close_call( rocketnet_server_rio_t * rio )
{
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions && rocketnet_rio_test_functions->close )
        return rocketnet_rio_test_functions->close( rio->socket );
#endif // #if ROCKETNET_DEV
    return closesocket( rio->socket );
}

static void rocketnet_rio_count( rocketnet_server_rio_t * rio, int counter, uint64_t amount = 1 )
{
    rio->counters[counter].fetch_add( amount, std::memory_order_relaxed );
}

// posted sends not yet retired, every phase
static int rocketnet_rio_outstanding_sends( rocketnet_server_rio_t * rio )
{
    int total = 0;
    for ( int p = 0; p < ROCKETNET_RIO_SEND_PHASES; ++p )
        total += rio->outstanding[p].load( std::memory_order_relaxed );
    return total;
}

// a destination as the SOCKADDR_INET the socket's family needs: a dual
// stack socket sends IPv4 as v4-mapped IPv6
static bool rocketnet_rio_address_write( const rocketnet_address_t * input, bool ipv6, SOCKADDR_INET * out )
{
    rocketnet_address_t address = *input;
    memset( out, 0, sizeof( *out ) );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
        rocketnet_address_convert_ipv4_to_ipv6( &address );
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
static uint8_t * rocketnet_rio_frame( rocketnet_server_rio_t * rio, int index )
{
    return rio->region + rocketnet_rio_send_frame_offset( &rio->layout, index );
}

static SOCKADDR_INET * rocketnet_rio_send_address( rocketnet_server_rio_t * rio, int index )
{
    return (SOCKADDR_INET *) ( rio->region + rocketnet_rio_send_address_offset( &rio->layout, index ) );
}

static const SOCKADDR_INET * rocketnet_rio_receive_address( rocketnet_server_rio_t * rio, int slice )
{
    return (const SOCKADDR_INET *) ( rio->region + rocketnet_rio_receive_address_offset( &rio->layout, slice ) );
}

// one RIO_MSG_COMMIT_ONLY when deferred sends are uncommitted; a refused
// commit leaves them uncommitted for the next try and counts nothing: each
// deferred send still completes once, as PACKETS_SENT or SEND_FAILED
static void rocketnet_rio_commit( rocketnet_server_rio_t * rio )
{
    if ( !rio->uncommitted )
        return;
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, &injected ) ||
         !rocketnet_rio_send_call( rio, NULL, NULL, RIO_MSG_COMMIT_ONLY, NULL ) )
    {
        rocketnet_warn( "rio transport: send commit refused (Winsock %d); retried", WSAGetLastError() );
        return;
    }

    rio->uncommitted = false;
    rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_COMMITS );
}

// the send frame index of a frame pointer: a pointer that is not the root of
// a send frame lands out of range or off a root and returns -1, a lost
// packet, never an out-of-bounds write
static int rocketnet_rio_frame_index( rocketnet_server_rio_t * rio, const uint8_t * frame )
{
    if ( !frame || !rio->region )
        return -1;
    const uint8_t * first = rio->region + rio->layout.send_frames_offset;
    if ( frame < first )
        return -1;
    const uint64_t offset = (uint64_t) ( frame - first );
    const uint64_t stride = rocketnet_rio_element_stride( rio->layout.frame_bytes );
    if ( offset % stride != 0 || offset / stride >= (uint64_t) rio->send_frames )
        return -1;
    // only a reserved slot of the open fill phase: a stale frame of a phase
    // the kernel owns is refused, never rewritten under a posted send

    const int index = (int) ( offset / stride );
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const int slot = index - phase * rio->send_queue_size;
    if ( slot < 0 || slot >= rio->send_queue_size ||
         (uint64_t) slot >= rio->next[phase].load( std::memory_order_relaxed ) )
        return -1;
    return index;
}

// posts one receive slice with its address slot (create, and the re-post
// in DequeueReceive)
static bool rocketnet_rio_post_receive( rocketnet_server_rio_t * rio, int slice )
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
        return false;
    int use_real_receive = 1;
#if ROCKETNET_DEV
    if ( rocketnet_rio_test_functions )
    {
        use_real_receive = 0;
        if ( !rocketnet_rio_test_functions->receive( rio->request_queue, &data, 1, NULL, &address, NULL, NULL, 0,
                                                     (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) slice ) ) )
            return false;
    }
#endif
    if ( use_real_receive && !rio->rio.RIOReceiveEx( rio->request_queue, &data, 1, NULL, &address, NULL, NULL, 0,
                                                     (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) slice ) ) )
    {
        // a post refused with an ICMP report of an earlier send is counted
        // with the reset statuses; a refused re-post waits on the retry list in
        // rocketnet_rio_complete_receive
        if ( rocketnet_platform_receive_status_is_reset( WSAGetLastError() ) )
            rocketnet_platform_count_receive_reset();
        return false;
    }

    rio->posted_receives.fetch_add( 1, std::memory_order_relaxed );

    return true;
}

// RNRioDriver RetireSend: one send completion; the slot is the transport's
// again. A failed status counts SEND_FAILED.
static void rocketnet_rio_retire_send( rocketnet_server_rio_t * rio, const RIORESULT * result )
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
        status = (LONG) injected;
    const bool sent = status == 0 && result->BytesTransferred == (ULONG) rio->slot_bytes[index];
    rocketnet_rio_count( rio, sent ? ROCKETNET_RIO_COUNTER_PACKETS_SENT : ROCKETNET_RIO_COUNTER_SEND_FAILED );
    rio->slot_bytes[index] = 0;
    rio->outstanding[phase].fetch_sub( 1, std::memory_order_relaxed );
}

// proofs/tla/RNRioDriver.tla DequeueSend: dequeues the send
// completion queue without blocking. Returns the completions retired.
// proofs/tla/RNRioSendSlotLayout.tla Complete: each completion frees its slot.
static int rocketnet_rio_dequeue_send( rocketnet_server_rio_t * rio )
{
    int total = 0;

    for ( ;; )
    {
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rocketnet_rio_dequeue( rio, rio->send_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
        if ( count == RIO_CORRUPT_CQ )
        {
            rocketnet_error( "rio transport: send completion queue corrupt" );
            return total;
        }

        for ( ULONG i = 0; i < count; ++i )
            rocketnet_rio_retire_send( rio, &results[i] );
        total += (int) count;
        if ( count < ROCKETNET_RIO_DEQUEUE_BATCH )
            return total;
    }
}

// proofs/tla/RNRioDriver.tla Close: closesocket completes every outstanding request
// with an error. proofs/tla/RNRioWorker.tla WClose: the close waits for a successful
// commit of the deferred sends, then for every committed send to complete.
// A deferred send still occupies request-queue capacity after a refused
// commit. Keep the socket open so a later completion check can retry the
// commit before closing the request queue that owns those sends. A
// committed send still in flight at closesocket is cancelled and retires as
// SEND_FAILED, which would lose the shutdown disconnect burst, so each pass
// retires the send completions available and keeps the socket open while
// any send is outstanding.
static bool rocketnet_rio_close_socket( rocketnet_server_rio_t * rio )
{
    if ( rio->request_queue != RIO_INVALID_RQ && rio->uncommitted )
    {
        rocketnet_rio_commit( rio );
        if ( rio->uncommitted )
            return false;
    }

    if ( rio->socket != INVALID_SOCKET && rocketnet_rio_outstanding_sends( rio ) > 0 )
    {
        if ( rio->have_table && rio->send_cq != RIO_INVALID_CQ )
            rocketnet_rio_dequeue_send( rio );
        if ( rocketnet_rio_outstanding_sends( rio ) > 0 )
            return false;
    }

    if ( rio->socket != INVALID_SOCKET )
    {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        const bool have_request_queue = rio->request_queue != RIO_INVALID_RQ;
        if ( have_request_queue && !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_RIO_RQ,
                                                                           (uintptr_t) rio->request_queue, rio->request_queue_generation ) )
            return false;
        if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) rio->socket, rio->socket_generation ) )
        {
            if ( have_request_queue )
                rocketnet_lifecycle_track_close_end( rio->request_queue_generation, false );
            return false;
        }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( rocketnet_rio_close_call( rio ) != 0 )
        {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            rocketnet_lifecycle_track_close_end( rio->socket_generation, false );
            if ( have_request_queue )
                rocketnet_lifecycle_track_close_end( rio->request_queue_generation, false );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            return false;
        }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        rocketnet_lifecycle_track_close_end( rio->socket_generation, true );
        if ( have_request_queue )
            rocketnet_lifecycle_track_close_end( rio->request_queue_generation, true );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        rio->socket = INVALID_SOCKET;
    }

    return true;
}

static void rocketnet_rio_post_phase( rocketnet_server_rio_t * rio, int phase, int count );

// proofs/tla/RNRioWorker.tla WStop, WClose and WDrain: post handed and finished fill
// phases, commit deferred sends, retire send completions until no committed
// send is in flight, close the socket (which cancels every posted receive),
// then dequeue one available batch of each queue. True when nothing is
// kernel owned. Runs on the worker, or inline on create's failure path
// before the worker exists; never waits.
// proofs/tla/RNRioDriver.tla Drain: both completion queues are dequeued and nothing
// is re-posted.
static bool rocketnet_rio_stop_pass( rocketnet_server_rio_t * rio )
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
        rocketnet_rio_post_phase( rio, phases[i], counts[i] );
    // Closing a UDP socket cancels pending RIO requests. Keep its registered
    // region and queues until their completions have all been observed.

    if ( !rocketnet_rio_close_socket( rio ) )
        return false;
    // Exactly one available batch per queue, never wait for new completions.

    if ( rio->have_table && rio->send_cq != RIO_INVALID_CQ )
    {
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rocketnet_rio_dequeue( rio, rio->send_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
        if ( count == RIO_CORRUPT_CQ )
            return false;
        for ( ULONG i = 0; i < count; ++i )
            rocketnet_rio_retire_send( rio, &results[i] );
    }

    if ( rio->have_table && rio->receive_cq != RIO_INVALID_CQ )
    {
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rocketnet_rio_dequeue( rio, rio->receive_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
        if ( count == RIO_CORRUPT_CQ )
            return false;
        rio->posted_receives.fetch_sub( (int) count, std::memory_order_relaxed );
    }

    return rocketnet_rio_outstanding_sends( rio ) == 0 &&
           rio->posted_receives.load( std::memory_order_relaxed ) == 0;
}

// proofs/tla/RNRioWorker.tla Stop: stopping, then the wake. Never waits.
void rocketnet_server_rio_stop( rocketnet_server_rio_t * rio )
{
    if ( !rio || rio->stopping.exchange( true, std::memory_order_acq_rel ) )
        return;
    if ( rio->wake )
        SetEvent( rio->wake );
}

// proofs/tla/RNRioWorker.tla Poll: main polls stopped, which changes nothing.
// The worker reports stopped only after its last drain pass. Before the
// worker exists (create's failure path) the drain runs here, one pass a call.
bool rocketnet_server_rio_stopped( rocketnet_server_rio_t * rio )
{
    if ( !rio || !rio->stopping.load( std::memory_order_acquire ) )
        return false;
    if ( rio->thread )
        return rio->exited.load( std::memory_order_acquire );
    return rocketnet_rio_stop_pass( rio );
}

// proofs/tla/RNRioWorker.tla Destroy: the registered region is freed only after stopped.
// proofs/tla/RNRioDriver.tla Free and Done: RIOCloseCompletionQueue,
// RIODeregisterBuffer and VirtualFree once every request is accounted for,
// then the transport itself is freed and nothing of it runs again.
void rocketnet_server_rio_destroy( rocketnet_server_rio_t * rio )
{
    if ( !rio )
        return;
    const bool stopped = rocketnet_server_rio_stopped( rio );
    rocketnet_assert( stopped );
    if ( !stopped )
        return;
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
        rocketnet_free( context, rio->region );
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
        rocketnet_platform_mutex_destroy( &rio->mutex );
    rocketnet_free( context, rio->sized_storage );
    rio->~rocketnet_server_rio_t();
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
    uint64_t bytes;
};

static bool rocketnet_rio_sized_layout( int max_clients, rocketnet_rio_sized_layout_t * layout )
{
    memset( layout, 0, sizeof( *layout ) );
    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
        return false;

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
static void rocketnet_rio_sized_bind( rocketnet_server_rio_t * rio, uint8_t * base, const rocketnet_rio_sized_layout_t * layout )
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
            rio->free_banks[rio->free_count++] = i;
    }
}

uint64_t rocketnet_server_rio_allocation_bytes( int max_clients )
{
    rocketnet_rio_sized_layout_t sized;
    if ( !rocketnet_rio_sized_layout( max_clients, &sized ) )
        return 0;
    rocketnet_rio_layout_t layout;
    if ( !rocketnet_rio_layout_plan( sized.send_frames, sized.receives, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &layout ) )
        return 0;
    uint64_t bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_rio_t ), sized.bytes } );
    if ( !bytes || !rocketnet_memory_add( bytes, 1, rocketnet_platform_thread_allocation_bytes() ) )
        return 0;
    return bytes;
}

uint64_t rocketnet_server_rio_registered_region_bytes( int max_clients )
{
    rocketnet_rio_sized_layout_t sized;
    if ( !rocketnet_rio_sized_layout( max_clients, &sized ) )
        return 0;
    rocketnet_rio_layout_t layout;
    if ( !rocketnet_rio_layout_plan( sized.send_frames, sized.receives, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &layout ) )
        return 0;
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
    SOCKET socket = WSASocketW( AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_REGISTERED_IO );
    if ( socket == INVALID_SOCKET )
        return false;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const uint64_t probe_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) socket,
                                                                      "rio extension probe socket" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) socket );
    RIO_EXTENSION_FUNCTION_TABLE table;
    const bool available = rocketnet_rio_load_table( socket, &table );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) socket, probe_generation ) )
        return false;
    const int closed = closesocket( socket );
    rocketnet_lifecycle_track_close_end( probe_generation, closed == 0 );
#else
    closesocket( socket );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return available;
}

static void rocketnet_rio_worker( rocketnet_platform_thread_t *, void * argument );

rocketnet_server_rio_t * rocketnet_server_rio_create( void * context, rocketnet_address_t * bind_address, int max_clients
#if ROCKETNET_DEV
                                                      ,
                                                      int fail_stage
#endif // #if ROCKETNET_DEV
                                                      ,
                                                      int * status )
{
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) )
        return NULL;
    rocketnet_rio_sized_layout_t sized;
    if ( !rocketnet_rio_sized_layout( max_clients, &sized ) )
    {
        rocketnet_warn( "rio transport refused: max_clients %d is outside 1..%d", max_clients, ROCKETNET_MAX_CLIENTS );
        return NULL;
    }
    void * storage = rocketnet_malloc( context, sizeof( rocketnet_server_rio_t ) );
    if ( !storage )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    rocketnet_server_rio_t * rio = new ( storage ) rocketnet_server_rio_t();
    rio->context = context;
    rio->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;

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
        goto failed;
    rio->wake = CreateEventW( NULL, FALSE, FALSE, NULL );
    rio->notify = CreateEventW( NULL, FALSE, FALSE, NULL );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->wake )
        rio->wake_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->wake, "rio wake event" );
    if ( rio->notify )
        rio->notify_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->notify, "rio notify event" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !rio->wake || !rio->notify )
        goto failed;
    notification.Type = RIO_EVENT_COMPLETION;
    notification.Event.EventHandle = rio->notify;
    notification.Event.NotifyReset = FALSE;

    if ( !rocketnet_rio_address_write( bind_address, rio->ipv6, &address ) )
        goto failed;
    address_bytes = rio->ipv6 ? (int) sizeof( sockaddr_in6 ) : (int) sizeof( sockaddr_in );

    rio->socket = WSASocketW( rio->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_REGISTERED_IO );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->socket != INVALID_SOCKET )
        rio->socket_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) rio->socket, "rio socket" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->socket == INVALID_SOCKET
#if ROCKETNET_DEV
         || fail_stage == 1
#endif // #if ROCKETNET_DEV
    )
        goto failed;
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
        goto failed;
    if ( rio->ipv6 && setsockopt( rio->socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &v6_only, sizeof( v6_only ) ) == SOCKET_ERROR )
        goto failed;
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_RCVBUF, (const char *) &buffer_bytes, sizeof( buffer_bytes ) ) == SOCKET_ERROR )
        goto failed;
    if ( getsockopt( rio->socket, SOL_SOCKET, SO_RCVBUF, (char *) &granted_receive_bytes, &granted_length ) == SOCKET_ERROR )
        granted_receive_bytes = 0;
    rocketnet_server_receive_buffer_granted( "rio", receive_buffer_bytes, granted_receive_bytes );
    buffer_bytes = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_SNDBUF, (const char *) &buffer_bytes, sizeof( buffer_bytes ) ) == SOCKET_ERROR )
        goto failed;
    // Match the portable socket's don't-fragment rule, tolerating option refusal.

    setsockopt( rio->socket, rio->ipv6 ? IPPROTO_IPV6 : IPPROTO_IP,
                rio->ipv6 ? IPV6_DONTFRAG : IP_DONTFRAGMENT, (const char *) &enabled, sizeof( enabled ) );
    // both ICMP reset reports off; a refusal is logged once and the receive
    // path skips the statuses

    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) rio->socket );
    if ( bind( rio->socket, (sockaddr *) &address, address_bytes ) == SOCKET_ERROR )
        goto failed;
    address_bytes = sizeof( address );
    if ( getsockname( rio->socket, (sockaddr *) &address, &address_bytes ) == SOCKET_ERROR )
        goto failed;

    if ( !rocketnet_rio_load_table( rio->socket, &rio->rio ) )
        goto failed;
    rio->have_table = true;
#if ROCKETNET_DEV
    if ( fail_stage == 2 )
        goto failed;
#endif // #if ROCKETNET_DEV

    // one region, carved only by the layout, registered once

    if ( !rocketnet_rio_layout_plan( rio->send_frames, rio->receives, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &rio->layout ) )
        goto failed;
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
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->buffer_id != RIO_INVALID_BUFFERID )
        rio->buffer_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_BUFFER, (uintptr_t) rio->buffer_id, "rio buffer registration" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->buffer_id == RIO_INVALID_BUFFERID
#if ROCKETNET_DEV
         || fail_stage == 3
#endif // #if ROCKETNET_DEV
    )
        goto failed;

    // both completion queues notify the worker's one event after RIONotify

    rio->send_cq = rio->rio.RIOCreateCompletionQueue( (DWORD) rio->send_frames, &notification );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->send_cq != RIO_INVALID_CQ )
        rio->send_cq_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_CQ, (uintptr_t) rio->send_cq, "rio send completion queue" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->send_cq == RIO_INVALID_CQ )
        goto failed;
    rio->receive_cq = rio->rio.RIOCreateCompletionQueue( (DWORD) rio->receives, &notification );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->receive_cq != RIO_INVALID_CQ )
        rio->receive_cq_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_CQ, (uintptr_t) rio->receive_cq, "rio receive completion queue" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->receive_cq == RIO_INVALID_CQ
#if ROCKETNET_DEV
         || fail_stage == 4
#endif // #if ROCKETNET_DEV
    )
        goto failed;

    // the request queue holds every send of every phase plus every receive

    rio->request_queue = rio->rio.RIOCreateRequestQueue( rio->socket, (ULONG) rio->receives, 1, (ULONG) rio->send_frames, 1,
                                                         rio->receive_cq, rio->send_cq, NULL );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->request_queue != RIO_INVALID_RQ )
        rio->request_queue_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_RIO_RQ, (uintptr_t) rio->request_queue, "rio socket-owned request queue" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->request_queue == RIO_INVALID_RQ
#if ROCKETNET_DEV
         || fail_stage == 5
#endif // #if ROCKETNET_DEV
    )
        goto failed;

    // RNRioDriver Init: every receive slice is posted

    for ( int i = 0; i < rio->receives; ++i )
    {
        if ( !rocketnet_rio_post_receive( rio, i ) )
            goto failed;
#if ROCKETNET_DEV
        if ( fail_stage == 6 )
            goto failed;
#endif // #if ROCKETNET_DEV
    }

    if ( !rocketnet_rio_address_read( &address, rio->ipv6, bind_address ) )
        goto failed;

    // the worker starts last: from here every RIO call is its own

    rio->thread = rocketnet_platform_thread_create( context, rocketnet_rio_worker, rio, status );
    if ( !rio->thread )
        goto failed;
    rocketnet_platform_server_thread_priority( rio->thread );
    rocketnet_info( "rio transport: port %d, one I/O worker, %d receive slices, %d send frames per phase, %llu registered bytes", bind_address->port,
                    rio->receives, rio->send_queue_size, (unsigned long long) rio->layout.total_bytes );
    return rio;

failed:
#if ROCKETNET_DEV
    rocketnet_warn( "rio transport create failed (Winsock %d, Win32 %lu, test stage %d)", WSAGetLastError(), GetLastError(), fail_stage );
#else
    rocketnet_warn( "rio transport create failed (Winsock %d, Win32 %lu)", WSAGetLastError(), GetLastError() );
#endif
    // Partial construction remains private to this synchronous create call.
    rocketnet_server_rio_stop( rio );
    while ( !rocketnet_server_rio_stopped( rio ) )
        rocketnet_platform_sleep( 0.001 );
    rocketnet_server_rio_destroy( rio );

    return NULL;
}

// proofs/tla/ServerStopSpread.tla Room: the open fill phase's free slots
int rocketnet_server_rio_send_room( rocketnet_server_rio_t * rio )
{
    if ( rio->stopping.load( std::memory_order_acquire ) )
        return 0;
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const uint64_t used = rio->next[phase].load( std::memory_order_relaxed );
    return used >= (uint64_t) rio->send_queue_size ? 0 : (int) ( (uint64_t) rio->send_queue_size - used );
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: no slot staged, handed or
// posted without its completion
bool rocketnet_server_rio_send_idle( rocketnet_server_rio_t * rio )
{
    const int fill = rio->fill.load( std::memory_order_relaxed );
    if ( rio->next[fill].load( std::memory_order_relaxed ) != 0 || rocketnet_rio_outstanding_sends( rio ) != 0 )
        return false;
    if ( !rocketnet_platform_mutex_try_acquire( &rio->mutex ) )
        return false;
    bool idle = rio->send_ready_count == 0;
    for ( int phase = 0; phase < ROCKETNET_RIO_SEND_PHASES; phase++ )
    {
        if ( phase != fill && rio->phase_state[phase] != ROCKETNET_RIO_PHASE_IDLE )
            idle = false;
    }
    rocketnet_platform_mutex_release( &rio->mutex );

    return idle;
}

// proofs/tla/RNRioDriver.tla Reserve: one fetch_add reserves the next slot of the fill
// phase; past the last slot it returns NULL and counts SEND_FULL.
// proofs/tla/RNRioWorker.tla Stage, first half: a fill thread takes a slot of the
// open fill phase.
// proofs/tla/RNRioSendSlotLayout.tla Reserve: one fetch_add per call, refused or not; a counter that wraps
// within reach hands out a staged slot again (MCRNRioSendSlotLayoutBrokenIntWrap.cfg).
uint8_t * rocketnet_server_rio_start_packet( rocketnet_server_rio_t * rio, const rocketnet_address_t * to, uint64_t * packet_id )
{
    if ( rio->stopping.load( std::memory_order_acquire ) )
        return NULL;
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const uint64_t slot = rio->next[phase].fetch_add( 1, std::memory_order_relaxed );
    if ( slot >= (uint64_t) rio->send_queue_size )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FULL );
        return NULL;
    }

    const int index = phase * rio->send_queue_size + (int) slot;
    rio->slot_bytes[index] = 0;
    if ( !rocketnet_rio_address_write( to, rio->ipv6, rocketnet_rio_send_address( rio, index ) ) )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
        return NULL;                            // the slot stays at zero bytes and is never posted
    }

    *packet_id = (uint64_t) index;

    return rocketnet_rio_frame( rio, index );
}

// proofs/tla/RNRioDriver.tla Finish: the slot records its bytes.
// proofs/tla/RNRioWorker.tla Stage, second half: the staged packet is postable.
// proofs/tla/RNRioSendSlotLayout.tla Finish: the reserved slot finished.
void rocketnet_server_rio_finish_packet( rocketnet_server_rio_t * rio, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );
    (void) packet_id;

    // a refused finish loses the packet: the slot keeps zero bytes and the
    // post phase skips it, so the loss is counted here
    const int index = rocketnet_rio_frame_index( rio, frame );
    if ( index < 0 || packet_bytes <= 0 || packet_bytes > ROCKETNET_RECEIVE_CEILING )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
        return;
    }
    rio->slot_bytes[index] = packet_bytes;
}

// proofs/tla/RNRioDriver.tla Abort: the slot's bytes are zeroed and it is never posted
// proofs/tla/RNRioSendSlotLayout.tla Finish: the reserved slot aborted, free again.
void rocketnet_server_rio_abort_packet( rocketnet_server_rio_t * rio, uint64_t packet_id, uint8_t * frame )
{
    (void) packet_id;

    const int index = rocketnet_rio_frame_index( rio, frame );
    if ( index < 0 )
        return;
    rio->slot_bytes[index] = 0;
}

// proofs/tla/RNRioWorker.tla SendPackets and TryFail, main with fill threads
// joined. One try of the mutex hands a non-empty fill phase to the worker, only when the next phase
// is idle (every send of it retired), and opens that phase to the fill
// threads. A failed try or a next phase not yet idle keeps the fill phase
// open: the fill threads keep appending and the next flush hands it. Main
// never waits and makes no RIO call.
// proofs/tla/RNRioDriver.tla SwitchPhase: the fill phase switches only to a phase
// whose sends have all completed, and counts SEND_WAITS otherwise.
// proofs/tla/RNRioSendSlotLayout.tla SendPackets: count = next[fill] clamped to the queue, the send_ready
// tail % ROCKETNET_RIO_SEND_PHASES, next[next] = 0 and fill = next.
void rocketnet_server_rio_send_packets( rocketnet_server_rio_t * rio )
{
    if ( rio->stopping.load( std::memory_order_acquire ) )
        return;
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const uint64_t staged = rio->next[phase].load( std::memory_order_relaxed );
    if ( staged == 0 )
        return;                  // nothing staged
    const int count = staged < (uint64_t) rio->send_queue_size ? (int) staged : rio->send_queue_size;
#if ROCKETNET_DEV
    if ( rio->test_try_fails )
        return;
#endif // #if ROCKETNET_DEV
    if ( !rocketnet_platform_mutex_try_acquire( &rio->mutex ) )
        return;
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
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_WAITS );
        return;
    }

    rio->next[next].store( 0, std::memory_order_relaxed );
    rio->fill.store( next, std::memory_order_relaxed );
    SetEvent( rio->wake );
}

// proofs/tla/RNRioWorker.tla WPost and WCommit: every finished slot of a taken phase by
// RIOSendEx with RIO_MSG_DEFER, then one RIO_MSG_COMMIT_ONLY. Between post
// and completion the kernel owns the slot's frame and its address slot.
// proofs/tla/RNRioDriver.tla Post: posts every finished slot of the phase and adds
// the posted count to the phase's outstanding count.
// proofs/tla/RNRioSendSlotLayout.tla TakeAndPost: finished slots below the count posted, the rest zeroed.
static void rocketnet_rio_post_phase( rocketnet_server_rio_t * rio, int phase, int count )
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
            posted++;
        else
        {
            // an unsuccessful post has no completion: the slot is free again
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
            rio->slot_bytes[index] = 0;
        }
    }

    rio->outstanding[phase].fetch_add( posted, std::memory_order_relaxed );

    if ( posted > 0 )
        rio->uncommitted = true;

    rocketnet_rio_commit( rio );
}

// proofs/tla/RNRioDriver.tla DequeueReceive for one completion: a failed status counts
// RECEIVE_FAILED; a datagram is checked against the size bounds and copied
// into the fill bank (the dequeue never takes more completions than the bank
// has room for); then the slice is re-posted, or waits in the retry list
static void rocketnet_rio_complete_receive( rocketnet_server_rio_t * rio, const RIORESULT * result )
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
        status = (LONG) injected;
    // an ICMP report of an earlier send (port, host or network unreachable)
    // is counted with the failed receives and the slice is re-posted below,
    // never the end of the drain

    if ( rocketnet_platform_receive_status_is_reset( (int) status ) )
        rocketnet_platform_count_receive_reset();
    if ( status == WSAEMSGSIZE )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE );     // past the slice: oversize
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE );
    }
    else if ( status != 0 )
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED );
    else
    {
        const int bytes = (int) result->BytesTransferred;
        rocketnet_address_t from;
        rocketnet_rio_bank_t * bank = &rio->banks[rio->fill_bank];

        if ( bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || bytes > ROCKETNET_RECEIVE_CEILING )
        {
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE );
            rocketnet_rio_count( rio, bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES ? ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT : ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE );
        }
        else if ( !rocketnet_rio_address_read( rocketnet_rio_receive_address( rio, slice ), rio->ipv6, &from ) )
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED );
        else
        {
            rocketnet_assert( bank->num_packets < bank->capacity );     // the dequeue takes at most the bank's room

            const int index = bank->num_packets++;
            bank->from[index] = from;
            bank->packet_bytes[index] = bytes;
            memcpy( bank->data[index], rio->region + rocketnet_rio_receive_slice_offset( &rio->layout, slice ), (size_t) bytes );
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED );
        }
    }

    // after stop nothing is re-posted. A refused re-post waits in the retry
    // list for the next worker pass, so a transient refusal never shrinks
    // the receive depth.

    if ( !rio->stopping.load( std::memory_order_acquire ) && !rocketnet_rio_post_receive( rio, slice ) )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED );

        const int retry = rio->num_retry.load( std::memory_order_relaxed );
        rocketnet_assert( retry < rio->receives );
        rio->retry_slices[retry] = slice;
        rio->num_retry.store( retry + 1, std::memory_order_relaxed );
    }
}

// proofs/tla/RNRioWorker.tla WDequeue, first half, and proofs/tla/RNRioDriver.tla
// RetryRepost: one more post of each slice a refused re-post left in the
// retry list; a slice refused again stays for the next pass and counts
// RECEIVE_REPOST_FAILED again.
static void rocketnet_rio_retry_receives( rocketnet_server_rio_t * rio )
{
    const int num_retry = rio->num_retry.load( std::memory_order_relaxed );
    if ( num_retry == 0 || rio->stopping.load( std::memory_order_acquire ) )
        return;
    int kept = 0;

    for ( int i = 0; i < num_retry; ++i )
    {
        const int slice = rio->retry_slices[i];
        if ( !rocketnet_rio_post_receive( rio, slice ) )
        {
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED );
            rio->retry_slices[kept++] = slice;
        }
    }

    rio->num_retry.store( kept, std::memory_order_relaxed );
}

// proofs/tla/RNRioWorker.tla WDequeue, second half: every available receive completion up
// to the fill bank's room. A full bank leaves the rest in the queue with
// their slices unposted (backpressure), never dropped. Returns the count.
static int rocketnet_rio_dequeue_receive( rocketnet_server_rio_t * rio )
{
    int total = 0;
    int room = rio->receives - rio->banks[rio->fill_bank].num_packets;
    while ( room > 0 )
    {
        const ULONG batch = room < ROCKETNET_RIO_DEQUEUE_BATCH ? (ULONG) room : ROCKETNET_RIO_DEQUEUE_BATCH;
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rocketnet_rio_dequeue( rio, rio->receive_cq, results, batch );
        if ( count == RIO_CORRUPT_CQ )
        {
            rocketnet_error( "rio transport: receive completion queue corrupt" );
            break;
        }

        for ( ULONG i = 0; i < count; ++i )
            rocketnet_rio_complete_receive( rio, &results[i] );
        total += (int) count;
        room -= (int) count;
        if ( count < batch )
            break;
    }

    return total;
}

// proofs/tla/RNRioWorker.tla WTake, WRetire and WPublish: one worker pass, with
// WPost, WCommit and WDequeue in the calls it makes. True when it did work.
// proofs/tla/RNBankFifoLayout.tla Publish: the receive ready tail % ROCKETNET_RIO_RECEIVE_BANKS and the free
// stack's top. proofs/tla/RNRioSendSlotLayout.tla TakeAndPost and Retire: the send_ready head % ROCKETNET_RIO_SEND_PHASES
// and a drained posted phase idle again.
static bool rocketnet_rio_worker_pass( rocketnet_server_rio_t * rio )
{
    bool progress = false;

    // WTake: the oldest handed phase, which main now sees posted

    int phase = -1;
    int count = 0;
    rocketnet_platform_mutex_acquire( &rio->mutex );

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
        progress = true;

    // WDequeue

    rocketnet_rio_retry_receives( rio );
    if ( rocketnet_rio_dequeue_receive( rio ) > 0 )
        progress = true;

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
        if ( rio->phase_state[p] == ROCKETNET_RIO_PHASE_POSTED && rio->outstanding[p].load( std::memory_order_relaxed ) == 0 )
            rio->phase_state[p] = ROCKETNET_RIO_PHASE_IDLE;
    rocketnet_platform_mutex_release( &rio->mutex );
    if ( published )
        rio->banks[rio->fill_bank].num_packets = 0;
    return progress;
}

// proofs/tla/RNRioWorker.tla WArm and WWait: nothing progressed. RIONotify on the receive
// queue when the fill bank has room (a full bank waits for main's wake
// alone), and on the send queue while sends are outstanding; RIONotify on a
// non-empty queue sets the event at once. A refused commit or re-post waits
// one millisecond and retries; otherwise the bounded wait tolerates a lost
// hint.
static void rocketnet_rio_idle_wait( rocketnet_server_rio_t * rio )
{
    const bool sends = rocketnet_rio_outstanding_sends( rio ) > 0;
    if ( rio->banks[rio->fill_bank].num_packets < rio->receives )
        rio->rio.RIONotify( rio->receive_cq );  // WSAEALREADY: an earlier notify is still armed
    if ( sends )
        rio->rio.RIONotify( rio->send_cq );
    const bool retry = rio->uncommitted || rio->num_retry.load( std::memory_order_relaxed ) > 0;
    HANDLE handles[2] = { rio->wake, rio->notify };
    WaitForMultipleObjects( 2, handles, FALSE, retry ? 1 : 50 );
}

// proofs/tla/RNRioWorker.tla WTop: the loop reads stopping at the top of every pass
// and turns to the stop pass once it is set.
static void rocketnet_rio_worker( rocketnet_platform_thread_t *, void * argument )
{
    rocketnet_server_rio_t * rio = (rocketnet_server_rio_t *) argument;

    for ( ;; )
    {
        if ( rio->stopping.load( std::memory_order_acquire ) )
        {
            if ( rocketnet_rio_stop_pass( rio ) )
                break;
            WaitForSingleObject( rio->notify, 1 );
            continue;
        }

        if ( !rocketnet_rio_worker_pass( rio ) )
            rocketnet_rio_idle_wait( rio );
    }

    rio->exited.store( true, std::memory_order_release );
}

// proofs/tla/RNRioWorker.tla ReceivePackets and TryFail, main: one try of the
// mutex returns the banks exposed last call and exposes the ready banks that fit the process view,
// oldest first; the views stay valid until the next call. A failed try
// exposes nothing new and retains everything main owns. Main wakes the
// worker when it returns banks to an empty free list (a worker with a full
// fill bank may be waiting for one).
// proofs/tla/RNRioDriver.tla Swap: the banks the worker filled become the exposed
// banks whose views process_packets returns until the next call.
// proofs/tla/RNBankFifoLayout.tla Release and Take: the exposed banks back on the free stack, then the
// ready banks that fit from the ready head, MCRNBankFifoLayoutLifoExposed.cfg.
void rocketnet_server_rio_receive_packets( rocketnet_server_rio_t * rio )
{
    rio->process.num_packets = 0;
    if ( rio->stopping.load( std::memory_order_acquire ) )
        return;
#if ROCKETNET_DEV
    if ( rio->test_try_fails )
        return;
#endif // #if ROCKETNET_DEV
    if ( !rocketnet_platform_mutex_try_acquire( &rio->mutex ) )
        return;
    const bool wake = rio->free_count == 0 && rio->exposed_count > 0;
    for ( int j = 0; j < rio->exposed_count; ++j )
        rio->free_banks[rio->free_count++] = rio->exposed_banks[j];
    rio->exposed_count = 0;

    int total = 0;

    while ( rio->ready_count > 0 )
    {
        const int candidate = rio->ready_banks[rio->ready_head];
        if ( total + rio->banks[candidate].num_packets > rio->process.capacity )
            break; // the oldest ready bank waits for the next call
        rio->ready_head = ( rio->ready_head + 1 ) % ROCKETNET_RIO_RECEIVE_BANKS;
        rio->ready_count--;
        rio->exposed_banks[rio->exposed_count++] = candidate;
        total += rio->banks[candidate].num_packets;
    }

    rocketnet_platform_mutex_release( &rio->mutex );
    if ( wake )
        SetEvent( rio->wake );
    for ( int j = 0; j < rio->exposed_count; ++j )
    {
        rocketnet_rio_bank_t * bank = &rio->banks[rio->exposed_banks[j]];
        for ( int i = 0; i < bank->num_packets; ++i )
        {
            const int index = rio->process.num_packets++;
            rio->process.from[index] = bank->from[i];
            rio->process.packet_bytes[index] = bank->packet_bytes[i];
            rio->process.packet_data[index] = bank->data[i];
        }
    }
}

#if ROCKETNET_DEV
// The unit fixtures are the default configuration: its send queue and
// receive slices, as constants for the fake kernels' fixed arrays.
#define ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_DEFAULT_MAX_CLIENTS )
#define ROCKETNET_RIO_TEST_RECEIVES ( 2 * ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE )

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

static ULONG WINAPI rocketnet_rio_overflow_test_dequeue( RIO_CQ cq, PRIORESULT results, ULONG max_results )
{
    if ( !rocketnet_rio_overflow_test_state || cq != (RIO_CQ) (uintptr_t) 1 )
        return RIO_CORRUPT_CQ;
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
        return FALSE;
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
        memset( block, 0, bytes );
    return block;
}

static rocketnet_server_rio_t * rocketnet_rio_test_fixture()
{
    void * storage = rocketnet_rio_test_zeroed( sizeof( rocketnet_server_rio_t ) );
    if ( !storage )
        return NULL;
    rocketnet_server_rio_t * rio = new ( storage ) rocketnet_server_rio_t();
    rocketnet_rio_sized_layout_t sized;
    bool ok = rocketnet_rio_sized_layout( ROCKETNET_DEFAULT_MAX_CLIENTS, &sized );
    uint8_t * base = ok ? (uint8_t *) rocketnet_rio_test_zeroed( (size_t) sized.bytes ) : NULL;
    if ( base )
        rocketnet_rio_sized_bind( rio, base, &sized );
    rocketnet_assert( !base || ( rio->send_queue_size == ROCKETNET_RIO_TEST_SEND_QUEUE_SIZE && rio->receives == ROCKETNET_RIO_TEST_RECEIVES ) );
    ok = ok && base && rocketnet_rio_layout_plan( rio->send_frames, rio->receives, ROCKETNET_FRAME_STRIDE, ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof( SOCKADDR_INET ), 4096, &rio->layout );
    rio->region = ok ? (uint8_t *) rocketnet_rio_test_zeroed( (size_t) rio->layout.total_bytes ) : NULL;
    ok = ok && rio->region && rocketnet_platform_mutex_create( &rio->mutex );
    rio->wake = ok ? CreateEventW( NULL, FALSE, FALSE, NULL ) : NULL;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rio->wake )
        rio->wake_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_EVENT, (uintptr_t) rio->wake,
                                                               "rio unit fixture wake event" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !ok || !rio->wake )
    {
        if ( rio->mutex.ok )
            rocketnet_platform_mutex_destroy( &rio->mutex );
        rocketnet_free( NULL, rio->sized_storage );
        rocketnet_free( NULL, rio->region );
        rio->~rocketnet_server_rio_t();
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

static void rocketnet_rio_test_fixture_free( rocketnet_server_rio_t * rio )
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
    rio->~rocketnet_server_rio_t();
    rocketnet_free( NULL, rio );
}

// A test-only fixture drives the production worker pass and receive_packets
// with an endlessly full fake queue. Its dequeue guard makes an unbounded
// loop return a corrupt-CQ sentinel; the bounded pass returns before it.
bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                 int * repost_calls, int * pending_receives, int * guard_hit )
{
    if ( !counters || !packet_counts || !repost_calls || !pending_receives || !guard_hit )
        return false;
    rocketnet_server_rio_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
        return false;

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
    rocketnet_server_rio_receive_packets( rio );
    packet_counts[0] = rio->process.num_packets;
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i )
        counters[i] = rio->counters[i].load( std::memory_order_relaxed );

    // The queue stays full through the guard. A bounded pass fills one bank
    // and leaves the still-full queue for the next pass.

    rocketnet_rio_worker_pass( rio );
    rocketnet_server_rio_receive_packets( rio );
    packet_counts[1] = rio->process.num_packets;
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i )
        counters[ROCKETNET_RIO_NUM_COUNTERS + i] = rio->counters[i].load( std::memory_order_relaxed );
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
        return 0;
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
            state->completed_context[state->completed++] = state->deferred_context[--state->deferred];
        return TRUE;
    }

    state->deferred_context[state->deferred++] = (ULONGLONG) (uintptr_t) context;

    return TRUE;
}

static bool rocketnet_rio_test_stage( rocketnet_server_rio_t * rio, int count )
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
        uint8_t * frame = rocketnet_server_rio_start_packet( rio, &to, &id );
        if ( !frame )
            return false;
        rocketnet_server_rio_finish_packet( rio, id, frame, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }

    return true;
}

static bool rocketnet_rio_test_woken( rocketnet_server_rio_t * rio )
{
    return WaitForSingleObject( rio->wake, 0 ) == WAIT_OBJECT_0;
}

// The RNRioWorker transitions, forced in order on one thread with no socket
// and no clock: receive backpressure with every bank full, a failed main try
// that retains its exposed banks, the wake on a return to an empty free list,
// and the send phase handoff that reopens a phase only when it is idle.
bool rocketnet_server_rio_test_worker_handoff()
{
    rocketnet_server_rio_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
        return false;
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
        valid &= rocketnet_rio_worker_pass( rio );
    valid &= rio->ready_count == ROCKETNET_RIO_RECEIVE_BANKS - 1 && rio->free_count == 0 &&
             rio->banks[rio->fill_bank].num_packets == rio->receives;
    const int dequeues = state.receive_dequeues;
    valid &= !rocketnet_rio_worker_pass( rio ) && state.receive_dequeues == dequeues;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW].load() == 0;
    valid &= rio->posted_receives.load() == rio->receives && state.reposts == ROCKETNET_RIO_RECEIVE_BANKS * rio->receives;

    // Main exposes the ready banks that fit the process view, oldest first.

    const int fit = rio->process.capacity / rio->receives < ROCKETNET_RIO_RECEIVE_BANKS - 1
                        ? rio->process.capacity / rio->receives
                        : ROCKETNET_RIO_RECEIVE_BANKS - 1;
    const int oldest = rio->ready_banks[rio->ready_head];
    rocketnet_server_rio_receive_packets( rio );
    valid &= rio->exposed_count == fit && rio->exposed_banks[0] == oldest &&
             rio->process.num_packets == fit * rio->receives &&
             rio->process.packet_data[0] == rio->banks[oldest].data[0] && !rocketnet_rio_test_woken( rio );

    // A failed try exposes nothing new and keeps the exposed banks main's.

    rio->test_try_fails = true;
    rocketnet_server_rio_receive_packets( rio );
    valid &= rio->process.num_packets == 0 && rio->exposed_count == fit && rio->free_count == 0;
    rio->test_try_fails = false;

    // Returning banks to an empty free list wakes the worker. Its next pass
    // publishes the full bank, and the pass after fills a free one.

    rocketnet_server_rio_receive_packets( rio );
    valid &= rocketnet_rio_test_woken( rio );
    rocketnet_rio_worker_pass( rio );
    valid &= rio->ready_count == 1 && state.receive_dequeues == dequeues;
    valid &= rocketnet_rio_worker_pass( rio ) && state.receive_dequeues > dequeues;
    state.receive_paused = true;

    for ( int call = 0; call < ROCKETNET_RIO_RECEIVE_BANKS; ++call )
    {
        rocketnet_rio_worker_pass( rio );
        rocketnet_server_rio_receive_packets( rio );
    }

    rocketnet_server_rio_receive_packets( rio );
    valid &= rio->ready_count == 0 && rio->exposed_count == 0 && rio->process.num_packets == 0;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED].load() ==
             (uint64_t) ( ROCKETNET_RIO_RECEIVE_BANKS + 1 ) * (uint64_t) rio->receives;

    // Send: each flush hands the fill phase and opens the next, idle phase.

    valid &= rocketnet_rio_test_stage( rio, 2 );
    rocketnet_server_rio_send_packets( rio );
    valid &= rio->fill.load() == 1 && rocketnet_rio_test_woken( rio );
    valid &= rocketnet_rio_test_stage( rio, 3 );
    rocketnet_server_rio_send_packets( rio );
    valid &= rio->fill.load() == 2 && rio->send_ready_count == 2;

    // Phase 0, the next phase, is still the worker's: the flush keeps phase 2
    // open and the fill threads keep appending to it, nothing refused.

    valid &= rocketnet_rio_test_stage( rio, 1 );
    rocketnet_server_rio_send_packets( rio );
    valid &= rio->fill.load() == 2 && rio->next[2].load() == 1 &&
             rio->counters[ROCKETNET_RIO_COUNTER_SEND_WAITS].load() == 1;
    valid &= rocketnet_rio_test_stage( rio, 1 );

    // A failed try keeps the fill phase too.

    rio->test_try_fails = true;
    rocketnet_server_rio_send_packets( rio );
    rio->test_try_fails = false;
    valid &= rio->fill.load() == 2 && rio->next[2].load() == 2;

    // The worker takes the oldest handed phase, 0, posts and commits it, and
    // returns it idle once its completions are retired. Phase 1 waits handed.

    valid &= rocketnet_rio_worker_pass( rio ) && state.commits == 1 &&
             rio->phase_state[0] == ROCKETNET_RIO_PHASE_IDLE && rio->phase_state[1] == ROCKETNET_RIO_PHASE_READY;
    // Now the flush hands phase 2 with both of its packets and opens phase 0.

    rocketnet_server_rio_send_packets( rio );
    valid &= rio->fill.load() == 0 && rio->phase_count[2] == 2 && rocketnet_rio_test_stage( rio, 1 );
    valid &= rocketnet_rio_worker_pass( rio ) && state.commits == 2 && rio->phase_state[1] == ROCKETNET_RIO_PHASE_IDLE;
    valid &= rocketnet_rio_worker_pass( rio ) && state.commits == 3 && rio->phase_state[2] == ROCKETNET_RIO_PHASE_IDLE;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT].load() == 7 && rocketnet_rio_outstanding_sends( rio ) == 0 &&
             rio->phase_state[0] == ROCKETNET_RIO_PHASE_FILL && rio->counters[ROCKETNET_RIO_COUNTER_SEND_FULL].load() == 0;

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
            state->in_flight_context[state->in_flight++] = state->deferred_context[--state->deferred];
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
    rocketnet_rio_stop_test_complete_in_flight( (LONG) WSA_OPERATION_ABORTED );
    state->cancelled_receives = ROCKETNET_RIO_TEST_RECEIVES;
    return 0;
}

// proofs/tla/RNRioWorker.tla WClose under DrainSendsBeforeClose, stepped on one thread
// with a fake kernel and no socket, worker thread or clock: the stop pass
// posts and commits the staged fill phase, keeps the socket open while the
// committed sends are in flight, and closes it only after every send
// completed, so the stop burst retires delivered and none is cancelled.
bool rocketnet_server_rio_test_stop_drains_sends()
{
    rocketnet_server_rio_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
        return false;
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
    rocketnet_server_rio_stop( rio );

    // The pass posts and commits the burst; the sends are in flight, so the
    // socket stays open, on this pass and the next.

    valid &= !rocketnet_rio_stop_pass( rio ) && state.in_flight == burst && rocketnet_rio_outstanding_sends( rio ) == burst;
    valid &= !rocketnet_rio_stop_pass( rio ) && state.closes == 0 && rio->socket != INVALID_SOCKET;

    // The sends complete. The next pass retires them, closes the socket and
    // drains the cancelled receives.

    rocketnet_rio_stop_test_complete_in_flight( 0 );
    bool stopped = false;
    for ( int pass = 0; pass <= rio->receives / ROCKETNET_RIO_DEQUEUE_BATCH + 1 && !stopped; ++pass )
        stopped = rocketnet_rio_stop_pass( rio );
    valid &= stopped && state.closes == 1 && rio->socket == INVALID_SOCKET;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT].load() == (uint64_t) burst &&
             rio->counters[ROCKETNET_RIO_COUNTER_SEND_FAILED].load() == 0 &&
             rocketnet_rio_outstanding_sends( rio ) == 0 && rio->posted_receives.load() == 0;

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
    rocketnet_server_rio_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
        return false;
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
        uint8_t * frame = rocketnet_server_rio_start_packet( rio, &to, &id );
        valid &= frame != NULL;
        if ( frame )
            rocketnet_server_rio_finish_packet( rio, id, frame, lengths[i] );
        valid &= rio->counters[ROCKETNET_RIO_COUNTER_SEND_FAILED].load() == (uint64_t) ( i + 1 );
    }
    rn_assert_function = saved_assert;

    // Three good packets beside them, and a commit refused on the next four
    // tries. The phase posts three sends and the worker retries the commit
    // until it is taken; nothing is counted until the sends complete.

    valid &= rocketnet_rio_test_stage( rio, 3 );
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, 4, WSAENOBUFS );
    rocketnet_server_rio_send_packets( rio );

    for ( int pass = 0; pass < 8 && rocketnet_server_rio_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT ) > 0; ++pass )
    {
        rocketnet_rio_worker_pass( rio );
        valid &= rio->uncommitted && state.commits == 0;
        valid &= rio->counters[ROCKETNET_RIO_COUNTER_SEND_FAILED].load() == 2;
    }

    valid &= rocketnet_server_rio_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT ) == 0;
    for ( int pass = 0; pass < 4 && rocketnet_rio_outstanding_sends( rio ) > 0; ++pass )
        rocketnet_rio_worker_pass( rio );

    valid &= state.commits == 1 && !rio->uncommitted && rocketnet_rio_outstanding_sends( rio ) == 0;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_COMMITS].load() == 1;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT].load() == 3;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_SEND_FAILED].load() == 2;
    valid &= rocketnet_rio_accounting_test_asserts == ( ROCKETNET_ASSERTS ? 2 : 0 );    // each refused finish asserts where asserts are compiled in

    rocketnet_rio_handoff_test_state = NULL;
    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

#endif // #if ROCKETNET_DEV

// proofs/tla/RNRioDriver.tla TransportReads: the views of the exposed bank
rocketnet_server_socket_process_packets_t * rocketnet_server_rio_process_packets( rocketnet_server_rio_t * rio )
{
    return &rio->process;
}

void rocketnet_server_rio_counters( rocketnet_server_rio_t * rio, uint64_t * counters )
{
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i )
        counters[i] = rio->counters[i].load( std::memory_order_relaxed );
}

int rocketnet_server_rio_pending( rocketnet_server_rio_t * rio )
{
    return rocketnet_rio_outstanding_sends( rio ) + rio->posted_receives.load( std::memory_order_relaxed );
}

#if ROCKETNET_DEV
uintptr_t rocketnet_server_rio_test_socket( rocketnet_server_rio_t * rio )
{
    return (uintptr_t) rio->socket;
}

void rocketnet_server_rio_test_fail_next( rocketnet_server_rio_t * rio, int fault, int count, int status )
{
    rocketnet_assert( fault >= 0 && fault < ROCKETNET_RIO_TEST_NUM_FAULTS && count >= 0 );
    rio->test_fault_status[fault].store( status, std::memory_order_relaxed );
    rio->test_faults[fault].store( count, std::memory_order_release );
}

int rocketnet_server_rio_test_faults_left( rocketnet_server_rio_t * rio, int fault )
{
    return rio->test_faults[fault].load( std::memory_order_acquire );
}

void rocketnet_server_rio_test_hold_send( rocketnet_server_rio_t * rio, bool hold )
{
    rocketnet_platform_mutex_acquire( &rio->mutex );
    rio->test_hold_send = hold;
    rocketnet_platform_mutex_release( &rio->mutex );
    if ( !hold )
        SetEvent( rio->wake );
}

int rocketnet_server_rio_test_handed_phases( rocketnet_server_rio_t * rio )
{
    rocketnet_platform_mutex_acquire( &rio->mutex );

    const int count = rio->send_ready_count;
    rocketnet_platform_mutex_release( &rio->mutex );
    return count;
}

bool rocketnet_server_rio_test_send_idle( rocketnet_server_rio_t * rio )
{
    rocketnet_platform_mutex_acquire( &rio->mutex );

    const int phase = rio->fill.load( std::memory_order_relaxed );
    bool idle = rio->send_ready_count == 0 && rio->next[phase].load( std::memory_order_relaxed ) == 0;
    for ( int p = 0; p < ROCKETNET_RIO_SEND_PHASES; ++p )
        idle = idle && rio->phase_state[p] != ROCKETNET_RIO_PHASE_POSTED;
    rocketnet_platform_mutex_release( &rio->mutex );

    return idle;
}

int rocketnet_server_rio_test_retry_receives( rocketnet_server_rio_t * rio )
{
    return rio->num_retry.load( std::memory_order_relaxed );
}
// A fill phase that has taken 2^32 - 1 reservations without a flush refuses
// the next two as full: the reservation count does not wrap back onto a slot
// it handed out.
bool rocketnet_server_rio_test_reservation_wrap()
{
    rocketnet_server_rio_t * rio = rocketnet_rio_test_fixture();
    if ( !rio )
        return false;

    rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40001;

    typedef decltype( rio->next[0].load() ) rocketnet_rio_test_count_t;
    const int phase = rio->fill.load( std::memory_order_relaxed );
    bool valid = rocketnet_server_rio_send_room( rio ) == rio->send_queue_size;
    rio->next[phase].store( (uint64_t) rio->send_queue_size - 1, std::memory_order_relaxed );
    valid &= rocketnet_server_rio_send_room( rio ) == 1;
    rio->next[phase].store( (uint64_t) rio->send_queue_size, std::memory_order_relaxed );
    valid &= rocketnet_server_rio_send_room( rio ) == 0;
    rio->next[phase].store( (rocketnet_rio_test_count_t) 0xFFFFFFFFull, std::memory_order_relaxed );
    valid &= rocketnet_server_rio_send_room( rio ) == 0;

    uint64_t id = 0;
    valid &= rocketnet_server_rio_start_packet( rio, &to, &id ) == NULL;
    valid &= rocketnet_server_rio_send_room( rio ) == 0;
    valid &= rocketnet_server_rio_start_packet( rio, &to, &id ) == NULL;
    valid &= rocketnet_server_rio_send_room( rio ) == 0;
    valid &= rio->counters[ROCKETNET_RIO_COUNTER_SEND_FULL].load() == 2;

    rocketnet_rio_test_fixture_free( rio );

    return valid;
}

#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

int rocketnet_server_rio_windows_cpp_dummy = 0;
