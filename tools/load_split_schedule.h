#ifndef ROCKETNET_LOAD_SPLIT_SCHEDULE_H
#define ROCKETNET_LOAD_SPLIT_SCHEDULE_H

#include <stdint.h>

/* Keep an absolute one-second phase. One eligible visit offers at most one
 * message; every whole due slot missed before that visit is counted. */
static inline int split_offer_due( double offer_now, double traffic_end,
                                   double * next_due, uint64_t * missed_due_slots )
{
    if ( offer_now >= traffic_end || offer_now < *next_due ) return 0;
    const uint64_t skipped = (uint64_t) (offer_now - *next_due);
    *missed_due_slots += skipped;
    *next_due += (double) skipped + 1.0;
    return 1;
}

#endif
