/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_packet_loss_tracker.cpp -- the packet loss tracker counts each
// sequence once. Two defect fixes land here (see docs/roadmap.sexp issue-190):
//
//   * the update guard used `finish - start > HISTORY`, so a scan whose length
//     is exactly HISTORY was not guarded and walked HISTORY + 1 sequences over
//     HISTORY ring slots, double-counting one received packet as lost;
//   * the most-recent store was unconditional, so an older reordered packet
//     rewound the high-water mark and a later update re-walked an already
//     judged range and counted its gaps again.
//
// Each test drives the public functions (the receiver stores sequence + 1, so
// a tracker's internal values are one past the public sequence fed to it) and
// asserts the update count is zero, meaning no received packet is judged lost.

#include "test_unit_registry.h"

#include "rocketnet_packet_loss_tracker.h"

// The update scan reaches exactly `finish - start == HISTORY` after HISTORY +
// SAFETY + 1 consecutive packets: the receiver stores sequence + 1, so the
// most-recent value is HISTORY + SAFETY + 1, finish is that minus SAFETY (=
// HISTORY + 1) and start is 1, so finish - start == HISTORY. At the guard, a
// run of that length is all received; the count must be zero.
static void test_packet_loss_tracker_window_edge_is_not_lost()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    const uint64_t run = ROCKETNET_PACKET_LOSS_TRACKER_HISTORY + ROCKETNET_PACKET_LOSS_TRACKER_SAFETY + 1;
    for ( uint64_t sequence = 0; sequence < run; ++sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
    }

    rocketnet_unit_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );
}

// A reordered older packet must not rewound the high-water mark. Deliver
// 0..N-1 in order and update (the whole judged run is received, so the count
// is zero). Then deliver one older in-window sequence: it is still received
// (its ring slot is stamped) but it is a hole, not a new high-water mark, so
// it must not move the most-recent high-water mark backwards. More packets
// push the mark forward again, and the next update must count zero: the older
// delivery must not make the tracker re-walk the already judged range and
// count its members again. At BASE the unconditional store rewinds the mark
// (the tracker's last-processed point drops below the previously judged
// range), and the next update re-walks that judged range and returns a
// positive count; at head the mark stays put and the update returns zero.
static void test_packet_loss_tracker_reorder_does_not_recount()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    const uint64_t N = 950;
    for ( uint64_t sequence = 0; sequence < N; ++sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
    }

    const int first = rocketnet_packet_loss_tracker_update( &tracker );
    rocketnet_unit_check( first == 0 );

    const uint64_t older = 32;
    rocketnet_packet_loss_tracker_packet_received( &tracker, older );

    // an update while the mark is rewound moves last-packet-processed below
    // the already judged range: at BASE the unconditional store dropped the
    // mark here, at head it stayed put. This update itself returns zero in
    // both (its scan range is empty); the recount shows up on the update
    // after the mark moves forward again.
    rocketnet_packet_loss_tracker_update( &tracker );

    for ( uint64_t sequence = N; sequence < N + 82; ++sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
    }

    rocketnet_unit_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );
}

REGISTER_UNIT_TEST( test_packet_loss_tracker_window_edge_is_not_lost );
REGISTER_UNIT_TEST( test_packet_loss_tracker_reorder_does_not_recount );
