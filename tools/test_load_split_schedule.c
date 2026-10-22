#include "load_split_schedule.h"

#include <assert.h>
#include <stdint.h>

int main( void )
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
    return 0;
}
