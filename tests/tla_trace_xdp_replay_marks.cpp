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
// The operations and the records they write:
//   forget          rocketnet_xdp_replay_marks_forget          Forget(k)
//   mark            rocketnet_xdp_replay_marks_mark            Mark(k, v)
//   commit          the kernel's answer, chosen by the walk    Commit
//   reset_keeping   rocketnet_xdp_replay_marks_reset_keeping   CompactForgetStep
//                   per forget-loop iteration and one for its end, then
//                   CompactMarkStep the same way for the mark loop
// The calls inside reset_keeping are steps the walk cannot observe: their
// records are hidden (state <<>>) but the last, which carries the code's
// state after the call (TraceRNXdpReplayMarksLayout.tla says why that is
// sound for these two actions). The ghosts absF, absM and absK are the
// code's own pending lists and kept_mark_count; at the commit record they
// are the lists reset_keeping left, so the model's contract update is
// checked against the code's compaction.
//
// The plant (ROCKETNET_TLA_PLANT_XDP_REPLAY_MARKS in the header) tests the
// forget list's room one late, the reversed witness
// MCRNXdpReplayMarksLayoutBrokenFullOffByOne: a new key is written at index
// M. Its effect is a forget that leaves forget_count past the capacity; the
// walk reports it at that record. No socket, no clock.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_TLA_PLANT_XDP_REPLAY_MARKS 1
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

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

        // the walk's own state: the model's phase, li, lk, failed, applied,
        // doneF and doneM
        const char * phase;
        int li;
        int lk;
        std::vector<int> failed;
        std::vector<int> applied;
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

    // the model's variables. abs_from is the buffer the ghosts are read from:
    // the walk's own, or a copy of it after a reset_keeping call
    tla_record_t marks_state( const marks_walk_t & walk, const rocketnet_xdp_replay_marks_t & abs_from )
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
        for ( uint32_t i = 0; i < abs_from.forget_count && i <= MARKS_M; i++ )
        {
            abs_forgets.push_back( marks_key_name( walk, abs_from.forgets[i] ) );
        }
        std::vector<std::string> abs_marks;
        for ( uint32_t i = 0; i < abs_from.mark_count && i <= MARKS_M; i++ )
        {
            abs_marks.push_back( "[k |-> " + marks_key_name( walk, abs_from.mark_keys[i] ) + ", v |-> " + marks_value( abs_from.mark_values[i].high_water ) + "]" );
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
        state.add( "li", tla_int( walk.li ) );
        state.add( "lk", tla_int( walk.lk ) );
        state.add( "failed", marks_set( walk.failed ) );
        state.add( "applied", marks_set( walk.applied ) );
        state.add( "doneF", marks_done( walk.done_forget ) );
        state.add( "doneM", marks_done( walk.done_mark ) );
        state.add( "absF", tla_sequence( abs_forgets ) );
        state.add( "absM", tla_sequence( abs_marks ) );
        state.add( "absK", tla_int( abs_from.kept_mark_count ) );
        state.add( "overflow", tla_bool( overflow ) );
        return state;
    }

    // a hidden record's state is <<>>, the function on the empty set
    tla_record_t marks_hidden_step( const char * op, const char * act )
    {
        tla_record_t step;
        step.add( "op", tla_string( op ) );
        step.add( "act", tla_string( act ) );
        step.add( "args", "<<>>" );
        step.add( "state", tla_function( std::vector<std::string>(), std::vector<std::string>() ) );
        return step;
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
    walk.li = 0;
    walk.lk = 0;
    walk.failed.clear();
    walk.applied.clear();

    rocketnet_xdp_replay_marks_bind( &walk.marks, walk.forgets, walk.mark_keys, walk.mark_values, MARKS_M );

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    std::vector<int> planted;

    records.push_back( tla_step( "init", "init", tla_sequence( std::vector<std::string>() ), marks_state( walk, walk.marks ) ) );
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
            std::vector<std::string> args;
            args.push_back( marks_key_arg( k ) );
            args.push_back( tla_bool( queued ) );
            records.push_back( tla_step( "forget", "Forget", tla_sequence( args ), marks_state( walk, walk.marks ) ) );
            if ( queued && walk.marks.forget_count > walk.marks.capacity )
            {
                planted.push_back( (int) records.size() );
            }
        }
        else if ( choice < 7 && !can_mark.empty() )
        {
            const int k = can_mark[(size_t) random.below( (int) can_mark.size() )];
            const uint32_t value = (uint32_t) random.below( MARKS_VALUES );
            const bool marked = rocketnet_xdp_replay_marks_mark( &walk.marks, &walk.address[k], value );
            walk.done_mark[k] = true;
            walk.phase = "mark";
            std::vector<std::string> args;
            args.push_back( marks_key_arg( k ) );
            args.push_back( tla_int( value ) );
            args.push_back( tla_bool( marked ) );
            records.push_back( tla_step( "mark", "Mark", tla_sequence( args ), marks_state( walk, walk.marks ) ) );
        }
        else
        {
            // the kernel's answer: each forget refused one time in two, each
            // mark applied two times in three
            walk.failed.clear();
            walk.applied.clear();
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
                    walk.failed.push_back( (int) i );
                }
            }
            for ( uint32_t j = 0; j < walk.marks.mark_count; j++ )
            {
                walk.mark_applied[j] = random.below( 3 ) != 0;
                if ( walk.mark_applied[j] )
                {
                    walk.applied.push_back( (int) j );
                }
            }

            // the loops' iterations: each runs to its count, then its end
            const uint32_t forget_steps = walk.marks.forget_count + 1;
            const uint32_t mark_steps = walk.marks.mark_count + 1;

            // the commit record: the arrays before the call, the kernel's
            // answer, and the ghosts the call leaves; written once the call
            // has run
            walk.phase = "cf";
            walk.li = 0;
            walk.lk = 0;
            marks_walk_t before = walk;

            const uint32_t refusals = rocketnet_xdp_replay_marks_reset_keeping( &walk.marks, walk.forget_failed, walk.mark_applied );

            // before's arrays are its own copies; its ghosts come from the
            // code's lists after the call
            rocketnet_xdp_replay_marks_t after = walk.marks;
            after.forgets = walk.forgets;
            after.mark_keys = walk.mark_keys;
            after.mark_values = walk.mark_values;
            before.marks.forgets = before.forgets;
            before.marks.mark_keys = before.mark_keys;
            before.marks.mark_values = before.mark_values;

            std::vector<std::string> args;
            args.push_back( marks_set( walk.failed ) );
            args.push_back( marks_set( walk.applied ) );
            args.push_back( tla_int( refusals ) );
            records.push_back( tla_step( "commit", "Commit", tla_sequence( args ), marks_state( before, after ) ) );

            for ( uint32_t i = 0; i < forget_steps; i++ )
            {
                records.push_back( marks_hidden_step( "reset_keeping", "CompactForgetStep" ) );
            }
            for ( uint32_t j = 0; j + 1 < mark_steps; j++ )
            {
                records.push_back( marks_hidden_step( "reset_keeping", "CompactMarkStep" ) );
            }

            walk.phase = "forget";
            walk.li = 0;
            walk.lk = 0;
            walk.failed.clear();
            walk.applied.clear();
            for ( int i = 0; i < MARKS_KEYS; i++ )
            {
                walk.done_forget[i] = false;
                walk.done_mark[i] = false;
            }
            records.push_back( tla_step( "reset_keeping", "CompactMarkStep", tla_sequence( std::vector<std::string>() ), marks_state( walk, walk.marks ) ) );
        }
    }

    // a commit's records may run past the walk's length: the trace is its
    // first steps + 1 records, every one a step the code took
    records.resize( (size_t) steps + 1 );

    tla_report_t report;
    for ( size_t i = 1; i < records.size(); i++ )
    {
        // the act field's text is the quoted name
        for ( size_t f = 0; f < records[i].fields.size(); f++ )
        {
            if ( records[i].fields[f] == "act" )
            {
                const std::string & quoted = records[i].values[f];
                report.act( quoted.substr( 1, quoted.size() - 2 ).c_str() );
            }
        }
    }
    for ( size_t i = 0; i < planted.size(); i++ )
    {
        if ( planted[i] <= steps + 1 )
        {
            report.planted( planted[i] );
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
