/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Churn probe bound: after churn of at least 64 times capacity (remove one
// key, add a fresh one), the worst absent-key probe count stays at or below
// the worst count on a freshly filled table holding the same live keys at
// the same load. With tombstone deletion the churned table degrades toward
// a full-table scan and this check is red; with backward-shift deletion the
// churned index is the canonical layout for its live set and it is green.
//
// Time is an argument; no sockets, real time or threads. The probe counters
// are DEV test seams, so this file is registered only under ROCKETNET_DEV
// (see CMakeLists.txt). Each layout case seeds the tables' SipHash keys
// (sdk/rocketnet_test_seams.h) and a failing check prints the seed.

#include "test_unit_registry.h"

#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_address.h"
#include "rocketnet_test_seams.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

// a failing check prints the hash-key seed its case set, so the layout it
// failed on is reproducible; the condition is evaluated once
#define check( condition )                                                                               \
do                                                                                                       \
{                                                                                                        \
    const bool check_passed = ( condition );                                                             \
    if ( !check_passed && rocketnet_dev_hash_key_seed() != 0 )                                           \
        printf( "hash key seed 0x%016llx\n", (unsigned long long) rocketnet_dev_hash_key_seed() );       \
    rocketnet_unit_check( check_passed && #condition );                                                  \
} while ( 0 )

extern "C" void rocketnet_dev_token_table_reset_operation_counts( struct rocketnet_used_token_history_t * history );
extern "C" uint64_t rocketnet_dev_token_claim_probe_steps( const struct rocketnet_used_token_history_t * history );
extern "C" int rocketnet_dev_token_claim_index_start( const struct rocketnet_used_token_history_t * history,
                                                      const uint8_t * token_identifier );


static struct rocketnet_address_t churn_map_address( int ordinal )
{
    struct rocketnet_address_t address;
    char text[64];
    snprintf( text, sizeof( text ), "10.%d.%d.%d:%d", ( ordinal >> 16 ) & 255,
              ( ordinal >> 8 ) & 255, ordinal & 255, 20000 + ordinal % 40000 );
    check( rocketnet_address_parse( &address, text ) );
    return address;
}

static uint64_t churn_map_worst_absent_probes( struct rocketnet_encryption_map_t * map,
                                               const struct rocketnet_address_t * absent, int absent_count )
{
    uint64_t worst = 0;
    for ( int i = 0; i < absent_count; ++i )
    {
        const uint64_t before = rocketnet_encryption_map_test_probes( map );
        check( rocketnet_encryption_map_find( map, &absent[i] ) == -1 );
        const uint64_t probes = rocketnet_encryption_map_test_probes( map ) - before;
        if ( probes > worst )
            worst = probes;
    }
    return worst;
}

static void test_encryption_map_churn_probe_bound()
{
    rocketnet_dev_set_hash_key_seed( UINT64_C( 0x5EED0000000000A1 ) );

    const int capacity = 32;
    const int load = 24;
    const int absent_count = 128;

    struct rocketnet_address_t absent[128];
    for ( int i = 0; i < absent_count; ++i )
        absent[i] = churn_map_address( 500000 + i );

    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, capacity );
    check( map != NULL );

    struct rocketnet_address_t live_addr[24];
    int live_index[24];
    for ( int i = 0; i < load; ++i )
    {
        live_addr[i] = churn_map_address( 1 + i );
        live_index[i] = rocketnet_encryption_map_add( map, &live_addr[i], 1000.0 );
        check( live_index[i] >= 0 );
    }
    const uint64_t fresh_worst = churn_map_worst_absent_probes( map, absent, absent_count );

    // Each cycle removes a permanent key, adds a transient fresh key,
    // removes the transient and re-adds the permanent key. The live set at
    // the end is exactly the freshly filled set, so a tombstone-free index
    // has exactly its fresh layout; tombstones instead accumulate at the
    // transient positions and merge probe clusters.
    const int churn_cycles = 64 * capacity;
    for ( int cycle = 0; cycle < churn_cycles; ++cycle )
    {
        const int victim = cycle % load;
        const struct rocketnet_address_t transient = churn_map_address( 10000 + cycle );
        rocketnet_encryption_map_remove_current( map, live_index[victim] );
        const int transient_index = rocketnet_encryption_map_add( map, &transient, 1000.0 );
        check( transient_index >= 0 );
        rocketnet_encryption_map_remove_current( map, transient_index );
        live_index[victim] = rocketnet_encryption_map_add( map, &live_addr[victim], 1000.0 );
        check( live_index[victim] >= 0 );
    }
    check( rocketnet_encryption_map_num_mappings( map ) == load );
    for ( int i = 0; i < load; ++i )
        check( rocketnet_encryption_map_find( map, &live_addr[i] ) == live_index[i] );

    const uint64_t churned_worst = churn_map_worst_absent_probes( map, absent, absent_count );

    printf( "test_encryption_map_churn_probe_bound: churned worst %llu, fresh worst %llu (capacity %d, load %d, churn %d)\n",
            (unsigned long long) churned_worst, (unsigned long long) fresh_worst, capacity, load, churn_cycles );

    rocketnet_encryption_map_destroy( map );

    check( churned_worst <= fresh_worst );
    rocketnet_dev_set_hash_key_seed( 0 );
}

REGISTER_UNIT_TEST( test_encryption_map_churn_probe_bound );



static void churn_token_id( uint8_t * identifier, uint32_t value )
{
    memset( identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    memcpy( identifier, &value, sizeof( value ) );
    identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES - 1] = (uint8_t) ( 0xA5 ^ ( value & 0xFF ) );
}

static uint64_t churn_token_worst_absent_probes( struct rocketnet_used_token_history_t * history,
                                                 const struct rocketnet_address_t * holder,
                                                 uint8_t ( *absent )[ROCKETNET_TOKEN_IDENTIFIER_BYTES], int absent_count )
{
    uint64_t worst = 0;
    for ( int i = 0; i < absent_count; ++i )
    {
        rocketnet_dev_token_table_reset_operation_counts( history );
        check( rocketnet_used_token_history_claim_check( history, absent[i], holder, 1000.0 ) == 1 );
        const uint64_t probes = rocketnet_dev_token_claim_probe_steps( history );
        if ( probes > worst )
            worst = probes;
    }
    return worst;
}

static void test_used_token_table_churn_probe_bound()
{
    rocketnet_dev_set_hash_key_seed( UINT64_C( 0x5EED0000000000A2 ) );

    const int capacity = 32;
    const int load = 24;
    const int absent_count = 128;
    const double now = 1000.0;

    struct rocketnet_address_t holder, other;
    check( rocketnet_address_parse( &holder, "198.51.100.7:50000" ) );
    check( rocketnet_address_parse( &other, "198.51.100.8:50000" ) );

    uint8_t absent[128][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < absent_count; ++i )
        churn_token_id( absent[i], 900000u + (uint32_t) i );

    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history != NULL );

    uint8_t live_id[24][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < load; ++i )
    {
        churn_token_id( live_id[i], (uint32_t) ( 1 + i ) );
        check( rocketnet_used_token_history_claim_record( history, live_id[i], &holder, now + 10.0, now ) == 1 );
    }
    const uint64_t fresh_worst = churn_token_worst_absent_probes( history, &holder, absent, absent_count );

    // Same cycle as the encryption map case: remove a permanent claim,
    // record a transient fresh claim, release it and re-record the
    // permanent claim, so the final live set is the freshly filled set.
    const int churn_cycles = 64 * capacity;
    uint32_t next_value = 10000;
    for ( int cycle = 0; cycle < churn_cycles; ++cycle )
    {
        const int victim = cycle % load;
        uint8_t transient[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        churn_token_id( transient, next_value++ );
        rocketnet_used_token_history_claim_release( history, live_id[victim], &holder );
        check( rocketnet_used_token_history_claim_record( history, transient, &holder, now + 10.0, now ) == 1 );
        rocketnet_used_token_history_claim_release( history, transient, &holder );
        check( rocketnet_used_token_history_claim_record( history, live_id[victim], &holder, now + 10.0, now ) == 1 );
    }
    for ( int i = 0; i < load; ++i )
    {
        check( rocketnet_used_token_history_claim_check( history, live_id[i], &holder, now ) == 1 );
        check( rocketnet_used_token_history_claim_check( history, live_id[i], &other, now ) == 0 );
    }

    const uint64_t churned_worst = churn_token_worst_absent_probes( history, &holder, absent, absent_count );

    printf( "test_used_token_table_churn_probe_bound: churned worst %llu, fresh worst %llu (capacity %d, load %d, churn %d)\n",
            (unsigned long long) churned_worst, (unsigned long long) fresh_worst, capacity, load, churn_cycles );

    rocketnet_used_token_history_destroy( history );

    check( churned_worst <= fresh_worst );
    rocketnet_dev_set_hash_key_seed( 0 );
}

REGISTER_UNIT_TEST( test_used_token_table_churn_probe_bound );

// The hash-key seed seam: one seed gives every table of a kind the same
// layout, so a seeded case is reproducible; another seed gives another
// layout; seed zero is the OS entropy draw again.

static bool seed_layouts_match( uint64_t seed_a, uint64_t seed_b, int * kinds_matching )
{
    struct rocketnet_address_t addresses[64];
    uint8_t identifiers[64][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 64; ++i )
    {
        addresses[i] = churn_map_address( 1 + i );
        churn_token_id( identifiers[i], (uint32_t) ( 1 + i ) );
    }

    rocketnet_dev_set_hash_key_seed( seed_a );
    struct rocketnet_client_slots_t * slots_a = rocketnet_client_slots_create( NULL, 32 );
    struct rocketnet_used_token_history_t * history_a = rocketnet_used_token_history_create( NULL, 32 );
    struct rocketnet_encryption_map_t * map_a = rocketnet_encryption_map_create( NULL, 32 );
    rocketnet_dev_set_hash_key_seed( seed_b );
    struct rocketnet_client_slots_t * slots_b = rocketnet_client_slots_create( NULL, 32 );
    struct rocketnet_used_token_history_t * history_b = rocketnet_used_token_history_create( NULL, 32 );
    struct rocketnet_encryption_map_t * map_b = rocketnet_encryption_map_create( NULL, 32 );
    rocketnet_dev_set_hash_key_seed( 0 );
    check( slots_a && history_a && map_a && slots_b && history_b && map_b );

    bool slots_match = true, history_match = true, map_match = true;
    for ( int i = 0; i < 64; ++i )
    {
        const uint64_t id = UINT64_C( 0x80000000A5A5A5A5 ) + (uint64_t) i;
        slots_match = slots_match && rocketnet_client_slots_test_hash_index( slots_a, id ) ==
                                         rocketnet_client_slots_test_hash_index( slots_b, id );
        history_match = history_match && rocketnet_dev_token_claim_index_start( history_a, identifiers[i] ) ==
                                             rocketnet_dev_token_claim_index_start( history_b, identifiers[i] );
        map_match = map_match && rocketnet_encryption_map_test_home( map_a, &addresses[i] ) ==
                                     rocketnet_encryption_map_test_home( map_b, &addresses[i] );
    }

    rocketnet_client_slots_destroy( slots_a );
    rocketnet_client_slots_destroy( slots_b );
    rocketnet_used_token_history_destroy( history_a );
    rocketnet_used_token_history_destroy( history_b );
    rocketnet_encryption_map_destroy( map_a );
    rocketnet_encryption_map_destroy( map_b );

    *kinds_matching = (int) slots_match + (int) history_match + (int) map_match;
    return slots_match && history_match && map_match;
}

static void test_hash_key_seed_reproduces_every_table_layout()
{
    int kinds_matching = 0;

    // the same seed: all three kinds lay out identically

    check( seed_layouts_match( UINT64_C( 0x5EED0000000000A3 ), UINT64_C( 0x5EED0000000000A3 ), &kinds_matching ) );
    check( kinds_matching == 3 );

    // another seed, and no seed: 64 keys over 64 buckets land identically
    // with odds far below one in 2^64, so no kind matches

    check( !seed_layouts_match( UINT64_C( 0x5EED0000000000A3 ), UINT64_C( 0x5EED0000000000A4 ), &kinds_matching ) );
    check( kinds_matching == 0 );
    check( !seed_layouts_match( 0, 0, &kinds_matching ) );
    check( kinds_matching == 0 );
}

REGISTER_UNIT_TEST( test_hash_key_seed_reproduces_every_table_layout );

#endif // #if ROCKETNET_DEV
