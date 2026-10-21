/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "rocketnet_ack.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_server_protocol.h"

extern int rocketnet_server_transport_test_update_client_stats( const rocketnet_client_slots_t *, rocketnet_server_acks_t * );

struct idle_stats_fixture_t
{
    rocketnet_client_slots_t * slots;
    rocketnet_server_acks_t * acks;
    idle_stats_fixture_t( int capacity )
    {
        slots = rocketnet_client_slots_create( NULL, capacity );
        acks = rocketnet_server_acks_create( NULL, capacity );
        rocketnet_unit_check( slots && acks );
    }
    ~idle_stats_fixture_t()
    {
        rocketnet_server_acks_destroy( acks );
        rocketnet_client_slots_destroy( slots );
    }
    int phase( uint64_t phase )
    {
        acks->update_count = phase;
        return rocketnet_server_transport_test_update_client_stats( slots, acks );
    }
    int assign( uint64_t identity )
    {
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
        return rocketnet_client_slots_assign( slots, identity, -1, user_data );
    }
};

static void test_server_idle_stats_skips_free_endpoints()
{
    const int capacities[] = { 1, 20, 21, 1000 };
    for ( int capacity : capacities )
    {
        idle_stats_fixture_t f( capacity );
        // A free endpoint with a sample would publish a nonzero RTT if the
        // cold stats function were called despite the slot being free.
        f.acks->endpoints[capacity - 1].rtt_history[0] = 13.0f;
        int calls = 0;
        const int phases = ( capacity + ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE - 1 ) /
                           ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE;
        for ( int phase = 0; phase < phases; phase++ )
            calls += f.phase( phase );
        printf( "capacity=%d free endpoint stats calls=%d\n", capacity, calls );
        rocketnet_unit_check( calls == 0 );
        rocketnet_unit_check( f.acks->endpoints[capacity - 1].rtt == 0.0f );
    }
}

static void test_server_idle_stats_preserves_phase_reset_and_reuse()
{
    idle_stats_fixture_t f( 101 );
    // Six modulo phases for this capacity; keep slots in two of them.
    for ( int i = 0; i < 13; i++ )
        rocketnet_unit_check( f.assign( (uint64_t) i + 1 ) == i );
    for ( int i = 0; i < 13; i++ )
        if ( i != 1 && i != 12 )
            rocketnet_client_slots_release( f.slots, i );

    rocketnet_ack_endpoint_t * endpoint = &f.acks->endpoints[12];
    const double now = 1000.0;
    rocketnet_ack_endpoint_record_received( endpoint, 1, 100, now );
    rocketnet_ack_endpoint_record_received( endpoint, 2, 100, now + 0.01 );
    endpoint->rtt_history[0] = 5.0f;
    endpoint->rtt_history[1] = 7.0f;
    rocketnet_ack_endpoint_t * expected = new rocketnet_ack_endpoint_t( *endpoint );
    rocketnet_ack_endpoint_update_stats( expected );

    int calls = 0;
    const int phases = ( 101 + ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE - 1 ) /
                       ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE;
    for ( int phase = 1; phase <= phases; phase++ )
    {
        const int visited = f.phase( phase );
        rocketnet_unit_check( visited == ( phase == 1 || phase == 6 ? 1 : 0 ) );
        calls += visited;
        if ( phase < 6 )
            rocketnet_unit_check( endpoint->rtt == 0.0f );
    }
    rocketnet_unit_check( calls == 2 );
    rocketnet_unit_check( endpoint->rtt == expected->rtt );
    rocketnet_unit_check( endpoint->jitter == expected->jitter );
    rocketnet_unit_check( endpoint->packet_loss == expected->packet_loss );
    rocketnet_unit_check( endpoint->bandwidth_sent_kbps == expected->bandwidth_sent_kbps );
    rocketnet_unit_check( endpoint->bandwidth_received_kbps == expected->bandwidth_received_kbps );
    rocketnet_unit_check( endpoint->bandwidth_acked_kbps == expected->bandwidth_acked_kbps );
    delete expected;

    rocketnet_server_protocol_t protocol = {};
    protocol.client_slots = f.slots;
    protocol.acks = f.acks;
    rocketnet_server_connection_reset( &protocol, 12 );
    rocketnet_unit_check( endpoint->rtt == 0.0f && endpoint->jitter == 0.0f );
    rocketnet_unit_check( endpoint->bandwidth_received_kbps == 0.0f );
    rocketnet_unit_check( f.phase( 6 ) == 0 );
    rocketnet_unit_check( f.assign( 100 ) == 12 );
    endpoint->rtt_history[0] = 9.0f;
    rocketnet_unit_check( f.phase( 7 ) == 1 ); // slot 1; reused slot 12 still waits
    rocketnet_unit_check( endpoint->rtt == 0.0f );
    rocketnet_unit_check( f.phase( 12 ) == 1 );
    rocketnet_unit_check( endpoint->rtt == 9.0f && endpoint->jitter == 0.0f );
}

static void test_server_stats_modulo_schedule_caps_twenty_visits()
{
    const int capacities[] = { 1, 20, 21, 1000 };
    for ( int capacity : capacities )
    {
        idle_stats_fixture_t f( capacity );
        for ( int i = 0; i < capacity; i++ )
        {
            rocketnet_unit_check( f.assign( (uint64_t) i + 1 ) == i );
            f.acks->endpoints[i].rtt_history[0] = (float) i + 1.0f;
        }
        const int phases = ( capacity + ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE - 1 ) /
                           ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE;
        int calls = 0;
        for ( int phase = 0; phase < phases; phase++ )
        {
            const int updated = f.phase( (uint64_t) phase );
            rocketnet_unit_check( updated > 0 && updated <= ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE );
            calls += updated;
        }
        rocketnet_unit_check( calls == capacity );
        for ( int i = 0; i < capacity; i++ )
            rocketnet_unit_check( f.acks->endpoints[i].rtt == (float) i + 1.0f );
        const int rollover = f.phase( (uint64_t) phases );
        rocketnet_unit_check( rollover > 0 && rollover <= ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE );
    }
}

REGISTER_UNIT_TEST( test_server_idle_stats_skips_free_endpoints );
REGISTER_UNIT_TEST( test_server_idle_stats_preserves_phase_reset_and_reuse );
REGISTER_UNIT_TEST( test_server_stats_modulo_schedule_caps_twenty_visits );
