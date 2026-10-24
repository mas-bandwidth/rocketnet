/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The load_split offer schedule (tools/load_split_schedule.h): an absolute
// phase of one offer per period, at most one offer per eligible visit, and every whole due
// slot missed before that visit counted. Time is the caller's.

#include "test_unit_registry.h"

#include "../tools/load_split_schedule.h"

#include <stdint.h>

#define check rocketnet_unit_check

static void test_load_split_schedule_offers_from_the_post_update_sample()
{
    double next_due = 0.0;
    uint64_t missed = 0;

    // a stale outer-loop sample would be 0.9 here. the actual post-update
    // offer sample is 1.9: skip due slot 0 and make exactly one offer

    check( split_offer_due( 1.9, 3.0, 1.0, &next_due, &missed ) );
    check( missed == 1 && next_due == 2.0 );
    check( !split_offer_due( 1.91, 3.0, 1.0, &next_due, &missed ) );
    check( missed == 1 && next_due == 2.0 );
    check( split_offer_due( 2.0, 3.0, 1.0, &next_due, &missed ) );
    check( missed == 1 && next_due == 3.0 );
}

static void test_load_split_schedule_no_offer_past_the_traffic_end()
{
    // a frame that started before the traffic boundary but finished after it
    // cannot offer, even when a due slot remains

    double next_due = 2.0;
    uint64_t missed = 1;
    check( !split_offer_due( 3.1, 3.0, 1.0, &next_due, &missed ) );
    check( missed == 1 && next_due == 2.0 );
}

static void test_load_split_schedule_counts_every_missed_slot()
{
    double next_due = 0.0;
    uint64_t missed = 0;
    check( split_offer_due( 5.2, 10.0, 1.0, &next_due, &missed ) );
    check( missed == 5 && next_due == 6.0 );
    check( !split_offer_due( 5.21, 10.0, 1.0, &next_due, &missed ) );
}

static void test_load_split_schedule_full_load_keeps_the_period_phase()
{
    // full load: one offer per 125 ms period. A visit 250 ms after its due
    // slot offers once and counts the two whole slots it skipped; the next
    // slot stays on the absolute phase

    double next_due = 1.0;
    uint64_t missed = 0;
    check( split_offer_due( 1.25, 10.0, 0.125, &next_due, &missed ) );
    check( missed == 2 && next_due == 1.375 );
    check( !split_offer_due( 1.3, 10.0, 0.125, &next_due, &missed ) );
    check( split_offer_due( 1.375, 10.0, 0.125, &next_due, &missed ) );
    check( missed == 2 && next_due == 1.5 );
}

REGISTER_UNIT_TEST( test_load_split_schedule_offers_from_the_post_update_sample );
REGISTER_UNIT_TEST( test_load_split_schedule_no_offer_past_the_traffic_end );
REGISTER_UNIT_TEST( test_load_split_schedule_counts_every_missed_slot );
REGISTER_UNIT_TEST( test_load_split_schedule_full_load_keeps_the_period_phase );
