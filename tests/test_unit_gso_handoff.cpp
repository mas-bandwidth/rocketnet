/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_gso.h"

#include <stdint.h>
#include <stdio.h>

#define check rocketnet_unit_check

#if defined( __linux__ ) && ROCKETNET_DEV

#include <errno.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

// The GSO worker's handoffs (proofs/tla/RNGsoHandoff.tla), stepped on this thread
// with a faked transport: no socket, no worker thread, no clock.
static void test_server_gso_worker_handoff()
{
    check( rocketnet_server_gso_test_worker_handoff() );
}

REGISTER_UNIT_TEST( test_server_gso_worker_handoff );

// A refused sendmmsg through the send-failure seam: an errno other than a
// full socket's drops one message and sends the rest.
static void test_server_gso_send_refusals()
{
    check( rocketnet_server_gso_test_send_refusals() );
}

REGISTER_UNIT_TEST( test_server_gso_send_refusals );

// A full socket mid-flush on a fake socket (proofs/tla/RNGsoHandoff.tla
// SentOrHeld): the bank keeps its unsent tail, nothing is dropped, and the
// next attempt sends the rest in order.
static void test_server_gso_send_full_keeps_rest()
{
    check( rocketnet_server_gso_test_send_full_keeps_rest() );
}

REGISTER_UNIT_TEST( test_server_gso_send_full_keeps_rest );

// The receive drain's read budget: full batches of zero-length, refused-source
// and undersized datagrams, far more than the budget, through the receive
// seam; each drain returns after ROCKETNET_GSO_RECV_QUEUE_SIZE messages.
static void test_server_gso_drain_budget()
{
    check( rocketnet_server_gso_test_drain_budget() );
}

REGISTER_UNIT_TEST( test_server_gso_drain_budget );

// a drain asks recvmmsg only for messages whose every segment fits the
// buffer, so nothing it reads is dropped as overflow
static void test_server_gso_drain_room()
{
    check( rocketnet_server_gso_test_drain_room() );
}

REGISTER_UNIT_TEST( test_server_gso_drain_room );

// one segmented sendmmsg to one destination returns EINVAL. that run leaves
// as plain datagrams. segmentation stays on for a later destination, and
// turns off only after the consecutive-refusal threshold with no accepted
// segmented send between the refusals.

struct gso_segment_socket_t
{
    int refuse_segment;
    int segmented;
    int plain_packets;
    uint16_t plain_ports[8];
    int plain_ports_n;
    uint16_t segment_port;
};

static int gso_segment_stride( const struct msghdr * header )
{
    if ( header->msg_controllen < sizeof( struct cmsghdr ) )
        return 0;

    struct msghdr mutable_header = *header;
    for ( struct cmsghdr * cmsg = CMSG_FIRSTHDR( &mutable_header ); cmsg != NULL; cmsg = CMSG_NXTHDR( &mutable_header, cmsg ) )
    {
        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_SEGMENT && cmsg->cmsg_len >= CMSG_LEN( sizeof( uint16_t ) ) )
        {
            uint16_t stride = 0;
            memcpy( &stride, CMSG_DATA( cmsg ), sizeof( stride ) );
            return (int) stride;
        }
    }

    return 0;
}

static uint16_t gso_segment_port( const struct msghdr * header )
{
    if ( !header->msg_name || header->msg_namelen < (int) sizeof( struct sockaddr_in ) )
        return 0;

    const struct sockaddr_in * name = (const struct sockaddr_in *) header->msg_name;
    return ntohs( name->sin_port );
}

static int gso_segment_send( struct mmsghdr * messages, unsigned int count, void * user )
{
    struct gso_segment_socket_t * socket = (struct gso_segment_socket_t *) user;

    if ( count > 0 && gso_segment_stride( &messages[0].msg_hdr ) > 0 && socket->refuse_segment > 0 )
    {
        socket->refuse_segment--;
        errno = EINVAL;
        return -1;
    }

    for ( unsigned int i = 0; i < count; i++ )
    {
        const int stride = gso_segment_stride( &messages[i].msg_hdr );
        const uint16_t port = gso_segment_port( &messages[i].msg_hdr );
        if ( stride > 0 )
        {
            socket->segmented++;
            socket->segment_port = port;
            continue;
        }

        check( messages[i].msg_hdr.msg_iovlen == 1 );
        if ( socket->plain_ports_n < 8 )
            socket->plain_ports[socket->plain_ports_n++] = port;
        socket->plain_packets++;
    }

    return (int) count;
}

static void gso_segment_stage( struct rocketnet_server_gso_t * gso, uint16_t port, int packets )
{
    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = port;

    for ( int i = 0; i < packets; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
        check( packet );
        rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }
}

static void test_server_gso_segment_refusal_keeps_other_destinations()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open();
    check( gso );

    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_segment_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.refuse_segment = 1;
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    gso_segment_stage( gso, 40000, 2 );
    gso_segment_stage( gso, 40001, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );

    // the refused run left as two plain datagrams. the other destination
    // was still one segmented send, and the tier is still on.
    check( socket.plain_packets == 2 );
    check( socket.plain_ports_n == 2 );
    check( socket.plain_ports[0] == 40000 );
    check( socket.plain_ports[1] == 40000 );
    check( socket.segmented == 1 );
    check( socket.segment_port == 40001 );
    check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == 4 );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 0 );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS] == 1 );

    // a later send to another destination is still segmented
    memset( &socket, 0, sizeof( socket ) );
    gso_segment_stage( gso, 40002, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 1 );
    check( socket.segment_port == 40002 );
    check( socket.plain_packets == 0 );
    check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );

    // refusals with no accepted segmented send between them. the threshold
    // itself is the one that clears the tier, and that run is still resent.
    for ( int i = 0; i < ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD - 1; i++ )
    {
        memset( &socket, 0, sizeof( socket ) );
        socket.refuse_segment = 100;
        gso_segment_stage( gso, (uint16_t) ( 41000 + i ), 2 );
        check( rocketnet_server_gso_test_flush_staged( gso ) );
        check( socket.plain_packets == 2 );
        check( socket.segmented == 0 );
        check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );
    }

    memset( &socket, 0, sizeof( socket ) );
    socket.refuse_segment = 100;
    gso_segment_stage( gso, 42000, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.plain_packets == 2 );
    check( socket.segmented == 0 );
    check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) == 0 );

    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 0 );

    // the tier stays plain
    memset( &socket, 0, sizeof( socket ) );
    gso_segment_stage( gso, 43000, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 0 );
    check( socket.plain_packets == 2 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_segment_refusal_keeps_other_destinations );

#endif
