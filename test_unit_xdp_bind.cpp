/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP bind mode (sdk/rocketnet_xdp_bind.h): copy by default,
// zero-copy only for ROCKETNET_XDP_ZEROCOPY=1. Pure logic: no socket, no
// environment read (the value is an argument), no clock.

#include "test_unit_registry.h"

#include "sdk/rocketnet_xdp_bind.h"

static void xdp_bind_defaults_to_copy_mode()
{
    const uint16_t flags = rocketnet_xdp_bind_flags( NULL );
    rocketnet_unit_check( flags == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_COPY ) );
    rocketnet_unit_check( ( flags & ROCKETNET_XDP_BIND_ZEROCOPY ) == 0 );
    rocketnet_unit_check( !rocketnet_xdp_want_zerocopy( NULL ) );
}

static void xdp_bind_zerocopy_only_for_exactly_one()
{
    const uint16_t flags = rocketnet_xdp_bind_flags( "1" );
    rocketnet_unit_check( flags == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_ZEROCOPY ) );
    rocketnet_unit_check( ( flags & ROCKETNET_XDP_BIND_COPY ) == 0 );

    const char * not_one[] = { "", "0", "2", "01", "1 ", " 1", "yes", "true", "on", "zerocopy" };
    for ( size_t i = 0; i < sizeof( not_one ) / sizeof( not_one[0] ); i++ )
    {
        rocketnet_unit_check( !rocketnet_xdp_want_zerocopy( not_one[i] ) );
        rocketnet_unit_check( rocketnet_xdp_bind_flags( not_one[i] ) == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_COPY ) );
    }
}

static void xdp_bind_always_names_exactly_one_mode()
{
    const char * values[] = { NULL, "", "0", "1", "yes" };
    for ( size_t i = 0; i < sizeof( values ) / sizeof( values[0] ); i++ )
    {
        const uint16_t flags = rocketnet_xdp_bind_flags( values[i] );
        const bool copy = ( flags & ROCKETNET_XDP_BIND_COPY ) != 0;
        const bool zerocopy = ( flags & ROCKETNET_XDP_BIND_ZEROCOPY ) != 0;
        rocketnet_unit_check( copy != zerocopy );
        rocketnet_unit_check( ( flags & ROCKETNET_XDP_BIND_USE_NEED_WAKEUP ) != 0 );
    }
}

REGISTER_UNIT_TEST( xdp_bind_defaults_to_copy_mode );
REGISTER_UNIT_TEST( xdp_bind_zerocopy_only_for_exactly_one );
REGISTER_UNIT_TEST( xdp_bind_always_names_exactly_one_mode );
