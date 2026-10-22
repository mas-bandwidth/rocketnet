/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The slots machine (sdk/rocketnet_slots.h) against docs/CHANNELS.md 10.2 and
// tla/RNSlots.tla Observe and Admit. The machine is driven alone: no object,
// no transport, no time, no allocation.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_slots.h"

#define check rocketnet_unit_check

// RNSlots Observe and Admit: a free slot discards, connect resets and binds,
// the next update is unchanged, a reassigned slot resets to the new
// connection, and a disconnect drops the binding.
static void test_slots_observe_and_admit()
{
    struct rn_slots_t slots;
    rn_slots_init( &slots, 4 );

    // a free slot: no binding, and no entry carries the id 0
    check( rn_slots_admit( &slots, 0, 0 ) == RN_SLOTS_DISCARD );
    check( rn_slots_admit( &slots, 0, 100 ) == RN_SLOTS_DISCARD );
    check( rn_slots_observe( &slots, 0, 0 ) == RN_SLOTS_NONE );
    check( slots.changed[0] == 0 );

    // CONNECTED: 0 to a nonzero id is RESET, and the slot is changed
    check( rn_slots_observe( &slots, 0, 100 ) == RN_SLOTS_RESET );
    check( slots.connection_id[0] == 100 );
    check( slots.changed[0] == 1 );
    check( rn_slots_admit( &slots, 0, 100 ) == RN_SLOTS_APPLY );
    check( rn_slots_admit( &slots, 0, 101 ) == RN_SLOTS_DISCARD );
    check( rn_slots_admit( &slots, 0, 0 ) == RN_SLOTS_DISCARD );

    // the next update sees the same id: NONE, and the changed bit clears
    check( rn_slots_observe( &slots, 0, 100 ) == RN_SLOTS_NONE );
    check( slots.changed[0] == 0 );
    check( rn_slots_admit( &slots, 0, 100 ) == RN_SLOTS_APPLY );
    check( rn_slots_admit( &slots, 0, 200 ) == RN_SLOTS_DISCARD );

    // a reassigned slot: nonzero to a different nonzero id is RESET
    check( rn_slots_observe( &slots, 0, 200 ) == RN_SLOTS_RESET );
    check( slots.connection_id[0] == 200 );
    check( slots.changed[0] == 1 );
    check( rn_slots_admit( &slots, 0, 100 ) == RN_SLOTS_DISCARD );
    check( rn_slots_admit( &slots, 0, 200 ) == RN_SLOTS_APPLY );
    check( rn_slots_observe( &slots, 0, 200 ) == RN_SLOTS_NONE );

    // DISCONNECTED: a nonzero id to 0 is DROP, and every entry is discarded
    check( rn_slots_observe( &slots, 0, 0 ) == RN_SLOTS_DROP );
    check( slots.connection_id[0] == 0 );
    check( slots.changed[0] == 1 );
    check( rn_slots_admit( &slots, 0, 200 ) == RN_SLOTS_DISCARD );
    check( rn_slots_observe( &slots, 0, 0 ) == RN_SLOTS_NONE );
    check( slots.changed[0] == 0 );

    // an id outside the machine changes nothing
    check( rn_slots_admit( &slots, 4, 100 ) == RN_SLOTS_DISCARD );
    check( rn_slots_admit( &slots, -1, 100 ) == RN_SLOTS_DISCARD );
    check( rn_slots_observe( &slots, 4, 100 ) == RN_SLOTS_NONE );
    check( rn_slots_observe( &slots, -1, 100 ) == RN_SLOTS_NONE );
}

// CHANNELS.md 10.2 same-batch churn: A sends, A leaves, B claims the slot in
// one batch. Observe sees the final owner B; A's entries carry A's id and are
// discarded, and B's own entries carry B's id and are applied. A free slot
// discards everything.
static void test_slots_same_batch_churn_discards_the_old_connection()
{
    struct rn_slots_t slots;
    rn_slots_init( &slots, 2 );

    // the update before the churn: A is bound to slot 0
    check( rn_slots_observe( &slots, 0, 100 ) == RN_SLOTS_RESET );
    check( rn_slots_observe( &slots, 0, 100 ) == RN_SLOTS_NONE );
    check( rn_slots_admit( &slots, 0, 100 ) == RN_SLOTS_APPLY );

    // one batch: A sends, A leaves, B claims. Observe binds B; the entries
    // keep the ids of the connections they were received for.
    check( rn_slots_observe( &slots, 0, 200 ) == RN_SLOTS_RESET );
    check( slots.changed[0] == 1 );
    check( rn_slots_admit( &slots, 0, 100 ) == RN_SLOTS_DISCARD );   // A's entries
    check( rn_slots_admit( &slots, 0, 200 ) == RN_SLOTS_APPLY );     // B's entries

    // slot 1 is free: no tag identifies it, so every entry is discarded
    check( rn_slots_observe( &slots, 1, 0 ) == RN_SLOTS_NONE );
    check( rn_slots_admit( &slots, 1, 0 ) == RN_SLOTS_DISCARD );
    check( rn_slots_admit( &slots, 1, 100 ) == RN_SLOTS_DISCARD );
    check( rn_slots_admit( &slots, 1, 200 ) == RN_SLOTS_DISCARD );

    // B stays bound across the next update
    check( rn_slots_observe( &slots, 0, 200 ) == RN_SLOTS_NONE );
    check( rn_slots_admit( &slots, 0, 200 ) == RN_SLOTS_APPLY );
    check( rn_slots_admit( &slots, 0, 100 ) == RN_SLOTS_DISCARD );
}

REGISTER_UNIT_TEST( test_slots_observe_and_admit );
REGISTER_UNIT_TEST( test_slots_same_batch_churn_discards_the_old_connection );
