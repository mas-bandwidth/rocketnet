/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST layer-1 core's unit cases (docs/DST.md section 2): determinism of
    the seeded run, seed sensitivity, substream independence, the fail line
    naming the seed and the replay command, and the fault schedules firing
    and replaying. Time is an argument starting
    at 1000.0; the core never reads the wall clock.
*/

#include "test_unit_registry.h"
#include "dst_harness.h"

#include <stdio.h>
#include <string.h>

static struct rocketnet_dst_options_t dst_test_options( uint64_t steps )
{
    struct rocketnet_dst_options_t options;
    memset( &options, 0, sizeof( options ) );
    options.steps = steps;
    options.fault_steps = steps;
    options.shape = ROCKETNET_DST_SHAPE_FULL;
    options.faults = ROCKETNET_DST_FAULT_ALL;
    options.force_fail_step = -1;
    return options;
}

static void test_dst_same_seed_same_trace()
{
    const struct rocketnet_dst_options_t options = dst_test_options( 200 );
    struct rocketnet_dst_result_t a;
    struct rocketnet_dst_result_t b;
    rocketnet_dst_run_seed_with_options( 0x1234567890abcdefULL, &options, &a );
    rocketnet_dst_run_seed_with_options( 0x1234567890abcdefULL, &options, &b );
    rocketnet_unit_check( a.passed );
    rocketnet_unit_check( b.passed );
    rocketnet_unit_check( a.trace_hash == b.trace_hash );
}

static void test_dst_seeds_differ()
{
    const struct rocketnet_dst_options_t options = dst_test_options( 200 );
    struct rocketnet_dst_result_t a;
    struct rocketnet_dst_result_t b;
    rocketnet_dst_run_seed_with_options( 0x1234567890abcdefULL, &options, &a );
    rocketnet_dst_run_seed_with_options( 0x0fedcba987654321ULL, &options, &b );
    rocketnet_unit_check( a.passed );
    rocketnet_unit_check( b.passed );
    rocketnet_unit_check( a.trace_hash != b.trace_hash );
}

static void test_dst_seed_range_uint64_max_terminates()
{
    const uint64_t maximum = ~(uint64_t) 0;
    uint64_t next = maximum;
    rocketnet_unit_check( rocketnet_dst_seed_range_next( maximum - 1, maximum, &next ) );
    rocketnet_unit_check( next == maximum );
    rocketnet_unit_check( !rocketnet_dst_seed_range_next( maximum, maximum, &next ) );
    rocketnet_unit_check( next == maximum );
}

static void test_dst_substreams_independent()
{
    const uint64_t seed = 0x1122334455667788ULL;
    struct rocketnet_dst_substream_t alone;
    struct rocketnet_dst_substream_t after;
    struct rocketnet_dst_substream_t other;
    rocketnet_dst_substream_init( &alone, seed, "traffic.c0" );
    rocketnet_dst_substream_init( &after, seed, "traffic.c0" );
    rocketnet_dst_substream_init( &other, seed, "schedule" );

    uint64_t drawn[8];
    for ( int i = 0; i < 8; i++ )
    {
        drawn[i] = rocketnet_dst_substream_draw( &alone );
    }

    for ( int i = 0; i < 64; i++ )
    {
        rocketnet_dst_substream_draw( &other );
    }

    for ( int i = 0; i < 8; i++ )
    {
        rocketnet_unit_check( rocketnet_dst_substream_draw( &after ) == drawn[i] );
    }
}

static void test_dst_fail_names_seed()
{
    const uint64_t seed = 0xABCDEF0123456789ULL;
    struct rocketnet_dst_options_t options = dst_test_options( 100 );
    options.clients = 2;
    options.faults = ROCKETNET_DST_FAULT_LOSS | ROCKETNET_DST_FAULT_DUP;
    options.force_fail_step = 37;

    struct rocketnet_dst_result_t first;
    struct rocketnet_dst_result_t replay;
    rocketnet_dst_run_seed_with_options( seed, &options, &first );
    rocketnet_dst_run_seed_with_options( seed, &options, &replay );

    char first_line[768];
    char replay_line[768];
    rocketnet_dst_format_failure( &first, &options, first_line, sizeof( first_line ) );
    rocketnet_dst_format_failure( &replay, &options, replay_line, sizeof( replay_line ) );

    char seed_text[32];
    snprintf( seed_text, sizeof( seed_text ), "%llu", (unsigned long long) seed );

    rocketnet_unit_check( !first.passed );
    rocketnet_unit_check( first.step == 37 );
    rocketnet_unit_check( strcmp( first.check, "forced" ) == 0 );
    rocketnet_unit_check( strstr( first_line, "FAIL seed=" ) != NULL );
    rocketnet_unit_check( strstr( first_line, seed_text ) != NULL );
    rocketnet_unit_check( strstr( first_line, "step=37" ) != NULL );
    rocketnet_unit_check( strstr( first_line, "./bin/dst --seeds" ) != NULL );
    rocketnet_unit_check( strstr( first_line, "--faults loss,dup" ) != NULL );
    rocketnet_unit_check( strstr( first_line, "--faults none" ) == NULL );
    rocketnet_unit_check( strcmp( first_line, replay_line ) == 0 );
    rocketnet_unit_check( replay.step == first.step );
}

// The server's kick (a reliable message past its timeout) sends its
// disconnect burst through the link with the message object's index, which
// is the server's slot. Seed 2's clients hold slots out of client order, so
// a burst routed by that index as a DST client index reached a client of
// another session (refused as already received or did not open) while the
// slot's holder never heard it and timed out.

static void test_dst_server_kick_reaches_slot_holder()
{
    struct rocketnet_dst_options_t options = dst_test_options( 2200 );
    options.fault_steps = 200;

    struct rocketnet_dst_result_t result;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 2, &options, &result ) );
    rocketnet_unit_check( result.passed );
}

// The server's maintenance sends a standalone ACK while a slot owes one, as
// the server transport does, and the wire's duplicate fault replays some of
// them at the client: CLIENT_ACK_ALREADY_RECEIVED is explained under that
// fault, as soak_sim explains it. Seed 8 replays server ACKs at the client.

static void test_dst_duplicate_server_ack_explained()
{
    struct rocketnet_dst_options_t options = dst_test_options( 2500 );
    options.fault_steps = 500;
    options.faults = ROCKETNET_DST_FAULT_DUP;

    struct rocketnet_dst_result_t result;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 8, &options, &result ) );
    rocketnet_unit_check( result.passed );
}

// Every fault schedule (docs/DST.md, "Fault schedules") replays: the same
// seed gives the same trace twice, and the schedule's trace differs from the
// run with no schedule, which stays the run it was. Each schedule but the
// seq-wrap pair fires in this run; those two need the process's
// ROCKETNET_DEV_SESSION_SEQUENCE_START, which ./bin/dst sets and the unit
// suite does not, so here they fire only when the seam holds their start.

static void test_dst_schedule_same_seed_same_trace()
{
    struct rocketnet_dst_options_t options = dst_test_options( 900 );
    options.fault_steps = 600;

    struct rocketnet_dst_result_t plain;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 3, &options, &plain ) );
    rocketnet_unit_check( plain.schedule_fired == 0 );
    rocketnet_unit_check( plain.clock_back_seen == 0 );

    for ( int schedule = ROCKETNET_DST_SCHEDULE_NONE + 1; schedule < ROCKETNET_DST_NUM_SCHEDULES; schedule++ )
    {
        options.schedule = schedule;

        struct rocketnet_dst_result_t a;
        struct rocketnet_dst_result_t b;
        rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 3, &options, &a ) );
        rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 3, &options, &b ) );
        rocketnet_unit_check( a.trace_hash == b.trace_hash );
        rocketnet_unit_check( a.schedule_fired == b.schedule_fired );
        rocketnet_unit_check( a.passed == b.passed );
        rocketnet_unit_check( a.step == b.step );

        const uint64_t start = rocketnet_dst_schedule_sequence_start( schedule );
        if ( start == 0 )
        {
            rocketnet_unit_check( a.schedule_fired > 0 );
            rocketnet_unit_check( a.trace_hash != plain.trace_hash );
        }
        else if ( rocketnet_dev_session_sequence_start() != start )
            rocketnet_unit_check( a.schedule_fired == 0 );
    }
}

// clock-back steps one endpoint's clock back by a drawn 10 ms .. 5 s: the
// transport's update is handed a time below the one it had the step before,
// and only that once.

static void test_dst_clock_back_reaches_endpoints()
{
    struct rocketnet_dst_options_t options = dst_test_options( 600 );
    options.fault_steps = 400;
    options.schedule = ROCKETNET_DST_SCHEDULE_CLOCK_BACK;

    struct rocketnet_dst_result_t result;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 5, &options, &result ) );
    rocketnet_unit_check( result.passed );
    rocketnet_unit_check( result.schedule_fired == 1 );
    rocketnet_unit_check( result.clock_back_seen == 1 );

    options.schedule = ROCKETNET_DST_SCHEDULE_NONE;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 5, &options, &result ) );
    rocketnet_unit_check( result.clock_back_seen == 0 );
}

// seq-gap advances one end's send sequence past the replay history
// (ROCKETNET_HISTORY_PACKETS) in one step: on every seed the applied gap is
// larger than the whole history, and the receiver's highest opened sequence
// rises by at least the gap at once, which no run without the schedule does.

static void test_dst_seq_gap_exceeds_history()
{
    struct rocketnet_dst_options_t options = dst_test_options( 600 );
    options.fault_steps = 400;

    struct rocketnet_dst_result_t result;
    for ( uint64_t seed = 1; seed <= 8; seed++ )
    {
        options.schedule = ROCKETNET_DST_SCHEDULE_SEQ_GAP;
        rocketnet_unit_check( rocketnet_dst_run_seed_with_options( seed, &options, &result ) );
        rocketnet_unit_check( result.passed );
        rocketnet_unit_check( result.schedule_fired == 1 );
        rocketnet_unit_check( result.sequence_gap > ROCKETNET_HISTORY_PACKETS );
        rocketnet_unit_check( result.max_sequence_jump >= result.sequence_gap );
        rocketnet_unit_check( result.max_sequence_jump > ROCKETNET_HISTORY_PACKETS );

        options.schedule = ROCKETNET_DST_SCHEDULE_NONE;
        rocketnet_unit_check( rocketnet_dst_run_seed_with_options( seed, &options, &result ) );
        rocketnet_unit_check( result.sequence_gap == 0 );
        rocketnet_unit_check( result.max_sequence_jump <= ROCKETNET_HISTORY_PACKETS );
    }
}

// The burst schedule sends and receives hundreds of packets in one step: the
// drawn client stages its whole drawn batch in one update (the counters are
// apart), and the window's release hands it the wire's whole backlog in one
// step. Seed 3 stages and releases at least two hundred each way; a run
// without the schedule moves none.

static void test_dst_burst_releases_a_batch_in_one_step()
{
    struct rocketnet_dst_options_t options = dst_test_options( 900 );
    options.fault_steps = 600;
    options.schedule = ROCKETNET_DST_SCHEDULE_BURST;

    struct rocketnet_dst_result_t result;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 3, &options, &result ) );
    rocketnet_unit_check( result.passed );
    rocketnet_unit_check( result.schedule_fired == 1 );
    rocketnet_unit_check( result.burst_sent >= 200 );
    rocketnet_unit_check( result.burst_received >= 200 );

    options.schedule = ROCKETNET_DST_SCHEDULE_NONE;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 3, &options, &result ) );
    rocketnet_unit_check( result.burst_sent == 0 );
    rocketnet_unit_check( result.burst_received == 0 );
}

// The dup-storm schedule delivers every packet of its window more than once,
// each copy at its own time, so the receivers take the payload twice. With
// the duplicate fault off, only the storm makes duplicates.

static void test_dst_dup_storm_duplicates_payloads()
{
    struct rocketnet_dst_options_t options = dst_test_options( 600 );
    options.fault_steps = 400;
    options.faults = ROCKETNET_DST_FAULT_LOSS;
    options.schedule = ROCKETNET_DST_SCHEDULE_DUP_STORM;

    struct rocketnet_dst_result_t result;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 3, &options, &result ) );
    rocketnet_unit_check( result.passed );
    rocketnet_unit_check( result.schedule_fired > 0 );
    rocketnet_unit_check( result.duplicate_payloads > 0 );

    options.schedule = ROCKETNET_DST_SCHEDULE_NONE;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 3, &options, &result ) );
    rocketnet_unit_check( result.duplicate_payloads == 0 );
}

// A reliable sequenced send late in the quiet window can wait behind its
// session's outstanding reliable bytes at the send budget, and the run must
// not owe it before that queue drains (ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS).
// Each of these seeds, in the nightly's full shape, sends a type 4 between 30
// and 40 steps before the last step that arrives 32 to 42 steps after it.

static void dst_quiet_window_seed_passes( uint64_t seed )
{
    struct rocketnet_dst_options_t options = dst_test_options( 4000 );
    options.fault_steps = 2000;

    struct rocketnet_dst_result_t result;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( seed, &options, &result ) );
    if ( !result.passed )
        printf( "seed %llu: %s %s\n", (unsigned long long) seed, result.check, result.detail );
    rocketnet_unit_check( result.passed );
}

static void test_dst_budget_queued_sequenced_seed_69363() { dst_quiet_window_seed_passes( 69363 ); }
static void test_dst_budget_queued_sequenced_seed_71434() { dst_quiet_window_seed_passes( 71434 ); }
static void test_dst_budget_queued_sequenced_seed_84959() { dst_quiet_window_seed_passes( 84959 ); }
static void test_dst_budget_queued_sequenced_seed_93289() { dst_quiet_window_seed_passes( 93289 ); }

REGISTER_UNIT_TEST( test_dst_budget_queued_sequenced_seed_69363 );
REGISTER_UNIT_TEST( test_dst_budget_queued_sequenced_seed_71434 );
REGISTER_UNIT_TEST( test_dst_budget_queued_sequenced_seed_84959 );
REGISTER_UNIT_TEST( test_dst_budget_queued_sequenced_seed_93289 );
REGISTER_UNIT_TEST( test_dst_same_seed_same_trace );
REGISTER_UNIT_TEST( test_dst_schedule_same_seed_same_trace );
REGISTER_UNIT_TEST( test_dst_clock_back_reaches_endpoints );
REGISTER_UNIT_TEST( test_dst_seq_gap_exceeds_history );
REGISTER_UNIT_TEST( test_dst_burst_releases_a_batch_in_one_step );
REGISTER_UNIT_TEST( test_dst_dup_storm_duplicates_payloads );
REGISTER_UNIT_TEST( test_dst_seeds_differ );
REGISTER_UNIT_TEST( test_dst_substreams_independent );
REGISTER_UNIT_TEST( test_dst_seed_range_uint64_max_terminates );
REGISTER_UNIT_TEST( test_dst_fail_names_seed );
REGISTER_UNIT_TEST( test_dst_server_kick_reaches_slot_holder );
REGISTER_UNIT_TEST( test_dst_duplicate_server_ack_explained );

static void test_dst_steps_refused_past_int_range()
{
    rocketnet_unit_check( rocketnet_dst_steps_valid( 0 ) == 0 );
    rocketnet_unit_check( rocketnet_dst_steps_valid( 4000 ) == 1 );
    rocketnet_unit_check( rocketnet_dst_steps_valid( ROCKETNET_DST_MAX_STEPS ) == 1 );
    rocketnet_unit_check( rocketnet_dst_steps_valid( 3000000000ULL ) == 0 );

    const struct rocketnet_dst_options_t options = dst_test_options( 3000000000ULL );
    struct rocketnet_dst_result_t result;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 1, &options, &result ) == 0 );
}
REGISTER_UNIT_TEST( test_dst_steps_refused_past_int_range );

// Reversed witnesses: each plants one defect in what the checks observe, and
// the same seed that passes clean must fail naming the check that guards it.
// ORDERED_SWAP hands the checks two ordered deliveries of one stream swapped;
// FOREIGN_ACK has a client's ack name a packet id the server issued and the
// client did not, which one id set shared by every endpoint would accept.

static void test_dst_witness_ordered_swap_fails()
{
    struct rocketnet_dst_options_t options = dst_test_options( 600 );

    struct rocketnet_dst_result_t clean;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 1, &options, &clean ) );
    rocketnet_unit_check( clean.passed );

    options.witness = ROCKETNET_DST_WITNESS_ORDERED_SWAP;
    struct rocketnet_dst_result_t planted;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 1, &options, &planted ) );
    rocketnet_unit_check( !planted.passed );
    rocketnet_unit_check( strcmp( planted.check, "ordered_in_order" ) == 0 );
}

static void test_dst_witness_foreign_ack_fails()
{
    struct rocketnet_dst_options_t options = dst_test_options( 600 );

    struct rocketnet_dst_result_t clean;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 1, &options, &clean ) );
    rocketnet_unit_check( clean.passed );

    options.witness = ROCKETNET_DST_WITNESS_FOREIGN_ACK;
    struct rocketnet_dst_result_t planted;
    rocketnet_unit_check( rocketnet_dst_run_seed_with_options( 1, &options, &planted ) );
    rocketnet_unit_check( !planted.passed );
    rocketnet_unit_check( strcmp( planted.check, "acked_means_opened" ) == 0 );
}

REGISTER_UNIT_TEST( test_dst_witness_ordered_swap_fails );
REGISTER_UNIT_TEST( test_dst_witness_foreign_ack_fails );
