/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring completion codec (sdk/rocketnet_uring_codec.h): pure byte
// codec, no sockets, no ring, no wall clock; the header body exists only on
// Linux with IORING_RECV_MULTISHOT uapi. Every native-endian kernel header
// field is written by this file, never by the code under test. The cases are
// built only where the header body is.

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if !defined( _WIN32 )
#include <arpa/inet.h>
#include <sys/socket.h>
#endif // #if !defined( _WIN32 )

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_uring_codec.h"
#include "../sdk/rocketnet_server_uring.h"
#include "../sdk/rocketnet_server_gso.h"

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT )
#define ROCKETNET_URING_CODEC_ACTIVE 1
#else
#define ROCKETNET_URING_CODEC_ACTIVE 0
#endif // #if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT )

#if ROCKETNET_URING_CODEC_ACTIVE
static void put_u32_native( uint8_t * b, uint32_t v )
{
    memcpy( b, &v, sizeof( v ) );
}

static void test_io_uring_transport_codec_tag_round_trips()
{
    uint64_t t = rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 );
    rocketnet_unit_check( rocketnet_uring_tag_kind( t ) == ROCKETNET_URING_TAG_SEND );
    rocketnet_unit_check( rocketnet_uring_tag_queue( t ) == 0 );
    rocketnet_unit_check( rocketnet_uring_tag_index( t ) == 0 );

    t = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 65535, 0xffffffff );
    rocketnet_unit_check( rocketnet_uring_tag_kind( t ) == ROCKETNET_URING_TAG_RECV );
    rocketnet_unit_check( rocketnet_uring_tag_queue( t ) == 65535 );
    rocketnet_unit_check( (uint32_t) rocketnet_uring_tag_index( t ) == 0xffffffffu );

    t = rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 65535, 0 );
    rocketnet_unit_check( rocketnet_uring_tag_kind( t ) == ROCKETNET_URING_TAG_CANCEL );
    rocketnet_unit_check( rocketnet_uring_tag_queue( t ) == 65535 );
    rocketnet_unit_check( rocketnet_uring_tag_index( t ) == 0 );

    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 ) != rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ) );
    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ) != rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, 0 ) );
    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 ) != rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 65535, 0 ) );
    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 ) != rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0xffffffff ) );

    rocketnet_unit_check( rocketnet_uring_buffer_id( ( 7u << IORING_CQE_BUFFER_SHIFT ) | IORING_CQE_F_BUFFER ) == 7 );
    rocketnet_unit_check( rocketnet_uring_buffer_id( 0x12345678u ) == -1 );
}

static void test_io_uring_transport_codec_recvmsg_parses_name_and_payload()
{
    // an IPv4 sockaddr_in name, msg_namelen = sizeof(sockaddr_storage),
    // controllen 0, and a 64-byte payload.

    uint8_t buf[208];
    memset( buf, 0xA5, sizeof( buf ) );
    put_u32_native( buf + 0, 16 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 64 );
    put_u32_native( buf + 12, 0 );

    struct sockaddr_in in4;
    memset( &in4, 0, sizeof( in4 ) );
    in4.sin_family = AF_INET;
    in4.sin_port = htons( 40000 );
    in4.sin_addr.s_addr = htonl( 0x7f000001u );
    memcpy( buf + 16, &in4, sizeof( in4 ) );

    for ( int i = 0; i < 64; ++i )
    {
        buf[16 + (int) sizeof( struct sockaddr_storage ) + i] = (uint8_t) i;
    }

    const uint8_t * name;
    const uint8_t * payload;
    int name_bytes;
    int payload_bytes;
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), (int) sizeof( struct sockaddr_storage ), 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_OK );
    rocketnet_unit_check( name_bytes == 16 );
    rocketnet_unit_check( name == buf + 16 );
    rocketnet_unit_check( memcmp( name, &in4, 16 ) == 0 );
    rocketnet_unit_check( payload_bytes == 64 );
    rocketnet_unit_check( payload == buf + 16 + (int) sizeof( struct sockaddr_storage ) );

    for ( int i = 0; i < 64; ++i )
    {
        rocketnet_unit_check( payload[i] == (uint8_t) i );
    }

    // the same with an IPv6 name

    memset( buf, 0xA5, sizeof( buf ) );
    put_u32_native( buf + 0, 28 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 64 );
    put_u32_native( buf + 12, 0 );

    struct sockaddr_in6 in6;
    memset( &in6, 0, sizeof( in6 ) );
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons( 40000 );
    in6.sin6_addr.s6_addr[15] = 1;
    memcpy( buf + 16, &in6, sizeof( in6 ) );

    for ( int i = 0; i < 64; ++i )
    {
        buf[16 + (int) sizeof( struct sockaddr_storage ) + i] = (uint8_t) ( 64 - i );
    }

    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), (int) sizeof( struct sockaddr_storage ), 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_OK );
    rocketnet_unit_check( name_bytes == 28 );
    rocketnet_unit_check( name == buf + 16 );
    rocketnet_unit_check( memcmp( name, &in6, sizeof( in6 ) ) == 0 );
    rocketnet_unit_check( payload_bytes == 64 );
    rocketnet_unit_check( payload == buf + 16 + (int) sizeof( struct sockaddr_storage ) );

    for ( int i = 0; i < 64; ++i )
    {
        rocketnet_unit_check( payload[i] == (uint8_t) ( 64 - i ) );
    }
}

static void test_io_uring_transport_codec_recvmsg_refuses_truncation_and_short_buffers()
{
    const uint8_t * name;
    const uint8_t * payload;
    int name_bytes;
    int payload_bytes;

    // short buffer: cannot hold header plus both reserved areas (16+32+8=56
    // > 55).

    uint8_t shortbuf[55];
    memset( shortbuf, 0, sizeof( shortbuf ) );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              shortbuf, (int) sizeof( shortbuf ), 32, 8,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_SHORT_BUFFER );

    // name truncated: buffer is big enough but out->namelen (16) > msg_namelen (8)

    uint8_t buf[64];
    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 16 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 0 );
    put_u32_native( buf + 12, 0 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), 8, 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_NAME_TRUNCATED );

    // payload truncated via MSG_TRUNC

    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 0 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 0 );
    put_u32_native( buf + 12, (uint32_t) MSG_TRUNC );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), 8, 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED );

    // payload truncated: payloadlen exceeds the bytes left in the buffer (100
    // > 64-16-8=40).

    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 0 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 100 );
    put_u32_native( buf + 12, 0 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), 8, 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED );

    // payload truncated: a payloadlen with the top bit set (0x80000000) is
    // past the bytes left, not a negative length, with MSG_TRUNC clear.

    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 0 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 0x80000000u );
    put_u32_native( buf + 12, 0 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), 8, 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED );

    // the control area is taken from the bytes left too: with 8 name and 8
    // control bytes, 64-16-8-8=32 bytes are left, so 33 is truncated and 32
    // is the whole payload, starting after both areas.

    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 8, 33 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), 8, 8,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED );

    put_u32_native( buf + 8, 32 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), 8, 8,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_OK );
    rocketnet_unit_check( payload == buf + 32 );
    rocketnet_unit_check( payload_bytes == 32 );

    // a zero-length payload accepted as OK with payload_bytes 0.

    uint8_t zerobuf[16];
    memset( zerobuf, 0, sizeof( zerobuf ) );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              zerobuf, (int) sizeof( zerobuf ), 0, 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_OK );
    rocketnet_unit_check( name_bytes == 0 );
    rocketnet_unit_check( payload_bytes == 0 );
    rocketnet_unit_check( payload == zerobuf + 16 );
}

static void test_io_uring_transport_codec_recvmsg_refuses_negative_lengths()
{
    const uint8_t * name;
    const uint8_t * payload;
    int name_bytes;
    int payload_bytes;

    // a negative msg_controllen must be refused before any arithmetic uses it

    uint8_t buf[64];
    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 16 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 0 );
    put_u32_native( buf + 12, 0 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), 32, -8,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_SHORT_BUFFER );

    // a negative msg_namelen must be refused the same way

    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), -32, 0,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_SHORT_BUFFER );

    // both negative

    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
                              buf, (int) sizeof( buf ), -16, -8,
                              &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_SHORT_BUFFER );
}

static void test_io_uring_transport_codec_sockaddr_normalizes_v4_mapped()
{
    rocketnet_address_t out;
    struct sockaddr_storage ss;

    // 127.0.0.1:40000 in a sockaddr_in

    memset( &ss, 0, sizeof( ss ) );
    ( (struct sockaddr_in *) &ss )->sin_family = AF_INET;
    ( (struct sockaddr_in *) &ss )->sin_port = htons( 40000 );
    ( (struct sockaddr_in *) &ss )->sin_addr.s_addr = htonl( 0x7f000001u );
    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in ), false, &out ) );
    rocketnet_unit_check( out.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( out.port == 40000 );
    rocketnet_unit_check( out.data.ipv4[0] == 127 && out.data.ipv4[1] == 0 );
    rocketnet_unit_check( out.data.ipv4[2] == 0 && out.data.ipv4[3] == 1 );

    // ::ffff:127.0.0.1:40000 on a dual stack socket becomes IPv4 127.0.0.1:40000

    memset( &ss, 0, sizeof( ss ) );
    struct sockaddr_in6 * in6 = (struct sockaddr_in6 *) &ss;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40000 );

    const uint8_t mapped[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1 };
    memcpy( &in6->sin6_addr, mapped, 16 );

    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in6 ), true, &out ) );
    rocketnet_unit_check( out.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( out.port == 40000 );
    rocketnet_unit_check( out.data.ipv4[0] == 127 && out.data.ipv4[1] == 0 );
    rocketnet_unit_check( out.data.ipv4[2] == 0 && out.data.ipv4[3] == 1 );

    // ::1:40000 stays IPv6

    memset( &ss, 0, sizeof( ss ) );
    in6 = (struct sockaddr_in6 *) &ss;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40000 );
    in6->sin6_addr.s6_addr[15] = 1;

    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in6 ), true, &out ) );
    rocketnet_unit_check( out.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( out.port == 40000 );
    rocketnet_unit_check( out.data.ipv6[0] == 0 && out.data.ipv6[7] == 1 );

    // AF_UNIX returns false

    memset( &ss, 0, sizeof( ss ) );
    ss.ss_family = AF_UNIX;
    rocketnet_unit_check( !rocketnet_uring_sockaddr_to_address(
        &ss, 16, false, &out ) );

    // a name shorter than its family's sockaddr returns false

    memset( &ss, 0, sizeof( ss ) );
    ( (struct sockaddr_in *) &ss )->sin_family = AF_INET;
    rocketnet_unit_check( !rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in ) - 1, false, &out ) );
    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in ), false, &out ) );

    memset( &ss, 0, sizeof( ss ) );
    ( (struct sockaddr_in6 *) &ss )->sin6_family = AF_INET6;
    rocketnet_unit_check( !rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in6 ) - 1, true, &out ) );
}

// the segmented send's msghdr (rocketnet_uring_gso_message): one iovec per
// packet of the run, a UDP_SEGMENT cmsg holding the stride when the run is
// longer than one, the shorter tail as the last iovec, and a longer packet
// or another destination ending the run. no socket: the msghdr is read back
// field by field.

static uint16_t gso_message_stride( const struct msghdr * message )
{
    const struct cmsghdr * cmsg = CMSG_FIRSTHDR( message );
    rocketnet_unit_check( cmsg != NULL );
    rocketnet_unit_check( cmsg->cmsg_level == IPPROTO_UDP );
    rocketnet_unit_check( cmsg->cmsg_type == UDP_SEGMENT );
    rocketnet_unit_check( cmsg->cmsg_len == CMSG_LEN( sizeof( uint16_t ) ) );

    uint16_t stride = 0;
    memcpy( &stride, CMSG_DATA( cmsg ), sizeof( stride ) );
    return stride;
}

static void test_uring_gso_message_stride_and_tail()
{
    static uint8_t frames[8][1500];
    uint8_t * wire[8];
    for ( int i = 0; i < 8; i++ )
    {
        wire[i] = frames[i];
    }

    struct rocketnet_address_t a;
    struct rocketnet_address_t b;
    rocketnet_unit_check( rocketnet_address_parse( &a, "10.0.0.1:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &b, "10.0.0.2:40000" ) );

    struct rocketnet_address_t to[8];
    for ( int i = 0; i < 8; i++ )
    {
        to[i] = a;
    }

    struct sockaddr_storage name;
    memset( &name, 0, sizeof( name ) );

    const socklen_t namelen = (socklen_t) sizeof( struct sockaddr_in );

    struct msghdr message;
    struct iovec iovecs[8];
    alignas( 8 ) uint8_t control[ROCKETNET_URING_GSO_CONTROL_BYTES];

    // an equal-sized run: one msghdr, one iovec per packet, the stride in
    // the cmsg

    {
        const int bytes[4] = { 400, 400, 400, 400 };
        const int run = rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                     wire, bytes, to, 4, 64, 65000 );
        rocketnet_unit_check( run == 4 );
        rocketnet_unit_check( message.msg_name == &name );
        rocketnet_unit_check( message.msg_namelen == namelen );
        rocketnet_unit_check( message.msg_iov == iovecs );
        rocketnet_unit_check( message.msg_iovlen == 4 );

        for ( int i = 0; i < 4; i++ )
        {
            rocketnet_unit_check( iovecs[i].iov_base == wire[i] );
            rocketnet_unit_check( iovecs[i].iov_len == 400 );
        }

        rocketnet_unit_check( message.msg_control == control );
        rocketnet_unit_check( message.msg_controllen == ROCKETNET_URING_GSO_CONTROL_BYTES );
        rocketnet_unit_check( gso_message_stride( &message ) == 400 );
    }

    // a shorter tail is the run's last iovec and closes the run

    {
        const int bytes[5] = { 400, 400, 400, 150, 400 };
        const int run = rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                     wire, bytes, to, 5, 64, 65000 );
        rocketnet_unit_check( run == 4 );
        rocketnet_unit_check( message.msg_iovlen == 4 );
        rocketnet_unit_check( iovecs[2].iov_len == 400 );
        rocketnet_unit_check( iovecs[3].iov_base == wire[3] );
        rocketnet_unit_check( iovecs[3].iov_len == 150 );
        rocketnet_unit_check( gso_message_stride( &message ) == 400 );
    }

    // a one-packet run carries no cmsg

    {
        const int bytes[1] = { 400 };
        const int run = rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                     wire, bytes, to, 1, 64, 65000 );
        rocketnet_unit_check( run == 1 );
        rocketnet_unit_check( message.msg_iovlen == 1 );
        rocketnet_unit_check( iovecs[0].iov_len == 400 );
        rocketnet_unit_check( message.msg_control == NULL );
        rocketnet_unit_check( message.msg_controllen == 0 );
    }

    // a longer packet ends the run before it

    {
        const int bytes[3] = { 400, 400, 500 };
        const int run = rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                     wire, bytes, to, 3, 64, 65000 );
        rocketnet_unit_check( run == 2 );
        rocketnet_unit_check( message.msg_iovlen == 2 );
        rocketnet_unit_check( gso_message_stride( &message ) == 400 );
    }

    // another destination ends the run before it

    {
        const int bytes[3] = { 400, 400, 400 };
        to[2] = b;
        const int run = rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                     wire, bytes, to, 3, 64, 65000 );
        to[2] = a;

        rocketnet_unit_check( run == 2 );
        rocketnet_unit_check( message.msg_iovlen == 2 );
    }

    // an aborted packet (no bytes) ends the run; at the front there is no run

    {
        const int bytes[3] = { 400, 0, 400 };
        rocketnet_unit_check( rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                           wire, bytes, to, 3, 64, 65000 ) == 1 );
        rocketnet_unit_check( message.msg_control == NULL );

        const int aborted[2] = { 0, 400 };
        rocketnet_unit_check( rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                           wire, aborted, to, 2, 64, 65000 ) == 0 );
    }

    // the segment and byte bounds, and no control buffer, limit the run

    {
        const int bytes[8] = { 400, 400, 400, 400, 400, 400, 400, 400 };
        rocketnet_unit_check( rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                           wire, bytes, to, 8, 3, 65000 ) == 3 );
        rocketnet_unit_check( rocketnet_uring_gso_message( &message, iovecs, control, (int) sizeof( control ), &name, namelen,
                                                           wire, bytes, to, 8, 64, 1000 ) == 2 );
        rocketnet_unit_check( rocketnet_uring_gso_message( &message, iovecs, NULL, 0, &name, namelen,
                                                           wire, bytes, to, 8, 64, 65000 ) == 1 );
        rocketnet_unit_check( message.msg_control == NULL );
    }
}

// the multishot buffer of a UDP_GRO socket: header, a sockaddr_storage name
// area, a ROCKETNET_URING_GRO_CONTROL_BYTES control area, then the payload.
// cmsg_len 0 writes no cmsg. control_written is the controllen the kernel
// reports.

static int gro_buffer_build( uint8_t * buf, int buf_bytes, int control_written, uint32_t out_flags,
                             int cmsg_level, int cmsg_type, size_t cmsg_len, int stride, int payload_bytes )
{
    const int name_area = (int) sizeof( struct sockaddr_storage );
    const int control_area = (int) ROCKETNET_URING_GRO_CONTROL_BYTES;
    memset( buf, 0, (size_t) buf_bytes );
    put_u32_native( buf + 0, (uint32_t) sizeof( struct sockaddr_in ) );
    put_u32_native( buf + 4, (uint32_t) control_written );
    put_u32_native( buf + 8, (uint32_t) payload_bytes );
    put_u32_native( buf + 12, out_flags );

    struct sockaddr_in in4;
    memset( &in4, 0, sizeof( in4 ) );
    in4.sin_family = AF_INET;
    in4.sin_port = htons( 40000 );
    in4.sin_addr.s_addr = htonl( 0x7f000001u );
    memcpy( buf + 16, &in4, sizeof( in4 ) );

    if ( cmsg_len > 0 )
    {
        struct cmsghdr header;
        memset( &header, 0, sizeof( header ) );
        header.cmsg_len = cmsg_len;
        header.cmsg_level = cmsg_level;
        header.cmsg_type = cmsg_type;
        memcpy( buf + 16 + name_area, &header, sizeof( header ) );
        memcpy( buf + 16 + name_area + CMSG_LEN( 0 ), &stride, sizeof( stride ) );
    }

    for ( int i = 0; i < payload_bytes; ++i )
    {
        buf[16 + name_area + control_area + i] = (uint8_t) i;
    }

    return 16 + name_area + control_area + payload_bytes;
}

static void test_uring_recvmsg_gro_stride()
{
    const int name_area = (int) sizeof( struct sockaddr_storage );
    const int control_area = (int) ROCKETNET_URING_GRO_CONTROL_BYTES;

    uint8_t buf[1024];
    const uint8_t * name;
    const uint8_t * payload;
    int name_bytes;
    int payload_bytes;
    int segment_bytes;
    uint64_t faults;

    // a UDP_GRO cmsg: three segments of 100, 100 and 50 in one buffer

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), control_area, 0, IPPROTO_UDP, UDP_GRO, CMSG_LEN( sizeof( int ) ), 100, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, control_area,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 100 );
        rocketnet_unit_check( faults == 0 );
        rocketnet_unit_check( payload_bytes == 250 );
        rocketnet_unit_check( payload == buf + 16 + name_area + control_area );
        rocketnet_unit_check( payload[249] == (uint8_t) 249 );

        int offsets[4];
        int lengths[4];
        rocketnet_unit_check( rocketnet_coalesce_split( payload_bytes, segment_bytes, offsets, lengths, 4 ) == 3 );
        rocketnet_unit_check( offsets[2] == 200 && lengths[2] == 50 );
    }

    // no cmsg: the kernel wrote no control, so one datagram, no fault

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), 0, 0, 0, 0, 0, 0, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, control_area,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 0 );
        rocketnet_unit_check( faults == 0 );
        rocketnet_unit_check( payload_bytes == 250 );
    }

    // another cmsg, not UDP_GRO: no stride, no fault

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), control_area, 0, IPPROTO_IP, 1, CMSG_LEN( sizeof( int ) ), 100, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, control_area,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 0 );
        rocketnet_unit_check( faults == 0 );
    }

    // the faults, each one counted and the buffer one datagram: a UDP_GRO
    // cmsg too short for an int, a cmsg_len under its own header, a cmsg_len
    // past the control the kernel wrote, and a truncated control area

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), control_area, 0, IPPROTO_UDP, UDP_GRO, CMSG_LEN( sizeof( uint16_t ) ), 100, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, control_area,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 0 );
        rocketnet_unit_check( faults == 1 );
    }

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), control_area, 0, IPPROTO_UDP, UDP_GRO, sizeof( struct cmsghdr ) - 1, 100, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, control_area,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 0 );
        rocketnet_unit_check( faults == 1 );
    }

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), (int) CMSG_LEN( 0 ), 0, IPPROTO_UDP, UDP_GRO, CMSG_LEN( sizeof( int ) ), 100, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, control_area,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 0 );
        rocketnet_unit_check( faults == 1 );
    }

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), control_area, (uint32_t) MSG_CTRUNC, IPPROTO_UDP, UDP_GRO, CMSG_LEN( sizeof( int ) ), 100, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, control_area,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 0 );
        rocketnet_unit_check( faults == 1 );
    }

    // no control area reserved: the stride is 0 and nothing is read past
    // the name area

    {
        const int bytes = gro_buffer_build( buf, (int) sizeof( buf ), control_area, 0, IPPROTO_UDP, UDP_GRO, CMSG_LEN( sizeof( int ) ), 100, 250 );
        segment_bytes = -1;
        faults = 0;
        rocketnet_unit_check( rocketnet_uring_recvmsg_parse( buf, bytes, name_area, 0,
                                                             &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults ) == ROCKETNET_URING_RECV_OK );
        rocketnet_unit_check( segment_bytes == 0 );
        rocketnet_unit_check( faults == 0 );
        rocketnet_unit_check( payload == buf + 16 + name_area );
    }
}

REGISTER_UNIT_TEST( test_io_uring_transport_codec_tag_round_trips );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_recvmsg_parses_name_and_payload );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_recvmsg_refuses_truncation_and_short_buffers );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_recvmsg_refuses_negative_lengths );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_sockaddr_normalizes_v4_mapped );
REGISTER_UNIT_TEST( test_uring_gso_message_stride_and_tail );
REGISTER_UNIT_TEST( test_uring_recvmsg_gro_stride );

#if ROCKETNET_DEV && defined( ROCKETNET_IO_URING )

// segmented SENDMSGs refused by their destination: -EIO in the run's CQE, as
// the kernel answers a destination whose route cannot take UDP segmentation
// (an IPsec policy, say). no kernel ring and no socket: the fake completion
// source, which reads each submitted send's destination port and run length
// from its staging slot and refuses a run of more than one packet to the
// refusing port, or to every port.

#define URING_SEGMENT_FAKE_FIFO 8192             // completions the fake holds: a few thousand sends in one flush

struct uring_segment_fake_t
{
    struct rocketnet_server_uring_test_completion_t fifo[URING_SEGMENT_FAKE_FIFO];
    int head;
    int tail;
    struct rocketnet_server_uring_t * uring;
    uint16_t refuse_port;                   // nonzero: segmented runs to this port are refused
    bool refuse_all;                        // every segmented run is refused: a kernel that cannot segment
    uint16_t accept_port;                   // nonzero: segmented runs to every other port are refused
    int refusals;
    int segmented;
    int plain;
};

static int uring_segment_fake_count( struct uring_segment_fake_t * fake )
{
    return fake->tail - fake->head;
}

static void uring_segment_fake_push( struct uring_segment_fake_t * fake, uint64_t user_data, int32_t res, uint32_t flags )
{
    rocketnet_unit_check( uring_segment_fake_count( fake ) < URING_SEGMENT_FAKE_FIFO );

    struct rocketnet_server_uring_test_completion_t * completion = &fake->fifo[fake->tail % URING_SEGMENT_FAKE_FIFO];
    completion->user_data = user_data;
    completion->res = res;
    completion->flags = flags;
    fake->tail++;
}

static int uring_segment_fake_peek( void * context, struct rocketnet_server_uring_test_completion_t * completions, int max )
{
    struct uring_segment_fake_t * fake = (struct uring_segment_fake_t *) context;
    int count = uring_segment_fake_count( fake );
    if ( count > max )
    {
        count = max;
    }

    for ( int i = 0; i < count; i++ )
    {
        completions[i] = fake->fifo[( fake->head + i ) % URING_SEGMENT_FAKE_FIFO];
    }

    return count;
}

static void uring_segment_fake_advance( void * context, int count )
{
    struct uring_segment_fake_t * fake = (struct uring_segment_fake_t *) context;
    rocketnet_unit_check( count >= 0 && count <= uring_segment_fake_count( fake ) );
    fake->head += count;
}

static int uring_segment_fake_ready( void * context )
{
    return uring_segment_fake_count( (struct uring_segment_fake_t *) context );
}

static void uring_segment_fake_submitted( void * context, uint64_t user_data )
{
    struct uring_segment_fake_t * fake = (struct uring_segment_fake_t *) context;
    const int kind = rocketnet_uring_tag_kind( user_data );
    const int queue = rocketnet_uring_tag_queue( user_data );

    if ( kind == ROCKETNET_URING_TAG_SEND )
    {
        uint16_t port = 0;
        int run_packets = 0;
        rocketnet_server_uring_test_send_slot( fake->uring, (int) rocketnet_uring_tag_index( user_data ), &port, &run_packets );

        if ( run_packets > 1 && ( fake->refuse_all || ( fake->refuse_port != 0 && port == fake->refuse_port ) || ( fake->accept_port != 0 && port != fake->accept_port ) ) )
        {
            fake->refusals++;
            uring_segment_fake_push( fake, user_data, -EIO, 0 );
            return;
        }

        if ( run_packets > 1 )
        {
            fake->segmented++;
        }
        else
        {
            fake->plain++;
        }
        uring_segment_fake_push( fake, user_data, 64 * ( run_packets > 1 ? run_packets : 1 ), 0 );
    }
    else if ( kind == ROCKETNET_URING_TAG_CANCEL )
    {
        uring_segment_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue, 0 ), -ECANCELED, 0 );
        uring_segment_fake_push( fake, user_data, 0, 0 );
    }
}

static void uring_segment_stage( struct rocketnet_server_uring_t * uring, uint16_t port, int packets )
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
        uint8_t * frame = rocketnet_server_uring_start_packet( uring, &to, &packet_id );
        rocketnet_unit_check( frame );
        rocketnet_server_uring_finish_packet( uring, packet_id, frame, 64 );
    }
}

static struct rocketnet_server_uring_t * uring_segment_open( struct uring_segment_fake_t * fake, struct rocketnet_server_uring_test_source_t * source )
{
    memset( source, 0, sizeof( *source ) );
    source->context = fake;
    source->peek = uring_segment_fake_peek;
    source->advance = uring_segment_fake_advance;
    source->ready = uring_segment_fake_ready;
    source->submitted = uring_segment_fake_submitted;

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 1, source );
    rocketnet_unit_check( uring );
    fake->uring = uring;
    rocketnet_server_uring_test_set_features( uring, ROCKETNET_SERVER_URING_FEATURE_SEGMENT );
    return uring;
}

static void uring_segment_close( struct rocketnet_server_uring_t * uring )
{
    rocketnet_server_uring_stop( uring );
    rocketnet_unit_check( rocketnet_server_uring_stopped( uring ) );
    rocketnet_server_uring_destroy( uring );
}

// one destination refuses every segmented run, another accepts them and
// sends one packet an update, the normal game load. many updates later the
// accepting destination's run is still one segmented send, the refusing
// destination's packets all left, plain, nothing was dropped, and the
// refusing destination paid one refusal, not one a run.

static void test_uring_segment_refusal_is_per_destination()
{
    struct uring_segment_fake_t fake;
    memset( &fake, 0, sizeof( fake ) );
    fake.refuse_port = 40000;

    struct rocketnet_server_uring_test_source_t source;
    struct rocketnet_server_uring_t * uring = uring_segment_open( &fake, &source );

    const int rounds = 4 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
    for ( int i = 0; i < rounds; i++ )
    {
        uring_segment_stage( uring, 40000, 2 );
        uring_segment_stage( uring, 40001, 1 );
        rocketnet_server_uring_send_packets( uring );
    }

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );

    rocketnet_unit_check( fake.refusals == 1 );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == (uint64_t) ( 3 * rounds ) );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == 0 );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_GSO_FALLBACKS] == 0 );
    rocketnet_unit_check( ( rocketnet_server_uring_features( uring ) & ROCKETNET_SERVER_URING_FEATURE_SEGMENT ) != 0 );
    rocketnet_unit_check( rocketnet_server_uring_test_send_idle( uring ) );

    // the accepting destination's run is one segmented send; the refusing
    // destination's run in the same update leaves plain, unrefused
    const int segmented_before = fake.segmented;
    uring_segment_stage( uring, 40001, 2 );
    uring_segment_stage( uring, 40000, 2 );
    rocketnet_server_uring_send_packets( uring );
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_unit_check( fake.segmented == segmented_before + 1 );
    rocketnet_unit_check( fake.refusals == 1 );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == (uint64_t) ( 3 * rounds + 4 ) );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == 0 );

    uring_segment_close( uring );
}

REGISTER_UNIT_TEST( test_uring_segment_refusal_is_per_destination );

// a kernel that accepted the UDP_SEGMENT probe but refuses every segmented
// send, so no segmented send is ever accepted: each refusing destination's
// run is resent plain, and once ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD
// destinations refused, segmentation is off for the transport and later runs
// leave plain without a refusal. nothing is dropped.

static void test_uring_segment_refused_everywhere_turns_off()
{
    struct uring_segment_fake_t fake;
    memset( &fake, 0, sizeof( fake ) );
    fake.refuse_all = true;

    struct rocketnet_server_uring_test_source_t source;
    struct rocketnet_server_uring_t * uring = uring_segment_open( &fake, &source );

    for ( int i = 0; i < ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD; i++ )
    {
        rocketnet_unit_check( ( rocketnet_server_uring_features( uring ) & ROCKETNET_SERVER_URING_FEATURE_SEGMENT ) != 0 );
        uring_segment_stage( uring, (uint16_t) ( 41000 + i ), 2 );
        rocketnet_server_uring_send_packets( uring );
    }

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_unit_check( fake.refusals == ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD );
    rocketnet_unit_check( ( rocketnet_server_uring_features( uring ) & ROCKETNET_SERVER_URING_FEATURE_SEGMENT ) == 0 );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_GSO_FALLBACKS] == 1 );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == (uint64_t) ( 2 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD ) );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == 0 );

    // a new destination: two plain SENDMSGs, no refusal
    uring_segment_stage( uring, 42000, 2 );
    rocketnet_server_uring_send_packets( uring );
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_unit_check( fake.refusals == ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == (uint64_t) ( 2 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD + 2 ) );

    uring_segment_close( uring );
}

REGISTER_UNIT_TEST( test_uring_segment_refused_everywhere_turns_off );

// the cost of more refusing destinations than the refused table holds: one
// destination accepts segmented sends (so segmentation never turns off for
// the transport), D others refuse every segmented run, and each of the D + 1
// sends a run of two packets every update. The table holds
// ROCKETNET_SEGMENT_REFUSED_ENTRIES destinations, direct mapped, so with D
// above it the displaced destinations pay a refusal and a plain resend again
// on their next run, every update. Measured: refused segmented sends per
// update in steady state, and sends submitted per update (v1.1.0 and v1.1.1
// turned io_uring segmentation off at the first refusal: from then on two
// plain sends a destination and no refusal). Nothing is dropped.

static void uring_segment_many_refusers( int D, int * refusals_per_update, int * sends_per_update )
{
    struct uring_segment_fake_t * fake = new uring_segment_fake_t();
    memset( fake, 0, sizeof( *fake ) );
    fake->accept_port = 40001;

    struct rocketnet_server_uring_test_source_t source;
    struct rocketnet_server_uring_t * uring = uring_segment_open( fake, &source );

    const int warm = 20;
    const int measured = 20;
    int refusals_before = 0;
    int sends_before = 0;
    for ( int u = 0; u < warm + measured; u++ )
    {
        if ( u == warm )
        {
            refusals_before = fake->refusals;
            sends_before = fake->refusals + fake->segmented + fake->plain;
        }
        uring_segment_stage( uring, 40001, 2 );
        for ( int d = 0; d < D; d++ )
        {
            uring_segment_stage( uring, (uint16_t) ( 50000 + d ), 2 );
        }
        rocketnet_server_uring_send_packets( uring );
    }

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_unit_check( ( rocketnet_server_uring_features( uring ) & ROCKETNET_SERVER_URING_FEATURE_SEGMENT ) != 0 );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == 0 );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == (uint64_t) ( 2 * ( D + 1 ) * ( warm + measured ) ) );

    *refusals_per_update = ( fake->refusals - refusals_before ) / measured;
    *sends_per_update = ( fake->refusals + fake->segmented + fake->plain - sends_before ) / measured;

    uring_segment_close( uring );
    delete fake;
}

static void test_uring_segment_many_refusing_destinations_cost()
{
    const int Ds[] = { 8, 64, 65, 128, 1000 };
    for ( int i = 0; i < (int) ( sizeof( Ds ) / sizeof( Ds[0] ) ); i++ )
    {
        int refusals = 0;
        int sends = 0;
        uring_segment_many_refusers( Ds[i], &refusals, &sends );
        printf( "    D=%d refusing destinations: %d refused segmented sends and %d sends per update (v1.1.1: 0 and %d)\n", Ds[i], refusals, sends, 2 * ( Ds[i] + 1 ) );

        // at most the table's entries are remembered: every destination
        // beyond them is refused again each update
        rocketnet_unit_check( refusals <= Ds[i] );
        rocketnet_unit_check( refusals >= ( Ds[i] > ROCKETNET_SEGMENT_REFUSED_ENTRIES ? Ds[i] - ROCKETNET_SEGMENT_REFUSED_ENTRIES : 0 ) );
    }
}

REGISTER_UNIT_TEST( test_uring_segment_many_refusing_destinations_cost );

#endif // #if ROCKETNET_DEV && defined( ROCKETNET_IO_URING )

#endif // #if ROCKETNET_URING_CODEC_ACTIVE

// the refused-destination table (sdk/rocketnet_server_gso.h), driven in the
// flush's order: a destination's segmented run is refused and added only
// when the table does not hold it. A kernel that refuses every segmented
// send, ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD distinct
// destinations, two of them sharing one entry: the fallback fires once all
// of them have refused (proofs/tla/RNSegmentRefusal.tla KernelFallback,
// MCRNSegmentRefusalShared.cfg). Counting only refusals that filled an empty
// entry, the shared pair kept the count one short and it never fired.

static void segment_refused_address( struct rocketnet_address_t * address, int port )
{
    rocketnet_unit_check( rocketnet_address_parse( address, "10.0.0.1:0" ) );
    address->port = (uint16_t) port;
}

static void test_segment_refused_shared_entry_still_reaches_the_fallback()
{
    const int threshold = ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
    struct rocketnet_address_t destinations[ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD];
    bool used_entry[ROCKETNET_SEGMENT_REFUSED_ENTRIES] = {};

    // the first two share an entry; the rest each have their own

    segment_refused_address( &destinations[0], 40000 );
    const int shared = rocketnet_segment_refused_entry( &destinations[0] );
    used_entry[shared] = true;
    int found = 1;
    for ( int port = 40001; port < 60000 && found < threshold; port++ )
    {
        struct rocketnet_address_t candidate;
        segment_refused_address( &candidate, port );
        const int entry = rocketnet_segment_refused_entry( &candidate );
        if ( found == 1 ? entry == shared : !used_entry[entry] )
        {
            destinations[found++] = candidate;
            used_entry[entry] = true;
        }
    }
    rocketnet_unit_check( found == threshold );

    struct rocketnet_segment_refused_t refused;
    memset( &refused, 0, sizeof( refused ) );

    bool fallback = false;
    for ( int round = 0; round < 4 && !fallback; round++ )
    {
        for ( int i = 0; i < threshold && !fallback; i++ )
        {
            if ( !rocketnet_segment_refused_contains( &refused, &destinations[i] ) )
            {
                fallback = rocketnet_segment_refused_add( &refused, &destinations[i] );
            }
        }
    }

    rocketnet_unit_check( fallback );
}

REGISTER_UNIT_TEST( test_segment_refused_shared_entry_still_reaches_the_fallback );
