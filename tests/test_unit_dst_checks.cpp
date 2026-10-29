/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST invariant checks' unit cases (docs/DST.md section 4): each case
    feeds a check a hand-built delivery sequence and asserts the check fires
    or passes. Time is an argument starting at 1000.0; the checks never read
    the wall clock.
*/

#include "test_unit_registry.h"
#include "dst_checks.h"

#include <string.h>

static void rec_send( struct rocketnet_dst_check_state_t * st, int stream, uint32_t seq )
{
    const int session = stream / 72;
    const int channel = ( stream % 72 ) / 8;
    const int type = stream % 8;
    rocketnet_dst_check_sent( st, 1000.0, session, channel, type, seq, 1, 1, 0, 64 );
}

static void test_dst_check_ordered_out_of_order_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 42 );

    double time = 1000.0;
    rocketnet_dst_check_ordered( &st, time, 3, 0 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_ordered( &st, time, 3, 2 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "ordered_in_order" ) == 0 );
}

static void test_dst_check_reliable_duplicate_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 43 );

    double time = 1000.0;
    rec_send( &st, 2, 5 );
    rocketnet_dst_check_reliable_once( &st, time, 2, 5 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_reliable_once( &st, time, 2, 5 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "reliable_once" ) == 0 );
}

static void test_dst_check_reliable_missing_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 44 );

    double time = 1000.0;
    rec_send( &st, 2, 0 );
    rec_send( &st, 2, 1 );
    rec_send( &st, 2, 2 );
    rocketnet_dst_check_reliable_once( &st, time, 2, 0 );
    rocketnet_dst_check_reliable_once( &st, time, 2, 1 );
    rocketnet_dst_check_liveness_delivered( &st, time, 2 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "liveness_delivered" ) == 0 );
}

static void test_dst_check_sequenced_backwards_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 45 );

    double time = 1000.0;
    rocketnet_dst_check_sequenced( &st, time, 4, 10 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_sequenced( &st, time, 4, 9 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "sequenced_forward" ) == 0 );
}

static void test_dst_check_unreliable_duplicate_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 46 );

    double time = 1000.0;
    rocketnet_dst_check_unreliable_once( &st, time, 0, 7 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_unreliable_once( &st, time, 0, 7 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "unreliable_once" ) == 0 );
}

static void test_dst_check_ack_undelivered_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 47 );

    double time = 1000.0;
    rocketnet_dst_check_opened( &st, 2, 0 );
    rocketnet_dst_check_opened( &st, 2, 1 );
    rocketnet_dst_check_opened( &st, 2, 2 );
    rocketnet_dst_check_acked( &st, time, 2, 2 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_acked( &st, time, 2, 3 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "acked_means_opened" ) == 0 );
}

static void test_dst_check_stale_session_payload_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 48 );

    double time = 1000.0;
    rocketnet_dst_check_stale_session( &st, time, 5, 3, 1 );
    rocketnet_dst_check_stale_session_payload( &st, time, 5, 3, 1 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_stale_session_payload( &st, time, 5, 4, 1 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "stale_session_payload" ) == 0 );
}

static void test_dst_check_clean_sequence_passes()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 49 );

    double time = 1000.0;

    rocketnet_dst_check_ordered( &st, time, 3, 0 );
    rocketnet_dst_check_ordered( &st, time, 3, 1 );
    rocketnet_dst_check_ordered( &st, time, 3, 2 );

    rocketnet_dst_check_sequenced( &st, time, 1, 10 );
    rocketnet_dst_check_sequenced( &st, time, 1, 11 );

    rec_send( &st, 2, 5 );
    rec_send( &st, 2, 6 );
    rocketnet_dst_check_reliable_once( &st, time, 2, 5 );
    rocketnet_dst_check_reliable_once( &st, time, 2, 6 );

    rocketnet_dst_check_unreliable_once( &st, time, 0, 7 );

    rocketnet_dst_check_opened( &st, 2, 5 );
    rocketnet_dst_check_opened( &st, 2, 6 );
    rocketnet_dst_check_acked( &st, time, 2, 6 );

    rocketnet_dst_check_stale_session( &st, time, 5, 3, 1 );
    rocketnet_dst_check_stale_session_payload( &st, time, 5, 3, 1 );

    rocketnet_dst_check_liveness_delivered( &st, time, 2 );

    rocketnet_unit_check( st.failed == 0 );
}

REGISTER_UNIT_TEST( test_dst_check_ordered_out_of_order_fails );
REGISTER_UNIT_TEST( test_dst_check_reliable_duplicate_fails );
REGISTER_UNIT_TEST( test_dst_check_reliable_missing_fails );
REGISTER_UNIT_TEST( test_dst_check_sequenced_backwards_fails );
REGISTER_UNIT_TEST( test_dst_check_unreliable_duplicate_fails );
REGISTER_UNIT_TEST( test_dst_check_ack_undelivered_fails );
REGISTER_UNIT_TEST( test_dst_check_stale_session_payload_fails );
REGISTER_UNIT_TEST( test_dst_check_clean_sequence_passes );

static void test_dst_check_whole_message_over_65535_bytes()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 44 );
    rocketnet_dst_check_sent( &st, 1000.0, 0, 0, 2, 5, 1, 1, 0, 70000 );
    rocketnet_dst_check_whole_message( &st, 2, 5, 70000 );
    rocketnet_unit_check( st.failed == 0 );

    static struct rocketnet_dst_check_state_t st2;
    rocketnet_dst_check_init( &st2, 1, 45 );
    rocketnet_dst_check_sent( &st2, 1000.0, 0, 0, 2, 5, 1, 1, 0, 70000 );
    rocketnet_dst_check_whole_message( &st2, 2, 5, 70000 + 65536 );
    rocketnet_unit_check( st2.failed == 1 );
    rocketnet_unit_check( strcmp( st2.check, "whole_message" ) == 0 );
}
REGISTER_UNIT_TEST( test_dst_check_whole_message_over_65535_bytes );

// Types 1 and 4 share one sequenced_write and one sequenced_watermark per
// channel (rn_channels_send_sequenced in sdk/rocketnet_channel.cpp;
// SequencedNewestWins in proofs/tla/RNChannelTypes.tla), so the end-of-run sequenced
// liveness check judges type 4 across both types on the channel. Session 17
// is client 1; channel 8 is the mixed channel.
//
// The messages are sent at step 100 of a run that ends at step 4000, so a type
// 4 of the quiet window is owed: only a type 4 sent in the last steps of the
// run is still on the wire.

static void sequenced_send( struct rocketnet_dst_check_state_t * st, int type, uint32_t seq )
{
    rocketnet_dst_check_sent( st, 1001.0, 17, 8, type, seq, 1, 1, 0, 64 );
}

static void sequenced_deliver( struct rocketnet_dst_check_state_t * st, int type, uint32_t seq )
{
    rocketnet_dst_check_delivery( st, 1001.0, 17, 8, type, seq, 1, 1, 1, 0, 64, 1, 1 );
}

static void sequenced_end( struct rocketnet_dst_check_state_t * st )
{
    int live[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint64_t identity[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    memset( live, 0, sizeof( live ) );
    memset( identity, 0, sizeof( identity ) );
    live[17] = 1;
    identity[17] = 1;
    st->step = 4000;
    rocketnet_dst_check_end( st, 1040.0, live, identity );
}

static void test_dst_check_sequenced_superseded_type4_passes()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 448, 100 );
    sequenced_send( &st, 4, 120 );
    sequenced_deliver( &st, 4, 120 );
    rocketnet_dst_check_settle( &st );

    // the type 4 is sent after the quiet start, a newer type 1 follows on the
    // channel and is delivered, and the type 4 is superseded and dropped

    sequenced_send( &st, 4, 129 );
    sequenced_send( &st, 1, 142 );
    sequenced_deliver( &st, 1, 142 );
    rocketnet_unit_check( st.failed == 0 );
    sequenced_end( &st );
    rocketnet_unit_check( st.failed == 0 );
}
REGISTER_UNIT_TEST( test_dst_check_sequenced_superseded_type4_passes );

static void test_dst_check_sequenced_older_type1_does_not_serve_type4()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 448, 100 );
    sequenced_send( &st, 4, 120 );
    sequenced_deliver( &st, 4, 120 );
    rocketnet_dst_check_settle( &st );

    // only a type 1 sent before the type 4 is delivered: it does not
    // supersede the later type 4, which is still owed

    sequenced_send( &st, 1, 141 );
    sequenced_send( &st, 4, 129 );
    sequenced_deliver( &st, 1, 141 );
    rocketnet_unit_check( st.failed == 0 );
    sequenced_end( &st );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "liveness_delivered" ) == 0 );
}
REGISTER_UNIT_TEST( test_dst_check_sequenced_older_type1_does_not_serve_type4 );

// A later type 4 of the quiet window is newer than the delivery that
// superseded the first one, so the channel owes it: the delivery that
// served the first type 4 does not serve the second.

static void test_dst_check_sequenced_later_type4_raises_the_requirement()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 448, 100 );
    sequenced_send( &st, 4, 120 );
    sequenced_deliver( &st, 4, 120 );
    rocketnet_dst_check_settle( &st );

    // the first type 4 of the quiet window, then a type 1 that supersedes it

    sequenced_send( &st, 4, 129 );
    sequenced_send( &st, 1, 142 );
    sequenced_deliver( &st, 1, 142 );
    rocketnet_unit_check( st.failed == 0 );

    // a second type 4 of the quiet window is newer than that delivery

    sequenced_send( &st, 4, 130 );
    sequenced_end( &st );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "liveness_delivered" ) == 0 );
}
REGISTER_UNIT_TEST( test_dst_check_sequenced_later_type4_raises_the_requirement );
