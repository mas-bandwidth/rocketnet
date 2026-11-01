/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gso transport's client side: sendmmsg with UDP_SEGMENT on send, recvmmsg
// with UDP_GRO on receive. See sdk/rocketnet_client_gso.h for the shape, docs/CLIENT-IO.md
// for the I/O thread it runs under (proofs/tla/RNClientIo.tla, proofs/tla/RNBufferHandoff.tla).

#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_client_gso.h"
#include "rocketnet_coalesce.h"
#include "rocketnet_constants.h"
#include "rocketnet_driver.h"
#include "rocketnet_memory.h"
#include "rocketnet_platform.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <new>
#include <string.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

static int rocketnet_client_gso_kernel_sendmmsg( void *, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    return sendmmsg( socket, messages, num_messages, 0 );
}

// segment size zero is the disabled value the socket already holds: the
// probe asks whether the option exists and changes nothing

static int rocketnet_client_gso_kernel_segment_probe( void *, int socket )
{
    int zero = 0;
    return setsockopt( socket, IPPROTO_UDP, UDP_SEGMENT, &zero, sizeof( zero ) );
}

static int rocketnet_client_gso_kernel_recvmmsg( void *, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    return recvmmsg( socket, messages, num_messages, MSG_DONTWAIT, NULL );
}

static int rocketnet_client_gso_kernel_gro_enable( void *, int socket )
{
    int one = 1;
    return setsockopt( socket, IPPROTO_UDP, UDP_GRO, &one, sizeof( one ) );
}

// the context is the platform socket: its family decides the dual stack rule

static bool rocketnet_client_gso_kernel_read_address( void * context, const struct sockaddr_storage * storage, rocketnet_address_t * from )
{
    return rocketnet_platform_mmsg_read_sockaddr( (rocketnet_platform_socket_t *) context, storage, from );
}

const rocketnet_client_gso_syscalls_t rocketnet_client_gso_kernel_syscalls = {
    rocketnet_client_gso_kernel_sendmmsg,
    rocketnet_client_gso_kernel_segment_probe,
    rocketnet_client_gso_kernel_recvmmsg,
    rocketnet_client_gso_kernel_gro_enable,
    rocketnet_client_gso_kernel_read_address,
};

#if ROCKETNET_DEV
std::atomic<bool> rocketnet_client_gso_test_disable_segment{ false };
std::atomic<uint64_t> rocketnet_client_gso_test_segment_messages_sent{ 0 };
std::atomic<bool> rocketnet_client_gso_test_disable_gro{ false };
std::atomic<uint64_t> rocketnet_client_gso_test_gro_batches{ 0 };
#endif // #if ROCKETNET_DEV

bool rocketnet_client_gso_init( rocketnet_client_gso_t * gso, const rocketnet_client_gso_syscalls_t * syscalls, void * syscall_context, int socket, bool probe_segment )
{
    rocketnet_assert( gso );
    rocketnet_assert( syscalls );

    gso->syscalls = syscalls;
    gso->syscall_context = syscall_context;
    gso->socket = socket;
    gso->segment = probe_segment && syscalls->segment_probe( syscall_context, socket ) == 0;
    gso->packets_sent = 0;
    gso->send_syscalls = 0;
    gso->segment_messages_sent = 0;
    gso->segment_refusals = 0;
    gso->packet_send_failures.store( 0, std::memory_order_relaxed );

    gso->gro = false;
    gso->receive_syscalls = 0;
    gso->packets_received = 0;
    gso->gro_batches = 0;
    gso->gro_cmsg_faults = 0;
    gso->carry_segment = 0;
    gso->carry_segments = 0;
    memset( &gso->carry_source, 0, sizeof( gso->carry_source ) );
    return gso->segment;
}

bool rocketnet_client_gso_init_receive( rocketnet_client_gso_t * gso, bool probe_gro )
{
    rocketnet_assert( gso );
    gso->gro = probe_gro && gso->syscalls->gro_enable( gso->syscall_context, gso->socket ) == 0;
    return gso->gro;
}

// Builds the messages for packets [first, num_packets) of the chunk: one
// message per run when segment is on, one per packet otherwise. Returns the
// message count.

static int rocketnet_client_gso_build( rocketnet_client_gso_t * gso, const rocketnet_platform_send_t * sends, int first, int num_packets )
{
    int num_messages = 0;
    int packet = first;
    while ( packet < num_packets )
    {
        const int run = gso->segment ? rocketnet_coalesce_send_run( &gso->run_packet_bytes[packet], &gso->run_to[packet], num_packets - packet,
                                                                    ROCKETNET_CLIENT_GSO_MAX_SEGMENTS, ROCKETNET_CLIENT_GSO_MAX_SUPER_BYTES )
                                     : 1;
        rocketnet_assert( run >= 1 );

        // a connected socket names no destination (rocketnet_platform_sendto)

        struct msghdr * header = &gso->messages[num_messages].msg_hdr;

        memset( &gso->messages[num_messages], 0, sizeof( gso->messages[num_messages] ) );

        header->msg_iov = &gso->packet_iovecs[packet];
        header->msg_iovlen = (size_t) run;

        for ( int i = packet; i < packet + run; i++ )
        {
            gso->packet_iovecs[i].iov_base = const_cast<uint8_t *>( sends[i].packet_data ); // the send only reads it
            gso->packet_iovecs[i].iov_len = (size_t) sends[i].packet_bytes;
        }

        if ( run > 1 )
        {
            memset( &gso->controls[num_messages], 0, sizeof( gso->controls[num_messages] ) );

            header->msg_control = gso->controls[num_messages].bytes;
            header->msg_controllen = CMSG_SPACE( sizeof( uint16_t ) );

            struct cmsghdr * cmsg = CMSG_FIRSTHDR( header );
            cmsg->cmsg_level = IPPROTO_UDP;
            cmsg->cmsg_type = UDP_SEGMENT;
            cmsg->cmsg_len = CMSG_LEN( sizeof( uint16_t ) );

            const uint16_t stride = (uint16_t) gso->run_packet_bytes[packet];
            memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
        }

        gso->message_first_packet[num_messages] = packet;
        gso->message_num_packets[num_messages] = run;
        num_messages++;
        packet += run;
    }

    return num_messages;
}

// One chunk of at most ROCKETNET_CLIENT_GSO_SEND_BATCH datagrams.

static void rocketnet_client_gso_send_chunk( rocketnet_client_gso_t * gso, const rocketnet_platform_send_t * sends, int num_packets )
{
    for ( int i = 0; i < num_packets; i++ )
    {
        rocketnet_assert( sends[i].to );
        rocketnet_assert( sends[i].packet_data );
        rocketnet_assert( sends[i].packet_bytes > 0 && sends[i].packet_bytes <= UINT16_MAX );

        gso->run_packet_bytes[i] = sends[i].packet_bytes;
        gso->run_to[i] = *sends[i].to;
    }

    int num_messages = rocketnet_client_gso_build( gso, sends, 0, num_packets );
    int sent = 0;
    while ( sent < num_messages )
    {
        errno = 0;

        const int result = gso->syscalls->sendmmsg( gso->syscall_context, gso->socket, &gso->messages[sent], (unsigned int) ( num_messages - sent ) );

        gso->send_syscalls++;

        if ( result > 0 )
        {
            for ( int i = sent; i < sent + result; i++ )
            {
                gso->packets_sent += (uint64_t) gso->message_num_packets[i];
                if ( gso->message_num_packets[i] > 1 )
                {
                    gso->segment_messages_sent++;
#if ROCKETNET_DEV
                    rocketnet_client_gso_test_segment_messages_sent.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
                }
            }

            sent += result;
            continue;
        }

        const int error = errno;

        if ( gso->message_num_packets[sent] > 1 && ( error == EINVAL || error == EIO || error == EOPNOTSUPP ) )
        {
            // the kernel accepted the UDP_SEGMENT probe but refused a live
            // segmented send. flip the tier and rebuild from this message's
            // first packet as plain messages -- lossless, not a dropped batch.
            // the rebuild has no segmented message, so this runs once.

            rocketnet_warn( "gso client: live UDP_SEGMENT send refused (%s). degrading to plain sendmmsg", strerror( error ) );

            gso->segment = false;
            gso->segment_refusals++;
            num_messages = rocketnet_client_gso_build( gso, sends, gso->message_first_packet[sent], num_packets );
            sent = 0;
            continue;
        }

        if ( error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS )
        {
            // the socket cannot take more now: the unsent tail is lost on the wire, counted, never resent (the per-packet path's drop)

            for ( int i = sent; i < num_messages; i++ )
                gso->packet_send_failures.fetch_add( (uint64_t) gso->message_num_packets[i], std::memory_order_relaxed );

            rocketnet_debug( "gso client sendmmsg failed: %s. %d messages dropped", strerror( error ), num_messages - sent );
            return;
        }

        // no message went in this call, so the first unsent one is the one
        // the kernel refused (an ICMP report on the connected socket, say):
        // drop it and send the rest in order, as the portable batch does

        gso->packet_send_failures.fetch_add( (uint64_t) gso->message_num_packets[sent], std::memory_order_relaxed );

        rocketnet_debug( "gso client sendmmsg failed: %s. one message dropped", strerror( error ) );

        sent++;
    }
}

void rocketnet_client_gso_send_batch( rocketnet_client_gso_t * gso, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( gso );
    rocketnet_assert( sends || num_packets == 0 );

    int base = 0;
    while ( base < num_packets )
    {
        const int remaining = num_packets - base;
        const int chunk = remaining < ROCKETNET_CLIENT_GSO_SEND_BATCH ? remaining : ROCKETNET_CLIENT_GSO_SEND_BATCH;
        rocketnet_client_gso_send_chunk( gso, sends + base, chunk );

        base += chunk;
    }
}

// ---------------------------------------------------------------------------
// The receive path

int rocketnet_client_gso_gro_segment_bytes( const struct msghdr * header, int message_bytes, uint64_t * faults )
{
    rocketnet_assert( header );
    rocketnet_assert( faults );

    if ( header->msg_flags & MSG_CTRUNC )
    {
        *faults += 1;
        return message_bytes;
    }

    struct msghdr * walk = const_cast<struct msghdr *>( header ); // CMSG_NXTHDR only reads it
    for ( struct cmsghdr * cmsg = CMSG_FIRSTHDR( walk ); cmsg != NULL; cmsg = CMSG_NXTHDR( walk, cmsg ) )
    {
        if ( cmsg->cmsg_len < sizeof( struct cmsghdr ) )
        {
            *faults += 1;
            return message_bytes;
        }

        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_GRO )
        {
            if ( cmsg->cmsg_len < CMSG_LEN( sizeof( int ) ) )
            {
                *faults += 1;
                return message_bytes;
            }

            int segment_bytes;
            memcpy( &segment_bytes, CMSG_DATA( cmsg ), sizeof( segment_bytes ) );
            return segment_bytes;
        }
    }

    return message_bytes;
}

// the receive errors that are weather, as the platform's receive reads them:
// skip and keep draining, a bounded number of times per call

static bool rocketnet_client_gso_receive_weather( int error )
{
    return error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH ||
           error == EHOSTDOWN || error == ENETDOWN || error == ENOBUFS || error == EINTR;
}

#define ROCKETNET_CLIENT_GSO_RECEIVE_WEATHER_RETRIES 8

// proofs/tla/RNClientGroCarry.tla Deliver: copies the held message's segments
// into the slots from total, in order, while the call has room, and returns
// the new total. Segments it has no room for stay held.

static int rocketnet_client_gso_receive_deliver( rocketnet_client_gso_t * gso, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes,
                                                 int max_packets, int total )
{
    while ( gso->carry_segment < gso->carry_segments && total < max_packets )
    {
        const int s = gso->carry_segment++;
        const int bytes = gso->segment_lengths[s];
        const int copy_bytes = bytes < packet_stride ? bytes : packet_stride;
        memcpy( packet_data + (size_t) total * (size_t) packet_stride, gso->coalesce_buffer + gso->segment_offsets[s], (size_t) copy_bytes );

        from[total] = gso->carry_source;
        packet_bytes[total] = bytes;
        total++;
        gso->packets_received++;
    }

    return total;
}

// proofs/tla/RNClientGroCarry.tla Begin and End: one call, max_packets slots
// from 0, returning the slots it filled.

int rocketnet_client_gso_receive_batch( rocketnet_client_gso_t * gso, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_assert( gso );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_stride > 0 );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( max_packets > 0 );

    int weather = 0;
    int skipped = 0;

    // proofs/tla/RNClientGroCarry.tla Deliver: the segments the last call had
    // no room for come first, and the socket is not read while any is held:
    // the coalesce buffer holds one message

    int total = rocketnet_client_gso_receive_deliver( gso, from, packet_data, packet_stride, packet_bytes, max_packets, 0 );

    while ( total < max_packets )
    {
        // proofs/tla/RNClientGroCarry.tla Read: a message can hold up to the
        // kernel's segment count: past the first slot, one is read only while
        // all of it fits, and the rest wait in the socket for the next call
        if ( total > 0 && max_packets - total < ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS )
            break;

        struct msghdr * header = &gso->receive_message.msg_hdr;

        memset( &gso->receive_message, 0, sizeof( gso->receive_message ) );
        gso->receive_iovec.iov_base = gso->coalesce_buffer;
        gso->receive_iovec.iov_len = sizeof( gso->coalesce_buffer );
        header->msg_name = &gso->receive_name;
        header->msg_namelen = sizeof( gso->receive_name );
        header->msg_iov = &gso->receive_iovec;
        header->msg_iovlen = 1;
        header->msg_control = gso->receive_control.bytes;
        header->msg_controllen = sizeof( gso->receive_control.bytes );

        errno = 0;

        const int result = gso->syscalls->recvmmsg( gso->syscall_context, gso->socket, &gso->receive_message, 1 );

        gso->receive_syscalls++;

        if ( result < 0 )
        {
            const int error = errno;
            if ( rocketnet_client_gso_receive_weather( error ) && weather++ < ROCKETNET_CLIENT_GSO_RECEIVE_WEATHER_RETRIES )
                continue;
            if ( error != EAGAIN && error != EWOULDBLOCK )
                rocketnet_debug( "gso client recvmmsg failed with error %d", error );

            return total;
        }

        if ( result == 0 )
            return total;

        // a zero-length datagram or a source the socket does not speak is not
        // delivered, as the platform's batch receive has it; a stream of them
        // holds the drain no longer than a batch of packets would

        const int message_bytes = (int) gso->receive_message.msg_len;
        if ( message_bytes <= 0 )
        {
            if ( ++skipped >= max_packets )
                return total;
            continue;
        }

        rocketnet_address_t source;
        if ( !gso->syscalls->read_address( gso->syscall_context, &gso->receive_name, &source ) )
        {
            if ( ++skipped >= max_packets )
                return total;
            continue;
        }

        int segment_bytes = rocketnet_client_gso_gro_segment_bytes( header, message_bytes, &gso->gro_cmsg_faults );
        int num_segments = rocketnet_coalesce_split( message_bytes, segment_bytes, gso->segment_offsets, gso->segment_lengths,
                                                     ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS );
        if ( num_segments < 0 )
        {
            // a stride that cuts more segments than the kernel ever coalesces is a fault: the message is one datagram
            gso->gro_cmsg_faults++;
            num_segments = rocketnet_coalesce_split( message_bytes, 0, gso->segment_offsets, gso->segment_lengths, 1 );
        }

        if ( num_segments > 1 )
        {
            gso->gro_batches++;
#if ROCKETNET_DEV
            rocketnet_client_gso_test_gro_batches.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
        }

        // the message is held from here: what does not fit in this call's
        // slots is the next call's, before the socket is read again

        gso->carry_source = source;
        gso->carry_segment = 0;
        gso->carry_segments = num_segments;
        total = rocketnet_client_gso_receive_deliver( gso, from, packet_data, packet_stride, packet_bytes, max_packets, total );
    }

    return total;
}

// ---------------------------------------------------------------------------
// The client transport operations: the socket and the wake are the
// platform's, as the portable client has them; send_batch is the gso path.

struct rocketnet_client_gso_io_t
{
    void * context;
    rocketnet_platform_socket_t * socket;
    rocketnet_platform_wake_t * wake;
    rocketnet_client_gso_t gso;
};

// RNClientIo initial / Stop and destroy order (docs/CLIENT-IO.md): create
// owns the socket and the wake beside it, in one allocation with the send
// scratch; any failure releases what it opened.

static void * rocketnet_client_gso_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    void * memory = rocketnet_malloc( context, sizeof( rocketnet_client_gso_io_t ) );
    if ( !memory )
    {
        rocketnet_create_status( config->status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    rocketnet_client_gso_io_t * client = new ( memory ) rocketnet_client_gso_io_t();
    client->context = context;

    // dual stack as the portable client is: the platform socket binds the family of bind and converts addresses on an ipv6 socket

    client->socket = rocketnet_platform_socket_create_with_pump( context, bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                 ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, config->name, config->status );
    if ( !client->socket )
    {
        client->~rocketnet_client_gso_io_t();

        rocketnet_free( context, memory );
        return NULL;
    }

    client->wake = rocketnet_platform_wake_create( context, client->socket, config->status );
    if ( !client->wake )
    {
        rocketnet_platform_socket_destroy( client->socket );

        client->~rocketnet_client_gso_io_t();

        rocketnet_free( context, memory );
        return NULL;
    }

    bool probe_segment = true;
#if ROCKETNET_DEV
    if ( rocketnet_client_gso_test_disable_segment.load( std::memory_order_relaxed ) )
        probe_segment = false;
#endif // #if ROCKETNET_DEV
    const bool segment = rocketnet_client_gso_init( &client->gso, &rocketnet_client_gso_kernel_syscalls, client->socket, client->socket->handle, probe_segment );

    bool probe_gro = true;
#if ROCKETNET_DEV
    if ( rocketnet_client_gso_test_disable_gro.load( std::memory_order_relaxed ) )
        probe_gro = false;
#endif // #if ROCKETNET_DEV
    const bool gro = rocketnet_client_gso_init_receive( &client->gso, probe_gro );
    rocketnet_info( "gso client: UDP_SEGMENT %s, UDP_GRO %s", segment ? "yes" : "no (plain sendmmsg)", gro ? "yes" : "no (plain recvmmsg)" );

    return client;
}

// the family race's losing family, on the reading thread (proofs/tla/RNClientRace.tla
// ReaderStep): the wake's handle, then the socket's; the memory stays for
// destroy on the caller's thread

static void rocketnet_client_gso_close( void * transport )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    rocketnet_platform_wake_close( client->wake );
    rocketnet_platform_socket_close( client->socket );
}

// RNClientIo Stop and destroy order (docs/CLIENT-IO.md): the transport joins
// the I/O thread before closing the transport, so the wake goes, then the socket.

static void rocketnet_client_gso_destroy( void * transport )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    if ( !client )
        return;

    void * context = client->context;

    rocketnet_platform_wake_destroy( client->wake );

    rocketnet_platform_socket_destroy( client->socket );

    client->~rocketnet_client_gso_io_t();

    rocketnet_free( context, client );
}

static void rocketnet_client_gso_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    rocketnet_platform_socket_send_packet( client->socket, to, data, bytes );
}

static int rocketnet_client_gso_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    return rocketnet_platform_socket_receive_packet( client->socket, from, data, capacity );
}

// RNClientIo IoSend (docs/CLIENT-IO.md The I/O step): the gso path names no
// destination, so it needs the socket connected to the server; connect runs
// at create, before the first send. An unconnected socket takes the
// platform's sendmmsg batch, which names each destination.

static void rocketnet_client_gso_send_batch_operation( void * transport, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    if ( !client->socket->connected )
    {
        rocketnet_platform_socket_send_packets_mmsg( client->socket, sends, num_packets );
        return;
    }

    rocketnet_client_gso_send_batch( &client->gso, sends, num_packets );
}

static int rocketnet_client_gso_send_batch_progress( void * transport, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    if ( !client->socket->connected )
        return rocketnet_platform_socket_send_packets_mmsg_progress( client->socket, sends, num_packets );

    // The connected GSO path retains its existing synchronous contract.
    rocketnet_client_gso_send_batch( &client->gso, sends, num_packets );
    return num_packets;
}

// RNClientIo IoReceive (docs/CLIENT-IO.md The I/O step): recvmmsg through the
// coalesce buffer, split at the GRO stride, when UDP_GRO is on (connected or
// not: a GRO socket's message may be coalesced, which the platform's slot
// sized batch would truncate); the platform's recvmmsg otherwise. Either way
// one synchronous call that fills the slots it was given: no new state for
// the I/O model.

static int rocketnet_client_gso_receive_batch_operation( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    if ( client->gso.gro )
        return rocketnet_client_gso_receive_batch( &client->gso, from, packet_data, packet_stride, packet_bytes, max_packets );

    return rocketnet_platform_socket_receive_packets_mmsg( client->socket, from, packet_data, packet_stride, packet_bytes, max_packets );
}

// RNClientIo IoWait / waitReadable and the wake (docs/CLIENT-IO.md The wake)

static bool rocketnet_client_gso_wait( void * transport, bool readable, double timeout_seconds )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    return rocketnet_platform_wake_wait( client->wake, readable, timeout_seconds );
}

static void rocketnet_client_gso_wake( void * transport )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    rocketnet_platform_wake_set( client->wake );
}

// the gso batch's drops and the platform socket's (per-packet sends and the
// unconnected batch)

static uint64_t rocketnet_client_gso_send_failures( void * transport )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    return client->gso.packet_send_failures.load( std::memory_order_relaxed ) + rocketnet_platform_socket_send_failures( client->socket );
}

// the client trusts only the server: the socket is connected to it, so the
// kernel drops datagrams from any other source (TRANSPORT.md t11)

static bool rocketnet_client_gso_connect( void * transport, const rocketnet_address_t * server_address )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t *) transport;
    return rocketnet_platform_socket_connect( client->socket, server_address );
}

const rocketnet_client_transport_io_operations_t rocketnet_client_gso_operations = {
    rocketnet_client_gso_create, rocketnet_client_gso_destroy, rocketnet_client_gso_send, rocketnet_client_gso_receive,
    rocketnet_client_gso_send_batch_operation, rocketnet_client_gso_receive_batch_operation, rocketnet_client_gso_wait, rocketnet_client_gso_wake,
    rocketnet_client_gso_send_failures, rocketnet_client_gso_connect, rocketnet_client_gso_close, rocketnet_client_gso_send_batch_progress
};

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

int rocketnet_client_gso_linux_cpp_dummy = 0;
