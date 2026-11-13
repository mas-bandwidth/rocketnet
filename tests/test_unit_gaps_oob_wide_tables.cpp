/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The backward-shift erase of the encryption map's index and of the token
// claim table, at the end of the index where the scan wraps. Each case finds
// keys by their home under the table's own random keyed hash (the DEV home
// seams), so it reaches the same layout on every run. No sockets, no real time
// and no threads; the home and step seams are ROCKETNET_DEV, so the cases are
// registered only there.

#include "test_unit_registry.h"

#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_address.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

extern "C" int rocketnet_dev_token_claim_index_start( const struct rocketnet_used_token_history_t * history,
                                                      const uint8_t * token_identifier );
extern "C" uint64_t rocketnet_dev_token_erase_steps();

static struct rocketnet_address_t gaps_oob_wide_address( int ordinal )
{
    struct rocketnet_address_t address;
    char text[64];
    snprintf( text, sizeof( text ), "10.%d.%d.%d:%d", ( ordinal >> 16 ) & 255, ( ordinal >> 8 ) & 255, ordinal & 255,
              20000 + ordinal % 40000 );
    rocketnet_unit_check( rocketnet_address_parse( &address, text ) );
    return address;
}

// mutation gap: me rocketnet_encryption_map.cpp:351 wrap to cell 1
//
// Two addresses whose home is the last index cell: the first sits there, the
// second wraps to cell 0. Erasing the first leaves a hole at the last cell; the
// erase scan wraps to cell 0 and moves the second back into the hole, its
// home. A scan that wraps to cell 1 instead meets the empty cell 1, stops, and
// leaves the second at cell 0 behind an empty home, where no find reaches it.

static void test_gaps_oob_wide_encryption_map_erase_wraps_to_cell_zero()
{
    const int capacity = 4;
    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, capacity );
    rocketnet_unit_check( map != NULL );

    const int last = 2 * capacity - 1;
    struct rocketnet_address_t found[2];
    int count = 0;
    for ( int ordinal = 1; ordinal < 100000 && count < 2; ++ordinal )
    {
        const struct rocketnet_address_t address = gaps_oob_wide_address( ordinal );
        if ( rocketnet_encryption_map_test_home( map, &address ) == last )
        {
            found[count++] = address;
        }
    }
    rocketnet_unit_check( count == 2 );

    const int first = rocketnet_encryption_map_add( map, &found[0], 1000.0 );
    const int second = rocketnet_encryption_map_add( map, &found[1], 1000.0 );
    rocketnet_unit_check( first >= 0 && second >= 0 && first != second );
    rocketnet_unit_check( rocketnet_encryption_map_find( map, &found[0] ) == first );
    rocketnet_unit_check( rocketnet_encryption_map_find( map, &found[1] ) == second );

    rocketnet_encryption_map_remove_current( map, first );
    rocketnet_unit_check( rocketnet_encryption_map_find( map, &found[0] ) == -1 );

    const int after = rocketnet_encryption_map_find( map, &found[1] );
    if ( after != second )
    {
        printf( "fault: the mapping that wrapped to cell 0 was lost when the one at the last cell was erased: the erase scan did not wrap to cell 0\n" );
    }
    rocketnet_unit_check( after == second );

    rocketnet_encryption_map_destroy( map );
}

REGISTER_UNIT_TEST( test_gaps_oob_wide_encryption_map_erase_wraps_to_cell_zero );

// mutation gap: hunt 2/773
//
// The claim table's erase stops at the empty cell that ends the released
// claim's cluster. Without the stop it reads the identifier of the EMPTY entry
// (identifier[-1], inside the allocator's header before the array, so no
// sanitizer sees it) and scans every cell of the index on each release. Two
// clusters separated by empty cells, the second wrapping the index end; the
// release in the first scans its one follower and the empty cell after it, two
// cells, and every other claim still refuses a second address.

static void gaps_oob_wide_token_id( uint8_t * identifier, uint32_t ordinal )
{
    memset( identifier, 0xA5, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    memcpy( identifier, &ordinal, sizeof( ordinal ) );
}

static void test_gaps_oob_wide_token_claim_erase_stops_at_the_empty_cell()
{
    const int capacity = 8;
    const int cells = 2 * capacity;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    rocketnet_unit_check( history != NULL );

    // ids[0], ids[1]: home the last cell (cells 15 and 0); ids[2], ids[3]: home 4 (cells 4 and 5)

    uint8_t ids[4][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    int wrapped = 0;
    int middle = 0;
    for ( uint32_t ordinal = 1; ordinal < 100000 && ( wrapped < 2 || middle < 2 ); ++ordinal )
    {
        uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        gaps_oob_wide_token_id( identifier, ordinal );
        const int home = rocketnet_dev_token_claim_index_start( history, identifier );
        if ( home == cells - 1 && wrapped < 2 )
        {
            memcpy( ids[wrapped++], identifier, sizeof( identifier ) );
        }
        else if ( home == 4 && middle < 2 )
        {
            memcpy( ids[2 + middle++], identifier, sizeof( identifier ) );
        }
    }
    rocketnet_unit_check( wrapped == 2 && middle == 2 );

    const double now = 1000.0;
    const struct rocketnet_address_t holder = gaps_oob_wide_address( 1 );
    const struct rocketnet_address_t other = gaps_oob_wide_address( 2 );
    for ( int i = 0; i < 4; ++i )
    {
        rocketnet_unit_check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, now + 10.0, now ) == 1 );
    }

    const uint64_t before = rocketnet_dev_token_erase_steps();
    rocketnet_used_token_history_claim_release( history, ids[2], &holder );
    const uint64_t steps = rocketnet_dev_token_erase_steps() - before;
    if ( steps != 2 )
    {
        printf( "fault: releasing a claim scanned %llu index cells, expected 2 (its follower and the empty cell after it): the erase did not stop at the empty cell ending the cluster\n",
                (unsigned long long) steps );
    }
    rocketnet_unit_check( steps == 2 );

    rocketnet_unit_check( rocketnet_used_token_history_claim_check( history, ids[2], &other, now ) == 1 );
    const int still_held[3] = { 0, 1, 3 };
    for ( int i = 0; i < 3; ++i )
    {
        const bool refused = rocketnet_used_token_history_claim_check( history, ids[still_held[i]], &other, now ) == 0;
        if ( !refused )
        {
            printf( "fault: a live claim became unfindable after another claim's release\n" );
        }
        rocketnet_unit_check( refused );
    }

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_gaps_oob_wide_token_claim_erase_stops_at_the_empty_cell );

#endif // #if ROCKETNET_DEV
