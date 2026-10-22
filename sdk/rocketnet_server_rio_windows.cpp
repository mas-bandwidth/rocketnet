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
#endif

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

// The model: tla/RNRioDriver.tla. Each function that implements one of its
// actions names it.

#define ROCKETNET_RIO_SEND_FRAMES ( 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE )
#define ROCKETNET_RIO_DEQUEUE_BATCH 256

// regions destroy could not drain and so leaked rather than freed
static std::atomic<uint64_t> rocketnet_rio_leaked_regions{0};

static_assert( ROCKETNET_RIO_RECEIVE_SLICE_BYTES >= ROCKETNET_RECEIVE_CEILING + 1, "receive detects oversize datagrams" );
static_assert( ROCKETNET_RIO_RECEIVES <= ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS, "a whole bank fits the process view" );

// a receive bank: the model's fillBank and exposed. Its capacity is the
// number of slices (BankCapacity == Slices). data leads, so its
// 16-alignment needs no padding before it.
struct rocketnet_rio_bank_t
{
    alignas(16) uint8_t data[ROCKETNET_RIO_RECEIVES][ROCKETNET_FRAME_STRIDE];
    int num_packets;
    rocketnet_address_t from[ROCKETNET_RIO_RECEIVES];
    int packet_bytes[ROCKETNET_RIO_RECEIVES];
};

struct rocketnet_server_rio_t
{
    // next and outstanding lead, each starting a cache line (next's line
    // padded out explicitly), so the compiler inserts no alignment padding.
    // next is read by fill threads, which run only while the main thread is
    // outside send_packets.
    alignas(64) std::atomic<int> next[2];
    uint8_t next_padding[64 - 2 * sizeof( std::atomic<int> )];
    alignas(64) int outstanding[2] = { 0, 0 };

    void * context;
    SOCKET socket = INVALID_SOCKET;
    bool ipv6 = false;
    RIO_EXTENSION_FUNCTION_TABLE rio;
    bool have_table = false;

    uint8_t * region = NULL;
    rocketnet_rio_layout_t layout;
    RIO_BUFFERID buffer_id = RIO_INVALID_BUFFERID;
    RIO_CQ send_cq = RIO_INVALID_CQ;
    RIO_CQ receive_cq = RIO_INVALID_CQ;
    RIO_RQ request_queue = RIO_INVALID_RQ;

    std::atomic<bool> stopping{false};
    // the model's sending: true from Post until SwitchPhase; the fill phase
    // is closed to fill threads while it holds
    std::atomic<bool> switch_owed{false};
    // deferred sends posted and not yet committed
    bool uncommitted = false;

    // send: the model's fill, next, outstanding and the slot bytes. fill and
    // next are read by fill threads, which run only while the main thread is
    // outside send_packets.
    std::atomic<int> fill{0};
    int * slot_bytes = NULL;                    // [ROCKETNET_RIO_SEND_FRAMES], zero for a free, reserved or aborted slot

    // receive: the model's slice states as one count of posted slices, and
    // the two banks. The fill bank is banks[fill_bank]; the other is exposed.
    int posted_receives = 0;
    rocketnet_rio_bank_t * banks = NULL;        // [2]
    int fill_bank = 0;
    // slices whose re-post was refused (a full request queue, a transient
    // Winsock refusal): neither posted nor lost, re-posted by the next
    // receive_packets. A slice is posted, completed, or here.
    int retry_slices[ROCKETNET_RIO_RECEIVES];
    int num_retry = 0;

    std::atomic<uint64_t> counters[ROCKETNET_RIO_NUM_COUNTERS]{};
    rocketnet_server_socket_process_packets_t process;
#if defined(ROCKETNET_RIO_TESTS)
    int test_faults[ROCKETNET_RIO_TEST_NUM_FAULTS] = {};
    int test_fault_status[ROCKETNET_RIO_TEST_NUM_FAULTS] = {};
#endif
};

static_assert( offsetof( rocketnet_server_rio_t, outstanding ) == 64, "the rio's aligned members have no implicit padding" );

// The fault seam (rocketnet_server_rio_test_fail_next) exists only in the
// test executable. Elsewhere the check is the constant false, so the
// production paths carry no test branch.
#if defined(ROCKETNET_RIO_TESTS)
static bool rocketnet_rio_test_fault( rocketnet_server_rio_t * rio, int fault, int * status )
{
    if ( rio->test_faults[fault] <= 0 )
        return false;
    rio->test_faults[fault]--;
    *status = rio->test_fault_status[fault];
    WSASetLastError( *status );
    return true;
}
#define ROCKETNET_RIO_TEST_FAULT( rio, fault, status ) rocketnet_rio_test_fault( rio, fault, status )
#else
#define ROCKETNET_RIO_TEST_FAULT( rio, fault, status ) ( (void) (status), false )
#endif

#if defined(ROCKETNET_RIO_OVERFLOW_TEST_HOOKS) || defined(ROCKETNET_RIO_TESTS)

// This table exists only in test builds, compiled into the library like the
// filter-epoch seam. The receive path substitutes only dequeue and receive
// repost; production builds call the RIO function table directly with no test
// branch.
typedef ULONG (WINAPI *rocketnet_rio_test_dequeue_fn)( RIO_CQ, PRIORESULT, ULONG );
typedef BOOL (WINAPI *rocketnet_rio_test_receive_fn)( RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF,
                                                      PRIO_BUF, PRIO_BUF, DWORD, PVOID );

struct rocketnet_rio_test_function_table_t
{
    rocketnet_rio_test_dequeue_fn dequeue;
    rocketnet_rio_test_receive_fn receive;
};

static const rocketnet_rio_test_function_table_t * rocketnet_rio_test_functions = NULL;

#endif // defined(ROCKETNET_RIO_OVERFLOW_TEST_HOOKS) || defined(ROCKETNET_RIO_TESTS)

static void rocketnet_rio_count( rocketnet_server_rio_t * rio, int counter, uint64_t amount = 1 )
{
    rio->counters[counter].fetch_add( amount, std::memory_order_relaxed );
}

// a destination as the SOCKADDR_INET the socket's family needs: a dual
// stack socket sends IPv4 as v4-mapped IPv6
static bool rocketnet_rio_address_write( const rocketnet_address_t * input, bool ipv6, SOCKADDR_INET * out )
{
    rocketnet_address_t address = *input;
    memset( out, 0, sizeof(*out) );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
        rocketnet_address_convert_ipv4_to_ipv6( &address );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV6 )
    {
        out->Ipv6.sin6_family = AF_INET6;
        out->Ipv6.sin6_port = htons( address.port );
        for ( int i = 0; i < 8; ++i )
        {
            out->Ipv6.sin6_addr.s6_addr[i * 2] = (uint8_t) ( address.data.ipv6[i] >> 8 );
            out->Ipv6.sin6_addr.s6_addr[i * 2 + 1] = (uint8_t) address.data.ipv6[i];
        }
        return true;
    }
    if ( !ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        out->Ipv4.sin_family = AF_INET;
        out->Ipv4.sin_port = htons( address.port );
        memcpy( &out->Ipv4.sin_addr, address.data.ipv4, 4 );
        return true;
    }
    return false;
}

// a source from its SOCKADDR_INET; an IPv4-mapped IPv6 source passes the one
// receive rule, rocketnet_address_accept_received_source: canonicalised to
// IPv4 on the dual-stack socket, refused on any other
static bool rocketnet_rio_address_read( const SOCKADDR_INET * in, bool dual_stack, rocketnet_address_t * address )
{
    memset( address, 0, sizeof(*address) );
    if ( in->si_family == AF_INET )
    {
        address->type = ROCKETNET_ADDRESS_IPV4;
        address->port = ntohs( in->Ipv4.sin_port );
        memcpy( address->data.ipv4, &in->Ipv4.sin_addr, 4 );
        return true;
    }
    if ( in->si_family == AF_INET6 )
    {
        address->type = ROCKETNET_ADDRESS_IPV6;
        address->port = ntohs( in->Ipv6.sin6_port );
        for ( int i = 0; i < 8; ++i )
            address->data.ipv6[i] = (uint16_t) ( ( in->Ipv6.sin6_addr.s6_addr[i * 2] << 8 ) | in->Ipv6.sin6_addr.s6_addr[i * 2 + 1] );
        return rocketnet_address_accept_received_source( address, dual_stack );
    }
    return false;
}

// a send frame index is phase * ROCKETNET_RIO_SEND_QUEUE_SIZE + slot
static uint8_t * rocketnet_rio_frame( rocketnet_server_rio_t * rio, int index )
{
    return rio->region + rocketnet_rio_send_frame_offset( &rio->layout, index );
}

static SOCKADDR_INET * rocketnet_rio_send_address( rocketnet_server_rio_t * rio, int index )
{
    return (SOCKADDR_INET*) ( rio->region + rocketnet_rio_send_address_offset( &rio->layout, index ) );
}

static const SOCKADDR_INET * rocketnet_rio_receive_address( rocketnet_server_rio_t * rio, int slice )
{
    return (const SOCKADDR_INET*) ( rio->region + rocketnet_rio_receive_address_offset( &rio->layout, slice ) );
}

// one RIO_MSG_COMMIT_ONLY when deferred sends are uncommitted; a refused
// commit leaves them uncommitted for the next try
static void rocketnet_rio_commit( rocketnet_server_rio_t * rio )
{
    if ( !rio->uncommitted )
        return;
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, &injected ) ||
         !rio->rio.RIOSendEx( rio->request_queue, NULL, 0, NULL, NULL, NULL, NULL, RIO_MSG_COMMIT_ONLY, NULL ) )
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
    if ( offset % stride != 0 || offset / stride >= (uint64_t) ROCKETNET_RIO_SEND_FRAMES )
        return -1;
    // only a reserved slot of the open fill phase: a stale frame of a phase
    // the kernel owns is refused, never rewritten under a posted send
    const int index = (int) ( offset / stride );
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const int slot = index - phase * ROCKETNET_RIO_SEND_QUEUE_SIZE;
    if ( rio->switch_owed.load( std::memory_order_relaxed ) || slot < 0 || slot >= ROCKETNET_RIO_SEND_QUEUE_SIZE ||
         slot >= rio->next[phase].load( std::memory_order_relaxed ) )
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
    address.Length = sizeof(SOCKADDR_INET);
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST, &injected ) )
        return false;
#if defined(ROCKETNET_RIO_OVERFLOW_TEST_HOOKS) || defined(ROCKETNET_RIO_TESTS)
    if ( rocketnet_rio_test_functions )
    {
        if ( !rocketnet_rio_test_functions->receive( rio->request_queue, &data, 1, NULL, &address, NULL, NULL, 0,
                (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) slice ) ) )
            return false;
    }
    else
#endif
    if ( !rio->rio.RIOReceiveEx( rio->request_queue, &data, 1, NULL, &address, NULL, NULL, 0,
                                 (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) slice ) ) )
    {
        // a post refused with an ICMP report of an earlier send is counted
        // with the reset statuses; a refused re-post waits on the retry list in
        // rocketnet_rio_complete_receive (windows-udp-connreset audit, rio post)
        if ( rocketnet_platform_receive_status_is_reset( WSAGetLastError() ) )
            rocketnet_platform_count_receive_reset();
        return false;
    }
    rio->posted_receives++;
    return true;
}

// RNRioDriver RetireSend: one send completion; the slot is the transport's
// again. A failed status counts SEND_FAILED.
static void rocketnet_rio_retire_send( rocketnet_server_rio_t * rio, const RIORESULT * result )
{
    const uint64_t context = (uint64_t) result->RequestContext;
    const int phase = rocketnet_rio_context_phase( context );
    const int slot = rocketnet_rio_context_index( context );
    if ( rocketnet_rio_context_kind( context ) != ROCKETNET_RIO_CONTEXT_SEND || phase < 0 || phase > 1 ||
         slot < 0 || slot >= ROCKETNET_RIO_SEND_QUEUE_SIZE )
    {
        rocketnet_error( "rio transport: send completion with a foreign request context %llx", (unsigned long long) context );
        return;
    }
    const int index = phase * ROCKETNET_RIO_SEND_QUEUE_SIZE + slot;
    LONG status = result->Status;
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_SEND_COMPLETION, &injected ) )
        status = (LONG) injected;
    const bool sent = status == 0 && result->BytesTransferred == (ULONG) rio->slot_bytes[index];
    rocketnet_rio_count( rio, sent ? ROCKETNET_RIO_COUNTER_PACKETS_SENT : ROCKETNET_RIO_COUNTER_SEND_FAILED );
    rio->slot_bytes[index] = 0;
    rio->outstanding[phase]--;
}

// RNRioDriver DequeueSend (and Drain's send half): dequeues the send
// completion queue without blocking. Returns the completions retired.
static int rocketnet_rio_dequeue_send( rocketnet_server_rio_t * rio )
{
    int total = 0;
    for ( ;; )
    {
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rio->rio.RIODequeueCompletion( rio->send_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
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

// A deferred send still occupies request-queue capacity after a refused
// commit. Keep the socket open so a later completion check can retry the
// commit before closing the request queue that owns those sends.
static bool rocketnet_rio_close_socket( rocketnet_server_rio_t * rio )
{
    if ( rio->request_queue != RIO_INVALID_RQ && rio->uncommitted )
    {
        rocketnet_rio_commit( rio );
        if ( rio->uncommitted ) return false;
    }
    if ( rio->socket != INVALID_SOCKET )
    {
        if ( closesocket( rio->socket ) != 0 ) return false;
        rio->socket = INVALID_SOCKET;
    }
    return true;
}

void rocketnet_server_rio_stop( rocketnet_server_rio_t * rio )
{
    if ( !rio || rio->stopping.exchange( true, std::memory_order_acq_rel ) ) return;
    // Closing a UDP socket cancels pending RIO requests. Keep its registered
    // region and queues until their completions have all been observed.
    rocketnet_rio_close_socket( rio );
}

bool rocketnet_server_rio_stopped( rocketnet_server_rio_t * rio )
{
    if ( !rio || !rio->stopping.load( std::memory_order_acquire ) ) return false;
    if ( !rocketnet_rio_close_socket( rio ) ) return false;
    // Exactly one available batch per queue, never wait for new completions.
    if ( rio->have_table && rio->send_cq != RIO_INVALID_CQ )
    {
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rio->rio.RIODequeueCompletion( rio->send_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
        if ( count == RIO_CORRUPT_CQ ) return false;
        for ( ULONG i = 0; i < count; ++i ) rocketnet_rio_retire_send( rio, &results[i] );
    }
    if ( rio->have_table && rio->receive_cq != RIO_INVALID_CQ )
    {
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
        const ULONG count = rio->rio.RIODequeueCompletion( rio->receive_cq, results, ROCKETNET_RIO_DEQUEUE_BATCH );
        if ( count == RIO_CORRUPT_CQ ) return false;
        rio->posted_receives -= (int) count;
    }
    return rio->outstanding[0] == 0 && rio->outstanding[1] == 0 && rio->posted_receives == 0;
}

void rocketnet_server_rio_destroy( rocketnet_server_rio_t * rio )
{
    if ( !rio ) return;
    const bool stopped = rocketnet_server_rio_stopped( rio );
    rocketnet_assert( stopped );
    if ( !stopped ) return;
    void * context = rio->context;
    if ( rio->send_cq != RIO_INVALID_CQ ) rio->rio.RIOCloseCompletionQueue( rio->send_cq );
    if ( rio->receive_cq != RIO_INVALID_CQ ) rio->rio.RIOCloseCompletionQueue( rio->receive_cq );
    if ( rio->buffer_id != RIO_INVALID_BUFFERID ) rio->rio.RIODeregisterBuffer( rio->buffer_id );
    if ( rio->region ) VirtualFree( rio->region, 0, MEM_RELEASE );
    rocketnet_free( context, rio->slot_bytes );
    rocketnet_free( context, rio->banks );
    rio->~rocketnet_server_rio_t();
    rocketnet_free( context, rio );
}

uint64_t rocketnet_server_rio_leaked_regions()
{
    return rocketnet_rio_leaked_regions.load( std::memory_order_relaxed );
}

uint64_t rocketnet_server_rio_allocation_bytes()
{
    rocketnet_rio_layout_t layout;
    if ( !rocketnet_rio_layout_plan( ROCKETNET_RIO_SEND_FRAMES, ROCKETNET_RIO_RECEIVES, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof(SOCKADDR_INET), 4096, &layout ) )
        return 0;
    return rocketnet_memory_allocations( { sizeof(rocketnet_server_rio_t),
        (uint64_t) ROCKETNET_RIO_SEND_FRAMES * sizeof(int),
        2 * (uint64_t) sizeof(rocketnet_rio_bank_t) } );
}

uint64_t rocketnet_server_rio_registered_region_bytes()
{
    rocketnet_rio_layout_t layout;
    if ( !rocketnet_rio_layout_plan( ROCKETNET_RIO_SEND_FRAMES, ROCKETNET_RIO_RECEIVES, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof(SOCKADDR_INET), 4096, &layout ) )
        return 0;
    return layout.total_bytes;
}

static bool rocketnet_rio_load_table( SOCKET socket, RIO_EXTENSION_FUNCTION_TABLE * table )
{
    GUID guid = WSAID_MULTIPLE_RIO;
    DWORD returned_bytes = 0;
    memset( table, 0, sizeof(*table) );
    table->cbSize = sizeof(*table);
    return WSAIoctl( socket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), table, sizeof(*table),
                     &returned_bytes, NULL, NULL ) != SOCKET_ERROR &&
           table->RIOReceiveEx && table->RIOSendEx && table->RIODequeueCompletion;
}

bool rocketnet_server_rio_available()
{
    SOCKET socket = WSASocketW( AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_REGISTERED_IO );
    if ( socket == INVALID_SOCKET )
        return false;
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) socket );
    RIO_EXTENSION_FUNCTION_TABLE table;
    const bool available = rocketnet_rio_load_table( socket, &table );
    closesocket( socket );
    return available;
}

rocketnet_server_rio_t * rocketnet_server_rio_create( void * context, rocketnet_address_t * bind_address, int fail_stage )
{
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) ) return NULL;
    void * storage = rocketnet_malloc( context, sizeof(rocketnet_server_rio_t) );
    if ( !storage ) return NULL;
    rocketnet_server_rio_t * rio = new (storage) rocketnet_server_rio_t();
    rio->context = context;
    rio->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    rio->next[0].store( 0, std::memory_order_relaxed );
    rio->next[1].store( 0, std::memory_order_relaxed );

    SOCKADDR_INET address;
    int address_bytes = 0;
    int buffer_bytes = ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE;
    BOOL enabled = TRUE;
    DWORD v6_only = FALSE;

    rio->slot_bytes = (int*) rocketnet_malloc( context, sizeof(int) * ROCKETNET_RIO_SEND_FRAMES );
    if ( !rio->slot_bytes ) goto failed;
    memset( rio->slot_bytes, 0, sizeof(int) * ROCKETNET_RIO_SEND_FRAMES );
    rio->banks = (rocketnet_rio_bank_t*) rocketnet_malloc( context, 2 * sizeof(rocketnet_rio_bank_t) );
    if ( !rio->banks ) goto failed;
    rio->banks[0].num_packets = 0;
    rio->banks[1].num_packets = 0;
    rio->process.num_packets = 0;

    if ( !rocketnet_rio_address_write( bind_address, rio->ipv6, &address ) ) goto failed;
    address_bytes = rio->ipv6 ? (int) sizeof(sockaddr_in6) : (int) sizeof(sockaddr_in);

    rio->socket = WSASocketW( rio->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_REGISTERED_IO );
    if ( rio->socket == INVALID_SOCKET || fail_stage == 1 ) goto failed;
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &enabled, sizeof(enabled) ) == SOCKET_ERROR ) goto failed;
    if ( rio->ipv6 && setsockopt( rio->socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &v6_only, sizeof(v6_only) ) == SOCKET_ERROR ) goto failed;
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_RCVBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    buffer_bytes = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    if ( setsockopt( rio->socket, SOL_SOCKET, SO_SNDBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    // windows-udp-connreset audit, rio socket: both ICMP reset reports off;
    // a refusal is logged once and the receive path skips the statuses
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) rio->socket );
    if ( bind( rio->socket, (sockaddr*) &address, address_bytes ) == SOCKET_ERROR ) goto failed;
    address_bytes = sizeof(address);
    if ( getsockname( rio->socket, (sockaddr*) &address, &address_bytes ) == SOCKET_ERROR ) goto failed;

    if ( !rocketnet_rio_load_table( rio->socket, &rio->rio ) ) goto failed;
    rio->have_table = true;
    if ( fail_stage == 2 ) goto failed;

    // one region, carved only by the layout, registered once
    if ( !rocketnet_rio_layout_plan( ROCKETNET_RIO_SEND_FRAMES, ROCKETNET_RIO_RECEIVES, ROCKETNET_FRAME_STRIDE,
                                     ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof(SOCKADDR_INET), 4096, &rio->layout ) ) goto failed;
    rio->region = (uint8_t*) VirtualAlloc( NULL, (SIZE_T) rio->layout.total_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    if ( !rio->region )
    {
        rocketnet_allocation_refused();
        goto failed;
    }
    rio->buffer_id = rio->rio.RIORegisterBuffer( (PCHAR) rio->region, (DWORD) rio->layout.total_bytes );
    if ( rio->buffer_id == RIO_INVALID_BUFFERID || fail_stage == 3 ) goto failed;

    // polled completion queues: no notification
    rio->send_cq = rio->rio.RIOCreateCompletionQueue( ROCKETNET_RIO_SEND_FRAMES, NULL );
    if ( rio->send_cq == RIO_INVALID_CQ ) goto failed;
    rio->receive_cq = rio->rio.RIOCreateCompletionQueue( ROCKETNET_RIO_RECEIVES, NULL );
    if ( rio->receive_cq == RIO_INVALID_CQ || fail_stage == 4 ) goto failed;

    // the request queue holds every send of both phases plus every receive
    rio->request_queue = rio->rio.RIOCreateRequestQueue( rio->socket, ROCKETNET_RIO_RECEIVES, 1, ROCKETNET_RIO_SEND_FRAMES, 1,
                                                         rio->receive_cq, rio->send_cq, NULL );
    if ( rio->request_queue == RIO_INVALID_RQ || fail_stage == 5 ) goto failed;

    // RNRioDriver Init: every receive slice is posted
    for ( int i = 0; i < ROCKETNET_RIO_RECEIVES; ++i )
    {
        if ( !rocketnet_rio_post_receive( rio, i ) ) goto failed;
        if ( fail_stage == 6 ) goto failed;
    }

    if ( !rocketnet_rio_address_read( &address, rio->ipv6, bind_address ) ) goto failed;
    rocketnet_info( "rio transport: port %d, %d receive slices, %d send frames per phase, %llu registered bytes", bind_address->port,
                    ROCKETNET_RIO_RECEIVES, ROCKETNET_RIO_SEND_QUEUE_SIZE, (unsigned long long) rio->layout.total_bytes );
    return rio;

failed:
    rocketnet_warn( "rio transport create failed (Winsock %d, Win32 %lu, test stage %d)", WSAGetLastError(), GetLastError(), fail_stage );
    // Partial construction remains private to this synchronous create call.
    rocketnet_server_rio_stop( rio );
    while ( !rocketnet_server_rio_stopped( rio ) ) rocketnet_platform_sleep( 0.001 );
    rocketnet_server_rio_destroy( rio );
    return NULL;
}

// RNRioDriver Reserve: one fetch_add reserves the next slot of the fill
// phase; past the last slot it returns NULL and counts SEND_FULL
uint8_t * rocketnet_server_rio_start_packet( rocketnet_server_rio_t * rio, const rocketnet_address_t * to, uint64_t * packet_id )
{
    if ( rio->stopping.load( std::memory_order_acquire ) ) return NULL;
    if ( rio->switch_owed.load( std::memory_order_acquire ) )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FULL );
        return NULL;
    }
    const int phase = rio->fill.load( std::memory_order_relaxed );
    const int slot = rio->next[phase].fetch_add( 1, std::memory_order_relaxed );
    // the lower bound too: a caller that stages forever without a flush
    // wraps the index negative, and a negative index is an out-of-bounds write
    if ( slot < 0 || slot >= ROCKETNET_RIO_SEND_QUEUE_SIZE )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FULL );
        return NULL;
    }
    const int index = phase * ROCKETNET_RIO_SEND_QUEUE_SIZE + slot;
    rio->slot_bytes[index] = 0;
    if ( !rocketnet_rio_address_write( to, rio->ipv6, rocketnet_rio_send_address( rio, index ) ) )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
        return NULL;                            // the slot stays at zero bytes and is never posted
    }
    *packet_id = (uint64_t) index;
    return rocketnet_rio_frame( rio, index );
}

// RNRioDriver Finish: the slot records its bytes
void rocketnet_server_rio_finish_packet( rocketnet_server_rio_t * rio, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );
    (void) packet_id;
    const int index = rocketnet_rio_frame_index( rio, frame );
    if ( index < 0 || packet_bytes <= 0 || packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return;
    rio->slot_bytes[index] = packet_bytes;
}

// RNRioDriver Abort: the slot's bytes are zeroed and it is never posted
void rocketnet_server_rio_abort_packet( rocketnet_server_rio_t * rio, uint64_t packet_id, uint8_t * frame )
{
    (void) packet_id;
    const int index = rocketnet_rio_frame_index( rio, frame );
    if ( index < 0 )
        return;
    rio->slot_bytes[index] = 0;
}

// send_packets, fill threads joined: DequeueSend, then Post, then
// SwitchPhase (RNRioDriver). If the other phase has not drained, the switch
// stays owed, the fill phase stays closed to fill threads, and this call
// returns for a later update. The model's sending flag is switch_owed.
void rocketnet_server_rio_send_packets( rocketnet_server_rio_t * rio )
{
    // RNRioDriver DequeueSend: retire completed slots of either phase
    rocketnet_rio_dequeue_send( rio );

    const int phase = rio->fill.load( std::memory_order_relaxed );
    if ( !rio->switch_owed.load( std::memory_order_relaxed ) )
    {
        // RNRioDriver Post: every finished slot of the fill phase by
        // RIOSendEx with RIO_MSG_DEFER, then one RIO_MSG_COMMIT_ONLY, and the
        // phase's outstanding count recorded. Between post and completion the
        // kernel owns the slot's frame and its address slot.
        int count = rio->next[phase].load( std::memory_order_relaxed );
        if ( count < 0 || count > ROCKETNET_RIO_SEND_QUEUE_SIZE ) count = ROCKETNET_RIO_SEND_QUEUE_SIZE;
        const bool stopping = rio->stopping.load( std::memory_order_acquire );
        int posted = 0;
        for ( int slot = 0; slot < count; ++slot )
        {
            const int index = phase * ROCKETNET_RIO_SEND_QUEUE_SIZE + slot;
            const int bytes = rio->slot_bytes[index];
            if ( bytes <= 0 || stopping )
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
            address.Length = sizeof(SOCKADDR_INET);
            int injected = 0;
            if ( !ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST, &injected ) &&
                 rio->rio.RIOSendEx( rio->request_queue, &data, 1, NULL, &address, NULL, NULL, RIO_MSG_DEFER,
                                     (PVOID) (uintptr_t) rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, phase, (uint32_t) slot ) ) )
                posted++;
            else
            {
                // an unsuccessful post has no completion: the slot is free again
                rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
                rio->slot_bytes[index] = 0;
            }
        }
        rio->outstanding[phase] += posted;
        if ( posted > 0 )
            rio->uncommitted = true;
        rio->switch_owed.store( true, std::memory_order_relaxed );
    }
    rocketnet_rio_commit( rio );

    // RNRioDriver SwitchPhase: the other phase opens to fill threads only
    // when its outstanding count is zero. A pending completion keeps the
    // switch owed until a later send_packets call.
    const int other = 1 - phase;
    if ( rio->outstanding[other] > 0 )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_SEND_WAITS );
        return;
    }
    rio->next[other].store( 0, std::memory_order_relaxed );
    rio->fill.store( other, std::memory_order_relaxed );
    rio->switch_owed.store( false, std::memory_order_release );
}

// RNRioDriver DequeueReceive for one completion: a failed status counts
// RECEIVE_FAILED; a datagram is checked against the size bounds and copied
// into the fill bank when it has room; then the slice is re-posted, or
// retired when the re-post fails
static void rocketnet_rio_complete_receive( rocketnet_server_rio_t * rio, const RIORESULT * result )
{
    const uint64_t context = (uint64_t) result->RequestContext;
    const int slice = rocketnet_rio_context_index( context );
    if ( rocketnet_rio_context_kind( context ) != ROCKETNET_RIO_CONTEXT_RECEIVE || slice < 0 || slice >= ROCKETNET_RIO_RECEIVES )
    {
        rocketnet_error( "rio transport: receive completion with a foreign request context %llx", (unsigned long long) context );
        return;
    }
    rio->posted_receives--;
    LONG status = result->Status;
    int injected = 0;
    if ( ROCKETNET_RIO_TEST_FAULT( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_COMPLETION, &injected ) )
        status = (LONG) injected;
    // an ICMP report of an earlier send (port, host or network unreachable)
    // is counted with the failed receives and the slice is re-posted below,
    // never the end of the drain (windows-udp-connreset audit, rio completion)
    if ( rocketnet_platform_receive_status_is_reset( (int) status ) )
        rocketnet_platform_count_receive_reset();
    if ( status == WSAEMSGSIZE )
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE );     // past the slice: oversize
    else if ( status != 0 )
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED );
    else
    {
        const int bytes = (int) result->BytesTransferred;
        rocketnet_address_t from;
        rocketnet_rio_bank_t * bank = &rio->banks[rio->fill_bank];
        if ( bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || bytes > ROCKETNET_RECEIVE_CEILING )
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE );
        else if ( !rocketnet_rio_address_read( rocketnet_rio_receive_address( rio, slice ), rio->ipv6, &from ) )
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED );
        else if ( bank->num_packets >= ROCKETNET_RIO_RECEIVES )
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW );
        else
        {
            const int index = bank->num_packets++;
            bank->from[index] = from;
            bank->packet_bytes[index] = bytes;
            memcpy( bank->data[index], rio->region + rocketnet_rio_receive_slice_offset( &rio->layout, slice ), (size_t) bytes );
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED );
        }
    }
    // after stop nothing is re-posted and nothing is counted. A refused
    // re-post waits in the retry list for the next receive_packets, so a
    // transient refusal never shrinks the receive depth.
    if ( !rio->stopping.load( std::memory_order_acquire ) && !rocketnet_rio_post_receive( rio, slice ) )
    {
        rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED );
        rocketnet_assert( rio->num_retry < ROCKETNET_RIO_RECEIVES );
        rio->retry_slices[rio->num_retry++] = slice;
    }
}

// RNRioDriver RetryRepost: one more post of each slice a refused re-post
// left in the retry list; a slice refused again stays for the next call and
// counts RECEIVE_REPOST_FAILED again.
static void rocketnet_rio_retry_receives( rocketnet_server_rio_t * rio )
{
    if ( rio->num_retry == 0 || rio->stopping.load( std::memory_order_acquire ) )
        return;
    int kept = 0;
    for ( int i = 0; i < rio->num_retry; ++i )
    {
        const int slice = rio->retry_slices[i];
        if ( !rocketnet_rio_post_receive( rio, slice ) )
        {
            rocketnet_rio_count( rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED );
            rio->retry_slices[kept++] = slice;
        }
    }
    rio->num_retry = kept;
}

// receive_packets: DequeueReceive at most one bank of completions, then
// Swap (RNRioDriver).
void rocketnet_server_rio_receive_packets( rocketnet_server_rio_t * rio )
{
    rocketnet_rio_retry_receives( rio );
    int remaining = ROCKETNET_RIO_RECEIVES;
    while ( remaining > 0 )
    {
        const ULONG batch = remaining < ROCKETNET_RIO_DEQUEUE_BATCH
            ? (ULONG) remaining : ROCKETNET_RIO_DEQUEUE_BATCH;
        RIORESULT results[ROCKETNET_RIO_DEQUEUE_BATCH];
#if defined(ROCKETNET_RIO_OVERFLOW_TEST_HOOKS) || defined(ROCKETNET_RIO_TESTS)
        const ULONG count = rocketnet_rio_test_functions
            ? rocketnet_rio_test_functions->dequeue( rio->receive_cq, results, batch )
            : rio->rio.RIODequeueCompletion( rio->receive_cq, results, batch );
#else
        const ULONG count = rio->rio.RIODequeueCompletion( rio->receive_cq, results, batch );
#endif
        if ( count == RIO_CORRUPT_CQ )
        {
            rocketnet_error( "rio transport: receive completion queue corrupt" );
            break;
        }
        for ( ULONG i = 0; i < count; ++i )
            rocketnet_rio_complete_receive( rio, &results[i] );
        remaining -= (int) count;
        if ( count < batch )
            break;
    }

    // RNRioDriver Swap: the old fill bank becomes the exposed bank, whose
    // views process_packets returns until the next receive_packets
    rocketnet_rio_bank_t * exposed = &rio->banks[rio->fill_bank];
    rio->fill_bank = 1 - rio->fill_bank;
    rio->banks[rio->fill_bank].num_packets = 0;
    rio->process.num_packets = exposed->num_packets;
    for ( int i = 0; i < exposed->num_packets; ++i )
    {
        rio->process.from[i] = exposed->from[i];
        rio->process.packet_bytes[i] = exposed->packet_bytes[i];
        rio->process.packet_data[i] = exposed->data[i];
    }
}

#if defined(ROCKETNET_RIO_OVERFLOW_TEST_HOOKS) || defined(ROCKETNET_RIO_TESTS)

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
            ROCKETNET_RIO_CONTEXT_RECEIVE, 0, (uint32_t) ( i % ROCKETNET_RIO_RECEIVES ) );
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

// A test-only fixture drives the production receive_packets loop with an
// endlessly full fake queue. Its dequeue guard makes an unbounded loop return
// a corrupt-CQ sentinel; the bounded loop returns before reaching the guard.
bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                  int * repost_calls, int * pending_receives, int * guard_hit )
{
    if ( !counters || !packet_counts || !repost_calls || !pending_receives || !guard_hit )
        return false;

    rocketnet_server_rio_t rio;
    if ( !rocketnet_rio_layout_plan( ROCKETNET_RIO_SEND_FRAMES, ROCKETNET_RIO_RECEIVES,
            ROCKETNET_FRAME_STRIDE, ROCKETNET_RIO_RECEIVE_SLICE_BYTES, (int) sizeof(SOCKADDR_INET),
            4096, &rio.layout ) )
        return false;

    rio.region = (uint8_t*) calloc( 1, (size_t) rio.layout.total_bytes );
    rio.banks = (rocketnet_rio_bank_t*) calloc( 2, sizeof(rocketnet_rio_bank_t) );
    if ( !rio.region || !rio.banks )
    {
        free( rio.banks );
        free( rio.region );
        return false;
    }

    for ( int i = 0; i < ROCKETNET_RIO_RECEIVES; ++i )
    {
        SOCKADDR_INET * address = (SOCKADDR_INET*) ( rio.region +
            rocketnet_rio_receive_address_offset( &rio.layout, i ) );
        address->Ipv4.sin_family = AF_INET;
        address->Ipv4.sin_port = htons( 40000 );
        address->Ipv4.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    }

    rio.receive_cq = (RIO_CQ) (uintptr_t) 1;
    rio.posted_receives = ROCKETNET_RIO_RECEIVES;
    const int calls_per_bank = ( ROCKETNET_RIO_RECEIVES + ROCKETNET_RIO_DEQUEUE_BATCH - 1 ) / ROCKETNET_RIO_DEQUEUE_BATCH;
    rocketnet_rio_overflow_test_state_t state = { NULL, 0, 0, 0, 0, 0, 2 * calls_per_bank };
    const rocketnet_rio_test_function_table_t functions = {
        rocketnet_rio_overflow_test_dequeue,
        rocketnet_rio_overflow_test_receive
    };
    rocketnet_rio_overflow_test_state = &state;
    rocketnet_rio_test_functions = &functions;

    rocketnet_server_rio_receive_packets( &rio );
    packet_counts[0] = rio.process.num_packets;
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i )
        counters[i] = rio.counters[i].load( std::memory_order_relaxed );

    // The queue stays full through the guard. A bounded call swaps after one
    // bank and leaves the still-full queue for the next update.
    rocketnet_server_rio_receive_packets( &rio );
    packet_counts[1] = rio.process.num_packets;
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i )
        counters[ROCKETNET_RIO_NUM_COUNTERS + i] = rio.counters[i].load( std::memory_order_relaxed );
    *repost_calls = state.repost_calls;
    *pending_receives = rio.posted_receives;
    *guard_hit = state.guard_hit;

    rocketnet_rio_test_functions = NULL;
    rocketnet_rio_overflow_test_state = NULL;
    free( rio.banks );
    free( rio.region );
    return true;
}

#endif // defined(ROCKETNET_RIO_OVERFLOW_TEST_HOOKS) || defined(ROCKETNET_RIO_TESTS)

// RNRioDriver TransportReads: the views of the exposed bank
rocketnet_server_socket_process_packets_t * rocketnet_server_rio_process_packets( rocketnet_server_rio_t * rio )
{
    return &rio->process;
}

void rocketnet_server_rio_counters( rocketnet_server_rio_t * rio, uint64_t * counters )
{
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; ++i ) counters[i] = rio->counters[i].load( std::memory_order_relaxed );
}

int rocketnet_server_rio_pending( rocketnet_server_rio_t * rio )
{
    return rio->outstanding[0] + rio->outstanding[1] + rio->posted_receives;
}

#if defined(ROCKETNET_RIO_TESTS)
void rocketnet_server_rio_test_fail_next( rocketnet_server_rio_t * rio, int fault, int count, int status )
{
    rocketnet_assert( fault >= 0 && fault < ROCKETNET_RIO_TEST_NUM_FAULTS && count >= 0 );
    rio->test_faults[fault] = count;
    rio->test_fault_status[fault] = status;
}

int rocketnet_server_rio_test_faults_left( rocketnet_server_rio_t * rio, int fault )
{
    return rio->test_faults[fault];
}

int rocketnet_server_rio_test_retry_receives( rocketnet_server_rio_t * rio )
{
    return rio->num_retry;
}
#endif

#endif // ROCKETNET_PLATFORM_WINDOWS

int rocketnet_server_rio_windows_cpp_dummy = 0;
