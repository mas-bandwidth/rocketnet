/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_internal.h"

#include <sodium.h>
#include <string.h>
#include <limits.h>

struct rocketnet_encryption_map_t
{
    void * context;
    int capacity;
    int num_buckets;
    int num_mappings;
    uint8_t hash_key[crypto_shorthash_KEYBYTES];

    int * bucket_head;                                      // num_buckets, -1 when empty
    int * next;                                             // capacity: chain link, or free list link
    int free_head;

    struct rocketnet_address_t * address;                   // hot: walked by find
    int * live;                                             // hot: parallel to address
    struct rocketnet_encryption_mapping_control_t * control; // hot: timeout walk
    struct rocketnet_encryption_mapping_t * mapping;        // cold: same index
#if ROCKETNET_TEST_SEAMS
    mutable uint64_t test_hashes;                           // keyed hashes computed, for the unit suite
#endif // #if ROCKETNET_TEST_SEAMS
};

uint64_t rocketnet_encryption_map_allocation_bytes(int capacity)
{
    if (capacity < 1) return 0;
    const uint64_t n = (uint64_t)capacity;
    return rocketnet_memory_allocations({sizeof(rocketnet_encryption_map_t), 2*n*sizeof(int),
        n*sizeof(int), n*sizeof(rocketnet_address_t), n*sizeof(int),
        n*sizeof(rocketnet_encryption_mapping_control_t), n*sizeof(rocketnet_encryption_mapping_t)});
}

struct rocketnet_encryption_map_t * rocketnet_encryption_map_create( void * context, int capacity )
{
    rocketnet_assert( capacity > 0 );

    // num_buckets is capacity * 2 in a signed int, so a capacity above
    // INT_MAX / 2 wraps the bucket count and the bucket array is sized from
    // the wrapped value while the other arrays use the capacity. refuse it
    // before any allocation.
    if ( capacity < 1 || capacity > INT_MAX / 2 )
        return NULL;

    struct rocketnet_encryption_map_t * map = (struct rocketnet_encryption_map_t*) rocketnet_malloc( context, sizeof(struct rocketnet_encryption_map_t) );
    if ( !map )
        return NULL;
    memset( map, 0, sizeof(struct rocketnet_encryption_map_t) );

    map->context = context;
    map->capacity = capacity;
    map->num_buckets = capacity * 2;
    randombytes_buf( map->hash_key, sizeof(map->hash_key) );

    map->bucket_head = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) map->num_buckets );
    map->next = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    map->address = (struct rocketnet_address_t*) rocketnet_malloc( context, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    map->live = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    map->control = (struct rocketnet_encryption_mapping_control_t*) rocketnet_malloc( context, sizeof(struct rocketnet_encryption_mapping_control_t) * (size_t) capacity );
    map->mapping = (struct rocketnet_encryption_mapping_t*) rocketnet_malloc( context, sizeof(struct rocketnet_encryption_mapping_t) * (size_t) capacity );

    if ( !map->bucket_head || !map->next || !map->address || !map->live || !map->control || !map->mapping )
    {
        rocketnet_free( context, map->bucket_head );
        rocketnet_free( context, map->next );
        rocketnet_free( context, map->address );
        rocketnet_free( context, map->live );
        rocketnet_free( context, map->control );
        rocketnet_free( context, map->mapping );
        rocketnet_free( context, map );
        return NULL;
    }

    for ( int i = 0; i < map->num_buckets; i++ )
        map->bucket_head[i] = -1;

    for ( int i = 0; i < capacity; i++ )
        map->next[i] = i + 1 < capacity ? i + 1 : -1;
    map->free_head = 0;

    // live is the state: every reader of an entry's address, control or
    // mapping reaches it through live (the bucket chains hold live entries
    // only), and add writes the entry whole, so those three arrays (3.4 KB an
    // entry) are never zeroed here
    memset( map->live, 0, sizeof(int) * (size_t) capacity );

    return map;
}

// the mapping's secrets, and only those: the session keys, the session
// nonce they were derived with, and the connect token's user data, which
// came out of the token's private part and can carry the game's secrets
// (tla/RNDestroyWipe.tla: Remove, and Destroy for a live mapping)
static void rocketnet_encryption_mapping_wipe_secrets( struct rocketnet_encryption_mapping_t * mapping )
{
    sodium_memzero( &mapping->keys, sizeof(mapping->keys) );
    sodium_memzero( mapping->session_nonce, sizeof(mapping->session_nonce) );
    sodium_memzero( mapping->user_data, sizeof(mapping->user_data) );
}

void rocketnet_encryption_map_destroy( struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    // a removed mapping was wiped by its remove; a live one still holds its
    // secrets, and those are the only secrets here
    for ( int i = 0; i < map->capacity; i++ )
        if ( map->live[i] )
            rocketnet_encryption_mapping_wipe_secrets( &map->mapping[i] );
    rocketnet_free( map->context, map->bucket_head );
    rocketnet_free( map->context, map->next );
    rocketnet_free( map->context, map->address );
    rocketnet_free( map->context, map->live );
    rocketnet_free( map->context, map->control );
    rocketnet_free( map->context, map->mapping );
    rocketnet_free( map->context, map );
}

static int rocketnet_encryption_map_bucket( const struct rocketnet_encryption_map_t * map, const struct rocketnet_address_t * address )
{
    uint8_t address_bytes[ROCKETNET_ADDRESS_BYTES_IPV6];
    int address_bytes_length = rocketnet_write_address_bytes( address_bytes, address );
    rocketnet_assert( address_bytes_length > 0 );

    uint8_t hash[crypto_shorthash_BYTES];
    crypto_shorthash( hash, address_bytes, (unsigned long long) address_bytes_length, map->hash_key );
#if ROCKETNET_TEST_SEAMS
    map->test_hashes++;
#endif // #if ROCKETNET_TEST_SEAMS
    uint64_t value;
    memcpy( &value, hash, 8 );
    return (int) ( value % (uint64_t) map->num_buckets );
}

int rocketnet_encryption_map_find( const struct rocketnet_encryption_map_t * map, const struct rocketnet_address_t * address )
{
    rocketnet_assert( map );
    rocketnet_assert( address );

    int bucket = rocketnet_encryption_map_bucket( map, address );
    for ( int i = map->bucket_head[bucket]; i != -1; i = map->next[i] )
    {
        if ( rocketnet_address_equal( &map->address[i], address ) )
            return i;
    }
    return -1;
}

int rocketnet_encryption_map_full( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return map->free_head == -1;
}

int rocketnet_encryption_map_add( struct rocketnet_encryption_map_t * map,
                                  const struct rocketnet_address_t * address,
                                  double handshake_deadline )
{
    rocketnet_assert( map );
    rocketnet_assert( address );
    rocketnet_assert( rocketnet_encryption_map_find( map, address ) == -1 );

    if ( map->free_head == -1 )
        return -1;

    int index = map->free_head;
    map->free_head = map->next[index];

    map->address[index] = *address;
    map->live[index] = 1;

    struct rocketnet_encryption_mapping_t * mapping = &map->mapping[index];
    memset( mapping, 0, sizeof(struct rocketnet_encryption_mapping_t) );
    mapping->control = &map->control[index];
    memset( mapping->control, 0, sizeof(*mapping->control) );
    mapping->control->client_index = -1;
    mapping->control->handshake_deadline = handshake_deadline;
    mapping->send_sequence_handshake = 1;
    mapping->send_sequence_session = 1;
    mapping->send_fragment_payload_id = 1;

    int bucket = rocketnet_encryption_map_bucket( map, address );
    map->next[index] = map->bucket_head[bucket];
    map->bucket_head[bucket] = index;

    map->num_mappings++;

    return index;
}

struct rocketnet_encryption_mapping_t * rocketnet_encryption_map_entry( struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->live[index] );
    return &map->mapping[index];
}

struct rocketnet_encryption_mapping_control_t * rocketnet_encryption_map_control( struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->live[index] );
    return &map->control[index];
}

const struct rocketnet_address_t * rocketnet_encryption_map_address( const struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->live[index] );
    return &map->address[index];
}

void rocketnet_encryption_map_remove( struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->live[index] );

    int bucket = rocketnet_encryption_map_bucket( map, &map->address[index] );
    if ( map->bucket_head[bucket] == index )
    {
        map->bucket_head[bucket] = map->next[index];
    }
    else
    {
        int found = 0;
        for ( int i = map->bucket_head[bucket]; i != -1; i = map->next[i] )
        {
            if ( map->next[i] == index )
            {
                map->next[i] = map->next[index];
                found = 1;
                break;
            }
        }
        rocketnet_assert( found );
        (void) found;
    }

    // the secrets are wiped; the rest of the entry is unreachable once it is
    // not live, and the next add writes it whole
    rocketnet_encryption_mapping_wipe_secrets( &map->mapping[index] );
    map->live[index] = 0;

    map->next[index] = map->free_head;
    map->free_head = index;

    map->num_mappings--;
}

int rocketnet_encryption_map_next( const struct rocketnet_encryption_map_t * map, int * iterator )
{
    rocketnet_assert( map );
    rocketnet_assert( iterator );

    for ( int i = *iterator + 1; i < map->capacity; i++ )
    {
        if ( map->live[i] )
        {
            *iterator = i;
            return 1;
        }
    }
    return 0;
}

int rocketnet_encryption_map_num_mappings( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return map->num_mappings;
}

#if ROCKETNET_TEST_SEAMS
uint64_t rocketnet_encryption_map_test_hashes( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return map->test_hashes;
}
#endif // #if ROCKETNET_TEST_SEAMS
