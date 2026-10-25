/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_ack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( ROCKETNET_ACK_COST_TEST_HOOKS )

static uint64_t ack_cost_allocations;
static void * ack_cost_malloc( void *, size_t bytes )
{
    ack_cost_allocations++;
    return malloc( bytes );
}
static void ack_cost_free( void *, void * pointer ) { free( pointer ); }

// Drive the production window/allocator with controlled time, no sockets or
// sleeps. Cost is the actual predecessor reads, separate from hash probes or
// ACK invocations. Ascending, descending and interleaved orders all used to
// read k*(k-1)/2 slots, even though span release ignores that computed rank.
static void ack_span_cost_case( uint32_t count, int order )
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.max_message_bytes = 512 * 1024;
    config.receive_message_buffer_bytes = 512 * 1024;
    config.send_message_buffer_bytes = 2 * 1024 * 1024;
    rocketnet_unit_check( rocketnet_message_validate_config( &config ) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
    rocketnet_unit_check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );
    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );
    const uint64_t allocations = ack_cost_allocations;
    const uint32_t descriptors = region->descriptors_free;
    const uint32_t bytes = count * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t d = rn_region_send_alloc_span( region, (int) bytes );
    rocketnet_unit_check( d != RN_DESCRIPTOR_NONE );
    rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    rocketnet_unit_check( descriptor->flags & RN_SEND_FLAG_SPAN );
    descriptor->flags |= RN_SEND_FLAG_RECEIPT_RELIABLE;
    descriptor->serial = 77;
    uint64_t first = 0, position = 0;
    rocketnet_unit_check( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                                                            bytes, d, &first, &position ) == count );
    rocketnet_unit_check( first == 0 );
    for ( uint32_t i = 0; i < count; i++ )
    {
        const uint64_t number = first + i;
        rn_window_ack_commit_packet( &ack, i + 1, &number, 1, 1000.0 );
    }
    // Two retransmission entries outlive their first completion. One is
    // acknowledged immediately, one after the descriptor has been reused.
    const uint64_t repeated = order == 1 ? count - 1 : 0;
    rn_window_ack_commit_packet( &ack, count + 1, &repeated, 1, 1000.001 );
    rn_window_ack_commit_packet( &ack, count + 2, &repeated, 1, 1000.002 );

    bool seen[256] = {};
    rocketnet_unit_check( count <= 256 );
    uint64_t expected_u = 0;
    rn_window_ack_receipt_begin_update( &ack );
    rn_window_ack_cost_reset();
    for ( uint32_t step = 0; step < count; step++ )
    {
        const uint32_t part = order == 0 ? step : order == 1 ? count - 1 - step
                                                             : ( step % 2 == 0 ? step / 2 : count - 1 - step / 2 );
        rocketnet_unit_check( !seen[part] );
        seen[part] = true;
        while ( expected_u < count && seen[expected_u] )
            expected_u++;
        const double now = 1000.01 + (double) step * 0.001;
        rocketnet_unit_check( rn_window_ack_on_ack( &ack, part + 1, now ) == 1 );
        rocketnet_unit_check( rn_window_ack_on_ack( &ack, part + 1, now ) == 0 );
        rocketnet_unit_check( rn_window_ack_on_ack( &ack, 10000, now ) == 0 );
        if ( step == 0 )
            rocketnet_unit_check( rn_window_ack_on_ack( &ack, count + 1, now ) == 1 );
        rocketnet_unit_check( descriptor->fragments_acked == step + 1 );
        rocketnet_unit_check( send.U == expected_u );
        rocketnet_unit_check( send.U_prime == ( step + 1 == count ? count : 0 ) );
        rocketnet_unit_check( ack.resend_count == count - step - 1 );
        rocketnet_unit_check( descriptor->in_use == ( step + 1 < count ) );
        rocketnet_unit_check( region->descriptors_free == descriptors - ( step + 1 < count ? 1 : 0 ) );
        rocketnet_unit_check( ack.receipt_tail == ( step + 1 == count ? 1 : 0 ) );
    }
    const rn_ack_cost_t cost = rn_window_ack_cost_read();
    printf( "span ACK cost: fragments=%u order=%d predecessor_visits=%llu completed_numbers=%llu\n",
            count, order, (unsigned long long) cost.predecessor_visits, (unsigned long long) cost.completed_numbers );
    fflush( stdout );
    rocketnet_unit_check( cost.completed_numbers == count );
    rocketnet_unit_check( cost.predecessor_visits == 0 );
    rocketnet_unit_check( region->span_first == RN_DESCRIPTOR_NONE && region->span_last == RN_DESCRIPTOR_NONE );
    rocketnet_unit_check( region->span_head == 0 && region->span_tail == 0 );
    rn_window_ack_receipt_end_update( &ack );
    rocketnet_message_receipt_t receipt[2];
    rocketnet_unit_check( rn_window_ack_receipts( &ack, receipt, 2 ) == 1 );
    rocketnet_unit_check( receipt[0].serial == 77 && receipt[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );

    // A distinct late packet ACK still finds its map entry, but its old
    // number cannot acknowledge a new message using the released descriptor.
    const uint32_t reused = rn_region_send_alloc_span( region, (int) bytes );
    rocketnet_unit_check( reused == d );
    rocketnet_unit_check( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                                                            bytes, reused, &first, &position ) == count );
    rocketnet_unit_check( first == count );
    rocketnet_unit_check( rn_window_ack_on_ack( &ack, count + 2, 1001.0 ) == 1 );
    rocketnet_unit_check( descriptor->in_use && descriptor->fragments_acked == 0 );
    rocketnet_unit_check( ack.receipt_tail == 1 );
    rocketnet_unit_check( rn_window_ack_cost_read().completed_numbers == count );
    rocketnet_unit_check( rn_window_ack_cost_read().predecessor_visits == 0 );
    rocketnet_unit_check( ack_cost_allocations == allocations );
    rn_region_send_free( region, reused );
    rn_arena_destroy( arena );
}

static void ack_span_cost_size( uint32_t count )
{
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, ack_cost_malloc, ack_cost_free ) == ROCKETNET_STATUS_OK );
    for ( int order = 0; order < 3; order++ )
        ack_span_cost_case( count, order );
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static void test_ack_span_cost_64() { ack_span_cost_size( 64 ); }
static void test_ack_span_cost_128() { ack_span_cost_size( 128 ); }
static void test_ack_span_cost_256() { ack_span_cost_size( 256 ); }
REGISTER_UNIT_TEST( test_ack_span_cost_64 );
REGISTER_UNIT_TEST( test_ack_span_cost_128 );
REGISTER_UNIT_TEST( test_ack_span_cost_256 );

#endif
