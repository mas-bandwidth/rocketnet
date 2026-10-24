/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "rocketnet_server_iocp.h"
#include "rocketnet_platform.h"
#include "rocketnet_coalesce.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <atomic>
#include <new>
#include <stddef.h>
#include <string.h>

// The 16-aligned members lead, each starting where the one before ends, so the
// compiler inserts no alignment padding; the struct takes their alignment.
struct rocketnet_iocp_operation_t
{
    OVERLAPPED overlapped;
    SLIST_ENTRY free_entry;
    alignas(16) char control[64];
    alignas(16) uint8_t frame[ROCKETNET_FRAME_STRIDE];
    WSABUF buffer;
    sockaddr_storage address;
    int address_bytes;
    DWORD flags;
    int index;
    bool send;
    int packet_bytes;
    // A send post carries run_count frames and run_bytes bytes: one frame on
    // the per-datagram path, a USO run otherwise. The post's operation is the
    // run's first frame; run_next chains the run's frames by index (-1 ends
    // it), and every frame of the run stays owned until that one completion.
    rocketnet_address_t to;
    int run_count;
    int run_bytes;
    int run_next;
    // WSASendMsg for a USO run, WSARecvMsg for a URO receive. uro_data is the
    // receive's coalesced buffer, NULL on the per-datagram receive path.
    WSAMSG message;
    uint8_t * uro_data;
};

static_assert( offsetof( rocketnet_iocp_operation_t, overlapped ) == 0, "completion identifies its operation" );
static_assert( offsetof( rocketnet_iocp_operation_t, free_entry ) % MEMORY_ALLOCATION_ALIGNMENT == 0, "Windows SList alignment" );
static_assert( alignof( rocketnet_iocp_operation_t ) % MEMORY_ALLOCATION_ALIGNMENT == 0 && alignof( rocketnet_iocp_operation_t ) >= 16, "an operation keeps its allocation alignment" );
static_assert( offsetof( rocketnet_iocp_operation_t, control ) == offsetof( rocketnet_iocp_operation_t, free_entry ) + sizeof( SLIST_ENTRY ) &&
               offsetof( rocketnet_iocp_operation_t, control ) % 16 == 0, "control follows the SList entry with no implicit padding" );
static_assert( offsetof( rocketnet_iocp_operation_t, frame ) == offsetof( rocketnet_iocp_operation_t, control ) + 64, "frame follows control with no implicit padding" );
static_assert( ROCKETNET_RECEIVE_CEILING + 1 <= ROCKETNET_FRAME_STRIDE, "receive detects oversize datagrams" );

// the bytes of a receive buffer's members after data, which pack with no
// padding between them
#define ROCKETNET_IOCP_RECEIVE_BUFFER_TAIL_BYTES ( sizeof( int ) + ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE * ( sizeof( rocketnet_address_t ) + sizeof( int ) ) )

static_assert( ROCKETNET_IOCP_RECEIVE_BUFFER_TAIL_BYTES % 16 != 0, "a receive buffer's tail padding is a nonzero array" );

// data leads, so its 16-alignment needs no padding before it, and the rest of
// the last 16 bytes is written out, so the compiler adds no tail padding
// (MSVC C4324)
struct rocketnet_iocp_receive_buffer_t
{
    alignas(16) uint8_t data[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];
    int num_packets;
    rocketnet_address_t from[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE];
    int packet_bytes[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE];
    uint8_t padding[16 - ROCKETNET_IOCP_RECEIVE_BUFFER_TAIL_BYTES % 16];
};

// the bytes of a worker's members, which pack with no padding between them
#define ROCKETNET_IOCP_RECEIVE_BANKS 4
#define ROCKETNET_IOCP_WORKER_MEMBER_BYTES ( ROCKETNET_IOCP_RECEIVE_BANKS * sizeof( rocketnet_iocp_receive_buffer_t ) + sizeof( rocketnet_server_iocp_t * ) + \
                                             sizeof( rocketnet_platform_thread_t * ) + sizeof( rocketnet_platform_mutex_t ) + 17 * sizeof( int ) )

static_assert( ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64 != 0, "a worker's tail padding is a nonzero array" );

struct alignas(64) rocketnet_iocp_worker_t
{
    rocketnet_iocp_receive_buffer_t buffers[ROCKETNET_IOCP_RECEIVE_BANKS];
    rocketnet_server_iocp_t * owner;
    rocketnet_platform_thread_t * thread;
    // Only ownership indices are exchanged under this mutex. Completion
    // handling, packet copies, send retirement and reposts run outside it.
    rocketnet_platform_mutex_t mutex;
    int free_banks[ROCKETNET_IOCP_RECEIVE_BANKS];
    int ready_banks[ROCKETNET_IOCP_RECEIVE_BANKS];
    int exposed_banks[ROCKETNET_IOCP_RECEIVE_BANKS];
    int free_count = ROCKETNET_IOCP_RECEIVE_BANKS - 1;
    int ready_head = 0;
    int ready_count = 0;
    int fill_index = 0;
    int exposed_count = 0;
    // the rest of the last cache line, explicit so the compiler adds no tail padding
    uint8_t padding[64 - ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64];
};

static_assert( sizeof( rocketnet_iocp_worker_t ) == ROCKETNET_IOCP_WORKER_MEMBER_BYTES + 64 - ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64 &&
               sizeof( rocketnet_iocp_worker_t ) % 64 == 0, "a worker has no implicit padding" );

// These compact command banks contain only operation indices. The cold frame
// pool remains completion-owned until Windows retires each overlapped send.
// Main owns one bank, TX owns at most one, and ready banks are FIFO.
#define ROCKETNET_IOCP_SEND_BANKS 3
struct rocketnet_iocp_send_bank_t
{
    int count = 0;
    int staged[ROCKETNET_IOCP_SEND_QUEUE_SIZE];
};

struct rocketnet_server_iocp_t
{
    // The aligned members lead: the two SList heads share the first cache
    // line, explicitly padded out, and num_staged starts the second, so the
    // compiler inserts no alignment padding before any of them.
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER free_sends;
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER retry_receives;
    uint8_t slist_padding[64 - 2 * sizeof( SLIST_HEADER )];
    alignas(64) std::atomic<uint64_t> num_staged{0};
    rocketnet_iocp_send_bank_t send_banks[ROCKETNET_IOCP_SEND_BANKS];
    rocketnet_platform_mutex_t send_mutex = {};
    rocketnet_platform_wake_t * send_wake = NULL;
    rocketnet_platform_thread_t * send_thread = NULL;
    std::atomic<bool> send_exited{true}; // also true before a worker is created
    int send_producer = 0; // caller-owned until stop publishes stopping
    int send_free[ROCKETNET_IOCP_SEND_BANKS] = {1, 2, 0};
    int send_free_count = ROCKETNET_IOCP_SEND_BANKS - 1;
    int send_ready[ROCKETNET_IOCP_SEND_BANKS] = {};
    int send_ready_head = 0;
    int send_ready_count = 0;
    void * context;
    SOCKET socket = INVALID_SOCKET;
    HANDLE port;
    bool ipv6;
    int num_workers;
    rocketnet_iocp_worker_t * workers;
    rocketnet_iocp_operation_t * sends;
    rocketnet_iocp_operation_t * receives;
    std::atomic<bool> stopping{false};
    std::atomic<int> pending{0};
    std::atomic<int> active_workers{0};
    std::atomic<uint64_t> counters[ROCKETNET_IOCP_NUM_COUNTERS]{};
    rocketnet_server_socket_process_packets_t process;
    std::atomic<int> features{0};
    // The socket's UDP_SEND_MSG_SIZE now. Only the TX worker writes it.
    int uso_stride = 0;
    LPFN_WSARECVMSG receive_message = NULL;
    uint8_t * uro_buffers = NULL;
    // The flush's view of the staged frames, slot by slot, for run grouping:
    // bytes (zero for an aborted or empty slot) and destination.
    int run_bytes[ROCKETNET_IOCP_SEND_QUEUE_SIZE];
    rocketnet_address_t run_to[ROCKETNET_IOCP_SEND_QUEUE_SIZE];
#if defined(ROCKETNET_IOCP_TESTS)
    std::atomic<int> test_faults[ROCKETNET_IOCP_TEST_NUM_FAULTS]{};
    std::atomic<int> test_fault_status[ROCKETNET_IOCP_TEST_NUM_FAULTS]{};
    std::atomic<bool> test_hold_tx{false};
    std::atomic<int> test_active_tx_batches{0};
    std::atomic<int> test_pending_tx_banks{0};
    std::atomic<bool> test_hold_completions{false};
    std::atomic<int> test_active_completion_batches{0};
#endif
};

static_assert( offsetof( rocketnet_server_iocp_t, retry_receives ) == sizeof( SLIST_HEADER ) &&
               offsetof( rocketnet_server_iocp_t, num_staged ) == 64, "the iocp's aligned members have no implicit padding" );

// The fault seam (rocketnet_server_iocp_test_fail_next) exists only in the
// test executable. Elsewhere the check is the constant false, so the
// production paths carry no test branch.
#if defined(ROCKETNET_IOCP_TESTS)
static bool rocketnet_iocp_test_fault( rocketnet_server_iocp_t * iocp, int fault, int * status )
{
    int left = iocp->test_faults[fault].load( std::memory_order_acquire );
    while ( left > 0 )
    {
        if ( iocp->test_faults[fault].compare_exchange_weak( left, left - 1, std::memory_order_acq_rel ) )
        {
            *status = iocp->test_fault_status[fault].load( std::memory_order_relaxed );
            return true;
        }
    }
    return false;
}
#define ROCKETNET_IOCP_TEST_FAULT( iocp, fault, status ) rocketnet_iocp_test_fault( iocp, fault, status )
#else
#define ROCKETNET_IOCP_TEST_FAULT( iocp, fault, status ) false
#endif

static int rocketnet_iocp_address_write( const rocketnet_address_t * input, bool ipv6, sockaddr_storage * storage )
{
    rocketnet_address_t address = *input;
    memset( storage, 0, sizeof(*storage) );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
        rocketnet_address_convert_ipv4_to_ipv6( &address );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 * out = (sockaddr_in6*) storage;
        out->sin6_family = AF_INET6;
        out->sin6_port = htons( address.port );
        for ( int i = 0; i < 8; ++i )
        {
            out->sin6_addr.s6_addr[i * 2] = (uint8_t) ( address.data.ipv6[i] >> 8 );
            out->sin6_addr.s6_addr[i * 2 + 1] = (uint8_t) address.data.ipv6[i];
        }
        return sizeof(sockaddr_in6);
    }
    if ( !ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        sockaddr_in * out = (sockaddr_in*) storage;
        out->sin_family = AF_INET;
        out->sin_port = htons( address.port );
        memcpy( &out->sin_addr, address.data.ipv4, 4 );
        return sizeof(sockaddr_in);
    }
    return 0;
}

// a source from its sockaddr; an IPv4-mapped IPv6 source passes the one
// receive rule, rocketnet_address_accept_received_source: canonicalised to
// IPv4 on the dual-stack socket, refused on any other
static bool rocketnet_iocp_address_read( const sockaddr_storage * storage, int bytes, bool dual_stack, rocketnet_address_t * address )
{
    memset( address, 0, sizeof(*address) );
    if ( storage->ss_family == AF_INET && bytes >= (int) sizeof(sockaddr_in) )
    {
        const sockaddr_in * from = (const sockaddr_in*) storage;
        address->type = ROCKETNET_ADDRESS_IPV4;
        address->port = ntohs( from->sin_port );
        memcpy( address->data.ipv4, &from->sin_addr, 4 );
        return true;
    }
    if ( storage->ss_family == AF_INET6 && bytes >= (int) sizeof(sockaddr_in6) )
    {
        const sockaddr_in6 * from = (const sockaddr_in6*) storage;
        address->type = ROCKETNET_ADDRESS_IPV6;
        address->port = ntohs( from->sin6_port );
        for ( int i = 0; i < 8; ++i )
            address->data.ipv6[i] = (uint16_t) ( ( from->sin6_addr.s6_addr[i * 2] << 8 ) | from->sin6_addr.s6_addr[i * 2 + 1] );
        return rocketnet_address_accept_received_source( address, dual_stack );
    }
    return false;
}

static bool rocketnet_iocp_post_receive( rocketnet_server_iocp_t * iocp, rocketnet_iocp_operation_t * op )
{
    // Only a completion worker or the creating thread posts a receive; no
    // publication mutex is held. A worker observing stop reissues cancellation
    // after its last potential repost:
    // a late repost is cancelled by that worker's next pass. Stack addresses/flags must not back pending I/O.
    if ( iocp->stopping.load( std::memory_order_acquire ) )
        return false;
    memset( &op->overlapped, 0, sizeof(op->overlapped) );
    op->address_bytes = sizeof(op->address);
    op->flags = 0;
    int result;
    int error = 0;
    if ( ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST, &error ) )
    {
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        result = SOCKET_ERROR;
    }
    else if ( op->uro_data )
    {
        // URO: a buffer of the coalesced maximum and a control area for
        // UDP_COALESCED_INFO, the stride of all but the last datagram.
        op->buffer.buf = (char*) op->uro_data;
        op->buffer.len = ROCKETNET_IOCP_URO_BUFFER_BYTES;
        memset( &op->message, 0, sizeof(op->message) );
        op->message.name = (LPSOCKADDR) &op->address;
        op->message.namelen = sizeof(op->address);
        op->message.lpBuffers = &op->buffer;
        op->message.dwBufferCount = 1;
        op->message.Control.buf = op->control;
        op->message.Control.len = sizeof(op->control);
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        result = iocp->receive_message( iocp->socket, &op->message, NULL, &op->overlapped, NULL );
    }
    else
    {
        op->buffer.buf = (char*) op->frame;
        op->buffer.len = ROCKETNET_RECEIVE_CEILING + 1;
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        result = WSARecvFrom( iocp->socket, &op->buffer, 1, NULL, &op->flags,
            (sockaddr*) &op->address, &op->address_bytes, &op->overlapped, NULL );
    }
    if ( result != 0 && error == 0 )
        error = WSAGetLastError();
    if ( result == 0 || error == WSA_IO_PENDING )
        return true; // Immediate success also produces exactly one IOCP event.
    iocp->pending.fetch_sub( 1, std::memory_order_release );
    // a post refused with an ICMP report of an earlier send is counted with
    // the reset statuses, then waits on the retry list like every refused post
    // (windows-udp-connreset audit, iocp post)
    if ( rocketnet_platform_receive_status_is_reset( error ) )
        rocketnet_platform_count_receive_reset();
    iocp->counters[ROCKETNET_IOCP_RECEIVE_REPOST_FAILED].fetch_add( 1, std::memory_order_relaxed );
    InterlockedPushEntrySList( &iocp->retry_receives, &op->free_entry );
    return false;
}

// Returns a send post's frames to the free pool: one frame, or every frame of
// a USO run. Each next index is read before its frame is released.
static void rocketnet_iocp_retire_send( rocketnet_server_iocp_t * iocp, rocketnet_iocp_operation_t * op )
{
    for ( ;; )
    {
        const int next = op->run_next;
        InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
        if ( next < 0 )
            break;
        op = &iocp->sends[next];
    }
}

// The UDP_COALESCED_INFO stride of a received message's control area, or 0
// when it has none. A UDP_COALESCED_INFO header whose cmsg_len is shorter
// than WSA_CMSG_LEN( sizeof( DWORD ) ) carries no whole DWORD and yields 0:
// only bytes the header claims are read. Not static: the unit suite calls it
// over a built WSAMSG (test_windows_coalesced_cmsg_short_length_is_not_read).
DWORD rocketnet_iocp_coalesced_stride( WSAMSG * message )
{
    for ( WSACMSGHDR * cmsg = WSA_CMSG_FIRSTHDR( message ); cmsg; cmsg = WSA_CMSG_NXTHDR( message, cmsg ) )
    {
        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_COALESCED_INFO )
        {
            DWORD stride = 0;
            if ( cmsg->cmsg_len >= WSA_CMSG_LEN( sizeof( stride ) ) )
                memcpy( &stride, WSA_CMSG_DATA( cmsg ), sizeof( stride ) );
            return stride;
        }
    }
    return 0;
}

// Publishes one URO completion: the datagrams of one source, each the
// UDP_COALESCED_INFO stride except a shorter last one, split by the shared
// coalescing rule and each checked against the bounds a single datagram is.
static void rocketnet_iocp_publish_uro( rocketnet_server_iocp_t * iocp, rocketnet_iocp_receive_buffer_t * buffer,
    rocketnet_iocp_operation_t * op, bool success, int receive_error, DWORD bytes )
{
    rocketnet_address_t from;
    if ( !success || bytes == 0 || ( op->message.dwFlags & MSG_TRUNC ) ||
         !rocketnet_iocp_address_read( &op->address, op->message.namelen, iocp->ipv6, &from ) )
    {
        iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( 1, std::memory_order_relaxed );
        if ( success && bytes == 0 )
            iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT].fetch_add( 1, std::memory_order_relaxed );
        else if ( receive_error == WSAEMSGSIZE || ( success && ( op->message.dwFlags & MSG_TRUNC ) ) )
            iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE].fetch_add( 1, std::memory_order_relaxed );
        return;
    }
    const DWORD stride = rocketnet_iocp_coalesced_stride( &op->message );
    const int message_bytes = (int) bytes;
    const int segment_bytes = stride > 0 && stride < bytes ? (int) stride : message_bytes;
    // Split window by window: each window but the last is a whole number of
    // strides, so the windows together are the split of the whole message.
    const int window_limit = segment_bytes * ROCKETNET_IOCP_URO_MAX_SEGMENTS;
    int offsets[ROCKETNET_IOCP_URO_MAX_SEGMENTS];
    int lengths[ROCKETNET_IOCP_URO_MAX_SEGMENTS];
    int segments = 0;
    for ( int base = 0; base < message_bytes; )
    {
        const int remaining = message_bytes - base;
        const int window = remaining < window_limit ? remaining : window_limit;
        const int count = rocketnet_coalesce_split( window, segment_bytes, offsets, lengths, ROCKETNET_IOCP_URO_MAX_SEGMENTS );
        for ( int i = 0; i < count; ++i )
        {
            ++segments;
            const int length = lengths[i];
            if ( length > 0 && length <= ROCKETNET_RECEIVE_CEILING && buffer->num_packets < ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE )
            {
                const int index = buffer->num_packets++;
                buffer->from[index] = from;
                buffer->packet_bytes[index] = length;
                memcpy( buffer->data[index], op->uro_data + base + offsets[i], (size_t) length );
                iocp->counters[ROCKETNET_IOCP_RECEIVE_COMPLETED].fetch_add( 1, std::memory_order_relaxed );
            }
            else
            {
                iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( 1, std::memory_order_relaxed );
                if ( length <= 0 )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT].fetch_add( 1, std::memory_order_relaxed );
                else if ( length > ROCKETNET_RECEIVE_CEILING )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE].fetch_add( 1, std::memory_order_relaxed );
            }
        }
        base += window;
    }
    if ( segments > 1 )
        iocp->counters[ROCKETNET_IOCP_URO_BATCHES].fetch_add( 1, std::memory_order_relaxed );
}

// A worker never gives its fill bank away until a different free bank is
// available. READY banks are FIFO; EXPOSED belongs to the main thread until
// its next receive call. This mutex protects only the index exchange.
static void rocketnet_iocp_publish_receive_bank( rocketnet_iocp_worker_t * worker )
{
    if ( worker->buffers[worker->fill_index].num_packets == 0 )
        return;
    bool published = false;
    rocketnet_platform_mutex_acquire( &worker->mutex );
    if ( worker->free_count > 0 )
    {
        const int tail = ( worker->ready_head + worker->ready_count ) % ROCKETNET_IOCP_RECEIVE_BANKS;
        rocketnet_assert( worker->ready_count < ROCKETNET_IOCP_RECEIVE_BANKS );
        worker->ready_banks[tail] = worker->fill_index;
        worker->ready_count++;
        worker->fill_index = worker->free_banks[--worker->free_count];
        published = true;
    }
    rocketnet_platform_mutex_release( &worker->mutex );
    if ( published ) worker->buffers[worker->fill_index].num_packets = 0;
}

static void rocketnet_iocp_worker( void * argument )
{
    rocketnet_iocp_worker_t * worker = (rocketnet_iocp_worker_t*) argument;
    rocketnet_server_iocp_t * iocp = worker->owner;
    for ( ;; )
    {
        if ( iocp->stopping.load( std::memory_order_acquire ) )
        {
            // Every worker cancels after its last potential repost. A sibling worker
            // may have read stopping=false before another worker's cancel;
            // its next pass cancels that late post as well.
            if ( iocp->socket != INVALID_SOCKET &&
                 !CancelIoEx( (HANDLE) iocp->socket, NULL ) && GetLastError() != ERROR_NOT_FOUND )
                rocketnet_error( "IOCP cancellation failed: %lu", GetLastError() );
            // TX may have observed running before the stop request. It must
            // finish that posting pass before a zero pending count can prove
            // there will never be a later overlapped submission.
            if ( iocp->send_exited.load( std::memory_order_acquire ) &&
                 iocp->pending.load( std::memory_order_acquire ) == 0 ) break;
        }

        OVERLAPPED_ENTRY entries[64];
        ULONG count = 0;
        const DWORD timeout = QueryDepthSList( &iocp->retry_receives ) ? 1 : 50;
        const BOOL ok = GetQueuedCompletionStatusEx( iocp->port, entries, 64, &count, timeout, FALSE );
        if ( !ok )
        {
            count = 0;
            if ( GetLastError() != WAIT_TIMEOUT )
            {
                rocketnet_error( "IOCP completion dequeue failed: %lu", GetLastError() );
                rocketnet_platform_sleep( 0.001 );
            }
        }
        if ( count )
            iocp->counters[ROCKETNET_IOCP_COMPLETION_BATCHES].fetch_add( 1, std::memory_order_relaxed );

#if defined(ROCKETNET_IOCP_TESTS)
        for ( ;; )
        {
            while ( iocp->test_hold_completions.load( std::memory_order_acquire ) )
                rocketnet_platform_sleep( 0.001 );
            iocp->test_active_completion_batches.fetch_add( 1, std::memory_order_acq_rel );
            if ( !iocp->test_hold_completions.load( std::memory_order_acquire ) )
                break;
            iocp->test_active_completion_batches.fetch_sub( 1, std::memory_order_release );
        }
#endif
        rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[worker->fill_index];
        for ( ULONG i = 0; i < count; ++i )
        {
            if ( !entries[i].lpOverlapped )
                continue; // A shutdown wakeup, not an I/O operation.
            rocketnet_iocp_operation_t * op = (rocketnet_iocp_operation_t*) entries[i].lpOverlapped;
            ULONG_PTR status = entries[i].Internal;
            int injected = 0;
            if ( ROCKETNET_IOCP_TEST_FAULT( iocp, op->send ? ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION : ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION, &injected ) )
                status = (ULONG_PTR) injected;
            const bool success = status == 0;
            const DWORD bytes = entries[i].dwNumberOfBytesTransferred;
            if ( op->send )
            {
                const int counter = success && bytes == (DWORD) op->run_bytes ? ROCKETNET_IOCP_SEND_COMPLETED : ROCKETNET_IOCP_SEND_FAILED;
                iocp->counters[counter].fetch_add( (uint64_t) op->run_count, std::memory_order_relaxed );
                iocp->pending.fetch_sub( 1, std::memory_order_release );
                rocketnet_iocp_retire_send( iocp, op );
                continue;
            }

            rocketnet_address_t from;
            const bool running = !iocp->stopping.load( std::memory_order_acquire );
            int receive_error = 0;
            if ( !success && running && entries[i].Internal != 0 )
            {
                // a receive completed with an ICMP report of an earlier send
                // (port, host or network unreachable): counted, dropped and
                // reposted below like every failed receive, never the end of
                // the worker (windows-udp-connreset audit, iocp and URO completion)
                DWORD transferred = 0;
                DWORD completion_flags = 0;
                if ( !WSAGetOverlappedResult( iocp->socket, &op->overlapped, &transferred, FALSE, &completion_flags ) )
                {
                    receive_error = WSAGetLastError();
                    if ( rocketnet_platform_receive_status_is_reset( receive_error ) )
                        rocketnet_platform_count_receive_reset();
                }
            }
            if ( op->uro_data )
            {
                if ( running )
                    rocketnet_iocp_publish_uro( iocp, buffer, op, success, receive_error, bytes );
            }
            else if ( running && success && bytes > 0 && bytes <= ROCKETNET_RECEIVE_CEILING &&
                 buffer->num_packets < ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE &&
                 rocketnet_iocp_address_read( &op->address, op->address_bytes, iocp->ipv6, &from ) )
            {
                const int index = buffer->num_packets++;
                buffer->from[index] = from;
                buffer->packet_bytes[index] = (int) bytes;
                memcpy( buffer->data[index], op->frame, bytes );
                iocp->counters[ROCKETNET_IOCP_RECEIVE_COMPLETED].fetch_add( 1, std::memory_order_relaxed );
            }
            else if ( running )
            {
                iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( 1, std::memory_order_relaxed );
                if ( success && bytes == 0 )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT].fetch_add( 1, std::memory_order_relaxed );
                else if ( receive_error == WSAEMSGSIZE || ( success && bytes > ROCKETNET_RECEIVE_CEILING ) )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE].fetch_add( 1, std::memory_order_relaxed );
            }

            iocp->pending.fetch_sub( 1, std::memory_order_release );
            rocketnet_iocp_post_receive( iocp, op );
        }

        // A transient posting failure must not permanently shrink the receive
        // pool. Detach one retry batch so failures wait for the next wakeup.
        SLIST_ENTRY * retry = InterlockedFlushSList( &iocp->retry_receives );
        while ( retry )
        {
            SLIST_ENTRY * next = retry->Next;
            rocketnet_iocp_operation_t * op = CONTAINING_RECORD( retry, rocketnet_iocp_operation_t, free_entry );
            rocketnet_iocp_post_receive( iocp, op );
            retry = next;
        }
        if ( !iocp->stopping.load( std::memory_order_acquire ) )
            rocketnet_iocp_publish_receive_bank( worker );
#if defined(ROCKETNET_IOCP_TESTS)
        iocp->test_active_completion_batches.fetch_sub( 1, std::memory_order_release );
#endif
    }
    iocp->active_workers.fetch_sub( 1, std::memory_order_release );
}

void rocketnet_server_iocp_stop( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp || iocp->stopping.load( std::memory_order_acquire ) ) return;
    // Fill threads have joined before stop. The release transfers the final
    // unpublished producer to TX, without scanning it or acquiring a mutex.
    uint64_t count = iocp->num_staged.load( std::memory_order_relaxed );
    if ( count > ROCKETNET_IOCP_SEND_QUEUE_SIZE ) count = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
    iocp->send_banks[iocp->send_producer].count = (int) count;
    iocp->stopping.store( true, std::memory_order_release );
    if ( iocp->send_wake ) rocketnet_platform_wake_set( iocp->send_wake );
    // Cancellation runs on the workers after their current pass. This request
    // never acquires a worker mutex or waits for an overlapped operation.
    if ( iocp->port )
        for ( int i = 0; i < iocp->num_workers; ++i )
            PostQueuedCompletionStatus( iocp->port, 0, 0, NULL );
}

bool rocketnet_server_iocp_stopped( rocketnet_server_iocp_t * iocp )
{
    return iocp->stopping.load( std::memory_order_acquire ) &&
           iocp->send_exited.load( std::memory_order_acquire ) &&
           iocp->pending.load( std::memory_order_acquire ) == 0 &&
           iocp->active_workers.load( std::memory_order_acquire ) == 0;
}

void rocketnet_server_iocp_destroy( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp ) return;
    const bool stopped = rocketnet_server_iocp_stopped( iocp );
    rocketnet_assert( stopped );
    if ( !stopped ) return;
    if ( iocp->send_thread )
    {
        rocketnet_platform_thread_join( iocp->send_thread );
        rocketnet_platform_thread_destroy( iocp->send_thread );
    }
    if ( iocp->send_wake ) rocketnet_platform_wake_destroy( iocp->send_wake );
    if ( iocp->send_mutex.ok ) rocketnet_platform_mutex_destroy( &iocp->send_mutex );
    if ( iocp->workers )
        for ( int i = 0; i < iocp->num_workers; ++i )
        {
            rocketnet_iocp_worker_t * worker = &iocp->workers[i];
            if ( worker->thread )
            {
                rocketnet_platform_thread_join( worker->thread );
                rocketnet_platform_thread_destroy( worker->thread );
            }
            if ( worker->mutex.ok ) rocketnet_platform_mutex_destroy( &worker->mutex );
            worker->~rocketnet_iocp_worker_t();
        }
    rocketnet_assert( iocp->pending.load() == 0 );
    if ( iocp->socket != INVALID_SOCKET ) closesocket( iocp->socket );
    if ( iocp->port ) CloseHandle( iocp->port );
    if ( iocp->sends )
        for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i ) iocp->sends[i].~rocketnet_iocp_operation_t();
    if ( iocp->receives )
        for ( int i = 0; i < iocp->num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i ) iocp->receives[i].~rocketnet_iocp_operation_t();
    void * context = iocp->context;
    rocketnet_free( context, iocp->workers );
    rocketnet_free( context, iocp->sends );
    rocketnet_free( context, iocp->receives );
    rocketnet_free( context, iocp->uro_buffers );
    iocp->~rocketnet_server_iocp_t();
    rocketnet_free( context, iocp );
}

uint64_t rocketnet_server_iocp_allocation_bytes(int workers)
{
    if (workers < 1 || workers > ROCKETNET_IOCP_MAX_WORKERS) return 0;
    uint64_t bytes = rocketnet_memory_allocations({sizeof(rocketnet_server_iocp_t),
        (uint64_t)workers * sizeof(rocketnet_iocp_worker_t),
        (uint64_t)ROCKETNET_IOCP_SEND_QUEUE_SIZE * sizeof(rocketnet_iocp_operation_t),
        (uint64_t)workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * sizeof(rocketnet_iocp_operation_t),
        (uint64_t)workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * ROCKETNET_IOCP_URO_BUFFER_BYTES});
    if (!bytes || !rocketnet_memory_add(bytes, (uint64_t)workers + 1, rocketnet_platform_thread_allocation_bytes()) ||
        !rocketnet_memory_add(bytes, 1, rocketnet_platform_wake_allocation_bytes())) return 0;
    return bytes <= SIZE_MAX ? bytes : 0;
}

static void rocketnet_iocp_send_worker( void * argument );

static std::atomic<int> rocketnet_iocp_test_forced_flags{0};

void rocketnet_server_iocp_test_force_flags( int flags )
{
    rocketnet_iocp_test_forced_flags.store( flags, std::memory_order_relaxed );
}

rocketnet_server_iocp_t * rocketnet_server_iocp_create( void * context, rocketnet_address_t * bind_address, int num_workers, int fail_stage, int flags )
{
    flags |= rocketnet_iocp_test_forced_flags.load( std::memory_order_relaxed );
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) ) return NULL;
    if ( num_workers == 0 ) num_workers = ROCKETNET_IOCP_DEFAULT_WORKERS;
    if ( num_workers < 1 || num_workers > ROCKETNET_IOCP_MAX_WORKERS )
    {
        rocketnet_warn( "iocp transport refused: worker count %d is outside 1..%d", num_workers, ROCKETNET_IOCP_MAX_WORKERS );
        return NULL;
    }
    void * storage = rocketnet_malloc( context, sizeof(rocketnet_server_iocp_t) );
    if ( !storage ) return NULL;
    rocketnet_server_iocp_t * iocp = new (storage) rocketnet_server_iocp_t();
    iocp->context = context;
    iocp->num_workers = num_workers;
    iocp->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    InitializeSListHead( &iocp->free_sends );
    InitializeSListHead( &iocp->retry_receives );

    sockaddr_storage address;
    int address_bytes = rocketnet_iocp_address_write( bind_address, iocp->ipv6, &address );
    int buffer_bytes = ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE;
    DWORD returned_bytes = 0;
    BOOL enabled = TRUE;
    DWORD v6_only = FALSE;

    iocp->workers = (rocketnet_iocp_worker_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_worker_t) * (size_t) num_workers );
    if ( !iocp->workers ) goto failed;
    for ( int i = 0; i < num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = new (&iocp->workers[i]) rocketnet_iocp_worker_t();
        for ( int bank = 1; bank < ROCKETNET_IOCP_RECEIVE_BANKS; ++bank )
            worker->free_banks[bank - 1] = bank;
    }

    iocp->sends = (rocketnet_iocp_operation_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_operation_t) * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    if ( !iocp->sends ) goto failed;
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        rocketnet_iocp_operation_t * op = new (&iocp->sends[i]) rocketnet_iocp_operation_t();
        op->index = i;
        op->send = true;
        InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
    }
    iocp->receives = (rocketnet_iocp_operation_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_operation_t) * (size_t) num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    if ( !iocp->receives ) goto failed;
    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i ) new (&iocp->receives[i]) rocketnet_iocp_operation_t();

    iocp->socket = WSASocketW( iocp->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED );
    if ( iocp->socket == INVALID_SOCKET || fail_stage == 1 ) goto failed;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &enabled, sizeof(enabled) ) == SOCKET_ERROR ) goto failed;
    if ( iocp->ipv6 && setsockopt( iocp->socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &v6_only, sizeof(v6_only) ) == SOCKET_ERROR ) goto failed;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_RCVBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    buffer_bytes = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_SNDBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    // windows-udp-connreset audit, iocp socket: both ICMP reset reports off;
    // a refusal is logged once and the receive paths skip the statuses
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) iocp->socket );
    if ( bind( iocp->socket, (sockaddr*) &address, address_bytes ) == SOCKET_ERROR ) goto failed;
    address_bytes = sizeof(address);
    if ( getsockname( iocp->socket, (sockaddr*) &address, &address_bytes ) == SOCKET_ERROR ) goto failed;

    // The probes run on the bound socket; a refusal is a tier, not an error.
    // UDP_SEND_MSG_SIZE (IPPROTO_UDP socket options, Winsock reference,
    // learn.microsoft.com): a DWORD; non-zero makes the stack break each sent
    // buffer into messages of that size, the last one possibly shorter; zero,
    // the default, is no segmentation. The probe writes zero.
    if ( ( flags & ROCKETNET_IOCP_DISABLE_USO ) == 0 )
    {
        DWORD zero = 0;
        if ( setsockopt( iocp->socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (const char*) &zero, sizeof(zero) ) != SOCKET_ERROR )
            iocp->features.fetch_or( ROCKETNET_IOCP_FEATURE_USO, std::memory_order_relaxed );
    }
    // UDP_RECV_MAX_COALESCED_SIZE (same page): a DWORD; non-zero lets the stack
    // coalesce datagrams of one source address and port into one message of
    // at most that many bytes, all the same size but a shorter last one; the
    // stride reaches the application only through a receive with control
    // data (WSARecvMsg) as the DWORD control message UDP_COALESCED_INFO.
    if ( ( flags & ROCKETNET_IOCP_DISABLE_URO ) == 0 )
    {
        GUID guid = WSAID_WSARECVMSG;
        DWORD maximum = ROCKETNET_IOCP_URO_BUFFER_BYTES;
        if ( WSAIoctl( iocp->socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &iocp->receive_message,
                       sizeof(iocp->receive_message), &returned_bytes, NULL, NULL ) != SOCKET_ERROR && iocp->receive_message &&
             setsockopt( iocp->socket, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, (const char*) &maximum, sizeof(maximum) ) != SOCKET_ERROR )
        {
            iocp->uro_buffers = (uint8_t*) rocketnet_malloc( context, (size_t) num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * ROCKETNET_IOCP_URO_BUFFER_BYTES );
            if ( !iocp->uro_buffers ) goto failed;
            iocp->features.fetch_or( ROCKETNET_IOCP_FEATURE_URO, std::memory_order_relaxed );
        }
    }

    iocp->port = CreateIoCompletionPort( (HANDLE) iocp->socket, NULL, 0, (DWORD) num_workers );
    if ( !iocp->port || fail_stage == 2 ) goto failed;
    if ( !rocketnet_platform_mutex_create( &iocp->send_mutex ) ) goto failed;
    iocp->send_wake = rocketnet_platform_wake_create( context, NULL );
    if ( !iocp->send_wake ) goto failed;
    iocp->send_exited.store( false, std::memory_order_relaxed );
    iocp->send_thread = rocketnet_platform_thread_create( context, rocketnet_iocp_send_worker, iocp );
    if ( !iocp->send_thread )
    {
        iocp->send_exited.store( true, std::memory_order_relaxed );
        goto failed;
    }
    rocketnet_platform_server_thread_priority( iocp->send_thread );
    for ( int i = 0; i < num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[i];
        worker->owner = iocp;
        if ( !rocketnet_platform_mutex_create( &worker->mutex ) ) goto failed;
        iocp->active_workers.fetch_add( 1, std::memory_order_relaxed );
        worker->thread = rocketnet_platform_thread_create( context, rocketnet_iocp_worker, worker );
        if ( !worker->thread )
        {
            iocp->active_workers.fetch_sub( 1, std::memory_order_relaxed );
            goto failed;
        }
        // each worker takes the server thread priority from this thread,
        // right after its create, as the GSO queue threads do
        rocketnet_platform_server_thread_priority( worker->thread );
        if ( fail_stage == 3 ) goto failed;
    }
    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
    {
        if ( iocp->uro_buffers )
            iocp->receives[i].uro_data = iocp->uro_buffers + (size_t) i * ROCKETNET_IOCP_URO_BUFFER_BYTES;
        if ( !rocketnet_iocp_post_receive( iocp, &iocp->receives[i] ) ) goto failed;
        if ( fail_stage == 4 ) goto failed;
    }
    if ( !rocketnet_iocp_address_read( &address, address_bytes, iocp->ipv6, bind_address ) ) goto failed;
    rocketnet_info( "iocp transport: %d completion workers and one TX worker on port %d, USO %s, URO %s", num_workers, bind_address->port,
        ( iocp->features.load() & ROCKETNET_IOCP_FEATURE_USO ) ? "yes" : "no (WSASendTo per datagram)",
        ( iocp->features.load() & ROCKETNET_IOCP_FEATURE_URO ) ? "yes" : "no (WSARecvFrom per datagram)" );
    return iocp;

failed:
    rocketnet_warn( "iocp transport create failed (Winsock %d, Win32 %lu, test stage %d)", WSAGetLastError(), GetLastError(), fail_stage );
    rocketnet_server_iocp_stop( iocp );
    while ( !rocketnet_server_iocp_stopped( iocp ) ) rocketnet_platform_sleep( 0.001 );
    rocketnet_server_iocp_destroy( iocp );
    return NULL;
}

uint8_t * rocketnet_server_iocp_start_packet( rocketnet_server_iocp_t * iocp, const rocketnet_address_t * to, uint64_t * packet_id )
{
    if ( iocp->stopping.load( std::memory_order_acquire ) ) return NULL;
    SLIST_ENTRY * entry = InterlockedPopEntrySList( &iocp->free_sends );
    if ( !entry ) return NULL;
    rocketnet_iocp_operation_t * op = CONTAINING_RECORD( entry, rocketnet_iocp_operation_t, free_entry );
    op->address_bytes = rocketnet_iocp_address_write( to, iocp->ipv6, &op->address );
    if ( !op->address_bytes )
    {
        InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
        return NULL;
    }
    // Reserve a staging slot only after acquiring a usable frame. A failed
    // pop/address must not consume capacity while completions return frames
    // during this same fill phase. Every accepted slot owns one pool entry.
    const uint64_t slot = iocp->num_staged.fetch_add( 1, std::memory_order_relaxed );
    if ( slot >= ROCKETNET_IOCP_SEND_QUEUE_SIZE )
    {
        InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
        return NULL;
    }
    op->packet_bytes = 0;
    op->to = *to;
    iocp->send_banks[iocp->send_producer].staged[slot] = op->index;
    *packet_id = (uint64_t) op->index;
    return op->frame;
}

void rocketnet_server_iocp_finish_packet( rocketnet_server_iocp_t * iocp, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    rocketnet_assert( packet_id < ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    rocketnet_assert( frame == iocp->sends[packet_id].frame );
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );
    (void) frame;
    iocp->sends[packet_id].packet_bytes = packet_bytes;
}

void rocketnet_server_iocp_abort_packet( rocketnet_server_iocp_t * iocp, uint64_t packet_id, uint8_t * frame )
{
    rocketnet_assert( packet_id < ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    rocketnet_assert( frame == iocp->sends[packet_id].frame );
    (void) frame;
    iocp->sends[packet_id].packet_bytes = 0;
}

// Sets the socket's UDP_SEND_MSG_SIZE when it differs. Only the TX worker calls it.
static bool rocketnet_iocp_set_stride( rocketnet_server_iocp_t * iocp, int stride )
{
    if ( iocp->uso_stride == stride )
        return true;
    DWORD value = (DWORD) stride;
    if ( setsockopt( iocp->socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (const char*) &value, sizeof(value) ) == SOCKET_ERROR )
        return false;
    iocp->uso_stride = stride;
    return true;
}

// The per-datagram send: one frame, one WSASendTo.
static void rocketnet_iocp_post_send( rocketnet_server_iocp_t * iocp, rocketnet_iocp_operation_t * op )
{
    op->run_count = 1;
    op->run_bytes = op->packet_bytes;
    op->run_next = -1;
    // A frame longer than the stride a USO run left on the socket would be
    // broken into segments: clear the stride first.
    if ( iocp->uso_stride != 0 && op->packet_bytes > iocp->uso_stride && !rocketnet_iocp_set_stride( iocp, 0 ) )
    {
        iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( 1, std::memory_order_relaxed );
        InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
        return;
    }
    memset( &op->overlapped, 0, sizeof(op->overlapped) );
    op->buffer.buf = (char*) op->frame + ROCKETNET_FRAME_WIRE_OFFSET;
    op->buffer.len = (ULONG) op->packet_bytes;
    iocp->pending.fetch_add( 1, std::memory_order_relaxed );
    int error = 0;
    int result = SOCKET_ERROR;
    if ( !ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, &error ) )
    {
        result = WSASendTo( iocp->socket, &op->buffer, 1, NULL, 0,
            (sockaddr*) &op->address, op->address_bytes, &op->overlapped, NULL );
        if ( result == SOCKET_ERROR )
            error = WSAGetLastError();
    }
    if ( result == SOCKET_ERROR && error != WSA_IO_PENDING )
    {
        // An unsuccessful submission has no completion. All accepted
        // submissions, including immediate success, retire in the worker.
        iocp->pending.fetch_sub( 1, std::memory_order_release );
        iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( 1, std::memory_order_relaxed );
        InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
    }
}

// A USO run: one overlapped WSASendMsg with one WSABUF per frame, the socket's
// UDP_SEND_MSG_SIZE set to the run's stride before the post. The Winsock
// reference documents the segment size as a socket option only, not as a
// per-send control message, so the stride is the option and a run is one
// stride (the shared run rule's shorter tail is the documented shorter last
// segment). WSASendMsg captures the WSABUF array before it returns (WSASendMsg
// reference), so the array lives on this stack; the WSAMSG lives in the run's
// first frame. Returns false when the host refused USO live: the feature is
// cleared, counted, and the caller sends the run frame by frame.
static bool rocketnet_iocp_post_send_run( rocketnet_server_iocp_t * iocp, const int * staged, int run )
{
    rocketnet_iocp_operation_t * first = &iocp->sends[staged[0]];
    WSABUF buffers[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
    int bytes = 0;
    for ( int k = 0; k < run; ++k )
    {
        rocketnet_iocp_operation_t * op = &iocp->sends[staged[k]];
        buffers[k].buf = (char*) op->frame + ROCKETNET_FRAME_WIRE_OFFSET;
        buffers[k].len = (ULONG) op->packet_bytes;
        bytes += op->packet_bytes;
        op->run_next = k + 1 < run ? staged[k + 1] : -1;
    }
    int error = 0;
    if ( rocketnet_iocp_set_stride( iocp, first->packet_bytes ) )
    {
        first->run_count = run;
        first->run_bytes = bytes;
        memset( &first->overlapped, 0, sizeof(first->overlapped) );
        memset( &first->message, 0, sizeof(first->message) );
        first->message.name = (LPSOCKADDR) &first->address;
        first->message.namelen = first->address_bytes;
        first->message.lpBuffers = buffers;
        first->message.dwBufferCount = (ULONG) run;
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        int result = SOCKET_ERROR;
        if ( !ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, &error ) )
        {
            result = WSASendMsg( iocp->socket, &first->message, 0, NULL, &first->overlapped, NULL );
            if ( result == 0 )
            {
                iocp->counters[ROCKETNET_IOCP_USO_SENDS].fetch_add( 1, std::memory_order_relaxed );
                return true;
            }
            error = WSAGetLastError();
        }
        if ( error == WSA_IO_PENDING )
        {
            iocp->counters[ROCKETNET_IOCP_USO_SENDS].fetch_add( 1, std::memory_order_relaxed );
            return true;
        }
        // An unsuccessful submission has no completion.
        iocp->pending.fetch_sub( 1, std::memory_order_release );
    }
    else
        error = WSAEINVAL;                          // the host refused the stride itself

    if ( error == WSAEINVAL || error == WSAEOPNOTSUPP )
    {
        // As the GSO transport does for a refused live UDP_SEGMENT send:
        // degrade the tier and resend losslessly, never drop the run.
        rocketnet_warn( "iocp transport: live USO send refused (Winsock %d). degrading to WSASendTo per datagram", error );
        iocp->features.fetch_and( ~ROCKETNET_IOCP_FEATURE_USO, std::memory_order_relaxed );
        iocp->counters[ROCKETNET_IOCP_USO_FALLBACKS].fetch_add( 1, std::memory_order_relaxed );
        rocketnet_iocp_set_stride( iocp, 0 );
        return false;
    }
    iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( (uint64_t) run, std::memory_order_relaxed );
    rocketnet_iocp_retire_send( iocp, first );
    return true;
}

static void rocketnet_iocp_send_bank( rocketnet_server_iocp_t * iocp, rocketnet_iocp_send_bank_t * bank )
{
    const uint64_t count = (uint64_t) bank->count;
    if ( iocp->features.load( std::memory_order_relaxed ) & ROCKETNET_IOCP_FEATURE_USO )
    {
        for ( uint64_t i = 0; i < count; ++i )
        {
            iocp->run_bytes[i] = iocp->sends[bank->staged[i]].packet_bytes;
            iocp->run_to[i] = iocp->sends[bank->staged[i]].to;
        }
    }
    for ( uint64_t i = 0; i < count; ++i )
    {
        rocketnet_iocp_operation_t * op = &iocp->sends[bank->staged[i]];
        if ( op->packet_bytes == 0 || iocp->stopping.load( std::memory_order_acquire ) )
        {
            if ( op->packet_bytes != 0 )
                iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( 1, std::memory_order_relaxed );
            InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
            continue;
        }
        if ( iocp->features.load( std::memory_order_relaxed ) & ROCKETNET_IOCP_FEATURE_USO )
        {
            const int run = rocketnet_coalesce_send_run( &iocp->run_bytes[i], &iocp->run_to[i], (int) ( count - i ),
                ROCKETNET_IOCP_USO_MAX_SEGMENTS, ROCKETNET_IOCP_USO_MAX_SUPER_BYTES );
            if ( run > 1 && rocketnet_iocp_post_send_run( iocp, &bank->staged[i], run ) )
            {
                i += (uint64_t) ( run - 1 );
                continue;
            }
        }
        rocketnet_iocp_post_send( iocp, op );
    }
    bank->count = 0;
}


// Publish an already finished producer with a constant-time ownership swap.
// A failed try leaves every frame in the caller-owned bank for the next call.
void rocketnet_server_iocp_send_packets( rocketnet_server_iocp_t * iocp )
{
    if ( iocp->stopping.load( std::memory_order_acquire ) ) return;
    uint64_t count = iocp->num_staged.load( std::memory_order_relaxed );
    if ( count == 0 ) return;
    if ( count > ROCKETNET_IOCP_SEND_QUEUE_SIZE ) count = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
    if ( !rocketnet_platform_mutex_try_acquire( &iocp->send_mutex ) ) return;
    if ( iocp->send_free_count == 0 )
    {
        rocketnet_platform_mutex_release( &iocp->send_mutex );
        return;
    }
    const int producer = iocp->send_producer;
    iocp->send_banks[producer].count = (int) count;
    iocp->send_producer = iocp->send_free[--iocp->send_free_count];
    iocp->send_ready[( iocp->send_ready_head + iocp->send_ready_count ) % ROCKETNET_IOCP_SEND_BANKS] = producer;
    iocp->send_ready_count++;
#if defined(ROCKETNET_IOCP_TESTS)
    iocp->test_pending_tx_banks.fetch_add( 1, std::memory_order_release );
#endif
    rocketnet_platform_mutex_release( &iocp->send_mutex );
    iocp->num_staged.store( 0, std::memory_order_relaxed );
    rocketnet_platform_wake_set( iocp->send_wake );
}

static void rocketnet_iocp_send_worker( void * argument )
{
    rocketnet_server_iocp_t * iocp = (rocketnet_server_iocp_t*) argument;
    for ( ;; )
    {
#if defined(ROCKETNET_IOCP_TESTS)
        for ( ;; )
        {
            while ( iocp->test_hold_tx.load( std::memory_order_acquire ) &&
                    !iocp->stopping.load( std::memory_order_acquire ) )
                rocketnet_platform_sleep( 0.001 );
            iocp->test_active_tx_batches.fetch_add( 1, std::memory_order_acq_rel );
            if ( !iocp->test_hold_tx.load( std::memory_order_acquire ) ||
                  iocp->stopping.load( std::memory_order_acquire ) ) break;
            iocp->test_active_tx_batches.fetch_sub( 1, std::memory_order_release );
        }
#endif
        rocketnet_platform_mutex_acquire( &iocp->send_mutex );
        int index = -1;
        if ( iocp->send_ready_count )
        {
            index = iocp->send_ready[iocp->send_ready_head];
            iocp->send_ready_head = ( iocp->send_ready_head + 1 ) % ROCKETNET_IOCP_SEND_BANKS;
            iocp->send_ready_count--;
        }
        const bool finish = index < 0 && iocp->stopping.load( std::memory_order_acquire );
        rocketnet_platform_mutex_release( &iocp->send_mutex );
        if ( index >= 0 )
        {
            rocketnet_iocp_send_bank( iocp, &iocp->send_banks[index] );
            rocketnet_platform_mutex_acquire( &iocp->send_mutex );
            iocp->send_free[iocp->send_free_count++] = index;
            rocketnet_platform_mutex_release( &iocp->send_mutex );
#if defined(ROCKETNET_IOCP_TESTS)
            iocp->test_pending_tx_banks.fetch_sub( 1, std::memory_order_release );
#endif
        }
        else if ( finish )
        {
            // Stop's release transferred the last unpublished producer. It
            // contains no kernel-owned sends; retire those frames outside the
            // mutex before advertising that no later post is possible.
            rocketnet_iocp_send_bank( iocp, &iocp->send_banks[iocp->send_producer] );
        }
#if defined(ROCKETNET_IOCP_TESTS)
        iocp->test_active_tx_batches.fetch_sub( 1, std::memory_order_release );
#endif
        if ( finish ) break;
        if ( index < 0 ) rocketnet_platform_wake_wait( iocp->send_wake, false, 0.05 );
    }
    iocp->send_exited.store( true, std::memory_order_release );
    for ( int i = 0; i < iocp->num_workers; ++i )
        PostQueuedCompletionStatus( iocp->port, 0, 0, NULL );
}

void rocketnet_server_iocp_receive_packets( rocketnet_server_iocp_t * iocp )
{
    iocp->process.num_packets = 0;
    for ( int q = 0; q < iocp->num_workers; ++q )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[q];
        // Never wait behind a worker or expose the bank it is filling.
        if ( !rocketnet_platform_mutex_try_acquire( &worker->mutex ) )
            continue;
        for ( int j = 0; j < worker->exposed_count; ++j )
        {
            rocketnet_assert( worker->free_count < ROCKETNET_IOCP_RECEIVE_BANKS );
            worker->free_banks[worker->free_count++] = worker->exposed_banks[j];
        }
        worker->exposed_count = 0;
        int write_index = iocp->process.num_packets;
        while ( worker->ready_count > 0 )
        {
            const int candidate = worker->ready_banks[worker->ready_head];
            const int remaining = ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS - iocp->process.num_packets;
            if ( worker->buffers[candidate].num_packets > remaining )
                break; // preserve the oldest complete bank for the next call
            worker->ready_head = ( worker->ready_head + 1 ) % ROCKETNET_IOCP_RECEIVE_BANKS;
            worker->ready_count--;
            worker->exposed_banks[worker->exposed_count++] = candidate;
            iocp->process.num_packets += worker->buffers[candidate].num_packets;
        }
        const int num_exposed = worker->exposed_count;
        int exposed[ROCKETNET_IOCP_RECEIVE_BANKS];
        for ( int j = 0; j < num_exposed; ++j )
            exposed[j] = worker->exposed_banks[j];
        // Reserve process capacity under this lock, then fill those slots
        // outside it. No other caller uses process concurrently.
        rocketnet_platform_mutex_release( &worker->mutex );
        for ( int j = 0; j < num_exposed; ++j )
        {
            rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[exposed[j]];
            for ( int i = 0; i < buffer->num_packets; ++i )
            {
                const int index = write_index++;
                iocp->process.from[index] = buffer->from[i];
                iocp->process.packet_bytes[index] = buffer->packet_bytes[i];
                iocp->process.packet_data[index] = buffer->data[i];
            }
        }
    }
}

rocketnet_server_socket_process_packets_t * rocketnet_server_iocp_process_packets( rocketnet_server_iocp_t * iocp )
{
    return &iocp->process;
}

void rocketnet_server_iocp_counters( rocketnet_server_iocp_t * iocp, uint64_t * counters )
{
    for ( int i = 0; i < ROCKETNET_IOCP_NUM_COUNTERS; ++i ) counters[i] = iocp->counters[i].load( std::memory_order_relaxed );
}

int rocketnet_server_iocp_pending( rocketnet_server_iocp_t * iocp )
{
    return iocp->pending.load( std::memory_order_acquire );
}

int rocketnet_server_iocp_features( rocketnet_server_iocp_t * iocp )
{
    return iocp->features.load( std::memory_order_relaxed );
}

#if defined(ROCKETNET_IOCP_TESTS)
void rocketnet_server_iocp_test_hold_tx( rocketnet_server_iocp_t * iocp, bool hold )
{
    iocp->test_hold_tx.store( hold, std::memory_order_release );
    if ( iocp->send_wake ) rocketnet_platform_wake_set( iocp->send_wake );
    if ( hold )
        while ( iocp->test_active_tx_batches.load( std::memory_order_acquire ) != 0 )
            rocketnet_platform_sleep( 0.001 );
}

void rocketnet_server_iocp_test_hold_tx_publication( rocketnet_server_iocp_t * iocp, bool hold )
{
    if ( hold ) rocketnet_platform_mutex_acquire( &iocp->send_mutex );
    else rocketnet_platform_mutex_release( &iocp->send_mutex );
}

bool rocketnet_server_iocp_test_tx_idle( rocketnet_server_iocp_t * iocp )
{
    // Idle: the producer is published (nothing staged) and every published
    // bank is serviced.
    return iocp->num_staged.load( std::memory_order_relaxed ) == 0 &&
           iocp->test_pending_tx_banks.load( std::memory_order_acquire ) == 0;
}

int rocketnet_server_iocp_test_ready_tx_banks( rocketnet_server_iocp_t * iocp )
{
    rocketnet_platform_mutex_acquire( &iocp->send_mutex );
    const int count = iocp->send_ready_count;
    rocketnet_platform_mutex_release( &iocp->send_mutex );
    return count;
}

// Only the dedicated test executable compiles this helper. The gate pauses
// completion work without making production work run under a publication
// mutex. A hold returns after all earlier batches have retired.
void rocketnet_server_iocp_test_hold_completions( rocketnet_server_iocp_t * iocp, bool hold )
{
    iocp->test_hold_completions.store( hold, std::memory_order_release );
    if ( hold )
        while ( iocp->test_active_completion_batches.load( std::memory_order_acquire ) != 0 )
            rocketnet_platform_sleep( 0.001 );
}

void rocketnet_server_iocp_test_hold_publication( rocketnet_server_iocp_t * iocp, bool hold )
{
    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        if ( hold ) rocketnet_platform_mutex_acquire( &iocp->workers[i].mutex );
        else rocketnet_platform_mutex_release( &iocp->workers[i].mutex );
    }
}

int rocketnet_server_iocp_test_ready_banks( rocketnet_server_iocp_t * iocp, int worker_index )
{
    rocketnet_assert( worker_index >= 0 && worker_index < iocp->num_workers );
    rocketnet_iocp_worker_t * worker = &iocp->workers[worker_index];
    rocketnet_platform_mutex_acquire( &worker->mutex );
    const int count = worker->ready_count;
    rocketnet_platform_mutex_release( &worker->mutex );
    return count;
}

bool rocketnet_server_iocp_test_receive_view_published( rocketnet_server_iocp_t * iocp, const uint8_t * view )
{
    bool published = false;
    const uintptr_t pointer = (uintptr_t) view;
    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[i];
        rocketnet_platform_mutex_acquire( &worker->mutex );
        for ( int j = 0; j < ROCKETNET_IOCP_RECEIVE_BANKS; ++j )
        {
            const uintptr_t begin = (uintptr_t) worker->buffers[j].data;
            if ( pointer >= begin && pointer < begin + sizeof(worker->buffers[j].data) )
                for ( int k = 0; k < worker->exposed_count; ++k )
                    published |= j == worker->exposed_banks[k];
        }
        rocketnet_platform_mutex_release( &worker->mutex );
    }
    return published;
}

bool rocketnet_server_iocp_test_stopping( rocketnet_server_iocp_t * iocp )
{
    return iocp->stopping.load( std::memory_order_acquire );
}

void rocketnet_server_iocp_test_fail_next( rocketnet_server_iocp_t * iocp, int fault, int count, int status )
{
    rocketnet_assert( fault >= 0 && fault < ROCKETNET_IOCP_TEST_NUM_FAULTS && count >= 0 );
    iocp->test_fault_status[fault].store( status, std::memory_order_relaxed );
    iocp->test_faults[fault].store( count, std::memory_order_release );
}

int rocketnet_server_iocp_test_faults_left( rocketnet_server_iocp_t * iocp, int fault )
{
    return iocp->test_faults[fault].load( std::memory_order_acquire );
}
#endif

#endif // ROCKETNET_PLATFORM_WINDOWS

int rocketnet_server_iocp_windows_cpp_dummy = 0;
