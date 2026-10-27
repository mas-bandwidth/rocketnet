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
// (see CMakeLists.txt).

#include "test_unit_registry.h"

#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_address.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV
extern "C" void rocketnet_dev_token_table_reset_operation_counts( struct rocketnet_used_token_history_t * history );
extern "C" uint64_t rocketnet_dev_token_claim_probe_steps( const struct rocketnet_used_token_history_t * history );
#endif

#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS

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
}

REGISTER_UNIT_TEST( test_encryption_map_churn_probe_bound );

#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS

#if ROCKETNET_DEV

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
}

REGISTER_UNIT_TEST( test_used_token_table_churn_probe_bound );

#endif // #if ROCKETNET_DEV
