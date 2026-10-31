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

// One delivery of stream ( session, channel, type ) in the live harness's
// form: generation 1, identity 1, the session's expected generation and
// incarnation 1.

static void rec_sent( struct rocketnet_dst_check_state_t * st, int session, int channel, int type, uint32_t seq )
{
    rocketnet_dst_check_sent( st, 1000.0, session, channel, type, seq, 1, 1, 0, 64 );
}

static void rec_deliver( struct rocketnet_dst_check_state_t * st, int session, int channel, int type, uint32_t seq )
{
    rocketnet_dst_check_delivery( st, 1000.0, session, channel, type, seq, 1, 1, 1, 0, 64, 1, 1 );
}

static void rec_end( struct rocketnet_dst_check_state_t * st, int session )
{
    int live[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint64_t identity[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    memset( live, 0, sizeof( live ) );
    memset( identity, 0, sizeof( identity ) );
    live[session] = 1;
    identity[session] = 1;
    rocketnet_dst_check_end( st, 1040.0, live, identity );
}

static void test_dst_check_ordered_out_of_order_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 42 );

    rec_sent( &st, 0, 0, 3, 0 );
    rec_sent( &st, 0, 0, 3, 1 );
    rec_sent( &st, 0, 0, 3, 2 );
    rec_deliver( &st, 0, 0, 3, 0 );
    rocketnet_unit_check( st.failed == 0 );
    rec_deliver( &st, 0, 0, 3, 2 );
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

// The end-of-run liveness row: a reliable message sent after the quiet start,
// more than the in-flight slack before the last send, is owed.

static void test_dst_check_reliable_missing_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 44 );

    for ( uint32_t seq = 0; seq <= 30; seq++ )
    {
        rec_sent( &st, 0, 0, 2, seq );
        if ( seq != 3 )
            rec_deliver( &st, 0, 0, 2, seq );
    }
    rocketnet_unit_check( st.failed == 0 );
    rec_end( &st, 0 );
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

// acked_means_opened: an ack names a packet id the acked endpoint issued
// (the server is endpoint 0, client i endpoint i + 1), and names it once.

static void test_dst_check_ack_never_issued_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 47 );

    rocketnet_dst_check_issued( &st, 1, 1 );
    rocketnet_dst_check_issued( &st, 1, 2 );
    rocketnet_dst_check_ack_id( &st, 1, 2 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_ack_id( &st, 1, 3 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "acked_means_opened" ) == 0 );
}

// Every endpoint numbers its packets from 1: id 1 issued by the server is not
// a packet client 0 issued.

static void test_dst_check_ack_of_another_endpoints_id_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 50 );

    rocketnet_dst_check_issued( &st, 0, 1 );
    rocketnet_dst_check_ack_id( &st, 1, 1 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "acked_means_opened" ) == 0 );
}

static void test_dst_check_ack_twice_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 51 );

    rocketnet_dst_check_issued( &st, 0, 1 );
    rocketnet_dst_check_ack_id( &st, 0, 1 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_ack_id( &st, 0, 1 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "acked_means_opened" ) == 0 );
}

// An aborted packet's id is skipped: it was never issued.

static void test_dst_check_ack_of_a_skipped_id_fails()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 52 );

    rocketnet_dst_check_issued( &st, 0, 1 );
    rocketnet_dst_check_issued( &st, 0, 3 );
    rocketnet_dst_check_ack_id( &st, 0, 3 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_ack_id( &st, 0, 2 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "acked_means_opened" ) == 0 );
}

// A client's new session restarts its packet ids: the old session's ids are
// not the new one's.

static void test_dst_check_ack_after_endpoint_reset()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 53 );

    for ( uint64_t id = 1; id <= 5; id++ )
        rocketnet_dst_check_issued( &st, 2, id );
    rocketnet_dst_check_endpoint_reset( &st, 2 );
    rocketnet_dst_check_issued( &st, 2, 1 );
    rocketnet_dst_check_ack_id( &st, 2, 1 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_ack_id( &st, 2, 5 );
    rocketnet_unit_check( st.failed == 1 );
    rocketnet_unit_check( strcmp( st.check, "acked_means_opened" ) == 0 );
}

// The ledger holds LEDGER_PACKETS ids: one an ack names inside it is
// answered exactly, one older is refused rather than aliased.

static void test_dst_check_ack_ledger_wraps()
{
    static struct rocketnet_dst_check_state_t st;
    rocketnet_dst_check_init( &st, 1, 54 );

    const uint64_t last = 3 * ROCKETNET_DST_CHECK_LEDGER_PACKETS;
    for ( uint64_t id = 1; id <= last; id += 2 )
        rocketnet_dst_check_issued( &st, 0, id );
    rocketnet_dst_check_ack_id( &st, 0, last );
    rocketnet_dst_check_ack_id( &st, 0, last - ROCKETNET_DST_CHECK_LEDGER_PACKETS + 1 );
    rocketnet_unit_check( st.failed == 0 );
    rocketnet_dst_check_ack_id( &st, 0, last - 1 );
    rocketnet_unit_check( st.failed == 1 );

    static struct rocketnet_dst_check_state_t old;
    rocketnet_dst_check_init( &old, 1, 55 );
    for ( uint64_t id = 1; id <= last; id += 2 )
        rocketnet_dst_check_issued( &old, 0, id );
    rocketnet_dst_check_ack_id( &old, 0, last - ROCKETNET_DST_CHECK_LEDGER_PACKETS - 1 );
    rocketnet_unit_check( old.failed == 1 );
    rocketnet_unit_check( strcmp( old.check, "acked_means_opened" ) == 0 );
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

    for ( uint32_t seq = 0; seq <= 30; seq++ )
    {
        rec_sent( &st, 0, 0, 3, seq );
        rec_deliver( &st, 0, 0, 3, seq );
        rec_sent( &st, 0, 0, 2, seq );
        rec_deliver( &st, 0, 0, 2, seq );
    }

    rocketnet_dst_check_sequenced( &st, time, 1, 10 );
    rocketnet_dst_check_sequenced( &st, time, 1, 11 );

    rocketnet_dst_check_unreliable_once( &st, time, 0, 7 );

    rocketnet_dst_check_issued( &st, 0, 1 );
    rocketnet_dst_check_issued( &st, 1, 1 );
    rocketnet_dst_check_ack_id( &st, 0, 1 );
    rocketnet_dst_check_ack_id( &st, 1, 1 );

    rocketnet_dst_check_stale_session( &st, time, 5, 3, 1 );
    rocketnet_dst_check_stale_session_payload( &st, time, 5, 3, 1 );

    rec_end( &st, 0 );

    rocketnet_unit_check( st.failed == 0 );
}

REGISTER_UNIT_TEST( test_dst_check_ordered_out_of_order_fails );
REGISTER_UNIT_TEST( test_dst_check_reliable_duplicate_fails );
REGISTER_UNIT_TEST( test_dst_check_reliable_missing_fails );
REGISTER_UNIT_TEST( test_dst_check_sequenced_backwards_fails );
REGISTER_UNIT_TEST( test_dst_check_unreliable_duplicate_fails );
REGISTER_UNIT_TEST( test_dst_check_ack_never_issued_fails );
REGISTER_UNIT_TEST( test_dst_check_ack_of_another_endpoints_id_fails );
REGISTER_UNIT_TEST( test_dst_check_ack_twice_fails );
REGISTER_UNIT_TEST( test_dst_check_ack_of_a_skipped_id_fails );
REGISTER_UNIT_TEST( test_dst_check_ack_after_endpoint_reset );
REGISTER_UNIT_TEST( test_dst_check_ack_ledger_wraps );
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
