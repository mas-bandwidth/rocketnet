/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The msgxsendruns layer of the TLA+ trace check
// (proofs/tla/README-TraceCheck.md): the macOS transmit worker's send path
// over one taken bank, sdk/rocketnet_msgx_send_runs.h, and its model
// proofs/tla/RNMsgxSendRuns.tla. A seeded random walk stages one bank over
// two fill phases, sorts it with the real sort, and drives the real
// rocketnet_msgx_send_runs_step one model step at a time over a fake table
// (slot 0 open at generation 1, slot 1 at generation 0, as the model's
// instances), a fake connected send and a fake shared send that answer at
// random, and the worker's real give-up check and resume after a kept call.
// Between steps it may retire a slot (at HEAD) or set shutdown (at KEPT), as
// the model's environment does. After every step it writes one record of
// every model variable: the bank, the table, the code's order array and send
// state, and the history the fakes saw (each packet's departures, each
// client's last packet sent, and the order and cross-client flags).
// TraceRNMsgxSendRuns.tla checks the trace. No socket, no clock, no thread.
//
// A packet is counted dropped when the cursor passes it live and it never
// reached a fake; the code's own dropped counter must agree after every
// step, or the record is poisoned (left + 100) and the trace fails.
//
// The header is a header of inline functions, and the transport includes it
// too, so this file compiles its own copy inside a namespace. Under
// ROCKETNET_TLA_PLANT that copy has ROCKETNET_MSGX_SEND_RUNS_PLANT, the sort
// that ignores the fill phase (the order before the fix): its trace must
// fail TraceConsistent at the sort. Every bank has a connected packet in
// phase 0 and a shared one in phase 1, so the two sorts differ on every
// seed; the walk prints PLANT-FIRED n, the pairs a slot-only sort puts out
// of phase order, a property of the input that the clean walk meets too
// (quiet False).

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_MSGX_SEND_RUNS_PLANT 1
#endif // #if defined( ROCKETNET_TLA_PLANT )

namespace tla_trace_msgx_send_runs_code
{
#include "../sdk/rocketnet_msgx_send_runs.h"
}

using namespace tla_trace_msgx_send_runs_code;

// the instance: RNMsgxSendRuns with Size 4, two slots, generations 0 and 1,
// clients 1 and 2, fill phases 0 and 1
#define SR_SIZE 4
#define SR_CAP 2
#define SR_CLIENTS 2
#define SR_STAMPS 5             // the shared socket's, then (slot, generation)
#define SR_MAX_STALLS 4
#define SR_MAX_RETIRES 2
#define SR_RETRIES_AFTER_QUIT 1
#define SR_SHARED -1

struct sr_info_t
{
    int packet_bytes;
    int table_slot;
    uint32_t generation;
    uint32_t phase;
    int client;                 // the walk's own: the destination
};

struct sr_walk_t
{
    tla_random_t random;

    // the bank, sorted, and the send's state
    sr_info_t info[SR_SIZE];
    int count;
    int bucket[SR_CAP + 2];
    int order[SR_SIZE];
    int run_packets[SR_SIZE];
    struct rocketnet_msgx_send_runs_state_t state;
    struct rocketnet_msgx_send_runs_counters_t counters;
    struct rocketnet_msgx_send_runs_io_t io;
    bool sorted;
    bool gave_up;

    // the table
    bool open[SR_CAP];
    uint32_t table_generation[SR_CAP];

    // the history (RNMsgxSendRuns left, lastC, lastS, bad)
    int left[SR_SIZE];
    int last_client[SR_CLIENTS + 1];
    int last_stamp[SR_CLIENTS + 1][SR_STAMPS];
    bool bad_cross;
    bool bad_client;
    bool bad_stamp;
    bool bad_dead;
    bool poisoned;

    // the environment and the bounds
    bool quit;
    int retries;
    int stalls;
    int retires;
};

static int sr_stamp_index( int slot, uint32_t generation )
{
    return slot < 0 ? 0 : 1 + slot * 2 + (int) generation;
}

static int sr_key( const sr_info_t * info )
{
    return info->table_slot >= 0 && info->table_slot < SR_CAP ? info->table_slot + 1 : 0;
}

// the client the slot's socket is connected to: the one stamped at the
// slot's open generation, 0 none (RNMsgxSendRuns Peer)
static int sr_peer( const sr_walk_t * walk, int slot )
{
    for ( int i = 0; i < walk->count; i++ )
    {
        if ( walk->info[i].table_slot == slot && walk->info[i].generation == walk->table_generation[slot] )
        {
            return walk->info[i].client;
        }
    }
    return 0;
}

// one packet onto the wire through via (a client, or SR_SHARED)
static void sr_send_one( sr_walk_t * walk, int i, int via )
{
    const sr_info_t * info = &walk->info[i];
    const int c = info->client;
    const int st = sr_stamp_index( info->table_slot, info->generation );
    walk->left[i]++;
    walk->bad_cross = walk->bad_cross || ( via != SR_SHARED && via != c );
    walk->bad_client = walk->bad_client || walk->last_client[c] > i;
    walk->bad_stamp = walk->bad_stamp || walk->last_stamp[c][st] > i;
    walk->last_client[c] = i;
    walk->last_stamp[c][st] = i;
}

static bool sr_may_stall( const sr_walk_t * walk )
{
    return walk->stalls < SR_MAX_STALLS;
}

static int sr_resolve( void * user, int table_slot, uint32_t generation )
{
    sr_walk_t * walk = (sr_walk_t *) user;
    if ( table_slot < 0 || table_slot >= SR_CAP || !walk->open[table_slot] || walk->table_generation[table_slot] != generation )
    {
        return -1;
    }
    return 100 + table_slot;
}

static int sr_send( void * user, int fd, const int * packets, int num_packets )
{
    sr_walk_t * walk = (sr_walk_t *) user;
    const int slot = fd - 100;
    const int pick = walk->random.below( 10 );
    int answer = num_packets;
    if ( pick == 4 && num_packets >= 2 && sr_may_stall( walk ) )
    {
        answer = 1 + walk->random.below( num_packets - 1 );
    }
    else if ( pick == 5 && sr_may_stall( walk ) )
    {
        answer = ROCKETNET_MSGX_SEND_FULL;
    }
    else if ( pick == 6 && sr_may_stall( walk ) )
    {
        answer = 0;
    }
    else if ( pick == 7 )
    {
        answer = ROCKETNET_MSGX_SEND_REFUSED;
    }
    else if ( pick == 8 )
    {
        answer = ROCKETNET_MSGX_SEND_DEAD;
    }
    else if ( pick == 9 )
    {
        answer = ROCKETNET_MSGX_SEND_ERROR;
    }
    const int peer = sr_peer( walk, slot );
    for ( int n = 0; n < answer; n++ )
    {
        sr_send_one( walk, packets[n], peer );
    }
    return answer;
}

static int sr_send_shared( void * user, int packet )
{
    sr_walk_t * walk = (sr_walk_t *) user;
    const int pick = walk->random.below( 8 );
    if ( pick == 6 )
    {
        return ROCKETNET_MSGX_SHARED_DROPPED;
    }
    if ( pick == 7 && sr_may_stall( walk ) )
    {
        return ROCKETNET_MSGX_SHARED_FULL;
    }
    sr_send_one( walk, packet, SR_SHARED );
    return ROCKETNET_MSGX_SHARED_SENT;
}

static void sr_report_dead( void * user, int table_slot, uint32_t generation )
{
    sr_walk_t * walk = (sr_walk_t *) user;
    (void) generation;
    walk->bad_dead = walk->bad_dead || table_slot < 0;
}

// a stamp for client c in one fill phase: a slot at a generation is one
// client's for the whole bank (owner), the shared socket anyone's
static int sr_pick_stamp( sr_walk_t * walk, int owner[SR_STAMPS], int c, bool connected )
{
    for ( int tries = 0; tries < 16; tries++ )
    {
        const int st = connected ? 1 + walk->random.below( SR_STAMPS - 1 ) : walk->random.below( SR_STAMPS );
        if ( st == 0 )
        {
            return 0;
        }
        if ( owner[st] == 0 || owner[st] == c )
        {
            owner[st] = c;
            return st;
        }
    }
    for ( int st = 1; st < SR_STAMPS; st++ )
    {
        if ( owner[st] == 0 || owner[st] == c )
        {
            owner[st] = c;
            return st;
        }
    }
    return 0;
}

static void sr_stage( sr_walk_t * walk, int i, int c, int st, uint32_t phase )
{
    sr_info_t * info = &walk->info[i];
    info->client = c;
    info->table_slot = st == 0 ? SR_SHARED : ( st - 1 ) / 2;
    info->generation = st == 0 ? 0 : (uint32_t) ( ( st - 1 ) % 2 );
    info->phase = phase;
    info->packet_bytes = walk->random.below( 5 ) == 0 ? 0 : 100;
}

// the bank: num_packets claimed 2..5 (clamped at the size), packets
// [0, split) in fill phase 0 and [split, count) in phase 1, one stamp per
// client per phase; packet 0 on a connected stamp and the last packet on
// the shared socket, so a slot-only sort differs from the phase sort
static int sr_make_bank( sr_walk_t * walk )
{
    const int claimed = 2 + walk->random.below( 4 );
    walk->count = claimed > SR_SIZE ? SR_SIZE : claimed;
    const int split = 1 + walk->random.below( walk->count - 1 );

    int owner[SR_STAMPS];
    memset( owner, 0, sizeof( owner ) );
    int stamp[2][SR_CLIENTS + 1];
    const int first = 1 + walk->random.below( SR_CLIENTS );
    const int last = 1 + walk->random.below( SR_CLIENTS );
    for ( int phase = 0; phase < 2; phase++ )
    {
        for ( int c = 1; c <= SR_CLIENTS; c++ )
        {
            if ( phase == 1 && c == last )
            {
                stamp[phase][c] = 0;
            }
            else
            {
                stamp[phase][c] = sr_pick_stamp( walk, owner, c, phase == 0 && c == first );
            }
        }
    }
    for ( int i = 0; i < walk->count; i++ )
    {
        const uint32_t phase = i < split ? 0 : 1;
        int c = 1 + walk->random.below( SR_CLIENTS );
        if ( i == 0 )
        {
            c = first;
        }
        if ( i == walk->count - 1 )
        {
            c = last;
        }
        sr_stage( walk, i, c, stamp[phase][c], phase );
    }
    for ( int i = walk->count; i < SR_SIZE; i++ )
    {
        walk->info[i].client = 0;
        walk->info[i].table_slot = SR_SHARED;
        walk->info[i].generation = 0;
        walk->info[i].phase = 0;
        walk->info[i].packet_bytes = 0;
    }

    // the plant's condition: pairs in staged order of a later phase that a
    // slot-only sort puts first
    int pairs = 0;
    for ( int i = 0; i < walk->count; i++ )
    {
        for ( int j = i + 1; j < walk->count; j++ )
        {
            if ( walk->info[i].phase < walk->info[j].phase && sr_key( &walk->info[i] ) > sr_key( &walk->info[j] ) )
            {
                pairs++;
            }
        }
    }
    return pairs;
}

// ---------------------------------------------------------------------------
// the record: every RNMsgxSendRuns variable
// ---------------------------------------------------------------------------

static std::string sr_function( int n, const int * values )
{
    std::vector<std::string> keys;
    std::vector<std::string> texts;
    for ( int i = 0; i < n; i++ )
    {
        keys.push_back( tla_int( i ) );
        texts.push_back( tla_int( values[i] ) );
    }
    return tla_function( keys, texts );
}

static std::string sr_stamp_text( int st )
{
    if ( st == 0 )
    {
        return "<<-1, 0>>";
    }
    return "<<" + tla_int( ( st - 1 ) / 2 ) + ", " + tla_int( ( st - 1 ) % 2 ) + ">>";
}

static const char * sr_pc_name( const sr_walk_t * walk )
{
    if ( !walk->sorted )
    {
        return "\"sort\"";
    }
    if ( walk->gave_up )
    {
        return "\"done\"";
    }
    switch ( walk->state.pc )
    {
    case ROCKETNET_MSGX_SEND_RUNS_HEAD:
        return "\"head\"";
    case ROCKETNET_MSGX_SEND_RUNS_SEND:
        return "\"send\"";
    case ROCKETNET_MSGX_SEND_RUNS_DEAD:
        return "\"dead\"";
    case ROCKETNET_MSGX_SEND_RUNS_KEPT:
        return "\"kept\"";
    default:
        return "\"done\"";
    }
}

static tla_record_t sr_record( const sr_walk_t * walk )
{
    int client[SR_SIZE];
    int slot[SR_SIZE];
    int gen[SR_SIZE];
    int phase[SR_SIZE];
    int left[SR_SIZE];
    std::vector<std::string> keys;
    std::vector<std::string> live;
    for ( int i = 0; i < SR_SIZE; i++ )
    {
        client[i] = walk->info[i].client;
        slot[i] = walk->info[i].table_slot;
        gen[i] = (int) walk->info[i].generation;
        phase[i] = (int) walk->info[i].phase;
        left[i] = walk->left[i] + ( walk->poisoned ? 100 : 0 );
        keys.push_back( tla_int( i ) );
        live.push_back( tla_bool( walk->info[i].packet_bytes > 0 ) );
    }

    std::vector<std::string> slot_keys;
    std::vector<std::string> open;
    int table_generation[SR_CAP];
    for ( int s = 0; s < SR_CAP; s++ )
    {
        slot_keys.push_back( tla_int( s ) );
        open.push_back( tla_bool( walk->open[s] ) );
        table_generation[s] = (int) walk->table_generation[s];
    }

    std::string run = "<<";
    for ( int n = 0; n < walk->state.run; n++ )
    {
        run += ( n > 0 ? ", " : "" ) + tla_int( walk->state.run_packets[n] );
    }
    run += ">>";

    std::vector<std::string> client_keys;
    std::vector<std::string> last_client;
    std::vector<std::string> stamp_keys;
    std::vector<std::string> last_stamp;
    for ( int c = 1; c <= SR_CLIENTS; c++ )
    {
        client_keys.push_back( tla_int( c ) );
        last_client.push_back( tla_int( walk->last_client[c] ) );
        for ( int st = 0; st < SR_STAMPS; st++ )
        {
            stamp_keys.push_back( "<<" + tla_int( c ) + ", " + sr_stamp_text( st ) + ">>" );
            last_stamp.push_back( tla_int( walk->last_stamp[c][st] ) );
        }
    }

    tla_record_t record;
    record.add( "count", tla_int( walk->count ) );
    record.add( "client", sr_function( SR_SIZE, client ) );
    record.add( "slot", sr_function( SR_SIZE, slot ) );
    record.add( "gen", sr_function( SR_SIZE, gen ) );
    record.add( "phase", sr_function( SR_SIZE, phase ) );
    record.add( "live", tla_function( keys, live ) );
    record.add( "tOpen", tla_function( slot_keys, open ) );
    record.add( "tGen", sr_function( SR_CAP, table_generation ) );
    record.add( "order", sr_function( SR_SIZE, walk->order ) );
    record.add( "pc", sr_pc_name( walk ) );
    record.add( "cursor", tla_int( walk->state.cursor ) );
    record.add( "hd", tla_int( walk->state.head ) );
    record.add( "scan", tla_int( walk->state.scan ) );
    record.add( "run", run );
    record.add( "attempts", tla_int( walk->state.attempts ) );
    record.add( "left", sr_function( SR_SIZE, left ) );
    record.add( "lastC", tla_function( client_keys, last_client ) );
    record.add( "lastS", tla_function( stamp_keys, last_stamp ) );
    record.add( "bad", "[cross |-> " + tla_bool( walk->bad_cross ) + ", client |-> " + tla_bool( walk->bad_client ) + ", stamp |-> " +
                           tla_bool( walk->bad_stamp ) + ", dead |-> " + tla_bool( walk->bad_dead ) + "]" );
    record.add( "quit", tla_bool( walk->quit ) );
    record.add( "retries", tla_int( walk->retries ) );
    record.add( "stalls", tla_int( walk->stalls ) );
    record.add( "retires", tla_int( walk->retires ) );
    return record;
}

// the packets the cursor passed live that no fake took were dropped: the
// code's dropped counter must have risen by exactly that many
static void sr_attribute_drops( sr_walk_t * walk, int from, int to, uint64_t dropped_before )
{
    uint64_t dropped = 0;
    for ( int p = from; p < to; p++ )
    {
        const int i = walk->order[p];
        if ( walk->info[i].packet_bytes > 0 && walk->left[i] == 0 )
        {
            walk->left[i]++;
            dropped++;
        }
    }
    if ( walk->counters.dropped - dropped_before != dropped )
    {
        fprintf( stderr, "tla_trace msgxsendruns: the dropped counter rose by %" PRIu64 ", the cursor passed %" PRIu64 " packets that never left\n",
                 walk->counters.dropped - dropped_before, dropped );
        walk->poisoned = true;
    }
}

static int trace_msgx_send_runs( uint64_t seed, int steps, const char * out )
{
    sr_walk_t * walk = new sr_walk_t();
    memset( walk, 0, sizeof( *walk ) );
    walk->random.state = seed;
    const int batch = 2 + walk->random.below( 2 );
    walk->open[0] = true;
    walk->open[1] = true;
    walk->table_generation[0] = 1;
    walk->table_generation[1] = 0;
    for ( int c = 0; c <= SR_CLIENTS; c++ )
    {
        walk->last_client[c] = -1;
        for ( int st = 0; st < SR_STAMPS; st++ )
        {
            walk->last_stamp[c][st] = -1;
        }
    }
    walk->io.user = walk;
    walk->io.resolve = sr_resolve;
    walk->io.send = sr_send;
    walk->io.send_shared = sr_send_shared;
    walk->io.report_dead = sr_report_dead;
    rocketnet_msgx_send_runs_start( &walk->state, walk->run_packets );

    const int fired = sr_make_bank( walk );

    std::vector<tla_record_t> records;
    records.push_back( sr_record( walk ) );

    // Sort
    rocketnet_msgx_send_runs_sort( walk->info, walk->count, SR_CAP, walk->bucket, walk->order );
    walk->sorted = true;
    records.push_back( sr_record( walk ) );

    for ( int n = 0; n < steps; n++ )
    {
        if ( walk->gave_up || walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_DONE )
        {
            break;
        }
        if ( walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_HEAD && walk->retires < SR_MAX_RETIRES && walk->random.below( 6 ) == 0 )
        {
            // Retire(s): main retires an open slot while the worker sends
            const int s = walk->random.below( SR_CAP );
            if ( walk->open[s] )
            {
                walk->open[s] = false;
                walk->retires++;
                records.push_back( sr_record( walk ) );
                continue;
            }
        }
        if ( walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_KEPT )
        {
            if ( !walk->quit && walk->random.below( 3 ) == 0 )
            {
                // Quit: main sets shutdown
                walk->quit = true;
                records.push_back( sr_record( walk ) );
                continue;
            }
            // Kept: the worker's check after a kept call
            if ( rocketnet_msgx_send_runs_give_up( walk->quit, &walk->retries, SR_RETRIES_AFTER_QUIT ) )
            {
                const uint64_t dropped_before = walk->counters.dropped;
                walk->counters.dropped += (uint64_t) rocketnet_msgx_send_runs_owed( walk->info, walk->order, walk->count, walk->state.cursor );
                sr_attribute_drops( walk, walk->state.cursor, walk->count, dropped_before );
                walk->gave_up = true;
            }
            else
            {
                rocketnet_msgx_send_runs_resume( &walk->state );
            }
            records.push_back( sr_record( walk ) );
            continue;
        }

        // one step of the send
        const int cursor_before = walk->state.cursor;
        const uint64_t dropped_before = walk->counters.dropped;
        rocketnet_msgx_send_runs_step( walk->info, walk->order, walk->count, batch, &walk->state, &walk->io, &walk->counters );
        if ( walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_KEPT )
        {
            walk->stalls++;
        }
        sr_attribute_drops( walk, cursor_before, walk->state.cursor, dropped_before );
        records.push_back( sr_record( walk ) );
    }

    printf( "PLANT-FIRED %d\n", fired );

    const std::string definitions = "MCTOpenGen(s) == IF s = 0 THEN 1 ELSE 0\n";
    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    Size = %d\n    Batch = %d\n    Cap = %d\n    Gens = {0, 1}\n    Clients = {1, 2}\n    Phases = {0, 1}\n"
              "    Claims = {2, 3, 4, 5}\n    MultiStamp = TRUE\n    MaxStalls = %d\n    MaxRetires = %d\n    RetriesAfterQuit = %d\n"
              "    PhaseSort = TRUE\n    StableSort = TRUE\n    GenBreak = TRUE\n    PartialSkipsAborted = TRUE\n    DeadResends = TRUE\n"
              "    OpenGen <- MCTOpenGen\n",
              SR_SIZE, batch, SR_CAP, SR_MAX_STALLS, SR_MAX_RETIRES, SR_RETRIES_AFTER_QUIT );
#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceMsgxSendRunsPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceMsgxSendRunsSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )
    const bool written = tla_write_trace( out, name, "TraceRNMsgxSendRuns", definitions, constants, records );
    delete walk;
    return written ? 0 : 1;
}

TLA_TRACE_LAYER( "msgxsendruns", trace_msgx_send_runs );
