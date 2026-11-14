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

// A forward step on a clock that never went back hands out the caller's
// time, bit for bit, and reports no clamp, whatever the flag held before.

static void test_update_clock_forward_step_clears_the_clamp()
{
    double raw_time = 10.0;
    double time_offset = 0.0;

    bool clamped = true;
    rocketnet_unit_check( rocketnet_update_clock_advance( 10.0, &raw_time, &time_offset, 10.25, &clamped ) == 10.25 );
    rocketnet_unit_check( !clamped );
    rocketnet_unit_check( raw_time == 10.25 );
    rocketnet_unit_check( time_offset == 0.0 );
}

REGISTER_UNIT_TEST( test_update_clock_forward_step_clears_the_clamp );

// After a backwards step the clamped time advances by exactly each caller
// step: each step is measured from the caller's previous time, never from
// the time the clock fell back to. A caller's time exactly equal to the last
// good time has caught up: the clock is the caller's clock again, not the
// last good time plus a step.

static void test_update_clock_catch_up_steps_from_the_callers_last_time()
{
    rocketnet_quiet( true );

    double last_good = 10.0;
    double raw_time = 10.0;
    double time_offset = 0.0;
    bool clamped = false;

    // back from 10 to 5: the clamped time stands at 10

    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 5.0, &clamped );
    rocketnet_unit_check( last_good == 10.0 );
    rocketnet_unit_check( clamped );
    rocketnet_unit_check( raw_time == 5.0 );
    rocketnet_unit_check( time_offset == 5.0 );

    // 5 to 6 to 7: the clamped time goes 11, 12, each one step of 1

    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 6.0, &clamped );
    rocketnet_unit_check( last_good == 11.0 );
    rocketnet_unit_check( !clamped );
    rocketnet_unit_check( raw_time == 6.0 );

    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 7.0, &clamped );
    rocketnet_unit_check( last_good == 12.0 );
    rocketnet_unit_check( raw_time == 7.0 );

    // a backwards step measured from the caller's last time, 7, not from 5

    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 6.5, &clamped );
    rocketnet_unit_check( last_good == 12.0 );
    rocketnet_unit_check( clamped );
    rocketnet_unit_check( raw_time == 6.5 );
    rocketnet_unit_check( time_offset == 5.5 );

    // a caller's time exactly at the last good time is the caller's time

    double caught_raw = 4.0;
    double caught_offset = 6.0;
    clamped = true;
    rocketnet_unit_check( rocketnet_update_clock_advance( 10.0, &caught_raw, &caught_offset, 10.0, &clamped ) == 10.0 );
    rocketnet_unit_check( !clamped );
    rocketnet_unit_check( caught_raw == 10.0 );
    rocketnet_unit_check( caught_offset == 6.0 );

    // and just past it, the caller's time again

    rocketnet_unit_check( rocketnet_update_clock_advance( 10.0, &caught_raw, &caught_offset, 10.5, &clamped ) == 10.5 );
}

REGISTER_UNIT_TEST( test_update_clock_catch_up_steps_from_the_callers_last_time );

// Every negative time is refused before the clock reads it, the way a
// non-finite one is: it changes neither the raw time nor the offset and
// counts one misuse, the first and each one after it. A later negative above
// an earlier one is not a forward step from it, so the clock does not start
// on negatives, and the first small valid time starts it at that time.

static void update_clock_negatives_then_start( double first, double second )
{
    double last_good = 0.0;
    double raw_time = 0.0;
    double time_offset = 0.0;

    const uint64_t before = rocketnet_api_misuse_count();

    bool clamped = false;
    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, first, &clamped );
    rocketnet_unit_check( last_good == 0.0 );
    rocketnet_unit_check( clamped );
    rocketnet_unit_check( raw_time == 0.0 );
    rocketnet_unit_check( time_offset == 0.0 );
    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 1 );

    clamped = false;
    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, second, &clamped );
    rocketnet_unit_check( last_good == 0.0 );
    rocketnet_unit_check( clamped );
    rocketnet_unit_check( raw_time == 0.0 );
    rocketnet_unit_check( time_offset == 0.0 );
    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 2 );

    // the first small valid time: the clock starts there, not at the
    // distance the negatives covered added to it

    clamped = true;
    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 0.5, &clamped );
    rocketnet_unit_check( last_good == 0.5 );
    rocketnet_unit_check( !clamped );
    rocketnet_unit_check( raw_time == 0.5 );
    rocketnet_unit_check( time_offset == 0.0 );
    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 2 );
}

static void test_update_clock_refuses_every_negative_time()
{
    rocketnet_quiet( true );

    // repeated, then increasing, negatives

    update_clock_negatives_then_start( -5.0, -5.0 );
    update_clock_negatives_then_start( -5.0, -4.0 );

    // on a running clock a negative is refused the same way, and the valid
    // backwards step after it still records its distance from the caller's
    // last good time, not from the negative

    double last_good = 10.0;
    double raw_time = 10.0;
    double time_offset = 0.0;
    bool clamped = false;

    const uint64_t before = rocketnet_api_misuse_count();

    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, -1.0, &clamped );
    rocketnet_unit_check( last_good == 10.0 );
    rocketnet_unit_check( clamped );
    rocketnet_unit_check( raw_time == 10.0 );
    rocketnet_unit_check( time_offset == 0.0 );

    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 8.0, &clamped );
    rocketnet_unit_check( last_good == 10.0 );
    rocketnet_unit_check( clamped );
    rocketnet_unit_check( raw_time == 8.0 );
    rocketnet_unit_check( time_offset == 2.0 );

    last_good = rocketnet_update_clock_advance( last_good, &raw_time, &time_offset, 9.0, &clamped );
    rocketnet_unit_check( last_good == 11.0 );
    rocketnet_unit_check( !clamped );

    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 2 );
}

REGISTER_UNIT_TEST( test_update_clock_refuses_every_negative_time );
