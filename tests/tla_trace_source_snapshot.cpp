/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace walk of the connected-source snapshot
// (sdk/rocketnet_source_snapshot.h), checked by
// proofs/tla/TraceRNSourceSnapshotLayout.tla: one writer and two readers on a
// snapshot of two entries (four cells a slot), interleaved one code call at
// a time in a seeded random order. The writer's calls are the pieces of
// rocketnet_source_snapshot_writer_begin (writer_scan per reader, then
// writer_fill), writer_add of one of three addresses and writer_publish; a
// reader's are the pieces of rocketnet_source_snapshot_reader_acquire
// (reader_load, reader_announce, reader_recheck), contains and
// reader_release. Each call is one record (tla_step): the call, its
// arguments and what it returned (the scan's skip, the loaded word, the
// re-check's result, the lookup's address and answer), the action the
// call's result names (Add or Refuse by the add's result), and every model
// variable after it: the cells, the published word and the announcements
// read from the snapshot's own memory, the writer's target from its state,
// and the walk's own program counters and ghosts (the sets added and
// published, the set published at each reader's load, the counts the caller
// keeps). The writer adds only an address it has not added this update, so
// no add is a repeat. The three addresses are chosen so the real hash homes
// two on the last cell and one on cell 0, as MCRNSourceSnapshotLayout does,
// so the probe wraps; their homes are written into the cfg as HomeAt.
// The planted effect (ROCKETNET_SOURCE_SNAPSHOT_PLANT, the publish store
// before the add, in sdk/rocketnet_source_snapshot.cpp) is an add after
// which the published slot is the writer's target, which the clean code
// never shows.

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_source_snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "tla_trace_record.h"

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_SOURCE_SNAPSHOT_PLANT 1
#include "../sdk/rocketnet_source_snapshot.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#define SNAPSHOT_READERS 2
#define SNAPSHOT_ADDRS 3
#define SNAPSHOT_MAX_ENTRIES 2

enum snapshot_writer_pc_t
{
    SNAPSHOT_WRITER_IDLE,
    SNAPSHOT_WRITER_SCAN,
    SNAPSHOT_WRITER_ADD,
};

enum snapshot_reader_pc_t
{
    SNAPSHOT_READER_IDLE,
    SNAPSHOT_READER_LOADED,
    SNAPSHOT_READER_ANNOUNCED,
    SNAPSHOT_READER_HELD,
};

struct snapshot_walk_t
{
    struct rocketnet_source_snapshot_t snapshot;
    struct rocketnet_address_t addrs[SNAPSHOT_ADDRS];

    snapshot_writer_pc_t wpc;
    int wi;
    unsigned wset;          // bit a - 1: address a added this update
    unsigned pset;          // bit a - 1: address a in the published set
    int skipped;
    int refused;

    snapshot_reader_pc_t rpc[SNAPSHOT_READERS];
    bool loaded[SNAPSHOT_READERS];
    uint64_t rl[SNAPSHOT_READERS];
    unsigned held[SNAPSHOT_READERS];
    int look_addr[SNAPSHOT_READERS];
    bool look_found[SNAPSHOT_READERS];
};

static std::string snapshot_set( unsigned bits )
{
    std::string text = "{";
    bool first = true;
    for ( int a = 1; a <= SNAPSHOT_ADDRS; a++ )
    {
        if ( bits & ( 1u << ( a - 1 ) ) )
        {
            text += ( first ? "" : ", " ) + tla_int( a );
            first = false;
        }
    }
    return text + "}";
}

static std::string snapshot_word( uint64_t word )
{
    // the generation is 63 bits, so it fits int64_t exactly; tla_int
    // refuses one past TLC's 32-bit integers
    return "[slot |-> " + tla_int( rocketnet_source_snapshot_word_slot( word ) ) + ", gen |-> " +
           tla_int( (int64_t) rocketnet_source_snapshot_word_generation( word ) ) + "]";
}

static std::string snapshot_args( const std::vector<std::string> & args )
{
    return tla_sequence( args );
}

static std::string snapshot_arg( int64_t a )
{
    return tla_sequence( std::vector<std::string>( 1, tla_int( a ) ) );
}

static const char * snapshot_writer_pc_name( snapshot_writer_pc_t pc )
{
    switch ( pc )
    {
        case SNAPSHOT_WRITER_IDLE: return "\"idle\"";
        case SNAPSHOT_WRITER_SCAN: return "\"scan\"";
        default:                   return "\"add\"";
    }
}

static const char * snapshot_reader_pc_name( snapshot_reader_pc_t pc )
{
    switch ( pc )
    {
        case SNAPSHOT_READER_IDLE:      return "\"idle\"";
        case SNAPSHOT_READER_LOADED:    return "\"loaded\"";
        case SNAPSHOT_READER_ANNOUNCED: return "\"announced\"";
        default:                        return "\"held\"";
    }
}

// the model's id of a cell's address: 0 for an empty cell
static int snapshot_cell_id( const snapshot_walk_t & w, const struct rocketnet_address_t * cell )
{
    if ( cell->type == ROCKETNET_ADDRESS_NONE )
    {
        return 0;
    }
    for ( int a = 0; a < SNAPSHOT_ADDRS; a++ )
    {
        if ( rocketnet_address_equal( cell, &w.addrs[a] ) )
        {
            return a + 1;
        }
    }
    return -1;          // an address the walk never added: no model state, the trace is rejected
}

static tla_record_t snapshot_record( const snapshot_walk_t & w )
{
    const struct rocketnet_source_snapshot_t * s = &w.snapshot;

    std::vector<std::string> slot_keys;
    std::vector<std::string> slot_values;
    for ( int slot = 0; slot < 2; slot++ )
    {
        std::vector<std::string> cell_keys;
        std::vector<std::string> cell_values;
        for ( int i = 0; i < s->capacity; i++ )
        {
            cell_keys.push_back( tla_int( i ) );
            cell_values.push_back( tla_int( snapshot_cell_id( w, &s->cells[slot * s->capacity + i] ) ) );
        }
        slot_keys.push_back( tla_int( slot ) );
        slot_values.push_back( tla_function( cell_keys, cell_values ) );
    }

    std::vector<std::string> reader_keys;
    std::vector<std::string> ann;
    std::vector<std::string> rpc;
    std::vector<std::string> rl;
    std::vector<std::string> held;
    std::vector<std::string> look;
    for ( int r = 0; r < SNAPSHOT_READERS; r++ )
    {
        reader_keys.push_back( tla_int( r + 1 ) );
        ann.push_back( tla_int( s->reader[r].slot.load( std::memory_order_relaxed ) ) );
        rpc.push_back( snapshot_reader_pc_name( w.rpc[r] ) );
        rl.push_back( w.loaded[r] ? snapshot_word( w.rl[r] ) : "[slot |-> -1, gen |-> -1]" );
        held.push_back( snapshot_set( w.held[r] ) );
        look.push_back( "[addr |-> " + tla_int( w.look_addr[r] ) + ", found |-> " + tla_bool( w.look_found[r] ) + "]" );
    }

    tla_record_t record;
    record.add( "slots", tla_function( slot_keys, slot_values ) );
    record.add( "pub", snapshot_word( s->published->word.load( std::memory_order_relaxed ) ) );
    record.add( "ann", tla_function( reader_keys, ann ) );
    record.add( "wpc", snapshot_writer_pc_name( w.wpc ) );
    record.add( "wi", tla_int( w.wi ) );
    record.add( "wslot", tla_int( s->writer->target ) );
    record.add( "wset", snapshot_set( w.wset ) );
    record.add( "pset", snapshot_set( w.pset ) );
    record.add( "skipped", tla_int( w.skipped ) );
    record.add( "refused", tla_int( w.refused ) );
    record.add( "rpc", tla_function( reader_keys, rpc ) );
    record.add( "rl", tla_function( reader_keys, rl ) );
    record.add( "held", tla_function( reader_keys, held ) );
    record.add( "look", tla_function( reader_keys, look ) );
    return record;
}

// three addresses whose real homes in a slot of capacity cells are
// capacity - 1, capacity - 1 and 0, as HomeAt in MCRNSourceSnapshotLayout.cfg
static bool snapshot_pick_addresses( snapshot_walk_t & w )
{
    const int capacity = w.snapshot.capacity;
    const int wanted[SNAPSHOT_ADDRS] = { capacity - 1, capacity - 1, 0 };
    int found = 0;
    for ( int port = 40000; port < 50000 && found < SNAPSHOT_ADDRS; port++ )
    {
        struct rocketnet_address_t a;
        memset( &a, 0, sizeof( a ) );
        a.type = ROCKETNET_ADDRESS_IPV4;
        a.data.ipv4[0] = 10;
        a.data.ipv4[3] = 7;
        a.port = (uint16_t) port;
        if ( rocketnet_source_snapshot_home( &w.snapshot, &a ) == wanted[found] )
        {
            w.addrs[found++] = a;
        }
    }
    return found == SNAPSHOT_ADDRS;
}

static int trace_source_snapshot( uint64_t seed, int steps, const char * out )
{
    std::vector<uint64_t> memory( rocketnet_source_snapshot_bytes( SNAPSHOT_MAX_ENTRIES, SNAPSHOT_READERS ) / sizeof( uint64_t ) + 1 );

    snapshot_walk_t w;
    if ( !rocketnet_source_snapshot_init( &w.snapshot, memory.data(), SNAPSHOT_MAX_ENTRIES, SNAPSHOT_READERS ) )
    {
        return 1;
    }
    if ( !snapshot_pick_addresses( w ) )
    {
        return 1;
    }
    struct rocketnet_source_snapshot_t * s = &w.snapshot;

    w.wpc = SNAPSHOT_WRITER_IDLE;
    w.wi = 0;
    w.wset = 0;
    w.pset = 0;
    w.skipped = 0;
    w.refused = 0;
    for ( int r = 0; r < SNAPSHOT_READERS; r++ )
    {
        w.rpc[r] = SNAPSHOT_READER_IDLE;
        w.loaded[r] = false;
        w.rl[r] = 0;
        w.held[r] = 0;
        w.look_addr[r] = 0;
        w.look_found[r] = false;
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", snapshot_record( w ) ) );

    for ( int step = 0; step < steps; step++ )
    {
        // one of the writer's next call or a reader's, evenly
        const int actor = random.below( 1 + SNAPSHOT_READERS );
        if ( actor == 0 )
        {
            if ( w.wpc == SNAPSHOT_WRITER_IDLE || w.wpc == SNAPSHOT_WRITER_SCAN )
            {
                if ( w.wi < SNAPSHOT_READERS )
                {
                    const bool skip = rocketnet_source_snapshot_writer_scan( s, w.wi );
                    if ( skip )
                    {
                        w.skipped++;
                        w.wpc = SNAPSHOT_WRITER_IDLE;
                        w.wi = 0;
                    }
                    else
                    {
                        w.wpc = SNAPSHOT_WRITER_SCAN;
                        w.wi++;
                    }
                    report.act( "Scan" );
                    records.push_back( tla_step( "writer_scan", "Scan", snapshot_args( std::vector<std::string>( 1, tla_bool( skip ) ) ),
                                                 snapshot_record( w ) ) );
                }
                else
                {
                    rocketnet_source_snapshot_writer_fill( s );
                    w.wpc = SNAPSHOT_WRITER_ADD;
                    w.wi = 0;
                    w.wset = 0;
                    report.act( "Fill" );
                    records.push_back( tla_step( "writer_fill", "Fill", "<<>>", snapshot_record( w ) ) );
                }
            }
            else if ( random.below( 4 ) == 0 )
            {
                if ( !rocketnet_source_snapshot_writer_publish( s ) )
                {
                    return 1;
                }
                w.pset = w.wset;
                w.wpc = SNAPSHOT_WRITER_IDLE;
                report.act( "Publish" );
                records.push_back( tla_step( "writer_publish", "Publish", "<<>>", snapshot_record( w ) ) );
            }
            else
            {
                // an address not added this update: there are three and a
                // slot takes two, so one always remains
                int choices[SNAPSHOT_ADDRS];
                int count = 0;
                for ( int a = 0; a < SNAPSHOT_ADDRS; a++ )
                {
                    if ( !( w.wset & ( 1u << a ) ) )
                    {
                        choices[count++] = a;
                    }
                }
                const int a = choices[random.below( count )];
                const int target = s->writer->target;
                const bool added = rocketnet_source_snapshot_writer_add( s, &w.addrs[a] );
                if ( added )
                {
                    w.wset |= 1u << a;
                }
                else
                {
                    w.refused++;
                }
                const char * act = added ? "Add" : "Refuse";
                report.act( act );
                records.push_back( tla_step( "writer_add", act, snapshot_arg( a + 1 ), snapshot_record( w ) ) );
                // the planted effect, where it shows: the target published
                if ( rocketnet_source_snapshot_word_slot( s->published->word.load( std::memory_order_relaxed ) ) == target )
                {
                    report.planted( (int) records.size() );
                }
            }
        }
        else
        {
            const int r = actor - 1;
            switch ( w.rpc[r] )
            {
                case SNAPSHOT_READER_IDLE:
                {
                    w.rl[r] = rocketnet_source_snapshot_reader_load( s );
                    w.loaded[r] = true;
                    w.held[r] = w.pset;
                    w.rpc[r] = SNAPSHOT_READER_LOADED;
                    std::vector<std::string> args;
                    args.push_back( tla_int( r + 1 ) );
                    args.push_back( snapshot_word( w.rl[r] ) );
                    report.act( "Load" );
                    records.push_back( tla_step( "reader_load", "Load", snapshot_args( args ), snapshot_record( w ) ) );
                    break;
                }
                case SNAPSHOT_READER_LOADED:
                {
                    rocketnet_source_snapshot_reader_announce( s, r, w.rl[r] );
                    w.rpc[r] = SNAPSHOT_READER_ANNOUNCED;
                    report.act( "Announce" );
                    records.push_back( tla_step( "reader_announce", "Announce", snapshot_arg( r + 1 ), snapshot_record( w ) ) );
                    break;
                }
                case SNAPSHOT_READER_ANNOUNCED:
                {
                    const bool ok = rocketnet_source_snapshot_reader_recheck( s, r, w.rl[r] );
                    if ( ok )
                    {
                        w.rpc[r] = SNAPSHOT_READER_HELD;
                    }
                    else
                    {
                        w.rpc[r] = SNAPSHOT_READER_IDLE;
                        w.loaded[r] = false;
                        w.held[r] = 0;
                    }
                    std::vector<std::string> args;
                    args.push_back( tla_int( r + 1 ) );
                    args.push_back( tla_bool( ok ) );
                    report.act( "Recheck" );
                    records.push_back( tla_step( "reader_recheck", "Recheck", snapshot_args( args ), snapshot_record( w ) ) );
                    break;
                }
                default:
                {
                    if ( random.below( 4 ) == 0 )
                    {
                        rocketnet_source_snapshot_reader_release( s, r );
                        w.rpc[r] = SNAPSHOT_READER_IDLE;
                        w.loaded[r] = false;
                        w.held[r] = 0;
                        w.look_addr[r] = 0;
                        w.look_found[r] = false;
                        report.act( "Release" );
                        records.push_back( tla_step( "reader_release", "Release", snapshot_arg( r + 1 ), snapshot_record( w ) ) );
                    }
                    else
                    {
                        const int a = random.below( SNAPSHOT_ADDRS );
                        const int slot = rocketnet_source_snapshot_word_slot( w.rl[r] );
                        const bool found = rocketnet_source_snapshot_contains( s, slot, &w.addrs[a] );
                        w.look_addr[r] = a + 1;
                        w.look_found[r] = found;
                        std::vector<std::string> args;
                        args.push_back( tla_int( r + 1 ) );
                        args.push_back( tla_int( a + 1 ) );
                        args.push_back( tla_bool( found ) );
                        report.act( "Lookup" );
                        records.push_back( tla_step( "contains", "Lookup", snapshot_args( args ), snapshot_record( w ) ) );
                    }
                    break;
                }
            }
        }
    }

    report.print();

    // the homes the code's hash gives the three addresses, as the model's
    // HomeAt: a * Cap + h for address a on cell h
    std::string home_at = "{";
    for ( int a = 0; a < SNAPSHOT_ADDRS; a++ )
    {
        home_at += ( a > 0 ? ", " : "" ) + tla_int( (int64_t) ( a + 1 ) * s->capacity + rocketnet_source_snapshot_home( s, &w.addrs[a] ) );
    }
    home_at += "}";

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    Readers = {1, 2}\n    Addrs = {1, 2, 3}\n    Cap = %d\n    MaxEntries = %d\n    HomeAt = %s\n"
              "    MaxUpdates = 1000000\n    MaxRefused = 1000000\n    HalfPublished = FALSE\n    NoRecheck = FALSE\n"
              "    LookupNoWrap = FALSE\n",
              s->capacity, SNAPSHOT_MAX_ENTRIES, home_at.c_str() );

    return tla_write_trace( out, "MCTraceSourceSnapshot", seed, "TraceRNSourceSnapshotLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "source-snapshot", trace_source_snapshot );
