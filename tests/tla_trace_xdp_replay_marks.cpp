/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the XDP replay marks buffer's layout
// (proofs/tla/RNXdpReplayMarksLayout.tla, proofs/tla/TraceRNXdpReplayMarksLayout.tla,
// README-TraceCheck.md): a seeded walk drives the code of
// sdk/rocketnet_xdp_replay_marks.h through updates, each a forget pass, a
// mark pass and a commit, and writes the model's variables after every step.
//
// The buffer is bound with capacity M over arrays of M + 1 entries: the
// entry at index M is the model's cell past the end, so a write out of
// bounds lands in memory the walk owns and shows in the trace. Every entry
// starts as a sentinel key and value that the record names NONE, a cell
// nothing has written.
//
// The operations and the records they write, one record per call:
//   forget          rocketnet_xdp_replay_marks_forget          Forget(k, ok)
//   mark            rocketnet_xdp_replay_marks_mark            Mark(k, v, ok)
//   reset_keeping   the commit: the kernel's answer, chosen by the walk,
//                   handed to rocketnet_xdp_replay_marks_reset_keeping
//                                                              ResetKeeping(fs, as, n)
// ok is what the call returned, fs the forgets the kernel refused, as the
// marks it applied, n the refusals reset_keeping returned. The model's
// Commit, CompactForgetStep and CompactMarkStep are that one call's inside,
// which the walk cannot see; ResetKeeping is the same call as one step. The
// ghosts absF, absM and absK are the code's own pending lists and
// kept_mark_count after each call, so the model's contract update at a
// commit is checked against the code's compaction.
//
// The plant (ROCKETNET_XDP_REPLAY_MARKS_PLANT in the header) tests the
// forget list's room one late, the reversed witness
// MCRNXdpReplayMarksLayoutBrokenFullOffByOne: a new key is written at index
// M. Its effect is a forget that leaves forget_count past the capacity; the
// walk reports it at that record. No socket, no clock.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_XDP_REPLAY_MARKS_PLANT 1
#include "../sdk/rocketnet_server_socket_linux.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "tla_trace_record.h"

#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_limits.h"
#include "../sdk/rocketnet_xdp_replay_marks.h"

namespace
{
    const uint32_t MARKS_M = 3;             // the buffer's capacity: M in the model
    const int MARKS_KEYS = 5;               // more keys than M, so the buffer fills and refuses
    const int MARKS_VALUES = 3;             // high-water values 0..2
    const uint8_t MARKS_SENTINEL_FAMILY = 0xEE;
    const uint32_t MARKS_SENTINEL_VALUE = 0xFFFFFFFFu;

    struct marks_walk_t
    {
        rocketnet_xdp_replay_marks_t marks;
        rocketnet_server_xdp_replay_key_t forgets[MARKS_M + 1];
        rocketnet_server_xdp_replay_key_t mark_keys[MARKS_M + 1];
        rocketnet_server_xdp_replay_value_t mark_values[MARKS_M + 1];
        bool forget_failed[MARKS_M + 1];
        bool mark_applied[MARKS_M + 1];
        rocketnet_address_t address[MARKS_KEYS];
        rocketnet_server_xdp_replay_key_t key[MARKS_KEYS];

        // the walk's own state: the model's phase, doneF and doneM. Between
        // calls the model's li, lk, failed and applied are reset_keeping's
        // loop state at rest: 0, 0, {} and {}
        const char * phase;
        bool done_forget[MARKS_KEYS];
        bool done_mark[MARKS_KEYS];
    };

    std::string marks_key_name( const marks_walk_t & walk, const rocketnet_server_xdp_replay_key_t & key )
    {
        for ( int i = 0; i < MARKS_KEYS; i++ )
        {
            if ( memcmp( &walk.key[i], &key, sizeof( key ) ) == 0 )
            {
                return tla_string( "k" + tla_int( i + 1 ) );
            }
        }
        if ( key.family != MARKS_SENTINEL_FAMILY )
        {
            fprintf( stderr, "tla_trace: xdp-replay-marks: a key the walk never made\n" );
            tla_value_errors()++;
        }
        return "NONE";
    }

    std::string marks_value( uint32_t value )
    {
        return value == MARKS_SENTINEL_VALUE ? "NONE" : tla_int( value );
    }

    std::string marks_set( const std::vector<int> & items )
    {
        std::string text = "{";
        for ( size_t i = 0; i < items.size(); i++ )
        {
            text += ( i > 0 ? ", " : "" ) + tla_int( items[i] );
        }
        return text + "}";
    }

    std::string marks_done( const bool * done )
    {
        std::string text = "{";
        bool first = true;
        for ( int i = 0; i < MARKS_KEYS; i++ )
        {
            if ( done[i] )
            {
                text += ( first ? "" : ", " ) + tla_string( "k" + tla_int( i + 1 ) );
                first = false;
            }
        }
        return text + "}";
    }

    // the model's variables; the ghosts are the code's own lists
    tla_record_t marks_state( const marks_walk_t & walk )
    {
        const rocketnet_xdp_replay_marks_t & marks = walk.marks;
        std::vector<std::string> cells;
        std::vector<std::string> forgets;
        std::vector<std::string> mkeys;
        std::vector<std::string> mvals;
        for ( uint32_t i = 0; i <= MARKS_M; i++ )
        {
            cells.push_back( tla_int( i ) );
            forgets.push_back( marks_key_name( walk, walk.forgets[i] ) );
            mkeys.push_back( marks_key_name( walk, walk.mark_keys[i] ) );
            mvals.push_back( marks_value( walk.mark_values[i].high_water ) );
        }

        // the cell past the end is written only out of bounds
        const bool overflow = walk.forgets[MARKS_M].family != MARKS_SENTINEL_FAMILY || walk.mark_keys[MARKS_M].family != MARKS_SENTINEL_FAMILY ||
                              walk.mark_values[MARKS_M].high_water != MARKS_SENTINEL_VALUE;

        std::vector<std::string> abs_forgets;
        for ( uint32_t i = 0; i < marks.forget_count && i <= MARKS_M; i++ )
        {
            abs_forgets.push_back( marks_key_name( walk, marks.forgets[i] ) );
        }
        std::vector<std::string> abs_marks;
        for ( uint32_t i = 0; i < marks.mark_count && i <= MARKS_M; i++ )
        {
            abs_marks.push_back( "[k |-> " + marks_key_name( walk, marks.mark_keys[i] ) + ", v |-> " + marks_value( marks.mark_values[i].high_water ) + "]" );
        }

        tla_record_t state;
        state.add( "forgets", tla_function( cells, forgets ) );
        state.add( "fc", tla_int( marks.forget_count ) );
        state.add( "kf", tla_int( marks.kept_forget_count ) );
        state.add( "mkeys", tla_function( cells, mkeys ) );
        state.add( "mvals", tla_function( cells, mvals ) );
        state.add( "mc", tla_int( marks.mark_count ) );
        state.add( "kc", tla_int( marks.kept_mark_count ) );
        state.add( "refused", tla_int( marks.refused ) );
        state.add( "phase", tla_string( walk.phase ) );
        state.add( "li", tla_int( 0 ) );
        state.add( "lk", tla_int( 0 ) );
        state.add( "failed", marks_set( std::vector<int>() ) );
        state.add( "applied", marks_set( std::vector<int>() ) );
        state.add( "doneF", marks_done( walk.done_forget ) );
        state.add( "doneM", marks_done( walk.done_mark ) );
        state.add( "absF", tla_sequence( abs_forgets ) );
        state.add( "absM", tla_sequence( abs_marks ) );
        state.add( "absK", tla_int( marks.kept_mark_count ) );
        state.add( "overflow", tla_bool( overflow ) );
        return state;
    }

    std::string marks_key_arg( int k )
    {
        return tla_string( "k" + tla_int( k + 1 ) );
    }
}

static int trace_xdp_replay_marks( uint64_t seed, int steps, const char * out )
{
    static marks_walk_t walk;
    memset( &walk.marks, 0, sizeof( walk.marks ) );
    for ( uint32_t i = 0; i <= MARKS_M; i++ )
    {
        memset( &walk.forgets[i], 0, sizeof( walk.forgets[i] ) );
        walk.forgets[i].family = MARKS_SENTINEL_FAMILY;
        walk.mark_keys[i] = walk.forgets[i];
        walk.mark_values[i].high_water = MARKS_SENTINEL_VALUE;
        walk.mark_values[i].pad = 0;
        walk.forget_failed[i] = false;
        walk.mark_applied[i] = false;
    }
    for ( int i = 0; i < MARKS_KEYS; i++ )
    {
        rocketnet_address_t * address = &walk.address[i];
        memset( address, 0, sizeof( *address ) );
        address->type = ROCKETNET_ADDRESS_IPV4;
        address->data.ipv4[0] = 198;
        address->data.ipv4[1] = 51;
        address->data.ipv4[2] = 100;
        address->data.ipv4[3] = 7;
        address->port = (uint16_t) ( 40000 + i );
        rocketnet_xdp_replay_key_from_address( address, &walk.key[i] );
        walk.done_forget[i] = false;
        walk.done_mark[i] = false;
    }
    walk.phase = "forget";

    rocketnet_xdp_replay_marks_bind( &walk.marks, walk.forgets, walk.mark_keys, walk.mark_values, MARKS_M );

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", "<<>>", marks_state( walk ) ) );
    while ( (int) records.size() < steps + 1 )
    {
        std::vector<int> can_forget;
        std::vector<int> can_mark;
        for ( int i = 0; i < MARKS_KEYS; i++ )
        {
            if ( strcmp( walk.phase, "forget" ) == 0 && !walk.done_forget[i] )
            {
                can_forget.push_back( i );
            }
            if ( !walk.done_mark[i] )
            {
                can_mark.push_back( i );
            }
        }

        const int choice = random.below( 8 );
        if ( choice < 6 && !can_forget.empty() )
        {
            const int k = can_forget[(size_t) random.below( (int) can_forget.size() )];
            const bool queued = rocketnet_xdp_replay_marks_forget( &walk.marks, &walk.address[k] );
            walk.done_forget[k] = true;
            report.act( "Forget" );
            records.push_back( tla_step( "forget", "Forget", tla_sequence( { marks_key_arg( k ), tla_bool( queued ) } ), marks_state( walk ) ) );
            // the planted effect: a forget the code took past the capacity
            if ( queued && walk.marks.forget_count > walk.marks.capacity )
            {
                report.planted( (int) records.size() );
            }
        }
        else if ( choice < 7 && !can_mark.empty() )
        {
            const int k = can_mark[(size_t) random.below( (int) can_mark.size() )];
            const uint32_t value = (uint32_t) random.below( MARKS_VALUES );
            const bool marked = rocketnet_xdp_replay_marks_mark( &walk.marks, &walk.address[k], value );
            walk.done_mark[k] = true;
            walk.phase = "mark";
            report.act( "Mark" );
            records.push_back( tla_step( "mark", "Mark", tla_sequence( { marks_key_arg( k ), tla_int( value ), tla_bool( marked ) } ), marks_state( walk ) ) );
        }
        else
        {
            // the kernel's answer: each forget refused one time in two, each
            // mark applied two times in three
            std::vector<int> failed;
            std::vector<int> applied;
            for ( uint32_t i = 0; i <= MARKS_M; i++ )
            {
                walk.forget_failed[i] = false;
                walk.mark_applied[i] = false;
            }
            for ( uint32_t i = 0; i < walk.marks.forget_count; i++ )
            {
                walk.forget_failed[i] = random.below( 2 ) == 0;
                if ( walk.forget_failed[i] )
                {
                    failed.push_back( (int) i );
                }
            }
            for ( uint32_t j = 0; j < walk.marks.mark_count; j++ )
            {
                walk.mark_applied[j] = random.below( 3 ) != 0;
                if ( walk.mark_applied[j] )
                {
                    applied.push_back( (int) j );
                }
            }

            const uint32_t refusals = rocketnet_xdp_replay_marks_reset_keeping( &walk.marks, walk.forget_failed, walk.mark_applied );

            // the call's end opens the next update
            walk.phase = "forget";
            for ( int i = 0; i < MARKS_KEYS; i++ )
            {
                walk.done_forget[i] = false;
                walk.done_mark[i] = false;
            }
            report.act( "ResetKeeping" );
            records.push_back( tla_step( "reset_keeping", "ResetKeeping", tla_sequence( { marks_set( failed ), marks_set( applied ), tla_int( refusals ) } ), marks_state( walk ) ) );
        }
    }

    report.print();

    std::string keys = "{";
    for ( int i = 0; i < MARKS_KEYS; i++ )
    {
        keys += ( i > 0 ? ", " : "" ) + marks_key_arg( i );
    }
    keys += "}";
    std::string values = "{";
    for ( int i = 0; i < MARKS_VALUES; i++ )
    {
        values += ( i > 0 ? ", " : "" ) + tla_int( i );
    }
    values += "}";

    const std::string constants = "    M = " + tla_int( MARKS_M ) + "\n" +
                                  "    Keys = " + keys + "\n" +
                                  "    Vals = " + values + "\n" +
                                  "    DeduplicateForgets = TRUE\n"
                                  "    FullTestExact = TRUE\n"
                                  "    KeptSearchBound = TRUE\n"
                                  "    CompactFromZero = TRUE\n"
                                  "    KeptForgetsRecorded = TRUE\n"
                                  "    NONE = NONE\n";
    return tla_write_trace( out, "MCTraceXdpReplayMarks", seed, "TraceRNXdpReplayMarksLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "xdp-replay-marks", trace_xdp_replay_marks );
