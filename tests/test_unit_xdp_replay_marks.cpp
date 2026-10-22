/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The kernel replay map's key and value layout and the userspace
// pending-marks buffer. The bytes here are written by hand, not taken from
// the conversion code, so a mis-layout in either direction shows up. No
// sockets, no kernel, no real time.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_limits.h"
#include "../sdk/rocketnet_xdp_replay_marks.h"

// the AF_XDP code is Linux only, so its tests are built there only
#if defined(__linux__)

static struct rocketnet_address_t replay_marks_ipv4( uint16_t port )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof(a) );
    a.type = ROCKETNET_ADDRESS_IPV4;
    a.data.ipv4[0] = 198;
    a.data.ipv4[1] = 51;
    a.data.ipv4[2] = 100;
    a.data.ipv4[3] = 7;
    a.port = port;
    return a;
}

static struct rocketnet_address_t replay_marks_ipv6( uint16_t port )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof(a) );
    a.type = ROCKETNET_ADDRESS_IPV6;
    a.data.ipv6[0] = 0x2001;
    a.data.ipv6[1] = 0x0db8;
    a.data.ipv6[7] = 0x0007;
    a.port = port;
    return a;
}

static void test_xdp_transport_replay_key_matches_golden_bytes()
{
    // 198.51.100.7:50000 -> family 4, port 0xc3 0x50, IPv4 in the first 4
    // address bytes, twelve zero bytes after

    const uint8_t golden_v4[20] = { 4, 0, 0xc3, 0x50, 198, 51, 100, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    struct rocketnet_address_t a4 = replay_marks_ipv4( 50000 );
    struct rocketnet_server_xdp_replay_key_t key4;
    rocketnet_xdp_replay_key_from_address( &a4, &key4 );
    rocketnet_unit_check( sizeof(key4) == 20 );
    rocketnet_unit_check( memcmp( &key4, golden_v4, 20 ) == 0 );

    // [2001:db8::7]:50000 -> family 6, the 16 address bytes
    // 0x20 0x01 0x0d 0xb8 then eleven zeros then 0x07

    const uint8_t golden_v6[20] = { 6, 0, 0xc3, 0x50, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07 };

    struct rocketnet_address_t a6 = replay_marks_ipv6( 50000 );
    struct rocketnet_server_xdp_replay_key_t key6;
    rocketnet_xdp_replay_key_from_address( &a6, &key6 );
    rocketnet_unit_check( sizeof(key6) == 20 );
    rocketnet_unit_check( memcmp( &key6, golden_v6, 20 ) == 0 );

    // two addresses differing only in port give different keys

    struct rocketnet_address_t a4b = replay_marks_ipv4( 50001 );
    struct rocketnet_server_xdp_replay_key_t key4b;
    rocketnet_xdp_replay_key_from_address( &a4b, &key4b );
    rocketnet_unit_check( memcmp( &key4, &key4b, 20 ) != 0 );
}

static void test_xdp_transport_replay_marks_commit_forgets_before_marks()
{
    struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    struct rocketnet_address_t a = replay_marks_ipv4( 50000 );
    struct rocketnet_address_t b = replay_marks_ipv4( 50001 );

    rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &a ) );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, 41 ) );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &b, 7 ) );

    uint32_t forget_count = 0;
    uint32_t mark_count = 0;
    const struct rocketnet_server_xdp_replay_key_t * mark_keys = NULL;
    const struct rocketnet_server_xdp_replay_value_t * mark_values = NULL;

    rocketnet_xdp_replay_marks_forgets( &marks, &forget_count );
    rocketnet_unit_check( forget_count == 1 );

    // the one forget is A, in insertion order

    struct rocketnet_server_xdp_replay_key_t key_a;
    struct rocketnet_server_xdp_replay_key_t key_b;
    rocketnet_xdp_replay_key_from_address( &a, &key_a );
    rocketnet_xdp_replay_key_from_address( &b, &key_b );

    rocketnet_unit_check( memcmp( &marks.forgets[0], &key_a, 20 ) == 0 );

    // the two marks are A at 41 and B at 7, in insertion order

    rocketnet_xdp_replay_marks_marks( &marks, &mark_keys, &mark_values, &mark_count );
    rocketnet_unit_check( mark_count == 2 );
    rocketnet_unit_check( memcmp( &mark_keys[0], &key_a, 20 ) == 0 );
    rocketnet_unit_check( mark_values[0].high_water == 41 );
    rocketnet_unit_check( memcmp( &mark_keys[1], &key_b, 20 ) == 0 );
    rocketnet_unit_check( mark_values[1].high_water == 7 );

    // reset empties both lists

    rocketnet_xdp_replay_marks_reset( &marks );
    rocketnet_xdp_replay_marks_forgets( &marks, &forget_count );
    rocketnet_unit_check( forget_count == 0 );
    rocketnet_xdp_replay_marks_marks( &marks, &mark_keys, &mark_values, &mark_count );
    rocketnet_unit_check( mark_count == 0 );
}

static void test_xdp_transport_replay_marks_full_refuses_and_counts()
{
    struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    for ( uint32_t i = 0; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( (uint16_t) ( 2000 + i ) );
        rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, i ) );
    }

    struct rocketnet_address_t extra = replay_marks_ipv4( 60000 );
    rocketnet_unit_check( !rocketnet_xdp_replay_marks_mark( &marks, &extra, 100 ) );
    rocketnet_unit_check( marks.refused == 1 );

    rocketnet_xdp_replay_marks_reset( &marks );

    for ( uint32_t i = 0; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( (uint16_t) ( 2000 + i ) );
        rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &a ) );
    }

    rocketnet_unit_check( !rocketnet_xdp_replay_marks_forget( &marks, &extra ) );
    rocketnet_unit_check( marks.refused == 1 );
}

REGISTER_UNIT_TEST( test_xdp_transport_replay_key_matches_golden_bytes );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_commit_forgets_before_marks );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_full_refuses_and_counts );

#endif // #if defined(__linux__)
