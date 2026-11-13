/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the fill-phase check (proofs/tla/README-TraceCheck.md,
// proofs/tla/RNFillPhase.tla, proofs/tla/TraceRNFillPhase.tla).
//
// The code under test is sdk/rocketnet_fill_phase.cpp, driven the way the
// transport drives it: one main thread makes main-thread operations
// (main_begin, then main_end when it entered) and filler threads make fill
// calls (fill_begin on a client, then fill_end when it entered). Each actor
// is a real thread, because the check names a thread by its id
// (rocketnet_platform_thread_id). The walk is the scheduler: every actor
// parks in the code's own trace hook at the end of each step the model names
// (rocketnet_fill_phase_set_trace), and the walk releases one actor at a
// time, chosen by the seed, so a seed is one interleaving, run the same way
// every time. After each step it writes one record of every model variable:
// the code's own state (main_calls, each fills_in line, the epoch, each
// client's word and owner) read from the fill phase, the control state and the words a
// call saved from the step the hook names, and the contract's ghost (who
// passed on what in this phase, which calls met a main operation, whether a
// refusal was of an honest call) by the model's own rules.
//
// The environment is free to break the contract, as in the model: the main
// thread and the fillers run at once, and two fillers may pick one client.
// Every refusal is then a step of the model too.
//
// The plant (ROCKETNET_FILL_PHASE_PLANT, the tla_trace_plant build) restores
// the defect MCRNFillPhaseBrokenNoRestore names: a refused look leaves its
// claim, out, in place of the word and owner it replaced. PLANT-FIRED counts
// the refused looks on a client where that differs from the restore. The
// walk steers toward them: after a filler counts itself in, the main thread
// often runs next, so the filler's look finds it.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_FILL_PHASE_PLANT 1
#include "../sdk/rocketnet_fill_phase.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <condition_variable>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_fill_phase.h"
#include "../sdk/rocketnet_platform.h"
#include "tla_trace_record.h"

#define FP_CLIENTS 2
#define FP_FILLERS 3
#define FP_MAX_MAIN 10
#define FP_MAX_FILLS 10
#define FP_ACTORS ( 1 + FP_FILLERS )
#define FP_LINES ROCKETNET_FILL_PHASE_FILL_LINES

// an actor between steps: parked before its next call (READY), parked in the
// hook at the end of a step (STEP), running, or done with its calls
enum fp_actor_state_t
{
    FP_READY,
    FP_STEP,
    FP_RUNNING,
    FP_DONE,
};

// the model's control state of one filler and of the main thread
enum fp_pc_t
{
    FP_IDLE,
    FP_BEGUN,
    FP_LOCKED,
    FP_DECIDED,
    FP_COUNTED,
    FP_IN,
    FP_UNCLAIMED,
    FP_ANNOUNCED,
    FP_EPOCHED,
};

struct fp_step_t
{
    char action[16];
    int client_index;
    uint64_t word;
    bool refused;
};

// a client's word as the model sees it
struct fp_word_t
{
    uint64_t ep;
    bool held;
    bool in;
};

struct fp_walk_t
{
    std::mutex mutex;
    std::condition_variable changed;
    fp_actor_state_t state[FP_ACTORS];
    int running;
    int next_client[FP_ACTORS];
    uint64_t id[FP_ACTORS];
    fp_step_t step[FP_ACTORS];

    struct rocketnet_fill_phase_t * phase;
    uint64_t overlaps;
    uint64_t two_fillers;

    // the owners as the last record read them: a decide's saved owner
    int last_owner[FP_CLIENTS];

    // the model's view, kept by the walk from the steps the hook names
    fp_pc_t mpc;
    bool mscan[FP_LINES];
    fp_pc_t fpc[FP_FILLERS];
    int fclient[FP_FILLERS];
    uint64_t fepoch[FP_FILLERS];
    fp_word_t seen[FP_FILLERS];
    int saved[FP_FILLERS];
    bool fbroke[FP_FILLERS];
    bool phase_in[FP_CLIENTS][FP_FILLERS];
    bool false_alarm;
    int mains_done;
    int fills_done[FP_FILLERS];
};

static thread_local int fp_this_actor = -1;

static std::string fp_filler_name( int filler )
{
    return "\"f" + tla_int( filler + 1 ) + "\"";
}

static std::string fp_owner_name( int filler )
{
    return filler < 0 ? "\"none\"" : fp_filler_name( filler );
}

static std::string fp_pc_text( fp_pc_t pc )
{
    switch ( pc )
    {
        case FP_BEGUN: return "\"begun\"";
        case FP_LOCKED: return "\"locked\"";
        case FP_DECIDED: return "\"decided\"";
        case FP_COUNTED: return "\"counted\"";
        case FP_IN: return "\"in\"";
        case FP_UNCLAIMED: return "\"unclaimed\"";
        case FP_ANNOUNCED: return "\"announced\"";
        case FP_EPOCHED: return "\"epoched\"";
        case FP_IDLE: break;
    }
    return "\"idle\"";
}

static fp_word_t fp_word( uint64_t word )
{
    fp_word_t w;
    w.ep = word >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT;
    w.held = ( word & ROCKETNET_FILL_PHASE_DEPTH_MASK ) != 0;
    w.in = ( word & ROCKETNET_FILL_PHASE_IN ) != 0;
    return w;
}

static std::string fp_word_text( fp_word_t w )
{
    return "[ep |-> " + tla_int( (int64_t) w.ep ) + ", held |-> " + tla_bool( w.held ) + ", in |-> " + tla_bool( w.in ) + "]";
}

// the filler whose thread id this is, -1 for 0, -2 for an id no filler has
static int fp_owner( const fp_walk_t * walk, uint64_t id )
{
    if ( id == 0 )
    {
        return -1;
    }
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        if ( walk->id[1 + f] == id )
        {
            return f;
        }
    }
    return -2;
}

static tla_record_t fp_record( fp_walk_t * walk )
{
    std::vector<std::string> fillers, clients;
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        fillers.push_back( fp_filler_name( f ) );
    }
    for ( int c = 0; c < FP_CLIENTS; c++ )
    {
        clients.push_back( tla_int( c ) );
    }

    std::string mscan = "{";
    std::vector<std::string> lines, fills_in;
    for ( int l = 0; l < FP_LINES; l++ )
    {
        if ( walk->mscan[l] )
        {
            mscan += ( mscan.size() > 1 ? ", " : "" ) + tla_int( l );
        }
        lines.push_back( tla_int( l ) );
        fills_in.push_back( tla_int( walk->phase->lines[l].fills_in.load() ) );
    }
    mscan += "}";

    std::vector<std::string> fpc, fclient, fepoch, seen, saved, fbroke, fills;
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        fpc.push_back( fp_pc_text( walk->fpc[f] ) );
        fclient.push_back( tla_int( walk->fclient[f] ) );
        fepoch.push_back( tla_int( (int64_t) walk->fepoch[f] ) );
        seen.push_back( fp_word_text( walk->seen[f] ) );
        saved.push_back( fp_owner_name( walk->saved[f] ) );
        fbroke.push_back( tla_bool( walk->fbroke[f] ) );
        fills.push_back( tla_int( walk->fills_done[f] ) );
    }

    std::vector<std::string> words, owners, phase_in;
    for ( int c = 0; c < FP_CLIENTS; c++ )
    {
        words.push_back( fp_word_text( fp_word( walk->phase->clients[c].claim.load() ) ) );
        walk->last_owner[c] = fp_owner( walk, walk->phase->clients[c].owner.load() );
        owners.push_back( walk->last_owner[c] == -2 ? "\"unknown\"" : fp_owner_name( walk->last_owner[c] ) );
        std::string set = "{";
        for ( int f = 0; f < FP_FILLERS; f++ )
        {
            if ( walk->phase_in[c][f] )
            {
                set += ( set.size() > 1 ? ", " : "" ) + fp_filler_name( f );
            }
        }
        phase_in.push_back( set + "}" );
    }

    tla_record_t record;
    record.add( "mpc", fp_pc_text( walk->mpc ) );
    record.add( "mscan", mscan );
    record.add( "fpc", tla_function( fillers, fpc ) );
    record.add( "fclient", tla_function( fillers, fclient ) );
    record.add( "fepoch", tla_function( fillers, fepoch ) );
    record.add( "seen", tla_function( fillers, seen ) );
    record.add( "saved", tla_function( fillers, saved ) );
    record.add( "mainCalls", tla_int( walk->phase->main_calls.load() ) );
    record.add( "fillsIn", tla_function( lines, fills_in ) );
    record.add( "epoch", tla_int( (int64_t) walk->phase->epoch.load() ) );
    record.add( "word", tla_function( clients, words ) );
    record.add( "owner", tla_function( clients, owners ) );
    record.add( "fbroke", tla_function( fillers, fbroke ) );
    record.add( "phaseIn", tla_function( clients, phase_in ) );
    record.add( "falseAlarm", tla_bool( walk->false_alarm ) );
    record.add( "mainsDone", tla_int( walk->mains_done ) );
    record.add( "fillsDone", tla_function( fillers, fills ) );
    return record;
}

static bool fp_fill_busy( const fp_walk_t * walk )
{
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        if ( walk->fpc[f] != FP_IDLE )
        {
            return true;
        }
    }
    return false;
}

// RNFillPhase Justified(f), on the state before the step
static bool fp_justified( const fp_walk_t * walk, int f )
{
    if ( walk->fbroke[f] || walk->mpc != FP_IDLE )
    {
        return true;
    }
    const int c = walk->fclient[f];
    for ( int g = 0; g < FP_FILLERS; g++ )
    {
        if ( g != f && ( walk->phase_in[c][g] || ( walk->fpc[g] != FP_IDLE && walk->fclient[g] == c ) ) )
        {
            return true;
        }
    }
    return false;
}

static void fp_fill_refused( fp_walk_t * walk, int f )
{
    walk->false_alarm = walk->false_alarm || !fp_justified( walk, f );
    walk->fpc[f] = FP_IDLE;
    walk->fills_done[f]++;
}

// one step of the model, the one the hook named: the control state and the
// ghost move by RNFillPhase's own rules, read on the state before the step
static bool fp_apply( fp_walk_t * walk, int actor, const fp_step_t * step )
{
    const bool main_busy = walk->mpc != FP_IDLE;
    const std::string action = step->action;

    if ( actor == 0 )
    {
        // MainMeets
        for ( int g = 0; g < FP_FILLERS; g++ )
        {
            walk->fbroke[g] = walk->fbroke[g] || walk->fpc[g] != FP_IDLE;
        }
        if ( action == "MainAnnounce" && !step->refused )
        {
            walk->mpc = FP_ANNOUNCED;
            for ( int l = 0; l < FP_LINES; l++ )
            {
                walk->mscan[l] = true;
            }
        }
        else if ( action == "MainLook" && step->refused )
        {
            walk->false_alarm = walk->false_alarm || !fp_fill_busy( walk );
            walk->mpc = FP_IDLE;
            memset( walk->mscan, 0, sizeof( walk->mscan ) );
            walk->mains_done++;
        }
        else if ( action == "MainLook" && step->client_index >= 0 && step->client_index < FP_LINES )
        {
            walk->mscan[step->client_index] = false;
        }
        else if ( action == "MainIn" )
        {
            walk->mpc = FP_IN;
        }
        else if ( action == "MainEpoch" )
        {
            walk->mpc = FP_EPOCHED;
            memset( walk->phase_in, 0, sizeof( walk->phase_in ) );
        }
        else if ( action == "MainDrop" )
        {
            walk->mpc = FP_IDLE;
            walk->mains_done++;
        }
        else
        {
            return false;
        }
        return true;
    }

    const int f = actor - 1;
    if ( action == "FillBegin" )
    {
        walk->fpc[f] = FP_BEGUN;
        walk->fclient[f] = step->client_index;
        walk->fepoch[f] = step->word;
        walk->fbroke[f] = main_busy;
        return true;
    }

    // FillMeets, after the refusal's justification is read on the state before
    const bool broke = walk->fbroke[f] || main_busy;
    const int c = walk->fclient[f];
    if ( action == "FillLock" && step->refused )
    {
        fp_fill_refused( walk, f );
    }
    else if ( action == "FillLock" )
    {
        walk->seen[f] = fp_word( step->word & ~ROCKETNET_FILL_PHASE_IN );
        walk->fpc[f] = FP_LOCKED;
    }
    else if ( action == "FillDecide" && step->refused )
    {
        fp_fill_refused( walk, f );
    }
    else if ( action == "FillDecide" )
    {
        walk->saved[f] = walk->last_owner[c];
        walk->fpc[f] = FP_DECIDED;
    }
    else if ( action == "FillCount" )
    {
        walk->fpc[f] = FP_COUNTED;
    }
    else if ( action == "FillLook" && step->refused )
    {
        fp_fill_refused( walk, f );
    }
    else if ( action == "FillLook" )
    {
        walk->fpc[f] = FP_IN;
        walk->phase_in[c][f] = true;
    }
    else if ( action == "FillUnclaim" )
    {
        walk->fpc[f] = FP_UNCLAIMED;
    }
    else if ( action == "FillLeave" )
    {
        walk->fpc[f] = FP_IDLE;
        walk->fills_done[f]++;
    }
    else
    {
        return false;
    }
    walk->fbroke[f] = broke;
    return true;
}

static void fp_hook( void * context, const char * action, int client_index, uint64_t word, bool refused )
{
    fp_walk_t * walk = (fp_walk_t *) context;
    const int me = fp_this_actor;
    std::unique_lock<std::mutex> lock( walk->mutex );
    fp_step_t * step = &walk->step[me];
    snprintf( step->action, sizeof( step->action ), "%s", action );
    step->client_index = client_index;
    step->word = word;
    step->refused = refused;
    walk->state[me] = FP_STEP;
    walk->running = -1;
    walk->changed.notify_all();
    walk->changed.wait( lock, [&] { return walk->running == me; } );
    walk->state[me] = FP_RUNNING;
}

static void fp_park_ready( fp_walk_t * walk, int me )
{
    std::unique_lock<std::mutex> lock( walk->mutex );
    walk->state[me] = FP_READY;
    walk->running = -1;
    walk->changed.notify_all();
    walk->changed.wait( lock, [&] { return walk->running == me; } );
    walk->state[me] = FP_RUNNING;
}

static void fp_actor( fp_walk_t * walk, int me )
{
    fp_this_actor = me;
    {
        std::unique_lock<std::mutex> lock( walk->mutex );
        walk->id[me] = rocketnet_platform_thread_id();
    }
    const int calls = me == 0 ? FP_MAX_MAIN : FP_MAX_FILLS;
    for ( int call = 0; call < calls; call++ )
    {
        fp_park_ready( walk, me );
        if ( me == 0 )
        {
            if ( rocketnet_fill_phase_main_begin( walk->phase ) )
            {
                rocketnet_fill_phase_main_end( walk->phase );
            }
        }
        else
        {
            const int client = walk->next_client[me];
            if ( rocketnet_fill_phase_fill_begin( walk->phase, client ) )
            {
                rocketnet_fill_phase_fill_end( walk->phase, client );
            }
        }
    }
    std::unique_lock<std::mutex> lock( walk->mutex );
    walk->state[me] = FP_DONE;
    walk->running = -1;
    walk->changed.notify_all();
}

// a refusal asserts in a debug build: here it is the step under test
static void fp_assert( const char *, const char *, const char *, int )
{
}

static int trace_fill_phase( uint64_t seed, int steps, const char * out )
{
    fp_walk_t * walk = new fp_walk_t();
    walk->phase = new rocketnet_fill_phase_t();
    rocketnet_fill_phase_init( walk->phase, FP_CLIENTS, &walk->overlaps, &walk->two_fillers );
    walk->running = -1;
    for ( int a = 0; a < FP_ACTORS; a++ )
    {
        walk->state[a] = FP_RUNNING;
    }
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        walk->saved[f] = -1;
    }
    for ( int c = 0; c < FP_CLIENTS; c++ )
    {
        walk->last_owner[c] = -1;
    }

    rocketnet_assert_function( fp_assert );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_fill_phase_set_trace( fp_hook, walk );

    std::vector<std::thread> actors;
    for ( int a = 0; a < FP_ACTORS; a++ )
    {
        actors.emplace_back( fp_actor, walk, a );
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    int fired = 0;
    bool unknown = false;

    // a filler that has loaded the epoch is often held back for a while, so
    // a main operation and other fillers' whole calls run inside its call (a
    // stale call), which a uniform schedule rarely makes; and after a filler
    // counts itself in, the main thread often runs next, so the filler's look
    // finds it (the plant's condition). Steering only orders steps: every
    // step is still the code's own, and the record after it is still checked.
    int hold[FP_ACTORS] = {};
    int then = -1;                                  // the actor the steering picks next

    {
        std::unique_lock<std::mutex> lock( walk->mutex );
        walk->changed.wait( lock, [&] {
            for ( int a = 0; a < FP_ACTORS; a++ )
            {
                if ( walk->state[a] != FP_READY )
                {
                    return false;
                }
            }
            return true;
        } );
        records.push_back( fp_record( walk ) );

        for ( int pick = 0; pick < steps; pick++ )
        {
            int live[FP_ACTORS];
            int num_live = 0;
            int free_live[FP_ACTORS];
            int num_free = 0;
            for ( int a = 0; a < FP_ACTORS; a++ )
            {
                if ( walk->state[a] != FP_DONE )
                {
                    live[num_live++] = a;
                    if ( hold[a] == 0 )
                    {
                        free_live[num_free++] = a;
                    }
                }
            }
            if ( num_live == 0 )
            {
                break;
            }
            int a = num_free > 0 ? free_live[random.below( num_free )] : live[random.below( num_live )];
            if ( then >= 0 && walk->state[then] != FP_DONE )
            {
                a = then;
            }
            then = -1;
            for ( int b = 0; b < FP_ACTORS; b++ )
            {
                hold[b] -= hold[b] > 0 ? 1 : 0;
            }
            hold[a] = 0;

            if ( a > 0 && walk->state[a] == FP_READY )
            {
                walk->next_client[a] = random.below( FP_CLIENTS );
            }

            walk->running = a;
            walk->changed.notify_all();
            walk->changed.wait( lock, [&] { return walk->running == -1; } );

            if ( walk->state[a] != FP_STEP )
            {
                continue;
            }

            const fp_step_t * step = &walk->step[a];
            const std::string action = step->action;
            if ( a > 0 && action == "FillLook" && step->refused )
            {
                // the plant's condition: the restore differs from the claim left in place
                const int f = a - 1;
                const fp_word_t left = { walk->fepoch[f], false, false };
                const fp_word_t back = walk->seen[f];
                if ( back.ep != left.ep || back.held || back.in || walk->saved[f] != f )
                {
                    fired++;
                }
            }

            if ( !fp_apply( walk, a, step ) )
            {
                unknown = true;
            }
            records.push_back( fp_record( walk ) );

            if ( a > 0 && action == "FillBegin" && random.below( 4 ) != 0 )
            {
                hold[a] = 8 + random.below( 16 );
            }
            if ( a > 0 && action == "FillCount" && ( fired == 0 || random.below( 2 ) == 0 ) )
            {
                then = 0;                           // the main thread, then this filler's look
            }
            else if ( a == 0 && action == "MainAnnounce" )
            {
                for ( int f = 0; f < FP_FILLERS; f++ )
                {
                    if ( walk->fpc[f] == FP_COUNTED )
                    {
                        then = 1 + f;
                    }
                }
            }
        }
    }

    bool finished = true;
    for ( int a = 0; a < FP_ACTORS; a++ )
    {
        finished = finished && walk->state[a] == FP_DONE;
    }
    if ( !finished )
    {
        // the step budget ended first: let every actor run to its end, unrecorded
        rocketnet_fill_phase_set_trace( NULL, NULL );
        std::unique_lock<std::mutex> lock( walk->mutex );
        for ( int a = 0; a < FP_ACTORS; a++ )
        {
            while ( walk->state[a] != FP_DONE )
            {
                walk->running = a;
                walk->changed.notify_all();
                walk->changed.wait( lock, [&] { return walk->running == -1; } );
            }
        }
    }
    for ( size_t i = 0; i < actors.size(); i++ )
    {
        actors[i].join();
    }
    rocketnet_fill_phase_set_trace( NULL, NULL );
    rocketnet_assert_function( NULL );

    printf( "PLANT-FIRED %d\n", fired );
    if ( unknown )
    {
        fprintf( stderr, "tla_trace fill_phase: the hook named a step the model does not have\n" );
    }

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceFillPhasePlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceFillPhaseSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    // each filler's fills_in line, from its thread's id
    std::string definitions = "TraceLines == 0.." + tla_int( FP_LINES - 1 ) + "\nTraceLineOf == (";
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        definitions += ( f > 0 ? " @@ " : "" ) + fp_filler_name( f ) + " :> " + tla_int( rocketnet_fill_phase_line_of( walk->id[1 + f] ) );
    }
    definitions += ")\n";

    char constants[640];
    snprintf( constants, sizeof( constants ),
              "    Fillers = {\"f1\", \"f2\", \"f3\"}\n"
              "    Clients = {0, 1}\n"
              "    NoOwner = \"none\"\n"
              "    MaxMain = %d\n"
              "    MaxFills = %d\n"
              "    LookFirst = FALSE\n"
              "    LoadStore = FALSE\n"
              "    ResetOnLeave = TRUE\n"
              "    NoEpochLook = FALSE\n"
              "    NoRestore = FALSE\n"
              "    ScanOne = FALSE\n"
              "    Lines <- TraceLines\n"
              "    LineOf <- TraceLineOf\n",
              FP_MAX_MAIN, FP_MAX_FILLS );
    const bool written = tla_write_trace( out, name, "TraceRNFillPhase", definitions, constants, records );

    delete walk->phase;
    delete walk;
    return written && !unknown ? 0 : 1;
}

TLA_TRACE_LAYER( "fillphase", trace_fill_phase );
