/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_internal.h"
#include "rocketnet_connection_reset.h"

#include <sodium.h>
#include <string.h>
#include <limits.h>

// index_table owns one allocation containing this optional development-only
// header followed by num_buckets slot indices (-1 when empty). Full IDs and
// generations live in the fixed slots and are checked through each index.
// In DEV=0 the header is zero bytes by construction and the allocation is
// only the product index.
struct rocketnet_client_slot_index_t
{
#if ROCKETNET_DEV
    uint64_t last_probes;
    uint64_t total_probes;
    int forced_hash_index;
#endif // #if ROCKETNET_DEV
};

static size_t rocketnet_client_slot_index_header_bytes()
{
#if ROCKETNET_DEV
    return sizeof( struct rocketnet_client_slot_index_t );
#else // #if ROCKETNET_DEV
    return 0;
#endif // #if ROCKETNET_DEV
}

/*
    The declaration manifest: the struct is declared from this list, so a
    member exists only as an entry here. The RESET/KEEP inventory in
    rocketnet_connection_reset.h is compared with it by name, and a member with
    no decision is named in test_connection_reset_inventory_covers_every_field.
*/
#define ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS( X ) \
    X( context, void * context )                                                      \
    X( max_clients, int max_clients )                                                 \
    X( num_connected, int num_connected )                                             \
    X( hash_key, uint8_t hash_key[crypto_shorthash_KEYBYTES] )                        \
    X( index_table, struct rocketnet_client_slot_index_t * index_table ) /* flat full-id index allocation */ \
    X( slot_generation, uint64_t * slot_generation ) /* occupied generation per numeric slot */ \
    X( num_buckets, int num_buckets ) /* the free list: a stack of unused client indices, popped on connect */ /* and pushed on disconnect, so finding a free slot is constant time in */ /* the number of client slots and never a search (standard/RATIONALE.md, */ /* "Lookup complexity": "a free slot is found from a free list, not a */ /* search") */                                                                    \
    X( free_stack, int * free_stack ) /* max_clients, unused indices */               \
    X( free_stack_count, int free_stack_count ) /* hot: walked by find */                                                         \
    X( client_id, uint64_t * client_id )                                              \
    X( connected, uint8_t * connected ) /* cold: same index */                                                            \
    X( mapping_index, int * mapping_index )                                           \
    X( confirmed, uint8_t * confirmed )                                               \
    X( user_data, uint8_t * user_data ) /* max_clients * ROCKETNET_USER_DATA_BYTES */

struct rocketnet_client_slots_t
{
#define ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS( ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS_DECLARE )
#undef ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS_DECLARE
};

static int * rocketnet_client_slot_index_entries( struct rocketnet_client_slots_t * slots )
{
    return (int *) ( (uint8_t *) slots->index_table + rocketnet_client_slot_index_header_bytes() );
}

static const int * rocketnet_client_slot_index_entries( const struct rocketnet_client_slots_t * slots )
{
    return (const int *) ( (const uint8_t *) slots->index_table + rocketnet_client_slot_index_header_bytes() );
}

static void rocketnet_client_slots_probe_begin( const struct rocketnet_client_slots_t * slots )
{
#if ROCKETNET_DEV
    slots->index_table->last_probes = 0;
#else // #if ROCKETNET_DEV
    (void) slots;
#endif // #if ROCKETNET_DEV
}

static void rocketnet_client_slots_probe_one( const struct rocketnet_client_slots_t * slots )
{
#if ROCKETNET_DEV
    slots->index_table->last_probes++;
    slots->index_table->total_probes++;
#else // #if ROCKETNET_DEV
    (void) slots;
#endif // #if ROCKETNET_DEV
}

uint64_t rocketnet_client_slots_allocation_bytes( int clients )
{
    if ( clients < 1 )
        return 0;

    const uint64_t n = (uint64_t) clients;
    uint64_t index_bytes = (uint64_t) rocketnet_client_slot_index_header_bytes();
    if ( !rocketnet_memory_add( index_bytes, 2 * n, sizeof( int ) ) )
        return 0;

    return rocketnet_memory_allocations( { sizeof( rocketnet_client_slots_t ), index_bytes,
                                           n * sizeof( uint64_t ), n * sizeof( uint64_t ), n * sizeof( uint8_t ), n * sizeof( int ), n * sizeof( uint8_t ), n * sizeof( int ),
                                           n * ROCKETNET_USER_DATA_BYTES } );
}

struct rocketnet_client_slots_t * rocketnet_client_slots_create( void * context, int max_clients )
{
    rocketnet_assert( max_clients > 0 );

    // num_buckets is max_clients * 2 in a signed int, so a max_clients above
    // INT_MAX / 2 wraps the bucket count and the bucket array is sized from
    // the wrapped value while the other arrays use the capacity. refuse it
    // before any allocation.

    if ( max_clients < 1 || max_clients > INT_MAX / 2 )
        return NULL;

    struct rocketnet_client_slots_t * slots = (struct rocketnet_client_slots_t *) rocketnet_malloc( context, sizeof( struct rocketnet_client_slots_t ) );
    if ( !slots )
        return NULL;

    memset( slots, 0, sizeof( struct rocketnet_client_slots_t ) );

    slots->context = context;
    slots->max_clients = max_clients;
    slots->num_buckets = max_clients * 2;
    randombytes_buf( slots->hash_key, sizeof( slots->hash_key ) );

    const size_t index_bytes = rocketnet_client_slot_index_header_bytes() +
                               sizeof( int ) * (size_t) slots->num_buckets;
    slots->index_table = (struct rocketnet_client_slot_index_t *) rocketnet_malloc( context, index_bytes );
    slots->slot_generation = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) max_clients );
    slots->client_id = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) max_clients );
    slots->connected = (uint8_t *) rocketnet_malloc( context, sizeof( uint8_t ) * (size_t) max_clients );
    slots->mapping_index = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) max_clients );
    slots->confirmed = (uint8_t *) rocketnet_malloc( context, sizeof( uint8_t ) * (size_t) max_clients );
    slots->free_stack = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) max_clients );
    slots->user_data = (uint8_t *) rocketnet_malloc( context, (size_t) ROCKETNET_USER_DATA_BYTES * (size_t) max_clients );

    if ( !slots->index_table || !slots->slot_generation || !slots->client_id || !slots->connected || !slots->mapping_index || !slots->confirmed || !slots->free_stack || !slots->user_data )
    {
        rocketnet_client_slots_destroy( slots );
        return NULL;
    }

    memset( slots->index_table, 0, index_bytes );
#if ROCKETNET_DEV
    slots->index_table->forced_hash_index = -1;
#endif // #if ROCKETNET_DEV
    for ( int i = 0; i < slots->num_buckets; ++i )
        rocketnet_client_slot_index_entries( slots )[i] = -1;

    memset( slots->slot_generation, 0, sizeof( uint64_t ) * (size_t) max_clients );
    memset( slots->client_id, 0, sizeof( uint64_t ) * (size_t) max_clients );
    memset( slots->connected, 0, sizeof( uint8_t ) * (size_t) max_clients );
    memset( slots->confirmed, 0, sizeof( uint8_t ) * (size_t) max_clients );

    // Assignment writes the entire user-data record before the slot is read.
    // Unassigned records have no secret and do not need their pages touched.

    for ( int i = 0; i < max_clients; i++ )
        slots->mapping_index[i] = -1;

    // proofs/tla/RNClientSlotsLayout.tla Init: the free list is filled so the
    // first max_clients assigns pop 0, 1, 2 and so on in order: the stack
    // holds the indices highest-first, and the pop at the top of the stack
    // is index 0

    for ( int i = 0; i < max_clients; i++ )
        slots->free_stack[i] = max_clients - 1 - i;

    slots->free_stack_count = max_clients;

    return slots;
}

void rocketnet_client_slots_destroy( struct rocketnet_client_slots_t * slots )
{
    rocketnet_assert( slots );

    // A released slot was wiped at release. Only connected slots can still
    // hold token secrets; never write unused records. num_connected is zero
    // during partial-create cleanup, before connected[] has been initialized.

    if ( slots->num_connected && slots->user_data && slots->connected )
        for ( int i = 0; i < slots->max_clients; ++i )
            if ( slots->connected[i] )
                sodium_memzero( slots->user_data + (size_t) i * ROCKETNET_USER_DATA_BYTES, ROCKETNET_USER_DATA_BYTES );

    rocketnet_free( slots->context, slots->index_table );
    rocketnet_free( slots->context, slots->slot_generation );
    rocketnet_free( slots->context, slots->client_id );
    rocketnet_free( slots->context, slots->connected );
    rocketnet_free( slots->context, slots->mapping_index );
    rocketnet_free( slots->context, slots->confirmed );
    rocketnet_free( slots->context, slots->free_stack );
    rocketnet_free( slots->context, slots->user_data );
    rocketnet_free( slots->context, slots );
}

int rocketnet_client_slots_max_clients( const struct rocketnet_client_slots_t * slots )
{
    rocketnet_assert( slots );
    return slots->max_clients;
}

int rocketnet_client_slots_num_connected( const struct rocketnet_client_slots_t * slots )
{
    rocketnet_assert( slots );
    return slots->num_connected;
}

int rocketnet_client_slots_num_free( const struct rocketnet_client_slots_t * slots )
{
    rocketnet_assert( slots );
    return slots->max_clients - slots->num_connected;
}

static int rocketnet_client_slots_bucket( const struct rocketnet_client_slots_t * slots, uint64_t client_id )
{
#if ROCKETNET_DEV
    if ( slots->index_table->forced_hash_index >= 0 )
        return slots->index_table->forced_hash_index;
#endif // #if ROCKETNET_DEV
    uint8_t hash[crypto_shorthash_BYTES];
    crypto_shorthash( hash, (const uint8_t *) &client_id, 8, slots->hash_key );

    uint64_t value;
    memcpy( &value, hash, 8 );

    return (int) ( value % (uint64_t) slots->num_buckets );
}

static int rocketnet_client_slots_next_index( const struct rocketnet_client_slots_t * slots, int index )
{
    return index + 1 == slots->num_buckets ? 0 : index + 1;
}

// Returns the occupied full-id entry or the first empty insertion position.
// There are at most max_clients occupied entries in a 2*max_clients table, so
// an empty position always exists unless private state is corrupt.

static int rocketnet_client_slots_locate( const struct rocketnet_client_slots_t * slots,
                                          uint64_t client_id,
                                          bool * found )
{
    const int * entries = rocketnet_client_slot_index_entries( slots );
    int position = rocketnet_client_slots_bucket( slots, client_id );

    for ( int probe = 0; probe < slots->num_buckets; ++probe )
    {
        rocketnet_client_slots_probe_one( slots );

        const int index = entries[position];
        if ( index == -1 )
        {
            *found = false;
            return position;
        }

        const bool valid = index >= 0 && index < slots->max_clients &&
                           slots->connected[index] && slots->slot_generation[index] != 0;
        rocketnet_assert( valid );

        if ( !valid )
            return -1;

        if ( slots->client_id[index] == client_id )
        {
            *found = true;
            return position;
        }

        position = rocketnet_client_slots_next_index( slots, position );
    }

    rocketnet_assert( false );

    return -1;
}

int rocketnet_client_slots_find( const struct rocketnet_client_slots_t * slots, uint64_t client_id )
{
    rocketnet_assert( slots );
    rocketnet_client_slots_probe_begin( slots );

    bool found = false;
    const int position = rocketnet_client_slots_locate( slots, client_id, &found );
    return position >= 0 && found ? rocketnet_client_slot_index_entries( slots )[position] : -1;
}

// proofs/tla/RNDestroyWipe.tla Add: copies the mapping's user data into the slot as the slot becomes live.

// proofs/tla/RNOpenAddress.tla Insert: linear probe from the home cell to the first
// empty one, wrapping at the end of the index
// proofs/tla/RNFreeStackLayout.tla TakeSlots: the free stack peeked, refused on the owner generation before the pop
// proofs/tla/RNClientSlotsLayout.tla AssignFresh and AssignStale: the top of the
// free stack, refused when it last held this owner generation, then popped.
int rocketnet_client_slots_assign( struct rocketnet_client_slots_t * slots,
                                   uint64_t client_id,
                                   int mapping_index,
                                   uint64_t owner_generation,
                                   const uint8_t * user_data )
{
    rocketnet_assert( slots );
    rocketnet_assert( user_data );
    rocketnet_assert( owner_generation != 0 );

    if ( owner_generation == 0 )
        return -1;

    rocketnet_client_slots_probe_begin( slots );

    bool duplicate = false;
    const int index_position = rocketnet_client_slots_locate( slots, client_id, &duplicate );
    rocketnet_assert( !duplicate );

    if ( duplicate || index_position < 0 )
        return -1;

    // a free slot comes from the free list, not a search: a stack of unused
    // indices, popped here on connect and pushed on release, so the
    // per-connection lookup is constant time in the number of client slots,
    // as standard/RATIONALE.md's free-list rule requires ("a free slot is
    // found from a free list, not a search"). a full table pops nothing and
    // returns -1.

    if ( slots->free_stack_count == 0 )
        return -1;

    const int client_index = slots->free_stack[slots->free_stack_count - 1];

    // The generation belongs to the owner, not to the numeric client slot.
    // UINT64_MAX is a valid final map generation. A duplicate generation is
    // stale ownership and is refused before any state changes.

    if ( slots->slot_generation[client_index] == owner_generation )
        return -1;

    --slots->free_stack_count;

    slots->connected[client_index] = 1;
    slots->confirmed[client_index] = 0;
    slots->client_id[client_index] = client_id;
    slots->mapping_index[client_index] = mapping_index;
    memcpy( slots->user_data + (size_t) client_index * ROCKETNET_USER_DATA_BYTES, user_data, ROCKETNET_USER_DATA_BYTES );

    slots->slot_generation[client_index] = owner_generation;
    rocketnet_client_slot_index_entries( slots )[index_position] = client_index;

    slots->num_connected++;

    rocketnet_debug( "server assigned client slot %d", client_index );

    return client_index;
}

#if ROCKETNET_DEV
int rocketnet_client_slots_assign( struct rocketnet_client_slots_t * slots,
                                   uint64_t client_id,
                                   int mapping_index,
                                   const uint8_t * user_data )
{
    // A null table is a refused test call. Returning here keeps that null from reaching the real assign, whose probe and index helpers dereference it.

    if ( !slots )
        return -1;

    uint64_t owner_generation = 1;

    if ( slots->free_stack_count > 0 )
    {
        const int client_index = slots->free_stack[slots->free_stack_count - 1];
        if ( client_index >= 0 && client_index < slots->max_clients &&
             slots->slot_generation[client_index] == owner_generation )
        {
            owner_generation = 2;
        }
    }

    return rocketnet_client_slots_assign( slots, client_id, mapping_index, owner_generation, user_data );
}
#endif // #if ROCKETNET_DEV

int rocketnet_client_slots_connected( const struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );

    if ( client_index < 0 || client_index >= slots->max_clients )
        return 0;

    return slots->connected[client_index];
}

uint64_t rocketnet_client_slots_client_id( const struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );

    if ( client_index < 0 || client_index >= slots->max_clients )
        return 0;
    if ( !slots->connected[client_index] )
        return 0;

    return slots->client_id[client_index];
}

int rocketnet_client_slots_mapping_index( const struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients );
    rocketnet_assert( slots->connected[client_index] );
    return slots->mapping_index[client_index];
}

uint64_t rocketnet_client_slots_owner_generation( const struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients );
    rocketnet_assert( slots->connected[client_index] );
    return slots->slot_generation[client_index];
}

const uint8_t * rocketnet_client_slots_user_data( const struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients );
    rocketnet_assert( slots->connected[client_index] );
    return slots->user_data + (size_t) client_index * ROCKETNET_USER_DATA_BYTES;
}

int rocketnet_client_slots_confirmed( const struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients );
    rocketnet_assert( slots->connected[client_index] );
    return slots->confirmed[client_index];
}

void rocketnet_client_slots_confirm( struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients );
    rocketnet_assert( slots->connected[client_index] );
    rocketnet_debug( "server confirmed client slot %d", client_index );

    slots->confirmed[client_index] = 1;
}

// Backward-shift deletion keeps every surviving probe cluster searchable and
// leaves no tombstones for repeated churn to accumulate. num_buckets need not
// be a power of two, so wrap is explicit rather than a bit mask.

// proofs/tla/RNOpenAddress.tla EraseStart and EraseStep (ClearLast TRUE): the scan
// moves back each entry whose home is not cyclically in (hole, scan], and the
// final hole clears last; the scan ends only on a cell already empty, so the
// index is sized at twice its capacity
static void rocketnet_client_slots_erase_index( struct rocketnet_client_slots_t * slots, int hole )
{
    int * entries = rocketnet_client_slot_index_entries( slots );
    int scan = hole;

    for ( ;; )
    {
        scan = rocketnet_client_slots_next_index( slots, scan );

        rocketnet_client_slots_probe_one( slots );

        if ( entries[scan] == -1 )
            break;

        const int home = rocketnet_client_slots_bucket( slots, slots->client_id[entries[scan]] );
        const bool stays = hole <= scan ? ( hole < home && home <= scan )
                                        : ( hole < home || home <= scan );
        if ( stays )
            continue;

        entries[hole] = entries[scan];
        hole = scan;
    }

    entries[hole] = -1;
}

// proofs/tla/RNClientSlotsLayout.tla ReleaseHandle: only the exact tenure (the
// slot connected under this client id and generation) is released, and its
// index is pushed at free_stack[free_stack_count++].
static bool rocketnet_client_slots_release_exact( struct rocketnet_client_slots_t * slots,
                                                  uint64_t client_id,
                                                  int client_index,
                                                  uint64_t generation )
{
    if ( client_index < 0 || client_index >= slots->max_clients ||
         !slots->connected[client_index] || slots->client_id[client_index] != client_id ||
         slots->slot_generation[client_index] != generation )
        return false;

    bool found = false;
    const int position = rocketnet_client_slots_locate( slots, client_id, &found );
    if ( position < 0 || !found )
        return false;
    if ( rocketnet_client_slot_index_entries( slots )[position] != client_index )
        return false;

    rocketnet_client_slots_erase_index( slots, position );

    slots->connected[client_index] = 0;
    slots->confirmed[client_index] = 0;
    slots->client_id[client_index] = 0;
    slots->mapping_index[client_index] = -1;
    sodium_memzero( slots->user_data + (size_t) client_index * ROCKETNET_USER_DATA_BYTES, ROCKETNET_USER_DATA_BYTES );

    // this remains a dense free stack, not linked ownership state. A release pushes exactly one numeric slot and the next assign pops it immediately.

    slots->free_stack[slots->free_stack_count++] = client_index;
    slots->num_connected--;

    rocketnet_debug( "server released client slot %d", client_index );

    return true;
}

#if ROCKETNET_DEV
bool rocketnet_client_slots_handle( const struct rocketnet_client_slots_t * slots,
                                    int client_index,
                                    struct rocketnet_client_slot_handle_t * handle )
{
    rocketnet_assert( slots );

    if ( !handle || client_index < 0 || client_index >= slots->max_clients || !slots->connected[client_index] )
        return false;

    handle->client_id = slots->client_id[client_index];
    handle->client_index = client_index;
    handle->generation = slots->slot_generation[client_index];

    return true;
}

bool rocketnet_client_slots_release_handle( struct rocketnet_client_slots_t * slots,
                                            const struct rocketnet_client_slot_handle_t * handle )
{
    rocketnet_assert( slots );
    rocketnet_client_slots_probe_begin( slots );
    return handle && rocketnet_client_slots_release_exact( slots, handle->client_id,
                                                           handle->client_index, handle->generation );
}

uint64_t rocketnet_client_slots_test_hash_index( const struct rocketnet_client_slots_t * slots,
                                                 uint64_t client_id )
{
    rocketnet_assert( slots );
    return (uint64_t) rocketnet_client_slots_bucket( slots, client_id );
}

bool rocketnet_client_slots_test_force_hash_index( struct rocketnet_client_slots_t * slots,
                                                   int index )
{
    rocketnet_assert( slots );

    if ( index < -1 || index >= slots->num_buckets )
        return false;

    slots->index_table->forced_hash_index = index;

    return true;
}

uint64_t rocketnet_client_slots_test_last_probes( const struct rocketnet_client_slots_t * slots )
{
    rocketnet_assert( slots );
    return slots->index_table->last_probes;
}

uint64_t rocketnet_client_slots_test_total_probes( const struct rocketnet_client_slots_t * slots )
{
    rocketnet_assert( slots );
    return slots->index_table->total_probes;
}

// Corrupts the private index for a unit test: every cell names client_index,
// so no cell is empty and only the erase scan's bound can end it.
void rocketnet_client_slots_fill_index_for_test( struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    int * entries = rocketnet_client_slot_index_entries( slots );
    for ( int i = 0; i < slots->num_buckets; ++i )
        entries[i] = client_index;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNDestroyWipe.tla Remove: clears the slot's user data and returns the slot to the free list.
// proofs/tla/RNClientSlotsLayout.tla Release: a slot not connected is refused, so
// no index is pushed twice.

void rocketnet_client_slots_release( struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients && slots->connected[client_index] );

    // proofs/tla/RNDestroyWipe.tla Remove requires m \in live: a free slot is refused before anything is touched

    if ( client_index < 0 || client_index >= slots->max_clients || !slots->connected[client_index] )
    {
        rocketnet_error( "server refused to release client slot %d: not connected", client_index );
        return;
    }

    const uint64_t client_id = slots->client_id[client_index];
    const uint64_t generation = slots->slot_generation[client_index];

    rocketnet_client_slots_probe_begin( slots );

    const bool released = rocketnet_client_slots_release_exact( slots, client_id, client_index, generation );
    rocketnet_assert( released );

    if ( !released )
        rocketnet_error( "server refused to release client slot %d: stale or missing full-id index entry", client_index );
}

#if ROCKETNET_DEV
bool rocketnet_client_slots_inventory_matches_for_test( char * failure, size_t failure_bytes )
{
#define SLOT_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rocketnet_client_slots_t, field, -1 ),
    static const rocketnet_reset_member_t members[] = { ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS( SLOT_MEMBER ) };
#undef SLOT_MEMBER
#define SLOT_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rocketnet_client_slots_t, field, #decision[0] == 'R' ),
    static const rocketnet_reset_member_t decisions[] = { ROCKETNET_CLIENT_SLOTS_OWNER_FIELDS( SLOT_DECISION ) };
#undef SLOT_DECISION
    return rocketnet_reset_inventory_matches( "rocketnet_client_slots_t", members, sizeof( members ) / sizeof( members[0] ),
                                              decisions, sizeof( decisions ) / sizeof( decisions[0] ), 0,
                                              sizeof( rocketnet_client_slots_t ), alignof( rocketnet_client_slots_t ),
                                              failure, failure_bytes );
}

bool rocketnet_client_slots_reset_witness_for_test()
{
    rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 1 );
    if ( !slots )
        return false;

    uint8_t key[sizeof( slots->hash_key )];
    memcpy( key, slots->hash_key, sizeof( key ) );

    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0xA5, sizeof( user ) );

    const int index = rocketnet_client_slots_assign( slots, UINT64_MAX, 77, 1, user );
    if ( index != 0 )
    {
        rocketnet_client_slots_destroy( slots );
        return false;
    }

    rocketnet_client_slots_confirm( slots, index );
    rocketnet_client_slots_release( slots, index );

    const int * entries = rocketnet_client_slot_index_entries( slots );
    bool clean = slots->num_connected == 0 && slots->free_stack_count == 1 && slots->free_stack[0] == 0 &&
                 entries[0] == -1 && entries[1] == -1 && slots->slot_generation[0] == 1 &&
                 slots->client_id[0] == 0 && slots->connected[0] == 0 && slots->mapping_index[0] == -1 &&
                 slots->confirmed[0] == 0 && memcmp( slots->hash_key, key, sizeof( key ) ) == 0;
    for ( int i = 0; clean && i < ROCKETNET_USER_DATA_BYTES; i++ )
        clean = slots->user_data[i] == 0;

    // UINT64_MAX is a valid owner generation. A reused slot may next take a
    // different generation belonging to an older half-open mapping; generation
    // exhaustion is owned and enforced by the encryption map, not this slot.

    if ( clean )
        clean = rocketnet_client_slots_assign( slots, 1, 88, UINT64_MAX, user ) == 0 && slots->slot_generation[0] == UINT64_MAX;
    if ( clean )
        rocketnet_client_slots_release( slots, 0 );
    if ( clean )
        clean = rocketnet_client_slots_assign( slots, 2, 99, UINT64_MAX - 1, user ) == 0 &&
                slots->num_connected == 1 && slots->free_stack_count == 0 &&
                slots->slot_generation[0] == UINT64_MAX - 1 && slots->client_id[0] == 2 &&
                slots->mapping_index[0] == 99;
    if ( clean )
        rocketnet_client_slots_release( slots, 0 );
    for ( int i = 0; clean && i < ROCKETNET_USER_DATA_BYTES; i++ )
        clean = slots->user_data[i] == 0;

    rocketnet_client_slots_destroy( slots );

    return clean;
}
#endif // #if ROCKETNET_DEV
