/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_test_seams.h"

#include <sodium.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>

#define ROCKETNET_ENCRYPTION_INDEX_EMPTY UINT32_MAX
#define ROCKETNET_ENCRYPTION_INDEX_TOMBSTONE ( UINT32_MAX - 1 )

struct rocketnet_encryption_map_t
{
    void * context;
    int capacity;
    int num_free;
    uint64_t next_generation;
    uint8_t hash_key[crypto_shorthash_KEYBYTES];

    uint32_t * index;                                        // capacity: public slot index, empty or tombstone
    uint32_t * free_indices;                                 // capacity: dense LIFO, no link ownership
    uint64_t * generation;                                   // capacity: full-width live slot generation
    struct rocketnet_address_t * address;                    // hot: full key; NONE is the free state
    struct rocketnet_encryption_mapping_control_t * control; // hot: timeout walk
    struct rocketnet_encryption_mapping_t * mapping;        // cold: same index
#if ROCKETNET_TEST_SEAMS
    mutable uint64_t test_hashes;                           // keyed hashes computed, for the unit suite
#endif // #if ROCKETNET_TEST_SEAMS
#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
    mutable uint64_t test_probes;                           // flat index cells inspected by bounded DEV oracles
#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
};

#if ROCKETNET_TEST_SEAMS
static uint64_t rocketnet_dev_session_sequence_start_from_env( void )
{
    char text[64];
    const int found = rocketnet_platform_getenv( "ROCKETNET_DEV_SESSION_SEQUENCE_START", text, sizeof( text ) );
    if ( found == 0 || ( found == 1 && text[0] == '\0' ) )
        return 1;
    char * end = NULL;
    errno = 0;
    const unsigned long long parsed = found == 1 ? strtoull( text, &end, 0 ) : 0;
    if ( found != 1 || end == text || *end != '\0' || errno != 0 || text[0] == '-' ||
         parsed == 0 || parsed > ROCKETNET_DEV_SESSION_SEQUENCE_START_MAX )
    {
        rocketnet_error( "ROCKETNET_DEV_SESSION_SEQUENCE_START=\"%s\" refused: expected 1 .. 2^62; using 1", text );
        return 1;
    }
    return (uint64_t) parsed;
}

// one read per process, at the first caller; a function-local static is
// initialized once even when two threads create at once
uint64_t rocketnet_dev_session_sequence_start( void )
{
    static const uint64_t start = rocketnet_dev_session_sequence_start_from_env();
    return start;
}
#endif // #if ROCKETNET_TEST_SEAMS

uint64_t rocketnet_encryption_map_allocation_bytes(int capacity)
{
    if (capacity < 1) return 0;
    const uint64_t n = (uint64_t)capacity;
    return rocketnet_memory_allocations({sizeof(rocketnet_encryption_map_t),
        n*sizeof(uint32_t), n*sizeof(uint32_t), n*sizeof(uint64_t), n*sizeof(rocketnet_address_t),
        n*sizeof(rocketnet_encryption_mapping_control_t), n*sizeof(rocketnet_encryption_mapping_t)});
}

struct rocketnet_encryption_map_t * rocketnet_encryption_map_create( void * context, int capacity )
{
    rocketnet_assert( capacity > 0 );

    // Preserve the table family's established capacity bound. Besides keeping
    // allocation products representable, this leaves both reserved uint32_t
    // index sentinels outside the stable public-index domain.
    if ( capacity < 1 || capacity > INT_MAX / 2 )
        return NULL;

    struct rocketnet_encryption_map_t * map = (struct rocketnet_encryption_map_t*) rocketnet_malloc( context, sizeof(struct rocketnet_encryption_map_t) );
    if ( !map )
        return NULL;
    memset( map, 0, sizeof(struct rocketnet_encryption_map_t) );

    map->context = context;
    map->capacity = capacity;
    map->next_generation = 1;
    randombytes_buf( map->hash_key, sizeof(map->hash_key) );
#if ROCKETNET_TEST_SEAMS
    (void) rocketnet_dev_session_sequence_start();          // the seam's one read, here at create
#endif // #if ROCKETNET_TEST_SEAMS

    map->index = (uint32_t*) rocketnet_malloc( context, sizeof(uint32_t) * (size_t) capacity );
    map->free_indices = (uint32_t*) rocketnet_malloc( context, sizeof(uint32_t) * (size_t) capacity );
    map->generation = (uint64_t*) rocketnet_malloc( context, sizeof(uint64_t) * (size_t) capacity );
    map->address = (struct rocketnet_address_t*) rocketnet_malloc( context, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    map->control = (struct rocketnet_encryption_mapping_control_t*) rocketnet_malloc( context, sizeof(struct rocketnet_encryption_mapping_control_t) * (size_t) capacity );
    map->mapping = (struct rocketnet_encryption_mapping_t*) rocketnet_malloc( context, sizeof(struct rocketnet_encryption_mapping_t) * (size_t) capacity );

    if ( !map->index || !map->free_indices || !map->generation || !map->address || !map->control || !map->mapping )
    {
        rocketnet_free( context, map->index );
        rocketnet_free( context, map->free_indices );
        rocketnet_free( context, map->generation );
        rocketnet_free( context, map->address );
        rocketnet_free( context, map->control );
        rocketnet_free( context, map->mapping );
        rocketnet_free( context, map );
        return NULL;
    }

    for ( int i = 0; i < capacity; i++ )
    {
        map->index[i] = ROCKETNET_ENCRYPTION_INDEX_EMPTY;
        map->generation[i] = 0;
        map->address[i].type = ROCKETNET_ADDRESS_NONE;
        // Pop returns 0, 1, ... initially while subsequent removes remain LIFO.
        map->free_indices[capacity - 1 - i] = (uint32_t) i;
    }
    map->num_free = capacity;

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
        if ( map->address[i].type != ROCKETNET_ADDRESS_NONE )
            rocketnet_encryption_mapping_wipe_secrets( &map->mapping[i] );
    rocketnet_free( map->context, map->index );
    rocketnet_free( map->context, map->free_indices );
    rocketnet_free( map->context, map->generation );
    rocketnet_free( map->context, map->address );
    rocketnet_free( map->context, map->control );
    rocketnet_free( map->context, map->mapping );
    rocketnet_free( map->context, map );
}

static uint64_t rocketnet_encryption_map_hash( const struct rocketnet_encryption_map_t * map,
                                               const struct rocketnet_address_t * address )
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
    return value;
}

// Returns the stable public slot index, or -1. insertion_position receives
// the first tombstone, else the first empty cell, for an absent key. A table
// with fewer than capacity live entries always has an empty or tombstone cell.
// At full capacity, an absent lookup examines all cells and stops. This keeps
// the old metadata budget exactly: the keyed index, dense free sequence and
// full-width generation tags together occupy the old bucket/next/live bytes.
static int rocketnet_encryption_map_lookup( const struct rocketnet_encryption_map_t * map,
                                            const struct rocketnet_address_t * address,
                                            uint64_t hash,
                                            int * insertion_position )
{
    const int home = (int) ( hash % (uint64_t) map->capacity );
    int tombstone = -1;
    for ( int probe = 0; probe < map->capacity; ++probe )
    {
        const int position = (int) ( ( (uint64_t) home + (uint64_t) probe ) % (uint64_t) map->capacity );
        const uint32_t candidate = map->index[position];
#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
        map->test_probes++;
#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
        if ( candidate == ROCKETNET_ENCRYPTION_INDEX_EMPTY )
        {
            if ( insertion_position )
                *insertion_position = tombstone >= 0 ? tombstone : position;
            return -1;
        }
        if ( candidate == ROCKETNET_ENCRYPTION_INDEX_TOMBSTONE )
        {
            if ( tombstone < 0 )
                tombstone = position;
            continue;
        }

        if ( candidate >= (uint32_t) map->capacity )
            continue;
        const int index = (int) candidate;
        if ( map->address[index].type != ROCKETNET_ADDRESS_NONE &&
             rocketnet_address_equal( &map->address[index], address ) )
        {
            if ( insertion_position )
                *insertion_position = position;
            return index;
        }
    }
    if ( insertion_position )
        *insertion_position = tombstone;
    return -1;
}

int rocketnet_encryption_map_find( const struct rocketnet_encryption_map_t * map, const struct rocketnet_address_t * address )
{
    rocketnet_assert( map );
    rocketnet_assert( address );

    const uint64_t hash = rocketnet_encryption_map_hash( map, address );
    return rocketnet_encryption_map_lookup( map, address, hash, NULL );
}

int rocketnet_encryption_map_full( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return map->num_free == 0 || map->next_generation == 0;
}

// tla/RNDestroyWipe.tla Add: inserts a live mapping, which then holds that mapping's secrets.
int rocketnet_encryption_map_add( struct rocketnet_encryption_map_t * map,
                                  const struct rocketnet_address_t * address,
                                  double handshake_deadline )
{
    rocketnet_assert( map );
    rocketnet_assert( address );
    if ( rocketnet_encryption_map_full( map ) )
        return -1;

    const uint64_t hash = rocketnet_encryption_map_hash( map, address );
    int index_position = -1;
    const int found = rocketnet_encryption_map_lookup( map, address, hash, &index_position );
    rocketnet_assert( found == -1 );
    if ( found != -1 || index_position < 0 )
        return -1;

    const int index = (int) map->free_indices[--map->num_free];
    rocketnet_assert( index >= 0 && index < map->capacity );
    const uint64_t generation = map->next_generation;
    map->next_generation = map->next_generation == UINT64_MAX ? 0 : map->next_generation + 1;

    map->address[index] = *address;
    map->generation[index] = generation;

    struct rocketnet_encryption_mapping_t * mapping = &map->mapping[index];
    memset( mapping, 0, sizeof(struct rocketnet_encryption_mapping_t) );
    mapping->control = &map->control[index];
    memset( mapping->control, 0, sizeof(*mapping->control) );
    mapping->control->client_index = -1;
    mapping->control->handshake_deadline = handshake_deadline;
    mapping->send_sequence_handshake = 1;
#if ROCKETNET_TEST_SEAMS
    mapping->send_sequence_session = rocketnet_dev_session_sequence_start();    // 1 unless the DEV seam moves it
#else // #if ROCKETNET_TEST_SEAMS
    mapping->send_sequence_session = 1;
#endif // #if ROCKETNET_TEST_SEAMS
    mapping->send_fragment_payload_id = 1;

    map->index[index_position] = (uint32_t) index;

    return index;
}

struct rocketnet_encryption_mapping_t * rocketnet_encryption_map_entry( struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->address[index].type != ROCKETNET_ADDRESS_NONE );
    return &map->mapping[index];
}

struct rocketnet_encryption_mapping_control_t * rocketnet_encryption_map_control( struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->address[index].type != ROCKETNET_ADDRESS_NONE );
    return &map->control[index];
}

const struct rocketnet_address_t * rocketnet_encryption_map_address( const struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->address[index].type != ROCKETNET_ADDRESS_NONE );
    return &map->address[index];
}

static bool rocketnet_encryption_map_remove_handle( struct rocketnet_encryption_map_t * map,
                                                    const struct rn_sequence_buffer_handle_t & handle )
{
    if ( handle.identity >= (uint64_t) map->capacity )
        return false;
    const int index = (int) handle.identity;
    const uint64_t generation = map->generation[index];
    if ( map->address[index].type == ROCKETNET_ADDRESS_NONE ||
         handle.generation != generation || handle.owner_epoch != generation || handle.ticket != generation )
        return false;

    const uint64_t hash = rocketnet_encryption_map_hash( map, &map->address[index] );
    int index_position = -1;
    if ( rocketnet_encryption_map_lookup( map, &map->address[index], hash, &index_position ) != index ||
         index_position < 0 )
        return false;
    map->index[index_position] = ROCKETNET_ENCRYPTION_INDEX_TOMBSTONE;

    // the secrets are wiped; the rest of the entry is unreachable once it is
    // not live, and the next add writes it whole
    rocketnet_encryption_mapping_wipe_secrets( &map->mapping[index] );
    map->address[index].type = ROCKETNET_ADDRESS_NONE;
    map->free_indices[map->num_free++] = (uint32_t) index;

    return true;
}

// tla/RNDestroyWipe.tla Remove: drops the mapping from live and wipes its secrets.
void rocketnet_encryption_map_remove( struct rocketnet_encryption_map_t * map, int index )
{
    rocketnet_assert( map );
    rocketnet_assert( index >= 0 && index < map->capacity );
    rocketnet_assert( map->address[index].type != ROCKETNET_ADDRESS_NONE );

    struct rn_sequence_buffer_handle_t handle = {};
    handle.identity = (uint64_t) index;
    handle.generation = map->generation[index];
    handle.owner_epoch = map->generation[index];
    handle.ticket = map->generation[index];
    const bool removed = rocketnet_encryption_map_remove_handle( map, handle );
    rocketnet_assert( removed );
    (void) removed;
}

bool rocketnet_encryption_map_reset( struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    if ( map->next_generation == 0 )
        return false;

    for ( int i = 0; i < map->capacity; ++i )
        if ( map->address[i].type != ROCKETNET_ADDRESS_NONE )
            rocketnet_encryption_mapping_wipe_secrets( &map->mapping[i] );

    map->num_free = map->capacity;
    for ( int i = 0; i < map->capacity; ++i )
    {
        map->index[i] = ROCKETNET_ENCRYPTION_INDEX_EMPTY;
        map->address[i].type = ROCKETNET_ADDRESS_NONE;
        map->free_indices[map->capacity - 1 - i] = (uint32_t) i;
    }
    return true;
}

int rocketnet_encryption_map_next( const struct rocketnet_encryption_map_t * map, int * iterator )
{
    rocketnet_assert( map );
    rocketnet_assert( iterator );

    for ( int i = *iterator + 1; i < map->capacity; i++ )
    {
        if ( map->address[i].type != ROCKETNET_ADDRESS_NONE )
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
    return map->capacity - map->num_free;
}

#if ROCKETNET_TEST_SEAMS
uint64_t rocketnet_encryption_map_test_hashes( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return map->test_hashes;
}
#endif // #if ROCKETNET_TEST_SEAMS

#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
uint64_t rocketnet_encryption_map_test_probes( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return map->test_probes;
}

uint64_t rocketnet_encryption_map_test_probe_bound( const struct rocketnet_encryption_map_t * map )
{
    rocketnet_assert( map );
    return (uint64_t) map->capacity;
}

int rocketnet_encryption_map_test_home( const struct rocketnet_encryption_map_t * map,
                                        const struct rocketnet_address_t * address )
{
    rocketnet_assert( map );
    rocketnet_assert( address );
    return (int) ( rocketnet_encryption_map_hash( map, address ) % (uint64_t) map->capacity );
}

bool rocketnet_encryption_map_test_handle( const struct rocketnet_encryption_map_t * map, int index,
                                           struct rn_sequence_buffer_handle_t * handle )
{
    rocketnet_assert( map );
    if ( handle == NULL || index < 0 || index >= map->capacity ||
         map->address[index].type == ROCKETNET_ADDRESS_NONE )
        return false;
    handle->identity = (uint64_t) index;
    handle->generation = map->generation[index];
    handle->owner_epoch = map->generation[index];
    handle->ticket = map->generation[index];
    return true;
}

bool rocketnet_encryption_map_test_remove_handle( struct rocketnet_encryption_map_t * map,
                                                  const struct rn_sequence_buffer_handle_t * handle )
{
    rocketnet_assert( map );
    return handle != NULL && rocketnet_encryption_map_remove_handle( map, *handle );
}
#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
