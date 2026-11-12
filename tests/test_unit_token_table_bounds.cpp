/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The used-token history (sdk/rocketnet_token_tables.cpp), the server's replay
// protection for connect tokens, at its bounds. No socket, no real time:
// timestamps and times are arguments from 1000.
//
//   * test_token_table_plan_at_the_capacity_bounds: the memory plan is 0 for a
//     capacity below 1 or above INT_MAX / 2, and linear in the capacity from
//     1 through INT_MAX / 2;
//   * test_token_bindings_refuse_a_second_address_at_every_slot and
//     test_token_claims_refuse_a_second_address_at_every_slot: a token seen
//     twice from another address is refused at every slot, the last one
//     included, and across the insertion ring's wrap, oldest evicted first;
//   * test_token_bindings_follow_the_model and test_token_claims_follow_the_model:
//     a long deterministic run of records, releases and clock moves at
//     capacities 1 to 7 against an independent model of the header's rules,
//     every token checked from both addresses after every step;
//   * test_token_history_create_at_the_capacity_bounds: through a counting
//     allocator, create refuses 0, -1 and INT_MAX / 2 + 1 before allocating,
//     reaches the allocator at INT_MAX / 2 (whose index the allocator
//     refuses) with nothing left live, and a table's allocations are exactly
//     its plan;
//   * test_token_index_cell_at_capacity_is_never_a_hit: by fault injection, an
//     index cell naming the slot one past the last (slot == capacity) is
//     skipped by every lookup. The slack after each allocation is filled with
//     a guard pattern that, read as a slot, would be a live binding and a live
//     claim for the probe token held by a third address: the tables are
//     separate allocations of one allocator, so a read one past an array
//     lands in memory a sanitizer does not flag, and only behaviour shows it.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_address.h"
#include "rocketnet_token_tables.h"
#include "loopback_hub.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

static void tt_id( uint8_t * identifier, int value )
{
    memset( identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    identifier[0] = (uint8_t) value;
    identifier[1] = (uint8_t) ( value >> 8 );
    identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES - 1] = (uint8_t) ( 0x3C ^ value );
}

static void tt_addresses( struct rocketnet_address_t * address )
{
    check( rocketnet_address_parse( &address[0], "198.51.100.7:50000" ) );
    check( rocketnet_address_parse( &address[1], "198.51.100.8:50000" ) );
}

static void test_token_table_plan_at_the_capacity_bounds()
{
    check( rocketnet_token_table_allocation_bytes( INT_MIN ) == 0 );
    check( rocketnet_token_table_allocation_bytes( -1 ) == 0 );
    check( rocketnet_token_table_allocation_bytes( 0 ) == 0 );
    check( rocketnet_token_table_allocation_bytes( INT_MAX / 2 + 1 ) == 0 );
    check( rocketnet_token_table_allocation_bytes( INT_MAX ) == 0 );

    const uint64_t one = rocketnet_token_table_allocation_bytes( 1 );
    const uint64_t two = rocketnet_token_table_allocation_bytes( 2 );
    check( one > 0 );
    check( two > one );

    // every array is a whole number of slots: the plan is linear in the capacity
    const uint64_t slot = two - one;
    const uint64_t top = rocketnet_token_table_allocation_bytes( INT_MAX / 2 );
    check( top == one + (uint64_t) ( INT_MAX / 2 - 1 ) * slot );
    check( rocketnet_token_table_allocation_bytes( 1000 ) == one + 999 * slot );
}

REGISTER_UNIT_TEST( test_token_table_plan_at_the_capacity_bounds );

static void test_token_bindings_refuse_a_second_address_at_every_slot()
{
    struct rocketnet_address_t address[2];
    tt_addresses( address );

    for ( int capacity = 1; capacity <= 5; ++capacity )
    {
        struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
        check( history );

        // five times the capacity, so the insertion ring (twice the capacity)
        // wraps and compacts more than once; every binding outlives the run
        const int tokens = 5 * capacity + 1;
        uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        for ( int k = 0; k < tokens; ++k )
        {
            tt_id( id, k );
            check( rocketnet_used_token_history_check( history, id, &address[1], 1000 ) == 1 );
            rocketnet_used_token_history_record( history, id, &address[k % 2], 5000, 1000 );

            // the newest capacity tokens are bound, each refused from the
            // other address and admitted from its own; the older ones were
            // evicted oldest first and are unseen again
            for ( int j = 0; j <= k; ++j )
            {
                tt_id( id, j );
                const bool bound = j > k - capacity;
                check( rocketnet_used_token_history_check( history, id, &address[1 - j % 2], 1000 ) == ( bound ? 0 : 1 ) );
                check( rocketnet_used_token_history_check( history, id, &address[j % 2], 1000 ) == 1 );
            }

            // a second record of the newest from the other address changes nothing
            tt_id( id, k );
            rocketnet_used_token_history_record( history, id, &address[1 - k % 2], 5000, 1000 );
            check( rocketnet_used_token_history_check( history, id, &address[1 - k % 2], 1000 ) == 0 );
            check( rocketnet_used_token_history_check( history, id, &address[k % 2], 1000 ) == 1 );
        }

        // at its expiry every binding is unseen
        for ( int j = tokens - capacity; j < tokens; ++j )
        {
            tt_id( id, j );
            check( rocketnet_used_token_history_check( history, id, &address[1 - j % 2], 4999 ) == 0 );
            check( rocketnet_used_token_history_check( history, id, &address[1 - j % 2], 5000 ) == 1 );
        }

        rocketnet_used_token_history_destroy( history );
    }
}

REGISTER_UNIT_TEST( test_token_bindings_refuse_a_second_address_at_every_slot );

static void test_token_claims_refuse_a_second_address_at_every_slot()
{
    struct rocketnet_address_t address[2];
    tt_addresses( address );

    for ( int capacity = 1; capacity <= 5; ++capacity )
    {
        struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
        check( history );

        uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        int first = 0;
        double now = 1000.0;
        for ( int round = 0; round < 6; ++round )
        {
            // a full table of live claims: each refused from the other address
            for ( int j = 0; j < capacity; ++j )
            {
                tt_id( id, first + j );
                check( rocketnet_used_token_history_claim_can_record( history, id, now ) == 1 );
                check( rocketnet_used_token_history_claim_record( history, id, &address[j % 2], now + 10.0, now ) == 1 );
            }

            for ( int j = 0; j < capacity; ++j )
            {
                tt_id( id, first + j );
                check( rocketnet_used_token_history_claim_check( history, id, &address[1 - j % 2], now ) == 0 );
                check( rocketnet_used_token_history_claim_check( history, id, &address[j % 2], now ) == 1 );

                // a live claim is never replaced, from either address
                check( rocketnet_used_token_history_claim_record( history, id, &address[1 - j % 2], now + 20.0, now ) == 1 );
                check( rocketnet_used_token_history_claim_check( history, id, &address[1 - j % 2], now ) == 0 );
            }

            tt_id( id, first + capacity );
            check( rocketnet_used_token_history_claim_can_record( history, id, now ) == 0 );
            check( rocketnet_used_token_history_claim_record( history, id, &address[0], now + 10.0, now ) == 0 );
            check( rocketnet_used_token_history_claim_check( history, id, &address[1], now ) == 1 );

            // even rounds release the last slot's claim by its holder (the
            // other address's release changes nothing) and claim it anew;
            // odd rounds let every deadline pass
            if ( round % 2 == 0 )
            {
                const int last = capacity - 1;
                tt_id( id, first + last );
                rocketnet_used_token_history_claim_release( history, id, &address[1 - last % 2] );
                check( rocketnet_used_token_history_claim_check( history, id, &address[1 - last % 2], now ) == 0 );
                rocketnet_used_token_history_claim_release( history, id, &address[last % 2] );
                check( rocketnet_used_token_history_claim_check( history, id, &address[1 - last % 2], now ) == 1 );

                tt_id( id, first + capacity );
                check( rocketnet_used_token_history_claim_record( history, id, &address[1], now + 10.0, now ) == 1 );
                check( rocketnet_used_token_history_claim_check( history, id, &address[0], now ) == 0 );
            }

            now += 10.0;
            for ( int j = 0; j <= capacity; ++j )
            {
                tt_id( id, first + j );
                check( rocketnet_used_token_history_claim_check( history, id, &address[0], now ) == 1 );
                check( rocketnet_used_token_history_claim_check( history, id, &address[1], now ) == 1 );
            }

            first += capacity + 1;
        }

        rocketnet_used_token_history_destroy( history );
    }
}

REGISTER_UNIT_TEST( test_token_claims_refuse_a_second_address_at_every_slot );

// ---- an independent model of the header's rules ----------------------------

enum
{
    TT_MODEL_MAX = 8,
    TT_MODEL_STEPS = 3000
};

struct tt_model_entry_t
{
    int id;
    int owner;
    uint64_t ticket;
    uint64_t expire;    // bindings
    double deadline;    // claims
};

struct tt_model_t
{
    int capacity;
    int count;
    uint64_t next_ticket;
    tt_model_entry_t entry[TT_MODEL_MAX];
};

static uint32_t tt_next( uint32_t * seed )
{
    *seed = *seed * 1664525u + 1013904223u;
    return *seed >> 8;
}

static int tt_model_find( const tt_model_t * model, int id )
{
    for ( int i = 0; i < model->count; ++i )
    {
        if ( model->entry[i].id == id )
        {
            return i;
        }
    }

    return -1;
}

static int tt_model_oldest( const tt_model_t * model )
{
    int oldest = -1;
    for ( int i = 0; i < model->count; ++i )
    {
        if ( oldest < 0 || model->entry[i].ticket < model->entry[oldest].ticket )
        {
            oldest = i;
        }
    }

    return oldest;
}

static void tt_model_remove( tt_model_t * model, int i )
{
    model->entry[i] = model->entry[model->count - 1];
    --model->count;
}

// header: an identifier is refused only when bound to another address and not
// expired; a record of a bound live identifier changes nothing; a record
// evicts an expired binding first, the oldest otherwise. Which expired binding
// goes is not observable: an expired binding already reads as unseen.
static void tt_model_bind( tt_model_t * model, int id, int owner, uint64_t expire, uint64_t now )
{
    const int found = tt_model_find( model, id );
    if ( found >= 0 && model->entry[found].expire > now )
    {
        return;
    }

    int at = found;
    if ( at < 0 )
    {
        if ( model->count < model->capacity )
        {
            at = model->count++;
        }
        else
        {
            for ( int i = 0; i < model->count && at < 0; ++i )
            {
                if ( model->entry[i].expire <= now )
                {
                    at = i;
                }
            }

            if ( at < 0 )
            {
                at = tt_model_oldest( model );
            }
        }
    }

    model->entry[at].id = id;
    model->entry[at].owner = owner;
    model->entry[at].expire = expire;
    model->entry[at].deadline = 0.0;
    model->entry[at].ticket = model->next_ticket++;
}

static void test_token_bindings_follow_the_model()
{
    struct rocketnet_address_t address[2];
    tt_addresses( address );

    const int capacities[] = { 1, 2, 3, 4, 7 };
    for ( int c = 0; c < (int) ( sizeof( capacities ) / sizeof( capacities[0] ) ); ++c )
    {
        const int capacity = capacities[c];
        const int universe = 3 * capacity + 3;

        struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
        check( history );

        tt_model_t model;
        memset( &model, 0, sizeof( model ) );
        model.capacity = capacity;
        model.next_ticket = 1;

        uint32_t seed = 0x51u + (uint32_t) capacity;
        uint64_t now = 1000;
        uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        for ( int step = 0; step < TT_MODEL_STEPS; ++step )
        {
            now += tt_next( &seed ) % 3;
            const int token = (int) ( tt_next( &seed ) % (uint32_t) universe );
            const int owner = (int) ( tt_next( &seed ) % 2 );
            const uint64_t expire = now + 1 + tt_next( &seed ) % 12;

            tt_id( id, token );
            rocketnet_used_token_history_record( history, id, &address[owner], expire, now );
            tt_model_bind( &model, token, owner, expire, now );

            for ( int u = 0; u < universe; ++u )
            {
                const int at = tt_model_find( &model, u );
                const bool live = at >= 0 && model.entry[at].expire > now;
                tt_id( id, u );
                for ( int o = 0; o < 2; ++o )
                {
                    const int want = live && model.entry[at].owner != o ? 0 : 1;
                    check( rocketnet_used_token_history_check( history, id, &address[o], now ) == want );
                }
            }
        }

        rocketnet_used_token_history_destroy( history );
    }
}

REGISTER_UNIT_TEST( test_token_bindings_follow_the_model );

// header: a claim is refused to another address until its deadline; a record
// takes a free slot, else the oldest claim once its deadline has passed, and a
// full table of live claims refuses; a live claim is never replaced; an
// expired claim of the same token is claimed again as the newest; a release by
// the holder frees the slot at once.
static int tt_model_claim( tt_model_t * model, int id, int owner, double deadline, double now )
{
    int at = tt_model_find( model, id );
    if ( at >= 0 && model->entry[at].deadline > now )
    {
        return 1;
    }

    if ( at < 0 )
    {
        if ( model->count < model->capacity )
        {
            at = model->count++;
        }
        else
        {
            at = tt_model_oldest( model );
            if ( model->entry[at].deadline > now )
            {
                return 0;
            }
        }
    }

    model->entry[at].id = id;
    model->entry[at].owner = owner;
    model->entry[at].expire = 0;
    model->entry[at].deadline = deadline;
    model->entry[at].ticket = model->next_ticket++;
    return 1;
}

static int tt_model_claim_can_record( const tt_model_t * model, int id, double now )
{
    if ( tt_model_find( model, id ) >= 0 || model->count < model->capacity )
    {
        return 1;
    }

    return model->entry[tt_model_oldest( model )].deadline <= now ? 1 : 0;
}

static void test_token_claims_follow_the_model()
{
    struct rocketnet_address_t address[2];
    tt_addresses( address );

    const int capacities[] = { 1, 2, 3, 4, 7 };
    for ( int c = 0; c < (int) ( sizeof( capacities ) / sizeof( capacities[0] ) ); ++c )
    {
        const int capacity = capacities[c];
        const int universe = 3 * capacity + 3;

        struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
        check( history );

        tt_model_t model;
        memset( &model, 0, sizeof( model ) );
        model.capacity = capacity;
        model.next_ticket = 1;

        uint32_t seed = 0xC1u + (uint32_t) capacity;
        double now = 1000.0;
        uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        for ( int step = 0; step < TT_MODEL_STEPS; ++step )
        {
            now += 0.25 * (double) ( tt_next( &seed ) % 5 );
            const int token = (int) ( tt_next( &seed ) % (uint32_t) universe );
            const int owner = (int) ( tt_next( &seed ) % 2 );
            const uint32_t kind = tt_next( &seed ) % 4;

            tt_id( id, token );
            if ( kind == 0 )
            {
                rocketnet_used_token_history_claim_release( history, id, &address[owner] );
                const int at = tt_model_find( &model, token );
                if ( at >= 0 && model.entry[at].owner == owner )
                {
                    tt_model_remove( &model, at );
                }
            }
            else
            {
                const double deadline = now + 0.25 * (double) ( 1 + tt_next( &seed ) % 12 );
                const int want = tt_model_claim( &model, token, owner, deadline, now );
                check( rocketnet_used_token_history_claim_record( history, id, &address[owner], deadline, now ) == want );
            }

            for ( int u = 0; u < universe; ++u )
            {
                const int at = tt_model_find( &model, u );
                const bool live = at >= 0 && model.entry[at].deadline > now;
                tt_id( id, u );
                for ( int o = 0; o < 2; ++o )
                {
                    const int want = live && model.entry[at].owner != o ? 0 : 1;
                    check( rocketnet_used_token_history_claim_check( history, id, &address[o], now ) == want );
                }
                check( rocketnet_used_token_history_claim_can_record( history, id, now ) == tt_model_claim_can_record( &model, u, now ) );
            }
        }

        rocketnet_used_token_history_destroy( history );
    }
}

REGISTER_UNIT_TEST( test_token_claims_follow_the_model );

// ---- a counting allocator with a guard pattern after every block -------------

enum
{
    TT_GUARD_BYTES = 128,
    TT_TRACE_MAX = 64
};

static const uint8_t TT_GUARD = 0x5A;

// a request past this is refused: the index of a table at INT_MAX / 2
static const size_t TT_REFUSE_ABOVE = (size_t) 1 << 30;

struct tt_allocator_t
{
    int live;
    int calls;
    int refused;
    uint64_t requested;
    int traced;
    uint8_t * pointer[TT_TRACE_MAX];
    size_t bytes[TT_TRACE_MAX];
};

static void * tt_allocate( void * context, size_t bytes )
{
    tt_allocator_t * state = (tt_allocator_t *) context;
    ++state->calls;
    if ( bytes > TT_REFUSE_ABOVE )
    {
        ++state->refused;
        return NULL;
    }

    void * base = malloc( bytes + TT_GUARD_BYTES + 128 );
    if ( !base )
    {
        return NULL;
    }

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memset( (void *) address, TT_GUARD, bytes + TT_GUARD_BYTES );
    ++state->live;
    state->requested += bytes;
    if ( state->traced < TT_TRACE_MAX )
    {
        state->pointer[state->traced] = (uint8_t *) address;
        state->bytes[state->traced] = bytes;
        ++state->traced;
    }

    return (void *) address;
}

static void tt_release( void * context, void * pointer )
{
    tt_allocator_t * state = (tt_allocator_t *) context;
    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    --state->live;
    free( base );
}

// the worker starts initialized: terminate, install the counting allocator,
// and initialize with its state as the context
static void tt_allocator_begin( tt_allocator_t * state )
{
    memset( state, 0, sizeof( *state ) );
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );

    int status = -1;
    check( rocketnet_init_internal( state, tt_allocate, tt_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );
}

static void tt_allocator_end( tt_allocator_t * state )
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( state->live == 0 );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

static void tt_allocator_mark( tt_allocator_t * state )
{
    state->calls = 0;
    state->refused = 0;
    state->requested = 0;
    state->traced = 0;
}

// the pointer rocketnet_malloc hands out from the block the allocator returned
static uint8_t * tt_handed_out( const uint8_t * block )
{
    const uintptr_t overhead = (uintptr_t) rocketnet_allocation_overhead();
    return (uint8_t *) ( ( (uintptr_t) block + overhead ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
}

static void test_token_history_create_at_the_capacity_bounds()
{
    tt_allocator_t state;
    tt_allocator_begin( &state );
    const int live = state.live;

    // refused before any allocation
    const int refused[] = { INT_MIN, -1, 0, INT_MAX / 2 + 1, INT_MAX };
    for ( int i = 0; i < (int) ( sizeof( refused ) / sizeof( refused[0] ) ); ++i )
    {
        tt_allocator_mark( &state );
        check( rocketnet_used_token_history_create( &state, refused[i] ) == NULL );
        check( state.calls == 0 );
        check( state.live == live );
    }

    // INT_MAX / 2 is in range: create asks for each table's arrays (the
    // index alone is INT_MAX - 1 cells), the allocator refuses them, and
    // nothing stays live
    tt_allocator_mark( &state );
    check( rocketnet_used_token_history_create( &state, INT_MAX / 2 ) == NULL );
    check( state.refused > 0 );
    check( state.live == live );

    // a table in range allocates exactly its plan, and returns it all
    const int capacities[] = { 1, 2, 7 };
    for ( int i = 0; i < (int) ( sizeof( capacities ) / sizeof( capacities[0] ) ); ++i )
    {
        tt_allocator_mark( &state );
        struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( &state, capacities[i] );
        check( history );
        check( state.requested == rocketnet_token_table_allocation_bytes( capacities[i] ) );
        rocketnet_used_token_history_destroy( history );
        check( state.live == live );
    }

    tt_allocator_end( &state );
}

REGISTER_UNIT_TEST( test_token_history_create_at_the_capacity_bounds );

// rn_token_table_create's allocation order: the history, then for each table
// (bindings, then claims) the table, its index, the free stack, ...: the
// bindings index is the third allocation and the claims index the twelfth
enum
{
    TT_BINDINGS_INDEX_CALL = 2,
    TT_CLAIMS_INDEX_CALL = 11,
    TT_HISTORY_CALLS = 19
};

static int tt_corrupt_index( int * cells, int count, int from, int to )
{
    int changed = 0;
    for ( int i = 0; i < count; ++i )
    {
        if ( cells[i] == from )
        {
            cells[i] = to;
            ++changed;
        }
    }

    return changed;
}

static void test_token_index_cell_at_capacity_is_never_a_hit()
{
    tt_allocator_t state;
    tt_allocator_begin( &state );
    const int live = state.live;

    struct rocketnet_address_t address[2];
    tt_addresses( address );

    const int capacity = 4;
    tt_allocator_mark( &state );
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( &state, capacity );
    check( history );
    check( state.traced == TT_HISTORY_CALLS );

    const size_t index_bytes = (size_t) ( 2 * capacity ) * sizeof( int ) + (size_t) rocketnet_allocation_overhead();
    check( state.bytes[TT_BINDINGS_INDEX_CALL] == index_bytes );
    check( state.bytes[TT_CLAIMS_INDEX_CALL] == index_bytes );
    int * bindings_index = (int *) (void *) tt_handed_out( state.pointer[TT_BINDINGS_INDEX_CALL] );
    int * claims_index = (int *) (void *) tt_handed_out( state.pointer[TT_CLAIMS_INDEX_CALL] );

    // two live bindings and two live claims held by address 0
    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int k = 0; k < 2; ++k )
    {
        tt_id( id, k );
        rocketnet_used_token_history_record( history, id, &address[0], 2000, 1000 );
        check( rocketnet_used_token_history_claim_record( history, id, &address[0], 1010.0, 1000.0 ) == 1 );
    }

    // every empty cell of both indexes now names slot == capacity, one past the
    // last. The guard after each array reads as a live slot whose identifier is
    // all guard bytes, bound until a far time to an address of guard bytes
    const int empty = -1;
    check( tt_corrupt_index( bindings_index, 2 * capacity, empty, capacity ) == 2 * capacity - 2 );
    check( tt_corrupt_index( claims_index, 2 * capacity, empty, capacity ) == 2 * capacity - 2 );

    uint8_t probe[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    memset( probe, TT_GUARD, sizeof( probe ) );
    for ( int o = 0; o < 2; ++o )
    {
        check( rocketnet_used_token_history_check( history, probe, &address[o], 1000 ) == 1 );
        check( rocketnet_used_token_history_claim_check( history, probe, &address[o], 1000.0 ) == 1 );
    }

    // the live entries are still found past the corrupt cells
    for ( int k = 0; k < 2; ++k )
    {
        tt_id( id, k );
        check( rocketnet_used_token_history_check( history, id, &address[1], 1000 ) == 0 );
        check( rocketnet_used_token_history_check( history, id, &address[0], 1000 ) == 1 );
        check( rocketnet_used_token_history_claim_check( history, id, &address[1], 1000.0 ) == 0 );
        check( rocketnet_used_token_history_claim_check( history, id, &address[0], 1000.0 ) == 1 );
    }

    tt_id( id, 2 );
    check( rocketnet_used_token_history_check( history, id, &address[1], 1000 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, id, &address[1], 1000.0 ) == 1 );

    check( tt_corrupt_index( bindings_index, 2 * capacity, capacity, empty ) == 2 * capacity - 2 );
    check( tt_corrupt_index( claims_index, 2 * capacity, capacity, empty ) == 2 * capacity - 2 );

    rocketnet_used_token_history_destroy( history );
    check( state.live == live );

    tt_allocator_end( &state );
}

REGISTER_UNIT_TEST( test_token_index_cell_at_capacity_is_never_a_hit );
