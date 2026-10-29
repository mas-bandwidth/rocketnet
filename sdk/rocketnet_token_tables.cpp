/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_internal.h"

#include <sodium.h>
#include <string.h>
#include <limits.h>

// Each table owns fixed slots, a full-token open-addressed index over them
// and an array stack of free slots. A slot's ticket is its insertion order
// (zero is unoccupied, a uint64_t that never wraps in practice), so the
// oldest live slot is the one with the smallest ticket. Index hits validate
// the complete token against an occupied slot. Probes and backward shifts
// are bounded by the creation-time capacity.

enum rn_token_index_state_t
{
    RN_TOKEN_INDEX_EMPTY = -1
};

struct rocketnet_token_table_t
{
    void * context;
    int capacity;
    int index_capacity;
    int free_count;
    uint64_t next_ticket;
    uint8_t hash_key[crypto_shorthash_KEYBYTES];
    bool claims;

    int * index;
    int * free_slots;
    uint8_t ( *identifier )[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    struct rocketnet_address_t * address;
    uint64_t * expire_timestamp;        // bindings
    double * claim_deadline;            // claims
    uint64_t * ticket;                  // zero means unoccupied

    // claims only: the slots in insertion order, a ring of 2 x capacity
    // entries. An entry whose ticket no longer matches its slot's is a
    // tombstone (released or re-claimed); the head skips tombstones, and a
    // full ring drops them in one pass, at most once per capacity pushes.
    int * order_slot;
    uint64_t * order_ticket;
    int order_capacity;
    mutable int order_head;
    mutable int order_count;

#if ROCKETNET_DEV
    mutable uint64_t probe_steps;
    mutable uint64_t slot_steps;
#endif // #if ROCKETNET_DEV
};

struct rocketnet_used_token_history_t
{
    void * context;
    struct rocketnet_token_table_t * bindings;
    struct rocketnet_token_table_t * claims;
};

#if ROCKETNET_DEV
#define RN_TOKEN_COUNT( table, field, amount ) ( (table)->field += (uint64_t) ( amount ) )
#else
#define RN_TOKEN_COUNT( table, field, amount ) do { (void) (table); } while ( 0 )
#endif // #if ROCKETNET_DEV

static bool rn_token_slot_live( const struct rocketnet_token_table_t * table, int slot )
{
    return slot >= 0 && slot < table->capacity && table->ticket[slot] != 0;
}

uint64_t rocketnet_token_table_allocation_bytes( int capacity )
{
    if ( capacity < 1 || capacity > INT_MAX / 2 )
        return 0;

    const uint64_t n = (uint64_t) capacity;
    const uint64_t index_n = 2 * n;
    return rocketnet_memory_allocations( { sizeof( rocketnet_used_token_history_t ),
                                           sizeof( rocketnet_token_table_t ), index_n * sizeof( int ), n * sizeof( int ),
                                           n * ROCKETNET_TOKEN_IDENTIFIER_BYTES, n * sizeof( rocketnet_address_t ),
                                           n * sizeof( uint64_t ), n * sizeof( uint64_t ),
                                           sizeof( rocketnet_token_table_t ), index_n * sizeof( int ), n * sizeof( int ),
                                           n * ROCKETNET_TOKEN_IDENTIFIER_BYTES, n * sizeof( rocketnet_address_t ),
                                           n * sizeof( double ), n * sizeof( uint64_t ),
                                           index_n * sizeof( int ), index_n * sizeof( uint64_t ) } );
}

static void rn_token_table_destroy( struct rocketnet_token_table_t * table )
{
    if ( !table )
        return;

    rocketnet_free( table->context, table->index );
    rocketnet_free( table->context, table->free_slots );
    rocketnet_free( table->context, table->identifier );
    rocketnet_free( table->context, table->address );
    rocketnet_free( table->context, table->expire_timestamp );
    rocketnet_free( table->context, table->claim_deadline );
    rocketnet_free( table->context, table->ticket );
    rocketnet_free( table->context, table->order_slot );
    rocketnet_free( table->context, table->order_ticket );
    rocketnet_free( table->context, table );
}

static struct rocketnet_token_table_t * rn_token_table_create( void * context, int capacity, bool claims )
{
    if ( capacity < 1 || capacity > INT_MAX / 2 )
        return NULL;

    struct rocketnet_token_table_t * table =
        (struct rocketnet_token_table_t *) rocketnet_malloc( context, sizeof( struct rocketnet_token_table_t ) );
    if ( !table )
        return NULL;

    memset( table, 0, sizeof( *table ) );

    table->context = context;
    table->capacity = capacity;
    table->index_capacity = capacity * 2;
    table->next_ticket = 1;
    table->claims = claims;
    randombytes_buf( table->hash_key, sizeof( table->hash_key ) );

    table->index = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) table->index_capacity );
    table->free_slots = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
    table->identifier = (uint8_t( * )[ROCKETNET_TOKEN_IDENTIFIER_BYTES]) rocketnet_malloc( context, ROCKETNET_TOKEN_IDENTIFIER_BYTES * (size_t) capacity );
    table->address = (struct rocketnet_address_t *) rocketnet_malloc( context, sizeof( struct rocketnet_address_t ) * (size_t) capacity );
    if ( claims )
    {
        table->claim_deadline = (double *) rocketnet_malloc( context, sizeof( double ) * (size_t) capacity );
        table->order_capacity = table->index_capacity;
        table->order_slot = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) table->order_capacity );
        table->order_ticket = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) table->order_capacity );
    }
    else
        table->expire_timestamp = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    table->ticket = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );

    if ( !table->index || !table->free_slots || !table->identifier || !table->address ||
         ( claims ? ( !table->claim_deadline || !table->order_slot || !table->order_ticket ) : !table->expire_timestamp ) || !table->ticket )
    {
        rn_token_table_destroy( table );
        return NULL;
    }

    for ( int i = 0; i < table->index_capacity; ++i )
        table->index[i] = RN_TOKEN_INDEX_EMPTY;

    memset( table->identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES * (size_t) capacity );
    memset( table->address, 0, sizeof( struct rocketnet_address_t ) * (size_t) capacity );
    if ( table->expire_timestamp )
        memset( table->expire_timestamp, 0, sizeof( uint64_t ) * (size_t) capacity );
    if ( table->claim_deadline )
        memset( table->claim_deadline, 0, sizeof( double ) * (size_t) capacity );
    memset( table->ticket, 0, sizeof( uint64_t ) * (size_t) capacity );

    for ( int slot = 0; slot < capacity; ++slot )
        table->free_slots[capacity - 1 - slot] = slot;

    table->free_count = capacity;

    return table;
}

static int rn_token_index_start( const struct rocketnet_token_table_t * table,
                                 const uint8_t * identifier )
{
    uint8_t hash[crypto_shorthash_BYTES];
    crypto_shorthash( hash, identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES, table->hash_key );

    uint64_t value;
    memcpy( &value, hash, sizeof( value ) );

    return (int) ( value % (uint64_t) table->index_capacity );
}

static int rn_token_index_position( const struct rocketnet_token_table_t * table,
                                    const uint8_t * identifier )
{
    const int start = rn_token_index_start( table, identifier );

    for ( int probe = 0; probe < table->index_capacity; ++probe )
    {
        RN_TOKEN_COUNT( table, probe_steps, 1 );

        const int position = ( start + probe ) % table->index_capacity;
        const int slot = table->index[position];
        if ( slot == RN_TOKEN_INDEX_EMPTY )
            return -1;

        if ( rn_token_slot_live( table, slot ) &&
             sodium_memcmp( table->identifier[slot], identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 )
            return position;
    }

    return -1;
}

static int rn_token_table_find( const struct rocketnet_token_table_t * table,
                                const uint8_t * identifier )
{
    const int position = rn_token_index_position( table, identifier );
    return position >= 0 ? table->index[position] : -1;
}

static bool rn_token_index_insert( struct rocketnet_token_table_t * table, int slot )
{
    const int start = rn_token_index_start( table, table->identifier[slot] );

    for ( int probe = 0; probe < table->index_capacity; ++probe )
    {
        RN_TOKEN_COUNT( table, probe_steps, 1 );

        const int position = ( start + probe ) % table->index_capacity;
        if ( table->index[position] == RN_TOKEN_INDEX_EMPTY )
        {
            table->index[position] = slot;
            return true;
        }
    }

    return false;
}

// Backward-shift deletion (as in rocketnet_client_slots.cpp): clearing the
// hole and shifting every later entry whose home is not cyclically in
// (hole, scan] back into the hole keeps every surviving probe cluster
// searchable and leaves no tombstone. The index holds at most capacity
// entries in twice that many cells, so an empty cell always stops the scan.
// The scan does not count as a lookup probe.
static void rn_token_index_remove( struct rocketnet_token_table_t * table, int slot )
{
    const int position = rn_token_index_position( table, table->identifier[slot] );
    rocketnet_assert( position >= 0 && table->index[position] == slot );
    if ( position < 0 )
        return;

    table->index[position] = RN_TOKEN_INDEX_EMPTY;
    int hole = position;
    int scan = hole;

    for ( int step = 0; step < table->index_capacity; ++step )
    {
        scan = scan + 1 == table->index_capacity ? 0 : scan + 1;
        const int entry = table->index[scan];
        if ( entry == RN_TOKEN_INDEX_EMPTY )
            break;

        const int home = rn_token_index_start( table, table->identifier[entry] );
        const bool stays = hole <= scan ? ( hole < home && home <= scan )
                                        : ( hole < home || home <= scan );
        if ( stays )
            continue;

        table->index[hole] = entry;
        table->index[scan] = RN_TOKEN_INDEX_EMPTY;
        hole = scan;
    }
}

static int rn_token_free_pop( struct rocketnet_token_table_t * table )
{
    if ( table->free_count == 0 )
        return -1;

    const int slot = table->free_slots[--table->free_count];
    rocketnet_assert( slot >= 0 && slot < table->capacity && !rn_token_slot_live( table, slot ) );
    return slot;
}

static void rn_token_free_push( struct rocketnet_token_table_t * table, int slot )
{
    rocketnet_assert( !rn_token_slot_live( table, slot ) && table->free_count < table->capacity );
    table->free_slots[table->free_count++] = slot;
}

// the live slot inserted first, -1 when none is live
static int rn_token_oldest( const struct rocketnet_token_table_t * table )
{
    int oldest = -1;
    for ( int slot = 0; slot < table->capacity; ++slot )
    {
        RN_TOKEN_COUNT( table, slot_steps, 1 );

        if ( rn_token_slot_live( table, slot ) && ( oldest == -1 || table->ticket[slot] < table->ticket[oldest] ) )
            oldest = slot;
    }

    return oldest;
}

static void rn_token_slot_insert( struct rocketnet_token_table_t * table,
                                  int slot,
                                  const uint8_t * identifier,
                                  const struct rocketnet_address_t * address,
                                  uint64_t expire_timestamp,
                                  double claim_deadline )
{
    rocketnet_assert( slot >= 0 && slot < table->capacity && !rn_token_slot_live( table, slot ) );

    memcpy( table->identifier[slot], identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES );

    if ( address )
        table->address[slot] = *address;
    else
        memset( &table->address[slot], 0, sizeof( table->address[slot] ) );

    if ( table->claims )
        table->claim_deadline[slot] = claim_deadline;
    else
        table->expire_timestamp[slot] = expire_timestamp;

    table->ticket[slot] = table->next_ticket++;

    // at most capacity entries in twice that many cells: an empty cell exists
    const bool indexed = rn_token_index_insert( table, slot );
    rocketnet_assert( indexed );
    (void) indexed;
}

static void rn_token_slot_retire( struct rocketnet_token_table_t * table, int slot )
{
    rocketnet_assert( rn_token_slot_live( table, slot ) );
    rn_token_index_remove( table, slot );
    table->ticket[slot] = 0;
}

// Bindings retain the exact documented selection: a free slot, otherwise the
// first expired physical slot, otherwise the oldest insertion.
static int rn_token_binding_select_slot( struct rocketnet_token_table_t * table,
                                         uint64_t current_timestamp )
{
    const int free_slot = rn_token_free_pop( table );
    if ( free_slot >= 0 )
        return free_slot;

    for ( int slot = 0; slot < table->capacity; ++slot )
    {
        RN_TOKEN_COUNT( table, slot_steps, 1 );
        rocketnet_assert( rn_token_slot_live( table, slot ) );

        if ( table->expire_timestamp[slot] <= current_timestamp )
        {
            rn_token_slot_retire( table, slot );
            return slot;
        }
    }

    const int oldest = rn_token_oldest( table );
    if ( oldest == -1 )
        return -1;

    rn_token_slot_retire( table, oldest );
    return oldest;
}

static bool rn_token_order_stale( const struct rocketnet_token_table_t * table, int position )
{
    const int slot = table->order_slot[position];
    return table->ticket[slot] != table->order_ticket[position];
}

// the oldest live claim, -1 when none: tombstones at the head are dropped
static int rn_token_order_oldest( const struct rocketnet_token_table_t * table )
{
    while ( table->order_count > 0 && rn_token_order_stale( table, table->order_head ) )
    {
        RN_TOKEN_COUNT( table, slot_steps, 1 );
        table->order_head = ( table->order_head + 1 ) % table->order_capacity;
        --table->order_count;
    }

    return table->order_count > 0 ? table->order_slot[table->order_head] : -1;
}

static void rn_token_order_push( struct rocketnet_token_table_t * table, int slot )
{
    if ( table->order_count == table->order_capacity )
    {
        // at most capacity entries are live, so the pass frees at least half
        int kept = 0;
        for ( int i = 0; i < table->order_count; ++i )
        {
            RN_TOKEN_COUNT( table, slot_steps, 1 );
            const int position = ( table->order_head + i ) % table->order_capacity;
            if ( rn_token_order_stale( table, position ) )
                continue;
            table->order_slot[kept] = table->order_slot[position];
            table->order_ticket[kept] = table->order_ticket[position];
            ++kept;
        }
        table->order_head = 0;
        table->order_count = kept;
    }

    const int tail = ( table->order_head + table->order_count ) % table->order_capacity;
    table->order_slot[tail] = slot;
    table->order_ticket[tail] = table->ticket[slot];
    ++table->order_count;
}

// Claims take a free slot, otherwise the oldest claim once its deadline has
// passed; a full table of live claims refuses. Claims share one handshake
// deadline, so the oldest insertion is also the first to expire.
static int rn_token_claim_select_slot( const struct rocketnet_token_table_t * table, double current_time )
{
    if ( table->free_count > 0 )
        return table->free_slots[table->free_count - 1];

    const int oldest = rn_token_order_oldest( table );
    if ( oldest == -1 || table->claim_deadline[oldest] > current_time )
        return -1;

    return oldest;
}

struct rocketnet_used_token_history_t * rocketnet_used_token_history_create( void * context, int capacity )
{
    if ( capacity < 1 || capacity > INT_MAX / 2 )
        return NULL;

    struct rocketnet_used_token_history_t * history =
        (struct rocketnet_used_token_history_t *) rocketnet_malloc( context, sizeof( struct rocketnet_used_token_history_t ) );
    if ( !history )
        return NULL;

    memset( history, 0, sizeof( *history ) );

    history->context = context;
    history->bindings = rn_token_table_create( context, capacity, false );
    history->claims = rn_token_table_create( context, capacity, true );

    if ( !history->bindings || !history->claims )
    {
        rn_token_table_destroy( history->bindings );
        rn_token_table_destroy( history->claims );
        rocketnet_free( context, history );
        return NULL;
    }

    return history;
}

void rocketnet_used_token_history_destroy( struct rocketnet_used_token_history_t * history )
{
    if ( !history )
        return;

    rn_token_table_destroy( history->bindings );
    rn_token_table_destroy( history->claims );
    rocketnet_free( history->context, history );
}

int rocketnet_used_token_history_check( const struct rocketnet_used_token_history_t * history,
                                        const uint8_t * token_identifier,
                                        const struct rocketnet_address_t * address,
                                        uint64_t current_timestamp )
{
    const struct rocketnet_token_table_t * table = history->bindings;
    const int slot = rn_token_table_find( table, token_identifier );
    if ( slot == -1 || table->expire_timestamp[slot] <= current_timestamp )
        return 1;

    return rocketnet_address_equal( &table->address[slot], address ) ? 1 : 0;
}

void rocketnet_used_token_history_record( struct rocketnet_used_token_history_t * history,
                                          const uint8_t * token_identifier,
                                          const struct rocketnet_address_t * address,
                                          uint64_t expire_timestamp,
                                          uint64_t current_timestamp )
{
    struct rocketnet_token_table_t * table = history->bindings;
    int slot = rn_token_table_find( table, token_identifier );
    if ( slot != -1 )
    {
        if ( table->expire_timestamp[slot] > current_timestamp )
            return;

        rn_token_slot_retire( table, slot );
    }
    else
    {
        slot = rn_token_binding_select_slot( table, current_timestamp );
        if ( slot == -1 )
            return;
    }

    rn_token_slot_insert( table, slot, token_identifier, address, expire_timestamp, 0.0 );
}

int rocketnet_used_token_history_claim_check( const struct rocketnet_used_token_history_t * history,
                                              const uint8_t * token_identifier,
                                              const struct rocketnet_address_t * address,
                                              double current_time )
{
    const struct rocketnet_token_table_t * table = history->claims;
    const int slot = rn_token_table_find( table, token_identifier );
    if ( slot == -1 || table->claim_deadline[slot] <= current_time )
        return 1;

    return rocketnet_address_equal( &table->address[slot], address ) ? 1 : 0;
}

int rocketnet_used_token_history_claim_record( struct rocketnet_used_token_history_t * history,
                                               const uint8_t * token_identifier,
                                               const struct rocketnet_address_t * address,
                                               double deadline,
                                               double current_time )
{
    struct rocketnet_token_table_t * table = history->claims;
    int slot = rn_token_table_find( table, token_identifier );
    if ( slot != -1 )
    {
        if ( table->claim_deadline[slot] > current_time )
            return 1;

        // an expired claim of the same token is claimed again, as the newest
        rn_token_slot_retire( table, slot );
    }
    else
    {
        slot = rn_token_claim_select_slot( table, current_time );
        if ( slot == -1 )
            return 0;

        if ( table->free_count > 0 && table->free_slots[table->free_count - 1] == slot )
            --table->free_count;
        else
            rn_token_slot_retire( table, slot );
    }

    rn_token_slot_insert( table, slot, token_identifier, address, 0, deadline );
    rn_token_order_push( table, slot );
    return 1;
}

int rocketnet_used_token_history_claim_can_record( const struct rocketnet_used_token_history_t * history,
                                                   const uint8_t * token_identifier,
                                                   double current_time )
{
    const struct rocketnet_token_table_t * table = history->claims;
    if ( rn_token_table_find( table, token_identifier ) != -1 )
        return 1;

    return rn_token_claim_select_slot( table, current_time ) != -1;
}

int rocketnet_used_token_history_claim_free_count( const struct rocketnet_used_token_history_t * history )
{
    return history->claims->free_count;
}

void rocketnet_used_token_history_claim_release( struct rocketnet_used_token_history_t * history,
                                                 const uint8_t * token_identifier,
                                                 const struct rocketnet_address_t * address )
{
    struct rocketnet_token_table_t * table = history->claims;
    const int slot = rn_token_table_find( table, token_identifier );
    if ( slot == -1 || !rocketnet_address_equal( &table->address[slot], address ) )
        return;

    rn_token_slot_retire( table, slot );
    table->claim_deadline[slot] = 0.0;
    rn_token_free_push( table, slot );
}

#if ROCKETNET_DEV
extern "C" void rocketnet_dev_token_table_reset_operation_counts( struct rocketnet_used_token_history_t * history )
{
    history->bindings->probe_steps = history->bindings->slot_steps = 0;
    history->claims->probe_steps = history->claims->slot_steps = 0;
}

extern "C" uint64_t rocketnet_dev_token_claim_probe_steps( const struct rocketnet_used_token_history_t * history )
{
    return history->claims->probe_steps;
}

extern "C" uint64_t rocketnet_dev_token_claim_slot_steps( const struct rocketnet_used_token_history_t * history )
{
    return history->claims->slot_steps;
}

extern "C" int rocketnet_dev_token_claim_index_start( const struct rocketnet_used_token_history_t * history,
                                                      const uint8_t * token_identifier )
{
    return rn_token_index_start( history->claims, token_identifier );
}
#endif // #if ROCKETNET_DEV
