/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "channel_harness.h"

static void unreliable_budget_check_impl( bool condition, const char * label, int line )
{
    if ( condition )
        return;
    printf( "unreliable receive budget failed: %s at line %d\n", label, line );
    fflush( stdout );
    exit( 1 );
}

#define unreliable_budget_check( condition, label ) unreliable_budget_check_impl( condition, label, __LINE__ )

static void unreliable_budget_payload( int id, uint8_t data[4] )
{
    data[0] = (uint8_t) id;
    data[1] = (uint8_t) ( id ^ 0x5A );
    data[2] = (uint8_t) ( id + 17 );
    data[3] = (uint8_t) ( id * 3 + 1 );
}

static int unreliable_budget_matches( const rocketnet_channel_delivery_t * delivery, int id )
{
    uint8_t expected[4];
    unreliable_budget_payload( id, expected );
    return delivery->bytes == (int) sizeof( expected ) &&
           delivery->hash == rocketnet_channel_hash( expected, (int) sizeof( expected ) ) &&
           memcmp( delivery->data, expected, sizeof( expected ) ) == 0;
}

void unreliable_over_receive_budget_dropped_and_counted()
{
    const int budget = 8;
    const int count = 12;
    rocketnet_channel_harness_t h;
    unreliable_budget_check( rocketnet_channel_harness_create_with_unreliable_messages_per_update( &h, budget ), "create" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );
    unreliable_budget_check( rocketnet_channel_harness_connect( &h, 600 ), "connect" );
    unreliable_budget_check( rocketnet_channel_harness_set_budgets( &h, 10000, 10000 ), "send budget" );

    for ( int i = 0; i < count; i++ )
    {
        uint8_t data[4];
        unreliable_budget_payload( i, data );
        unreliable_budget_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, data, sizeof( data ) ) != 0, "send" );
    }

    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

    uint32_t seen = 0;
    rocketnet_channel_delivery_t delivery;
    while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
    {
        const int id = delivery.bytes > 0 ? delivery.data[0] : -1;
        unreliable_budget_check( id >= 0 && id < count && unreliable_budget_matches( &delivery, id ), "payload intact" );
        unreliable_budget_check( !( seen & ( 1u << id ) ), "no duplicate" );
        seen |= 1u << id;
    }

    const uint64_t dropped = h.message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL];

    unreliable_budget_check( seen == ( ( 1u << budget ) - 1 ), "exact receive budget delivered" );
    unreliable_budget_check( dropped == (uint64_t) ( count - budget ), "excess counted" );

    for ( int tick = 0; tick < 3; tick++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        unreliable_budget_check( !rocketnet_channel_harness_server_collect( &h, 0, &delivery ), "no later delivery" );
    }

    unreliable_budget_check( h.message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL] == dropped, "drop count stable" );
    unreliable_budget_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "no allocations after create" );
    rocketnet_channel_harness_destroy( &h );
}

void reliable_delivered_when_unreliable_budget_full()
{
    const int budget = 8;
    const int unreliable_count = 12;
    const int reliable_count = 4;
    rocketnet_channel_harness_t h;
    unreliable_budget_check( rocketnet_channel_harness_create_with_unreliable_messages_per_update( &h, budget ), "create" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );
    unreliable_budget_check( rocketnet_channel_harness_connect( &h, 600 ), "connect" );
    unreliable_budget_check( rocketnet_channel_harness_set_budgets( &h, 10000, 10000 ), "send budget" );

    for ( int i = 0; i < unreliable_count; i++ )
    {
        uint8_t data[4];
        unreliable_budget_payload( i, data );
        unreliable_budget_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, data, sizeof( data ) ) != 0, "send unreliable" );
    }

    for ( int i = 0; i < reliable_count; i++ )
    {
        uint8_t data[4];
        unreliable_budget_payload( 32 + i, data );
        unreliable_budget_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE, data, sizeof( data ) ) != 0, "send reliable" );
    }

    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

    uint32_t unreliable_seen = 0;
    uint32_t reliable_seen = 0;
    rocketnet_channel_delivery_t delivery;
    while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
    {
        const int id = delivery.bytes > 0 ? delivery.data[0] : -1;
        unreliable_budget_check( id >= 0 && unreliable_budget_matches( &delivery, id ), "payload intact" );
        if ( id < unreliable_count )
        {
            unreliable_budget_check( !( unreliable_seen & ( 1u << id ) ), "no duplicate unreliable" );
            unreliable_seen |= 1u << id;
        }
        else
        {
            const int reliable_id = id - 32;
            unreliable_budget_check( reliable_id >= 0 && reliable_id < reliable_count, "unexpected reliable payload" );
            unreliable_budget_check( !( reliable_seen & ( 1u << reliable_id ) ), "no duplicate reliable" );
            reliable_seen |= 1u << reliable_id;
        }
    }

    unreliable_budget_check( unreliable_seen == ( ( 1u << budget ) - 1 ), "unreliable budget full" );
    unreliable_budget_check( reliable_seen == ( ( 1u << reliable_count ) - 1 ), "all reliable delivered" );
    unreliable_budget_check( h.message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL] == (uint64_t) ( unreliable_count - budget ), "excess unreliable counted" );

    for ( int tick = 0; tick < 3; tick++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        unreliable_budget_check( !rocketnet_channel_harness_server_collect( &h, 0, &delivery ), "no later delivery" );
    }

    unreliable_budget_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "no allocations after create" );
    rocketnet_channel_harness_destroy( &h );
}
