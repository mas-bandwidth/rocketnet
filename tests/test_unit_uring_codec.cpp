/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring completion codec (sdk/rocketnet_uring_codec.h): pure byte
// codec, no sockets, no ring, no wall clock; the header body exists only on
// Linux with IORING_RECV_MULTISHOT uapi. Every native-endian kernel header
// field is written by this file, never by the code under test. The cases are
// built only where the header body is.

#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_uring_codec.h"

#if defined(__linux__) && defined(IORING_RECV_MULTISHOT)
#define ROCKETNET_URING_CODEC_ACTIVE 1
#else
#define ROCKETNET_URING_CODEC_ACTIVE 0
#endif

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

    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 )
        != rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ) );
    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 )
        != rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, 0 ) );
    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 )
        != rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 65535, 0 ) );
    rocketnet_unit_check( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 )
        != rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0xffffffff ) );

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
        buf[16 + (int) sizeof( struct sockaddr_storage ) + i] = (uint8_t) i;

    const uint8_t * name;
    const uint8_t * payload;
    int name_bytes;
    int payload_bytes;
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        buf, (int) sizeof( buf ), (int) sizeof( struct sockaddr_storage ), 0,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_OK );
    rocketnet_unit_check( name_bytes == 16 );
    rocketnet_unit_check( name == buf + 16 );
    rocketnet_unit_check( memcmp( name, &in4, 16 ) == 0 );
    rocketnet_unit_check( payload_bytes == 64 );
    rocketnet_unit_check( payload == buf + 16 + (int) sizeof( struct sockaddr_storage ) );
    for ( int i = 0; i < 64; ++i )
        rocketnet_unit_check( payload[i] == (uint8_t) i );

    // the same with an IPv6 name.
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
        buf[16 + (int) sizeof( struct sockaddr_storage ) + i] = (uint8_t) ( 64 - i );

    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        buf, (int) sizeof( buf ), (int) sizeof( struct sockaddr_storage ), 0,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_OK );
    rocketnet_unit_check( name_bytes == 28 );
    rocketnet_unit_check( name == buf + 16 );
    rocketnet_unit_check( memcmp( name, &in6, sizeof( in6 ) ) == 0 );
    rocketnet_unit_check( payload_bytes == 64 );
    rocketnet_unit_check( payload == buf + 16 + (int) sizeof( struct sockaddr_storage ) );
    for ( int i = 0; i < 64; ++i )
        rocketnet_unit_check( payload[i] == (uint8_t) ( 64 - i ) );
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
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_SHORT_BUFFER );

    // name truncated: buffer is big enough but out->namelen (16) > msg_namelen (8).
    uint8_t buf[64];
    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 16 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 0 );
    put_u32_native( buf + 12, 0 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        buf, (int) sizeof( buf ), 8, 0,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_NAME_TRUNCATED );

    // payload truncated via MSG_TRUNC.
    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 0 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 0 );
    put_u32_native( buf + 12, (uint32_t) MSG_TRUNC );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        buf, (int) sizeof( buf ), 8, 0,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED );

    // payload truncated: payloadlen exceeds the bytes left in the buffer (100
    // > 64-16-8=40).
    memset( buf, 0, sizeof( buf ) );
    put_u32_native( buf + 0, 0 );
    put_u32_native( buf + 4, 0 );
    put_u32_native( buf + 8, 100 );
    put_u32_native( buf + 12, 0 );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        buf, (int) sizeof( buf ), 8, 0,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED );

    // a zero-length payload accepted as OK with payload_bytes 0.
    uint8_t zerobuf[16];
    memset( zerobuf, 0, sizeof( zerobuf ) );
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        zerobuf, (int) sizeof( zerobuf ), 0, 0,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_OK );
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
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_SHORT_BUFFER );

    // a negative msg_namelen must be refused the same way
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        buf, (int) sizeof( buf ), -32, 0,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_SHORT_BUFFER );

    // both negative
    rocketnet_unit_check( rocketnet_uring_recvmsg_parse(
        buf, (int) sizeof( buf ), -16, -8,
        &name, &name_bytes, &payload, &payload_bytes ) == ROCKETNET_URING_RECV_SHORT_BUFFER );
}

static void test_io_uring_transport_codec_sockaddr_normalizes_v4_mapped()
{
    rocketnet_address_t out;
    struct sockaddr_storage ss;

    // 127.0.0.1:40000 in a sockaddr_in.
    memset( &ss, 0, sizeof( ss ) );
    ( (struct sockaddr_in*) &ss )->sin_family = AF_INET;
    ( (struct sockaddr_in*) &ss )->sin_port = htons( 40000 );
    ( (struct sockaddr_in*) &ss )->sin_addr.s_addr = htonl( 0x7f000001u );
    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in ), false, &out ) );
    rocketnet_unit_check( out.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( out.port == 40000 );
    rocketnet_unit_check( out.data.ipv4[0] == 127 && out.data.ipv4[1] == 0 );
    rocketnet_unit_check( out.data.ipv4[2] == 0 && out.data.ipv4[3] == 1 );

    // ::ffff:127.0.0.1:40000 on a dual stack socket becomes IPv4 127.0.0.1:40000.
    memset( &ss, 0, sizeof( ss ) );
    struct sockaddr_in6 * in6 = (struct sockaddr_in6*) &ss;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40000 );
    const uint8_t mapped[16] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff,127,0,0,1 };
    memcpy( &in6->sin6_addr, mapped, 16 );
    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in6 ), true, &out ) );
    rocketnet_unit_check( out.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( out.port == 40000 );
    rocketnet_unit_check( out.data.ipv4[0] == 127 && out.data.ipv4[1] == 0 );
    rocketnet_unit_check( out.data.ipv4[2] == 0 && out.data.ipv4[3] == 1 );

    // ::1:40000 stays IPv6.
    memset( &ss, 0, sizeof( ss ) );
    in6 = (struct sockaddr_in6*) &ss;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40000 );
    in6->sin6_addr.s6_addr[15] = 1;
    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in6 ), true, &out ) );
    rocketnet_unit_check( out.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( out.port == 40000 );
    rocketnet_unit_check( out.data.ipv6[0] == 0 && out.data.ipv6[7] == 1 );

    // AF_UNIX returns false.
    memset( &ss, 0, sizeof( ss ) );
    ss.ss_family = AF_UNIX;
    rocketnet_unit_check( !rocketnet_uring_sockaddr_to_address(
        &ss, 16, false, &out ) );

    // a name shorter than its family's sockaddr returns false.
    memset( &ss, 0, sizeof( ss ) );
    ( (struct sockaddr_in*) &ss )->sin_family = AF_INET;
    rocketnet_unit_check( !rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in ) - 1, false, &out ) );
    rocketnet_unit_check( rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in ), false, &out ) );

    memset( &ss, 0, sizeof( ss ) );
    ( (struct sockaddr_in6*) &ss )->sin6_family = AF_INET6;
    rocketnet_unit_check( !rocketnet_uring_sockaddr_to_address(
        &ss, (int) sizeof( struct sockaddr_in6 ) - 1, true, &out ) );
}

REGISTER_UNIT_TEST( test_io_uring_transport_codec_tag_round_trips );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_recvmsg_parses_name_and_payload );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_recvmsg_refuses_truncation_and_short_buffers );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_recvmsg_refuses_negative_lengths );
REGISTER_UNIT_TEST( test_io_uring_transport_codec_sockaddr_normalizes_v4_mapped );

#endif // #if ROCKETNET_URING_CODEC_ACTIVE
