/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

/* The split default profile's one-second offer phase (load_split_schedule.h).
   The space profile's frame phase is in tests/test_unit_load_split_schedule.cpp.
   Build and run beside the harness, from the repository root:

       cc -std=c11 -I. tools/test_load_split_schedule.c -o test_load_split_schedule && ./test_load_split_schedule
*/

#include "tools/load_split_schedule.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_split_offer_due_one_second_phase( void )
{
    double next_due = 0.0;
    uint64_t missed = 0;

    /* A stale outer-loop sample would be 0.9 here. The actual post-update
     * offer sample is 1.9: skip due slot 0 and make exactly one offer. */
    assert(split_offer_due(1.9, 3.0, &next_due, &missed));
    assert(missed == 1 && next_due == 2.0);
    assert(!split_offer_due(1.91, 3.0, &next_due, &missed));
    assert(missed == 1 && next_due == 2.0);
    assert(split_offer_due(2.0, 3.0, &next_due, &missed));
    assert(missed == 1 && next_due == 3.0);

    /* A frame that started before the traffic boundary but finished after
     * it cannot offer, even when a due slot remains. */
    next_due = 2.0;
    assert(!split_offer_due(3.1, 3.0, &next_due, &missed));
    assert(missed == 1 && next_due == 2.0);

    next_due = 0.0;
    missed = 0;
    assert(split_offer_due(5.2, 10.0, &next_due, &missed));
    assert(missed == 5 && next_due == 6.0);
    assert(!split_offer_due(5.21, 10.0, &next_due, &missed));
}

int main( void )
{
    test_split_offer_due_one_second_phase();
    printf( "test_split_offer_due_one_second_phase\n" );
    return 0;
}
