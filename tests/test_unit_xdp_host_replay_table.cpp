/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim's replay table: entries set by the host, read back through
// the shim's get/set interface. The kernel never writes this table, so the
// shim declares no bpf_map_update_elem and no bpf_map_delete_elem; the host
// drives the table alone. No sockets, no kernel, no real time.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_limits.h"
#include "../sdk/rocketnet_xdp_replay_marks.h"

#if defined( ROCKETNET_XDP_FILTER_HOST ) && ROCKETNET_XDP_FILTER_HOST

#include "xdp_host/server_xdp_host.h"

static struct rocketnet_address_t replay_table_ipv4_bytes( const uint8_t bytes[4], uint16_t port )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ROCKETNET_ADDRESS_IPV4;
    memcpy( a.data.ipv4, bytes, 4 );
    a.port = port;
    return a;
}

static struct rocketnet_address_t replay_table_ipv6( uint16_t port )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ROCKETNET_ADDRESS_IPV6;
    a.data.ipv6[0] = 0x2001;
    a.data.ipv6[1] = 0x0db8;
    a.data.ipv6[7] = 0x0007;
    a.port = port;
    return a;
}

static void test_xdp_transport_host_replay_table_set_get_reset()
{
    static const uint8_t replay_table_v4[4] = { 1, 2, 3, 4 };
    static const uint8_t replay_table_fill[4] = { 10, 0, 0, 1 };

    struct rocketnet_server_xdp_replay_key_t key;
    struct rocketnet_server_xdp_replay_key_t key_other;
    uint32_t high_water = 0;

    rocketnet_xdp_host_reset();

    // get of an IPv4 key returns 0 and leaves a sentinel high_water untouched

    struct rocketnet_address_t a4 = replay_table_ipv4_bytes( replay_table_v4, 40000 );
    rocketnet_xdp_replay_key_from_address( &a4, &key );
    high_water = 12345;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key, &high_water ) == 0 );
    rocketnet_unit_check( high_water == 12345 );

    // set IPv4 1.2.3.4:40000 to 100: get returns 1 and 100

    rocketnet_xdp_host_set_replay_entry( &key, 100 );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key, &high_water ) == 1 );
    rocketnet_unit_check( high_water == 100 );

    // the same address on port 40001 returns 0

    struct rocketnet_address_t a4b = replay_table_ipv4_bytes( replay_table_v4, 40001 );
    rocketnet_xdp_replay_key_from_address( &a4b, &key_other );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key_other, &high_water ) == 0 );

    // an IPv6 key returns 0

    struct rocketnet_address_t a6 = replay_table_ipv6( 40000 );
    struct rocketnet_server_xdp_replay_key_t key6;
    rocketnet_xdp_replay_key_from_address( &a6, &key6 );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key6, &high_water ) == 0 );

    // set the IPv6 key to 7: both entries read back

    rocketnet_xdp_host_set_replay_entry( &key6, 7 );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key6, &high_water ) == 1 );
    rocketnet_unit_check( high_water == 7 );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key, &high_water ) == 1 );
    rocketnet_unit_check( high_water == 100 );

    // set the IPv4 key to 200: it reads 200 and the IPv6 key still reads 7

    rocketnet_xdp_host_set_replay_entry( &key, 200 );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key, &high_water ) == 1 );
    rocketnet_unit_check( high_water == 200 );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key6, &high_water ) == 1 );
    rocketnet_unit_check( high_water == 7 );

    // reset, then fill the table with MAX distinct keys (IPv4 10.0.0.1,
    // ports 1 up), each high_water its port

    rocketnet_xdp_host_reset();
    for ( uint32_t i = 1; i <= ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        struct rocketnet_address_t addr = replay_table_ipv4_bytes( replay_table_fill, (uint16_t) i );
        struct rocketnet_server_xdp_replay_key_t k;
        rocketnet_xdp_replay_key_from_address( &addr, &k );
        rocketnet_xdp_host_set_replay_entry( &k, i );
    }
    for ( uint32_t i = 1; i <= ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        struct rocketnet_address_t addr = replay_table_ipv4_bytes( replay_table_fill, (uint16_t) i );
        struct rocketnet_server_xdp_replay_key_t k;
        rocketnet_xdp_replay_key_from_address( &addr, &k );
        high_water = 0;
        rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &k, &high_water ) == 1 );
        rocketnet_unit_check( high_water == i );
    }

    // one more new key is ignored (get returns 0)

    struct rocketnet_address_t extra = replay_table_ipv4_bytes( replay_table_v4, 55555 );
    struct rocketnet_server_xdp_replay_key_t key_extra;
    rocketnet_xdp_replay_key_from_address( &extra, &key_extra );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key_extra, &high_water ) == 0 );

    // an existing key is still overwritten on the full table

    struct rocketnet_address_t first = replay_table_ipv4_bytes( replay_table_fill, 1 );
    struct rocketnet_server_xdp_replay_key_t key_first;
    rocketnet_xdp_replay_key_from_address( &first, &key_first );
    rocketnet_xdp_host_set_replay_entry( &key_first, 9999 );
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key_first, &high_water ) == 1 );
    rocketnet_unit_check( high_water == 9999 );

    // reset again: the first key reads 0

    rocketnet_xdp_host_reset();
    high_water = 0;
    rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key_first, &high_water ) == 0 );
}

REGISTER_UNIT_TEST( test_xdp_transport_host_replay_table_set_get_reset );

#endif // #if defined(ROCKETNET_XDP_FILTER_HOST) && ROCKETNET_XDP_FILTER_HOST
