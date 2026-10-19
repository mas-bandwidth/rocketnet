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

REGISTER_UNIT_TEST( test_client_bind_family_ipv4_server_without_ipv6 );
REGISTER_UNIT_TEST( test_client_bind_family_ipv4_server_with_ipv6 );
REGISTER_UNIT_TEST( test_client_bind_family_ipv6_server );
