/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The msgxsendruns layer of the TLA+ trace check
// (proofs/tla/README-TraceCheck.md): the macOS transmit worker's send path
// over one taken bank, sdk/rocketnet_msgx_send_runs.h, and its model
// proofs/tla/RNMsgxSendRuns.tla. A seeded random walk stages one bank of up
// to SR_SIZE packets over three slots and three clients, sorts it with the real sort,
// and drives the real rocketnet_msgx_send_runs_step one model step at a time
// over a fake table (each slot open at the generation OpenGenAt gives), a
// fake connected send and a fake shared send that answer at random, and the
// worker's real give-up check and resume after a kept call. Between steps it
// may retire a slot (at HEAD) or set shutdown (at KEPT), as the model's
// environment does. Once the bank is done or given up, the worker's step
// takes no step, and the walk records that call (Done) until the trace has
// its steps + 1 records.
//
// Each record is tla_step( op, act, args, state ): op the call ("sort",
// "step", "check" after a kept call, "retire" and "quit" for the
// environment), act the model action the call's answer names (the fakes'
// answer to it, the give-up check's result, or the step's out-state where no
// fake was called), args the action's parameters (the prefix a partial send
// took, the slot retired) and state every model variable: the bank, the
// table, the code's order array and send state, and the history the fakes
// saw (each packet's departures, each client's last packet sent, and the
// order and cross-client flags). proofs/tla/TraceRNMsgxSendRuns.tla checks
// the trace.
// The bank is the walk's input: the cfg gives it as Bank, so Init is that one
// bank. No socket, no clock, no thread.
//
// A packet is counted dropped when the cursor passes it live and it never
// reached a fake; the code's own dropped counter must agree after every
// step, or the record is poisoned (left + 100) and the trace fails.
//
// The plant (ROCKETNET_MSGX_SEND_RUNS_PLANT, the run builder that crosses a
// generation) is in sdk/rocketnet_msgx_send_runs.cpp. The bank's first six
// packets are, for each slot, a packet at the slot's open generation and then
// a packet of another client at a stale generation of the same slot, so the
// first run built on a slot that is still open has a generation change right
// after its head: the clean run ends there and the planted one goes on. The
// record of that run (HeadRun) is PLANT-AT, where the run holds a packet of
// another generation than its head's.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../sdk/rocketnet_msgx_send_runs.h"
#include "tla_trace_record.h"

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_MSGX_SEND_RUNS_PLANT 1
#include "../sdk/rocketnet_msgx_send_runs.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

// the instance: RNMsgxSendRuns with a bank of up to 24, three slots,
// generations 0 and 1, three clients
#define SR_SIZE 24
#define SR_CAP 3
#define SR_GENS 2
#define SR_CLIENTS 3
#define SR_STAMPS ( 1 + SR_CAP * SR_GENS ) // the shared socket's, then (slot, generation)
#define SR_MAX_STALLS 64
#define SR_MAX_RETIRES 1
#define SR_RETRIES_AFTER_QUIT 1
#define SR_SHARED -1
#define SR_NONE -1000                      // no fake answered

struct sr_info_t
{
    int packet_bytes;
    int table_slot;
    uint32_t generation;
    int client;                 // the walk's own: the destination
};

struct sr_walk_t
{
    tla_random_t random;

    // the bank, sorted, and the send's state
    sr_info_t info[SR_SIZE];
    int claimed;
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

    // the fakes' answers to the last call
    int sent_answer;
    int shared_answer;

    // the history (RNMsgxSendRuns left, lastC, lastS, bad)
    int left[SR_SIZE];
    int last_client[SR_CLIENTS + 1];
    int last_stamp[SR_CLIENTS + 1][SR_STAMPS];
    bool bad_cross;
    bool bad_client;
    bool bad_stamp;
    bool bad_dead;
    bool poisoned;

    // the environment and the bounds: main sets shutdown only on even
    // seeds, so odd seeds send their bank to the end; the first run whose
    // range holds an aborted packet is answered dead, so a dead run skips
    // one (DeadSkip)
    bool may_quit;
    bool dead_skip_sent;
    bool quit;
    int retries;
    int stalls;
    int retires;
};

// the generation each slot is open at when the send starts
static uint32_t sr_open_generation( int slot )
{
    return slot == 1 ? 0 : 1;
}

static int sr_stamp_index( int slot, uint32_t generation )
{
    return slot < 0 ? 0 : 1 + slot * SR_GENS + (int) generation;
}

// the client the slot's socket is connected to: the one stamped at the
// slot's open generation, 0 none (RNMsgxSendRuns Remote)
static int sr_remote( const sr_walk_t * walk, int slot )
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
    const int pick = walk->random.below( 12 );
    int answer = num_packets;
    bool aborted_inside = false;
    for ( int p = walk->state.cursor; p < walk->state.scan; p++ )
    {
        aborted_inside = aborted_inside || walk->info[walk->order[p]].packet_bytes <= 0;
    }
    if ( ( pick == 3 || pick == 4 || pick == 5 ) && num_packets >= 2 && sr_may_stall( walk ) )
    {
        answer = 1 + walk->random.below( num_packets - 1 );
    }
    else if ( pick == 6 && sr_may_stall( walk ) )
    {
        answer = ROCKETNET_MSGX_SEND_FULL;
    }
    else if ( pick == 7 && sr_may_stall( walk ) )
    {
        answer = 0;
    }
    else if ( pick == 8 || pick == 2 )
    {
        answer = ROCKETNET_MSGX_SEND_REFUSED;
    }
    else if ( pick == 1 || pick == 9 || pick == 10 )
    {
        answer = ROCKETNET_MSGX_SEND_DEAD;
    }
    else if ( pick == 11 )
    {
        answer = ROCKETNET_MSGX_SEND_ERROR;
    }
    if ( aborted_inside && !walk->dead_skip_sent )
    {
        answer = ROCKETNET_MSGX_SEND_DEAD;
        walk->dead_skip_sent = true;
    }
    const int remote = sr_remote( walk, slot );
    for ( int n = 0; n < answer; n++ )
    {
        sr_send_one( walk, packets[n], remote );
    }
    walk->sent_answer = answer;
    return answer;
}

static int sr_send_shared( void * user, int packet )
{
    sr_walk_t * walk = (sr_walk_t *) user;
    const int pick = walk->random.below( 6 );
    if ( pick == 4 )
    {
        walk->shared_answer = ROCKETNET_MSGX_SHARED_DROPPED;
    }
    else if ( pick == 5 && sr_may_stall( walk ) )
    {
        walk->shared_answer = ROCKETNET_MSGX_SHARED_FULL;
    }
    else
    {
        sr_send_one( walk, packet, SR_SHARED );
        walk->shared_answer = ROCKETNET_MSGX_SHARED_SENT;
    }
    return walk->shared_answer;
}

static void sr_report_dead( void * user, int table_slot, uint32_t generation )
{
    sr_walk_t * walk = (sr_walk_t *) user;
    (void) generation;
    walk->bad_dead = walk->bad_dead || table_slot < 0;
}

// a stamp for client c: a slot at a generation is one client's for the
// whole bank (owner), the shared socket anyone's
static int sr_pick_stamp( sr_walk_t * walk, int owner[SR_STAMPS], int c )
{
    for ( int tries = 0; tries < 16; tries++ )
    {
        const int st = walk->random.below( SR_STAMPS );
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

static void sr_stage( sr_walk_t * walk, int i, int c, int st )
{
    sr_info_t * info = &walk->info[i];
    info->client = c;
    info->table_slot = st == 0 ? SR_SHARED : ( st - 1 ) / SR_GENS;
    info->generation = st == 0 ? 0 : (uint32_t) ( ( st - 1 ) % SR_GENS );
    info->packet_bytes = walk->random.below( 4 ) == 0 ? 0 : 100;
}

// the bank: num_packets claimed 18..SR_SIZE + 1 (clamped at the size). The
// first six packets are each slot's pair (see the plant above): the slot's
// home client at its open generation, then the next client at the slot's
// other generation, both finished. The rest are any client's: most often its
// home slot (slot c - 1 at its open generation, so most runs resolve), the
// shared socket, or any stamp the owners allow (a client may hold two stamps,
// MultiStamp)
static void sr_make_bank( sr_walk_t * walk )
{
    walk->claimed = 18 + walk->random.below( SR_SIZE - 18 + 2 );
    walk->count = walk->claimed > SR_SIZE ? SR_SIZE : walk->claimed;

    int owner[SR_STAMPS];
    memset( owner, 0, sizeof( owner ) );
    for ( int c = 1; c <= SR_CLIENTS; c++ )
    {
        owner[sr_stamp_index( c - 1, sr_open_generation( c - 1 ) )] = c;
    }
    for ( int s = 0; s < SR_CAP; s++ )
    {
        const int home = s + 1;
        const int other = 1 + ( s + 1 ) % SR_CLIENTS;
        const int stale = sr_stamp_index( s, 1 - sr_open_generation( s ) );
        owner[stale] = other;
        sr_stage( walk, 2 * s, home, sr_stamp_index( s, sr_open_generation( s ) ) );
        sr_stage( walk, 2 * s + 1, other, stale );
        walk->info[2 * s].packet_bytes = 100;
        walk->info[2 * s + 1].packet_bytes = 100;
    }
    for ( int i = 2 * SR_CAP; i < walk->count; i++ )
    {
        const int c = 1 + walk->random.below( SR_CLIENTS );
        const int pick = walk->random.below( 4 );
        int st = sr_stamp_index( c - 1, sr_open_generation( c - 1 ) );
        if ( pick == 2 )
        {
            st = 0;
        }
        else if ( pick == 3 )
        {
            st = sr_pick_stamp( walk, owner, c );
        }
        sr_stage( walk, i, c, st );
    }
    for ( int i = walk->count; i < SR_SIZE; i++ )
    {
        walk->info[i].client = 0;
        walk->info[i].table_slot = SR_SHARED;
        walk->info[i].generation = 0;
        walk->info[i].packet_bytes = 0;
    }
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
    return "<<" + tla_int( ( st - 1 ) / SR_GENS ) + ", " + tla_int( ( st - 1 ) % SR_GENS ) + ">>";
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
    int left[SR_SIZE];
    std::vector<std::string> keys;
    std::vector<std::string> live;
    for ( int i = 0; i < SR_SIZE; i++ )
    {
        client[i] = walk->info[i].client;
        slot[i] = walk->info[i].table_slot;
        gen[i] = (int) walk->info[i].generation;
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

    std::vector<std::string> run;
    for ( int n = 0; n < walk->state.run; n++ )
    {
        run.push_back( tla_int( walk->state.run_packets[n] ) );
    }

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
    record.add( "live", tla_function( keys, live ) );
    record.add( "tOpen", tla_function( slot_keys, open ) );
    record.add( "tGen", sr_function( SR_CAP, table_generation ) );
    record.add( "order", sr_function( SR_SIZE, walk->order ) );
    record.add( "pc", sr_pc_name( walk ) );
    record.add( "cursor", tla_int( walk->state.cursor ) );
    record.add( "hd", tla_int( walk->state.head ) );
    record.add( "scan", tla_int( walk->state.scan ) );
    record.add( "run", tla_sequence( run ) );
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

// the action of a shared send by its answer, at HEAD or in the dead run
static const char * sr_shared_act( int answer, const char * sent, const char * dropped, const char * kept )
{
    if ( answer == ROCKETNET_MSGX_SHARED_SENT )
    {
        return sent;
    }
    return answer == ROCKETNET_MSGX_SHARED_DROPPED ? dropped : kept;
}

// the action a connected send's answer names (RNMsgxSendRuns SendAll ..
// SendError)
static const char * sr_send_act( int answer, int run )
{
    if ( answer >= run )
    {
        return "SendAll";
    }
    if ( answer > 0 )
    {
        return "SendPartial";
    }
    if ( answer == 0 || answer == ROCKETNET_MSGX_SEND_FULL )
    {
        return "SendFull";
    }
    if ( answer == ROCKETNET_MSGX_SEND_REFUSED )
    {
        return "SendRefused";
    }
    return answer == ROCKETNET_MSGX_SEND_DEAD ? "SendDead" : "SendError";
}

// one call of rocketnet_msgx_send_runs_step and the action its answer names
static const char * sr_step( sr_walk_t * walk, int batch, std::string * args )
{
    const int pc = walk->state.pc;
    const int run = walk->state.run;
    const int cursor_before = walk->state.cursor;
    const uint64_t dropped_before = walk->counters.dropped;
    walk->sent_answer = SR_NONE;
    walk->shared_answer = SR_NONE;
    rocketnet_msgx_send_runs_step( walk->info, walk->order, walk->count, batch, &walk->state, &walk->io, &walk->counters );
    if ( walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_KEPT && pc != ROCKETNET_MSGX_SEND_RUNS_KEPT )
    {
        walk->stalls++;
    }
    sr_attribute_drops( walk, cursor_before, walk->state.cursor, dropped_before );
    *args = "<<>>";
    if ( walk->gave_up || pc == ROCKETNET_MSGX_SEND_RUNS_DONE || pc == ROCKETNET_MSGX_SEND_RUNS_KEPT )
    {
        // KEPT and DONE take no step
        return "Done";
    }
    if ( pc == ROCKETNET_MSGX_SEND_RUNS_HEAD )
    {
        if ( walk->shared_answer != SR_NONE )
        {
            return sr_shared_act( walk->shared_answer, "HeadShared", "HeadDropped", "HeadKept" );
        }
        if ( walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_DONE )
        {
            return "HeadDone";
        }
        return walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_SEND ? "HeadRun" : "HeadSkip";
    }
    if ( pc == ROCKETNET_MSGX_SEND_RUNS_SEND )
    {
        const char * act = sr_send_act( walk->sent_answer, run );
        if ( strcmp( act, "SendPartial" ) == 0 )
        {
            *args = tla_sequence( { tla_int( walk->sent_answer ) } );
        }
        return act;
    }
    // DEAD
    if ( walk->shared_answer != SR_NONE )
    {
        return sr_shared_act( walk->shared_answer, "DeadShared", "DeadDropped", "DeadKept" );
    }
    return walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_HEAD ? "DeadDone" : "DeadSkip";
}

// the record of one call: the step, its act and the state after it
static void sr_push( std::vector<tla_record_t> & records, tla_report_t & report, const sr_walk_t * walk, const char * op, const char * act,
                     const std::string & args )
{
    records.push_back( tla_step( op, act, args, sr_record( walk ) ) );
    report.act( act );
}

static int trace_msgx_send_runs( uint64_t seed, int steps, const char * out )
{
    sr_walk_t * walk = new sr_walk_t();
    memset( walk, 0, sizeof( *walk ) );
    walk->random.state = seed;
    walk->may_quit = seed % 2 == 0;
    const int batch = 2 + walk->random.below( 2 );
    for ( int s = 0; s < SR_CAP; s++ )
    {
        walk->open[s] = true;
        walk->table_generation[s] = sr_open_generation( s );
    }
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

    sr_make_bank( walk );

    tla_report_t report;
    std::vector<tla_record_t> records;
    records.push_back( tla_step( "init", "init", "<<>>", sr_record( walk ) ) );

    // Sort: the order the code returns
    rocketnet_msgx_send_runs_sort( walk->info, walk->count, SR_CAP, walk->bucket, walk->order );
    walk->sorted = true;
    sr_push( records, report, walk, "sort", "Sort", "<<>>" );

    while ( (int) records.size() < steps + 1 )
    {
        const bool done = walk->gave_up || walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_DONE;
        if ( !done && walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_HEAD && walk->retires < SR_MAX_RETIRES && walk->random.below( 16 ) == 0 )
        {
            // Retire(s): main retires an open slot while the worker sends
            const int s = walk->random.below( SR_CAP );
            if ( walk->open[s] )
            {
                walk->open[s] = false;
                walk->retires++;
                sr_push( records, report, walk, "retire", "Retire", tla_sequence( { tla_int( s ) } ) );
                continue;
            }
        }
        if ( !done && walk->state.pc == ROCKETNET_MSGX_SEND_RUNS_KEPT )
        {
            if ( walk->may_quit && !walk->quit && walk->random.below( 4 ) == 0 )
            {
                // Quit: main sets shutdown
                walk->quit = true;
                sr_push( records, report, walk, "quit", "Quit", "<<>>" );
                continue;
            }
            // the worker's check after a kept call: give up, or resume
            if ( rocketnet_msgx_send_runs_give_up( walk->quit, &walk->retries, SR_RETRIES_AFTER_QUIT ) )
            {
                const uint64_t dropped_before = walk->counters.dropped;
                walk->counters.dropped += (uint64_t) rocketnet_msgx_send_runs_owed( walk->info, walk->order, walk->count, walk->state.cursor );
                sr_attribute_drops( walk, walk->state.cursor, walk->count, dropped_before );
                walk->gave_up = true;
                sr_push( records, report, walk, "check", "GiveUp", "<<>>" );
            }
            else
            {
                rocketnet_msgx_send_runs_resume( &walk->state );
                sr_push( records, report, walk, "check", "Resume", "<<>>" );
            }
            continue;
        }

        // one step of the send (at KEPT after a give-up, or DONE, no step)
        std::string args;
        const char * act = sr_step( walk, batch, &args );
        sr_push( records, report, walk, "step", act, args );
        if ( strcmp( act, "HeadRun" ) == 0 )
        {
            // the plant's effect: a run that holds a packet of another
            // generation than its head's
            for ( int p = walk->state.cursor; p < walk->state.scan; p++ )
            {
                if ( walk->info[walk->order[p]].generation != walk->info[walk->state.head].generation )
                {
                    report.planted( (int) records.size() );
                    break;
                }
            }
        }
    }

    report.print();

    // the bank, as RNMsgxSendRuns BankCode: one decimal digit per field
    // below the index
    std::string bank = "{";
    for ( int i = 0; i < walk->count; i++ )
    {
        const sr_info_t * info = &walk->info[i];
        const int64_t code = (int64_t) i * 10000 + info->client * 1000 + ( info->table_slot + 1 ) * 100 + (int64_t) info->generation * 10 + ( info->packet_bytes > 0 ? 1 : 0 );
        bank += ( i > 0 ? ", " : "" ) + tla_int( code );
    }
    bank += "}";
    std::string open_gen_at = "{";
    for ( int s = 0; s < SR_CAP; s++ )
    {
        open_gen_at += ( s > 0 ? ", " : "" ) + tla_int( (int64_t) s * SR_GENS + sr_open_generation( s ) );
    }
    open_gen_at += "}";

    char constants[4096];
    snprintf( constants, sizeof( constants ),
              "    Size = %d\n    Batch = %d\n    Cap = %d\n    Gens = {0, 1}\n    Clients = {1, 2, 3}\n"
              "    Claims = {%d}\n    MultiStamp = TRUE\n    MaxStalls = %d\n    MaxRetires = %d\n    RetriesAfterQuit = %d\n"
              "    StableSort = TRUE\n    GenBreak = TRUE\n    PartialSkipsAborted = TRUE\n    DeadResends = TRUE\n"
              "    OpenGenAt = %s\n    Bank = %s\n",
              SR_SIZE, batch, SR_CAP, walk->claimed, SR_MAX_STALLS, SR_MAX_RETIRES, SR_RETRIES_AFTER_QUIT, open_gen_at.c_str(), bank.c_str() );
    const bool written = tla_write_trace( out, "MCTraceMsgxSendRuns", seed, "TraceRNMsgxSendRuns", "", constants, records );
    delete walk;
    return written ? 0 : 1;
}

TLA_TRACE_LAYER( "msgxsendruns", trace_msgx_send_runs );
