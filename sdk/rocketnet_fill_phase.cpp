/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_fill_phase.h"
#include "rocketnet_platform.h"

// The fill-phase check (rocketnet_fill_phase.h; proofs/tla/RNFillPhase.tla).

#if ROCKETNET_DEV
static rocketnet_fill_phase_trace_function_t * rocketnet_fill_phase_trace_function = NULL;
static void * rocketnet_fill_phase_trace_context = NULL;

void rocketnet_fill_phase_set_trace( rocketnet_fill_phase_trace_function_t * function, void * context )
{
    rocketnet_fill_phase_trace_function = function;
    rocketnet_fill_phase_trace_context = context;
}

#define RN_FILL_PHASE_TRACE( action, client_index, word, refused )                                                         \
    do                                                                                                                     \
    {                                                                                                                      \
        if ( rocketnet_fill_phase_trace_function )                                                                         \
        {                                                                                                                  \
            rocketnet_fill_phase_trace_function( rocketnet_fill_phase_trace_context, action, client_index, word, refused ); \
        }                                                                                                                  \
    } while ( 0 )

static thread_local int rn_fill_phase_test_line = -1;

void rocketnet_fill_phase_test_set_line( int line )
{
    rn_fill_phase_test_line = line;
}

// the fills_in line this thread counts on (the trace check's, when it set one)
static inline int rn_fill_phase_line( uint64_t me )
{
    return rn_fill_phase_test_line >= 0 && rn_fill_phase_test_line < ROCKETNET_FILL_PHASE_FILL_LINES ? rn_fill_phase_test_line
                                                                                                     : rocketnet_fill_phase_line_of( me );
}
#else // #if ROCKETNET_DEV
#define RN_FILL_PHASE_TRACE( action, client_index, word, refused ) ( (void) 0 )

static inline int rn_fill_phase_line( uint64_t me )
{
    return rocketnet_fill_phase_line_of( me );
}
#endif // #if ROCKETNET_DEV

static inline uint64_t rn_fill_phase_word( uint64_t epoch, uint64_t depth, bool in )
{
    return ( epoch << ROCKETNET_FILL_PHASE_EPOCH_SHIFT ) | ( depth << ROCKETNET_FILL_PHASE_DEPTH_SHIFT ) | ( in ? ROCKETNET_FILL_PHASE_IN : 0 );
}

static inline uint64_t rn_fill_phase_word_epoch( uint64_t word )
{
    return word >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT;
}

static inline uint64_t rn_fill_phase_word_depth( uint64_t word )
{
    return ( word & ROCKETNET_FILL_PHASE_DEPTH_MASK ) >> ROCKETNET_FILL_PHASE_DEPTH_SHIFT;
}

static inline bool rn_fill_phase_word_in( uint64_t word )
{
    return ( word & ROCKETNET_FILL_PHASE_IN ) != 0;
}

static void rn_fill_phase_count( uint64_t * counter )
{
    if ( counter )
    {
        rocketnet_platform_atomic_add_u64( counter, 1 );
    }
}

static inline bool rn_fill_phase_claims( const struct rocketnet_fill_phase_t * fill_phase, int client_index )
{
    return client_index >= 0 && client_index < fill_phase->num_clients;
}

void rocketnet_fill_phase_init( struct rocketnet_fill_phase_t * fill_phase, int num_clients,
                                uint64_t * overlap_counter, uint64_t * two_fillers_counter )
{
    rocketnet_assert( fill_phase );
    rocketnet_assert( num_clients >= 0 && num_clients <= ROCKETNET_MAX_CLIENTS );

    fill_phase->main_calls.store( 0 );
    fill_phase->window.store( 0 );
    fill_phase->epoch.store( 1 );
    fill_phase->main_owner.store( 0 );
    fill_phase->armed.store( 1 );
    fill_phase->main_depth = 0;
    fill_phase->num_clients = num_clients < 0 ? 0 : ( num_clients > ROCKETNET_MAX_CLIENTS ? ROCKETNET_MAX_CLIENTS : num_clients );
    fill_phase->overlap_counter = overlap_counter;
    fill_phase->two_fillers_counter = two_fillers_counter;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        fill_phase->lines[i].fills_in.store( 0, std::memory_order_relaxed );
    }
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        fill_phase->clients[i].owner.store( 0, std::memory_order_relaxed );
        fill_phase->clients[i].claim.store( 0, std::memory_order_relaxed );
    }
}

// proofs/tla/RNFillPhase.tla MainAnnounce, MainLook (one per fills_in line), MainIn
// (refused for an operation other than an update call while the window is open)
bool rocketnet_fill_phase_main_begin( struct rocketnet_fill_phase_t * fill_phase, bool update_call )
{
    rocketnet_assert( fill_phase );

    // main_owner equals this thread only when this thread stored it: a main
    // operation made inside one (a callback of update) is the outer one's work

    const uint64_t me = rocketnet_platform_thread_id();
    if ( fill_phase->main_owner.load( std::memory_order_relaxed ) == me )
    {
        fill_phase->main_depth++;
        return true;
    }

    const uint32_t mains = fill_phase->main_calls.fetch_add( 1 );
    if ( mains != 0 )
    {
        fill_phase->main_calls.fetch_sub( 1 );
        rn_fill_phase_count( fill_phase->overlap_counter );
        RN_FILL_PHASE_TRACE( "MainAnnounce", -1, mains, true );
        rocketnet_error( "server main-thread operation refused: another main-thread operation is running" );
        rocketnet_assert( false && "two main-thread operations at once" );
        return false;
    }
    RN_FILL_PHASE_TRACE( "MainAnnounce", -1, mains, false );

    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        const uint32_t fills = fill_phase->lines[i].fills_in.load();
        if ( fills != 0 )
        {
            fill_phase->main_calls.fetch_sub( 1 );
            rn_fill_phase_count( fill_phase->overlap_counter );
            RN_FILL_PHASE_TRACE( "MainLook", i, fills, true );
            rocketnet_error( "server main-thread operation refused: a fill call is running" );
            rocketnet_assert( false && "a main-thread operation inside a fill call" );
            return false;
        }
        RN_FILL_PHASE_TRACE( "MainLook", i, fills, false );
    }

    // the window of the update in three calls: only update_client and the
    // update calls run in it. Only a main operation writes window, and none
    // runs now but this one, so the load sees the last one's store
    if ( !update_call && fill_phase->window.load() != 0 )
    {
        fill_phase->main_calls.fetch_sub( 1 );
        rn_fill_phase_count( fill_phase->overlap_counter );
        RN_FILL_PHASE_TRACE( "MainIn", -1, 1, true );
        rocketnet_error( "server main-thread operation refused: the update is open (update_receive called, update_finish not yet)" );
        rocketnet_assert( false && "a main-thread operation between update_receive and update_finish" );
        return false;
    }

    fill_phase->main_owner.store( me, std::memory_order_relaxed );
    fill_phase->main_depth = 1;
    RN_FILL_PHASE_TRACE( "MainIn", -1, 0, false );
    return true;
}

// proofs/tla/RNFillPhase.tla MainEpoch: the window's store is the work of the
// receive or finish operation that MainEpoch ends
void rocketnet_fill_phase_window( struct rocketnet_fill_phase_t * fill_phase, bool open )
{
    rocketnet_assert( fill_phase );
    fill_phase->window.store( open ? 1 : 0 );
}

// proofs/tla/RNFillPhase.tla TakeCount, TakeLook: update_client counts itself
// in, then looks for a main operation; the main side's MainLook sees the count
bool rocketnet_fill_phase_take_begin( struct rocketnet_fill_phase_t * fill_phase )
{
    rocketnet_assert( fill_phase );

    const uint64_t me = rocketnet_platform_thread_id();
    if ( fill_phase->main_owner.load( std::memory_order_relaxed ) == me )
    {
        return true;                                // a main operation's own work
    }

    const int line = rn_fill_phase_line( me );
    std::atomic<uint32_t> * fills_in = &fill_phase->lines[line].fills_in;
    fills_in->fetch_add( 1 );
    RN_FILL_PHASE_TRACE( "TakeCount", -1, (uint64_t) line, false );

    if ( fill_phase->main_calls.load() != 0 )
    {
        fills_in->fetch_sub( 1 );
        rn_fill_phase_count( fill_phase->overlap_counter );
        RN_FILL_PHASE_TRACE( "TakeLook", -1, (uint64_t) line, true );
        rocketnet_error( "server update_client refused: a main-thread operation is running" );
        rocketnet_assert( false && "update_client inside a main-thread operation" );
        return false;
    }

    RN_FILL_PHASE_TRACE( "TakeLook", -1, 0, false );
    return true;
}

// proofs/tla/RNFillPhase.tla TakeLeave
void rocketnet_fill_phase_take_end( struct rocketnet_fill_phase_t * fill_phase )
{
    rocketnet_assert( fill_phase );

    const uint64_t me = rocketnet_platform_thread_id();
    if ( fill_phase->main_owner.load( std::memory_order_relaxed ) == me )
    {
        return;
    }

    const int line = rn_fill_phase_line( me );
    fill_phase->lines[line].fills_in.fetch_sub( 1 );
    RN_FILL_PHASE_TRACE( "TakeLeave", -1, (uint64_t) line, false );
}

// proofs/tla/RNFillPhase.tla MainEpoch, MainDrop: the epoch moves, so the next fill phase begins with every client unclaimed
void rocketnet_fill_phase_main_end( struct rocketnet_fill_phase_t * fill_phase )
{
    rocketnet_assert( fill_phase );
    rocketnet_assert( fill_phase->main_owner.load( std::memory_order_relaxed ) == rocketnet_platform_thread_id() );
    rocketnet_assert( fill_phase->main_depth > 0 );

    if ( --fill_phase->main_depth > 0 )
    {
        return;
    }

    fill_phase->main_owner.store( 0, std::memory_order_relaxed );
    const uint64_t epoch = fill_phase->epoch.load( std::memory_order_relaxed ) + 1;
    fill_phase->epoch.store( epoch );
    RN_FILL_PHASE_TRACE( "MainEpoch", -1, epoch, false );
    fill_phase->main_calls.fetch_sub( 1 );
    RN_FILL_PHASE_TRACE( "MainDrop", -1, epoch, false );
}

// proofs/tla/RNFillPhase.tla Arm: the latch arms at the game's first call
// that names a client, or its first update_receive. Until then the call
// sites' scopes (rocketnet_fill_phase.h) do nothing: FillSkip, TakeSkip and
// MainSkip.
void rocketnet_fill_phase_arm( struct rocketnet_fill_phase_t * fill_phase, bool armed )
{
    rocketnet_assert( fill_phase );
    if ( ( fill_phase->armed.load( std::memory_order_relaxed ) != 0 ) != armed )
    {
        fill_phase->armed.store( armed ? 1 : 0 );
    }
}

// proofs/tla/RNFillPhase.tla FillBegin, FillLock, FillDecide, FillCount, FillLook, and for a word
// already claimed FillPeek and FillNest. The lock is one compare-and-swap; the reversed witness
// LoadStore takes it as a load (FillLoad) and a store (FillStore). The reversed witness
// OwnerAtDecide stores the owner at the decide and puts it back at a refused look in three stores
// (FillLook, FillUnwind1, FillUnwind2), the code before the owner moved after the look.
bool rocketnet_fill_phase_fill_begin( struct rocketnet_fill_phase_t * fill_phase, int client_index )
{
    rocketnet_assert( fill_phase );

    // a fill call from inside a main-thread operation on this thread (a send
    // made from a callback of update) is that operation's own work

    const uint64_t me = rocketnet_platform_thread_id();
    if ( fill_phase->main_owner.load( std::memory_order_relaxed ) == me )
    {
        return true;
    }

    const bool claims = rn_fill_phase_claims( fill_phase, client_index );
    const uint64_t epoch = fill_phase->epoch.load();
    RN_FILL_PHASE_TRACE( "FillBegin", client_index, epoch, false );

    uint64_t before = 0;                        // the word before the lock, restored on a refusal

    if ( claims )
    {
        std::atomic<uint64_t> * word = &fill_phase->clients[client_index].claim;
        std::atomic<uint64_t> * owner = &fill_phase->clients[client_index].owner;

        before = word->load();
        for ( ;; )
        {
            if ( rn_fill_phase_word_in( before ) )
            {
                // inside a fill call for this client already: this thread's
                // own when the word shows a claim (depth 1 or more) and the
                // owner is this thread; otherwise another thread is inside,
                // or deciding (depth 0). The owner is loaded after the word:
                // a claim's word is stored after its owner, by a call that
                // has passed its look, and no other store of the owner
                // follows while the claim stands, so a word that shows a
                // claim is never read with an owner that is not its
                // claimer's (FillPeek, FillNest).
                //
                // The depth stores (here and in fill_end) are relaxed, and
                // that holds under C++20, where a plain store does not
                // continue a release sequence. The argument is not
                // synchronizes-with but the single total order S of the
                // seq_cst operations ([atomics.order]): only the claimer
                // stores a depth, after its seq_cst claim store C (owner
                // store O sequenced before C), while its claim stands. A
                // seq_cst word load L here that reads a depth store D
                // follows C in modification order (C, then D, same thread),
                // so C is coherence-ordered before L, and both are seq_cst:
                // C precedes L in S. O precedes C in S (sequenced before),
                // and L precedes the owner load L2 below. So O precedes L2
                // in S, and a seq_cst load preceded in S by a seq_cst store
                // to the same object cannot read a value from before that
                // store: L2 reads the claimer, or a later owner (only after
                // the claim ended, never this thread, which is inside this
                // call). Either way not this thread, so it is refused.
                // arm64 gives the same: O and C are stlr, D is a str after C
                // to the same address (a local write successor), L and L2
                // are ldar. RNFillPhase is sequentially consistent and
                // cannot see this; the argument is the evidence

                const uint64_t depth = rn_fill_phase_word_depth( before );
                if ( depth == 0 )
                {
                    rn_fill_phase_count( fill_phase->two_fillers_counter );
                    RN_FILL_PHASE_TRACE( "FillLock", client_index, before, true );
                    rocketnet_error( "server fill call refused: client %d is filled by another thread in this fill phase", client_index );
                    rocketnet_assert( false && "two fillers on one client in one fill phase" );
                    return false;
                }
                RN_FILL_PHASE_TRACE( "FillPeek", client_index, before, false );
                if ( owner->load() != me )
                {
                    rn_fill_phase_count( fill_phase->two_fillers_counter );
                    RN_FILL_PHASE_TRACE( "FillNest", client_index, before, true );
                    rocketnet_error( "server fill call refused: client %d is filled by another thread in this fill phase", client_index );
                    rocketnet_assert( false && "two fillers on one client in one fill phase" );
                    return false;
                }

                // a nested call: only the depth counts. The depth has 7 bits,
                // and one more past its top would carry into the epoch, so a
                // call nested that deep is refused, as misuse
                if ( depth >= ROCKETNET_FILL_PHASE_DEPTH_MAX )
                {
                    rocketnet_api_misuse_record();
                    RN_FILL_PHASE_TRACE( "FillNest", client_index, before, true );
                    rocketnet_error( "server fill call refused: fill calls for client %d nested %d deep", client_index, (int) depth );
                    rocketnet_assert( false && "fill calls nested past ROCKETNET_FILL_PHASE_DEPTH_MAX" );
                    return false;
                }
                const uint64_t nested = rn_fill_phase_word( rn_fill_phase_word_epoch( before ), depth + 1, true );
                word->store( nested, std::memory_order_relaxed );
                RN_FILL_PHASE_TRACE( "FillNest", client_index, nested, false );
                return true;
            }
            if ( word->compare_exchange_weak( before, before | ROCKETNET_FILL_PHASE_IN ) )
            {
                break;
            }
        }
        RN_FILL_PHASE_TRACE( "FillLock", client_index, before | ROCKETNET_FILL_PHASE_IN, false );

        // the lock is held: only this thread writes the word and the owner
        // until it clears in. Another thread's claim of this call's epoch
        // refuses. A call that loaded an older epoch (stale: a main operation
        // ran inside it) may go on over a newer one: its look refuses it.
        // The owner and the claim are stored only after the look lets the
        // call in, so a refusal puts back the word alone and never the
        // owner: an owner put back over a claim that a nested test already
        // read would let that test take another thread's claim as its own

        const uint64_t owner_before = owner->load();
        if ( owner_before != 0 && owner_before != me && rn_fill_phase_word_epoch( before ) == epoch )
        {
            word->store( before );
            rn_fill_phase_count( fill_phase->two_fillers_counter );
            RN_FILL_PHASE_TRACE( "FillDecide", client_index, before, true );
            rocketnet_error( "server fill call refused: client %d is filled by another thread in this fill phase", client_index );
            rocketnet_assert( false && "two fillers on one client in one fill phase" );
            return false;
        }
        RN_FILL_PHASE_TRACE( "FillDecide", client_index, before | ROCKETNET_FILL_PHASE_IN, false );
    }

    const int line = rn_fill_phase_line( me );
    std::atomic<uint32_t> * fills_in = &fill_phase->lines[line].fills_in;
    fills_in->fetch_add( 1 );
    RN_FILL_PHASE_TRACE( "FillCount", client_index, (uint64_t) line, false );

    // the epoch is read again after the look: a main operation that began and
    // ended between the epoch load and the count left this call deciding with
    // the old phase, which a second filler in the new phase would not see
    // (proofs/tla/RNFillPhase.tla FillLook). The window is read too: between
    // update_receive and update_finish no fill call runs

    if ( fill_phase->main_calls.load() != 0 || fill_phase->epoch.load() != epoch || fill_phase->window.load() != 0 )
    {
        // a refused call leaves no claim: the client's next filler in this
        // phase, honest, must not be refused for it (NoFalseAlarm). The owner
        // was never written: the word alone goes back, out

        if ( claims )
        {
#if defined( ROCKETNET_FILL_PHASE_PLANT )
            // the trace check's plant (tests/tla_trace_fill_phase.cpp): the
            // refused call leaves a word, out, stamped with its epoch, in
            // place of the word it replaced
            fill_phase->clients[client_index].claim.store( rn_fill_phase_word( epoch, 0, false ) );
#else  // #if defined( ROCKETNET_FILL_PHASE_PLANT )
            fill_phase->clients[client_index].claim.store( before );
#endif // #if defined( ROCKETNET_FILL_PHASE_PLANT )
        }
        fills_in->fetch_sub( 1 );
        rn_fill_phase_count( fill_phase->overlap_counter );
        RN_FILL_PHASE_TRACE( "FillLook", client_index, before, true );
        rocketnet_error( "server fill call refused: a main-thread operation is running, or the update is open" );
        rocketnet_assert( false && "a fill call inside a main-thread operation or an open update" );
        return false;
    }

    // the call is in: its claim, the owner first and then the word. Until the
    // word's store a nested test of another thread reads depth 0 and is
    // refused; from it on the owner it reads is this thread
    if ( claims )
    {
        fill_phase->clients[client_index].owner.store( me );
        fill_phase->clients[client_index].claim.store( rn_fill_phase_word( epoch, 1, true ) );
    }

    RN_FILL_PHASE_TRACE( "FillLook", client_index, 0, false );
    return true;
}

// proofs/tla/RNFillPhase.tla FillUnnest, FillUnclaim, FillLeave: only this thread writes the word while its in bit is set
void rocketnet_fill_phase_fill_end( struct rocketnet_fill_phase_t * fill_phase, int client_index )
{
    rocketnet_assert( fill_phase );

    const uint64_t me = rocketnet_platform_thread_id();
    if ( fill_phase->main_owner.load( std::memory_order_relaxed ) == me )
    {
        return;                                     // a fill made by a main-thread operation claimed nothing
    }

    if ( rn_fill_phase_claims( fill_phase, client_index ) )
    {
        std::atomic<uint64_t> * word = &fill_phase->clients[client_index].claim;
        const uint64_t held = word->load( std::memory_order_relaxed );
        rocketnet_assert( rn_fill_phase_word_in( held ) );
        rocketnet_assert( fill_phase->clients[client_index].owner.load( std::memory_order_relaxed ) == me );

        const uint64_t depth = rn_fill_phase_word_depth( held );
        rocketnet_assert( depth > 0 );
        if ( depth > 1 )
        {
            // proofs/tla/RNFillPhase.tla FillUnnest. Relaxed, as the nested
            // store in fill_begin is: a nested test that loads this word is
            // ordered after the claim store by the seq_cst order S (the
            // argument at fill_begin's nested test), not by a release
            // sequence
            const uint64_t outer = rn_fill_phase_word( rn_fill_phase_word_epoch( held ), depth - 1, true );
            word->store( outer, std::memory_order_relaxed );
            RN_FILL_PHASE_TRACE( "FillUnnest", client_index, outer, false );
            return;                                 // a nested call: the outer call still holds the client
        }

        const uint64_t left = rn_fill_phase_word( rn_fill_phase_word_epoch( held ), 0, false );
        word->store( left );
        RN_FILL_PHASE_TRACE( "FillUnclaim", client_index, left, false );
    }

    const int line = rn_fill_phase_line( me );
    fill_phase->lines[line].fills_in.fetch_sub( 1 );
    RN_FILL_PHASE_TRACE( "FillLeave", client_index, (uint64_t) line, false );
}
