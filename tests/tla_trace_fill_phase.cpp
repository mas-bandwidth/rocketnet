/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the fill-phase check (proofs/tla/README-TraceCheck.md,
// proofs/tla/RNFillPhase.tla, proofs/tla/TraceRNFillPhase.tla).
//
// The code under test is sdk/rocketnet_fill_phase.cpp, driven the way the
// transport drives it: one main thread makes main-thread operations
// (main_begin, then main_end when it entered), each an update call that
// opens the window (update_receive) or shuts it (update_finish) or another
// operation (a send, a stop), and filler threads make fill calls (fill_begin
// on a client, then fill_end when it entered) and takes (update_client:
// take_begin, then take_end when it entered). Each actor
// is a real thread, because the check names a thread by its id
// (rocketnet_platform_thread_id). The walk is the scheduler: every actor
// parks in the code's own trace hook at the end of each step the model names
// (rocketnet_fill_phase_set_trace), and the walk releases one actor at a
// time, chosen by the seed, so a seed is one interleaving, run the same way
// every time. Each filler counts on a fills_in line the seed picks
// (rocketnet_fill_phase_test_set_line, in place of its thread id's hash, an
// address that differs run to run); a seed sometimes gives two fillers one
// line.
//
// One record per step (tla_step, record format v2): the call the thread is
// in (main_begin, main_end, fill_begin, fill_end), the step the code's hook
// named (the action), its arguments with what the step gave back (the line
// a look read or a count took, ok for a look, lock or decide that let the
// call go on), and every model variable after it: the code's own state
// (main_calls, the window, each fills_in line, the epoch, each client's word
// and owner, the two refusal counters) read from the fill phase, the control
// state and the kind of the main operation, the line each call counted
// on and the words a call saved from the steps the hook names, and the
// contract's ghost (who passed on what in this phase, which calls met a main
// operation, whether a refusal was of an honest call) by the model's own
// rules.
//
// The environment is free to break the contract, as in the model: the main
// thread and the fillers run at once, and two fillers may pick one client.
// Every refusal is then a step of the model too.
//
// The plant (ROCKETNET_FILL_PHASE_PLANT, tla_trace_plant_fill_phase)
// restores the defect MCRNFillPhaseBrokenNoRestore names: a refused look
// leaves its claim, out, in place of the word and owner it replaced. The
// walk reports it (PLANT-AT) at each refused look on a client where that
// differs from the restore, the record where the word shows it. The walk
// steers toward them: after a filler counts itself in, the main thread
// often runs next, so the filler's look finds it.

#if defined( ROCKETNET_TLA_PLANT )
#include "../sdk/rocketnet_fill_phase.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <condition_variable>
#include <mutex>
#include <new>
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

// the instance: two clients, three fillers, every fills_in line of the
// code. Each actor may make as many calls as the walk has steps, so the
// walk never runs out of calls before its last step (the model's Done is
// never reached)
#define FP_CLIENTS 2
#define FP_FILLERS 3
#define FP_ACTORS ( 1 + FP_FILLERS )
#define FP_LINES ROCKETNET_FILL_PHASE_FILL_LINES
#define FP_NO_LINE -1
#define FP_ARM_AT  12  // the records before the walk arms the check: whole calls skipped

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
    FP_TCOUNTED,
    FP_TIN,
};

// a main operation's kind: what the walk's main thread calls next, and the
// model's mkind ("update" for the update calls, "other" for the rest)
enum fp_main_kind_t
{
    FP_KIND_NONE,
    FP_KIND_RECEIVE,                                // an update call whose work opens the window
    FP_KIND_FINISH,                                 // an update call whose work shuts it
    FP_KIND_OTHER,                                  // send_packets, stop ...: refused while the window is open
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
    bool next_take[FP_ACTORS];                      // a filler's next call is a take
    fp_main_kind_t next_kind;                       // the main thread's next operation
    uint64_t id[FP_ACTORS];
    fp_step_t step[FP_ACTORS];

    struct rocketnet_fill_phase_t * phase;
    uint64_t overlaps;
    uint64_t two_fillers;
    int calls;                                      // the calls each actor may make
    int line[FP_FILLERS];                           // each filler's fills_in line, picked by the seed

    // the owners as the last record read them: a decide's saved owner
    int last_owner[FP_CLIENTS];

    // the model's view, kept by the walk from the steps the hook names
    fp_pc_t mpc;
    fp_main_kind_t mkind;
    bool mscan[FP_LINES];
    fp_pc_t fpc[FP_FILLERS];
    int fclient[FP_FILLERS];
    uint64_t fepoch[FP_FILLERS];
    fp_word_t seen[FP_FILLERS];
    int saved[FP_FILLERS];
    int fline[FP_FILLERS];
    bool fbroke[FP_FILLERS];
    bool phase_in[FP_CLIENTS][FP_FILLERS];
    bool false_alarm;
    int mains_done;
    int fills_done[FP_FILLERS];
    int takes_done[FP_FILLERS];

    // the latch's ghost: the game has named a client (the walk's own arm step)
    bool opted;
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
        case FP_BEGUN:     return "\"begun\"";
        case FP_LOCKED:    return "\"locked\"";
        case FP_DECIDED:   return "\"decided\"";
        case FP_COUNTED:   return "\"counted\"";
        case FP_IN:        return "\"in\"";
        case FP_UNCLAIMED: return "\"unclaimed\"";
        case FP_ANNOUNCED: return "\"announced\"";
        case FP_EPOCHED:   return "\"epoched\"";
        case FP_TCOUNTED:  return "\"tcounted\"";
        case FP_TIN:       return "\"tin\"";
        case FP_IDLE:      break;
    }
    return "\"idle\"";
}

static std::string fp_kind_text( fp_main_kind_t kind )
{
    switch ( kind )
    {
        case FP_KIND_RECEIVE:
        case FP_KIND_FINISH:  return "\"update\"";
        case FP_KIND_OTHER:   return "\"other\"";
        case FP_KIND_NONE:    break;
    }
    return "\"none\"";
}

static bool fp_window( const fp_walk_t * walk )
{
    return walk->phase->window.load() != 0;
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

// every model variable, as the code and the walk hold them after a step
static tla_record_t fp_state( fp_walk_t * walk )
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

    std::vector<std::string> fpc, fclient, fepoch, seen, saved, fline, fbroke, fills, takes;
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        fpc.push_back( fp_pc_text( walk->fpc[f] ) );
        fclient.push_back( tla_int( walk->fclient[f] ) );
        fepoch.push_back( tla_int( (int64_t) walk->fepoch[f] ) );
        seen.push_back( fp_word_text( walk->seen[f] ) );
        saved.push_back( fp_owner_name( walk->saved[f] ) );
        fline.push_back( tla_int( walk->fline[f] ) );
        fbroke.push_back( tla_bool( walk->fbroke[f] ) );
        fills.push_back( tla_int( walk->fills_done[f] ) );
        takes.push_back( tla_int( walk->takes_done[f] ) );
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
    record.add( "mkind", fp_kind_text( walk->mkind ) );
    record.add( "fpc", tla_function( fillers, fpc ) );
    record.add( "fclient", tla_function( fillers, fclient ) );
    record.add( "fepoch", tla_function( fillers, fepoch ) );
    record.add( "seen", tla_function( fillers, seen ) );
    record.add( "saved", tla_function( fillers, saved ) );
    record.add( "mainCalls", tla_int( walk->phase->main_calls.load() ) );
    record.add( "window", tla_bool( fp_window( walk ) ) );
    record.add( "fillsIn", tla_function( lines, fills_in ) );
    record.add( "fline", tla_function( fillers, fline ) );
    record.add( "epoch", tla_int( (int64_t) walk->phase->epoch.load() ) );
    record.add( "word", tla_function( clients, words ) );
    record.add( "owner", tla_function( clients, owners ) );
    record.add( "armed", tla_bool( rocketnet_fill_phase_armed( walk->phase ) ) );
    // the counters the check counts its refusals on (overlap_counter, two_fillers_counter)
    record.add( "overlaps", tla_int( (int64_t) walk->overlaps ) );
    record.add( "twoFillers", tla_int( (int64_t) walk->two_fillers ) );
    record.add( "opted", tla_bool( walk->opted ) );
    record.add( "fbroke", tla_function( fillers, fbroke ) );
    record.add( "phaseIn", tla_function( clients, phase_in ) );
    record.add( "falseAlarm", tla_bool( walk->false_alarm ) );
    record.add( "mainsDone", tla_int( walk->mains_done ) );
    record.add( "fillsDone", tla_function( fillers, fills ) );
    record.add( "takesDone", tla_function( fillers, takes ) );
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

// a filler inside a fill call (RNFillPhase FillStates), not idle and not in a take
static bool fp_in_fill( const fp_walk_t * walk, int g )
{
    return walk->fpc[g] != FP_IDLE && walk->fpc[g] != FP_TCOUNTED && walk->fpc[g] != FP_TIN;
}

// RNFillPhase Justified(f), on the state before the step (a filler's step
// leaves the window as it was)
static bool fp_justified( const fp_walk_t * walk, int f )
{
    if ( walk->fbroke[f] || walk->mpc != FP_IDLE || fp_window( walk ) )
    {
        return true;
    }
    const int c = walk->fclient[f];
    for ( int g = 0; g < FP_FILLERS; g++ )
    {
        if ( g != f && ( walk->phase_in[c][g] || ( fp_in_fill( walk, g ) && walk->fclient[g] == c ) ) )
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
            walk->mkind = walk->next_kind;
            for ( int l = 0; l < FP_LINES; l++ )
            {
                walk->mscan[l] = true;
            }
        }
        else if ( action == "MainLook" && step->refused )
        {
            walk->false_alarm = walk->false_alarm || !fp_fill_busy( walk );
            walk->mpc = FP_IDLE;
            walk->mkind = FP_KIND_NONE;
            memset( walk->mscan, 0, sizeof( walk->mscan ) );
            walk->mains_done++;
        }
        else if ( action == "MainLook" && step->client_index >= 0 && step->client_index < FP_LINES )
        {
            walk->mscan[step->client_index] = false;
        }
        else if ( action == "MainIn" && step->refused )
        {
            walk->mpc = FP_IDLE;
            walk->mkind = FP_KIND_NONE;
            walk->mains_done++;
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
            walk->mkind = FP_KIND_NONE;
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
    if ( action == "TakeCount" )
    {
        walk->fpc[f] = FP_TCOUNTED;
        walk->fline[f] = (int) step->word;
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
        walk->fline[f] = (int) step->word;
    }
    else if ( action == "FillLook" && step->refused )
    {
        fp_fill_refused( walk, f );
        walk->fline[f] = FP_NO_LINE;
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
        walk->fline[f] = FP_NO_LINE;
        walk->fills_done[f]++;
    }
    else if ( action == "TakeLook" && step->refused )
    {
        walk->false_alarm = walk->false_alarm || !broke;
        walk->fpc[f] = FP_IDLE;
        walk->fline[f] = FP_NO_LINE;
        walk->takes_done[f]++;
    }
    else if ( action == "TakeLook" )
    {
        walk->fpc[f] = FP_TIN;
    }
    else if ( action == "TakeLeave" )
    {
        walk->fpc[f] = FP_IDLE;
        walk->fline[f] = FP_NO_LINE;
        walk->takes_done[f]++;
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
    {
        std::unique_lock<std::mutex> lock( walk->mutex );
        walk->id[me] = rocketnet_platform_thread_id();
    }
    if ( me > 0 )
    {
        rocketnet_fill_phase_test_set_line( walk->line[me - 1] );
    }
    for ( int call = 0; call < walk->calls; call++ )
    {
        fp_park_ready( walk, me );
        // the call sites' own scopes: not armed, a scope does nothing and
        // the call takes no step of the check (RNFillPhase MainSkip,
        // FillSkip, TakeSkip). An update call's work stores the window, as
        // the transport's update_receive and update_finish do, between
        // main_begin's last step and main_end's first (RNFillPhase MainEpoch)
        if ( me == 0 )
        {
            const fp_main_kind_t kind = walk->next_kind;
            rocketnet_fill_phase_main_scope_t scope( walk->phase, kind != FP_KIND_OTHER );
            if ( scope.entered && kind != FP_KIND_OTHER )
            {
                rocketnet_fill_phase_window( walk->phase, kind == FP_KIND_RECEIVE );
            }
        }
        else if ( walk->next_take[me] )
        {
            rocketnet_fill_phase_take_scope_t scope( walk->phase );
        }
        else
        {
            rocketnet_fill_phase_fill_scope_t scope( walk->phase, walk->next_client[me] );
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

// the call a step is part of: main_begin (announce, the looks, in), main_end
// (epoch, drop), fill_begin (begin to look), fill_end (unclaim, leave),
// take_begin (count, look), take_end (leave)
static const char * fp_op( int actor, const std::string & action )
{
    if ( actor == 0 )
    {
        return action == "MainEpoch" || action == "MainDrop" ? "main_end" : "main_begin";
    }
    if ( action == "TakeCount" || action == "TakeLook" )
    {
        return "take_begin";
    }
    if ( action == "TakeLeave" )
    {
        return "take_end";
    }
    return action == "FillUnclaim" || action == "FillLeave" ? "fill_end" : "fill_begin";
}

// the step's arguments in the model's order, what it gave back among them:
// the line a look read or a count took, ok, the step let the call or
// operation go on, and the window an update call's work left (read from the
// check). An announce names the operation's kind, the walk's own choice
static std::string fp_args( const fp_walk_t * walk, int actor, const fp_step_t * step )
{
    const std::string action = step->action;
    const std::string ok = tla_bool( !step->refused );
    if ( action == "MainLook" )
    {
        return tla_sequence( { tla_int( step->client_index ), ok } );
    }
    if ( action == "MainAnnounce" )
    {
        return tla_sequence( { fp_kind_text( walk->next_kind ) } );
    }
    if ( action == "MainIn" )
    {
        return tla_sequence( { ok } );
    }
    if ( action == "MainEpoch" )
    {
        return tla_sequence( { tla_bool( fp_window( walk ) ) } );
    }
    if ( actor == 0 )
    {
        return "<<>>";
    }
    const std::string f = fp_filler_name( actor - 1 );
    if ( action == "FillBegin" )
    {
        return tla_sequence( { f, tla_int( step->client_index ) } );
    }
    if ( action == "FillCount" || action == "TakeCount" )
    {
        return tla_sequence( { f, tla_int( (int64_t) step->word ) } );
    }
    if ( action == "FillLock" || action == "FillDecide" || action == "FillLook" || action == "TakeLook" )
    {
        return tla_sequence( { f, ok } );
    }
    return tla_sequence( { f } );
}

static int trace_fill_phase( uint64_t seed, int steps, const char * out )
{
    fp_walk_t * walk = new fp_walk_t();
    // the check is cache-line aligned, which a plain new does not promise on
    // every compiler: it is built in static storage of its own alignment
    alignas( rocketnet_fill_phase_t ) static unsigned char phase_memory[sizeof( rocketnet_fill_phase_t )];
    walk->phase = new ( phase_memory ) rocketnet_fill_phase_t();
    rocketnet_fill_phase_init( walk->phase, FP_CLIENTS, &walk->overlaps, &walk->two_fillers );

    // as the server makes it: not armed until the game names a client, which
    // the walk does once, after FP_ARM_AT records of skipped calls (RNFillPhase Arm)
    rocketnet_fill_phase_arm( walk->phase, false );
    walk->running = -1;
    walk->calls = steps;
    for ( int a = 0; a < FP_ACTORS; a++ )
    {
        walk->state[a] = FP_RUNNING;
    }
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        walk->saved[f] = -1;
        walk->fline[f] = FP_NO_LINE;
    }
    for ( int c = 0; c < FP_CLIENTS; c++ )
    {
        walk->last_owner[c] = -1;
    }

    tla_random_t random = { seed };

    // each filler's line; one seed in two puts the third filler on the first's
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        walk->line[f] = random.below( FP_LINES );
    }
    if ( random.below( 2 ) == 0 )
    {
        walk->line[FP_FILLERS - 1] = walk->line[0];
    }

    rocketnet_assert_function( fp_assert );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_fill_phase_set_trace( fp_hook, walk );

    std::vector<std::thread> actors;
    for ( int a = 0; a < FP_ACTORS; a++ )
    {
        actors.emplace_back( fp_actor, walk, a );
    }

    std::vector<tla_record_t> records;
    tla_report_t report;
    bool unknown = false;
    int refused = 0;
    int mains = 0;
    int chances = 0;

    // a filler that has loaded the epoch is often held back for a while, so
    // a main operation and other fillers' whole calls run inside its call (a
    // stale call), which a uniform schedule rarely makes; and after a filler
    // counts itself in, the main thread often runs next, so the filler's look
    // finds it (the plant's condition). After a main operation drops, the
    // main thread is held back for a while, so fillers also make whole calls
    // with no main operation in (a main call is one look per line, so an
    // unpaced main thread is in an operation nearly all the walk). Steering
    // only orders steps: every
    // step is still the code's own, and the record after it is still checked.
    int hold[FP_ACTORS] = {};
    int then = -1;                                  // the actor the steering picks next

    // an odd seed's main thread mostly opens the window (update_receive), an
    // even seed's mostly makes other operations: a walk has room for few
    // main operations, and the fill calls that pass need the window shut
    const bool window_walk = ( seed & 1 ) != 0;

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
        records.push_back( tla_step( "init", "init", "<<>>", fp_state( walk ) ) );

        while ( (int) records.size() < steps + 1 )
        {
            if ( !walk->opted && (int) records.size() > FP_ARM_AT )
            {
                // every actor is parked between calls: the latch is set
                rocketnet_fill_phase_arm( walk->phase, true );
                walk->opted = true;
                report.act( "Arm" );
                records.push_back( tla_step( "arm", "Arm", "<<>>", fp_state( walk ) ) );
                continue;
            }

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

            // the next call: a filler's client, and whether it is a take (more
            // often with the window open, where takes are the game threads'
            // work); the main thread's operation, mostly an update_receive
            // with the window shut (window_walk) and another operation
            // (refused) or an update_finish with it open
            if ( a > 0 && walk->state[a] == FP_READY )
            {
                walk->next_client[a] = random.below( FP_CLIENTS );
                walk->next_take[a] = random.below( fp_window( walk ) ? 2 : 4 ) == 0;
            }
            if ( a == 0 && walk->state[0] == FP_READY )
            {
                const int pick = random.below( 4 );
                if ( fp_window( walk ) && window_walk )
                {
                    walk->next_kind = pick < 2 ? FP_KIND_OTHER : ( pick == 2 ? FP_KIND_FINISH : FP_KIND_RECEIVE );
                }
                else if ( fp_window( walk ) )
                {
                    walk->next_kind = pick < 2 ? FP_KIND_FINISH : ( pick == 2 ? FP_KIND_OTHER : FP_KIND_RECEIVE );
                }
                else
                {
                    walk->next_kind = pick < ( window_walk ? 3 : 1 ) ? FP_KIND_RECEIVE : FP_KIND_OTHER;
                }
            }

            const bool was_ready = walk->state[a] == FP_READY;
            walk->running = a;
            walk->changed.notify_all();
            walk->changed.wait( lock, [&]
                                { return walk->running == -1; } );

            if ( walk->state[a] != FP_STEP )
            {
                if ( was_ready )
                {
                    // a whole call with no step of the check: its scope found
                    // the check not armed
                    if ( a == 0 )
                    {
                        walk->mains_done++;
                        report.act( "MainSkip" );
                        records.push_back( tla_step( "main_scope", "MainSkip", tla_sequence( { tla_bool( fp_window( walk ) ) } ), fp_state( walk ) ) );
                    }
                    else if ( walk->next_take[a] )
                    {
                        walk->takes_done[a - 1]++;
                        report.act( "TakeSkip" );
                        records.push_back( tla_step( "take_scope", "TakeSkip", tla_sequence( { fp_filler_name( a - 1 ) } ), fp_state( walk ) ) );
                    }
                    else
                    {
                        walk->fills_done[a - 1]++;
                        report.act( "FillSkip" );
                        records.push_back( tla_step( "fill_scope", "FillSkip", tla_sequence( { fp_filler_name( a - 1 ) } ), fp_state( walk ) ) );
                    }
                }
                continue;                           // the call returned: no step of the model
            }

            const fp_step_t * step = &walk->step[a];
            const std::string action = step->action;
            bool planted = false;
            if ( a > 0 && action == "FillLook" && step->refused )
            {
                // a refused look whose restore differs from the claim the
                // plant would leave in place: the plant's chance, the same
                // in both builds
                const int f = a - 1;
                const fp_word_t left = { walk->fepoch[f], false, false };
                const fp_word_t back = walk->seen[f];
                if ( back.ep != left.ep || back.held || back.in || walk->saved[f] != f )
                {
                    chances++;
                }
                // the planted effect: the word or the owner the look left is
                // not the one it replaced, which the clean code never leaves
                const int c = walk->fclient[f];
                const fp_word_t now = fp_word( walk->phase->clients[c].claim.load() );
                planted = now.ep != back.ep || now.held != back.held || now.in != back.in ||
                          fp_owner( walk, walk->phase->clients[c].owner.load() ) != walk->saved[f];
            }
            refused += step->refused ? 1 : 0;
            mains += action == "MainDrop" ? 1 : 0;

            const std::string args = fp_args( walk, a, step );
            if ( !fp_apply( walk, a, step ) )
            {
                unknown = true;
            }
            report.act( step->action );
            records.push_back( tla_step( fp_op( a, action ), step->action, args, fp_state( walk ) ) );
            if ( planted )
            {
                report.planted( (int) records.size() );
            }

            if ( a > 0 && action == "FillBegin" && random.below( 4 ) != 0 )
            {
                hold[a] = 8 + random.below( 16 );
            }
            if ( a == 0 && ( action == "MainDrop" || ( step->refused && ( action == "MainLook" || action == "MainIn" ) ) ) )
            {
                // after an operation ends, done or refused, fillers' whole
                // calls with no main operation in; with the
                // window open, one time in two the main thread's next
                // operation comes soon, so an operation other than an update
                // call meets the window in a walk that has room for few
                hold[0] = fp_window( walk ) && ( window_walk || random.below( 2 ) == 0 ) ? 4 + random.below( 8 ) : 32 + random.below( 64 );
            }
            if ( a > 0 && action == "FillCount" && ( chances == 0 || ( hold[0] == 0 && random.below( 2 ) == 0 ) ) )
            {
                // the main thread, then this filler's look; once the plant has
                // had its chance, only when the main thread is not held back
                then = 0;
            }
            else if ( a > 0 && action == "TakeCount" && hold[0] == 0 && random.below( 2 ) == 0 )
            {
                then = 0;                           // the main thread announces, then this take's look finds it
            }
            else if ( a > 0 && action == "TakeLook" && !step->refused && random.below( 2 ) == 0 )
            {
                // a take held in while other takes come to it, and now and
                // then the main thread's look (an update_finish before the
                // game threads are done)
                hold[a] = 72 + random.below( 32 );
                then = hold[0] == 0 && random.below( 2 ) == 0 ? 0 : -1;
            }
            else if ( a == 0 && action == "MainAnnounce" )
            {
                for ( int f = 0; f < FP_FILLERS; f++ )
                {
                    if ( walk->fpc[f] == FP_COUNTED || walk->fpc[f] == FP_TCOUNTED )
                    {
                        then = 1 + f;
                    }
                }
            }
            if ( then < 0 && a == 0 && window_walk && walk->mpc != FP_IDLE )
            {
                // an odd seed's main operation, once in, runs straight to its
                // end: a walk has room for the two operations, a receive
                // and then one the window refuses, only so
                then = 0;
            }
        }
    }

    // the walk's last step taken: let every actor run to its end, unrecorded
    rocketnet_fill_phase_set_trace( NULL, NULL );
    {
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
    rocketnet_assert_function( NULL );

    printf( "WALK records=%d mains=%d refused=%d chances=%d lines=%d,%d,%d\n", (int) records.size(), mains, refused, chances, walk->line[0],
            walk->line[1], walk->line[2] );
    report.print();
    if ( unknown )
    {
        fprintf( stderr, "tla_trace fill_phase: the hook named a step the model does not have\n" );
    }

    std::vector<std::string> line_names;
    for ( int l = 0; l < FP_LINES; l++ )
    {
        line_names.push_back( tla_int( l ) );
    }
    std::string lines = "{";
    for ( size_t l = 0; l < line_names.size(); l++ )
    {
        lines += ( l > 0 ? ", " : "" ) + line_names[l];
    }
    lines += "}";

    std::string fillers = "{";
    for ( int f = 0; f < FP_FILLERS; f++ )
    {
        fillers += ( f > 0 ? ", " : "" ) + fp_filler_name( f );
    }
    fillers += "}";

    std::string clients = "{";
    for ( int c = 0; c < FP_CLIENTS; c++ )
    {
        clients += ( c > 0 ? ", " : "" ) + tla_int( c );
    }
    clients += "}";

    const std::string constants = "    Fillers = " + fillers + "\n" + "    Clients = " + clients + "\n" + "    NoOwner = \"none\"\n" +
                                  "    MaxMain = " + tla_int( walk->calls ) + "\n" + "    MaxFills = " + tla_int( walk->calls ) + "\n" +
                                  "    MaxTakes = " + tla_int( walk->calls ) + "\n" +
                                  "    LookFirst = FALSE\n    LoadStore = FALSE\n    ResetOnLeave = TRUE\n    NoEpochLook = FALSE\n" +
                                  "    NoRestore = FALSE\n    ScanOne = FALSE\n    NoWindow = FALSE\n    NoMainWindow = FALSE\n" +
                                  "    TakeUncounted = FALSE\n    OptedAtStart = FALSE\n    ArmedAtCreate = FALSE\n" + "    Lines = " + lines + "\n";
    const bool written = tla_write_trace( out, "MCTraceFillPhase", seed, "TraceRNFillPhase", "", constants, records );

    walk->phase->~rocketnet_fill_phase_t();
    delete walk;
    return written && !unknown ? 0 : 1;
}

TLA_TRACE_LAYER( "fillphase", trace_fill_phase );
