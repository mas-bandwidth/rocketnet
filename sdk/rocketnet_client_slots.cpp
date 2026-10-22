/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_internal.h"
#include "rocketnet_connection_reset.h"

#include <sodium.h>
#include <string.h>
#include <limits.h>

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
    X( bucket_head, int * bucket_head ) /* max_clients * 2, -1 when empty */          \
    X( next, int * next ) /* chain link per slot */                                   \
    X( num_buckets, int num_buckets )                                                 \
    /* the free list: a stack of unused client indices, popped on connect */          \
    /* and pushed on disconnect, so finding a free slot is constant time in */        \
    /* the number of client slots and never a search (STANDARD.md, */                 \
    /* "Lookup complexity": "a free slot is found from a free list, not a */          \
    /* search") */                                                                    \
    X( free_stack, int * free_stack ) /* max_clients, unused indices */               \
    X( free_stack_count, int free_stack_count )                                       \
    /* hot: walked by find */                                                         \
    X( client_id, uint64_t * client_id )                                              \
    X( connected, int * connected )                                                   \
    /* cold: same index */                                                            \
    X( mapping_index, int * mapping_index )                                           \
    X( confirmed, int * confirmed )                                                   \
    X( user_data, uint8_t * user_data ) /* max_clients * ROCKETNET_USER_DATA_BYTES */

struct rocketnet_client_slots_t
{
#define ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS( ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS_DECLARE )
#undef ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS_DECLARE
};

uint64_t rocketnet_client_slots_allocation_bytes(int clients)
{
    if (clients < 1) return 0;
    const uint64_t n = (uint64_t)clients;
    return rocketnet_memory_allocations({sizeof(rocketnet_client_slots_t), 2*n*sizeof(int),
        n*sizeof(int), n*sizeof(uint64_t), n*sizeof(int), n*sizeof(int), n*sizeof(int), n*sizeof(int),
        n*ROCKETNET_USER_DATA_BYTES});
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

    struct rocketnet_client_slots_t * slots = (struct rocketnet_client_slots_t*) rocketnet_malloc( context, sizeof(struct rocketnet_client_slots_t) );
    if ( !slots )
        return NULL;
    memset( slots, 0, sizeof(struct rocketnet_client_slots_t) );

    slots->context = context;
    slots->max_clients = max_clients;
    slots->num_buckets = max_clients * 2;
    randombytes_buf( slots->hash_key, sizeof(slots->hash_key) );

    slots->bucket_head = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) slots->num_buckets );
    slots->next = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) max_clients );
    slots->client_id = (uint64_t*) rocketnet_malloc( context, sizeof(uint64_t) * (size_t) max_clients );
    slots->connected = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) max_clients );
    slots->mapping_index = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) max_clients );
    slots->confirmed = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) max_clients );
    slots->free_stack = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) max_clients );
    slots->user_data = (uint8_t*) rocketnet_malloc( context, (size_t) ROCKETNET_USER_DATA_BYTES * (size_t) max_clients );

    if ( !slots->bucket_head || !slots->next || !slots->client_id || !slots->connected || !slots->mapping_index || !slots->confirmed || !slots->free_stack || !slots->user_data )
    {
        rocketnet_client_slots_destroy( slots );
        return NULL;
    }

    for ( int i = 0; i < slots->num_buckets; i++ )
        slots->bucket_head[i] = -1;

    memset( slots->next, 0, sizeof(int) * (size_t) max_clients );
    memset( slots->client_id, 0, sizeof(uint64_t) * (size_t) max_clients );
    memset( slots->connected, 0, sizeof(int) * (size_t) max_clients );
    memset( slots->confirmed, 0, sizeof(int) * (size_t) max_clients );
    memset( slots->user_data, 0, (size_t) ROCKETNET_USER_DATA_BYTES * (size_t) max_clients );

    for ( int i = 0; i < max_clients; i++ )
        slots->mapping_index[i] = -1;

    // the free list is filled so the first max_clients assigns pop 0, 1, 2
    // and so on in order: the stack holds the indices highest-first, and
    // the pop at the top of the stack is index 0
    for ( int i = 0; i < max_clients; i++ )
        slots->free_stack[i] = max_clients - 1 - i;
    slots->free_stack_count = max_clients;

    return slots;
}

void rocketnet_client_slots_destroy( struct rocketnet_client_slots_t * slots )
{
    rocketnet_assert( slots );
    rocketnet_free( slots->context, slots->bucket_head );
    rocketnet_free( slots->context, slots->next );
    rocketnet_free( slots->context, slots->client_id );
    rocketnet_free( slots->context, slots->connected );
    rocketnet_free( slots->context, slots->mapping_index );
    rocketnet_free( slots->context, slots->confirmed );
    rocketnet_free( slots->context, slots->free_stack );
    // the connect tokens' user data can carry the game's secrets: wiped
    // whole, which touches no new page (create zeroed it)
    if ( slots->user_data )
        sodium_memzero( slots->user_data, (size_t) ROCKETNET_USER_DATA_BYTES * (size_t) slots->max_clients );
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
    uint8_t hash[crypto_shorthash_BYTES];
    crypto_shorthash( hash, (const uint8_t*) &client_id, 8, slots->hash_key );
    uint64_t value;
    memcpy( &value, hash, 8 );
    return (int) ( value % (uint64_t) slots->num_buckets );
}

int rocketnet_client_slots_find( const struct rocketnet_client_slots_t * slots, uint64_t client_id )
{
    rocketnet_assert( slots );
    int bucket = rocketnet_client_slots_bucket( slots, client_id );
    for ( int i = slots->bucket_head[bucket]; i != -1; i = slots->next[i] )
    {
        if ( slots->client_id[i] == client_id )
            return i;
    }
    return -1;
}

int rocketnet_client_slots_assign( struct rocketnet_client_slots_t * slots,
                                   uint64_t client_id,
                                   int mapping_index,
                                   const uint8_t * user_data )
{
    rocketnet_assert( slots );
    rocketnet_assert( user_data );
    rocketnet_assert( rocketnet_client_slots_find( slots, client_id ) == -1 );

    // a free slot comes from the free list, not a search: a stack of unused
    // indices, popped here on connect and pushed on release, so the
    // per-connection lookup is constant time in the number of client slots,
    // as STANDARD.md's "Lookup complexity" rule requires ("a free slot is
    // found from a free list, not a search"). a full table pops nothing and
    // returns -1.

    if ( slots->free_stack_count == 0 )
        return -1;
    int client_index = slots->free_stack[--slots->free_stack_count];

    slots->connected[client_index] = 1;
    slots->confirmed[client_index] = 0;
    slots->client_id[client_index] = client_id;
    slots->mapping_index[client_index] = mapping_index;
    memcpy( slots->user_data + (size_t) client_index * ROCKETNET_USER_DATA_BYTES, user_data, ROCKETNET_USER_DATA_BYTES );

    int bucket = rocketnet_client_slots_bucket( slots, client_id );
    slots->next[client_index] = slots->bucket_head[bucket];
    slots->bucket_head[bucket] = client_index;

    slots->num_connected++;

    rocketnet_debug( "server assigned client slot %d", client_index );

    return client_index;
}

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

void rocketnet_client_slots_release( struct rocketnet_client_slots_t * slots, int client_index )
{
    rocketnet_assert( slots );
    rocketnet_assert( client_index >= 0 && client_index < slots->max_clients );
    rocketnet_assert( slots->connected[client_index] );

    uint64_t client_id = slots->client_id[client_index];
    int bucket = rocketnet_client_slots_bucket( slots, client_id );
    if ( slots->bucket_head[bucket] == client_index )
    {
        slots->bucket_head[bucket] = slots->next[client_index];
    }
    else
    {
        int found = 0;
        for ( int i = slots->bucket_head[bucket]; i != -1; i = slots->next[i] )
        {
            if ( slots->next[i] == client_index )
            {
                slots->next[i] = slots->next[client_index];
                found = 1;
                break;
            }
        }
        rocketnet_assert( found );
        (void) found;
    }

    slots->connected[client_index] = 0;
    slots->next[client_index] = 0;
    slots->confirmed[client_index] = 0;
    slots->client_id[client_index] = 0;
    slots->mapping_index[client_index] = -1;
    sodium_memzero( slots->user_data + (size_t) client_index * ROCKETNET_USER_DATA_BYTES, ROCKETNET_USER_DATA_BYTES );

    // the free list takes the index back: pushed on disconnect, popped on
    // the next connect, so the slot is immediately available for reuse and
    // no slot is ever handed out twice while held
    slots->free_stack[slots->free_stack_count++] = client_index;

    slots->num_connected--;

    rocketnet_debug( "server released client slot %d", client_index );
}

bool rocketnet_client_slots_inventory_matches_for_test( char * failure, size_t failure_bytes )
{
#define SLOT_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rocketnet_client_slots_t, field, -1 ),
    static const rocketnet_reset_member_t members[] = { ROCKETNET_CLIENT_SLOTS_LAYOUT_FIELDS( SLOT_MEMBER ) };
#undef SLOT_MEMBER
#define SLOT_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rocketnet_client_slots_t, field, #decision[0] == 'R' ),
    static const rocketnet_reset_member_t decisions[] = { ROCKETNET_CLIENT_SLOTS_OWNER_FIELDS( SLOT_DECISION ) };
#undef SLOT_DECISION
    return rocketnet_reset_inventory_matches( "rocketnet_client_slots_t", members, sizeof(members) / sizeof(members[0]),
                                              decisions, sizeof(decisions) / sizeof(decisions[0]), 0,
                                              sizeof(rocketnet_client_slots_t), alignof(rocketnet_client_slots_t),
                                              failure, failure_bytes );
}

bool rocketnet_client_slots_reset_witness_for_test()
{
    rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 1 );
    if ( !slots )
        return false;
    uint8_t key[sizeof(slots->hash_key)];
    memcpy( key, slots->hash_key, sizeof(key) );
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0xA5, sizeof(user) );
    const int index = rocketnet_client_slots_assign( slots, UINT64_MAX, 77, user );
    if ( index != 0 )
    {
        rocketnet_client_slots_destroy( slots );
        return false;
    }
    rocketnet_client_slots_confirm( slots, index );
    rocketnet_client_slots_release( slots, index );
    bool clean = slots->num_connected == 0 && slots->free_stack_count == 1 && slots->free_stack[0] == 0 &&
                 slots->bucket_head[0] == -1 && slots->bucket_head[1] == -1 && slots->next[0] == 0 &&
                 slots->client_id[0] == 0 && slots->connected[0] == 0 && slots->mapping_index[0] == -1 &&
                 slots->confirmed[0] == 0 && memcmp( slots->hash_key, key, sizeof(key) ) == 0;
    for ( int i = 0; clean && i < ROCKETNET_USER_DATA_BYTES; i++ )
        clean = slots->user_data[i] == 0;
    rocketnet_client_slots_destroy( slots );
    return clean;
}
