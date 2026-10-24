/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gso client's send path (sdk/rocketnet_client_gso.h) on a fake socket:
// the syscall seam records every sendmmsg message (its datagrams, its
// UDP_SEGMENT stride) and refuses on command. No socket, no thread, no time.
// Every expected value is a literal, never computed by the code under test.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_client_gso.h"
#include "rocketnet_platform.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdint.h>
#include <string.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

#define FAKE_GSO_MAX_MESSAGES 64
#define FAKE_GSO_MAX_PACKETS 256

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

struct fake_gso_socket_t
{
    int probe_calls;
    int probe_result;
    int sendmmsg_calls;
    int max_messages_per_call;          // 0: take every message offered
    int refuse_segment_errno;           // nonzero: a call whose first message is segmented fails with it
    int num_messages;
    fake_gso_message_t messages[FAKE_GSO_MAX_MESSAGES];
    uint8_t packet_data[FAKE_GSO_MAX_PACKETS][512];
    int num_packets;
};

static int fake_gso_stride( const struct msghdr * header )
{
    if ( !header->msg_control )
        return 0;
    for ( const struct cmsghdr * cmsg = CMSG_FIRSTHDR( header ); cmsg; cmsg = CMSG_NXTHDR( const_cast<struct msghdr*>( header ), const_cast<struct cmsghdr*>( cmsg ) ) )
    {
        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_SEGMENT )
        {
            uint16_t stride;
            memcpy( &stride, CMSG_DATA( cmsg ), sizeof(stride) );
            return stride;
        }
    }
    return 0;
}

static int fake_gso_sendmmsg( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    fake_gso_socket_t * fake = (fake_gso_socket_t*) context;
    rocketnet_unit_check( socket == 7 );
    fake->sendmmsg_calls++;

    if ( fake->refuse_segment_errno != 0 && fake_gso_stride( &messages[0].msg_hdr ) != 0 )
    {
        errno = fake->refuse_segment_errno;
        return -1;
    }

    int take = (int) num_messages;
    if ( fake->max_messages_per_call > 0 && take > fake->max_messages_per_call )
        take = fake->max_messages_per_call;

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
            record->first_byte[i] = ( (const uint8_t*) iov->iov_base )[0];
            rocketnet_unit_check( fake->num_packets < FAKE_GSO_MAX_PACKETS );
            rocketnet_unit_check( iov->iov_len <= sizeof(fake->packet_data[0]) );
            memcpy( fake->packet_data[fake->num_packets++], iov->iov_base, iov->iov_len );
        }
    }
    return take;
}

static int fake_gso_segment_probe( void * context, int socket )
{
    fake_gso_socket_t * fake = (fake_gso_socket_t*) context;
    rocketnet_unit_check( socket == 7 );
    fake->probe_calls++;
    return fake->probe_result;
}

static const rocketnet_client_gso_syscalls_t fake_gso_syscalls = { fake_gso_sendmmsg, fake_gso_segment_probe };

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
    memset( &batch->server, 0, sizeof(batch->server) );
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
    memset( &test_socket, 0, sizeof(test_socket) );
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
        rocketnet_unit_check( !test_socket.messages[m].named_destination );

    // every datagram byte-exact, in order
    rocketnet_unit_check( test_socket.num_packets == 10 );
    for ( int i = 0; i < 10; i++ )
        for ( int j = 0; j < packet_bytes[i]; j++ )
            rocketnet_unit_check( test_socket.packet_data[i][j] == (uint8_t) ( 10 + i ) );

    rocketnet_unit_check( test_gso.packets_sent == 10 );
    rocketnet_unit_check( test_gso.segment_messages_sent == 2 );
    rocketnet_unit_check( test_gso.send_syscalls == 1 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // a batch past one sendmmsg call: 70 of 100 leave as a 64-datagram
    // call (one run of 64 segments) and a 6-datagram call
    memset( &test_socket, 0, sizeof(test_socket) );
    int many_bytes[70];
    for ( int i = 0; i < 70; i++ )
        many_bytes[i] = 100;
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
    memset( &test_socket, 0, sizeof(test_socket) );
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
    memset( &test_socket, 0, sizeof(test_socket) );
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
        for ( int j = 0; j < packet_bytes[i]; j++ )
            rocketnet_unit_check( test_socket.packet_data[i][j] == (uint8_t) ( 20 + i ) );

    rocketnet_unit_check( !test_gso.segment );
    rocketnet_unit_check( test_gso.segment_refusals == 1 );
    rocketnet_unit_check( test_gso.segment_messages_sent == 0 );
    rocketnet_unit_check( test_gso.packets_sent == 4 );
    rocketnet_unit_check( test_gso.packet_send_failures.load() == 0 );

    // the tier stays plain: the next equal-sized batch carries no UDP_SEGMENT
    memset( &test_socket, 0, sizeof(test_socket) );
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
        memset( &test_socket, 0, sizeof(test_socket) );
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
    memset( &test_socket, 0, sizeof(test_socket) );
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

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
