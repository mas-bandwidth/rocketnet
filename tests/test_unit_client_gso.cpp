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
    int num_receives;
    int next_receive;
    fake_gso_receive_t receives[FAKE_GSO_MAX_RECEIVES];
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

static bool fake_gso_read_address( void *, const struct sockaddr_storage * storage, rocketnet_address_t * from )
{
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
        for ( int j = 0; j < packet_bytes[i]; j++ )
            rocketnet_unit_check( test_socket.packet_data[i][j] == (uint8_t) ( 10 + i ) );

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
        for ( int j = 0; j < packet_bytes[i]; j++ )
            rocketnet_unit_check( test_socket.packet_data[i][j] == (uint8_t) ( 20 + i ) );

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

// a completion the kernel cut short would split into a plausible but short
// final segment: it is dropped whole, spends the skip budget, and leaves no carry

static void test_client_gso_truncated_completion_dropped()
{
    memset( &test_socket, 0, sizeof( test_socket ) );
    memset( test_receive_bytes, 0, sizeof( test_receive_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &test_gso, &fake_gso_syscalls, &test_socket, 7, true ) );
    rocketnet_unit_check( rocketnet_client_gso_init_receive( &test_gso, true ) );

    fake_gso_queue_receive( 540, 100, 10, FAKE_GSO_CONTROL_PAYLOAD_TRUNCATED );
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 64 ) == 0 );
    rocketnet_unit_check( test_gso.carry_segments == 0 );
    rocketnet_unit_check( test_gso.packets_received == 0 );
    rocketnet_unit_check( test_gso.gro_batches == 0 );

    // the skip budget bounds a stream of them, and the next call carries on

    for ( int i = 0; i < 6; i++ )
    {
        fake_gso_queue_receive( 540, 100, 20, FAKE_GSO_CONTROL_PAYLOAD_TRUNCATED );
    }
    fake_gso_queue_receive( 200, 0, 30, FAKE_GSO_CONTROL_NONE );

    const int calls = test_socket.recvmmsg_calls;
    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 0 );
    rocketnet_unit_check( test_socket.recvmmsg_calls == calls + 4 );

    rocketnet_unit_check( rocketnet_client_gso_receive_batch( &test_gso, test_receive_from, test_receive_data, FAKE_GSO_RECEIVE_STRIDE, test_receive_bytes, 4 ) == 1 );
    check_gso_receive_slot( 0, 200, 30 );
    rocketnet_unit_check( test_gso.packets_received == 1 );
}
REGISTER_UNIT_TEST( test_client_gso_truncated_completion_dropped );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
