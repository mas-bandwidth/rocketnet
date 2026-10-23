/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The load_split traffic schedule (tools/load_split_schedule.h): the space
// profile's 60 Hz frame phase, its mod-3 delta stagger and the offers each
// role must make per profile. Time is an argument; it starts at 1000.0.

#include "test_unit_registry.h"

#include "../tools/load_split_schedule.h"

#include <stdint.h>

#define check rocketnet_unit_check

#define SCHEDULE_START 1000.0

static double schedule_due( uint64_t frame )
{
    return SCHEDULE_START + (double) frame / (double) SPLIT_SPACE_FRAME_HZ;
}

static void test_load_split_frame_due_runs_each_frame_once()
{
    const double end = SCHEDULE_START + 1.0;
    uint64_t next_frame = 0, missed = 0, frame = 99;
    check( !split_frame_due( SCHEDULE_START - 0.001, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    check( split_frame_due( SCHEDULE_START, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    check( frame == 0 && next_frame == 1 && missed == 0 );
    // a second visit inside frame 0 runs nothing
    check( !split_frame_due( SCHEDULE_START + 0.010, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    check( next_frame == 1 && missed == 0 );
    // a wake exactly at a frame's due time runs that frame
    for ( uint64_t s = 1; s < 60; s++ )
    {
        check( split_frame_due( schedule_due( s ), SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
        check( frame == s && next_frame == s + 1 );
    }
    check( missed == 0 );
    // every frame of the window has run
    check( !split_frame_due( end - 0.0001, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
}

static void test_load_split_frame_due_counts_skipped_frames()
{
    const double end = SCHEDULE_START + 1.0;
    uint64_t next_frame = 0, missed = 0, frame = 0;
    check( split_frame_due( SCHEDULE_START, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    // a stall to the middle of frame 5 runs frame 5 and counts 1, 2, 3 and 4
    check( split_frame_due( schedule_due( 5 ) + 0.008, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    check( frame == 5 && next_frame == 6 && missed == 4 );
    // a visit late inside one frame is not a skip
    check( split_frame_due( schedule_due( 6 ) + 0.016, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    check( frame == 6 && missed == 4 );
}

static void test_load_split_frame_due_stops_at_traffic_end()
{
    const double end = SCHEDULE_START + 1.0;
    uint64_t next_frame = 59, missed = 0, frame = 0;
    // a frame that starts after the boundary never runs, even if one is owed
    check( !split_frame_due( end, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    check( next_frame == 59 && missed == 0 );
    // the last frame runs up to the boundary and is never passed
    check( split_frame_due( end - 0.000001, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, 60, &next_frame, &missed, &frame ) );
    check( frame == 59 && next_frame == 60 && missed == 0 );
}

static void test_load_split_frame_next_time_follows_the_phase()
{
    check( split_frame_next_time( SCHEDULE_START - 1.0, SCHEDULE_START, SPLIT_SPACE_FRAME_HZ ) == SCHEDULE_START );
    const double first = split_frame_next_time( SCHEDULE_START, SCHEDULE_START, SPLIT_SPACE_FRAME_HZ );
    check( first > SCHEDULE_START + 0.0166 && first < SCHEDULE_START + 0.0167 );
    const double third = split_frame_next_time( SCHEDULE_START + 0.020, SCHEDULE_START, SPLIT_SPACE_FRAME_HZ );
    check( third > SCHEDULE_START + 0.0333 && third < SCHEDULE_START + 0.0334 );
}

// A server woken at each next frame time, a little late, runs every frame of
// a 60-second window once and misses none.
static void test_load_split_frame_loop_runs_every_frame_of_a_window()
{
    const int duration = 60;
    const uint64_t frames = split_space_frames( duration );
    const double end = SCHEDULE_START + (double) duration;
    uint64_t next_frame = 0, missed = 0, frame = 0, ran = 0;
    double now = SCHEDULE_START;
    while ( now < end + 3.0 )
    {
        if ( split_frame_due( now, SCHEDULE_START, end, SPLIT_SPACE_FRAME_HZ, frames, &next_frame, &missed, &frame ) )
        {
            check( frame == ran );
            ran++;
        }
        now = split_frame_next_time( now, SCHEDULE_START, SPLIT_SPACE_FRAME_HZ ) + 0.0005 * (double) ( ran % 7 );
    }
    check( ran == frames && frames == 3600 && missed == 0 );
}

static void test_load_split_space_delta_stagger_is_client_mod_3()
{
    for ( uint64_t frame = 0; frame < 9; frame++ )
        for ( uint32_t client = 0; client < 9; client++ )
            check( split_space_delta_due( frame, client ) == ( frame % 3 == client % 3 ) );
    // every client takes exactly one of any three consecutive frames
    for ( uint32_t client = 0; client < 1000; client++ )
        for ( uint64_t frame = 0; frame < 9; frame++ )
        {
            const int taken = split_space_delta_due( frame, client ) + split_space_delta_due( frame + 1, client ) +
                              split_space_delta_due( frame + 2, client );
            check( taken == 1 );
        }
    // a frame carries a delta for a third of 1000 clients, never all of them
    for ( uint64_t frame = 0; frame < 3; frame++ )
    {
        int clients = 0;
        for ( uint32_t client = 0; client < 1000; client++ )
            clients += split_space_delta_due( frame, client );
        check( clients == ( frame == 0 ? 334 : 333 ) );
    }
}

static void test_load_split_space_delta_offers_match_the_stagger()
{
    check( split_space_delta_offers( 0, 3600 ) == 1200 );
    check( split_space_delta_offers( 1, 3600 ) == 1200 );
    check( split_space_delta_offers( 2, 3600 ) == 1200 );
    for ( uint64_t frames = 0; frames < 10; frames++ )
        for ( uint32_t client = 0; client < 6; client++ )
        {
            uint64_t counted = 0;
            for ( uint64_t frame = 0; frame < frames; frame++ )
                counted += (uint64_t) split_space_delta_due( frame, client );
            check( split_space_delta_offers( client, frames ) == counted );
        }
}

static void test_load_split_expected_offers_per_profile()
{
    // default: one offer a second per client, either role
    check( split_expected_offers( SPLIT_PROFILE_DEFAULT, 1, 0, 1000, 60 ) == 60000 );
    check( split_expected_offers( SPLIT_PROFILE_DEFAULT, 0, 250, 250, 60 ) == 15000 );
    // space shard: one input a frame per client
    check( split_expected_offers( SPLIT_PROFILE_SPACE, 0, 250, 250, 60 ) == 250 * 3600 );
    // space server: a player message a frame and a delta every third frame
    check( split_expected_offers( SPLIT_PROFILE_SPACE, 1, 0, 1000, 60 ) == 1000 * 3600 + 1000 * 1200 );
    // a shard whose first client is not a multiple of 3 sums its own clients
    uint64_t sum = 0;
    for ( int i = 0; i < 5; i++ )
        sum += split_space_frames( 1 ) + split_space_delta_offers( (uint32_t) ( 7 + i ), split_space_frames( 1 ) );
    check( split_expected_offers( SPLIT_PROFILE_SPACE, 1, 7, 5, 1 ) == sum );
}

REGISTER_UNIT_TEST( test_load_split_frame_due_runs_each_frame_once );
REGISTER_UNIT_TEST( test_load_split_frame_due_counts_skipped_frames );
REGISTER_UNIT_TEST( test_load_split_frame_due_stops_at_traffic_end );
REGISTER_UNIT_TEST( test_load_split_frame_next_time_follows_the_phase );
REGISTER_UNIT_TEST( test_load_split_frame_loop_runs_every_frame_of_a_window );
REGISTER_UNIT_TEST( test_load_split_space_delta_stagger_is_client_mod_3 );
REGISTER_UNIT_TEST( test_load_split_space_delta_offers_match_the_stagger );
REGISTER_UNIT_TEST( test_load_split_expected_offers_per_profile );
