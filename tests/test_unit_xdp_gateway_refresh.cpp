/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gateway refresh of the reply MAC table (sdk/rocketnet_xdp_frame.h
// GATEWAY REFRESH). The resolve is a fake that answers whatever the case
// sets, the clock is an argument, and the reply MAC for an address is what
// start_packet gives a fill (rocketnet_xdp_reply_mac). No netlink, no
// sockets, no threads, no real time.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_xdp_frame.h"

struct fake_gateway_t
{
    bool resolved;
    bool found;
    uint8_t ipv4[6];
    uint8_t ipv6[6];
    int calls;
};

static bool fake_resolve_gateway( void * context, int address_type, uint8_t * mac, bool * found )
{
    struct fake_gateway_t * gateway = (struct fake_gateway_t *) context;
    gateway->calls++;
    memcpy( mac, address_type == ROCKETNET_ADDRESS_IPV6 ? gateway->ipv6 : gateway->ipv4, 6 );
    *found = gateway->found;
    return gateway->resolved;
}

static bool reply_mac_is( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * to, const uint8_t * mac )
{
    uint64_t refused = 0;
    const uint8_t * reply = rocketnet_xdp_reply_mac( table, to, NULL, &refused );
    return reply && memcmp( reply, mac, 6 ) == 0;
}

static struct rocketnet_xdp_reply_macs_t refresh_reply_macs;

static const uint8_t old_gateway[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t new_gateway[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

// one family: the router is replaced, and once the period passes every reply
// to an unlearned address takes the new MAC while a learned client keeps its own
static void test_xdp_transport_gateway_refresh_follows_a_replaced_router()
{
    static const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };

    struct rocketnet_xdp_reply_macs_t * table = &refresh_reply_macs;
    rocketnet_xdp_reply_macs_reset( table, old_gateway );

    rocketnet_address_t client, stranger;
    rocketnet_unit_check( rocketnet_address_parse( &client, "10.77.4.2:50000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &stranger, "203.0.113.9:40000" ) );
    rocketnet_xdp_reply_macs_learn( table, &client, client_mac );

    struct fake_gateway_t gateway;
    memset( &gateway, 0, sizeof( gateway ) );
    gateway.resolved = true;
    gateway.found = true;
    memcpy( gateway.ipv4, old_gateway, 6 );

    struct rocketnet_xdp_gateway_refresh_t refresh;
    memset( &refresh, 0, sizeof( refresh ) );
    refresh.enabled = true;
    refresh.address_type = ROCKETNET_ADDRESS_IPV4;

    struct rocketnet_xdp_gateway_change_t changes[2];
    const double start = 1000.0;

    // the first call schedules only: create has just resolved

    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, start, fake_resolve_gateway, &gateway, changes ) == 0 );
    rocketnet_unit_check( gateway.calls == 0 );

    // an unchanged gateway resolves and changes nothing

    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, start + ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS, fake_resolve_gateway, &gateway, changes ) == 0 );
    rocketnet_unit_check( gateway.calls == 1 );

    // the router is replaced: inside the period nothing resolves and replies take the old MAC

    memcpy( gateway.ipv4, new_gateway, 6 );
    const double replaced = start + 2.0 * ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS - 0.001;
    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, replaced, fake_resolve_gateway, &gateway, changes ) == 0 );
    rocketnet_unit_check( gateway.calls == 1 );
    rocketnet_unit_check( reply_mac_is( table, &stranger, old_gateway ) );

    // past the period: one change, recorded, and replies follow the new router

    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, replaced + 0.001, fake_resolve_gateway, &gateway, changes ) == 1 );
    rocketnet_unit_check( gateway.calls == 2 );
    rocketnet_unit_check( changes[0].address_type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( changes[0].was_known );
    rocketnet_unit_check( memcmp( changes[0].was, old_gateway, 6 ) == 0 );
    rocketnet_unit_check( memcmp( changes[0].mac, new_gateway, 6 ) == 0 );
    rocketnet_unit_check( reply_mac_is( table, &stranger, new_gateway ) );
    rocketnet_unit_check( reply_mac_is( table, &client, client_mac ) );
}

// a failed resolve, no default route and an unusable MAC keep the gateway the
// table has; the override's disabled refresh resolves nothing
static void test_xdp_transport_gateway_refresh_keeps_the_gateway_on_a_bad_resolve()
{
    static const uint8_t multicast_mac[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };

    struct rocketnet_xdp_reply_macs_t * table = &refresh_reply_macs;
    rocketnet_xdp_reply_macs_reset( table, old_gateway );

    rocketnet_address_t stranger;
    rocketnet_unit_check( rocketnet_address_parse( &stranger, "203.0.113.9:40000" ) );

    struct fake_gateway_t gateway;
    memset( &gateway, 0, sizeof( gateway ) );
    memcpy( gateway.ipv4, new_gateway, 6 );

    struct rocketnet_xdp_gateway_refresh_t refresh;
    memset( &refresh, 0, sizeof( refresh ) );
    refresh.enabled = true;
    refresh.address_type = ROCKETNET_ADDRESS_IPV4;

    struct rocketnet_xdp_gateway_change_t changes[2];
    double time = 1000.0;
    rocketnet_xdp_gateway_refresh( &refresh, table, time, fake_resolve_gateway, &gateway, changes );

    gateway.resolved = false;
    gateway.found = true;
    time += ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS;
    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, time, fake_resolve_gateway, &gateway, changes ) == 0 );

    gateway.resolved = true;
    gateway.found = false;
    time += ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS;
    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, time, fake_resolve_gateway, &gateway, changes ) == 0 );

    gateway.found = true;
    memcpy( gateway.ipv4, multicast_mac, 6 );
    time += ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS;
    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, time, fake_resolve_gateway, &gateway, changes ) == 0 );

    memcpy( gateway.ipv4, zero_mac, 6 );
    time += ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS;
    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, time, fake_resolve_gateway, &gateway, changes ) == 0 );

    rocketnet_unit_check( gateway.calls == 4 );
    rocketnet_unit_check( reply_mac_is( table, &stranger, old_gateway ) );

    refresh.enabled = false;
    memcpy( gateway.ipv4, new_gateway, 6 );
    time += ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS;
    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, time, fake_resolve_gateway, &gateway, changes ) == 0 );
    rocketnet_unit_check( gateway.calls == 4 );
    rocketnet_unit_check( reply_mac_is( table, &stranger, old_gateway ) );
}

// both families: each per-family gateway refreshes on its own, and a family
// with no gateway at create gains one when a default route appears
static void test_xdp_transport_gateway_refresh_both_families()
{
    static const uint8_t ipv6_gateway[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x06 };

    struct rocketnet_xdp_reply_macs_t * table = &refresh_reply_macs;
    rocketnet_xdp_reply_macs_reset( table, NULL );
    memcpy( table->gateway_ipv4, old_gateway, 6 );
    table->gateway_ipv4_known = true;

    rocketnet_address_t stranger4, stranger6;
    rocketnet_unit_check( rocketnet_address_parse( &stranger4, "203.0.113.9:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &stranger6, "[2001:db8::9]:40000" ) );

    uint64_t refused = 0;
    rocketnet_unit_check( rocketnet_xdp_reply_mac( table, &stranger6, NULL, &refused ) == NULL );

    struct fake_gateway_t gateway;
    memset( &gateway, 0, sizeof( gateway ) );
    gateway.resolved = true;
    gateway.found = true;
    memcpy( gateway.ipv4, new_gateway, 6 );
    memcpy( gateway.ipv6, ipv6_gateway, 6 );

    struct rocketnet_xdp_gateway_refresh_t refresh;
    memset( &refresh, 0, sizeof( refresh ) );
    refresh.enabled = true;
    refresh.serves_both = true;

    struct rocketnet_xdp_gateway_change_t changes[2];
    rocketnet_xdp_gateway_refresh( &refresh, table, 1000.0, fake_resolve_gateway, &gateway, changes );
    rocketnet_unit_check( rocketnet_xdp_gateway_refresh( &refresh, table, 1000.0 + ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS, fake_resolve_gateway, &gateway, changes ) == 2 );
    rocketnet_unit_check( gateway.calls == 2 );
    rocketnet_unit_check( changes[0].address_type == ROCKETNET_ADDRESS_IPV4 && changes[0].was_known );
    rocketnet_unit_check( changes[1].address_type == ROCKETNET_ADDRESS_IPV6 && !changes[1].was_known );
    rocketnet_unit_check( !table->gateway_known );
    rocketnet_unit_check( reply_mac_is( table, &stranger4, new_gateway ) );
    rocketnet_unit_check( reply_mac_is( table, &stranger6, ipv6_gateway ) );
}

REGISTER_UNIT_TEST( test_xdp_transport_gateway_refresh_follows_a_replaced_router );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_refresh_keeps_the_gateway_on_a_bad_resolve );
REGISTER_UNIT_TEST( test_xdp_transport_gateway_refresh_both_families );
