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
// is a real thread, because the check's identity is the thread's token and
// its nesting depths are per thread. The walk is the scheduler: every actor
// parks in the code's own trace hook at the end of each step the model names
// (rocketnet_fill_phase_set_trace), and the walk releases one actor at a
// time, chosen by the seed, so a seed is one interleaving, run the same way
// every time. After each step it writes one record of every model variable:
// the code's own state (main_calls, the epoch, each client's claim word, the
// refusal counters) read from the fill phase, the control state from the
// step the hook names, and the contract's ghost (who claimed what in this
// phase, whether the contract was broken) by the model's own rules.
//
// The environment is free to break the contract, as in the model: the main
// thread and the fillers run at once, and two fillers may pick one client.
// Every refusal is then a step of the model too (flagged).
//
// The plant (ROCKETNET_FILL_PHASE_PLANT, the tla_trace_plant build) restores
// the stale-claim defect TLC found: the claim refuses only a word of the
// caller's own epoch, so a call that loaded the epoch before a main
// operation ended claims over a newer claim. PLANT-FIRED counts the claims
// that met that condition (another thread's word, not in, stamped with a
// later epoch than the caller's). The clean code meets it too and refuses,
// so the layer is not quiet.

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
#include "tla_trace_record.h"

#define FP_CLIENTS 2
#define FP_FILLERS 3
#define FP_MAX_MAIN 10
#define FP_MAX_FILLS 10
#define FP_ACTORS ( 1 + FP_FILLERS )

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
    FP_CLAIMED,
    FP_IN,
    FP_ANNOUNCED,
};

struct fp_step_t
{
    char action[16];
    int client_index;
    uint64_t word;
    bool refused;
};

struct fp_walk_t
{
    std::mutex mutex;
    std::condition_variable changed;
    fp_actor_state_t state[FP_ACTORS];
    int running;
    int next_client[FP_ACTORS];
    uint32_t token[FP_ACTORS];
    fp_step_t step[FP_ACTORS];

    struct rocketnet_fill_phase_t * phase;
    uint64_t overlaps;
    uint64_t two_fillers;

    // the model's view, kept by the walk from the steps the hook names
    fp_pc_t mpc;
    bool mscan[FP_CLIENTS];
    fp_pc_t fpc[FP_FILLERS];
    int fclient[FP_FILLERS];
    int fepoch[FP_FILLERS];
    bool phase_fillers[FP_CLIENTS][FP_FILLERS];
    bool violated;
    int mains_done;
    int fills_done[FP_FILLERS];
};

static thread_local int fp_this_actor = -1;

static std::string fp_filler_name( int filler )
{
    return "\"f" + tla_int( filler + 1 ) + "\"";
}

static std::string fp_pc_text( fp_pc_t pc )
{
    switch ( pc )
    {
        case FP_BEGUN:     return "\"begun\"";
        case FP_CLAIMED:   return "\"claimed\"";
        case FP_IN:        return "\"in\"";
        case FP_ANNOUNCED: return "\"announced\"";
        case FP_IDLE:      break;
    }
    return "\"idle\"";
}

static const char * fp_no_word = "[ep |-> 0, th |-> \"none\", in |-> FALSE]";

// the model's word of a claim: the code's epoch starts at 1, the model's at 0
static std::string fp_word_text( const fp_walk_t * walk, uint64_t word )
{
    const uint32_t thread = (uint32_t) word & ROCKETNET_FILL_PHASE_THREAD_MASK;
    if ( thread == 0 )
    {
        return fp_no_word;
    }
    std::string name = "\"unknown\"";
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        if ( walk->token[1 + f] == thread )
        {
            name = fp_filler_name( f );
        }
    }
    const int64_t epoch = (int64_t) ( word >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT ) - 1;
    return "[ep |-> " + tla_int( epoch ) + ", th |-> " + name + ", in |-> " + tla_bool( ( word & ROCKETNET_FILL_PHASE_IN ) != 0 ) + "]";
}

static tla_record_t fp_record( const fp_walk_t * walk )
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
    for ( int c = 0; c < FP_CLIENTS; c++ )
    {
        if ( walk->mscan[c] )
        {
            mscan += ( mscan.size() > 1 ? ", " : "" ) + tla_int( c );
        }
    }
    mscan += "}";

    std::vector<std::string> fpc, fclient, fepoch, seen, fills;
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        fpc.push_back( fp_pc_text( walk->fpc[f] ) );
        fclient.push_back( tla_int( walk->fclient[f] ) );
        fepoch.push_back( tla_int( walk->fepoch[f] ) );
        seen.push_back( fp_no_word );
        fills.push_back( tla_int( walk->fills_done[f] ) );
    }

    std::vector<std::string> words, phase_fillers;
    for ( int c = 0; c < FP_CLIENTS; c++ )
    {
        words.push_back( fp_word_text( walk, walk->phase->claims[c].load() ) );
        std::string set = "{";
        for ( int f = 0; f < FP_FILLERS; f++ )
        {
            if ( walk->phase_fillers[c][f] )
            {
                set += ( set.size() > 1 ? ", " : "" ) + fp_filler_name( f );
            }
        }
        phase_fillers.push_back( set + "}" );
    }

    tla_record_t record;
    record.add( "mpc", fp_pc_text( walk->mpc ) );
    record.add( "mscan", mscan );
    record.add( "fpc", tla_function( fillers, fpc ) );
    record.add( "fclient", tla_function( fillers, fclient ) );
    record.add( "fepoch", tla_function( fillers, fepoch ) );
    record.add( "seen", tla_function( fillers, seen ) );
    record.add( "mainCalls", tla_int( walk->phase->main_calls.load() ) );
    record.add( "epoch", tla_int( (int64_t) walk->phase->epoch.load() - 1 ) );
    record.add( "word", tla_function( clients, words ) );
    record.add( "flagged", tla_bool( walk->overlaps + walk->two_fillers > 0 ) );
    record.add( "phaseFillers", tla_function( clients, phase_fillers ) );
    record.add( "violated", tla_bool( walk->violated ) );
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

static bool fp_second( const fp_walk_t * walk, int f )
{
    for ( int g = 0; g < FP_FILLERS; g++ )
    {
        if ( g != f && walk->phase_fillers[walk->fclient[f]][g] )
        {
            return true;
        }
    }
    return false;
}

// one step of the model, the one the hook named: the control state and the
// ghost move by RNFillPhase's own rules, read on the state before the step
static bool fp_apply( fp_walk_t * walk, int actor, const fp_step_t * step )
{
    const bool main_busy = walk->mpc != FP_IDLE;
    const bool fill_busy = fp_fill_busy( walk );
    const std::string action = step->action;

    if ( actor == 0 )
    {
        walk->violated = walk->violated || fill_busy;
        if ( action == "MainAnnounce" )
        {
            walk->mpc = FP_ANNOUNCED;
            for ( int c = 0; c < FP_CLIENTS; c++ )
            {
                walk->mscan[c] = true;
            }
        }
        else if ( action == "MainLook" && step->refused )
        {
            walk->mpc = FP_IDLE;
            memset( walk->mscan, 0, sizeof( walk->mscan ) );
            walk->mains_done++;
        }
        else if ( action == "MainLook" )
        {
            walk->mscan[step->client_index] = false;
        }
        else if ( action == "MainIn" )
        {
            walk->mpc = FP_IN;
        }
        else if ( action == "MainLeave" )
        {
            walk->mpc = FP_IDLE;
            walk->mains_done++;
            memset( walk->phase_fillers, 0, sizeof( walk->phase_fillers ) );
        }
        else
        {
            return false;
        }
        return true;
    }

    const int f = actor - 1;
    walk->violated = walk->violated || main_busy;
    if ( action == "FillBegin" )
    {
        walk->fpc[f] = FP_BEGUN;
        walk->fclient[f] = step->client_index;
        walk->fepoch[f] = (int) step->word - 1;
    }
    else if ( action == "FillClaim" )
    {
        walk->violated = walk->violated || fp_second( walk, f );
        if ( step->refused )
        {
            walk->fpc[f] = FP_IDLE;
            walk->fills_done[f]++;
        }
        else
        {
            walk->fpc[f] = FP_CLAIMED;
            walk->phase_fillers[walk->fclient[f]][f] = true;
        }
    }
    else if ( action == "FillLook" )
    {
        walk->fpc[f] = step->refused ? FP_IDLE : FP_IN;
        walk->fills_done[f] += step->refused ? 1 : 0;
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
    walk->changed.wait( lock, [&]
                        { return walk->running == me; } );
    walk->state[me] = FP_RUNNING;
}

static void fp_park_ready( fp_walk_t * walk, int me )
{
    std::unique_lock<std::mutex> lock( walk->mutex );
    walk->state[me] = FP_READY;
    walk->running = -1;
    walk->changed.notify_all();
    walk->changed.wait( lock, [&]
                        { return walk->running == me; } );
    walk->state[me] = FP_RUNNING;
}

static void fp_actor( fp_walk_t * walk, int me )
{
    fp_this_actor = me;
    walk->token[me] = rocketnet_fill_phase_thread_token();
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
    // a main operation and another filler's whole call run inside its call:
    // the interleaving the stale-claim rule is for, which a uniform schedule
    // almost never makes. Holding only orders steps: every step is still the
    // code's own, and the record after it is still checked.
    int hold[FP_ACTORS] = {};

    // and the walk stages that interleaving outright, every time until it has
    // happened once and then one time in two: filler a
    // has loaded the epoch (stale), the main thread runs until it has left
    // one operation (stage 1), another filler b makes one whole call on a's
    // client (stage 2), then a goes on (stage 3). The stage only picks which
    // actor runs next; when the actor it wants is done it gives up.
    int stage = 0;
    int stale = -1;
    int other = -1;

    {
        std::unique_lock<std::mutex> lock( walk->mutex );
        walk->changed.wait( lock, [&]
                            {
            for ( int a = 0; a < FP_ACTORS; a++ )
            {
                if ( walk->state[a] != FP_READY )
                {
                    return false;
                }
            }
            return true; } );
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
            if ( stage == 1 )
            {
                a = walk->state[0] != FP_DONE ? 0 : -1;
            }
            else if ( stage == 2 )
            {
                a = walk->state[other] != FP_DONE ? other : -1;
            }
            else if ( stage == 3 )
            {
                a = stale;
                stage = 0;
            }
            if ( a < 0 )
            {
                stage = 0;
                continue;
            }
            for ( int b = 0; b < FP_ACTORS; b++ )
            {
                hold[b] -= hold[b] > 0 ? 1 : 0;
            }
            hold[a] = 0;

            if ( a > 0 && walk->state[a] == FP_READY )
            {
                walk->next_client[a] = stage == 2 ? walk->fclient[stale - 1] : random.below( FP_CLIENTS );
            }
            const bool other_was_ready = stage == 2 && walk->state[a] == FP_READY;

            // the plant's condition: a claim about to meet another thread's
            // word, not in, stamped with a later epoch than the caller loaded
            if ( a > 0 && walk->fpc[a - 1] == FP_BEGUN )
            {
                const uint64_t word = walk->phase->claims[walk->fclient[a - 1]].load();
                const uint32_t thread = (uint32_t) word & ROCKETNET_FILL_PHASE_THREAD_MASK;
                const int64_t ep = (int64_t) ( word >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT ) - 1;
                if ( thread != 0 && thread != walk->token[a] && !( word & ROCKETNET_FILL_PHASE_IN ) && ep > walk->fepoch[a - 1] )
                {
                    fired++;
                }
            }

            walk->running = a;
            walk->changed.notify_all();
            walk->changed.wait( lock, [&]
                                { return walk->running == -1; } );

            if ( walk->state[a] == FP_STEP )
            {
                if ( !fp_apply( walk, a, &walk->step[a] ) )
                {
                    unknown = true;
                }
                records.push_back( fp_record( walk ) );
                if ( a > 0 && walk->fpc[a - 1] == FP_BEGUN && random.below( 4 ) != 0 )
                {
                    hold[a] = 8 + random.below( 16 );
                }
                if ( stage == 0 && a > 0 && walk->fpc[a - 1] == FP_BEGUN && ( fired == 0 || random.below( 2 ) == 0 ) )
                {
                    // stage the stale claim: the main thread first, then
                    // another filler that is between calls
                    int candidates[FP_FILLERS];
                    int num_candidates = 0;
                    for ( int g = 1; g < FP_ACTORS; g++ )
                    {
                        if ( g != a && walk->state[g] == FP_READY )
                        {
                            candidates[num_candidates++] = g;
                        }
                    }
                    if ( num_candidates > 0 && walk->state[0] != FP_DONE )
                    {
                        stale = a;
                        other = candidates[random.below( num_candidates )];
                        stage = 1;
                    }
                }
                else if ( stage == 1 && a == 0 && std::string( walk->step[0].action ) == "MainLeave" )
                {
                    stage = 2;      // one main operation left: the epoch moved under a's call
                }
            }
            else if ( stage == 2 && a == other && !other_was_ready )
            {
                stage = 3;          // b's call ended: it parked before its next call, or finished
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
                walk->changed.wait( lock, [&]
                                    { return walk->running == -1; } );
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

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    Fillers = {\"f1\", \"f2\", \"f3\"}\n"
              "    Clients = {0, 1}\n"
              "    NoOwner = \"none\"\n"
              "    MaxMain = %d\n"
              "    MaxFills = %d\n"
              "    LookFirst = FALSE\n"
              "    LoadStore = FALSE\n"
              "    ResetOnLeave = TRUE\n"
              "    StaleClaim = FALSE\n"
              "    NoEpochLook = FALSE\n",
              FP_MAX_MAIN, FP_MAX_FILLS );
    const bool written = tla_write_trace( out, name, "TraceRNFillPhase", "", constants, records );

    delete walk->phase;
    delete walk;
    return written && !unknown ? 0 : 1;
}

TLA_TRACE_LAYER( "fillphase", trace_fill_phase );
