/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gso server transport's worker placement (sdk/rocketnet_gso_pin.h):
// queue q goes on cpu num_os_cpus + q, the XDP transport's own rule, and a
// cpu outside the cpu set or not online is refused, never clamped. Pure: no
// thread, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_gso_pin.h"

#define check rocketnet_unit_check

static void test_gso_pin_cpu_placement()
{
    // the owner's layout: cpu 0 the OS, two queues
    check( rocketnet_gso_pin_cpu( 0, 1 ) == 1 );
    check( rocketnet_gso_pin_cpu( 1, 1 ) == 2 );
    // two cpus left to the OS
    check( rocketnet_gso_pin_cpu( 0, 2 ) == 2 );
    check( rocketnet_gso_pin_cpu( 3, 2 ) == 5 );
}

static void test_gso_pin_check_refusals()
{
    check( rocketnet_gso_pin_check( 1, 1024, 32 ) == ROCKETNET_GSO_PIN_OK );
    check( rocketnet_gso_pin_check( 31, 1024, 32 ) == ROCKETNET_GSO_PIN_OK );
    // the last online cpu is online - 1
    check( rocketnet_gso_pin_check( 32, 1024, 32 ) == ROCKETNET_GSO_PIN_OFFLINE );
    // the cpu set's own bound comes first
    check( rocketnet_gso_pin_check( 1024, 1024, 4096 ) == ROCKETNET_GSO_PIN_OUTSIDE_SET );
    check( rocketnet_gso_pin_check( 1023, 1024, 4096 ) == ROCKETNET_GSO_PIN_OK );
    check( rocketnet_gso_pin_check( -1, 1024, 32 ) == ROCKETNET_GSO_PIN_OUTSIDE_SET );
    // a one cpu machine has no cpu 1
    check( rocketnet_gso_pin_check( 1, 1024, 1 ) == ROCKETNET_GSO_PIN_OFFLINE );
}

// the OS owns cpu 0 on Linux: with the server's default num_os_cpus (1, the
// builtin driver's value for any setting below 1) no queue's worker is ever
// placed on cpu 0, however many queues there are
static void test_gso_pin_never_cpu_zero()
{
    for ( int queue = 0; queue < 64; queue++ )
    {
        check( rocketnet_gso_pin_cpu( queue, 1 ) == 1 + queue );
        check( rocketnet_gso_pin_cpu( queue, 1 ) != 0 );
    }
}

REGISTER_UNIT_TEST( test_gso_pin_cpu_placement );
REGISTER_UNIT_TEST( test_gso_pin_never_cpu_zero );
REGISTER_UNIT_TEST( test_gso_pin_check_refusals );
