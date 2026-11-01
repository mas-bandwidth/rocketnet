/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The update clock the server, client and message objects share
// (rocketnet_update_clock_advance, proofs/tla/RNUpdateClock.tla). Time is an
// argument; no transport and no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"

#include <math.h>

static int update_clock_asserts = 0;

static void update_clock_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;

    update_clock_asserts++;
}

// rocketnet.h promises that an update time that goes backwards or is not
// finite is clamped to the last good time and counted as API misuse, and
// keeps asserts for NULL arguments: a bad time keeps the last good time,
// counts, and does not assert, so a debug build's default handler does not
// trap on a caller's backwards clock correction.

static void test_update_clock_bad_time_counts_and_does_not_assert()
{
    rocketnet_quiet( true );

    rocketnet_assert_function_t saved_assert = rn_assert_function;
    rocketnet_assert_function( update_clock_assert_hook );
    update_clock_asserts = 0;

    const double last_good = 10.0;
    double raw_time = 10.0;
    double time_offset = 0.0;

    const uint64_t before = rocketnet_api_misuse_count();

    bool clamped = false;
    rocketnet_unit_check( rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 9.0, &clamped ) == last_good );
    rocketnet_unit_check( clamped );

    clamped = false;
    rocketnet_unit_check( rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, (double) NAN, &clamped ) == last_good );
    rocketnet_unit_check( clamped );

    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 2 );
    rocketnet_unit_check( update_clock_asserts == 0 );

    rocketnet_assert_function( saved_assert );
}

REGISTER_UNIT_TEST( test_update_clock_bad_time_counts_and_does_not_assert );
