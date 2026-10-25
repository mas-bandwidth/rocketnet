/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// rocketnet_sleep clamps its argument before the platform sleep: a negative,
// zero or NaN request sleeps not at all, and a request longer than
// ROCKETNET_SLEEP_MAX_SECONDS sleeps that long, so the microsecond conversion
// never overflows. The durations are arguments; the only real sleeps here are
// the zero-length ones a refused request takes.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#include <float.h>
#include <math.h>

#define check rocketnet_unit_check

static void test_sleep_clamp_bounds_negative_nan_and_huge()
{
    check( rocketnet_sleep_clamp( -1.0 ) == 0.0 );
    check( rocketnet_sleep_clamp( -DBL_MAX ) == 0.0 );
    check( rocketnet_sleep_clamp( -(double) INFINITY ) == 0.0 );
    check( rocketnet_sleep_clamp( (double) NAN ) == 0.0 );
    check( rocketnet_sleep_clamp( 0.0 ) == 0.0 );

    check( rocketnet_sleep_clamp( 0.001 ) == 0.001 );
    check( rocketnet_sleep_clamp( 1000.0 ) == 1000.0 );
    check( rocketnet_sleep_clamp( ROCKETNET_SLEEP_MAX_SECONDS ) == ROCKETNET_SLEEP_MAX_SECONDS );

    check( rocketnet_sleep_clamp( ROCKETNET_SLEEP_MAX_SECONDS + 1.0 ) == ROCKETNET_SLEEP_MAX_SECONDS );
    check( rocketnet_sleep_clamp( 1.0e300 ) == ROCKETNET_SLEEP_MAX_SECONDS );
    check( rocketnet_sleep_clamp( DBL_MAX ) == ROCKETNET_SLEEP_MAX_SECONDS );
    check( rocketnet_sleep_clamp( (double) INFINITY ) == ROCKETNET_SLEEP_MAX_SECONDS );

    // the longest clamped request still fits the platform's int microseconds
    check( ROCKETNET_SLEEP_MAX_SECONDS * 1000000.0 < 2147483647.0 );

    // A DEV-only dispatch counter proves each refused request stays out of
    // the platform sleep call; this has no wall-clock or OS timing assertion.
#if ROCKETNET_DEV
    const uint64_t before = rocketnet_sleep_platform_dispatch_count_for_test();
    rocketnet_sleep( -1.0 );
    check( rocketnet_sleep_platform_dispatch_count_for_test() == before );
    rocketnet_sleep( (double) NAN );
    check( rocketnet_sleep_platform_dispatch_count_for_test() == before );
    rocketnet_sleep( -(double) INFINITY );
    check( rocketnet_sleep_platform_dispatch_count_for_test() == before );
#else
    rocketnet_sleep( -1.0 );
    rocketnet_sleep( (double) NAN );
    rocketnet_sleep( -(double) INFINITY );
#endif // #if ROCKETNET_DEV
}

REGISTER_UNIT_TEST( test_sleep_clamp_bounds_negative_nan_and_huge );
