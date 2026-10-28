/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Windows sleep conversion (sdk/rocketnet_platform.cpp), as a pure
// function of the request: the relative 100-ns due time for the
// high-resolution waitable timer, rounded up and never zero; the fallback
// Sleep in whole milliseconds, rounded up and capped below INFINITE; zero,
// negative and NaN requests yield only; and the at most 50 us SwitchToThread
// tail. No clock, no sleeping: the values are exact.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#include <math.h>
#include <stdint.h>

struct rocketnet_platform_sleep_plan_t
{
    bool yield_only;
    int64_t due_time;
    uint32_t fallback_milliseconds;
};

extern rocketnet_platform_sleep_plan_t rocketnet_platform_sleep_plan( double time );
extern bool rocketnet_platform_sleep_tail_yields( double remaining );

static void test_platform_sleep_plan_exact_values()
{
    rocketnet_platform_sleep_plan_t plan;

    plan = rocketnet_platform_sleep_plan( 0.0005 );
    rocketnet_unit_check( !plan.yield_only );
    rocketnet_unit_check( plan.due_time == -5000 );
    rocketnet_unit_check( plan.fallback_milliseconds == 1 );

    plan = rocketnet_platform_sleep_plan( 0.001 );
    rocketnet_unit_check( !plan.yield_only );
    rocketnet_unit_check( plan.due_time == -10000 );
    rocketnet_unit_check( plan.fallback_milliseconds == 1 );

    plan = rocketnet_platform_sleep_plan( 1.0 / 60.0 );
    rocketnet_unit_check( !plan.yield_only );
    rocketnet_unit_check( plan.due_time == -166667 );
    rocketnet_unit_check( plan.fallback_milliseconds == 17 );

    // a positive request never rounds down to zero
    plan = rocketnet_platform_sleep_plan( 1.0e-12 );
    rocketnet_unit_check( !plan.yield_only );
    rocketnet_unit_check( plan.due_time == -1 );
    rocketnet_unit_check( plan.fallback_milliseconds == 1 );

    // a huge request clamps: no signed overflow, the fallback below INFINITE
    plan = rocketnet_platform_sleep_plan( 1.0e30 );
    rocketnet_unit_check( !plan.yield_only );
    rocketnet_unit_check( plan.due_time == -INT64_C( 9000000000000000000 ) );
    rocketnet_unit_check( plan.fallback_milliseconds == 0xFFFFFFFEu );

    plan = rocketnet_platform_sleep_plan( 0.0 );
    rocketnet_unit_check( plan.yield_only );

    plan = rocketnet_platform_sleep_plan( -1.0 );
    rocketnet_unit_check( plan.yield_only );

    plan = rocketnet_platform_sleep_plan( nan( "" ) );
    rocketnet_unit_check( plan.yield_only );
}

static void test_platform_sleep_tail_yields_at_most_50_us()
{
    rocketnet_unit_check( rocketnet_platform_sleep_tail_yields( 0.00005 ) );
    rocketnet_unit_check( rocketnet_platform_sleep_tail_yields( 0.00001 ) );
    rocketnet_unit_check( !rocketnet_platform_sleep_tail_yields( 0.000051 ) );
    rocketnet_unit_check( !rocketnet_platform_sleep_tail_yields( 0.001 ) );
    rocketnet_unit_check( !rocketnet_platform_sleep_tail_yields( 0.0 ) );
    rocketnet_unit_check( !rocketnet_platform_sleep_tail_yields( -0.001 ) );
    rocketnet_unit_check( !rocketnet_platform_sleep_tail_yields( nan( "" ) ) );
}

REGISTER_UNIT_TEST( test_platform_sleep_plan_exact_values );
REGISTER_UNIT_TEST( test_platform_sleep_tail_yields_at_most_50_us );
