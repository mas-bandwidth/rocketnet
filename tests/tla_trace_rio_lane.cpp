/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check's walk over the RIO transport's lanes
// (proofs/tla/RNRioLaneLayout.tla, checked by
// proofs/tla/TraceRNRioLaneLayout.tla). The lane arithmetic the Windows
// transport runs is sdk/rocketnet_rio_layout.h: the destination hash, its
// lane, the packet id of a lane's slot and its bounded decode. The walk
// drives exactly those calls as rocketnet_server_rio_start_packet,
// finish_packet and abort_packet make them, keeps the slot table the lanes
// keep, and after every step writes one record of the model's variables.
// No socket, no clock, no thread.
//
//   tla_trace --layer rio-lane --seed N --out DIR [--steps K]
//
// writes DIR/MCTraceRioLaneSeed<seed>.tla and .cfg, one record a step: the
// operation called (start, finish, foreign, send), its arguments, the model
// action its own return value says it took (Start, Finish, Foreign, Send: a
// start that finds its lane full, and a send that finds no frame staged,
// change nothing and are not steps) and the model's variables after it. The
// plant build (ROCKETNET_TLA_PLANT sets ROCKETNET_RIO_LANE_PLANT, in
// sdk/rocketnet_rio_layout.h) decodes the lane as id % lanes; its trace must
// fail TraceConsistent. The walk reports (tla_report_t) the planted effect,
// a decode that left the slot it named, at the record where it happened:
// the planted code's own act, so none in the clean build.

// the plant build (ROCKETNET_RIO_LANE_PLANT, set by the build from the row's
// plants): sdk/rocketnet_rio_layout.h decodes the lane as id % lanes
#include "../sdk/rocketnet_rio_layout.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the instance: four lanes of three slots, six destinations
#define RIO_LANE_LANES 4
#define RIO_LANE_SLOTS 3
#define RIO_LANE_DESTS 6
#define RIO_LANE_NONE ( -1 )

struct rio_lane_dest_t
{
    uint8_t address[4];
    uint16_t port;
    uint32_t hash;
};

struct rio_lane_walk_t
{
    rio_lane_dest_t dests[RIO_LANE_DESTS];
    int slot[RIO_LANE_LANES][RIO_LANE_SLOTS];     // the destination staged there, or none
    std::vector<uint64_t> issued_id;              // reserved and not finished, in issue order
    std::vector<int> issued_dest;
    std::vector<int> staged_dest;                 // finished and not yet sent, in order
    std::vector<int> staged_lane;
    int starts;
    bool strayed;
    bool misrouted;
    int astray;                                   // decodes that left the lane or slot they named
};

// the model names a destination by its hash (RNRioLaneLayout.tla Hashes): the
// walk's destinations' hashes as the model reads them, set by the walk
static int rio_lane_model_hash[RIO_LANE_DESTS];

static std::string rio_lane_dest_name( int d )
{
    return tla_int( rio_lane_model_hash[d] );
}

static tla_record_t rio_lane_record( const rio_lane_walk_t & w )
{
    tla_record_t record;

    std::vector<std::string> lanes;
    std::vector<std::string> lane_values;
    for ( int l = 0; l < RIO_LANE_LANES; l++ )
    {
        std::vector<std::string> keys;
        std::vector<std::string> values;
        for ( int i = 0; i < RIO_LANE_SLOTS; i++ )
        {
            keys.push_back( tla_int( i ) );
            values.push_back( w.slot[l][i] == RIO_LANE_NONE ? tla_int( -1 ) : rio_lane_dest_name( w.slot[l][i] ) );
        }
        lanes.push_back( tla_int( l ) );
        lane_values.push_back( tla_function( keys, values ) );
    }
    record.add( "slot", tla_function( lanes, lane_values ) );

    std::vector<std::string> issued_keys;
    std::vector<std::string> issued_values;
    for ( size_t i = 0; i < w.issued_id.size(); i++ )
    {
        issued_keys.push_back( tla_int( (int64_t) w.issued_id[i] ) );
        issued_values.push_back( rio_lane_dest_name( w.issued_dest[i] ) );
    }
    record.add( "issued", tla_function( issued_keys, issued_values ) );

    std::string staged = "<<";
    for ( size_t i = 0; i < w.staged_dest.size(); i++ )
    {
        staged += ( i > 0 ? ", " : "" );
        staged += "<<" + rio_lane_dest_name( w.staged_dest[i] ) + ", " + tla_int( w.staged_lane[i] ) + ">>";
    }
    staged += ">>";
    record.add( "staged", staged );
    record.add( "starts", tla_int( w.starts ) );
    record.add( "strayed", tla_bool( w.strayed ) );
    record.add( "misrouted", tla_bool( w.misrouted ) );
    return record;
}

// the transport's decode; the plant build compiles sdk/rocketnet_rio_layout.h
// with ROCKETNET_RIO_LANE_PLANT
static bool rio_lane_decode( uint64_t id, int * lane, int * index )
{
    return rocketnet_rio_lane_packet_decode( id, RIO_LANE_LANES, RIO_LANE_SLOTS, lane, index );
}

// rocketnet_server_rio_start_packet: the destination's lane, a free slot of
// it (the lowest: the lane's own reservation takes its next), the id
static bool rio_lane_start( rio_lane_walk_t & w, int d )
{
    const rio_lane_dest_t & dest = w.dests[d];
    const int lane = rocketnet_rio_lane_of( rocketnet_rio_lane_hash( 1, dest.port, dest.address, 4 ), RIO_LANE_LANES );
    for ( int i = 0; i < RIO_LANE_SLOTS; i++ )
    {
        if ( w.slot[lane][i] == RIO_LANE_NONE )
        {
            w.slot[lane][i] = d;
            w.issued_id.push_back( rocketnet_rio_lane_packet_id( lane, i, RIO_LANE_SLOTS ) );
            w.issued_dest.push_back( d );
            w.starts++;
            return true;
        }
    }
    return false;
}

// rocketnet_server_rio_finish_packet (keep) or abort_packet: the decoded
// lane and slot act; one that is not the reserved slot is misrouted
static void rio_lane_finish( rio_lane_walk_t & w, size_t k, bool keep )
{
    const uint64_t id = w.issued_id[k];
    const int d = w.issued_dest[k];
    int lane = -1;
    int index = -1;
    const bool ok = rio_lane_decode( id, &lane, &index );
    const bool in_range = ok && lane >= 0 && lane < RIO_LANE_LANES && index >= 0 && index < RIO_LANE_SLOTS;
    if ( ok && !in_range )
    {
        w.strayed = true;
        w.astray++;
    }
    if ( in_range && w.slot[lane][index] == d && rocketnet_rio_lane_packet_id( lane, index, RIO_LANE_SLOTS ) == id )
    {
        w.slot[lane][index] = RIO_LANE_NONE;
        if ( keep )
        {
            w.staged_dest.push_back( d );
            w.staged_lane.push_back( lane );
        }
    }
    else
    {
        w.misrouted = true;
        w.astray++;
    }
    w.issued_id.erase( w.issued_id.begin() + (long) k );
    w.issued_dest.erase( w.issued_dest.begin() + (long) k );
}

// a finish with an id start never issued: refused, no lane touched
static void rio_lane_foreign( rio_lane_walk_t & w, uint64_t id )
{
    int lane = -1;
    int index = -1;
    if ( rio_lane_decode( id, &lane, &index ) )
    {
        w.misrouted = true;
        w.astray++;
        if ( lane < 0 || lane >= RIO_LANE_LANES )
        {
            w.strayed = true;
        }
    }
}

// a lane's worker posts the oldest frame staged in it
static bool rio_lane_send( rio_lane_walk_t & w, int lane )
{
    for ( size_t k = 0; k < w.staged_lane.size(); k++ )
    {
        if ( w.staged_lane[k] == lane )
        {
            w.staged_dest.erase( w.staged_dest.begin() + (long) k );
            w.staged_lane.erase( w.staged_lane.begin() + (long) k );
            return true;
        }
    }
    return false;
}

static int rio_lane_walk( uint64_t seed, int steps, const char * out )
{
    tla_random_t random = { seed };
    tla_report_t report;
    rio_lane_walk_t w;
    w.starts = 0;
    w.strayed = false;
    w.misrouted = false;
    w.astray = 0;
    for ( int l = 0; l < RIO_LANE_LANES; l++ )
    {
        for ( int i = 0; i < RIO_LANE_SLOTS; i++ )
        {
            w.slot[l][i] = RIO_LANE_NONE;
        }
    }

    // destinations: random IPv4 addresses and ports, as clients arrive
    for ( int d = 0; d < RIO_LANE_DESTS; d++ )
    {
        rio_lane_dest_t & dest = w.dests[d];
        const uint64_t r = random.next();
        dest.address[0] = 172;
        dest.address[1] = 31;
        dest.address[2] = (uint8_t) ( r >> 8 );
        dest.address[3] = (uint8_t) r;
        dest.port = (uint16_t) ( 1024 + ( r >> 16 ) % 64000 );
        dest.hash = rocketnet_rio_lane_hash( 1, dest.port, dest.address, 4 );
        // the model reads h % L; the code folds the high half in first, so
        // the trace hands TLC the folded hash reduced modulo a multiple of
        // L, which keeps its lane and fits TLC's integers
        const uint32_t folded = dest.hash ^ ( dest.hash >> 16 );
        rio_lane_model_hash[d] = (int) ( folded % ( (uint32_t) RIO_LANE_LANES << 20 ) );
    }

    const uint64_t foreign[] = { (uint64_t) RIO_LANE_LANES * RIO_LANE_SLOTS, (uint64_t) RIO_LANE_LANES * RIO_LANE_SLOTS + RIO_LANE_SLOTS,
                                 2ull * RIO_LANE_LANES * RIO_LANE_SLOTS + 1 };

    std::vector<tla_record_t> records;
    records.push_back( tla_step( "init", "init", "<<>>", rio_lane_record( w ) ) );
    for ( int step = 0; step < steps; step++ )
    {
        const int astray_before = w.astray;
        bool moved = false;
        // every step is a record: an action the code finds nothing to do for is drawn again
        while ( !moved )
        {
            const int action = random.below( 10 );
            if ( action < 4 && w.starts < steps )
            {
                const int d = random.below( RIO_LANE_DESTS );
            // the action is the call's return value: a start that finds its lane full is no step
                if ( rio_lane_start( w, d ) )
                {
                    report.act( "Start" );
                    records.push_back( tla_step( "start", "Start", tla_sequence( std::vector<std::string>( 1, rio_lane_dest_name( d ) ) ), rio_lane_record( w ) ) );
                    moved = true;
                }
            }
            else if ( action < 7 && !w.issued_id.empty() )
            {
                const size_t k = (size_t) random.below( (int) w.issued_id.size() );
                const uint64_t id = w.issued_id[k];
                const bool keep = random.below( 4 ) != 0;
                rio_lane_finish( w, k, keep );
                std::vector<std::string> args;
                args.push_back( tla_int( (int64_t) id ) );
                args.push_back( tla_bool( keep ) );
                report.act( "Finish" );
                records.push_back( tla_step( "finish", "Finish", tla_sequence( args ), rio_lane_record( w ) ) );
                moved = true;
            }
            else if ( action < 8 )
            {
                const uint64_t id = foreign[random.below( 3 )];
                rio_lane_foreign( w, id );
                report.act( "Foreign" );
                records.push_back( tla_step( "foreign", "Foreign", tla_sequence( std::vector<std::string>( 1, tla_int( (int64_t) id ) ) ), rio_lane_record( w ) ) );
                moved = true;
            }
            else
            {
                const int lane = random.below( RIO_LANE_LANES );
                if ( rio_lane_send( w, lane ) )
                {
                    report.act( "Send" );
                    records.push_back( tla_step( "send", "Send", tla_sequence( std::vector<std::string>( 1, tla_int( lane ) ) ), rio_lane_record( w ) ) );
                    moved = true;
                }
            }
        }
        // the planted effect, where it shows: a decode that left the slot it named
        if ( w.astray != astray_before )
        {
            report.planted( (int) records.size() );
        }
    }
    std::string hashes = "{";
    for ( int d = 0; d < RIO_LANE_DESTS; d++ )
    {
        hashes += ( d > 0 ? ", " : "" ) + rio_lane_dest_name( d );
    }
    hashes += "}";
    char constants[256];
    snprintf( constants, sizeof( constants ), "    L = %d\n    F = %d\n    MaxStarts = %d\n    Mode = \"Code\"\n    Hashes = %s\n", RIO_LANE_LANES, RIO_LANE_SLOTS, steps, hashes.c_str() );
    report.print();
    return tla_write_trace( out, "MCTraceRioLane", seed, "TraceRNRioLaneLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "rio-lane", rio_lane_walk );
