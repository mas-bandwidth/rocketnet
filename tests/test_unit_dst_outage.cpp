/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The outage scenarios' per-commit share (docs/DST.md, "Outage scenarios";
// the world is tests/dst_outage.h). The owner's three tests: client to
// server, server to client and both directions dark for T - 1 seconds while
// both ends keep sending on every reliability mode, with a large fragmented
// message in flight when the outage starts; the connection must survive and
// deliver everything it accepted, exactly once and in order. Their
// complement: an outage past T ends both sides by the timeout and for no
// other reason. An outage at each stage of the handshake, and the opted-in
// reliable timeout ending the connection as v1.0.0 did. The wide grid (T up
// to 60 and beyond, round trips up to 900 ms) is `./bin/dst --outage grid`
// in the nightly DST run. The loopback transport and a test clock from
// 1000.0: no socket, no thread, no real time.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "loopback_hub.h"
#include "dst_outage.h"

#include <stdio.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

static dst_outage_result_t dst_outage_unit_run( const char * label, const dst_outage_options_t * options )
{
    dst_outage_result_t result;
    check( dst_outage_run( options, &result ) == 1 );
    dst_outage_print( label, options, &result );
    check( result.passed );
    return result;
}

// T = 10, D = 9 in each direction at a 150 ms round trip and 2% loss.

static void test_dst_outage_t_minus_one_each_direction()
{
    rocketnet_quiet( 1 );
    const int directions[3] = { DST_OUTAGE_C2S, DST_OUTAGE_S2C, DST_OUTAGE_BOTH };
    for ( int d = 0; d < 3; d++ )
    {
        dst_outage_options_t options;
        dst_outage_default_options( &options, rocketnet_test_loopback_hub() );
        options.dark = directions[d];
        options.loss_percent = 2;
        dst_outage_unit_run( "t-1", &options );
    }
}

// The shortest T the token takes in practice, at a 600 ms round trip: a 1 s
// outage at T = 2 leaves a margin of one second less two keep-alives, an
// update and the round trip's change.

static void test_dst_outage_t2_at_600_ms()
{
    rocketnet_quiet( 1 );
    const int directions[3] = { DST_OUTAGE_C2S, DST_OUTAGE_S2C, DST_OUTAGE_BOTH };
    for ( int d = 0; d < 3; d++ )
    {
        dst_outage_options_t options;
        dst_outage_default_options( &options, rocketnet_test_loopback_hub() );
        options.timeout_seconds = 2;
        options.outage_seconds = 1.0;
        options.rtt_ms = 600;
        options.dark = directions[d];
        dst_outage_unit_run( "t2", &options );
    }
}

// The complement: an outage of T + 2 s ends the deaf side by its timeout at
// T, and the other side by its own timeout or the deaf side's notice.

static void test_dst_outage_past_t_ends_by_the_timeout()
{
    rocketnet_quiet( 1 );
    const int directions[3] = { DST_OUTAGE_C2S, DST_OUTAGE_S2C, DST_OUTAGE_BOTH };
    for ( int d = 0; d < 3; d++ )
    {
        dst_outage_options_t options;
        dst_outage_default_options( &options, rocketnet_test_loopback_hub() );
        options.timeout_seconds = 2;
        options.outage_seconds = 4.0;
        options.dark = directions[d];
        options.expect = DST_OUTAGE_EXPECT_TIMEOUT;
        dst_outage_unit_run( "past-t", &options );
    }
}

// Opted in, the reliable timeout ends the connection when the oldest
// reliable message has waited that long since its first send, outage or
// not, as in v1.0.0.

static void test_dst_outage_opted_in_reliable_timeout()
{
    rocketnet_quiet( 1 );
    dst_outage_options_t options;
    dst_outage_default_options( &options, rocketnet_test_loopback_hub() );
    // the smallest value the config accepts: four initial resend delays
    options.reliable_timeout_ms = 8000;
    options.expect = DST_OUTAGE_EXPECT_RELIABLE_TIMEOUT;
    dst_outage_unit_run( "opted-in", &options );
}

// An outage at each stage of the handshake, T = 10, D = 9, a 30 s token,
// 150 ms round trip. States 1 and 2, the true X1 window, the server's and the
// client's CONNECTED: survives. State 3 with the client's direction dark:
// the server never sees the response (its handshake window is 5 s), and the
// attempt fails with CONNECTION_RESPONSE_TIMED_OUT at T; with only the
// server's direction dark it survives.

static void test_dst_outage_at_each_handshake_stage()
{
    rocketnet_quiet( 1 );
    struct
    {
        int start;
        int dark;
        int expect;
    } rows[] = {
        { DST_OUTAGE_START_STATE_1, DST_OUTAGE_BOTH, DST_OUTAGE_EXPECT_SURVIVE },
        { DST_OUTAGE_START_STATE_2, DST_OUTAGE_BOTH, DST_OUTAGE_EXPECT_SURVIVE },
        { DST_OUTAGE_START_STATE_3, DST_OUTAGE_S2C, DST_OUTAGE_EXPECT_SURVIVE },
        { DST_OUTAGE_START_STATE_3, DST_OUTAGE_C2S, DST_OUTAGE_EXPECT_RESPONSE_TIMEOUT },
        { DST_OUTAGE_START_TRUE_X1, DST_OUTAGE_BOTH, DST_OUTAGE_EXPECT_SURVIVE },
        { DST_OUTAGE_START_SERVER_CONNECTED, DST_OUTAGE_BOTH, DST_OUTAGE_EXPECT_SURVIVE },
        { DST_OUTAGE_START_CLIENT_CONNECTED, DST_OUTAGE_BOTH, DST_OUTAGE_EXPECT_SURVIVE },
    };
    for ( size_t i = 0; i < sizeof( rows ) / sizeof( rows[0] ); i++ )
    {
        dst_outage_options_t options;
        dst_outage_default_options( &options, rocketnet_test_loopback_hub() );
        options.token_lifetime_seconds = 30;
        options.start = rows[i].start;
        options.dark = rows[i].dark;
        options.expect = rows[i].expect;
        options.large_bytes = 0;
        const dst_outage_result_t result = dst_outage_unit_run( "handshake", &options );

        // the state 3 row ends as it claims: the response did not escape
        // before the outage (the harness would judge survival instead), and
        // the attempt failed with CONNECTION_RESPONSE_TIMED_OUT. The client
        // sends its response at its send step, after the harness has gone
        // dark; a library that sent it inside the update that entered state
        // 3 (which tests/dst_outage.h allows) moves this row to survival
        if ( rows[i].expect == DST_OUTAGE_EXPECT_RESPONSE_TIMEOUT )
        {
            check( !result.response_escaped );
            check( result.client_ended );
            check( result.client_state == ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT );
            check( !result.connected );
        }
    }
}

// The receipt oracle on hand-built lists (no library): the client's
// unreliable receipt send has serial 7 and its reliable one serial 8,
// delivered. Each list is what one update surfaced.

static void dst_outage_receipt_world( dst_outage_world_t * w, dst_outage_result_t * result )
{
    memset( result, 0, sizeof( *result ) );
    w->result = result;
    w->now = 1000.0;
    dst_outage_message_t m;
    memset( &m, 0, sizeof( m ) );
    m.sent = 1000.0;
    m.delivered = -1.0;
    m.bytes = DST_OUTAGE_SMALL_BYTES;
    m.mode = 5;
    m.channel = 10;
    w->messages.push_back( m );
    m.mode = 6;
    m.channel = 12;
    m.delivered = 1000.0;
    w->messages.push_back( m );
    w->serials[0][7] = 0;
    w->serials[0][8] = 1;
}

static int dst_outage_receipt_lists_pass( const rocketnet_receipt_t * first, int first_count, const rocketnet_receipt_t * second,
                                          int second_count )
{
    dst_outage_world_t * w = new dst_outage_world_t();
    dst_outage_result_t result;
    dst_outage_receipt_world( w, &result );
    dst_outage_receipts( w, 0, first, first_count );
    dst_outage_receipts( w, 0, second, second_count );
    delete w;
    return result.failure[0] == 0 && result.receipts_bad == 0;
}

static void test_dst_outage_receipt_oracle()
{
    const rocketnet_receipt_t acked[2] = { { 7, ROCKETNET_RECEIPT_STATUS_ACKED }, { 8, ROCKETNET_RECEIPT_STATUS_ACKED } };
    const rocketnet_receipt_t loss[1] = { { 7, ROCKETNET_RECEIPT_STATUS_LOSS } };
    const rocketnet_receipt_t foreign[1] = { { 9, ROCKETNET_RECEIPT_STATUS_ACKED } };
    const rocketnet_receipt_t none[1] = { { 7, ROCKETNET_RECEIPT_STATUS_NONE } };
    const rocketnet_receipt_t again[1] = { { 8, ROCKETNET_RECEIPT_STATUS_ACKED } };

    // clean: one outcome each, in one update or over two
    check( dst_outage_receipt_lists_pass( acked, 2, NULL, 0 ) );
    check( dst_outage_receipt_lists_pass( loss, 1, again, 1 ) );

    // a serial this side never issued for a receipt-mode send
    check( !dst_outage_receipt_lists_pass( foreign, 1, NULL, 0 ) );

    // a status that is neither ACKED nor LOSS
    check( !dst_outage_receipt_lists_pass( none, 1, NULL, 0 ) );

    // the same outcome again in a later update's list
    check( !dst_outage_receipt_lists_pass( acked, 2, again, 1 ) );
}

REGISTER_UNIT_TEST( test_dst_outage_receipt_oracle );
REGISTER_UNIT_TEST( test_dst_outage_t_minus_one_each_direction );
REGISTER_UNIT_TEST( test_dst_outage_t2_at_600_ms );
REGISTER_UNIT_TEST( test_dst_outage_past_t_ends_by_the_timeout );
REGISTER_UNIT_TEST( test_dst_outage_opted_in_reliable_timeout );
REGISTER_UNIT_TEST( test_dst_outage_at_each_handshake_stage );
UNIT_TEST_SKIPPED_UNLESS( test_dst_outage_at_each_handshake_stage, ROCKETNET_MAX_CHANNELS >= 16 ); // runs at 16 channels, not at 8

// the survivable outages send the large message on DST_OUTAGE_LARGE_CHANNEL (tests/dst_outage.h)
UNIT_TEST_SKIPPED_UNLESS( test_dst_outage_t_minus_one_each_direction, ROCKETNET_MAX_CHANNELS > DST_OUTAGE_LARGE_CHANNEL );
UNIT_TEST_SKIPPED_UNLESS( test_dst_outage_t2_at_600_ms, ROCKETNET_MAX_CHANNELS > DST_OUTAGE_LARGE_CHANNEL );

#endif // #if ROCKETNET_DEV
