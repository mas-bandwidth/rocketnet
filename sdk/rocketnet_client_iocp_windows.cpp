/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The iocp transport's client side: overlapped WSARecvFrom receives on a
// completion port of the client's own, synchronous sends. See
// sdk/rocketnet_client_iocp.h for the shape, docs/CLIENT-IO.md for the I/O
// thread it runs under (proofs/tla/RNClientIo.tla, proofs/tla/RNBufferHandoff.tla), and
// proofs/tla/RNClientIocp.tla for the posted-receive ownership it adds.
//
// The core (the first half) is portable C++ on the port seam and builds on
// every platform, so the unit cases run anywhere; the Win32 binding (the
// second half) builds on Windows only.

#include "rocketnet_internal.h"
#include "rocketnet_client_iocp.h"

#include <string.h>

// proofs/tla/RNClientIocp.tla Post: the kernel owns the buffer until its completion

static void rocketnet_client_iocp_post( rocketnet_client_iocp_t * iocp, int receive )
{
    rocketnet_assert( iocp->state[receive] == ROCKETNET_CLIENT_IOCP_RECEIVE_IDLE );

    if ( iocp->closed )
        return;

    if ( !iocp->port->post_receive( iocp->port_context, receive, iocp->buffers + (size_t) receive * (size_t) iocp->buffer_bytes, iocp->buffer_bytes ) )
    {
        // a refused post started no I/O: the receive stays idle and the next receive_batch posts it again, so a transient refusal never shrinks the receive depth
        iocp->post_refusals++;
        iocp->num_idle++;
        return;
    }

    iocp->state[receive] = (uint8_t) ROCKETNET_CLIENT_IOCP_RECEIVE_POSTED;
    iocp->num_posted++;
}

int rocketnet_client_iocp_init( rocketnet_client_iocp_t * iocp, const rocketnet_client_iocp_port_t * port, void * port_context,
                                uint8_t * buffers, int buffer_bytes, int num_receives )
{
    rocketnet_assert( iocp );
    rocketnet_assert( port );
    rocketnet_assert( buffers );
    rocketnet_assert( buffer_bytes > 0 );
    rocketnet_assert( num_receives > 0 && num_receives <= ROCKETNET_CLIENT_IOCP_RECEIVES );

    iocp->port = port;
    iocp->port_context = port_context;
    iocp->buffers = buffers;
    iocp->buffer_bytes = buffer_bytes;
    iocp->num_receives = num_receives;
    iocp->num_posted = 0;
    iocp->num_idle = 0;
    iocp->closed = false;
    iocp->woken = false;
    iocp->wake_posted.store( false, std::memory_order_relaxed );

    iocp->receives_completed = 0;
    iocp->receives_failed = 0;
    iocp->post_refusals = 0;
    iocp->ready_head = 0;
    iocp->ready_count = 0;

    for ( int i = 0; i < num_receives; i++ )
    {
        iocp->state[i] = (uint8_t) ROCKETNET_CLIENT_IOCP_RECEIVE_IDLE;
        iocp->packet_bytes[i] = 0;
        memset( &iocp->from[i], 0, sizeof( iocp->from[i] ) );
    }

    for ( int i = 0; i < num_receives; i++ )
        rocketnet_client_iocp_post( iocp, i );

    return iocp->num_posted;
}

// proofs/tla/RNClientIocp.tla Dequeue and DequeueWake: one dequeue of every
// completion the port holds (up to one per receive, plus the wake), waiting
// up to timeout_seconds for the first. A receive completion makes its buffer
// ready, in completion order, or idle when it carried no datagram; the wake
// completion sets woken.

static void rocketnet_client_iocp_dequeue( rocketnet_client_iocp_t * iocp, double timeout_seconds )
{
    const int max_completions = iocp->num_receives + 1;
    const int count = iocp->port->dequeue( iocp->port_context, iocp->completions, max_completions, timeout_seconds );
    rocketnet_assert( count >= 0 && count <= max_completions );

    for ( int i = 0; i < count; i++ )
    {
        const rocketnet_client_iocp_completion_t * completion = &iocp->completions[i];
        if ( completion->kind == ROCKETNET_CLIENT_IOCP_COMPLETION_WAKE )
        {
            // clear before the wait returns: a wake set after this posts a new completion, and one set before it is the one consumed here
            iocp->wake_posted.exchange( false, std::memory_order_acq_rel );

            iocp->woken = true;
            continue;
        }

        const int receive = completion->receive;
        rocketnet_assert( receive >= 0 && receive < iocp->num_receives );
        rocketnet_assert( iocp->state[receive] == ROCKETNET_CLIENT_IOCP_RECEIVE_POSTED );

        iocp->num_posted--;
        if ( completion->ok && completion->packet_bytes > 0 && completion->packet_bytes <= iocp->buffer_bytes )
        {
            iocp->state[receive] = (uint8_t) ROCKETNET_CLIENT_IOCP_RECEIVE_READY;
            iocp->packet_bytes[receive] = completion->packet_bytes;
            iocp->from[receive] = completion->from;
            rocketnet_assert( iocp->ready_count < iocp->num_receives );

            iocp->ready[( iocp->ready_head + iocp->ready_count ) % iocp->num_receives] = receive;
            iocp->ready_count++;
        }
        else
        {
            // an error, an abort, an unreadable source or an empty datagram:
            // nothing to copy, so the buffer goes back to the kernel
            if ( !iocp->closed )
                iocp->receives_failed++;

            iocp->state[receive] = (uint8_t) ROCKETNET_CLIENT_IOCP_RECEIVE_IDLE;
            rocketnet_client_iocp_post( iocp, receive );
        }
    }
}

// receives a refused post left idle, posted again

static void rocketnet_client_iocp_retry_idle( rocketnet_client_iocp_t * iocp )
{
    if ( iocp->num_idle == 0 || iocp->closed )
        return;

    iocp->num_idle = 0;
    for ( int i = 0; i < iocp->num_receives; i++ )
    {
        if ( iocp->state[i] == ROCKETNET_CLIENT_IOCP_RECEIVE_IDLE )
            rocketnet_client_iocp_post( iocp, i );
    }
}

// proofs/tla/RNClientIocp.tla Drain: the copy reads only a ready buffer, then Post
// gives it back to the kernel

int rocketnet_client_iocp_receive_batch( rocketnet_client_iocp_t * iocp, rocketnet_address_t * from, uint8_t * packet_data,
                                         int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_assert( iocp );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_stride > 0 );
    rocketnet_assert( packet_bytes );

    rocketnet_client_iocp_retry_idle( iocp );

    if ( iocp->ready_count < max_packets )
        rocketnet_client_iocp_dequeue( iocp, 0.0 );

    int num_packets = 0;
    while ( num_packets < max_packets && iocp->ready_count > 0 )
    {
        const int receive = iocp->ready[iocp->ready_head];

        iocp->ready_head = ( iocp->ready_head + 1 ) % iocp->num_receives;
        iocp->ready_count--;
        rocketnet_assert( iocp->state[receive] == ROCKETNET_CLIENT_IOCP_RECEIVE_READY );

        const int bytes = iocp->packet_bytes[receive];
        const int copy = bytes < packet_stride ? bytes : packet_stride;
        memcpy( packet_data + (size_t) num_packets * (size_t) packet_stride, iocp->buffers + (size_t) receive * (size_t) iocp->buffer_bytes, (size_t) copy );

        from[num_packets] = iocp->from[receive];
        packet_bytes[num_packets] = bytes;
        num_packets++;
        iocp->receives_completed++;

        iocp->state[receive] = (uint8_t) ROCKETNET_CLIENT_IOCP_RECEIVE_IDLE;
        rocketnet_client_iocp_post( iocp, receive );
    }

    return num_packets;
}

// proofs/tla/RNClientIo.tla IoWait: the wait on the port; DequeueWake consumes the wake

bool rocketnet_client_iocp_wait( rocketnet_client_iocp_t * iocp, bool readable, double timeout_seconds )
{
    rocketnet_assert( iocp );

    if ( !iocp->woken && !( readable && iocp->ready_count > 0 ) )
        rocketnet_client_iocp_dequeue( iocp, timeout_seconds > 0.0 ? timeout_seconds : 0.0 );

    if ( iocp->woken )
    {
        iocp->woken = false;
        return true;
    }

    return readable && iocp->ready_count > 0;
}

// proofs/tla/RNClientIocp.tla Wake: at most one wake completion is on the port

void rocketnet_client_iocp_wake( rocketnet_client_iocp_t * iocp )
{
    rocketnet_assert( iocp );

    if ( !iocp->wake_posted.exchange( true, std::memory_order_acq_rel ) )
    {
        // a refused post leaves no completion on the port: clear the flag so
        // the next wake posts again instead of every later wake being
        // swallowed for the life of the client

        if ( !iocp->port->post_wake( iocp->port_context ) )
            iocp->wake_posted.store( false, std::memory_order_release );
    }
}

// proofs/tla/RNClientIocp.tla Close, then Dequeue until Drained: Free is the caller's,
// and only on true (FreedOnlyWhenDrained)

bool rocketnet_client_iocp_close( rocketnet_client_iocp_t * iocp, int max_waits, double wait_seconds )
{
    rocketnet_assert( iocp );

    if ( !iocp->closed )
    {
        iocp->closed = true;
        iocp->port->close( iocp->port_context );
    }

    for ( int i = 0; i < max_waits && iocp->num_posted > 0; i++ )
        rocketnet_client_iocp_dequeue( iocp, wait_seconds );

    return iocp->num_posted == 0;
}

// ---------------------------------------------------------------------------
// The Win32 binding: the platform socket, a completion port of its own, and
// the client transport operations.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#include "rocketnet_constants.h"
#include "rocketnet_driver.h"
#include "rocketnet_memory.h"
#include "rocketnet_platform.h"

#include <ws2tcpip.h>
#include <new>

#define ROCKETNET_CLIENT_IOCP_RECEIVE_KEY ( (ULONG_PTR) 1 )
#define ROCKETNET_CLIENT_IOCP_WAKE_KEY ( (ULONG_PTR) 2 )

// one posted receive: the overlapped and everything WSARecvFrom writes
// until its completion, never on a stack
struct rocketnet_client_iocp_receive_t
{
    OVERLAPPED overlapped;
    WSABUF buffer;
    DWORD flags;
    INT from_bytes;
    sockaddr_storage from;
    int index;
};

struct rocketnet_client_iocp_io_t
{
    void * context;
    rocketnet_platform_socket_t * socket;
    bool ipv6;
    HANDLE port;
    rocketnet_client_iocp_t core;
    rocketnet_client_iocp_receive_t receives[ROCKETNET_CLIENT_IOCP_RECEIVES];
    OVERLAPPED_ENTRY entries[ROCKETNET_CLIENT_IOCP_RECEIVES + 1];
    uint8_t buffers[ROCKETNET_CLIENT_IOCP_RECEIVES * ROCKETNET_CLIENT_IOCP_RECEIVE_BYTES];
};

static bool rocketnet_client_iocp_address_read( const sockaddr_storage * storage, int bytes, bool dual_stack, rocketnet_address_t * address )
{
    memset( address, 0, sizeof( *address ) );

    if ( ( storage->ss_family == AF_INET && bytes < (int) sizeof( sockaddr_in ) ) ||
         ( storage->ss_family == AF_INET6 && bytes < (int) sizeof( sockaddr_in6 ) ) )
        return false;

    return rocketnet_platform_sockaddr_to_address( storage, dual_stack, address );
}

static bool rocketnet_client_iocp_win32_post_receive( void * context, int receive, uint8_t * buffer, int buffer_bytes )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) context;
    rocketnet_client_iocp_receive_t * op = &client->receives[receive];
    memset( &op->overlapped, 0, sizeof( op->overlapped ) );
    op->buffer.buf = (char *) buffer;
    op->buffer.len = (ULONG) buffer_bytes;
    op->flags = 0;
    op->from_bytes = (INT) sizeof( op->from );
    op->index = receive;

    const int result = WSARecvFrom( (SOCKET) client->socket->handle, &op->buffer, 1, NULL, &op->flags,
                                    (sockaddr *) &op->from, &op->from_bytes, &op->overlapped, NULL );
    if ( result == 0 )
        return true; // completed at once; the completion is still queued on the port

    const int error = WSAGetLastError();
    if ( error == WSA_IO_PENDING )
        return true;

    rocketnet_debug( "iocp client: WSARecvFrom post failed: %d", error );

    return false;
}

static int rocketnet_client_iocp_win32_dequeue( void * context, rocketnet_client_iocp_completion_t * completions, int max_completions, double timeout_seconds )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) context;

    rocketnet_assert( max_completions <= ROCKETNET_CLIENT_IOCP_RECEIVES + 1 );

    DWORD timeout_ms = 0;

    if ( timeout_seconds > 0.0 )
    {
        const double ms = timeout_seconds * 1000.0 + 0.999;
        timeout_ms = ms >= 4294967294.0 ? (DWORD) 4294967294UL : (DWORD) ms;
    }

    ULONG count = 0;
    if ( !GetQueuedCompletionStatusEx( client->port, client->entries, (ULONG) max_completions, &count, timeout_ms, FALSE ) )
    {
        const DWORD error = GetLastError();
        if ( error != WAIT_TIMEOUT )
            rocketnet_debug( "iocp client: completion dequeue failed: %lu", error );

        return 0;
    }

    for ( ULONG i = 0; i < count; i++ )
    {
        const OVERLAPPED_ENTRY * entry = &client->entries[i];
        rocketnet_client_iocp_completion_t * completion = &completions[i];

        if ( entry->lpCompletionKey == ROCKETNET_CLIENT_IOCP_WAKE_KEY || !entry->lpOverlapped )
        {
            completion->kind = ROCKETNET_CLIENT_IOCP_COMPLETION_WAKE;
            completion->receive = -1;
            completion->ok = false;
            completion->packet_bytes = 0;
            continue;
        }

        const rocketnet_client_iocp_receive_t * op = CONTAINING_RECORD( entry->lpOverlapped, rocketnet_client_iocp_receive_t, overlapped );

        completion->kind = ROCKETNET_CLIENT_IOCP_COMPLETION_RECEIVE;
        completion->receive = op->index;
        completion->packet_bytes = (int) entry->dwNumberOfBytesTransferred;

        // Internal is the completion's status: zero is success; an ICMP report, a datagram larger than the buffer and closesocket's abort all carry nothing to copy

        completion->ok = entry->Internal == 0 &&
                         rocketnet_client_iocp_address_read( &op->from, op->from_bytes, client->ipv6, &completion->from );
    }

    return (int) count;
}

static bool rocketnet_client_iocp_win32_post_wake( void * context )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) context;
    if ( PostQueuedCompletionStatus( client->port, 0, ROCKETNET_CLIENT_IOCP_WAKE_KEY, NULL ) )
        return true;

    rocketnet_debug( "iocp client: wake post failed: %lu", GetLastError() );

    return false;
}

// closesocket aborts every posted receive; each still completes on the port,
// which stays open until the drain is done. The socket's memory stays for
// destroy, on the caller's thread.

static void rocketnet_client_iocp_win32_close( void * context )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) context;
    rocketnet_platform_socket_close( client->socket );
}

static const rocketnet_client_iocp_port_t rocketnet_client_iocp_win32_port = {
    rocketnet_client_iocp_win32_post_receive,
    rocketnet_client_iocp_win32_dequeue,
    rocketnet_client_iocp_win32_post_wake,
    rocketnet_client_iocp_win32_close,
};

// RNClientIo initial (docs/CLIENT-IO.md): create owns the socket, its port and
// every receive buffer, in one allocation; any failure releases what it opened.

static void * rocketnet_client_iocp_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;

    void * memory = rocketnet_malloc( context, sizeof( rocketnet_client_iocp_io_t ) );
    if ( !memory )
    {
        rocketnet_create_status( config->status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    rocketnet_client_iocp_io_t * client = new ( memory ) rocketnet_client_iocp_io_t();
    client->context = context;
    client->ipv6 = bind->type == ROCKETNET_ADDRESS_IPV6;

    client->socket = rocketnet_platform_socket_create( context, bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                       ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, config->status );
    if ( !client->socket )
    {
        client->~rocketnet_client_iocp_io_t();

        rocketnet_free( context, memory );
        return NULL;
    }

    // the client's own port, never the server's

    client->port = CreateIoCompletionPort( (HANDLE) client->socket->handle, NULL, ROCKETNET_CLIENT_IOCP_RECEIVE_KEY, 1 );
    if ( !client->port )
    {
        rocketnet_warn( "iocp client: CreateIoCompletionPort failed: %lu", GetLastError() );

        rocketnet_platform_socket_destroy( client->socket );

        client->~rocketnet_client_iocp_io_t();

        rocketnet_free( context, memory );
        return NULL;
    }

    const int posted = rocketnet_client_iocp_init( &client->core, &rocketnet_client_iocp_win32_port, client,
                                                   client->buffers, ROCKETNET_CLIENT_IOCP_RECEIVE_BYTES, ROCKETNET_CLIENT_IOCP_RECEIVES );
    if ( posted == 0 )
    {
        rocketnet_warn( "iocp client: no receive could be posted" );

        rocketnet_client_iocp_close( &client->core, 0, 0.0 );

        CloseHandle( client->port );

        rocketnet_platform_socket_destroy( client->socket );

        client->~rocketnet_client_iocp_io_t();

        rocketnet_free( context, memory );
        return NULL;
    }

    return client;
}

// proofs/tla/RNClientIocp.tla Close and the drain, then the port: every handle,
// and no memory. A drain that does not finish within max_waits keeps the
// port open, and destroy drains again.

static bool rocketnet_client_iocp_close_handles( rocketnet_client_iocp_io_t * client, int max_waits, double wait_seconds )
{
    if ( !rocketnet_client_iocp_close( &client->core, max_waits, wait_seconds ) )
        return false;

    if ( client->port )
    {
        CloseHandle( client->port );
        client->port = NULL;
    }

    return true;
}

// The family race closes its losing family here, on the reading thread
// (proofs/tla/RNClientRace.tla ReaderStep): the socket closes and the receives it
// aborted are taken as far as they have already completed, with no wait,
// so the kept family's I/O never stalls behind the drain; the port stays
// open until they all have, and destroy, on the caller's thread, drains
// the rest within its bound and frees.

static void rocketnet_client_iocp_close_operation( void * transport )
{
    rocketnet_client_iocp_close_handles( (rocketnet_client_iocp_io_t *) transport, 0, 0.0 );
}

// RNClientIo Stop and destroy order (docs/CLIENT-IO.md): the I/O thread is
// joined before this; proofs/tla/RNClientIocp.tla Close, the drain, then Free only
// when every posted receive has completed.

static void rocketnet_client_iocp_destroy( void * transport )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    if ( !client )
        return;

    void * context = client->context;

    if ( !rocketnet_client_iocp_close_handles( client, ROCKETNET_CLIENT_IOCP_DRAIN_WAITS, ROCKETNET_CLIENT_IOCP_DRAIN_SECONDS ) )
    {
        // the kernel may still write these buffers: leak them, never free
        rocketnet_error( "iocp client: %d posted receives did not complete after close; leaking the transport", client->core.num_posted );
        return;
    }

    rocketnet_platform_socket_destroy( client->socket );

    client->~rocketnet_client_iocp_io_t();

    rocketnet_free( context, client );
}

static void rocketnet_client_iocp_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    rocketnet_platform_socket_send_packet( client->socket, to, data, bytes );
}

static int rocketnet_client_iocp_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    int bytes = 0;
    if ( rocketnet_client_iocp_receive_batch( &client->core, from, data, capacity, &bytes, 1 ) == 0 )
        return 0;

    return bytes <= capacity ? bytes : 0;
}

// RNClientIo IoSend: synchronous WSASendTo on the connected socket, one per
// datagram (overlapped send is rn-win-client-overlapped-send)

static void rocketnet_client_iocp_send_batch( void * transport, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    rocketnet_platform_socket_send_packets( client->socket, sends, num_packets );
}

// RNClientIo IoReceive: the completed receives, re-posted after the copy

static int rocketnet_client_iocp_receive_batch_operation( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    return rocketnet_client_iocp_receive_batch( &client->core, from, packet_data, packet_stride, packet_bytes, max_packets );
}

// RNClientIo IoWait / waitReadable and the wake (docs/CLIENT-IO.md The wake)

static bool rocketnet_client_iocp_wait_operation( void * transport, bool readable, double timeout_seconds )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    return rocketnet_client_iocp_wait( &client->core, readable, timeout_seconds );
}

static void rocketnet_client_iocp_wake_operation( void * transport )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    rocketnet_client_iocp_wake( &client->core );
}

static uint64_t rocketnet_client_iocp_send_failures( void * transport )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    return client->socket ? rocketnet_platform_socket_send_failures( client->socket ) : 0;
}

// the client trusts only the server: the socket is connected to it, so the
// kernel drops datagrams from any other source (TRANSPORT.md t11)

static bool rocketnet_client_iocp_connect( void * transport, const rocketnet_address_t * server_address )
{
    rocketnet_client_iocp_io_t * client = (rocketnet_client_iocp_io_t *) transport;
    return rocketnet_platform_socket_connect( client->socket, server_address );
}

const rocketnet_client_transport_io_operations_t rocketnet_client_iocp_operations = {
    rocketnet_client_iocp_create, rocketnet_client_iocp_destroy, rocketnet_client_iocp_send, rocketnet_client_iocp_receive,
    rocketnet_client_iocp_send_batch, rocketnet_client_iocp_receive_batch_operation, rocketnet_client_iocp_wait_operation,
    rocketnet_client_iocp_wake_operation, rocketnet_client_iocp_send_failures, rocketnet_client_iocp_connect,
    rocketnet_client_iocp_close_operation
};

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
