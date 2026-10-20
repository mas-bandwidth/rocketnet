/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client bind-family choice (rocketnet_platform_client_bind_family in
// sdk/rocketnet_platform.cpp): a host with no IPv6 stack binds IPv4 for an
// IPv4 server, refuses an IPv6 server, and every other combination follows
// the server family and the platform's dual-stack capability. No sockets and
// no clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"

#include <string.h>

// sdk/rocketnet_client_protocol.cpp: the server address a token selects on a
// host with or without IPv6
void rocketnet_client_protocol_pick_server_address( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                    bool ipv6_available,
                                                    struct rocketnet_address_t * address );

static void expect_bind_family( int server_address_type, bool ipv6_available, bool dual_stack, int expected )
{
    rocketnet_unit_check( rocketnet_platform_client_bind_family( server_address_type, ipv6_available, dual_stack ) == expected );
}

// An IPv4 server on a host with no IPv6 stack binds IPv4 whether or not the
// platform's dual-stack capability is reported.
static void test_client_bind_family_ipv4_server_without_ipv6()
{
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, false, true, ROCKETNET_ADDRESS_IPV4 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, false, false, ROCKETNET_ADDRESS_IPV4 );
}

// With IPv6 available, an IPv4 server uses dual stack where the platform has
// it and IPv4 alone where it does not.
static void test_client_bind_family_ipv4_server_with_ipv6()
{
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, true, true, ROCKETNET_ADDRESS_IPV6 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, true, false, ROCKETNET_ADDRESS_IPV4 );
}

// An IPv6 server binds IPv6 whenever an IPv6 socket can be made, and refuses
// the create when the host has no IPv6 stack.
static void test_client_bind_family_ipv6_server()
{
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, true, true, ROCKETNET_ADDRESS_IPV6 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, true, false, ROCKETNET_ADDRESS_IPV6 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, false, true, ROCKETNET_ADDRESS_NONE );
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, false, false, ROCKETNET_ADDRESS_NONE );
}

// A token naming 127.0.0.1:40000 and [::1]:40000. On a host with no IPv6
// the client selects the IPv4 address, whose bind family is IPv4; on a host
// with IPv6 it selects the IPv6 address. A token naming only one family
// selects that family whatever the host, and an IPv6-only token on a host
// with no IPv6 has no bind family (the create refuses it).
static void test_client_pick_server_address_by_host_capability()
{
    struct rocketnet_signed_connect_data_t token;
    memset( &token, 0, sizeof(token) );
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    const uint8_t loopback_ipv6[18] = { 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0x01,0x00, 0x40,0x9c };
    memcpy( token.ipv4_address, loopback_ipv4, sizeof(loopback_ipv4) );
    memcpy( token.ipv6_address, loopback_ipv6, sizeof(loopback_ipv6) );

    struct rocketnet_address_t ipv4_expected;
    struct rocketnet_address_t ipv6_expected;
    rocketnet_unit_check( rocketnet_address_parse( &ipv4_expected, "127.0.0.1:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &ipv6_expected, "[::1]:40000" ) );

    struct rocketnet_address_t picked;

    // both families named, a host with no IPv6: the IPv4 address
    token.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;
    rocketnet_client_protocol_pick_server_address( &token, false, &picked );
    rocketnet_unit_check( picked.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv4_expected ) );
    rocketnet_unit_check( rocketnet_platform_client_bind_family( picked.type, false, false ) == ROCKETNET_ADDRESS_IPV4 );

    // both families named, a host with IPv6: the IPv6 address
    rocketnet_client_protocol_pick_server_address( &token, true, &picked );
    rocketnet_unit_check( picked.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv6_expected ) );

    // one family named: that family on either host
    token.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    rocketnet_client_protocol_pick_server_address( &token, true, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv4_expected ) );
    rocketnet_client_protocol_pick_server_address( &token, false, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv4_expected ) );

    token.address_flags = ROCKETNET_ADDRESS_FLAG_IPV6;
    rocketnet_client_protocol_pick_server_address( &token, true, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv6_expected ) );
    rocketnet_client_protocol_pick_server_address( &token, false, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv6_expected ) );
    rocketnet_unit_check( rocketnet_platform_client_bind_family( picked.type, false, false ) == ROCKETNET_ADDRESS_NONE );
}

REGISTER_UNIT_TEST( test_client_bind_family_ipv4_server_without_ipv6 );
REGISTER_UNIT_TEST( test_client_bind_family_ipv4_server_with_ipv6 );
REGISTER_UNIT_TEST( test_client_bind_family_ipv6_server );
REGISTER_UNIT_TEST( test_client_pick_server_address_by_host_capability );
