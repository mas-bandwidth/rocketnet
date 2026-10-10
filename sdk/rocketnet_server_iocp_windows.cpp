/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "rocketnet_server_iocp.h"
#include "rocketnet_platform.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <atomic>
#include <new>
#include <stddef.h>
#include <string.h>

struct alignas(MEMORY_ALLOCATION_ALIGNMENT) rocketnet_iocp_operation_t
{
    OVERLAPPED overlapped;
    SLIST_ENTRY free_entry;
    WSABUF buffer;
    sockaddr_storage address;
    int address_bytes;
    DWORD flags;
    int index;
    bool send;
    int packet_bytes;
    alignas(16) uint8_t frame[ROCKETNET_FRAME_STRIDE];
};

static_assert( offsetof( rocketnet_iocp_operation_t, overlapped ) == 0, "completion identifies its operation" );
static_assert( offsetof( rocketnet_iocp_operation_t, free_entry ) % MEMORY_ALLOCATION_ALIGNMENT == 0, "Windows SList alignment" );
static_assert( ROCKETNET_RECEIVE_CEILING + 1 <= ROCKETNET_FRAME_STRIDE, "receive detects oversize datagrams" );

struct rocketnet_iocp_receive_buffer_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE];
    int packet_bytes[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE];
    alignas(16) uint8_t data[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];
};

struct alignas(64) rocketnet_iocp_worker_t
{
    rocketnet_server_iocp_t * owner;
    rocketnet_platform_thread_t * thread;
    // This mutex also joins a worker's reposts before shutdown cancels I/O.
    // Receive publication takes it once per completion batch, not per packet.
    rocketnet_platform_mutex_t mutex;
    uint64_t receive_counter;
    rocketnet_iocp_receive_buffer_t buffers[2];
};

struct rocketnet_server_iocp_t
{
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
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER free_sends;
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER retry_receives;
    alignas(64) std::atomic<uint64_t> num_staged{0};
    int staged[ROCKETNET_IOCP_SEND_QUEUE_SIZE];
    std::atomic<uint64_t> counters[ROCKETNET_IOCP_NUM_COUNTERS]{};
    rocketnet_server_socket_process_packets_t process;
};

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

static bool rocketnet_iocp_address_read( const sockaddr_storage * storage, int bytes, rocketnet_address_t * address )
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
        if ( rocketnet_address_is_ipv4_in_ipv6( address ) )
            rocketnet_address_convert_ipv6_to_ipv4( address );
        return true;
    }
    return false;
}

static bool rocketnet_iocp_post_receive( rocketnet_server_iocp_t * iocp, rocketnet_iocp_operation_t * op )
{
    // The caller holds its worker mutex, or is the creating thread. stop sets
    // stopping then joins all worker mutexes before CancelIoEx: no repost can
    // slip past cancellation. Stack addresses/flags must not back pending I/O.
    if ( iocp->stopping.load( std::memory_order_acquire ) )
        return false;
    memset( &op->overlapped, 0, sizeof(op->overlapped) );
    op->address_bytes = sizeof(op->address);
    op->flags = 0;
    op->buffer.buf = (char*) op->frame;
    op->buffer.len = ROCKETNET_RECEIVE_CEILING + 1;
    iocp->pending.fetch_add( 1, std::memory_order_relaxed );
    const int result = WSARecvFrom( iocp->socket, &op->buffer, 1, NULL, &op->flags,
        (sockaddr*) &op->address, &op->address_bytes, &op->overlapped, NULL );
    if ( result == 0 || WSAGetLastError() == WSA_IO_PENDING )
        return true; // Immediate success also produces exactly one IOCP event.
    iocp->pending.fetch_sub( 1, std::memory_order_release );
    iocp->counters[ROCKETNET_IOCP_RECEIVE_REPOST_FAILED].fetch_add( 1, std::memory_order_relaxed );
    InterlockedPushEntrySList( &iocp->retry_receives, &op->free_entry );
    return false;
}

static void rocketnet_iocp_worker( void * argument )
{
    rocketnet_iocp_worker_t * worker = (rocketnet_iocp_worker_t*) argument;
    rocketnet_server_iocp_t * iocp = worker->owner;
    for ( ;; )
    {
        if ( iocp->stopping.load( std::memory_order_acquire ) && iocp->pending.load( std::memory_order_acquire ) == 0 )
            break;

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

        rocketnet_platform_mutex_acquire( &worker->mutex );
        rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[worker->receive_counter % 2];
        for ( ULONG i = 0; i < count; ++i )
        {
            if ( !entries[i].lpOverlapped )
                continue; // A shutdown wakeup, not an I/O operation.
            rocketnet_iocp_operation_t * op = (rocketnet_iocp_operation_t*) entries[i].lpOverlapped;
            const bool success = entries[i].Internal == 0;
            const DWORD bytes = entries[i].dwNumberOfBytesTransferred;
            if ( op->send )
            {
                const int counter = success && bytes == (DWORD) op->packet_bytes ? ROCKETNET_IOCP_SEND_COMPLETED : ROCKETNET_IOCP_SEND_FAILED;
                iocp->counters[counter].fetch_add( 1, std::memory_order_relaxed );
                iocp->pending.fetch_sub( 1, std::memory_order_release );
                InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
                continue;
            }

            rocketnet_address_t from;
            const bool running = !iocp->stopping.load( std::memory_order_acquire );
            if ( running && success && bytes > 0 && bytes <= ROCKETNET_RECEIVE_CEILING &&
                 buffer->num_packets < ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE &&
                 rocketnet_iocp_address_read( &op->address, op->address_bytes, &from ) )
            {
                const int index = buffer->num_packets++;
                buffer->from[index] = from;
                buffer->packet_bytes[index] = (int) bytes;
                memcpy( buffer->data[index], op->frame, bytes );
                iocp->counters[ROCKETNET_IOCP_RECEIVE_COMPLETED].fetch_add( 1, std::memory_order_relaxed );
            }
            else if ( running )
                iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( 1, std::memory_order_relaxed );

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
        rocketnet_platform_mutex_release( &worker->mutex );
    }
    iocp->active_workers.fetch_sub( 1, std::memory_order_release );
}

void rocketnet_server_iocp_stop( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp || iocp->stopping.exchange( true, std::memory_order_acq_rel ) )
        return;
    if ( iocp->workers )
        for ( int i = 0; i < iocp->num_workers; ++i )
            if ( iocp->workers[i].mutex.ok )
            {
                rocketnet_platform_mutex_acquire( &iocp->workers[i].mutex );
                rocketnet_platform_mutex_release( &iocp->workers[i].mutex );
            }
    if ( iocp->socket != INVALID_SOCKET )
    {
        // Cancellation is only a request. The workers drain every completion;
        // the socket, OVERLAPPEDs and data remain alive until the joins below.
        if ( !CancelIoEx( (HANDLE) iocp->socket, NULL ) && GetLastError() != ERROR_NOT_FOUND )
            rocketnet_error( "IOCP cancellation failed: %lu", GetLastError() );
    }
    if ( iocp->port )
        for ( int i = 0; i < iocp->num_workers; ++i )
            PostQueuedCompletionStatus( iocp->port, 0, 0, NULL );
}

bool rocketnet_server_iocp_stopped( rocketnet_server_iocp_t * iocp )
{
    return iocp->stopping.load( std::memory_order_acquire ) &&
           iocp->pending.load( std::memory_order_acquire ) == 0 &&
           iocp->active_workers.load( std::memory_order_acquire ) == 0;
}

void rocketnet_server_iocp_destroy( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp ) return;
    rocketnet_server_iocp_stop( iocp );
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
    iocp->~rocketnet_server_iocp_t();
    rocketnet_free( context, iocp );
}

uint64_t rocketnet_server_iocp_allocation_bytes(int workers)
{
    if (workers < 1 || workers > ROCKETNET_IOCP_MAX_WORKERS) return 0;
    uint64_t bytes = rocketnet_memory_allocations({sizeof(rocketnet_server_iocp_t),
        (uint64_t)workers * sizeof(rocketnet_iocp_worker_t),
        (uint64_t)ROCKETNET_IOCP_SEND_QUEUE_SIZE * sizeof(rocketnet_iocp_operation_t),
        (uint64_t)workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * sizeof(rocketnet_iocp_operation_t)});
    if (!bytes || !rocketnet_memory_add(bytes, (uint64_t)workers, rocketnet_platform_thread_allocation_bytes())) return 0;
    return bytes <= SIZE_MAX ? bytes : 0;
}

rocketnet_server_iocp_t * rocketnet_server_iocp_create( void * context, rocketnet_address_t * bind_address, int num_workers, int fail_stage )
{
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) ) return NULL;
    if ( num_workers == 0 ) num_workers = ROCKETNET_IOCP_DEFAULT_WORKERS;
    if ( num_workers < 1 || num_workers > ROCKETNET_IOCP_MAX_WORKERS )
    {
        rocketnet_warn( "iocp driver refused: worker count %d is outside 1..%d", num_workers, ROCKETNET_IOCP_MAX_WORKERS );
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
    BOOL connection_reset = FALSE;

    iocp->workers = (rocketnet_iocp_worker_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_worker_t) * num_workers );
    if ( !iocp->workers ) goto failed;
    for ( int i = 0; i < num_workers; ++i ) new (&iocp->workers[i]) rocketnet_iocp_worker_t();

    iocp->sends = (rocketnet_iocp_operation_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_operation_t) * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    if ( !iocp->sends ) goto failed;
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        rocketnet_iocp_operation_t * op = new (&iocp->sends[i]) rocketnet_iocp_operation_t();
        op->index = i;
        op->send = true;
        InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
    }
    iocp->receives = (rocketnet_iocp_operation_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_operation_t) * num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    if ( !iocp->receives ) goto failed;
    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i ) new (&iocp->receives[i]) rocketnet_iocp_operation_t();

    iocp->socket = WSASocketW( iocp->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED );
    if ( iocp->socket == INVALID_SOCKET || fail_stage == 1 ) goto failed;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &enabled, sizeof(enabled) ) == SOCKET_ERROR ) goto failed;
    if ( iocp->ipv6 && setsockopt( iocp->socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &v6_only, sizeof(v6_only) ) == SOCKET_ERROR ) goto failed;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_RCVBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    buffer_bytes = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_SNDBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    if ( WSAIoctl( iocp->socket, SIO_UDP_CONNRESET, &connection_reset, sizeof(connection_reset), NULL, 0, &returned_bytes, NULL, NULL ) == SOCKET_ERROR ) goto failed;
    if ( bind( iocp->socket, (sockaddr*) &address, address_bytes ) == SOCKET_ERROR ) goto failed;
    address_bytes = sizeof(address);
    if ( getsockname( iocp->socket, (sockaddr*) &address, &address_bytes ) == SOCKET_ERROR ) goto failed;

    iocp->port = CreateIoCompletionPort( (HANDLE) iocp->socket, NULL, 0, (DWORD) num_workers );
    if ( !iocp->port || fail_stage == 2 ) goto failed;
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
        if ( fail_stage == 3 ) goto failed;
    }
    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
    {
        if ( !rocketnet_iocp_post_receive( iocp, &iocp->receives[i] ) ) goto failed;
        if ( fail_stage == 4 ) goto failed;
    }
    if ( !rocketnet_iocp_address_read( &address, address_bytes, bind_address ) ) goto failed;
    return iocp;

failed:
    rocketnet_warn( "iocp driver create failed (Winsock %d, Win32 %lu, test stage %d)", WSAGetLastError(), GetLastError(), fail_stage );
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
    iocp->staged[slot] = op->index;
    *packet_id = (uint64_t) op->index;
    return op->frame;
}

void rocketnet_server_iocp_finish_packet( rocketnet_server_iocp_t * iocp, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    rocketnet_assert( packet_id < ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    rocketnet_assert( frame == iocp->sends[packet_id].frame );
    rocketnet_assert( packet_bytes > 0 );

    // Drop oversized packets instead of asserting (release builds strip assertions)
    if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return;

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

void rocketnet_server_iocp_send_packets( rocketnet_server_iocp_t * iocp )
{
    uint64_t count = iocp->num_staged.load( std::memory_order_relaxed );
    if ( count > ROCKETNET_IOCP_SEND_QUEUE_SIZE ) count = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
    for ( uint64_t i = 0; i < count; ++i )
    {
        if ( iocp->staged[i] < 0 ) continue;
        rocketnet_iocp_operation_t * op = &iocp->sends[iocp->staged[i]];
        if ( op->packet_bytes == 0 || iocp->stopping.load( std::memory_order_acquire ) )
        {
            InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
            continue;
        }
        memset( &op->overlapped, 0, sizeof(op->overlapped) );
        op->buffer.buf = (char*) op->frame + ROCKETNET_FRAME_WIRE_OFFSET;
        op->buffer.len = (ULONG) op->packet_bytes;
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        const int result = WSASendTo( iocp->socket, &op->buffer, 1, NULL, 0,
            (sockaddr*) &op->address, op->address_bytes, &op->overlapped, NULL );
        if ( result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING )
        {
            // An unsuccessful submission has no completion. All accepted
            // submissions, including immediate success, retire in the worker.
            iocp->pending.fetch_sub( 1, std::memory_order_release );
            iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( 1, std::memory_order_relaxed );
            InterlockedPushEntrySList( &iocp->free_sends, &op->free_entry );
        }
    }
    iocp->num_staged.store( 0, std::memory_order_relaxed );
}

void rocketnet_server_iocp_receive_packets( rocketnet_server_iocp_t * iocp )
{
    iocp->process.num_packets = 0;
    for ( int q = 0; q < iocp->num_workers; ++q )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[q];
        rocketnet_platform_mutex_acquire( &worker->mutex );
        ++worker->receive_counter;
        rocketnet_platform_mutex_release( &worker->mutex );
        rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[(worker->receive_counter + 1) % 2];
        for ( int i = 0; i < buffer->num_packets; ++i )
        {
            if ( iocp->process.num_packets == ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS )
            {
                iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( buffer->num_packets - i, std::memory_order_relaxed );
                break;
            }
            const int index = iocp->process.num_packets++;
            iocp->process.from[index] = buffer->from[i];
            iocp->process.packet_bytes[index] = buffer->packet_bytes[i];
            iocp->process.packet_data[index] = buffer->data[i];
        }
        buffer->num_packets = 0;
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

#if defined(ROCKETNET_IOCP_TESTS)
// Only the dedicated test executable compiles this helper. Holding existing
// publication mutexes deterministically delays retirement without adding a
// test branch to the production completion path.
void rocketnet_server_iocp_test_hold_completions( rocketnet_server_iocp_t * iocp, bool hold )
{
    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        if ( hold ) rocketnet_platform_mutex_acquire( &iocp->workers[i].mutex );
        else rocketnet_platform_mutex_release( &iocp->workers[i].mutex );
    }
}

bool rocketnet_server_iocp_test_receive_view_published( rocketnet_server_iocp_t * iocp, const uint8_t * view )
{
    bool published = false;
    const uintptr_t pointer = (uintptr_t) view;
    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[i];
        rocketnet_platform_mutex_acquire( &worker->mutex );
        for ( int j = 0; j < 2; ++j )
        {
            const uintptr_t begin = (uintptr_t) worker->buffers[j].data;
            if ( pointer >= begin && pointer < begin + sizeof(worker->buffers[j].data) )
                published = j != (int) (worker->receive_counter % 2);
        }
        rocketnet_platform_mutex_release( &worker->mutex );
    }
    return published;
}

bool rocketnet_server_iocp_test_stopping( rocketnet_server_iocp_t * iocp )
{
    return iocp->stopping.load( std::memory_order_acquire );
}
#endif

#endif // ROCKETNET_PLATFORM_WINDOWS

int rocketnet_server_iocp_windows_cpp_dummy = 0;
