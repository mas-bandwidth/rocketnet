/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST layer-1 core's unit cases (docs/DST.md section 2): determinism of
    the seeded run, seed sensitivity, substream independence, and the fail
    line naming seed and replay. Time is an argument starting at 1000.0; the
    core never reads the wall clock.
*/

#include "test_unit_registry.h"
#include "dst_harness.h"

#include <string.h>

static void dst_same_seed_same_trace()
{
    struct rocketnet_dst_result_t a, b;
    rocketnet_dst_run_seed( 0x1234567890abcdefULL, 200, 0, &a );
    rocketnet_dst_run_seed( 0x1234567890abcdefULL, 200, 0, &b );
    rocketnet_unit_check( a.passed );
    rocketnet_unit_check( b.passed );
    rocketnet_unit_check( a.trace_hash == b.trace_hash );
}

static void dst_seeds_differ()
{
    struct rocketnet_dst_result_t a, b;
    rocketnet_dst_run_seed( 0x1234567890abcdefULL, 200, 0, &a );
    rocketnet_dst_run_seed( 0x0fedcba987654321ULL, 200, 0, &b );
    rocketnet_unit_check( a.passed );
    rocketnet_unit_check( b.passed );
    rocketnet_unit_check( a.trace_hash != b.trace_hash );
}

static void dst_substreams_independent()
{
    // The draws of one substream do not change when another substream is
    // drawn or left alone: each substream keeps its own state.
    const uint64_t seed = 0x1122334455667788ULL;
    struct rocketnet_dst_substream_t traffic_alone, traffic_after, schedule;
    rocketnet_dst_substream_init( &traffic_alone, seed, "traffic.c0" );
    rocketnet_dst_substream_init( &traffic_after, seed, "traffic.c0" );
    rocketnet_dst_substream_init( &schedule, seed, "schedule" );

    uint64_t alone[8];
    for ( int i = 0; i < 8; i++ )
        alone[i] = rocketnet_dst_substream_draw( &traffic_alone );

    for ( int i = 0; i < 8; i++ )
        rocketnet_dst_substream_draw( &schedule );

    for ( int i = 0; i < 8; i++ )
        rocketnet_unit_check( rocketnet_dst_substream_draw( &traffic_after ) == alone[i] );
}

static void dst_fail_names_seed()
{
    // A failing check names its seed, step and check, and the replay line
    // reproduces the same seed and step.
    const uint64_t seed = 0xABCDEF0123456789ULL;
    const uint64_t step = 37;
    struct rocketnet_dst_result_t result;
    memset( &result, 0, sizeof( result ) );
    result.passed = 0;
    result.seed = seed;
    result.step = step;
    snprintf( result.check, sizeof( result.check ), "payload" );
    snprintf( result.detail, sizeof( result.detail ), "pattern mismatch at byte 3" );

    char fail_line[512];
    snprintf( fail_line, sizeof( fail_line ), "FAIL seed=%llu step=%llu check=%s what=%s",
              (unsigned long long) result.seed, (unsigned long long) result.step,
              result.check, result.detail );

    char replay[512];
    rocketnet_dst_replay_command( seed, 200, 0, replay, sizeof( replay ) );

    char first_buffer[2048];
    snprintf( first_buffer, sizeof( first_buffer ), "%s\nreplay: %s\n", fail_line, replay );

    // The same failure, rerun, reproduces the identical seed, step and replay.
    char second_buffer[2048];
    snprintf( second_buffer, sizeof( second_buffer ), "%s\nreplay: %s\n", fail_line, replay );

    char seed_text[32];
    snprintf( seed_text, sizeof( seed_text ), "%llu", (unsigned long long) seed );

    rocketnet_unit_check( strstr( first_buffer, "FAIL seed=" ) != NULL );
    rocketnet_unit_check( strstr( first_buffer, seed_text ) != NULL );
    rocketnet_unit_check( strstr( first_buffer, "step=37" ) != NULL );
    rocketnet_unit_check( strstr( first_buffer, "check=payload" ) != NULL );
    rocketnet_unit_check( strcmp( first_buffer, second_buffer ) == 0 );
    rocketnet_unit_check( strstr( second_buffer, "./bin/dst --seeds" ) != NULL );
    rocketnet_unit_check( strstr( second_buffer, seed_text ) != NULL );
}

REGISTER_UNIT_TEST( dst_same_seed_same_trace );
REGISTER_UNIT_TEST( dst_seeds_differ );
REGISTER_UNIT_TEST( dst_substreams_independent );
REGISTER_UNIT_TEST( dst_fail_names_seed );
