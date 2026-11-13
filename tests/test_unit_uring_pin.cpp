/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server transport's worker placement (sdk/rocketnet_uring_pin.h):
// queue q goes on cpu num_os_cpus + q, the XDP transport's own rule, and a
// cpu outside the cpu set or not online is refused, never clamped. Pure: no
// thread, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_uring_pin.h"

#define check rocketnet_unit_check

// the transport's queue limit (ROCKETNET_URING_MAX_QUEUES, sdk/rocketnet_server_uring.h)
#define ROCKETNET_URING_PIN_TEST_MAX_QUEUES 32

static void test_uring_pin_cpu_placement()
{
    // the owner's layout: cpu 0 the OS, two queues
    check( rocketnet_uring_pin_cpu( 0, 1 ) == 1 );
    check( rocketnet_uring_pin_cpu( 1, 1 ) == 2 );
    // two cpus left to the OS
    check( rocketnet_uring_pin_cpu( 0, 2 ) == 2 );
    check( rocketnet_uring_pin_cpu( 3, 2 ) == 5 );
}

// "the OS on core 0 is law on linux" (owner, 2026-11-13): the placement never
// chooses cpu 0 for any worker, whatever the queue and the number of cpus left
// to the OS. The server's default num_os_cpus is 1 (the builtin driver passes
// 1 when the config says 0), and the transport driven directly with 0 pins
// nothing at all; the placement is only asked with a positive count.

static void test_uring_pin_never_chooses_core_zero()
{
    for ( int num_os_cpus = 1; num_os_cpus <= 8; num_os_cpus++ )
    {
        for ( int queue = 0; queue < ROCKETNET_URING_PIN_TEST_MAX_QUEUES; queue++ )
        {
            const int cpu = rocketnet_uring_pin_cpu( queue, num_os_cpus );
            check( cpu >= 1 );
            check( cpu == num_os_cpus + queue );
        }
    }
}

static void test_uring_pin_check_refusals()
{
    check( rocketnet_uring_pin_check( 1, 1024, 32 ) == ROCKETNET_URING_PIN_OK );
    check( rocketnet_uring_pin_check( 31, 1024, 32 ) == ROCKETNET_URING_PIN_OK );
    // the last online cpu is online - 1
    check( rocketnet_uring_pin_check( 32, 1024, 32 ) == ROCKETNET_URING_PIN_OFFLINE );
    // the cpu set's own bound comes first
    check( rocketnet_uring_pin_check( 1024, 1024, 4096 ) == ROCKETNET_URING_PIN_OUTSIDE_SET );
    check( rocketnet_uring_pin_check( 1023, 1024, 4096 ) == ROCKETNET_URING_PIN_OK );
    check( rocketnet_uring_pin_check( -1, 1024, 32 ) == ROCKETNET_URING_PIN_OUTSIDE_SET );
    // a one cpu machine has no cpu 1
    check( rocketnet_uring_pin_check( 1, 1024, 1 ) == ROCKETNET_URING_PIN_OFFLINE );
}

// a refused worker is confined to the cpus of its own mask past the OS's, or
// to all of them but cpu 0 when none is past them; never cpu 0, and the mask
// is left alone only when nothing but cpu 0 is in it. Pure: the unit suite
// never sets an affinity (the confine itself is a functional case)

static void test_uring_pin_confine_never_allows_core_zero()
{
    const int words = 4;

    // every mask of the first 12 cpus, every num_os_cpus around them
    for ( uint64_t mask = 0; mask < ( (uint64_t) 1 << 12 ); mask++ )
    {
        for ( int num_os_cpus = -1; num_os_cpus <= 14; num_os_cpus++ )
        {
            uint64_t current[4] = { mask, 0, 0, 0 };
            uint64_t confined[4] = { ~(uint64_t) 0, ~(uint64_t) 0, ~(uint64_t) 0, ~(uint64_t) 0 };
            const bool ok = rocketnet_uring_pin_confine_words( current, words, num_os_cpus, confined );
            const int os_cpus = num_os_cpus < 1 ? 1 : num_os_cpus;
            const uint64_t past = os_cpus >= 64 ? 0 : ( mask & ~( ( (uint64_t) 1 << os_cpus ) - 1 ) );
            const uint64_t expected = past != 0 ? past : ( mask & ~(uint64_t) 1 );
            check( ok == ( expected != 0 ) );
            check( confined[0] == expected );
            check( ( confined[0] & 1 ) == 0 );
            check( confined[1] == 0 && confined[2] == 0 && confined[3] == 0 );
        }
    }

    // gaps in the numbering: cpus 0, 4, 5, 6 (the count online is 4)
    {
        uint64_t current[4] = { ( 1u << 0 ) | ( 1u << 4 ) | ( 1u << 5 ) | ( 1u << 6 ), 0, 0, 0 };
        uint64_t confined[4];
        check( rocketnet_uring_pin_confine_words( current, words, 1, confined ) );
        check( confined[0] == ( ( 1u << 4 ) | ( 1u << 5 ) | ( 1u << 6 ) ) );
    }

    // cpus past the first word: the OS's 70, the mask 0, 65, 130
    {
        uint64_t current[4] = { 1, 2, 4, 0 };
        uint64_t confined[4];
        check( rocketnet_uring_pin_confine_words( current, words, 70, confined ) );
        check( confined[0] == 0 && confined[1] == 0 && confined[2] == 4 && confined[3] == 0 );
        check( rocketnet_uring_pin_confine_words( current, words, 200, confined ) );
        check( confined[0] == 0 && confined[1] == 2 && confined[2] == 4 );
    }

    // cpu 0 alone, or nothing: left alone
    {
        uint64_t current[4] = { 1, 0, 0, 0 };
        uint64_t confined[4];
        check( !rocketnet_uring_pin_confine_words( current, words, 1, confined ) );
        check( confined[0] == 0 );
    }

    // the OS keeps at least cpu 0, whatever the config says
    check( rocketnet_uring_pin_os_cpus( 0 ) == 1 );
    check( rocketnet_uring_pin_os_cpus( -5 ) == 1 );
    check( rocketnet_uring_pin_os_cpus( 3 ) == 3 );
    check( rocketnet_uring_pin_cpu( 0, 0 ) == 1 );
}

REGISTER_UNIT_TEST( test_uring_pin_cpu_placement );
REGISTER_UNIT_TEST( test_uring_pin_confine_never_allows_core_zero );
REGISTER_UNIT_TEST( test_uring_pin_never_chooses_core_zero );
REGISTER_UNIT_TEST( test_uring_pin_check_refusals );
