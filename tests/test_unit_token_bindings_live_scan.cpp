/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The bindings table full of unexpired bindings (sdk/rocketnet_token_tables.cpp,
// rn_token_binding_select_slot). The history counts the slots each insertion
// reads (DEV only). No socket, no real time: timestamps are arguments from 1000.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_token_tables.h"

#include <string.h>

#if ROCKETNET_DEV

extern "C" void rocketnet_dev_token_table_reset_operation_counts( struct rocketnet_used_token_history_t * history );
extern "C" uint64_t rocketnet_dev_token_binding_slot_steps( const struct rocketnet_used_token_history_t * history );

static void tb_live_id( uint8_t * identifier, int round, int index )
{
    memset( identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    identifier[0] = (uint8_t) index;
    identifier[1] = (uint8_t) ( index >> 8 );
    identifier[2] = (uint8_t) round;
    identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES - 1] = (uint8_t) ( 0xA5 ^ index );
}

// A table of 8 x 1000 bindings, none expired: a new binding evicts the oldest
// with a slot or two read, never a scan of every slot for an expired one
// that is not there. The evicted binding is the oldest; the next oldest still
// refuses another address.
static void test_binding_full_of_live_bindings_evicts_oldest_in_constant_time()
{
    const int capacity = 8 * 1000;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    rocketnet_unit_check( history );

    struct rocketnet_address_t holder, other;
    rocketnet_unit_check( rocketnet_address_parse( &holder, "198.51.100.7:50000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &other, "198.51.100.8:50000" ) );

    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < capacity; ++i )
    {
        tb_live_id( id, 0, i );
        rocketnet_used_token_history_record( history, id, &holder, 2000, 1000 );
    }

    for ( int i = 0; i < 10; ++i )
    {
        tb_live_id( id, 1, i );
        rocketnet_dev_token_table_reset_operation_counts( history );
        rocketnet_used_token_history_record( history, id, &holder, 2000, 1001 );
        rocketnet_unit_check( rocketnet_dev_token_binding_slot_steps( history ) <= 2 );
    }

    tb_live_id( id, 0, 9 );
    rocketnet_unit_check( rocketnet_used_token_history_check( history, id, &other, 1001 ) == 1 );
    tb_live_id( id, 0, 10 );
    rocketnet_unit_check( rocketnet_used_token_history_check( history, id, &other, 1001 ) == 0 );

    rocketnet_used_token_history_destroy( history );
}
REGISTER_UNIT_TEST( test_binding_full_of_live_bindings_evicts_oldest_in_constant_time );

// Different lifetimes: with the oldest binding live and a later one expired,
// the expired one is taken and the oldest is kept, as before.
static void test_binding_expired_later_binding_taken_before_live_oldest()
{
    const int capacity = 8;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    rocketnet_unit_check( history );

    struct rocketnet_address_t holder, other;
    rocketnet_unit_check( rocketnet_address_parse( &holder, "198.51.100.7:50000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &other, "198.51.100.8:50000" ) );

    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < capacity; ++i )
    {
        tb_live_id( id, 0, i );
        rocketnet_used_token_history_record( history, id, &holder, i == 5 ? 1005 : 2000, 1000 );
    }

    // live table: the oldest goes, and the short-lived binding stays bound
    tb_live_id( id, 1, 0 );
    rocketnet_used_token_history_record( history, id, &holder, 2000, 1001 );
    tb_live_id( id, 0, 0 );
    rocketnet_unit_check( rocketnet_used_token_history_check( history, id, &other, 1001 ) == 1 );

    // binding 5 has expired: it is taken, binding 1 (the oldest) is kept
    tb_live_id( id, 1, 1 );
    rocketnet_used_token_history_record( history, id, &holder, 2000, 1010 );
    tb_live_id( id, 0, 1 );
    rocketnet_unit_check( rocketnet_used_token_history_check( history, id, &other, 1010 ) == 0 );
    tb_live_id( id, 1, 1 );
    rocketnet_unit_check( rocketnet_used_token_history_check( history, id, &other, 1010 ) == 0 );

    rocketnet_used_token_history_destroy( history );
}
REGISTER_UNIT_TEST( test_binding_expired_later_binding_taken_before_live_oldest );

#endif // #if ROCKETNET_DEV
