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
    fill_phase->epoch.store( 1 );
    fill_phase->main_owner.store( 0 );
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
bool rocketnet_fill_phase_main_begin( struct rocketnet_fill_phase_t * fill_phase )
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

    fill_phase->main_owner.store( me, std::memory_order_relaxed );
    fill_phase->main_depth = 1;
    RN_FILL_PHASE_TRACE( "MainIn", -1, 0, false );
    return true;
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

// proofs/tla/RNFillPhase.tla FillBegin, FillLock, FillDecide, FillCount, FillLook. The lock is one
// compare-and-swap; the reversed witness LoadStore takes it as a load (FillLoad) and a store (FillStore).
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
    uint64_t owner_before = 0;

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
                // own when it finished deciding (depth 1 or more, stored after
                // the owner) and named itself owner; otherwise another thread
                // is inside, or deciding

                const uint64_t depth = rn_fill_phase_word_depth( before );
                if ( depth > 0 && owner->load() == me )
                {
                    rocketnet_assert( depth < ROCKETNET_FILL_PHASE_DEPTH_MAX );
                    word->store( rn_fill_phase_word( rn_fill_phase_word_epoch( before ), depth + 1, true ), std::memory_order_relaxed );
                    return true;
                }
                rn_fill_phase_count( fill_phase->two_fillers_counter );
                RN_FILL_PHASE_TRACE( "FillLock", client_index, before, true );
                rocketnet_error( "server fill call refused: client %d is filled by another thread in this fill phase", client_index );
                rocketnet_assert( false && "two fillers on one client in one fill phase" );
                return false;
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
        // ran inside it) may claim over a newer one: its look refuses it and
        // puts the owner and the word back, and while it holds the lock any
        // other filler of the client is refused at FillLock

        owner_before = owner->load();
        if ( owner_before != 0 && owner_before != me && rn_fill_phase_word_epoch( before ) == epoch )
        {
            word->store( before );
            rn_fill_phase_count( fill_phase->two_fillers_counter );
            RN_FILL_PHASE_TRACE( "FillDecide", client_index, before, true );
            rocketnet_error( "server fill call refused: client %d is filled by another thread in this fill phase", client_index );
            rocketnet_assert( false && "two fillers on one client in one fill phase" );
            return false;
        }

        owner->store( me );
        word->store( rn_fill_phase_word( epoch, 1, true ) );
        RN_FILL_PHASE_TRACE( "FillDecide", client_index, rn_fill_phase_word( epoch, 1, true ), false );
    }

    const int line = rn_fill_phase_line( me );
    std::atomic<uint32_t> * fills_in = &fill_phase->lines[line].fills_in;
    fills_in->fetch_add( 1 );
    RN_FILL_PHASE_TRACE( "FillCount", client_index, (uint64_t) line, false );

    // the epoch is read again after the look: a main operation that began and
    // ended between the epoch load and the count left this call deciding with
    // the old phase, which a second filler in the new phase would not see
    // (proofs/tla/RNFillPhase.tla FillLook)

    if ( fill_phase->main_calls.load() != 0 || fill_phase->epoch.load() != epoch )
    {
        // a refused call leaves no claim: the client's next filler in this
        // phase, honest, must not be refused for it (NoFalseAlarm)

        if ( claims )
        {
#if defined( ROCKETNET_FILL_PHASE_PLANT )
            // the trace check's plant (tests/tla_trace_fill_phase.cpp): the
            // refused claim stays, out, in place of the word it replaced
            fill_phase->clients[client_index].claim.store( rn_fill_phase_word( epoch, 0, false ) );
#else  // #if defined( ROCKETNET_FILL_PHASE_PLANT )

            // the word goes back to the lock's own form (in, depth 0: a
            // claim being decided) before the owner does: with the word
            // still [epoch, 1, in] and the owner already the thread it was,
            // that thread's next call for the client would read itself as
            // nested in a call it never made, and its end would then leave
            // a fills_in line it never entered
            fill_phase->clients[client_index].claim.store( before | ROCKETNET_FILL_PHASE_IN );
            fill_phase->clients[client_index].owner.store( owner_before );
            fill_phase->clients[client_index].claim.store( before );
#endif // #if defined( ROCKETNET_FILL_PHASE_PLANT )
        }
        fills_in->fetch_sub( 1 );
        rn_fill_phase_count( fill_phase->overlap_counter );
        RN_FILL_PHASE_TRACE( "FillLook", client_index, before, true );
        rocketnet_error( "server fill call refused: a main-thread operation is running" );
        rocketnet_assert( false && "a fill call inside a main-thread operation" );
        return false;
    }

    RN_FILL_PHASE_TRACE( "FillLook", client_index, 0, false );
    return true;
}

// proofs/tla/RNFillPhase.tla FillUnclaim, FillLeave: only this thread writes the word while its in bit is set
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
            word->store( rn_fill_phase_word( rn_fill_phase_word_epoch( held ), depth - 1, true ), std::memory_order_relaxed );
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
