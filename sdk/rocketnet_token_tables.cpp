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

// Each independent table owns fixed generation-tagged slots, a full-token
// open-addressed index, an array free-slot stack and, for claims, a dense
// chronological slot array. Index hits validate the complete token against an
// occupied slot; every recycle advances that slot's generation without wrap.
// The dense arrays remove their entry before reuse, so no stale reference is
// retained. Linear probing and shifts are bounded by creation-time capacity.

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
    int order_count;
    uint64_t owner_epoch;
    uint64_t next_ticket;               // zero is the exhausted state
    uint8_t hash_key[crypto_shorthash_KEYBYTES];
    bool claims;

    int * index;
    int * free_slots;
    int * order_slots;
    uint64_t * order_generations;
    uint8_t ( *identifier )[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    struct rocketnet_address_t * address;
    uint64_t * expire_timestamp;
    double * claim_deadline;
    uint64_t * ticket;                  // zero means unoccupied
    uint64_t * generation;

#if ROCKETNET_DEV
    mutable uint64_t probe_steps;
    mutable uint64_t order_steps;
    mutable uint64_t slot_steps;
#endif
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
#endif

static bool rn_token_slot_live( const struct rocketnet_token_table_t * table, int slot )
{
    return slot >= 0 && slot < table->capacity && table->ticket[slot] != 0;
}

static bool rn_token_handle_current( const struct rocketnet_token_table_t * table,
                                     int slot,
                                     uint64_t generation )
{
    return slot >= 0 && slot < table->capacity &&
           table->generation[slot] == generation;
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
                                           n * sizeof( uint64_t ), n * sizeof( uint64_t ), n * sizeof( uint64_t ),
                                           sizeof( rocketnet_token_table_t ), index_n * sizeof( int ), n * sizeof( int ),
                                           n * sizeof( int ), n * sizeof( uint64_t ),
                                           n * ROCKETNET_TOKEN_IDENTIFIER_BYTES, n * sizeof( rocketnet_address_t ),
                                           n * sizeof( double ), n * sizeof( uint64_t ), n * sizeof( uint64_t ) } );
}

static void rn_token_table_destroy( struct rocketnet_token_table_t * table )
{
    if ( !table )
        return;
    rocketnet_free( table->context, table->index );
    rocketnet_free( table->context, table->free_slots );
    rocketnet_free( table->context, table->order_slots );
    rocketnet_free( table->context, table->order_generations );
    rocketnet_free( table->context, table->identifier );
    rocketnet_free( table->context, table->address );
    rocketnet_free( table->context, table->expire_timestamp );
    rocketnet_free( table->context, table->claim_deadline );
    rocketnet_free( table->context, table->ticket );
    rocketnet_free( table->context, table->generation );
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
    table->owner_epoch = 1;
    table->next_ticket = 1;
    table->claims = claims;
    randombytes_buf( table->hash_key, sizeof( table->hash_key ) );

    table->index = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) table->index_capacity );
    table->free_slots = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
    if ( claims )
    {
        table->order_slots = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
        table->order_generations = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    }
    table->identifier = (uint8_t( * )[ROCKETNET_TOKEN_IDENTIFIER_BYTES]) rocketnet_malloc( context, ROCKETNET_TOKEN_IDENTIFIER_BYTES * (size_t) capacity );
    table->address = (struct rocketnet_address_t *) rocketnet_malloc( context, sizeof( struct rocketnet_address_t ) * (size_t) capacity );
    if ( claims )
        table->claim_deadline = (double *) rocketnet_malloc( context, sizeof( double ) * (size_t) capacity );
    else
        table->expire_timestamp = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    table->ticket = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    table->generation = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    if ( !table->index || !table->free_slots ||
         ( claims && ( !table->order_slots || !table->order_generations ) ) || !table->identifier ||
         !table->address || ( claims ? !table->claim_deadline : !table->expire_timestamp ) ||
         !table->ticket || !table->generation )
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
    memset( table->generation, 0, sizeof( uint64_t ) * (size_t) capacity );
    for ( int slot = 0; slot < capacity; ++slot )
    {
        const int free_position = capacity - 1 - slot;
        table->free_slots[free_position] = slot;
    }
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
        if ( slot >= 0 && rn_token_slot_live( table, slot ) &&
             sodium_memcmp( table->identifier[slot], identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 )
            return position;
    }
    return -1;
}

static int rn_token_table_find_any( const struct rocketnet_token_table_t * table,
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
        const int entry = table->index[position];
        if ( entry == RN_TOKEN_INDEX_EMPTY )
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
        if ( entry < 0 )
            continue;
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

static bool rn_token_can_insert( const struct rocketnet_token_table_t * table, int slot )
{
    return table->next_ticket != 0 && table->generation[slot] != UINT64_MAX;
}

static int rn_token_free_pop( struct rocketnet_token_table_t * table )
{
    if ( table->free_count == 0 )
        return -1;
    const int position = table->free_count - 1;
    const int slot = table->free_slots[position];
    if ( slot < 0 || slot >= table->capacity || rn_token_slot_live( table, slot ) )
        return -1;
    --table->free_count;
    return slot;
}

static void rn_token_free_push( struct rocketnet_token_table_t * table, int slot )
{
    rocketnet_assert( !rn_token_slot_live( table, slot ) && table->free_count < table->capacity );
    table->free_slots[table->free_count] = slot;
    ++table->free_count;
}

static void rn_token_order_append( struct rocketnet_token_table_t * table, int slot )
{
    rocketnet_assert( table->claims && table->order_count < table->capacity );
    table->order_slots[table->order_count] = slot;
    table->order_generations[table->order_count] = table->generation[slot];
    ++table->order_count;
}

static bool rn_token_order_remove( struct rocketnet_token_table_t * table, int slot )
{
    rocketnet_assert( table->claims );
    int position = -1;
    for ( int i = 0; i < table->order_count; ++i )
    {
        RN_TOKEN_COUNT( table, order_steps, 1 );
        if ( table->order_slots[i] == slot &&
             rn_token_handle_current( table, slot, table->order_generations[i] ) )
        {
            position = i;
            break;
        }
    }
    if ( position < 0 )
        return false;
    for ( int i = position + 1; i < table->order_count; ++i )
    {
        RN_TOKEN_COUNT( table, order_steps, 1 );
        table->order_slots[i - 1] = table->order_slots[i];
        table->order_generations[i - 1] = table->order_generations[i];
    }
    --table->order_count;
    return true;
}

static int rn_token_order_head( const struct rocketnet_token_table_t * table )
{
    if ( table->order_count == 0 )
        return -1;
    const int slot = table->order_slots[0];
    if ( !rn_token_handle_current( table, slot, table->order_generations[0] ) ||
         !rn_token_slot_live( table, slot ) )
        return -1;
    return slot;
}

static bool rn_token_slot_insert( struct rocketnet_token_table_t * table,
                                  int slot,
                                  const uint8_t * identifier,
                                  const struct rocketnet_address_t * address,
                                  uint64_t expire_timestamp,
                                  double claim_deadline )
{
    rocketnet_assert( slot >= 0 && slot < table->capacity && !rn_token_slot_live( table, slot ) );
    if ( !rn_token_can_insert( table, slot ) )
        return false;
    ++table->generation[slot];
    memcpy( table->identifier[slot], identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    if ( address )
        table->address[slot] = *address;
    else
        memset( &table->address[slot], 0, sizeof( table->address[slot] ) );
    if ( table->claims )
        table->claim_deadline[slot] = claim_deadline;
    else
        table->expire_timestamp[slot] = expire_timestamp;
    table->ticket[slot] = table->next_ticket;
    table->next_ticket = table->next_ticket == UINT64_MAX ? 0 : table->next_ticket + 1;
    if ( !rn_token_index_insert( table, slot ) )
    {
        table->ticket[slot] = 0;
        return false;
    }
    return true;
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
    int oldest = -1;
    for ( int slot = 0; slot < table->capacity; ++slot )
    {
        RN_TOKEN_COUNT( table, slot_steps, 1 );
        rocketnet_assert( rn_token_slot_live( table, slot ) );
        if ( table->expire_timestamp[slot] <= current_timestamp )
        {
            if ( !rn_token_can_insert( table, slot ) )
                return -1;
            rn_token_slot_retire( table, slot );
            return slot;
        }
        if ( oldest == -1 || table->ticket[slot] < table->ticket[oldest] )
            oldest = slot;
    }
    if ( oldest == -1 || !rn_token_can_insert( table, oldest ) )
        return -1;
    rn_token_slot_retire( table, oldest );
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
    const int slot = rn_token_table_find_any( table, token_identifier );
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
    int slot = rn_token_table_find_any( table, token_identifier );
    if ( slot != -1 )
    {
        if ( table->expire_timestamp[slot] > current_timestamp )
            return;
        if ( !rn_token_can_insert( table, slot ) )
            return;
        rn_token_slot_retire( table, slot );
    }
    else
    {
        slot = rn_token_binding_select_slot( table, current_timestamp );
        if ( slot == -1 )
            return;
    }
    const bool inserted = rn_token_slot_insert( table, slot, token_identifier, address, expire_timestamp, 0.0 );
    rocketnet_assert( inserted );
    if ( !inserted )
        rn_token_free_push( table, slot );
    (void) inserted;
}

int rocketnet_used_token_history_claim_check( const struct rocketnet_used_token_history_t * history,
                                              const uint8_t * token_identifier,
                                              const struct rocketnet_address_t * address,
                                              double current_time )
{
    const struct rocketnet_token_table_t * table = history->claims;
    const int slot = rn_token_table_find_any( table, token_identifier );
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
    int slot = rn_token_table_find_any( table, token_identifier );
    if ( slot != -1 )
    {
        if ( table->claim_deadline[slot] > current_time )
            return 1;
        if ( !rn_token_can_insert( table, slot ) )
            return 0;
        if ( !rn_token_order_remove( table, slot ) )
            return 0;
        rn_token_slot_retire( table, slot );
    }
    else
    {
        const int free_slot = rn_token_free_pop( table );
        if ( free_slot >= 0 )
            slot = free_slot;
        else
        {
            slot = rn_token_order_head( table );
            if ( slot == -1 || table->claim_deadline[slot] > current_time ||
                 !rn_token_can_insert( table, slot ) )
                return 0;
            if ( !rn_token_order_remove( table, slot ) )
                return 0;
            rn_token_slot_retire( table, slot );
        }
    }
    if ( !rn_token_slot_insert( table, slot, token_identifier, address, 0, deadline ) )
    {
        rn_token_free_push( table, slot );
        return 0;
    }
    rn_token_order_append( table, slot );
    return 1;
}

int rocketnet_used_token_history_claim_can_record( const struct rocketnet_used_token_history_t * history,
                                                   const uint8_t * token_identifier,
                                                   double current_time )
{
    const struct rocketnet_token_table_t * table = history->claims;
    const int found = rn_token_table_find_any( table, token_identifier );
    if ( found != -1 )
        return table->claim_deadline[found] > current_time || rn_token_can_insert( table, found );
    if ( table->free_count > 0 )
    {
        const int position = table->free_count - 1;
        const int slot = table->free_slots[position];
        return slot >= 0 && slot < table->capacity && !rn_token_slot_live( table, slot ) &&
               rn_token_can_insert( table, slot );
    }
    const int slot = rn_token_order_head( table );
    return slot != -1 && table->claim_deadline[slot] <= current_time && rn_token_can_insert( table, slot );
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
    const int slot = rn_token_table_find_any( table, token_identifier );
    if ( slot == -1 || !rocketnet_address_equal( &table->address[slot], address ) )
        return;
    if ( !rn_token_order_remove( table, slot ) )
        return;
    rn_token_slot_retire( table, slot );
    table->claim_deadline[slot] = 0.0;
    rn_token_free_push( table, slot );
}

#if ROCKETNET_DEV
extern "C" void rocketnet_dev_token_table_reset_operation_counts( struct rocketnet_used_token_history_t * history )
{
    history->bindings->probe_steps = history->bindings->order_steps = history->bindings->slot_steps = 0;
    history->claims->probe_steps = history->claims->order_steps = history->claims->slot_steps = 0;
}

extern "C" uint64_t rocketnet_dev_token_claim_probe_steps( const struct rocketnet_used_token_history_t * history )
{
    return history->claims->probe_steps;
}

extern "C" uint64_t rocketnet_dev_token_claim_order_steps( const struct rocketnet_used_token_history_t * history )
{
    return history->claims->order_steps;
}

extern "C" int rocketnet_dev_token_claim_index_start( const struct rocketnet_used_token_history_t * history,
                                                      const uint8_t * token_identifier )
{
    return rn_token_index_start( history->claims, token_identifier );
}

extern "C" void rocketnet_dev_token_claim_set_wide_state( struct rocketnet_used_token_history_t * history,
                                                          uint64_t generation,
                                                          uint64_t next_ticket )
{
    struct rocketnet_token_table_t * table = history->claims;
    for ( int slot = 0; slot < table->capacity; ++slot )
        table->generation[slot] = generation;
    table->next_ticket = next_ticket;
}

extern "C" int rocketnet_dev_token_claim_reset( struct rocketnet_used_token_history_t * history )
{
    struct rocketnet_token_table_t * table = history->claims;
    if ( table->owner_epoch == UINT64_MAX )
        return 0;
    ++table->owner_epoch;
    table->next_ticket = 1;
    table->order_count = 0;
    table->free_count = table->capacity;
    for ( int i = 0; i < table->index_capacity; ++i )
        table->index[i] = RN_TOKEN_INDEX_EMPTY;
    for ( int slot = 0; slot < table->capacity; ++slot )
    {
        table->ticket[slot] = 0;
        table->generation[slot] = 0;
        const int free_position = table->capacity - 1 - slot;
        table->free_slots[free_position] = slot;
    }
    return 1;
}

extern "C" int rocketnet_dev_token_claim_head_handle( const struct rocketnet_used_token_history_t * history,
                                                      int * slot,
                                                      uint64_t * generation,
                                                      uint64_t * owner_epoch )
{
    const struct rocketnet_token_table_t * table = history->claims;
    if ( table->order_count == 0 || !slot || !generation || !owner_epoch )
        return 0;
    *slot = table->order_slots[0];
    *generation = table->order_generations[0];
    *owner_epoch = table->owner_epoch;
    return rn_token_handle_current( table, *slot, *generation ) ? 1 : 0;
}

extern "C" int rocketnet_dev_token_claim_remove_handle( struct rocketnet_used_token_history_t * history,
                                                        int slot,
                                                        uint64_t generation,
                                                        uint64_t owner_epoch )
{
    struct rocketnet_token_table_t * table = history->claims;
    if ( owner_epoch != table->owner_epoch ||
         !rn_token_handle_current( table, slot, generation ) || !rn_token_slot_live( table, slot ) )
        return 0;
    if ( !rn_token_order_remove( table, slot ) )
        return 0;
    rn_token_slot_retire( table, slot );
    rn_token_free_push( table, slot );
    return 1;
}
#endif
