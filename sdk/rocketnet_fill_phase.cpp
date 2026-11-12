/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_fill_phase.h"
#include "rocketnet_platform.h"

// The fill-phase check (rocketnet_fill_phase.h; proofs/tla/RNFillPhase.tla).
// One translation unit, so the thread token and the nesting depths are one
// per thread for the whole library.

static std::atomic<uint32_t> rocketnet_fill_phase_next_thread{ 0 };
static thread_local uint32_t rocketnet_fill_phase_this_thread = 0;
static thread_local int rocketnet_fill_phase_main_depth = 0;
static thread_local int rocketnet_fill_phase_fill_depth = 0;

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
#else // #if ROCKETNET_DEV
#define RN_FILL_PHASE_TRACE( action, client_index, word, refused ) ( (void) 0 )
#endif // #if ROCKETNET_DEV

uint32_t rocketnet_fill_phase_thread_token()
{
    if ( rocketnet_fill_phase_this_thread == 0 )
    {
        // 31 bits of thread number, shifted clear of the in bit; 0 is never issued
        uint32_t number = rocketnet_fill_phase_next_thread.fetch_add( 1, std::memory_order_relaxed ) + 1;
        number &= 0x7FFFFFFFu;
        if ( number == 0 )
        {
            number = 1;
        }
        rocketnet_fill_phase_this_thread = number << 1;
    }
    return rocketnet_fill_phase_this_thread;
}

static inline uint64_t rn_fill_phase_word( uint32_t epoch, uint32_t thread, bool in )
{
    return ( (uint64_t) epoch << ROCKETNET_FILL_PHASE_EPOCH_SHIFT ) | (uint64_t) thread | ( in ? ROCKETNET_FILL_PHASE_IN : 0 );
}

static inline uint32_t rn_fill_phase_word_epoch( uint64_t word )
{
    return (uint32_t) ( word >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT );
}

static inline uint32_t rn_fill_phase_word_thread( uint64_t word )
{
    return (uint32_t) ( word & ROCKETNET_FILL_PHASE_THREAD_MASK );
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

void rocketnet_fill_phase_init( struct rocketnet_fill_phase_t * fill_phase, int num_clients,
                                uint64_t * overlap_counter, uint64_t * two_fillers_counter )
{
    rocketnet_assert( fill_phase );
    rocketnet_assert( num_clients >= 0 && num_clients <= ROCKETNET_MAX_CLIENTS );

    fill_phase->main_calls.store( 0 );
    fill_phase->epoch.store( 1 );
    fill_phase->num_clients = num_clients < 0 ? 0 : ( num_clients > ROCKETNET_MAX_CLIENTS ? ROCKETNET_MAX_CLIENTS : num_clients );
    fill_phase->overlap_counter = overlap_counter;
    fill_phase->two_fillers_counter = two_fillers_counter;
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        fill_phase->claims[i].store( 0, std::memory_order_relaxed );
    }
}

// RNFillPhase MainAnnounce, MainLook
bool rocketnet_fill_phase_main_begin( struct rocketnet_fill_phase_t * fill_phase )
{
    rocketnet_assert( fill_phase );

    if ( rocketnet_fill_phase_main_depth > 0 )
    {
        rocketnet_fill_phase_main_depth++;          // the outer operation's own work
        return true;
    }

    // a main-thread operation from inside a fill call on this thread overlaps itself

    if ( rocketnet_fill_phase_fill_depth > 0 )
    {
        rn_fill_phase_count( fill_phase->overlap_counter );
        rocketnet_error( "server main-thread operation refused: called from inside a fill call" );
        rocketnet_assert( false && "a main-thread operation inside a fill call" );
        return false;
    }

    const uint32_t mains = fill_phase->main_calls.fetch_add( 1 );
    RN_FILL_PHASE_TRACE( "MainAnnounce", -1, mains, false );
    if ( mains != 0 )
    {
        fill_phase->main_calls.fetch_sub( 1 );
        rn_fill_phase_count( fill_phase->overlap_counter );
        rocketnet_error( "server main-thread operation refused: another main-thread operation is running" );
        rocketnet_assert( false && "two main-thread operations at once" );
        return false;
    }

    for ( int i = 0; i < fill_phase->num_clients; ++i )
    {
        const uint64_t word = fill_phase->claims[i].load();
        if ( rn_fill_phase_word_in( word ) )
        {
            RN_FILL_PHASE_TRACE( "MainLook", i, word, true );
            fill_phase->main_calls.fetch_sub( 1 );
            rn_fill_phase_count( fill_phase->overlap_counter );
            rocketnet_error( "server main-thread operation refused: a fill call for client %d is running", i );
            rocketnet_assert( false && "a main-thread operation inside a fill call" );
            return false;
        }
    }

    RN_FILL_PHASE_TRACE( "MainLook", -1, 0, false );
    rocketnet_fill_phase_main_depth = 1;
    return true;
}

// RNFillPhase MainLeave: the epoch moves, so the next fill phase begins with every client unclaimed
void rocketnet_fill_phase_main_end( struct rocketnet_fill_phase_t * fill_phase )
{
    rocketnet_assert( fill_phase );
    rocketnet_assert( rocketnet_fill_phase_main_depth > 0 );

    if ( --rocketnet_fill_phase_main_depth > 0 )
    {
        return;
    }

    const uint32_t epoch = fill_phase->epoch.load( std::memory_order_relaxed ) + 1;
    fill_phase->epoch.store( epoch == 0 ? 1 : epoch );
    RN_FILL_PHASE_TRACE( "MainLeave", -1, epoch, false );
    fill_phase->main_calls.fetch_sub( 1 );
}

// RNFillPhase FillClaim, FillLook
bool rocketnet_fill_phase_fill_begin( struct rocketnet_fill_phase_t * fill_phase, int client_index )
{
    rocketnet_assert( fill_phase );

    if ( rocketnet_fill_phase_fill_depth > 0 )
    {
        rocketnet_fill_phase_fill_depth++;          // the outer call's own work (message start inside start_packet's caller)
        return true;
    }

    // a fill call from inside a main-thread operation on this thread (a send
    // made from a callback of update) is that operation's own work

    if ( rocketnet_fill_phase_main_depth > 0 )
    {
        rocketnet_fill_phase_fill_depth = 1;
        return true;
    }

    const bool claims = client_index >= 0 && client_index < fill_phase->num_clients;
    const uint32_t me = rocketnet_fill_phase_thread_token();
    const uint32_t epoch = fill_phase->epoch.load();
    uint64_t claimed = 0;

    if ( claims )
    {
        std::atomic<uint64_t> * word = &fill_phase->claims[client_index];
        uint64_t seen = word->load();
        for ( ;; )
        {
            const uint32_t holder = rn_fill_phase_word_thread( seen );
            const bool other = holder != 0 && holder != me;
            if ( other && ( rn_fill_phase_word_in( seen ) || rn_fill_phase_word_epoch( seen ) == epoch ) )
            {
                RN_FILL_PHASE_TRACE( "FillClaim", client_index, seen, true );
                rn_fill_phase_count( fill_phase->two_fillers_counter );
                rocketnet_error( "server fill call refused: client %d is filled by another thread in this fill phase", client_index );
                rocketnet_assert( false && "two fillers on one client in one fill phase" );
                return false;
            }

            claimed = rn_fill_phase_word( epoch, me, true );
            if ( word->compare_exchange_weak( seen, claimed ) )
            {
                break;
            }
        }
        RN_FILL_PHASE_TRACE( "FillClaim", client_index, claimed, false );
    }

    // the epoch is read again after the look: a main operation that began and
    // ended between the epoch load and the claim left this claim stamped with
    // the old phase, which a second filler in the new phase would not see
    // (proofs/tla/RNFillPhase.tla FillLook)

    if ( fill_phase->main_calls.load() != 0 || fill_phase->epoch.load() != epoch )
    {
        if ( claims )
        {
            fill_phase->claims[client_index].store( claimed & ~ROCKETNET_FILL_PHASE_IN );
        }
        RN_FILL_PHASE_TRACE( "FillLook", client_index, claimed, true );
        rn_fill_phase_count( fill_phase->overlap_counter );
        rocketnet_error( "server fill call refused: a main-thread operation is running" );
        rocketnet_assert( false && "a fill call inside a main-thread operation" );
        return false;
    }

    RN_FILL_PHASE_TRACE( "FillLook", client_index, claimed, false );
    rocketnet_fill_phase_fill_depth = 1;
    return true;
}

// RNFillPhase FillLeave: only this thread writes the word while its in bit is set
void rocketnet_fill_phase_fill_end( struct rocketnet_fill_phase_t * fill_phase, int client_index )
{
    rocketnet_assert( fill_phase );
    rocketnet_assert( rocketnet_fill_phase_fill_depth > 0 );

    if ( --rocketnet_fill_phase_fill_depth > 0 )
    {
        return;
    }

    if ( rocketnet_fill_phase_main_depth > 0 )
    {
        return;                                     // a fill made by a main-thread operation claimed nothing
    }

    if ( client_index >= 0 && client_index < fill_phase->num_clients )
    {
        std::atomic<uint64_t> * word = &fill_phase->claims[client_index];
        const uint64_t left = word->load( std::memory_order_relaxed ) & ~ROCKETNET_FILL_PHASE_IN;
        word->store( left, std::memory_order_release );
        RN_FILL_PHASE_TRACE( "FillLeave", client_index, left, false );
    }
    else
    {
        RN_FILL_PHASE_TRACE( "FillLeave", client_index, 0, false );
    }
}
