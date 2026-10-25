/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "rocketnet_sequence_buffer.h"

#include <stdint.h>

#define check rocketnet_unit_check

typedef rn_sequence_buffer_t<uint64_t, 4, RN_SEQUENCE_BUFFER_HISTORY> history4_t;
typedef rn_sequence_buffer_t<uint64_t, 4, RN_SEQUENCE_BUFFER_OWNED> owned4_t;

static rn_sequence_buffer_handle_t put_owned( owned4_t & buffer, uint64_t identity, uint64_t value )
{
    rn_sequence_buffer_handle_t handle;
    check( buffer.insert( identity, value, &handle ) == RN_SEQUENCE_BUFFER_ADMITTED );
    return handle;
}

static rn_sequence_buffer_handle_t put_history( history4_t & buffer, uint64_t identity, uint64_t value )
{
    rn_sequence_buffer_handle_t handle;
    check( buffer.insert( identity, value, &handle ) == RN_SEQUENCE_BUFFER_ADMITTED );
    return handle;
}

static void test_sequence_buffer_full_width_aliases()
{
    owned4_t buffer;
    rn_sequence_buffer_handle_t low = put_owned( buffer, 1, 11 );
    rn_sequence_buffer_handle_t ignored;

    // These identities differ beyond the old 16-bit boundary and alias the
    // same modulo cell. A live owner is refused, never mistaken for a match.
    check( buffer.insert( 1 + 65536, 12, &ignored ) == RN_SEQUENCE_BUFFER_SLOT_LIVE );
    check( buffer.find( low ) != NULL && *buffer.find( low ) == 11 );
    check( buffer.remove( low ) );
    rn_sequence_buffer_handle_t max16 = put_owned( buffer, 65535, 12 );
    check( max16.identity == 65535 );
    check( buffer.remove( max16 ) );
    rn_sequence_buffer_handle_t wide16 = put_owned( buffer, 65536, 13 );
    check( wide16.identity == 65536 && *buffer.find( wide16 ) == 13 );
    check( buffer.remove( wide16 ) );

    rn_sequence_buffer_handle_t u32max = put_owned( buffer, UINT32_MAX, 14 );
    check( u32max.identity == UINT32_MAX );
    check( buffer.remove( u32max ) );
    rn_sequence_buffer_handle_t above32 = put_owned( buffer, (uint64_t) UINT32_MAX + 1, 15 );
    check( above32.identity == (uint64_t) UINT32_MAX + 1 );
    check( buffer.find( above32 ) != NULL && *buffer.find( above32 ) == 15 );
}

static void test_sequence_buffer_history_sparse_laps_and_boundaries()
{
    history4_t buffer;
    rn_sequence_buffer_handle_t n_minus_1 = put_history( buffer, 3, 3 );
    rn_sequence_buffer_handle_t n = put_history( buffer, 4, 4 );
    rn_sequence_buffer_handle_t n_plus_1 = put_history( buffer, 5, 5 );
    check( buffer.find( n_minus_1 ) != NULL );
    check( buffer.find( n ) != NULL );
    check( buffer.find( n_plus_1 ) != NULL );

    // One full sparse lap retires only history outside [newest-N+1,newest].
    rn_sequence_buffer_handle_t lap = put_history( buffer, 9, 9 );
    check( buffer.find( n_minus_1 ) == NULL );
    check( buffer.find( n ) == NULL );
    check( buffer.find( n_plus_1 ) == NULL );
    check( buffer.find( lap ) != NULL );

    // A many-N gap has the same capacity-bounded behavior.
    rn_sequence_buffer_handle_t many = put_history( buffer, 1000009, 99 );
    check( buffer.find( lap ) == NULL );
    check( buffer.find( many ) != NULL && *buffer.find( many ) == 99 );
    check( buffer.size() == 1 );
}

static void test_sequence_buffer_owned_sparse_lap_refuses_live_overwrite()
{
    owned4_t buffer;
    rn_sequence_buffer_handle_t held = put_owned( buffer, 7, 70 );
    rn_sequence_buffer_handle_t ignored;
    check( buffer.insert( 7 + 4, 71, &ignored ) == RN_SEQUENCE_BUFFER_SLOT_LIVE );
    check( buffer.insert( 7 + 4000000, 72, &ignored ) == RN_SEQUENCE_BUFFER_SLOT_LIVE );
    check( buffer.find( held ) != NULL && *buffer.find( held ) == 70 );
    check( buffer.size() == 1 );
}

static void test_sequence_buffer_late_history_refusal_has_no_mutation()
{
    history4_t buffer;
    rn_sequence_buffer_handle_t ten = put_history( buffer, 10, 10 );
    rn_sequence_buffer_handle_t eleven = put_history( buffer, 11, 11 );
    const size_t before = buffer.size();
    rn_sequence_buffer_handle_t ignored;
    check( buffer.insert( 7, 7, &ignored ) == RN_SEQUENCE_BUFFER_TOO_OLD );
    check( buffer.size() == before );
    check( buffer.find( ten ) != NULL && *buffer.find( ten ) == 10 );
    check( buffer.find( eleven ) != NULL && *buffer.find( eleven ) == 11 );
}

static void test_sequence_buffer_stale_generation_and_double_remove()
{
    owned4_t buffer;
    rn_sequence_buffer_handle_t first = put_owned( buffer, 20, 1 );
    check( buffer.remove( first ) );
    check( !buffer.remove( first ) );
    rn_sequence_buffer_handle_t second = put_owned( buffer, 20, 2 );
    check( second.generation != first.generation );
    check( buffer.find( first ) == NULL );
    check( !buffer.remove( first ) );
    check( buffer.find( second ) != NULL && *buffer.find( second ) == 2 );

    rn_sequence_buffer_handle_t wrong_owner = second;
    ++wrong_owner.owner_epoch;
    check( buffer.find( wrong_owner ) == NULL );
    check( !buffer.remove( wrong_owner ) );
}

static void test_sequence_buffer_cancel_requeue_leaves_no_tombstones()
{
    owned4_t buffer;
    for ( uint64_t round = 0; round < 100; ++round )
    {
        rn_sequence_buffer_handle_t handles[4];
        for ( uint64_t i = 0; i < 4; ++i )
            handles[i] = put_owned( buffer, round * 4 + i, round + i );
        check( buffer.remove( handles[1] ) );
        check( buffer.remove( handles[3] ) );
        check( buffer.remove( handles[0] ) );
        check( buffer.remove( handles[2] ) );
        check( buffer.size() == 0 );
    }
}

static void test_sequence_buffer_reset_reuses_ids_without_reviving_handles()
{
    owned4_t buffer;
    rn_sequence_buffer_handle_t old = put_owned( buffer, 42, 1 );
    const uint64_t old_epoch = buffer.owner_epoch();
    check( buffer.reset() );
    check( buffer.owner_epoch() == old_epoch + 1 );
    check( buffer.find( old ) == NULL );
    check( !buffer.remove( old ) );
    rn_sequence_buffer_handle_t current = put_owned( buffer, 42, 2 );
    check( current.owner_epoch != old.owner_epoch );
    check( buffer.find( current ) != NULL && *buffer.find( current ) == 2 );

    buffer.initialize( UINT64_MAX, 1 );
    rn_sequence_buffer_handle_t final_epoch = put_owned( buffer, 1, 3 );
    check( !buffer.reset() );
    check( buffer.find( final_epoch ) != NULL );
}

static void test_sequence_buffer_ticket_boundary_and_exhaustion()
{
    owned4_t buffer;
    buffer.initialize( 9, UINT64_MAX - 1 );
    rn_sequence_buffer_handle_t penultimate = put_owned( buffer, 1, 1 );
    rn_sequence_buffer_handle_t last = put_owned( buffer, 2, 2 );
    check( penultimate.ticket == UINT64_MAX - 1 );
    check( last.ticket == UINT64_MAX );
    check( buffer.tickets_exhausted() );

    rn_sequence_buffer_handle_t ignored;
    check( buffer.insert( 3, 3, &ignored ) == RN_SEQUENCE_BUFFER_TICKETS_EXHAUSTED );
    check( buffer.size() == 2 );
    check( buffer.find( penultimate ) != NULL );
    check( buffer.find( last ) != NULL );

    check( buffer.reset() );
    rn_sequence_buffer_handle_t restarted = put_owned( buffer, 1, 4 );
    check( restarted.ticket == 1 );
    check( restarted.owner_epoch == 10 );
    check( buffer.find( penultimate ) == NULL );
}

static void test_sequence_buffer_generation_boundary_refuses_wrap()
{
    owned4_t buffer;
    buffer.initialize( 4, 1, UINT64_MAX - 1 );
    rn_sequence_buffer_handle_t last_generation = put_owned( buffer, 1, 1 );
    check( last_generation.generation == UINT64_MAX );
    check( buffer.remove( last_generation ) );

    rn_sequence_buffer_handle_t ignored;
    check( buffer.insert( 1, 2, &ignored ) == RN_SEQUENCE_BUFFER_GENERATION_EXHAUSTED );
    check( buffer.size() == 0 );
    check( buffer.reset() );
    rn_sequence_buffer_handle_t restarted = put_owned( buffer, 1, 3 );
    check( restarted.generation == 1 );
    check( restarted.owner_epoch == 5 );
}

REGISTER_UNIT_TEST( test_sequence_buffer_full_width_aliases );
REGISTER_UNIT_TEST( test_sequence_buffer_history_sparse_laps_and_boundaries );
REGISTER_UNIT_TEST( test_sequence_buffer_owned_sparse_lap_refuses_live_overwrite );
REGISTER_UNIT_TEST( test_sequence_buffer_late_history_refusal_has_no_mutation );
REGISTER_UNIT_TEST( test_sequence_buffer_stale_generation_and_double_remove );
REGISTER_UNIT_TEST( test_sequence_buffer_cancel_requeue_leaves_no_tombstones );
REGISTER_UNIT_TEST( test_sequence_buffer_reset_reuses_ids_without_reviving_handles );
REGISTER_UNIT_TEST( test_sequence_buffer_ticket_boundary_and_exhaustion );
REGISTER_UNIT_TEST( test_sequence_buffer_generation_boundary_refuses_wrap );
