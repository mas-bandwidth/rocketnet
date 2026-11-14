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

// the OS owns cpu 0 on Linux: a num_os_cpus below 1 reads as 1 (the
// transport's create and its test seam take rocketnet_gso_pin_os_cpus, and the
// placement clamps through it too), so no queue's worker is ever placed on
// cpu 0, whatever the setting and however many queues there are
static void test_gso_pin_never_cpu_zero()
{
    check( rocketnet_gso_pin_os_cpus( 0 ) == 1 );
    check( rocketnet_gso_pin_os_cpus( -5 ) == 1 );
    check( rocketnet_gso_pin_os_cpus( 1 ) == 1 );
    check( rocketnet_gso_pin_os_cpus( 3 ) == 3 );
    for ( int num_os_cpus = -5; num_os_cpus <= 8; num_os_cpus++ )
    {
        const int os_cpus = num_os_cpus < 1 ? 1 : num_os_cpus;
        for ( int queue = 0; queue < 64; queue++ )
        {
            check( rocketnet_gso_pin_cpu( queue, num_os_cpus ) == os_cpus + queue );
            check( rocketnet_gso_pin_cpu( queue, num_os_cpus ) >= 1 );
        }
    }
}

// a worker whose pin was refused is confined to the online cpus from
// num_os_cpus up, never cpu 0
static void test_gso_pin_refused_worker_never_cpu_zero()
{
    unsigned char allowed[1024];
    check( rocketnet_gso_pin_fallback( allowed, 16, 8, 1 ) == 7 );
    check( allowed[0] == 0 && allowed[1] == 1 && allowed[7] == 1 && allowed[8] == 0 );
    // two cpus left to the OS
    check( rocketnet_gso_pin_fallback( allowed, 16, 8, 2 ) == 6 );
    check( allowed[0] == 0 && allowed[1] == 0 && allowed[2] == 1 );
    // num_os_cpus below 1 still leaves cpu 0 out
    check( rocketnet_gso_pin_fallback( allowed, 16, 8, 0 ) == 7 );
    check( allowed[0] == 0 );
    // a one cpu machine has nothing but cpu 0: no cpu is allowed, the caller changes nothing
    check( rocketnet_gso_pin_fallback( allowed, 16, 1, 1 ) == 0 );
    // an online count past the set is cut at the set
    check( rocketnet_gso_pin_fallback( allowed, 4, 100, 1 ) == 3 );
    // num_os_cpus at or past the online count (the refused functional case,
    // online + 1000): every online cpu but cpu 0, not nothing
    check( rocketnet_gso_pin_fallback( allowed, 16, 8, 8 ) == 7 );
    check( allowed[0] == 0 && allowed[1] == 1 && allowed[7] == 1 );
    check( rocketnet_gso_pin_fallback( allowed, 16, 8, 1000 ) == 7 );
    check( allowed[0] == 0 );
    check( rocketnet_gso_pin_fallback( allowed, 1024, 4, 1024 ) == 3 );
    check( allowed[0] == 0 && allowed[3] == 1 );
    // two cpus online: cpu 1 only
    check( rocketnet_gso_pin_fallback( allowed, 16, 2, 5 ) == 1 && allowed[0] == 0 && allowed[1] == 1 );
}

// a Linux transport's network thread placement (xdp, gso, io_uring,
// portable), rocketnet_network_thread_cpu. Reserved (the default): thread q on
// cpu num_os_cpus + q, never cpu 0, a num_os_cpus below 1 read as 1. Not
// reserved (*_reserve_cpu_0 ROCKETNET_CPU_0_NOT_RESERVED): thread q on cpu q,
// num_os_cpus not applied
static void test_network_thread_cpu_reserved_and_not()
{
    // reserved: the owner's layout, the OS on cpu 0, the network threads next
    check( rocketnet_network_thread_cpu( 0, 1, true ) == 1 );
    check( rocketnet_network_thread_cpu( 1, 1, true ) == 2 );
    check( rocketnet_network_thread_cpu( 0, 2, true ) == 2 );
    check( rocketnet_network_thread_cpu( 3, 2, true ) == 5 );
    check( rocketnet_network_thread_cpu( 0, 0, true ) == 1 );
    check( rocketnet_network_thread_cpu( 1, -7, true ) == 2 );
    for ( int worker = 0; worker < 64; worker++ )
    {
        check( rocketnet_network_thread_cpu( worker, 1, true ) == rocketnet_gso_pin_cpu( worker, 1 ) );
        check( rocketnet_network_thread_cpu( worker, 1, true ) != 0 );
    }
    // not reserved: thread q on cpu q, whatever num_os_cpus says
    check( rocketnet_network_thread_cpu( 0, 1, false ) == 0 );
    check( rocketnet_network_thread_cpu( 1, 1, false ) == 1 );
    check( rocketnet_network_thread_cpu( 0, 4, false ) == 0 );
    check( rocketnet_network_thread_cpu( 3, 256, false ) == 3 );
    check( rocketnet_network_thread_cpu( 2, 0, false ) == 2 );
}

REGISTER_UNIT_TEST( test_network_thread_cpu_reserved_and_not );
REGISTER_UNIT_TEST( test_gso_pin_cpu_placement );
REGISTER_UNIT_TEST( test_gso_pin_refused_worker_never_cpu_zero );
REGISTER_UNIT_TEST( test_gso_pin_never_cpu_zero );
REGISTER_UNIT_TEST( test_gso_pin_check_refusals );
