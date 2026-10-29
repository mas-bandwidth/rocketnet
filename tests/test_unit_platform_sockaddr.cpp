/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Endian-neutral IPv4 sockaddr_in conversion in sdk/rocketnet_platform.cpp
// (rocketnet_platform_address_to_sockaddr_in and
// rocketnet_platform_sockaddr_in_to_address): rocketnet_address_t.data.ipv4
// and sockaddr_in.sin_addr are both four network-order bytes in memory, and
// sin_port is two big-endian bytes in memory. No socket, no clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_platform.h"

#if !defined( _WIN32 )
#include <netinet/in.h>
#include <sys/socket.h>
#endif // #if !defined( _WIN32 )

#include <stdint.h>
#include <string.h>

// Pins that rocketnet_platform_address_to_sockaddr_in (sdk/rocketnet_platform.cpp)
// writes the four IPv4 octets of "203.0.113.10:40000" into sin_addr as the
// exact memory bytes { 203, 0, 113, 10 } and port 40000 into sin_port as the
// exact memory bytes { 0x9c, 0x40 } on both little- and big-endian hosts.
static void test_platform_sockaddr_in_bytes_match_address_bytes()
{
    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "203.0.113.10:40000" ) );
    rocketnet_unit_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( address.port == 40000 );

    struct sockaddr_in sa;
    rocketnet_platform_address_to_sockaddr_in( &address, &sa );

    const uint8_t expected_addr[4] = { 203, 0, 113, 10 };
    const uint8_t expected_port[2] = { 0x9c, 0x40 };
    rocketnet_unit_check( sa.sin_family == AF_INET );
    rocketnet_unit_check( memcmp( &sa.sin_addr, expected_addr, 4 ) == 0 );
    rocketnet_unit_check( memcmp( &sa.sin_port, expected_port, 2 ) == 0 );
}

// Pins that rocketnet_platform_sockaddr_in_to_address and
// rocketnet_platform_address_to_sockaddr_in (sdk/rocketnet_platform.cpp)
// round-trip raw network-order memory bytes in sockaddr_in back to an address
// equal (rocketnet_address_equal) to the parsed string for "203.0.113.10:40000",
// "127.0.0.1", and "0.0.0.0".
static void test_platform_sockaddr_in_round_trips_address()
{
    struct
    {
        const char * text;
        uint8_t addr_bytes[4];
        uint8_t port_bytes[2];
    } cases[] = {
        { "203.0.113.10:40000", { 203, 0, 113, 10 }, { 0x9c, 0x40 } },
        { "127.0.0.1:40000", { 127, 0, 0, 1 }, { 0x9c, 0x40 } },
        { "0.0.0.0:0", { 0, 0, 0, 0 }, { 0x00, 0x00 } },
    };

    for ( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); ++i )
    {
        rocketnet_address_t expected;
        rocketnet_unit_check( rocketnet_address_parse( &expected, cases[i].text ) );

        struct sockaddr_in sa;
        memset( &sa, 0, sizeof( sa ) );
        sa.sin_family = AF_INET;
        memcpy( &sa.sin_addr, cases[i].addr_bytes, 4 );
        memcpy( &sa.sin_port, cases[i].port_bytes, 2 );

        rocketnet_address_t actual;
        rocketnet_platform_sockaddr_in_to_address( &sa, &actual );
        rocketnet_unit_check( rocketnet_address_equal( &actual, &expected ) );

        struct sockaddr_in round_trip;
        rocketnet_platform_address_to_sockaddr_in( &actual, &round_trip );
        rocketnet_unit_check( round_trip.sin_family == AF_INET );
        rocketnet_unit_check( memcmp( &round_trip.sin_addr, cases[i].addr_bytes, 4 ) == 0 );
        rocketnet_unit_check( memcmp( &round_trip.sin_port, cases[i].port_bytes, 2 ) == 0 );
    }
}

// Pins the same conversion for the loopback sender shape the Linux mmsg batch
// round trip compares (tests/functional/rocketnet_functional_tests.cpp
// test_client_io_linux_mmsg_batch_round_trip): 127.0.0.1 plus a nonzero port whose
// bytes are not symmetric. The mmsg fill and read helpers
// (rocketnet_platform_mmsg_fill_sockaddr and
// rocketnet_platform_mmsg_read_sockaddr, sdk/rocketnet_platform.cpp) call
// rocketnet_platform_address_to_sockaddr_in and
// rocketnet_platform_sockaddr_in_to_address, so a host-integer shift of the
// octets reverses the source address on a big-endian host.
static void test_loopback_transport_platform_sockaddr_in_mmsg_round_trip()
{
    rocketnet_address_t sender_bind;
    rocketnet_unit_check( rocketnet_address_parse( &sender_bind, "127.0.0.1:54321" ) );

    const uint8_t expected_addr[4] = { 127, 0, 0, 1 };
    const uint8_t expected_port[2] = { 0xD4, 0x31 };

    struct sockaddr_in received;
    memset( &received, 0, sizeof( received ) );
    received.sin_family = AF_INET;
    memcpy( &received.sin_addr, expected_addr, 4 );
    memcpy( &received.sin_port, expected_port, 2 );

    rocketnet_address_t from;
    rocketnet_platform_sockaddr_in_to_address( &received, &from );
    rocketnet_unit_check( rocketnet_address_equal( &from, &sender_bind ) );

    struct sockaddr_in filled;
    rocketnet_platform_address_to_sockaddr_in( &sender_bind, &filled );
    rocketnet_unit_check( filled.sin_family == AF_INET );
    rocketnet_unit_check( memcmp( &filled.sin_addr, expected_addr, 4 ) == 0 );
    rocketnet_unit_check( memcmp( &filled.sin_port, expected_port, 2 ) == 0 );
}

REGISTER_UNIT_TEST( test_platform_sockaddr_in_bytes_match_address_bytes );
REGISTER_UNIT_TEST( test_platform_sockaddr_in_round_trips_address );
REGISTER_UNIT_TEST( test_loopback_transport_platform_sockaddr_in_mmsg_round_trip );
