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
// writes DIR/MCTraceRioLaneSeed<seed>.tla and .cfg. The plant build
// (ROCKETNET_TLA_PLANT sets ROCKETNET_RIO_LANE_PLANT, in
// sdk/rocketnet_rio_layout.h) decodes the lane as id % lanes; its trace must
// fail TraceConsistent. PLANT-FIRED counts the decodes that left the slot they
// named: the planted code's own act, so zero in the clean build.

// the plant build: sdk/rocketnet_rio_layout.h decodes the lane as id % lanes
#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_RIO_LANE_PLANT 1
#endif // #if defined( ROCKETNET_TLA_PLANT )
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

static std::string rio_lane_dest_name( int d )
{
    return "\"d" + tla_int( d + 1 ) + "\"";
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
            values.push_back( w.slot[l][i] == RIO_LANE_NONE ? "\"none\"" : rio_lane_dest_name( w.slot[l][i] ) );
        }
        lanes.push_back( tla_int( l ) );
        lane_values.push_back( tla_function( keys, values ) );
    }
    record.add( "slot", tla_function( lanes, lane_values ) );

    if ( w.issued_id.empty() )
    {
        record.add( "issued", "[x \\in {} |-> \"none\"]" );
    }
    else
    {
        std::vector<std::string> keys;
        std::vector<std::string> values;
        for ( size_t i = 0; i < w.issued_id.size(); i++ )
        {
            keys.push_back( tla_int( (int64_t) w.issued_id[i] ) );
            values.push_back( rio_lane_dest_name( w.issued_dest[i] ) );
        }
        record.add( "issued", tla_function( keys, values ) );
    }

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
    std::string hashes;
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
        const uint32_t model_hash = folded % ( (uint32_t) RIO_LANE_LANES << 20 );
        hashes += ( d > 0 ? " @@ " : "" );
        hashes += "(" + rio_lane_dest_name( d ) + " :> " + tla_int( model_hash ) + ")";
    }

    const uint64_t foreign[] = { (uint64_t) RIO_LANE_LANES * RIO_LANE_SLOTS, (uint64_t) RIO_LANE_LANES * RIO_LANE_SLOTS + RIO_LANE_SLOTS,
                                 2ull * RIO_LANE_LANES * RIO_LANE_SLOTS + 1 };

    std::vector<tla_record_t> records;
    records.push_back( rio_lane_record( w ) );
    for ( int step = 0; step < steps; step++ )
    {
        const int action = random.below( 10 );
        bool moved = false;
        if ( action < 4 && w.starts < steps )
        {
            moved = rio_lane_start( w, random.below( RIO_LANE_DESTS ) );
        }
        else if ( action < 7 && !w.issued_id.empty() )
        {
            rio_lane_finish( w, (size_t) random.below( (int) w.issued_id.size() ), random.below( 4 ) != 0 );
            moved = true;
        }
        else if ( action < 8 )
        {
            // a refused finish changes no model variable unless it went astray
            const bool before = w.misrouted;
            rio_lane_foreign( w, foreign[random.below( 3 )] );
            moved = w.misrouted != before;
        }
        else
        {
            moved = rio_lane_send( w, random.below( RIO_LANE_LANES ) );
        }
        if ( moved )
        {
            records.push_back( rio_lane_record( w ) );
        }
    }

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceRioLanePlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceRioLaneSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )
    const std::string definitions = "TraceHashOf == " + hashes + "\n";
    const std::string constants = "    L <- TraceL\n    F <- TraceF\n    Dests <- TraceDests\n    HashOf <- TraceHashOf\n"
                                  "    MaxStarts <- TraceMaxStarts\n    Mode <- TraceMode\n";
    std::string dests = "{";
    for ( int d = 0; d < RIO_LANE_DESTS; d++ )
    {
        dests += ( d > 0 ? ", " : "" ) + rio_lane_dest_name( d );
    }
    dests += "}";
    const std::string all = definitions + "TraceL == " + tla_int( RIO_LANE_LANES ) + "\nTraceF == " + tla_int( RIO_LANE_SLOTS ) +
                            "\nTraceDests == " + dests + "\nTraceMaxStarts == " + tla_int( steps ) + "\nTraceMode == \"Code\"\n";
    printf( "PLANT-FIRED %d\n", w.astray );
    return tla_write_trace( out, name, "TraceRNRioLaneLayout", all, constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "rio-lane", rio_lane_walk );
