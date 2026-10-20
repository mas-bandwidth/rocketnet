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
    struct rocketnet_encryption_mapping_t * mapping;        // cold: same index

    int * token_bucket_head;                                // num_buckets, -1 when empty: chains by token identifier
    int * token_next;                                       // capacity: token chain link
    int * token_linked;                                     // capacity: 1 while the index is on a token chain
};

uint64_t rocketnet_encryption_map_allocation_bytes(int capacity)
{
    if (capacity < 1) return 0;
    const uint64_t n = (uint64_t)capacity;
    return rocketnet_memory_allocations({sizeof(rocketnet_encryption_map_t), 2*n*sizeof(int),
        n*sizeof(int), n*sizeof(rocketnet_address_t), n*sizeof(int), n*sizeof(rocketnet_encryption_mapping_t),
        2*n*sizeof(int), n*sizeof(int), n*sizeof(int)});
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
    map->mapping = (struct rocketnet_encryption_mapping_t*) rocketnet_malloc( context, sizeof(struct rocketnet_encryption_mapping_t) * (size_t) capacity );
    map->token_bucket_head = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) map->num_buckets );
    map->token_next = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    map->token_linked = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );

    if ( !map->bucket_head || !map->next || !map->address || !map->live || !map->mapping ||
         !map->token_bucket_head || !map->token_next || !map->token_linked )
    {
        rocketnet_free( context, map->bucket_head );
        rocketnet_free( context, map->next );
        rocketnet_free( context, map->address );
        rocketnet_free( context, map->live );
        rocketnet_free( context, map->mapping );
        rocketnet_free( context, map->token_bucket_head );
        rocketnet_free( context, map->token_next );
        rocketnet_free( context, map->token_linked );
        rocketnet_free( context, map );
        return NULL;
    }

    for ( int i = 0; i < map->num_buckets; i++ )
    {
        map->bucket_head[i] = -1;
        map->token_bucket_head[i] = -1;
    }

    for ( int i = 0; i < capacity; i++ )
        map->next[i] = i + 1 < capacity ? i + 1 : -1;
    map->free_head = 0;

    memset( map->address, 0, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    memset( map->live, 0, sizeof(int) * (size_t) capacity );
    memset( map->mapping, 0, sizeof(struct rocketnet_encryption_mapping_t) * (size_t) capacity );
    for ( int i = 0; i < capacity; i++ )
        map->token_next[i] = -1;
    memset( map->token_linked, 0, sizeof(int) * (size_t) capacity );

    return map;
}

void rocketnet_encryption_map_destroy( struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    sodium_memzero( map->mapping, sizeof(struct rocketnet_encryption_mapping_t) * (size_t) map->capacity );
    rocketnet_free( map->context, map->bucket_head );
    rocketnet_free( map->context, map->next );
    rocketnet_free( map->context, map->address );
    rocketnet_free( map->context, map->live );
    rocketnet_free( map->context, map->mapping );
    rocketnet_free( map->context, map->token_bucket_head );
    rocketnet_free( map->context, map->token_next );
    rocketnet_free( map->context, map->token_linked );
    rocketnet_free( map->context, map );
}

static int rocketnet_encryption_map_bucket( const struct rocketnet_encryption_map_t * map, const struct rocketnet_address_t * address )
{
    uint8_t address_bytes[ROCKETNET_ADDRESS_BYTES_IPV6];
    int address_bytes_length = rocketnet_write_address_bytes( address_bytes, address );
    rocketnet_assert( address_bytes_length > 0 );

    uint8_t hash[crypto_shorthash_BYTES];
    crypto_shorthash( hash, address_bytes, (size_t) address_bytes_length, map->hash_key );
    uint64_t value;
    memcpy( &value, hash, 8 );
    return (int) ( value % (uint64_t) map->num_buckets );
}

static int rocketnet_encryption_map_token_bucket( const struct rocketnet_encryption_map_t * map, const uint8_t * token_identifier )
{
    uint8_t hash[crypto_shorthash_BYTES];
    crypto_shorthash( hash, token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES, map->hash_key );
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
    mapping->client_index = -1;
    mapping->handshake_deadline = handshake_deadline;
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

const struct rocketnet_address_t * rocketnet_encryption_map_address( const struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->live[index] );
    return &map->address[index];
}

void rocketnet_encryption_map_claim_token( struct rocketnet_encryption_map_t * map, int index, const uint8_t * token_identifier )
{
    rocketnet_assert( map );
    rocketnet_assert( token_identifier );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->live[index] );
    rocketnet_assert( !map->token_linked[index] );

    memcpy( map->mapping[index].token_identifier, token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES );

    int bucket = rocketnet_encryption_map_token_bucket( map, token_identifier );
    map->token_next[index] = map->token_bucket_head[bucket];
    map->token_bucket_head[bucket] = index;
    map->token_linked[index] = 1;
}

int rocketnet_encryption_map_find_token_claim( const struct rocketnet_encryption_map_t * map,
                                               const uint8_t * token_identifier,
                                               const struct rocketnet_address_t * address,
                                               double current_time )
{
    rocketnet_assert( map );
    rocketnet_assert( token_identifier );
    rocketnet_assert( address );

    int bucket = rocketnet_encryption_map_token_bucket( map, token_identifier );
    for ( int i = map->token_bucket_head[bucket]; i != -1; i = map->token_next[i] )
    {
        const struct rocketnet_encryption_mapping_t * mapping = &map->mapping[i];
        if ( mapping->client_index == -1 &&
             current_time < mapping->handshake_deadline &&
             sodium_memcmp( mapping->token_identifier, token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 &&
             !rocketnet_address_equal( &map->address[i], address ) )
        {
            return i;
        }
    }
    return -1;
}

int rocketnet_encryption_map_full( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return map->free_head == -1;
}

void rocketnet_encryption_map_remove( struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->live[index] );

    if ( map->token_linked[index] )
    {
        int token_bucket = rocketnet_encryption_map_token_bucket( map, map->mapping[index].token_identifier );
        int * link = &map->token_bucket_head[token_bucket];
        while ( *link != index )
        {
            rocketnet_assert( *link != -1 );
            link = &map->token_next[*link];
        }
        *link = map->token_next[index];
        map->token_next[index] = -1;
        map->token_linked[index] = 0;
    }

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

    sodium_memzero( &map->mapping[index], sizeof(struct rocketnet_encryption_mapping_t) );
    memset( &map->address[index], 0, sizeof(struct rocketnet_address_t) );
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
