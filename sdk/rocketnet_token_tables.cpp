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

// The table's structure, laid out data-oriented: parallel fixed
// arrays allocated once at create, chained hashing with index links and a
// free list, and a hot/cold split -- the identifiers walked by find sit in
// their own array; addresses, timestamps and ordering are cold and touched
// only on a hit or an insert. Buckets are chosen by a per-table SipHash of
// the identifier, keyed with a random key generated at create, so an
// attacker cannot craft identifiers that pile into one chain. Nothing is
// ever overwritten in place across chains: eviction unlinks the victim from
// its own chain and relinks the slot under the new identifier's bucket, so
// every entry is always findable from its own bucket.

struct rocketnet_token_table_t
{
    void * context;
    int capacity;
    int num_buckets;
    uint64_t next_insert_order;
    uint8_t hash_key[crypto_shorthash_KEYBYTES];

    int * bucket_head;                                      // num_buckets, -1 when empty
    int * next;                                             // capacity: chain link, or free list link
    int free_head;

    uint8_t ( * identifier )[ROCKETNET_TOKEN_IDENTIFIER_BYTES];   // hot: walked by find
    struct rocketnet_address_t * address;                   // cold, history and claim
    uint64_t * expire_timestamp;                            // cold: the response binding's expiry
    double * claim_deadline;                                // cold: the request claim's handshake deadline
    uint64_t * insert_order;                                // cold
};

// the used-token history: two independent capacity tables, the response
// binding table and the request-claim table, so claim pressure cannot
// consume or evict response-binding capacity

struct rocketnet_used_token_history_t
{
    void * context;
    struct rocketnet_token_table_t * bindings;   // response-binding history
    struct rocketnet_token_table_t * claims;     // request-claim table
};

uint64_t rocketnet_token_table_allocation_bytes(int capacity)
{
    if (capacity < 1) return 0;
    const uint64_t n = (uint64_t)capacity;
    // the history holder plus two independent tables, the response-binding
    // table and the request-claim table, each with its own expiry and
    // deadline arrays: 1 + 8 + 8 = 17 allocations
    return rocketnet_memory_allocations({
        sizeof(rocketnet_used_token_history_t),
        sizeof(rocketnet_token_table_t), 2*n*sizeof(int), n*sizeof(int),
        n*ROCKETNET_TOKEN_IDENTIFIER_BYTES, n*sizeof(rocketnet_address_t),
        n*sizeof(uint64_t), n*sizeof(uint64_t), n*sizeof(uint64_t),
        sizeof(rocketnet_token_table_t), 2*n*sizeof(int), n*sizeof(int),
        n*ROCKETNET_TOKEN_IDENTIFIER_BYTES, n*sizeof(rocketnet_address_t),
        n*sizeof(uint64_t), n*sizeof(uint64_t), n*sizeof(uint64_t)
    });
}

static struct rocketnet_token_table_t * rocketnet_token_table_create( void * context, int capacity )
{
    rocketnet_assert( capacity > 0 );

    // num_buckets is capacity * 2 in a signed int, so a capacity above
    // INT_MAX / 2 wraps the bucket count and the bucket array is sized from
    // the wrapped value while the other arrays use the capacity. refuse it
    // before any allocation.
    if ( capacity < 1 || capacity > INT_MAX / 2 )
        return NULL;

    struct rocketnet_token_table_t * table = (struct rocketnet_token_table_t*) rocketnet_malloc( context, sizeof(struct rocketnet_token_table_t) );
    if ( !table )
        return NULL;
    memset( table, 0, sizeof(struct rocketnet_token_table_t) );

    table->context = context;
    table->capacity = capacity;
    table->num_buckets = capacity * 2;
    randombytes_buf( table->hash_key, sizeof(table->hash_key) );

    table->bucket_head = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) table->num_buckets );
    table->next = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    table->identifier = (uint8_t(*)[ROCKETNET_TOKEN_IDENTIFIER_BYTES]) rocketnet_malloc( context, ROCKETNET_TOKEN_IDENTIFIER_BYTES * (size_t) capacity );
    table->address = (struct rocketnet_address_t*) rocketnet_malloc( context, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    table->expire_timestamp = (uint64_t*) rocketnet_malloc( context, sizeof(uint64_t) * (size_t) capacity );
    table->claim_deadline = (double*) rocketnet_malloc( context, sizeof(double) * (size_t) capacity );
    table->insert_order = (uint64_t*) rocketnet_malloc( context, sizeof(uint64_t) * (size_t) capacity );

    if ( !table->bucket_head || !table->next || !table->identifier || !table->address || !table->expire_timestamp || !table->claim_deadline || !table->insert_order )
    {
        rocketnet_free( context, table->bucket_head );
        rocketnet_free( context, table->next );
        rocketnet_free( context, table->identifier );
        rocketnet_free( context, table->address );
        rocketnet_free( context, table->expire_timestamp );
        rocketnet_free( context, table->claim_deadline );
        rocketnet_free( context, table->insert_order );
        rocketnet_free( context, table );
        return NULL;
    }

    for ( int i = 0; i < table->num_buckets; i++ )
        table->bucket_head[i] = -1;

    for ( int i = 0; i < capacity; i++ )
        table->next[i] = i + 1 < capacity ? i + 1 : -1;
    table->free_head = 0;

    memset( table->identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES * (size_t) capacity );
    memset( table->address, 0, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    memset( table->expire_timestamp, 0, sizeof(uint64_t) * (size_t) capacity );
    memset( table->claim_deadline, 0, sizeof(double) * (size_t) capacity );
    memset( table->insert_order, 0, sizeof(uint64_t) * (size_t) capacity );

    return table;
}

static void rocketnet_token_table_destroy( struct rocketnet_token_table_t * table )
{
    rocketnet_assert( table );
    rocketnet_free( table->context, table->bucket_head );
    rocketnet_free( table->context, table->next );
    rocketnet_free( table->context, table->identifier );
    rocketnet_free( table->context, table->address );
    rocketnet_free( table->context, table->expire_timestamp );
    rocketnet_free( table->context, table->claim_deadline );
    rocketnet_free( table->context, table->insert_order );
    rocketnet_free( table->context, table );
}

static int rocketnet_token_table_bucket( const struct rocketnet_token_table_t * table, const uint8_t * identifier )
{
    uint8_t hash[crypto_shorthash_BYTES];
    crypto_shorthash( hash, identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES, table->hash_key );
    uint64_t value;
    memcpy( &value, hash, 8 );
    return (int) ( value % (uint64_t) table->num_buckets );
}

// the entry for this identifier, live or not. constant expected time: one
// keyed hash, one short chain walk.

static int rocketnet_token_table_find_any( const struct rocketnet_token_table_t * table,
                                           const uint8_t * identifier )
{
    int bucket = rocketnet_token_table_bucket( table, identifier );
    for ( int i = table->bucket_head[bucket]; i != -1; i = table->next[i] )
    {
        if ( sodium_memcmp( table->identifier[i], identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 )
            return i;
    }
    return -1;
}

// finds the live response binding for this identifier, treating an expired
// binding as absent. constant expected time: one keyed hash, one short chain
// walk.

static int rocketnet_token_table_find( const struct rocketnet_token_table_t * table,
                                       const uint8_t * identifier,
                                       uint64_t current_timestamp )
{
    int index = rocketnet_token_table_find_any( table, identifier );
    if ( index == -1 )
        return -1;
    return table->expire_timestamp[index] > current_timestamp ? index : -1;
}

// finds the live request claim for this identifier, treating an expired
// claim as absent.

static int rocketnet_token_table_find_claim( const struct rocketnet_token_table_t * table,
                                             const uint8_t * identifier,
                                             double current_time )
{
    int index = rocketnet_token_table_find_any( table, identifier );
    if ( index == -1 )
        return -1;
    return table->claim_deadline[index] > current_time ? index : -1;
}

static void rocketnet_token_table_unlink( struct rocketnet_token_table_t * table, int index )
{
    int bucket = rocketnet_token_table_bucket( table, table->identifier[index] );
    if ( table->bucket_head[bucket] == index )
    {
        table->bucket_head[bucket] = table->next[index];
        return;
    }
    for ( int i = table->bucket_head[bucket]; i != -1; i = table->next[i] )
    {
        if ( table->next[i] == index )
        {
            table->next[i] = table->next[index];
            return;
        }
    }
    rocketnet_assert( 0 );
}

// Selects a slot for a new entry: the free list first, then any entry whose
// binding (and, when protect_claims is set, claim) has expired, then -- only
// when evict_oldest is set -- the oldest such entry. A claim record sets
// protect_claims so a live claim is never replaced or evicted: it is the
// token-to-address claim that keeps one token to one half-open mapping until
// the handshake deadline. A binding record leaves protect_claims clear, as
// the history it writes is the documented capacity-bounded one. The scans run
// only on the record path, which is bounded by genuinely issued tokens; find
// stays hot and short. A used victim is unlinked from its own chain here.

static int rocketnet_token_table_select_slot( struct rocketnet_token_table_t * table,
                                              uint64_t current_timestamp,
                                              double current_time,
                                              int evict_oldest,
                                              int protect_claims )
{
    if ( table->free_head != -1 )
    {
        int index = table->free_head;
        table->free_head = table->next[index];
        return index;
    }

    int oldest = -1;
    for ( int i = 0; i < table->capacity; i++ )
    {
        const int claim_free = !protect_claims || table->claim_deadline[i] <= current_time;
        if ( claim_free && table->expire_timestamp[i] <= current_timestamp )
        {
            rocketnet_token_table_unlink( table, i );
            return i;
        }
        if ( claim_free && ( oldest == -1 || table->insert_order[i] < table->insert_order[oldest] ) )
            oldest = i;
    }

    if ( !evict_oldest || oldest == -1 )
        return -1;

    rocketnet_token_table_unlink( table, oldest );
    return oldest;
}

static void rocketnet_token_table_insert( struct rocketnet_token_table_t * table,
                                          int index,
                                          const uint8_t * identifier,
                                          const struct rocketnet_address_t * address,
                                          uint64_t expire_timestamp,
                                          double claim_deadline )
{
    memcpy( table->identifier[index], identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    if ( address )
        table->address[index] = *address;
    else
        memset( &table->address[index], 0, sizeof(struct rocketnet_address_t) );
    table->expire_timestamp[index] = expire_timestamp;
    table->claim_deadline[index] = claim_deadline;
    table->insert_order[index] = table->next_insert_order++;

    int bucket = rocketnet_token_table_bucket( table, identifier );
    table->next[index] = table->bucket_head[bucket];
    table->bucket_head[bucket] = index;
}

// the used-token history
//
// Two independent capacity tables, defined above: the response-binding table
// binds a token identifier to the first source address that opened a response
// under it and lives until the token expires; the request-claim table binds
// the same identifier to the first source address that presented a valid
// request, before any asymmetric work, and lives only until the handshake
// deadline or the response binding. The tables are independent, so claim
// pressure cannot consume or evict response-binding capacity: one token buys
// one half-open mapping, and a replay from a second address is refused at the
// claim rung while the first address's mapping is live.

struct rocketnet_used_token_history_t * rocketnet_used_token_history_create( void * context, int capacity )
{
    if ( capacity < 1 || capacity > INT_MAX / 2 )
        return NULL;

    struct rocketnet_used_token_history_t * history =
        (struct rocketnet_used_token_history_t*) rocketnet_malloc( context, sizeof(struct rocketnet_used_token_history_t) );
    if ( !history )
        return NULL;
    memset( history, 0, sizeof(struct rocketnet_used_token_history_t) );

    history->context = context;
    history->bindings = rocketnet_token_table_create( context, capacity );
    history->claims = rocketnet_token_table_create( context, capacity );
    if ( !history->bindings || !history->claims )
    {
        if ( history->bindings )
            rocketnet_token_table_destroy( history->bindings );
        if ( history->claims )
            rocketnet_token_table_destroy( history->claims );
        rocketnet_free( context, history );
        return NULL;
    }
    return history;
}

void rocketnet_used_token_history_destroy( struct rocketnet_used_token_history_t * history )
{
    if ( !history )
        return;
    if ( history->bindings )
        rocketnet_token_table_destroy( history->bindings );
    if ( history->claims )
        rocketnet_token_table_destroy( history->claims );
    rocketnet_free( history->context, history );
}

int rocketnet_used_token_history_check( const struct rocketnet_used_token_history_t * history,
                                        const uint8_t * token_identifier,
                                        const struct rocketnet_address_t * address,
                                        uint64_t current_timestamp )
{
    const struct rocketnet_token_table_t * table = history->bindings;
    int index = rocketnet_token_table_find( table, token_identifier, current_timestamp );
    if ( index == -1 )
        return 1;
    return rocketnet_address_equal( &table->address[index], address ) ? 1 : 0;
}

void rocketnet_used_token_history_record( struct rocketnet_used_token_history_t * history,
                                          const uint8_t * token_identifier,
                                          const struct rocketnet_address_t * address,
                                          uint64_t expire_timestamp,
                                          uint64_t current_timestamp )
{
    struct rocketnet_token_table_t * table = history->bindings;

    // a live binding is a no-op; an expired one is replaced by this new
    // binding to this address
    int index = rocketnet_token_table_find_any( table, token_identifier );
    if ( index != -1 )
    {
        if ( table->expire_timestamp[index] > current_timestamp )
            return;
        table->address[index] = *address;
        table->expire_timestamp[index] = expire_timestamp;
        table->insert_order[index] = table->next_insert_order++;
        return;
    }

    index = rocketnet_token_table_select_slot( table, current_timestamp, 0.0, 1, 0 );
    rocketnet_assert( index != -1 );

    rocketnet_token_table_insert( table, index, token_identifier, address, expire_timestamp, 0.0 );
}

// returns 1 when this token has no live claim, or holds it for this same
// address; 0 when another address holds a live claim.

int rocketnet_used_token_history_claim_check( const struct rocketnet_used_token_history_t * history,
                                              const uint8_t * token_identifier,
                                              const struct rocketnet_address_t * address,
                                              double current_time )
{
    const struct rocketnet_token_table_t * table = history->claims;
    int index = rocketnet_token_table_find_claim( table, token_identifier, current_time );
    if ( index == -1 )
        return 1;
    return rocketnet_address_equal( &table->address[index], address ) ? 1 : 0;
}

// records the request claim for this token until the handshake deadline. A
// live claim is never replaced or evicted; a full table refuses the record
// (0), and the request is refused before any crypto.

int rocketnet_used_token_history_claim_record( struct rocketnet_used_token_history_t * history,
                                               const uint8_t * token_identifier,
                                               const struct rocketnet_address_t * address,
                                               double deadline,
                                               double current_time )
{
    struct rocketnet_token_table_t * table = history->claims;

    int index = rocketnet_token_table_find_any( table, token_identifier );
    if ( index != -1 )
    {
        if ( table->claim_deadline[index] > current_time )
            return 1;
        table->address[index] = *address;
        table->claim_deadline[index] = deadline;
        table->insert_order[index] = table->next_insert_order++;
        return 1;
    }

    index = rocketnet_token_table_select_slot( table, 0, current_time, 1, 1 );
    if ( index == -1 )
        return 0;

    rocketnet_token_table_insert( table, index, token_identifier, address, 0, deadline );
    return 1;
}

// whether claim_record would return 1 now, without writing: the identifier
// is already in the table (a live claim for this address -- claim_check has
// refused any other -- or an expired entry to reuse), the free list has a
// slot, or some entry's claim has expired. These are exactly the cases in
// which claim_record finds or selects a slot.

int rocketnet_used_token_history_claim_can_record( const struct rocketnet_used_token_history_t * history,
                                                   const uint8_t * token_identifier,
                                                   double current_time )
{
    const struct rocketnet_token_table_t * table = history->claims;
    if ( rocketnet_token_table_find_any( table, token_identifier ) != -1 )
        return 1;
    if ( table->free_head != -1 )
        return 1;
    for ( int i = 0; i < table->capacity; i++ )
    {
        if ( table->claim_deadline[i] <= current_time )
            return 1;
    }
    return 0;
}

// releases this address's claim, when a request that holds one is refused
// before its mapping is added.

void rocketnet_used_token_history_claim_release( struct rocketnet_used_token_history_t * history,
                                                 const uint8_t * token_identifier,
                                                 const struct rocketnet_address_t * address )
{
    struct rocketnet_token_table_t * table = history->claims;
    int index = rocketnet_token_table_find_any( table, token_identifier );
    if ( index == -1 )
        return;
    if ( !rocketnet_address_equal( &table->address[index], address ) )
        return;
    table->claim_deadline[index] = 0.0;
}
