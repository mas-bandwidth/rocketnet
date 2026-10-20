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
