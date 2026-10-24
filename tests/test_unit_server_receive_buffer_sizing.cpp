/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_constants.h"

#include <limits.h>
#include <stdio.h>

// The one server receive sizing request is pure arithmetic over the configured
// population and the update rate. No socket or clock is involved, so the exact
// bytes can be pinned here: one update of 256-kbps per-client traffic plus the
// stated margin, never below the historical fixed floor, rounded up to KiB.

static void test_server_receive_buffer_bytes_formula()
{
    // 1000 clients at 256 kbps: 32,000 bytes/s each.
    // 10 Hz: 3,200,000 bytes + 25% = 4,000,000, rounded to 4,000,768.
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1000, 10 ) == 4000768 );
    // 20 Hz: 1,600,000 + 25% = 2,000,000, rounded to 2,000,896.
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1000, 20 ) == 2000896 );
    // 30 Hz: ceil(32,000,000 / 30) = 1,066,667; + 25% floor = 1,333,333; rounded to 1,334,272.
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1000, 30 ) == 1334272 );
    // 100 Hz: 320,000 + 25% = 400,000, under the fixed floor, rounded to 1,000,448.
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1000, 100 ) == 1000448 );

    // One client never drops below the fixed floor at any of the four rates.
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1, 10 ) == 1000448 );
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1, 20 ) == 1000448 );
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1, 30 ) == 1000448 );
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1, 100 ) == 1000448 );

    // A rate below 1 Hz reads as 1 Hz, and a population below 1 reads as 1.
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 1000, 0 ) == rocketnet_server_receive_buffer_bytes( 1000, 1 ) );
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( 0, 10 ) == rocketnet_server_receive_buffer_bytes( 1, 10 ) );

    // The cap is INT_MAX, not wraparound, when the arithmetic exceeds an int.
    rocketnet_unit_check( rocketnet_server_receive_buffer_bytes( INT_MAX, 1 ) == INT_MAX );
}

REGISTER_UNIT_TEST( test_server_receive_buffer_bytes_formula );
