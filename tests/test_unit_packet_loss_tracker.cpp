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

#include "rocketnet_ack.h"
#include "rocketnet_jitter_tracker.h"
#include "rocketnet_packet_loss_tracker.h"
#include <math.h>

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

// A gap longer than the history ring between two updates is still judged.
// Receive 0..99 and update, then receive 5000..5099 and update. The span the
// second update judges is longer than HISTORY, so its older slots were
// overwritten; the tracker judges it by count. The two updates together must
// report exactly the sequences up to the second update's finish (the highest
// sequence received less SAFETY) that were never received, counted here from
// what the test sent. At BASE the overflow branch returned zero and the stall
// read as no loss.
static void test_packet_loss_tracker_gap_over_history_counts_lost()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    static bool sent[5100];
    memset( sent, 0, sizeof( sent ) );

    for ( uint64_t sequence = 0; sequence < 100; ++sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        sent[sequence] = true;
    }

    const int first = rocketnet_packet_loss_tracker_update( &tracker );

    uint64_t highest = 0;
    for ( uint64_t sequence = 5000; sequence < 5100; ++sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        sent[sequence] = true;
        highest = sequence;
    }

    const int second = rocketnet_packet_loss_tracker_update( &tracker );

    const uint64_t finish = highest - ROCKETNET_PACKET_LOSS_TRACKER_SAFETY;
    int expected_lost = 0;
    for ( uint64_t sequence = 0; sequence <= finish; ++sequence )
    {
        if ( !sent[sequence] )
        {
            expected_lost++;
        }
    }

    rocketnet_unit_check( expected_lost > 0 );
    rocketnet_unit_check( first >= 0 && second >= 0 );
    rocketnet_unit_check( first + second == expected_lost );
}

// A burst longer than the history ring with no update between is all
// received, so the update judging it counts zero lost: counting every
// unjudged sequence as lost would be wrong in the other direction.
static void test_packet_loss_tracker_burst_over_history_not_lost()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    for ( uint64_t sequence = 0; sequence < 3000; ++sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
    }

    rocketnet_unit_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );
}

// A sequence jump beyond any burst a real stream at the assumed MTU can carry
// is bounded by the tracker's documented per-update cap, not saturated to
// INT_MAX: the client adds the count to a lifetime total, so a corrupt or
// renamed sequence must not poison the loss rate. The jump crosses far more
// than the cap; the update reports exactly the cap.
static void test_packet_loss_tracker_overflow_loss_is_bounded()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    rocketnet_packet_loss_tracker_packet_received( &tracker, 0 );
    rocketnet_packet_loss_tracker_update( &tracker );

    rocketnet_packet_loss_tracker_packet_received( &tracker, 0xFFFFFFFFFFFFFFFULL );

    rocketnet_unit_check( rocketnet_packet_loss_tracker_update( &tracker ) == ROCKETNET_PACKET_LOSS_TRACKER_MAX_LOST_PER_UPDATE );
}

// A packet already judged lost cannot replace a newer, received sequence in
// the same ring slot. The missing sequence 50 is counted in the first phase;
// its late arrival changes nothing, so the second phase reports no loss.
static void test_packet_loss_tracker_late_packet_does_not_clobber()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    int first_phase_lost = 0;
    for ( uint64_t sequence = 0; sequence <= 1100; ++sequence )
    {
        if ( sequence != 50 )
        {
            rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        }
        if ( ( sequence + 1 ) % 10 == 0 )
        {
            first_phase_lost += rocketnet_packet_loss_tracker_update( &tracker );
        }
    }
    first_phase_lost += rocketnet_packet_loss_tracker_update( &tracker );
    rocketnet_unit_check( first_phase_lost == 1 );

    rocketnet_packet_loss_tracker_packet_received( &tracker, 50 );

    int second_phase_lost = 0;
    for ( uint64_t sequence = 1101; sequence <= 1200; ++sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        if ( ( sequence - 1100 ) % 10 == 0 )
        {
            second_phase_lost += rocketnet_packet_loss_tracker_update( &tracker );
        }
    }
    second_phase_lost += rocketnet_packet_loss_tracker_update( &tracker );
    rocketnet_unit_check( second_phase_lost == 0 );
}

// A replay-valid older sequence can arrive before the first update. It counts
// as received, but it cannot replace the newer unjudged resident of its slot.
static void test_packet_loss_tracker_late_alias_before_first_update()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    for ( uint64_t wire_sequence = 1; wire_sequence <= 2000; ++wire_sequence )
    {
        if ( wire_sequence != 976 )
        {
            rocketnet_packet_loss_tracker_packet_received( &tracker, wire_sequence - 1 );
        }
    }

    const int alias_slot = int( 2000 % ROCKETNET_PACKET_LOSS_TRACKER_HISTORY );
    rocketnet_unit_check( tracker.received_packets[alias_slot] == 2000 );
    rocketnet_packet_loss_tracker_packet_received( &tracker, 976 - 1 );
    rocketnet_unit_check( tracker.received_packets[alias_slot] == 2000 );

    const int first = rocketnet_packet_loss_tracker_update( &tracker );
    for ( uint64_t wire_sequence = 2001; wire_sequence <= 2030; ++wire_sequence )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, wire_sequence - 1 );
    }
    const int second = rocketnet_packet_loss_tracker_update( &tracker );
    rocketnet_unit_check( first + second == 0 );
}

// A long in-order run has no loss at either side of the exact ring boundary.
// The high-water span, including the safety margin, decides when counting must
// replace a ring scan because newer margin records have reused older slots.
static void test_packet_loss_tracker_in_order_ring_boundary_not_lost()
{
    static const uint64_t ends[] = { 1000, 1024, 1025, 1030, 1053, 1054, 1055, 1060, 1100 };

    for ( uint64_t n : ends )
    {
        rocketnet_packet_loss_tracker_t tracker;
        rocketnet_packet_loss_tracker_reset( &tracker );

        for ( uint64_t wire_sequence = 1; wire_sequence <= n; ++wire_sequence )
        {
            rocketnet_packet_loss_tracker_packet_received( &tracker, wire_sequence - 1 );
        }
        const int first = rocketnet_packet_loss_tracker_update( &tracker );

        for ( uint64_t wire_sequence = n + 1; wire_sequence <= n + 30; ++wire_sequence )
        {
            rocketnet_packet_loss_tracker_packet_received( &tracker, wire_sequence - 1 );
        }
        const int second = rocketnet_packet_loss_tracker_update( &tracker );
        rocketnet_unit_check( first + second == 0 );
    }
}

// A send record is written only for a sequence newer than the one its slot
// holds and within one buffer of the highest sent, at a finite time; anything
// else is refused and changes nothing. rocketnet_ack_endpoint_record_sent_
// classified (sdk/rocketnet_ack.cpp) returns ROCKETNET_ACK_SENT_REFUSED_STALE
// and leaves the endpoint byte-identical when the slot already holds this or a
// newer sequence, when the sequence fell more than one buffer behind the
// highest sent, or when the time is not finite. Sequence 0 is never passed: it
// is an assert, not a refusal.
// a sent record as state: whether its slot was written since the reset and,
// when it was, its fields (never its pad byte, and never the bytes of an
// unwritten slot, which are not state)
struct sent_state_t
{
    bool written;
    struct rocketnet_sent_packet_record_t record;
};

static sent_state_t sent_state( const struct rocketnet_ack_endpoint_t * endpoint, uint64_t slot )
{
    sent_state_t state;
    memset( &state, 0, sizeof( state ) );
    state.written = ( endpoint->sent_written[slot >> 6] >> ( slot & 63 ) ) & 1;
    if ( state.written )
    {
        const struct rocketnet_sent_packet_record_t * record = &endpoint->sent[slot];
        state.record.sequence = record->sequence;
        state.record.packet_id = record->packet_id;
        state.record.aggregate_id = record->aggregate_id;
        state.record.send_time = record->send_time;
        state.record.wire_bytes = record->wire_bytes;
        state.record.acked = record->acked;
        state.record.has_packet_id = record->has_packet_id;
        state.record.was_keep_alive = record->was_keep_alive;
    }
    return state;
}

static bool sent_state_equal( const sent_state_t & a, const sent_state_t & b )
{
    return memcmp( &a, &b, sizeof( a ) ) == 0;
}

static void test_ack_record_sent_refuses_stale_and_nonfinite()
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;

    struct rocketnet_server_acks_t * acks = rocketnet_server_acks_create( NULL, 1 );
    rocketnet_unit_check( acks );
    struct rocketnet_ack_endpoint_t * endpoint = &acks->endpoints[0];

    // (a) the same sequence twice: the first is recorded, the second (the
    // same slot, not newer) is refused and leaves the record and high-water
    // mark untouched.
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
        endpoint, 5, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_RECORDED );

    {
        const uint64_t highest_before = endpoint->highest_sequence;
        const sent_state_t before = sent_state( endpoint, 5 % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            endpoint, 5, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( endpoint->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( endpoint, 5 % N ) ) );
    }

    // (b) a new high is recorded; the older sequence in the same slot is
    // refused.
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
        endpoint, N + 10, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_RECORDED );

    {
        const uint64_t highest_before = endpoint->highest_sequence;
        const sent_state_t before = sent_state( endpoint, 10 % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            endpoint, 10, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( endpoint->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( endpoint, 10 % N ) ) );
    }

    rocketnet_server_acks_destroy( acks );

    // (c) on a fresh endpoint, a sequence more than one buffer behind the
    // highest sent (in a different slot) is refused.
    struct rocketnet_server_acks_t * acks_fresh = rocketnet_server_acks_create( NULL, 1 );
    rocketnet_unit_check( acks_fresh );
    struct rocketnet_ack_endpoint_t * fresh = &acks_fresh->endpoints[0];

    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
        fresh, 2 * N, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_RECORDED );

    {
        const uint64_t highest_before = fresh->highest_sequence;
        const sent_state_t before = sent_state( fresh, ( N - 1 ) % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            fresh, N - 1, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( fresh->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( fresh, ( N - 1 ) % N ) ) );
    }

    // (d) a non-finite time refuses before any state is touched.
    {
        const uint64_t highest_before = fresh->highest_sequence;
        const sent_state_t before = sent_state( fresh, ( 2 * N + 1 ) % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            fresh, 2 * N + 1, 100, 0, 0, 0, (double) NAN ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( fresh->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( fresh, ( 2 * N + 1 ) % N ) ) );
    }
    {
        const uint64_t highest_before = fresh->highest_sequence;
        const sent_state_t before = sent_state( fresh, ( 2 * N + 2 ) % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            fresh, 2 * N + 2, 100, 0, 0, 0, (double) INFINITY ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( fresh->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( fresh, ( 2 * N + 2 ) % N ) ) );
    }

    rocketnet_server_acks_destroy( acks_fresh );
}

// A receive stamp that steps backwards is not a jitter sample. Ten packets on
// a steady 10 ms spacing settle the estimate near zero; then one packet is
// stamped a second earlier and the next two resume the steady spacing from
// there. Before the guard, the backwards difference went through fabs() as a
// large positive deviation and each of the two packets around it added about
// 1.1 s to the 1% running average, so the jitter jumped by about 0.02 s. With
// the guard the backwards packet only re-seeds the tracker, and the estimate
// stays below 1 ms.
static void test_jitter_tracker_backwards_stamp_is_not_a_sample()
{
    rocketnet_jitter_tracker_t tracker;
    rocketnet_jitter_tracker_reset( &tracker );

    const double spacing_seconds = 0.01;
    for ( uint64_t sequence = 0; sequence < 10; ++sequence )
    {
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, 1000.0 + spacing_seconds * (double) sequence );
    }
    rocketnet_unit_check( tracker.jitter < 0.001 );

    const double delta_before_seconds = tracker.last_packet_delta;

    rocketnet_jitter_tracker_packet_received( &tracker, 10, 999.0 );
    rocketnet_unit_check( tracker.jitter < 0.001 );
    rocketnet_unit_check( tracker.last_packet_delta == delta_before_seconds );
    rocketnet_unit_check( tracker.last_packet_processed == 10 );
    rocketnet_unit_check( tracker.last_packet_time == 999.0 );

    rocketnet_jitter_tracker_packet_received( &tracker, 11, 999.0 + spacing_seconds );
    rocketnet_jitter_tracker_packet_received( &tracker, 12, 999.0 + spacing_seconds * 2.0 );
    rocketnet_unit_check( tracker.jitter < 0.001 );
}

REGISTER_UNIT_TEST( test_packet_loss_tracker_window_edge_is_not_lost );
REGISTER_UNIT_TEST( test_packet_loss_tracker_reorder_does_not_recount );
REGISTER_UNIT_TEST( test_packet_loss_tracker_gap_over_history_counts_lost );
REGISTER_UNIT_TEST( test_packet_loss_tracker_burst_over_history_not_lost );
REGISTER_UNIT_TEST( test_packet_loss_tracker_overflow_loss_is_bounded );
REGISTER_UNIT_TEST( test_packet_loss_tracker_late_packet_does_not_clobber );
REGISTER_UNIT_TEST( test_packet_loss_tracker_late_alias_before_first_update );
REGISTER_UNIT_TEST( test_packet_loss_tracker_in_order_ring_boundary_not_lost );
REGISTER_UNIT_TEST( test_ack_record_sent_refuses_stale_and_nonfinite );
REGISTER_UNIT_TEST( test_jitter_tracker_backwards_stamp_is_not_a_sample );
