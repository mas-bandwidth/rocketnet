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
//
// The request-claim table adds an intrusive doubly linked order list over
// its live claims (order_prev and order_next, allocated at create beside the
// other arrays). A claim is appended at the tail when recorded, and every
// deadline is current_time + the handshake deadline, so while the server's
// time does not go backwards the head holds the earliest deadline. A
// released claim leaves its chain and the order list and goes back on the
// free list. Claim, release and the capacity test are then O(1): the free
// list head, else the order head when its deadline has passed, else full
// (tla/RNSecurityTables.tla, ClaimRecord, ClaimRelease and
// CanRecordMatchesScan).

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
    int free_count;                                         // slots on the free list

    int * order_prev;                                       // capacity: claim order list, -1 at the head
    int * order_next;                                       // capacity: claim order list, -1 at the tail
    int order_head;                                         // the live claim with the earliest deadline, or -1
    int order_tail;                                         // the most recently recorded claim, or -1

    uint8_t ( *identifier )[ROCKETNET_TOKEN_IDENTIFIER_BYTES];   // hot: walked by find
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

uint64_t rocketnet_token_table_allocation_bytes( int capacity )
{
    if ( capacity < 1 )
        return 0;
    const uint64_t n = (uint64_t) capacity;
    // the history holder plus two independent tables, the response-binding
    // table and the request-claim table, each with its own expiry and
    // deadline arrays and its two order-list link arrays: 1 + 10 + 10 = 21
    // allocations
    return rocketnet_memory_allocations( { sizeof( rocketnet_used_token_history_t ),
                                           sizeof( rocketnet_token_table_t ), 2 * n * sizeof( int ), n * sizeof( int ),
                                           n * ROCKETNET_TOKEN_IDENTIFIER_BYTES, n * sizeof( rocketnet_address_t ),
                                           n * sizeof( uint64_t ), n * sizeof( uint64_t ), n * sizeof( uint64_t ),
                                           n * sizeof( int ), n * sizeof( int ),
                                           sizeof( rocketnet_token_table_t ), 2 * n * sizeof( int ), n * sizeof( int ),
                                           n * ROCKETNET_TOKEN_IDENTIFIER_BYTES, n * sizeof( rocketnet_address_t ),
                                           n * sizeof( uint64_t ), n * sizeof( uint64_t ), n * sizeof( uint64_t ),
                                           n * sizeof( int ), n * sizeof( int ) } );
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

    struct rocketnet_token_table_t * table = (struct rocketnet_token_table_t *) rocketnet_malloc( context, sizeof( struct rocketnet_token_table_t ) );
    if ( !table )
        return NULL;
    memset( table, 0, sizeof( struct rocketnet_token_table_t ) );

    table->context = context;
    table->capacity = capacity;
    table->num_buckets = capacity * 2;
    randombytes_buf( table->hash_key, sizeof( table->hash_key ) );

    table->bucket_head = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) table->num_buckets );
    table->next = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
    table->identifier = (uint8_t( * )[ROCKETNET_TOKEN_IDENTIFIER_BYTES]) rocketnet_malloc( context, ROCKETNET_TOKEN_IDENTIFIER_BYTES * (size_t) capacity );
    table->address = (struct rocketnet_address_t *) rocketnet_malloc( context, sizeof( struct rocketnet_address_t ) * (size_t) capacity );
    table->expire_timestamp = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    table->claim_deadline = (double *) rocketnet_malloc( context, sizeof( double ) * (size_t) capacity );
    table->insert_order = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    table->order_prev = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
    table->order_next = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );

    if ( !table->bucket_head || !table->next || !table->identifier || !table->address || !table->expire_timestamp || !table->claim_deadline || !table->insert_order ||
         !table->order_prev || !table->order_next )
    {
        rocketnet_free( context, table->bucket_head );
        rocketnet_free( context, table->next );
        rocketnet_free( context, table->identifier );
        rocketnet_free( context, table->address );
        rocketnet_free( context, table->expire_timestamp );
        rocketnet_free( context, table->claim_deadline );
        rocketnet_free( context, table->insert_order );
        rocketnet_free( context, table->order_prev );
        rocketnet_free( context, table->order_next );
        rocketnet_free( context, table );
        return NULL;
    }

    for ( int i = 0; i < table->num_buckets; i++ )
        table->bucket_head[i] = -1;

    for ( int i = 0; i < capacity; i++ )
        table->next[i] = i + 1 < capacity ? i + 1 : -1;
    table->free_head = 0;
    table->free_count = capacity;

    for ( int i = 0; i < capacity; i++ )
    {
        table->order_prev[i] = -1;
        table->order_next[i] = -1;
    }
    table->order_head = -1;
    table->order_tail = -1;

    memset( table->identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES * (size_t) capacity );
    memset( table->address, 0, sizeof( struct rocketnet_address_t ) * (size_t) capacity );
    memset( table->expire_timestamp, 0, sizeof( uint64_t ) * (size_t) capacity );
    memset( table->claim_deadline, 0, sizeof( double ) * (size_t) capacity );
    memset( table->insert_order, 0, sizeof( uint64_t ) * (size_t) capacity );

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
    rocketnet_free( table->context, table->order_prev );
    rocketnet_free( table->context, table->order_next );
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

static int rocketnet_token_table_free_pop( struct rocketnet_token_table_t * table )
{
    int index = table->free_head;
    if ( index == -1 )
        return -1;
    table->free_head = table->next[index];
    table->free_count--;
    return index;
}

static void rocketnet_token_table_free_push( struct rocketnet_token_table_t * table, int index )
{
    table->next[index] = table->free_head;
    table->free_head = index;
    table->free_count++;
}

// the claim order list: append at the tail on record, remove from anywhere
// on release or reuse (tla/RNSecurityTables.tla, ClaimRecord and
// ClaimRelease, invariant FreeAndOrderPartitionSlots)

static void rocketnet_token_table_order_append( struct rocketnet_token_table_t * table, int index )
{
    table->order_prev[index] = table->order_tail;
    table->order_next[index] = -1;
    if ( table->order_tail != -1 )
        table->order_next[table->order_tail] = index;
    else
        table->order_head = index;
    table->order_tail = index;
}

static void rocketnet_token_table_order_remove( struct rocketnet_token_table_t * table, int index )
{
    const int prev = table->order_prev[index];
    const int next = table->order_next[index];
    if ( prev != -1 )
        table->order_next[prev] = next;
    else
        table->order_head = next;
    if ( next != -1 )
        table->order_prev[next] = prev;
    else
        table->order_tail = prev;
    table->order_prev[index] = -1;
    table->order_next[index] = -1;
}

// Selects a slot for a new response binding: the free list first, then any
// entry whose binding has expired, then the oldest entry, live or not: the
// history it writes is the documented capacity-bounded one. The response-
// binding table only; the request-claim table selects in O(1) from its free
// list and its order list (rocketnet_used_token_history_claim_record). A used
// victim is unlinked from its own chain here.

static int rocketnet_token_table_select_slot( struct rocketnet_token_table_t * table,
                                              uint64_t current_timestamp )
{
    int index = rocketnet_token_table_free_pop( table );
    if ( index != -1 )
        return index;

    int oldest = -1;
    for ( int i = 0; i < table->capacity; i++ )
    {
        if ( table->expire_timestamp[i] <= current_timestamp )
        {
            rocketnet_token_table_unlink( table, i );
            return i;
        }
        if ( oldest == -1 || table->insert_order[i] < table->insert_order[oldest] )
            oldest = i;
    }

    if ( oldest == -1 )
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
        memset( &table->address[index], 0, sizeof( struct rocketnet_address_t ) );
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
        (struct rocketnet_used_token_history_t *) rocketnet_malloc( context, sizeof( struct rocketnet_used_token_history_t ) );
    if ( !history )
        return NULL;
    memset( history, 0, sizeof( struct rocketnet_used_token_history_t ) );

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

    index = rocketnet_token_table_select_slot( table, current_timestamp );
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
// (0), and the request is refused before any crypto. O(1): the entry for this
// identifier when there is one, else the free list head, else the order
// list's head when its deadline has passed (tla/RNSecurityTables.tla,
// ClaimRecord, invariants LiveClaimNeverReplaced and OrderIsDeadlineOrder).
// Should a deadline arrive out of order, a later expired claim waits behind
// a live head: its reuse is delayed, and a live claim is still never taken.

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
        rocketnet_token_table_order_remove( table, index );
        rocketnet_token_table_order_append( table, index );
        return 1;
    }

    index = rocketnet_token_table_free_pop( table );
    if ( index == -1 )
    {
        index = table->order_head;
        if ( index == -1 || table->claim_deadline[index] > current_time )
            return 0;
        rocketnet_token_table_unlink( table, index );
        rocketnet_token_table_order_remove( table, index );
    }

    rocketnet_token_table_insert( table, index, token_identifier, address, 0, deadline );
    rocketnet_token_table_order_append( table, index );
    return 1;
}

// whether claim_record would return 1 now, without writing: the identifier
// is already in the table (a live claim for this address -- claim_check has
// refused any other -- or an expired entry to reuse), the free list has a
// slot, or the order list's head, the earliest deadline, has expired. These
// are exactly the cases in which claim_record finds or selects a slot, and
// the answer is O(1): no walk over the table (tla/RNSecurityTables.tla,
// CanRecordMatchesScan).

int rocketnet_used_token_history_claim_can_record( const struct rocketnet_used_token_history_t * history,
                                                   const uint8_t * token_identifier,
                                                   double current_time )
{
    const struct rocketnet_token_table_t * table = history->claims;
    if ( rocketnet_token_table_find_any( table, token_identifier ) != -1 )
        return 1;
    if ( table->free_head != -1 )
        return 1;
    return table->order_head != -1 && table->claim_deadline[table->order_head] <= current_time;
}

int rocketnet_used_token_history_claim_free_count( const struct rocketnet_used_token_history_t * history )
{
    return history->claims->free_count;
}

// releases this address's claim, when a request that holds one is refused
// before its mapping is added, or when the half-open mapping that holds it is
// removed before the handshake deadline. The entry leaves its chain and the
// order list and its slot goes back on the free list, so a second release,
// which no longer finds it, or a release from another address changes
// nothing (tla/RNSecurityTables.tla, ClaimRelease, invariant
// FreeAndOrderPartitionSlots).

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
    rocketnet_token_table_unlink( table, index );
    rocketnet_token_table_order_remove( table, index );
    table->claim_deadline[index] = 0.0;
    rocketnet_token_table_free_push( table, index );
}
