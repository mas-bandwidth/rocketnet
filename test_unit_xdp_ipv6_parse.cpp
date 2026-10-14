/* RocketNet. Copyright 2026 Mas Bandwidth LLC. Licensed under AGPL-3.0. */

// test_unit_xdp_ipv6_parse.cpp -- the shared XDP parse function over
// in-memory frames, for IPv4 and IPv6: the accept case and every refusal.
// No socket, no time, no network: frames are built in memory and the shared
// parse function is called directly.

#include "test_unit_registry.h"
#include "rocketnet_xdp_parse.h"

#include <stdint.h>
#include <string.h>

static struct rocketnet_xdp_parse_config_t test_config()
{
    struct rocketnet_xdp_parse_config_t config;
    memset( &config, 0, sizeof(config) );
    config.public_port = (uint16_t) ( ( 7777 << 8 ) | ( 7777 >> 8 ) ); // 7777 big endian by byte swap on little endian hosts; set below portably
    // Portably store 7777 in big endian:
    uint8_t * p = (uint8_t *) &config.public_port;
    p[0] = (uint8_t) ( 7777 >> 8 ); p[1] = (uint8_t) ( 7777 & 0xff );
    uint8_t * a = (uint8_t *) &config.public_address;
    a[0] = 10; a[1] = 0; a[2] = 0; a[3] = 1;
    const uint8_t fv[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( config.filter_value, fv, ROCKETNET_FILTER_VALUE_BYTES );
    config.filter_epoch = ROCKETNET_FILTER_EPOCH_UNKEYED;
    return config;
}

// Builds a valid payload: type REQUEST (exact wire bytes) is fragile, so the
// accept case uses KEEP_ALIVE at its minimum wire bytes with the prefilter.
static int build_payload( uint8_t * out, int payload_bytes )
{
    memset( out, 0, (size_t) payload_bytes );
    const uint8_t fv[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    out[0] = ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET;
    out[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;
    memcpy( out + 2, fv, ROCKETNET_FILTER_VALUE_BYTES );
    return payload_bytes;
}

static int build_v6( uint8_t * frame, int payload_bytes, uint8_t next_header )
{
    memset( frame, 0, 512 );
    frame[12] = 0x86; frame[13] = 0xdd;
    uint8_t * ip6 = frame + 14;
    ip6[0] = 0x60;
    ip6[4] = (uint8_t) ( ( payload_bytes + 8 ) >> 8 ); ip6[5] = (uint8_t) ( payload_bytes + 8 );
    ip6[6] = next_header; ip6[7] = 64;
    ip6[8 + 15] = 1; // source ::1
    ip6[24 + 15] = 1; // destination ::1
    uint8_t * udp = frame + 14 + 40;
    udp[0] = 0x1f; udp[1] = 0x90; // source port 8080
    udp[2] = (uint8_t) ( 7777 >> 8 ); udp[3] = (uint8_t) 7777;
    build_payload( udp + 8, payload_bytes );
    return 14 + 40 + 8 + payload_bytes;
}

static int build_v4( uint8_t * frame, int payload_bytes )
{
    memset( frame, 0, 512 );
    frame[12] = 0x08; frame[13] = 0x00;
    uint8_t * ip = frame + 14;
    ip[0] = 0x45; ip[9] = 17;
    ip[12] = 10; ip[15] = 2; ip[16] = 10; ip[19] = 1;
    uint8_t * udp = frame + 14 + 20;
    udp[0] = 0x1f; udp[1] = 0x90;
    udp[2] = (uint8_t) ( 7777 >> 8 ); udp[3] = (uint8_t) 7777;
    build_payload( udp + 8, payload_bytes );
    return 14 + 20 + 8 + payload_bytes;
}

static void test_xdp_ipv6_parse_accept()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int payload_bytes = ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
    const int n = build_v6( frame, payload_bytes, 17 );
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_REDIRECT );
    rocketnet_unit_check( result.address_family == 6 );
    rocketnet_unit_check( result.payload_bytes == payload_bytes );
    rocketnet_unit_check( result.source_address[15] == 1 );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_accept );

static void test_xdp_ipv6_parse_accept_v4()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v4( frame, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_REDIRECT );
    rocketnet_unit_check( result.address_family == 4 );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_accept_v4 );

static void test_xdp_ipv6_parse_short_header()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v6( frame, 41, 17 );
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, 14 + 20, &config, &result ) == ROCKETNET_XDP_VERDICT_DROP_HEADER );
    (void) n;
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_short_header );

static void test_xdp_ipv6_parse_wrong_next_header()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v6( frame, 41, 6 ); // TCP
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_PASS );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_wrong_next_header );

static void test_xdp_ipv6_parse_fragment_refused()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    // Fixed header names Fragment (44) as the next header.
    const int n = build_v6( frame, 41, 44 );
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_DROP_HEADER );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_fragment_refused );

static void test_xdp_ipv6_parse_extension_past_bound()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    memset( frame, 0, sizeof(frame) );
    frame[12] = 0x86; frame[13] = 0xdd;
    uint8_t * ip6 = frame + 14;
    ip6[0] = 0x60; ip6[6] = 0; ip6[7] = 64; ip6[8 + 15] = 1; ip6[24 + 15] = 1;
    // Chain 5 Hop-by-Hop headers (bound is 4), each 8 bytes naming the next.
    int off = 14 + 40;
    for ( int i = 0; i < 5; i++ ) { frame[off] = 0; frame[off + 1] = 0; off += 8; }
    const int n = off + 8 + 41;
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_DROP_HEADER );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_extension_past_bound );

static void test_xdp_ipv6_parse_wrong_port()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v6( frame, 41, 17 );
    frame[14 + 40 + 2] = 0; frame[14 + 40 + 3] = 9;
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_PASS );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_wrong_port );

static void test_xdp_ipv6_parse_too_small()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v6( frame, 10, 17 );
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_TOO_SMALL );
    rocketnet_unit_check( ROCKETNET_XDP_VERDICT_TOO_SMALL == ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_too_small );

static void test_xdp_ipv6_parse_too_large()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    static uint8_t frame[2048];
    struct rocketnet_xdp_parse_result_t result;
    const int payload_bytes = ROCKETNET_RECEIVE_CEILING + 1;
    memset( frame, 0, sizeof(frame) );
    const int n = build_v6( frame, payload_bytes, 17 );
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_TOO_LARGE );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_too_large );

static void test_xdp_ipv6_parse_wrong_epoch()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v6( frame, 41, 17 );
    frame[14 + 40 + 8 + 1] = 9;
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_WRONG_EPOCH );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_wrong_epoch );

static void test_xdp_ipv6_parse_wrong_filter()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v6( frame, 41, 17 );
    frame[14 + 40 + 8 + 2] ^= 0xff;
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_WRONG_FILTER );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_wrong_filter );

static void test_xdp_ipv6_parse_invalid_type()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    const int n = build_v6( frame, 41, 17 );
    frame[14 + 40 + 8 + 0] = 200;
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_INVALID_TYPE );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_invalid_type );

static void test_xdp_ipv6_parse_extension_accept()
{
    struct rocketnet_xdp_parse_config_t config = test_config();
    uint8_t frame[512];
    struct rocketnet_xdp_parse_result_t result;
    memset( frame, 0, sizeof(frame) );
    frame[12] = 0x86; frame[13] = 0xdd;
    uint8_t * ip6 = frame + 14;
    ip6[0] = 0x60; ip6[6] = 60; ip6[7] = 64; ip6[8 + 15] = 1; ip6[24 + 15] = 1;
    int off = 14 + 40;
    frame[off] = 17; frame[off + 1] = 0; // Destination Options, 8 bytes, next = UDP
    uint8_t * udp = frame + off + 8;
    udp[0] = 0x1f; udp[1] = 0x90; udp[2] = (uint8_t) ( 7777 >> 8 ); udp[3] = (uint8_t) 7777;
    build_payload( udp + 8, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    const int n = off + 8 + 8 + ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
    rocketnet_unit_check( rocketnet_xdp_parse_frame( frame, n, &config, &result ) == ROCKETNET_XDP_VERDICT_REDIRECT );
}
REGISTER_UNIT_TEST( test_xdp_ipv6_parse_extension_accept );
