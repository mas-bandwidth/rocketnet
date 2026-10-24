/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The one header-size rule behind every wire byte counter: the IP and UDP
// headers of the remote address's family, 28 for IPv4 and 48 for IPv6, and no headers
// for an address with no family. Time is never read and no socket is opened.

#include "test_unit_registry.h"

#include "rocketnet_layers.h"
#include "rocketnet_wire.h"

#define check rocketnet_unit_check

void test_wire_header_bytes_per_family()
{
    check( rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 ) == 28 );
    check( rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV6 ) == 48 );
    check( rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_NONE ) == 0 );

    check( rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 ) ==
           ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );
    check( rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV6 ) ==
           ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );
}

REGISTER_UNIT_TEST( test_wire_header_bytes_per_family );
