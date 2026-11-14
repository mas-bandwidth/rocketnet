/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gso client's send and receive paths (sdk/rocketnet_client_gso.h) on a
// fake socket: the syscall seam records every sendmmsg message (its datagrams,
// its UDP_SEGMENT stride) and refuses on command, and delivers queued receive
// messages with the UDP_GRO cmsg each names. No socket, no thread, no time.
// Every expected value is a literal, never computed by the code under test.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_client_gso.h"
#include "rocketnet_platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdint.h>
#include <string.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

#define FAKE_GSO_MAX_MESSAGES 64
#define FAKE_GSO_MAX_PACKETS 256
#define FAKE_GSO_MAX_RECEIVES 8

// one sendmmsg message the fake socket took: its datagrams in order, and its
// UDP_SEGMENT stride (0: a plain message)
struct fake_gso_message_t
{
    int num_packets;
    int stride;
    bool named_destination;
    int packet_bytes[ROCKETNET_CLIENT_GSO_MAX_SEGMENTS];
    uint8_t first_byte[ROCKETNET_CLIENT_GSO_MAX_SEGMENTS];
};

// one message the fake socket delivers on recvmmsg: bytes of payload, all of
// segment s equal to tag + s when cut at stride, and its control area
enum fake_gso_control_t
{
    FAKE_GSO_CONTROL_NONE,              // no cmsg: one datagram
    FAKE_GSO_CONTROL_GRO,               // a UDP_GRO cmsg holding stride
    FAKE_GSO_CONTROL_SHORT,             // a UDP_GRO cmsg too short to hold an int
    FAKE_GSO_CONTROL_TRUNCATED,         // MSG_CTRUNC: the kernel had more control than room
    FAKE_GSO_CONTROL_PAYLOAD_TRUNCATED, // a UDP_GRO cmsg holding stride, and MSG_TRUNC: the kernel had more payload than room
};

struct fake_gso_receive_t
{
    int bytes;
    int stride;
    int tag;
    fake_gso_control_t control;
};

struct fake_gso_socket_t
{
    int gro_calls;
    int gro_result;
    int recvmmsg_calls;
    int weather_errors;                 // recvmmsg calls that fail with weather_errno before any receive is delivered
    int weather_errno;                  // 0: ECONNREFUSED
    int num_receives;
    int next_receive;
    fake_gso_receive_t receives[FAKE_GSO_MAX_RECEIVES];
    int probe_calls;
    int probe_result;
    int sendmmsg_calls;
    int max_messages_per_call;          // 0: take every message offered
    int refuse_segment_errno;           // nonzero: a call whose first message is segmented fails with it
    int interrupts;                     // the next this many sendmmsg calls fail with EINTR, sending nothing
    int pending_errors;                 // then the next this many fail with pending_errno, sending nothing (an ICMP error the connected socket held)
    int pending_errno;
    int num_messages;
    fake_gso_message_t messages[FAKE_GSO_MAX_MESSAGES];
    uint8_t packet_data[FAKE_GSO_MAX_PACKETS][512];
    int num_packets;
    int refuse_addresses;               // the next reads of a source address refused
};

static int fake_gso_stride( const struct msghdr * header )
{
    if ( !header->msg_control )
    {
        return 0;
    }

    for ( const struct cmsghdr * cmsg = CMSG_FIRSTHDR( header ); cmsg; cmsg = CMSG_NXTHDR( const_cast<struct msghdr *>( header ), const_cast<struct cmsghdr *>( cmsg ) ) )
    {
        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_SEGMENT )
        {
            uint16_t stride;
            memcpy( &stride, CMSG_DATA( cmsg ), sizeof( stride ) );
            return stride;
        }
    }

    return 0;
}

static int fake_gso_sendmmsg( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    fake_gso_socket_t * fake = (fake_gso_socket_t *) context;
    rocketnet_unit_check( socket == 7 );
    fake->sendmmsg_calls++;

    if ( fake->interrupts > 0 )
    {
        fake->interrupts--;
        errno = EINTR;
        return -1;
    }

    if ( fake->pending_errors > 0 )
    {
        fake->pending_errors--;
        errno = fake->pending_errno;
        return -1;
    }

    if ( fake->refuse_segment_errno != 0 && fake_gso_stride( &messages[0].msg_hdr ) != 0 )
    {
        errno = fake->refuse_segment_errno;
        return -1;
    }

    int take = (int) num_messages;
    if ( fake->max_messages_per_call > 0 && take > fake->max_messages_per_call )
    {
        take = fake->max_messages_per_call;
    }

    for ( int m = 0; m < take; m++ )
    {
        const struct msghdr * header = &messages[m].msg_hdr;
        rocketnet_unit_check( fake->num_messages < FAKE_GSO_MAX_MESSAGES );

        fake_gso_message_t * record = &fake->messages[fake->num_messages++];
        record->num_packets = (int) header->msg_iovlen;
        record->stride = fake_gso_stride( header );
        record->named_destination = header->msg_name != NULL;
        for ( int i = 0; i < record->num_packets; i++ )
        {
            const struct iovec * iov = &header->msg_iov[i];
            record->packet_bytes[i] = (int) iov->iov_len;
            record->first_byte[i] = ( (const uint8_t *) iov->iov_base )[0];
            rocketnet_unit_check( fake->num_packets < FAKE_GSO_MAX_PACKETS );
            rocketnet_unit_check( iov->iov_len <= sizeof( fake->packet_data[0] ) );
            memcpy( fake->packet_data[fake->num_packets++], iov->iov_base, iov->iov_len );
        }
    }

    return take;
}

static int fake_gso_segment_probe( void * context, int socket )
{
    fake_gso_socket_t * fake = (fake_gso_socket_t *) context;
    rocketnet_unit_check( socket == 7 );
    fake->probe_calls++;
    return fake->probe_result;
}

static int fake_gso_recvmmsg( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    fake_gso_socket_t * fake = (fake_gso_socket_t *) context;
    rocketnet_unit_check( socket == 7 );
    rocketnet_unit_check( num_messages == 1 );
    fake->recvmmsg_calls++;

    if ( fake->weather_errors > 0 )
    {
        fake->weather_errors--;
        errno = fake->weather_errno != 0 ? fake->weather_errno : ECONNREFUSED;
        return -1;
    }

    if ( fake->next_receive >= fake->num_receives )
    {
        errno = EAGAIN;
        return -1;
    }

    const fake_gso_receive_t * receive = &fake->receives[fake->next_receive++];
    struct msghdr * header = &messages[0].msg_hdr;

    rocketnet_unit_check( header->msg_iovlen == 1 );
    rocketnet_unit_check( header->msg_iov[0].iov_len >= (size_t) receive->bytes );

    uint8_t * data = (uint8_t *) header->msg_iov[0].iov_base;
    for ( int i = 0; i < receive->bytes; i++ )
    {
        data[i] = (uint8_t) ( receive->tag + ( receive->stride > 0 ? i / receive->stride : 0 ) );
    }
    messages[0].msg_len = (unsigned int) receive->bytes;

    // the source: the one server, 127.0.0.1:40000

    rocketnet_unit_check( header->msg_name != NULL );
    rocketnet_unit_check( header->msg_namelen >= sizeof( struct sockaddr_in ) );

    struct sockaddr_in name;
    memset( &name, 0, sizeof( name ) );
    name.sin_family = AF_INET;
    name.sin_port = htons( 40000 );
    name.sin_addr.s_addr = htonl( 0x7F000001 );
    memcpy( header->msg_name, &name, sizeof( name ) );
    header->msg_namelen = sizeof( name );

    header->msg_flags = 0;

    if ( receive->control == FAKE_GSO_CONTROL_NONE )
    {
        header->msg_controllen = 0;
    }
    else if ( receive->control == FAKE_GSO_CONTROL_TRUNCATED )
    {
        header->msg_controllen = 0;
        header->msg_flags = MSG_CTRUNC;
    }
    else
    {
        rocketnet_unit_check( header->msg_control != NULL );
        rocketnet_unit_check( header->msg_controllen >= CMSG_SPACE( sizeof( int ) ) );
        memset( header->msg_control, 0, header->msg_controllen );

        struct cmsghdr * cmsg = CMSG_FIRSTHDR( header );
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_GRO;

        if ( receive->control == FAKE_GSO_CONTROL_GRO || receive->control == FAKE_GSO_CONTROL_PAYLOAD_TRUNCATED )
        {
            cmsg->cmsg_len = CMSG_LEN( sizeof( int ) );

            const int stride = receive->stride;
            memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
            header->msg_controllen = CMSG_SPACE( sizeof( int ) );

            if ( receive->control == FAKE_GSO_CONTROL_PAYLOAD_TRUNCATED )
            {
                header->msg_flags = MSG_TRUNC;
            }
        }
        else
        {
            cmsg->cmsg_len = CMSG_LEN( sizeof( uint16_t ) );

            const uint16_t stride = (uint16_t) receive->stride;
            memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
            header->msg_controllen = CMSG_SPACE( sizeof( uint16_t ) );
        }
    }

    return 1;
}

static int fake_gso_gro_enable( void * context, int socket )
{
    fake_gso_socket_t * fake = (fake_gso_socket_t *) context;
    rocketnet_unit_check( socket == 7 );
    fake->gro_calls++;
    return fake->gro_result;
}

static bool fake_gso_read_address( void * context, const struct sockaddr_storage * storage, rocketnet_address_t * from )
{
    fake_gso_socket_t * fake = (fake_gso_socket_t *) context;
    if ( fake->refuse_addresses > 0 )
    {
        fake->refuse_addresses--;
        return false;
    }

    rocketnet_unit_check( storage->ss_family == AF_INET );

    const struct sockaddr_in * name = (const struct sockaddr_in *) storage;
    memset( from, 0, sizeof( *from ) );
    from->type = ROCKETNET_ADDRESS_IPV4;

    const uint32_t host = ntohl( name->sin_addr.s_addr );
    from->data.ipv4[0] = (uint8_t) ( host >> 24 );
    from->data.ipv4[1] = (uint8_t) ( host >> 16 );
    from->data.ipv4[2] = (uint8_t) ( host >> 8 );
    from->data.ipv4[3] = (uint8_t) host;
    from->port = ntohs( name->sin_port );

    return true;
}

static const rocketnet_client_gso_syscalls_t fake_gso_syscalls = {
    fake_gso_sendmmsg, fake_gso_segment_probe, fake_gso_recvmmsg, fake_gso_gro_enable, fake_gso_read_address
};

// num_packets datagrams of packet_bytes[i] bytes each, every byte of packet i
// equal to tag + i, all to the one server
struct fake_gso_batch_t
{
    rocketnet_address_t server;
    uint8_t data[FAKE_GSO_MAX_PACKETS][1500];
    rocketnet_platform_send_t sends[FAKE_GSO_MAX_PACKETS];
};

static void fake_gso_batch( fake_gso_batch_t * batch, const int * packet_bytes, int num_packets, int tag )
{
    memset( &batch->server, 0, sizeof( batch->server ) );
    batch->server.type = ROCKETNET_ADDRESS_IPV4;
    batch->server.data.ipv4[0] = 127;
    batch->server.data.ipv4[3] = 1;
    batch->server.port = 40000;

    for ( int i = 0; i < num_packets; i++ )
    {
        memset( batch->data[i], tag + i, (size_t) packet_bytes[i] );
        batch->sends[i].to = &batch->server;
        batch->sends[i].packet_data = batch->data[i];
        batch->sends[i].packet_bytes = packet_bytes[i];
    }
}

static rocketnet_client_gso_t test_gso;
static fake_gso_socket_t test_socket;
static fake_gso_batch_t test_batch;

static void test_client_gso_send_runs_segment_by_stride()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( test_socket.probe_calls == 1 );

    // five of 100 and a 60-byte tail (the shorter tail closes the run), three
    // of 200, then a larger 400 that starts a run of its own and stands alone

    const int packet_bytes[] = { 100, 100, 100, 100, 100, 60, 200, 200, 200, 400 };
    fake_gso_batch( &test_batch, packet_bytes, 10, 10 );
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 10 );

    // one sendmmsg call, three messages

    rocketnet_unit_check( test_socket.sendmmsg_calls == 1 );
    rocketnet_unit_check( test_socket.num_messages == 3 );

    rocketnet_unit_check( test_socket.messages[0].num_packets == 6 );
    rocketnet_unit_check( test_socket.messages[0].stride == 100 );
    rocketnet_unit_check( test_socket.messages[0].packet_bytes[0] == 100 );
    rocketnet_unit_check( test_socket.messages[0].packet_bytes[4] == 100 );
    rocketnet_unit_check( test_socket.messages[0].packet_bytes[5] == 60 );
    rocketnet_unit_check( test_socket.messages[0].first_byte[0] == 10 );
    rocketnet_unit_check( test_socket.messages[0].first_byte[5] == 15 );

    rocketnet_unit_check( test_socket.messages[1].num_packets == 3 );
    rocketnet_unit_check( test_socket.messages[1].stride == 200 );
    rocketnet_unit_check( test_socket.messages[1].packet_bytes[2] == 200 );
    rocketnet_unit_check( test_socket.messages[1].first_byte[0] == 16 );
    rocketnet_unit_check( test_socket.messages[1].first_byte[2] == 18 );

    rocketnet_unit_check( test_socket.messages[2].num_packets == 1 );
    rocketnet_unit_check( test_socket.messages[2].stride == 0 );
    rocketnet_unit_check( test_socket.messages[2].packet_bytes[0] == 400 );
    rocketnet_unit_check( test_socket.messages[2].first_byte[0] == 19 );

    // the connected socket names no destination

    for ( int m = 0; m < 3; m++ )
    {
        rocketnet_unit_check( !test_socket.messages[m].named_destination );
    }

    // every datagram byte-exact, in order

    rocketnet_unit_check( test_socket.num_packets == 10 );
    for ( int i = 0; i < 10; i++ )
    {
        for ( int j = 0; j < packet_bytes[i]; j++ )
        {
            rocketnet_unit_check( test_socket.packet_data[i][j] == (uint8_t) ( 10 + i ) );
        }
    }

    rocketnet_unit_check( test_gso.packets_sent == 10 );
    rocketnet_unit_check( test_gso.segment_messages_sent == 2 );
    rocketnet_unit_check( test_gso.send_syscalls == 1 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // a batch past one sendmmsg call: 70 of 100 leave as a 64-datagram
    // call (one run of 64 segments) and a 6-datagram call

    memset( &test_socket, 0, sizeof( test_socket ) );

    int many_bytes[70];
    for ( int i = 0; i < 70; i++ )
    {
        many_bytes[i] = 100;
    }
    fake_gso_batch( &test_batch, many_bytes, 70, 0 );
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 70 );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
    rocketnet_unit_check( test_socket.num_messages == 2 );
    rocketnet_unit_check( test_socket.messages[0].num_packets == 64 );
    rocketnet_unit_check( test_socket.messages[0].stride == 100 );
    rocketnet_unit_check( test_socket.messages[1].num_packets == 6 );
    rocketnet_unit_check( test_socket.messages[1].stride == 100 );
    rocketnet_unit_check( test_socket.messages[1].first_byte[0] == 64 );

    // a refused probe is the plain tier: one datagram per message, one call

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.probe_result = -1;
    rocketnet_unit_check( !rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    fake_gso_batch( &test_batch, packet_bytes, 3, 0 );
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 3 );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 1 );
    rocketnet_unit_check( test_socket.num_messages == 3 );

    for ( int m = 0; m < 3; m++ )
    {
        rocketnet_unit_check( test_socket.messages[m].num_packets == 1 );
        rocketnet_unit_check( test_socket.messages[m].stride == 0 );
    }
}
REGISTER_UNIT_TEST( test_client_gso_send_runs_segment_by_stride );

static void test_client_gso_send_falls_back_on_live_refusal()
{
    // the kernel accepts the probe but refuses a live segmented send, one
    // message at a time: a plain message, then a run of three that is refused

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.max_messages_per_call = 1;
    test_socket.refuse_segment_errno = EINVAL;
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    const int packet_bytes[] = { 100, 300, 300, 300 };
    fake_gso_batch( &test_batch, packet_bytes, 4, 20 );
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 4 );

    // the plain 100 goes, the run is refused once, then the three leave as
    // plain messages from the run's first datagram: nothing lost, nothing twice

    rocketnet_unit_check( test_socket.sendmmsg_calls == 5 );
    rocketnet_unit_check( test_socket.num_messages == 4 );
    rocketnet_unit_check( test_socket.num_packets == 4 );

    for ( int m = 0; m < 4; m++ )
    {
        rocketnet_unit_check( test_socket.messages[m].num_packets == 1 );
        rocketnet_unit_check( test_socket.messages[m].stride == 0 );
        rocketnet_unit_check( test_socket.messages[m].packet_bytes[0] == packet_bytes[m] );
        rocketnet_unit_check( test_socket.messages[m].first_byte[0] == 20 + m );
    }

    for ( int i = 0; i < 4; i++ )
    {
        for ( int j = 0; j < packet_bytes[i]; j++ )
        {
            rocketnet_unit_check( test_socket.packet_data[i][j] == (uint8_t) ( 20 + i ) );
        }
    }

    rocketnet_unit_check( !test_gso.segment );
    rocketnet_unit_check( test_gso.segment_refusals == 1 );
    rocketnet_unit_check( test_gso.segment_messages_sent == 0 );
    rocketnet_unit_check( test_gso.packets_sent == 4 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // the tier stays plain: the next equal-sized batch carries no UDP_SEGMENT

    memset( &test_socket, 0, sizeof( test_socket ) );

    const int again_bytes[] = { 300, 300 };
    fake_gso_batch( &test_batch, again_bytes, 2, 30 );
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 2 );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 1 );
    rocketnet_unit_check( test_socket.num_messages == 2 );
    rocketnet_unit_check( test_socket.messages[0].stride == 0 );
    rocketnet_unit_check( test_socket.messages[1].stride == 0 );

    // EIO and EOPNOTSUPP degrade the same way

    const int refusals[] = { EIO, EOPNOTSUPP };

    for ( int r = 0; r < 2; r++ )
    {
        memset( &test_socket, 0, sizeof( test_socket ) );
        test_socket.refuse_segment_errno = refusals[r];
        rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
        fake_gso_batch( &test_batch, again_bytes, 2, 40 );
        rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 2 );
        rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
        rocketnet_unit_check( test_socket.num_messages == 2 );
        rocketnet_unit_check( test_socket.messages[0].first_byte[0] == 40 );
        rocketnet_unit_check( test_socket.messages[1].first_byte[0] == 41 );
        rocketnet_unit_check( !test_gso.segment );
        rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );
    }

    // any other refusal is send weather, not a tier: EAGAIN drops the unsent
    // run, counted, and the tier stays segmented

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.refuse_segment_errno = EAGAIN;
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    fake_gso_batch( &test_batch, again_bytes, 2, 50 );
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 2 );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 1 );
    rocketnet_unit_check( test_socket.num_messages == 0 );
    rocketnet_unit_check( test_gso.segment );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 2 );
}
REGISTER_UNIT_TEST( test_client_gso_send_falls_back_on_live_refusal );

// the I/O core's send (send_batch_progress): a full socket consumes only what
// went, and the unsent rest is neither counted a failure nor reported consumed,
// so the core keeps it for a retry, as the portable progress does

static void test_client_gso_send_progress_keeps_unsent_on_full_socket()
{
    // EAGAIN on the first call: nothing consumed, nothing counted

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.refuse_segment_errno = EAGAIN;
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    const int two_bytes[] = { 300, 300 };
    fake_gso_batch( &test_batch, two_bytes, 2, 50 );
    rocketnet_unit_check( rocketnet_client_gso_send_batch_progress( &test_gso, test_batch.sends, 2 ) == 0 );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 1 );
    rocketnet_unit_check( test_socket.num_messages == 0 );
    rocketnet_unit_check( test_gso.segment );
    rocketnet_unit_check( test_gso.packets_sent == 0 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // EAGAIN after a partial take: a plain 100 goes, the run of three 300s
    // behind it is refused, so one datagram is consumed and three stay

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.max_messages_per_call = 1;
    test_socket.refuse_segment_errno = EAGAIN;
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    const int four_bytes[] = { 100, 300, 300, 300 };
    fake_gso_batch( &test_batch, four_bytes, 4, 60 );
    rocketnet_unit_check( rocketnet_client_gso_send_batch_progress( &test_gso, test_batch.sends, 4 ) == 1 );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
    rocketnet_unit_check( test_socket.num_messages == 1 );
    rocketnet_unit_check( test_socket.messages[0].first_byte[0] == 60 );
    rocketnet_unit_check( test_gso.packets_sent == 1 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // the retry of the kept suffix, once the socket drains, sends it whole

    test_socket.refuse_segment_errno = 0;
    test_socket.max_messages_per_call = 0;
    rocketnet_unit_check( rocketnet_client_gso_send_batch_progress( &test_gso, test_batch.sends + 1, 3 ) == 3 );
    rocketnet_unit_check( test_socket.num_messages == 2 );
    rocketnet_unit_check( test_socket.messages[1].num_packets == 3 );
    rocketnet_unit_check( test_socket.messages[1].first_byte[0] == 61 );
    rocketnet_unit_check( test_gso.packets_sent == 4 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // past one chunk: the first chunk is 64 datagrams of rising size (each
    // larger one starts a run of its own, so 64 plain messages that go), the
    // second is a run of six 300s that is refused, so 64 of 70 are consumed

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.refuse_segment_errno = EAGAIN;
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    int many_bytes[70];
    for ( int i = 0; i < 70; i++ )
    {
        many_bytes[i] = i < ROCKETNET_CLIENT_GSO_SEND_BATCH ? 100 + i : 300;
    }
    fake_gso_batch( &test_batch, many_bytes, 70, 0 );
    rocketnet_unit_check( rocketnet_client_gso_send_batch_progress( &test_gso, test_batch.sends, 70 ) == ROCKETNET_CLIENT_GSO_SEND_BATCH );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
    rocketnet_unit_check( test_socket.num_messages == ROCKETNET_CLIENT_GSO_SEND_BATCH );
    rocketnet_unit_check( test_gso.packets_sent == ROCKETNET_CLIENT_GSO_SEND_BATCH );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );
}
REGISTER_UNIT_TEST( test_client_gso_send_progress_keeps_unsent_on_full_socket );

// a sendmmsg a signal interrupted sent nothing: it is called again, as the
// platform batch does, and no datagram is counted a failure

static void test_client_gso_send_retries_an_interrupted_call()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    const int packet_bytes[] = { 100, 100, 100, 300 };
    fake_gso_batch( &test_batch, packet_bytes, 4, 40 );
    test_socket.interrupts = 1;
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 4 );

    rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
    rocketnet_unit_check( test_socket.num_messages == 2 );
    rocketnet_unit_check( test_socket.messages[0].num_packets == 3 );
    rocketnet_unit_check( test_socket.messages[0].first_byte[0] == 40 );
    rocketnet_unit_check( test_socket.messages[1].num_packets == 1 );
    rocketnet_unit_check( test_socket.messages[1].first_byte[0] == 43 );
    rocketnet_unit_check( test_gso.packets_sent == 4 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );
}
REGISTER_UNIT_TEST( test_client_gso_send_retries_an_interrupted_call );

// an ICMP error the connected socket held for an earlier datagram comes back
// from the next sendmmsg, which sends nothing and clears it: the message that
// call carried, here a run of three that is a whole frame's coalesced send,
// goes again once and nothing is counted lost. Each errno a connected socket
// holds this way (rocketnet_platform_send_error_is_pending) does the same.

static void test_client_gso_send_retries_a_pending_icmp_error()
{
    const int pending[] = { ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN, ENONET, ENOPROTOOPT, EPROTO, EACCES, EMSGSIZE };
    const int packet_bytes[] = { 100, 100, 100, 300 };

    for ( int p = 0; p < 9; p++ )
    {
        memset( &test_socket, 0, sizeof( test_socket ) );
        rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

        fake_gso_batch( &test_batch, packet_bytes, 4, 40 );
        test_socket.pending_errors = 1;
        test_socket.pending_errno = pending[p];
        rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 4 );

        rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
        rocketnet_unit_check( test_socket.num_messages == 2 );
        rocketnet_unit_check( test_socket.messages[0].num_packets == 3 );
        rocketnet_unit_check( test_socket.messages[0].stride == 100 );
        rocketnet_unit_check( test_socket.messages[0].first_byte[0] == 40 );
        rocketnet_unit_check( test_socket.messages[1].num_packets == 1 );
        rocketnet_unit_check( test_socket.messages[1].first_byte[0] == 43 );
        rocketnet_unit_check( test_socket.num_packets == 4 );
        rocketnet_unit_check( test_gso.packets_sent == 4 );
        rocketnet_unit_check( test_gso.send_syscalls == 2 );
        rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );
        rocketnet_unit_check( test_gso.segment );
    }

    // the I/O core's progress send: the whole batch consumed, all of it sent

    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    fake_gso_batch( &test_batch, packet_bytes, 4, 50 );
    test_socket.pending_errors = 1;
    test_socket.pending_errno = ECONNREFUSED;
    rocketnet_unit_check( rocketnet_client_gso_send_batch_progress( &test_gso, test_batch.sends, 4 ) == 4 );
    rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
    rocketnet_unit_check( test_socket.num_packets == 4 );
    rocketnet_unit_check( test_gso.packets_sent == 4 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // two back to back: the retry meets a second pending error, so that one
    // message is dropped and counted (its three datagrams), once, and the
    // next message goes

    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    fake_gso_batch( &test_batch, packet_bytes, 4, 60 );
    test_socket.pending_errors = 2;
    test_socket.pending_errno = ECONNREFUSED;
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 4 );

    rocketnet_unit_check( test_socket.sendmmsg_calls == 3 );
    rocketnet_unit_check( test_socket.num_messages == 1 );
    rocketnet_unit_check( test_socket.messages[0].num_packets == 1 );
    rocketnet_unit_check( test_socket.messages[0].first_byte[0] == 63 );
    rocketnet_unit_check( test_gso.packets_sent == 1 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 3 );

    // a refusal of the message itself (a local firewall's EPERM) is not a
    // pending error: dropped and counted at once, no retry

    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    fake_gso_batch( &test_batch, packet_bytes, 4, 70 );
    test_socket.pending_errors = 1;
    test_socket.pending_errno = EPERM;
    rocketnet_client_gso_send_batch( &test_gso, test_batch.sends, 4 );

    rocketnet_unit_check( test_socket.sendmmsg_calls == 2 );
    rocketnet_unit_check( test_socket.num_messages == 1 );
    rocketnet_unit_check( test_socket.messages[0].first_byte[0] == 73 );
    rocketnet_unit_check( test_gso.packets_sent == 1 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 3 );
}
REGISTER_UNIT_TEST( test_client_gso_send_retries_a_pending_icmp_error );

// the receive slots the I/O thread hands receive_batch: ROCKETNET_FRAME_SIZE
// apart, as the client I/O step lays them out
#define FAKE_GSO_RECEIVE_SLOTS 64
#define FAKE_GSO_RECEIVE_STRIDE 2048

static uint8_t test_receive_data[FAKE_GSO_RECEIVE_SLOTS * FAKE_GSO_RECEIVE_STRIDE];
static rocketnet_address_t test_receive_from[FAKE_GSO_RECEIVE_SLOTS];
static int test_receive_bytes[FAKE_GSO_RECEIVE_SLOTS];

static void fake_gso_queue_receive( int bytes, int stride, int tag, fake_gso_control_t control )
{
    rocketnet_unit_check( test_socket.num_receives < FAKE_GSO_MAX_RECEIVES );

    fake_gso_receive_t * receive = &test_socket.receives[test_socket.num_receives++];
    receive->bytes = bytes;
    receive->stride = stride;
    receive->tag = tag;
    receive->control = control;
}

// slot i holds bytes datagram bytes, every one equal to value, from 127.0.0.1:40000

static void check_gso_receive_slot( int i, int bytes, int value )
{
    rocketnet_unit_check( test_receive_bytes[i] == bytes );
    rocketnet_unit_check( test_receive_from[i].type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( test_receive_from[i].data.ipv4[0] == 127 );
    rocketnet_unit_check( test_receive_from[i].data.ipv4[3] == 1 );
    rocketnet_unit_check( test_receive_from[i].port == 40000 );

    const uint8_t * slot = test_receive_data + (size_t) i * FAKE_GSO_RECEIVE_STRIDE;
    for ( int j = 0; j < bytes; j++ )
    {
        rocketnet_unit_check( slot[j] == (uint8_t) value );
    }
}

static void test_client_gso_receive_splits_gro_by_stride()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    memset( test_receive_bytes, 0, sizeof( test_receive_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( rocketnet_client_gso_init_receive( &test_gso, true ) );
    rocketnet_unit_check( test_socket.gro_calls == 1 );

    // a coalesced message of five 100-byte datagrams and a 40-byte tail, then
    // a plain 200-byte datagram with no cmsg

    fake_gso_queue_receive( 540, 100, 10, FAKE_GSO_CONTROL_GRO );
    fake_gso_queue_receive( 200, 0, 30, FAKE_GSO_CONTROL_NONE );

    // the coalesced message lands as its six datagrams, in order, from slot 0;
    // the next message waits: 58 slots left cannot hold a full GRO message

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 6 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == 1 );
    for ( int i = 0; i < 5; i++ )
    {
        check_gso_receive_slot( i, 100, 10 + i );
    }
    check_gso_receive_slot( 5, 40, 15 );

    // the plain datagram is the next call's, in slot 0 of the slots it is given

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 1 );
    check_gso_receive_slot( 0, 200, 30 );

    // an empty socket is zero

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 0 );

    rocketnet_unit_check( test_gso.gro_batches == 1 );
    rocketnet_unit_check( test_gso.packets_received == 7 );
    rocketnet_unit_check( test_gso.gro_cmsg_faults == 0 );

    // a receive buffer with room for four takes the first four of ten 300-byte
    // datagrams and keeps the other six (proofs/tla/RNClientGroCarry.tla
    // Conservation): the next calls deliver them first, in order, and the
    // socket is not read again until the last of them is out, so the plain
    // datagram behind the message comes after all ten

    memset( &test_socket, 0, sizeof( test_socket ) );
    fake_gso_queue_receive( 3000, 300, 50, FAKE_GSO_CONTROL_GRO );
    fake_gso_queue_receive( 200, 0, 60, FAKE_GSO_CONTROL_NONE );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 4 );
    for ( int i = 0; i < 4; i++ )
    {
        check_gso_receive_slot( i, 300, 50 + i );
    }
    rocketnet_unit_check( test_socket.recvmmsg_calls == 1 );
    rocketnet_unit_check( test_gso.packets_received == 11 );
    rocketnet_unit_check( test_gso.gro_batches == 2 );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 4 );
    for ( int i = 0; i < 4; i++ )
    {
        check_gso_receive_slot( i, 300, 54 + i );
    }
    rocketnet_unit_check( test_socket.recvmmsg_calls == 1 );

    // room for 64: the last two of the ten, and no read past them while a
    // message with up to 64 segments could not fit in the 62 left

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 2 );
    check_gso_receive_slot( 0, 300, 58 );
    check_gso_receive_slot( 1, 300, 59 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == 1 );
    rocketnet_unit_check( test_gso.packets_received == 17 );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 1 );
    check_gso_receive_slot( 0, 200, 60 );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 0 );
    rocketnet_unit_check( test_gso.packets_received == 18 );

    // room for one: a message of three is delivered one datagram per call

    memset( &test_socket, 0, sizeof( test_socket ) );
    fake_gso_queue_receive( 300, 100, 20, FAKE_GSO_CONTROL_GRO );
    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 1 ) == 1 );
        check_gso_receive_slot( 0, 100, 20 + i );
    }
    rocketnet_unit_check( test_socket.recvmmsg_calls == 1 );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 1 ) == 0 );
    rocketnet_unit_check( test_gso.packets_received == 21 );

    // a refused UDP_GRO probe is the plain tier

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.gro_result = -1;
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( !rocketnet_client_gso_init_receive( &test_gso, true ) );
    rocketnet_unit_check( test_socket.gro_calls == 1 );

    // probe_gro false asks nothing

    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( !rocketnet_client_gso_init_receive( &test_gso, false ) );
    rocketnet_unit_check( test_socket.gro_calls == 0 );
}
REGISTER_UNIT_TEST( test_client_gso_receive_splits_gro_by_stride );

// A local sender's segmented send can arrive as one message of more segments
// than GRO coalesces (UDP_MAX_SEGMENTS is 128 on recent kernels): it is cut
// at its stride like any other, every datagram delivered in order across
// calls, none lost and no fault counted.

static void test_client_gso_receive_delivers_past_64_segments()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    memset( test_receive_bytes, 0, sizeof( test_receive_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( rocketnet_client_gso_init_receive( &test_gso, true ) );

    // 129 datagrams of 100 bytes and a 40-byte tail, then a plain datagram

    fake_gso_queue_receive( 12940, 100, 0, FAKE_GSO_CONTROL_GRO );
    fake_gso_queue_receive( 200, 0, 7, FAKE_GSO_CONTROL_NONE );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 64 );
    for ( int i = 0; i < 64; i++ )
    {
        check_gso_receive_slot( i, 100, i );
    }

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 64 );
    for ( int i = 0; i < 64; i++ )
    {
        check_gso_receive_slot( i, 100, 64 + i );
    }
    rocketnet_unit_check( test_socket.recvmmsg_calls == 1 );

    // the last two, and no read past them while a GRO message of 64 could
    // not fit in the 62 left; the plain datagram is the next call's

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 2 );
    check_gso_receive_slot( 0, 100, 128 );
    check_gso_receive_slot( 1, 40, 129 );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 1 );
    check_gso_receive_slot( 0, 200, 7 );

    rocketnet_unit_check( test_gso.packets_received == 131 );
    rocketnet_unit_check( test_gso.gro_batches == 1 );
    rocketnet_unit_check( test_gso.gro_cmsg_faults == 0 );
}
REGISTER_UNIT_TEST( test_client_gso_receive_delivers_past_64_segments );

// a stream of zero-length datagrams holds the drain no longer than a batch
// of packets would: a receive call skips at most its batch of them, and the
// next call carries on

static void test_client_gso_receive_bounds_the_skipped_datagrams()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    for ( int i = 0; i < 6; i++ )
    {
        fake_gso_queue_receive( 0, 0, 0, FAKE_GSO_CONTROL_NONE );
    }
    fake_gso_queue_receive( 200, 0, 30, FAKE_GSO_CONTROL_NONE );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 0 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == 4 );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 1 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == 7 );
    check_gso_receive_slot( 0, 200, 30 );
}

REGISTER_UNIT_TEST( test_client_gso_receive_bounds_the_skipped_datagrams );

// a datagram whose source cannot be read is skipped like an empty one: the
// call reads on past it, up to its batch of skips, and the next call carries
// on to the datagram behind them

static void test_client_gso_receive_skips_unreadable_sources_within_the_batch()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    // one unreadable source, then a good datagram: both read in one call

    test_socket.refuse_addresses = 1;
    fake_gso_queue_receive( 200, 0, 40, FAKE_GSO_CONTROL_NONE );
    fake_gso_queue_receive( 200, 0, 41, FAKE_GSO_CONTROL_NONE );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 1 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == 2 );
    check_gso_receive_slot( 0, 200, 41 );

    // six unreadable sources: a call skips at most its batch of four

    memset( &test_socket, 0, sizeof( test_socket ) );
    test_socket.refuse_addresses = 6;
    for ( int i = 0; i < 6; i++ )
    {
        fake_gso_queue_receive( 200, 0, 50 + i, FAKE_GSO_CONTROL_NONE );
    }
    fake_gso_queue_receive( 200, 0, 60, FAKE_GSO_CONTROL_NONE );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 0 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == 4 );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 1 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == 7 );
    check_gso_receive_slot( 0, 200, 60 );
}

REGISTER_UNIT_TEST( test_client_gso_receive_skips_unreadable_sources_within_the_batch );

// one read budget per call, as the platform's receive has it
// (docs/TRANSPORT-INTERNALS.md; proofs/tla/RNClientGroCarry.tla ReadBudget):
// a weather errno, a skipped datagram and a kept one each spend one read of
// max_packets, so a call never reads the socket more than max_packets times,
// and the calls after it carry on to the data in order

static void test_client_gso_receive_one_read_budget()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    memset( test_receive_bytes, 0, sizeof( test_receive_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    const int max_packets = 4;

    // 8 weather errors, then max_packets zero-length datagrams, then
    // max_packets datagrams of data

    test_socket.weather_errors = 8;
    for ( int i = 0; i < max_packets; i++ )
    {
        fake_gso_queue_receive( 0, 0, 0, FAKE_GSO_CONTROL_NONE );
    }
    for ( int i = 0; i < max_packets; i++ )
    {
        fake_gso_queue_receive( 200, 0, 40 + i, FAKE_GSO_CONTROL_NONE );
    }

    int delivered = 0;
    int calls = 0;
    while ( delivered < max_packets && calls < 16 )
    {
        const int reads_before = test_socket.recvmmsg_calls;
        const int received = rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, max_packets );
        rocketnet_unit_check( test_socket.recvmmsg_calls - reads_before <= max_packets );
        rocketnet_unit_check( received >= 0 && received <= max_packets - delivered );
        for ( int i = 0; i < received; i++ )
        {
            check_gso_receive_slot( i, 200, 40 + delivered + i );
        }
        delivered += received;
        calls++;
    }

    rocketnet_unit_check( delivered == max_packets );
    rocketnet_unit_check( test_socket.weather_errors == 0 );
    rocketnet_unit_check( test_socket.next_receive == test_socket.num_receives );
    rocketnet_unit_check( test_gso.packets_received == (uint64_t) max_packets );
}

REGISTER_UNIT_TEST( test_client_gso_receive_one_read_budget );

// EMSGSIZE on a receive is weather: a connected client socket gets it from an
// ICMP fragmentation-needed or packet-too-big naming its 4-tuple (sk_err,
// returned before any datagram is dequeued), never from a long datagram, which
// MSG_TRUNC reports by size. The drain skips it and reads the datagram behind
// it in the same call.

static void test_client_gso_receive_emsgsize_is_weather()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    memset( test_receive_bytes, 0, sizeof( test_receive_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );

    test_socket.weather_errors = 1;
    test_socket.weather_errno = EMSGSIZE;
    fake_gso_queue_receive( 200, 0, 50, FAKE_GSO_CONTROL_NONE );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 1 );
    rocketnet_unit_check( test_socket.weather_errors == 0 );
    check_gso_receive_slot( 0, 200, 50 );
}

REGISTER_UNIT_TEST( test_client_gso_receive_emsgsize_is_weather );

static void test_client_gso_receive_gro_fault_is_counted()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    memset( test_receive_bytes, 0, sizeof( test_receive_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( rocketnet_client_gso_init_receive( &test_gso, true ) );

    // a UDP_GRO cmsg too short to hold its int is one fault and the message
    // is one datagram of all 600 bytes: never cut at a stride it cannot read

    fake_gso_queue_receive( 600, 0, 70, FAKE_GSO_CONTROL_SHORT );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 1 );
    check_gso_receive_slot( 0, 600, 70 );
    rocketnet_unit_check( test_gso.gro_cmsg_faults == 1 );
    rocketnet_unit_check( test_gso.gro_batches == 0 );

    // a truncated control area is the same fault

    fake_gso_queue_receive( 400, 0, 80, FAKE_GSO_CONTROL_TRUNCATED );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 1 );
    check_gso_receive_slot( 0, 400, 80 );
    rocketnet_unit_check( test_gso.gro_cmsg_faults == 2 );

    // the fault is the message's alone: the next coalesced message splits

    fake_gso_queue_receive( 400, 200, 90, FAKE_GSO_CONTROL_GRO );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 2 );
    check_gso_receive_slot( 0, 200, 90 );
    check_gso_receive_slot( 1, 200, 91 );
    rocketnet_unit_check( test_gso.gro_cmsg_faults == 2 );
    rocketnet_unit_check( test_gso.gro_batches == 1 );
    rocketnet_unit_check( test_gso.packets_received == 4 );
}
REGISTER_UNIT_TEST( test_client_gso_receive_gro_fault_is_counted );

// a completion that fills the coalesce buffer with MSG_TRUNC set would split into a plausible but short
// final segment: it is dropped whole, spends the skip budget, and leaves no carry

static void test_unit_client_gso_truncated_completion_dropped()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    memset( test_receive_bytes, 0, sizeof( test_receive_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( rocketnet_client_gso_init_receive( &test_gso, true ) );

    fake_gso_queue_receive( (int) sizeof( test_gso.coalesce_buffer ), 1200, 10, FAKE_GSO_CONTROL_PAYLOAD_TRUNCATED );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 0 );
    rocketnet_unit_check( test_gso.carry_segments == 0 );
    rocketnet_unit_check( test_gso.packets_received == 0 );
    rocketnet_unit_check( test_gso.gro_batches == 0 );

    // the skip budget bounds a stream of them, and the next call carries on

    for ( int i = 0; i < 6; i++ )
    {
        fake_gso_queue_receive( (int) sizeof( test_gso.coalesce_buffer ), 1200, 20, FAKE_GSO_CONTROL_PAYLOAD_TRUNCATED );
    }
    fake_gso_queue_receive( 200, 0, 30, FAKE_GSO_CONTROL_NONE );

    const int calls = test_socket.recvmmsg_calls;
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 0 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == calls + 4 );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 1 );
    check_gso_receive_slot( 0, 200, 30 );
    rocketnet_unit_check( test_gso.packets_received == 1 );
}
REGISTER_UNIT_TEST( test_unit_client_gso_truncated_completion_dropped );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
