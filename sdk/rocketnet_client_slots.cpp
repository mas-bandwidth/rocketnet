/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_client_slots.h"
#include "rocketnet.h"

#include <sodium.h>
#include <string.h>
#include <limits.h>

struct rocketnet_client_slots_t
{
    void * context;
    int max_clients;
    int num_connected;
    uint8_t hash_key[crypto_shorthash_KEYBYTES];

    int * bucket_head;                      // max_clients * 2, -1 when empty
    int * next;                             // chain link per slot
    int num_buckets;

    // hot: walked by find
    uint64_t * client_id;
    int * connected;

    // cold: same index
    int * mapping_index;
    int * confirmed;
    uint8_t * user_data;                    // max_clients * ROCKETNET_USER_DATA_BYTES
};

uint64_t rocketnet_client_slots_allocation_bytes(int clients)
{
    if (clients < 1) return 0;
    const uint64_t n = (uint64_t)clients;
    return rocketnet_memory_allocations({sizeof(rocketnet_client_slots_t), 2*n*sizeof(int),
        n*sizeof(int), n*sizeof(uint64_t), n*sizeof(int), n*sizeof(int), n*sizeof(int), n*ROCKETNET_USER_DATA_BYTES});
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
    slots->user_data = (uint8_t*) rocketnet_malloc( context, (size_t) ROCKETNET_USER_DATA_BYTES * (size_t) max_clients );

    if ( !slots->bucket_head || !slots->next || !slots->client_id || !slots->connected || !slots->mapping_index || !slots->confirmed || !slots->user_data )
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

    // the lowest free slot, as netcode assigns them

    int client_index = -1;
    for ( int i = 0; i < slots->max_clients; i++ )
    {
        if ( !slots->connected[i] )
        {
            client_index = i;
            break;
        }
    }
    if ( client_index == -1 )
        return -1;

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
    slots->confirmed[client_index] = 0;
    slots->client_id[client_index] = 0;
    slots->mapping_index[client_index] = -1;
    memset( slots->user_data + (size_t) client_index * ROCKETNET_USER_DATA_BYTES, 0, ROCKETNET_USER_DATA_BYTES );

    slots->num_connected--;

    rocketnet_debug( "server released client slot %d", client_index );
}
