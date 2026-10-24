/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gso transport's client side: sendmmsg with UDP_SEGMENT on send, recvmmsg
// on receive. See sdk/rocketnet_client_gso.h for the shape, docs/CLIENT-IO.md
// for the I/O thread it runs under (tla/RNClientIo.tla, tla/RNBufferHandoff.tla).

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
#endif

static int rocketnet_client_gso_kernel_sendmmsg( void *, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    return sendmmsg( socket, messages, num_messages, 0 );
}

// segment size zero is the disabled value the socket already holds: the
// probe asks whether the option exists and changes nothing
static int rocketnet_client_gso_kernel_segment_probe( void *, int socket )
{
    int zero = 0;
    return setsockopt( socket, IPPROTO_UDP, UDP_SEGMENT, &zero, sizeof(zero) );
}

const rocketnet_client_gso_syscalls_t rocketnet_client_gso_kernel_syscalls =
{
    rocketnet_client_gso_kernel_sendmmsg,
    rocketnet_client_gso_kernel_segment_probe,
};

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
std::atomic<bool> rocketnet_client_gso_test_disable_segment{false};
std::atomic<uint64_t> rocketnet_client_gso_test_segment_messages_sent{0};
#endif

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
    return gso->segment;
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
        const int run = gso->segment ?
            rocketnet_coalesce_send_run( &gso->run_packet_bytes[packet], &gso->run_to[packet], num_packets - packet,
                                         ROCKETNET_CLIENT_GSO_MAX_SEGMENTS, ROCKETNET_CLIENT_GSO_MAX_SUPER_BYTES ) : 1;
        rocketnet_assert( run >= 1 );

        // a connected socket names no destination (rocketnet_platform_sendto)
        struct msghdr * header = &gso->messages[num_messages].msg_hdr;
        memset( &gso->messages[num_messages], 0, sizeof(gso->messages[num_messages]) );
        header->msg_iov = &gso->packet_iovecs[packet];
        header->msg_iovlen = (size_t) run;
        for ( int i = packet; i < packet + run; i++ )
        {
            gso->packet_iovecs[i].iov_base = const_cast<uint8_t*>( sends[i].packet_data ); // the send only reads it
            gso->packet_iovecs[i].iov_len = (size_t) sends[i].packet_bytes;
        }

        if ( run > 1 )
        {
            memset( &gso->controls[num_messages], 0, sizeof(gso->controls[num_messages]) );
            header->msg_control = gso->controls[num_messages].bytes;
            header->msg_controllen = CMSG_SPACE( sizeof(uint16_t) );
            struct cmsghdr * cmsg = CMSG_FIRSTHDR( header );
            cmsg->cmsg_level = IPPROTO_UDP;
            cmsg->cmsg_type = UDP_SEGMENT;
            cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
            const uint16_t stride = (uint16_t) gso->run_packet_bytes[packet];
            memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );
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
#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
                    rocketnet_client_gso_test_segment_messages_sent.fetch_add( 1, std::memory_order_relaxed );
#endif
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
            // the socket cannot take more now: the unsent tail is lost on the
            // wire, counted, never resent (the per-packet path's drop)

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
        return NULL;
    rocketnet_client_gso_io_t * client = new ( memory ) rocketnet_client_gso_io_t();
    client->context = context;

    // dual stack as the portable client is: the platform socket binds the
    // family of bind and converts addresses on an ipv6 socket
    client->socket = rocketnet_platform_socket_create_with_pump( context, bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
        ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, config->name );
    if ( !client->socket )
    {
        client->~rocketnet_client_gso_io_t();
        rocketnet_free( context, memory );
        return NULL;
    }

    client->wake = rocketnet_platform_wake_create( context, client->socket );
    if ( !client->wake )
    {
        rocketnet_platform_socket_destroy( client->socket );
        client->~rocketnet_client_gso_io_t();
        rocketnet_free( context, memory );
        return NULL;
    }

    bool probe_segment = true;
#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
    if ( rocketnet_client_gso_test_disable_segment.load( std::memory_order_relaxed ) )
        probe_segment = false;
#endif
    const bool segment = rocketnet_client_gso_init( &client->gso, &rocketnet_client_gso_kernel_syscalls, NULL, client->socket->handle, probe_segment );
    rocketnet_info( "gso client: UDP_SEGMENT %s", segment ? "yes" : "no (plain sendmmsg)" );
    return client;
}

// RNClientIo Stop and destroy order (docs/CLIENT-IO.md): the transport joins
// the I/O thread before closing the transport, so the wake goes, then the socket.
static void rocketnet_client_gso_destroy( void * transport )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
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
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    rocketnet_platform_socket_send_packet( client->socket, to, data, bytes );
}

static int rocketnet_client_gso_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    return rocketnet_platform_socket_receive_packet( client->socket, from, data, capacity );
}

// RNClientIo IoSend (docs/CLIENT-IO.md The I/O step): the gso path names no
// destination, so it needs the socket connected to the server; connect runs
// at create, before the first send. An unconnected socket takes the
// platform's sendmmsg batch, which names each destination.
static void rocketnet_client_gso_send_batch_operation( void * transport, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    if ( !client->socket->connected )
    {
        rocketnet_platform_socket_send_packets_mmsg( client->socket, sends, num_packets );
        return;
    }
    rocketnet_client_gso_send_batch( &client->gso, sends, num_packets );
}

// RNClientIo IoReceive (docs/CLIENT-IO.md The I/O step): recvmmsg
static int rocketnet_client_gso_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    return rocketnet_platform_socket_receive_packets_mmsg( client->socket, from, packet_data, packet_stride, packet_bytes, max_packets );
}

// RNClientIo IoWait / waitReadable and the wake (docs/CLIENT-IO.md The wake)
static bool rocketnet_client_gso_wait( void * transport, bool readable, double timeout_seconds )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    return rocketnet_platform_wake_wait( client->wake, readable, timeout_seconds );
}

static void rocketnet_client_gso_wake( void * transport )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    rocketnet_platform_wake_set( client->wake );
}

// the gso batch's drops and the platform socket's (per-packet sends and the
// unconnected batch)
static uint64_t rocketnet_client_gso_send_failures( void * transport )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    return client->gso.packet_send_failures.load( std::memory_order_relaxed ) + rocketnet_platform_socket_send_failures( client->socket );
}

// the client trusts only the server: the socket is connected to it, so the
// kernel drops datagrams from any other source (TRANSPORT.md t11)
static bool rocketnet_client_gso_connect( void * transport, const rocketnet_address_t * server_address )
{
    rocketnet_client_gso_io_t * client = (rocketnet_client_gso_io_t*) transport;
    return rocketnet_platform_socket_connect( client->socket, server_address );
}

const rocketnet_client_transport_io_operations_t rocketnet_client_gso_operations =
{
    rocketnet_client_gso_create, rocketnet_client_gso_destroy, rocketnet_client_gso_send, rocketnet_client_gso_receive,
    rocketnet_client_gso_send_batch_operation, rocketnet_client_gso_receive_batch, rocketnet_client_gso_wait, rocketnet_client_gso_wake,
    rocketnet_client_gso_send_failures, rocketnet_client_gso_connect
};

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

int rocketnet_client_gso_linux_cpp_dummy = 0;
