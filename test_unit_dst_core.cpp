/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST layer-1 core's unit cases: determinism of
    the seeded run, seed sensitivity, substream independence, and the fail
    line naming the seed and the replay command. Time is an argument starting
    at 1000.0; the core never reads the wall clock.
*/

#include "test_unit_registry.h"
#include "dst_harness.h"

#include <string.h>

static struct rocketnet_dst_options_t dst_test_options( uint64_t steps )
{
    struct rocketnet_dst_options_t options;
    memset( &options, 0, sizeof(options) );
    options.steps = steps;
    options.fault_steps = steps;
    options.shape = ROCKETNET_DST_SHAPE_FULL;
    options.faults = ROCKETNET_DST_FAULT_ALL;
    options.force_fail_step = -1;
    return options;
}

static void dst_same_seed_same_trace()
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

static void dst_seeds_differ()
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

static void dst_seed_range_uint64_max_terminates()
{
    const uint64_t maximum = ~(uint64_t) 0;
    uint64_t next = maximum;
    rocketnet_unit_check( rocketnet_dst_seed_range_next( maximum - 1, maximum, &next ) );
    rocketnet_unit_check( next == maximum );
    rocketnet_unit_check( !rocketnet_dst_seed_range_next( maximum, maximum, &next ) );
    rocketnet_unit_check( next == maximum );
}

static void dst_substreams_independent()
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
        drawn[i] = rocketnet_dst_substream_draw( &alone );

    for ( int i = 0; i < 64; i++ )
        rocketnet_dst_substream_draw( &other );

    for ( int i = 0; i < 8; i++ )
        rocketnet_unit_check( rocketnet_dst_substream_draw( &after ) == drawn[i] );
}

static void dst_fail_names_seed()
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
    rocketnet_dst_format_failure( &first, &options, first_line, sizeof(first_line) );
    rocketnet_dst_format_failure( &replay, &options, replay_line, sizeof(replay_line) );

    char seed_text[32];
    snprintf( seed_text, sizeof(seed_text), "%llu", (unsigned long long) seed );

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

REGISTER_UNIT_TEST( dst_same_seed_same_trace );
REGISTER_UNIT_TEST( dst_seeds_differ );
REGISTER_UNIT_TEST( dst_substreams_independent );
REGISTER_UNIT_TEST( dst_seed_range_uint64_max_terminates );
REGISTER_UNIT_TEST( dst_fail_names_seed );
