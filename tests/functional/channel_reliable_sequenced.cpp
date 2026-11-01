/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNChannelTypes!Dispatch, !Drain and !ReceiveAck over a deterministic lossy link.
#include "channel_harness.h"

#include <stdio.h>
#include <stdlib.h>

static void rs_functional_check( bool condition, const char * label )
{
    if ( condition )
        return;
    printf( "reliable state stream failed: %s\n", label );
    fflush( stdout );
    exit( 1 );
}

static void rs_state_bytes( uint32_t state, uint8_t bytes[4] )
{
    for ( int i = 0; i < 4; i++ )
    {
        bytes[i] = (uint8_t) ( state >> ( i * 8 ) );
    }
}

static uint32_t rs_state_read( const uint8_t * bytes )
{
    uint32_t state = 0;

    for ( int i = 0; i < 4; i++ )
    {
        state |= (uint32_t) bytes[i] << ( i * 8 );
    }

    return state;
}

void reliable_state_stream_over_lossy_link()
{
    rocketnet_channel_harness_t h;
    rs_functional_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    rs_functional_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.025, 10.0f, 5.0f );

    uint32_t last = 0;
    uint32_t delivered = 0;
    uint32_t accepted = 0;

    for ( uint32_t tick = 1; tick <= 120 + 900; tick++ )
    {
        if ( tick <= 120 )
        {
            uint8_t bytes[4];
            rs_state_bytes( tick, bytes );

            const int type = tick == 60 ? ROCKETNET_RELIABILITY_RELIABLE_ORDERED : ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED;
            rs_functional_check( rocketnet_channel_harness_client_send( &h, 0, type, bytes, 4 ) != 0, "send" );
            accepted++;
        }

        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

        rocketnet_channel_delivery_t d = {};
        while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            rs_functional_check( d.bytes == 4, "payload length" );

            const uint32_t state = rs_state_read( d.data );
            rs_functional_check( state > last, "state moved backwards or duplicated" );
            last = state;
            delivered++;
        }
    }

    rocketnet_channel_harness_read_counters( &h );
    rs_functional_check( last == 120, "last reliable state did not arrive" );
    rs_functional_check( delivered >= 20, "too few deliveries" );
    rs_functional_check( h.resends > 0, "loss caused no resend" );
    rs_functional_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rs_functional_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "allocation after create" );
    printf( "test_reliable_state_stream_over_lossy_link: accepted=%u delivered=%u last=%u resends=%llu wire_to_server=%lld wire_to_client=%lld seed=0x%016llx\n",
            accepted, delivered, last, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client,
            (unsigned long long) h.seed );
    rocketnet_channel_harness_destroy( &h );
}

// Each payload names its send position and whether it is the ordered record.
static const uint32_t rs_ordered_flag = 0x80000000u;

static uint32_t rs_server_stale( const rocketnet_channel_harness_t * h )
{
    return (uint32_t) h->message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE];
}

// RNChannelTypes!Dispatch (I12) on the shipped path: a code-4 state that is
// late for its ordered epoch is acked and discarded, and its receive number is
// released. A leaked number keeps its descriptor live until the number W above
// it arrives and collides with it, so the stream stops after about W numbers:
// what the user sees is a channel that goes quiet. This run sends one code-4
// state per tick with an ordered record every eighth tick, more than 1024
// messages over a lossy reordering link, and finishes only when every
// ordered record and the newest state have arrived.
void reliable_sequenced_long_run_across_epochs()
{
    enum
    {
        states = 1400,
        ordered_every = 8
    };

    rocketnet_channel_harness_t h;
    rs_functional_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    rs_functional_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.01, 0.05, 10.0f, 5.0f );

    uint32_t position = 0;
    uint32_t ordered_sent = 0;
    uint32_t last_position = 0;
    uint32_t ordered_seen = 0;
    uint32_t states_seen = 0;
    uint32_t newest_seen = 0;
    int have_last = 0;

    for ( uint32_t tick = 1; tick <= states + 60 * 30; tick++ )
    {
        if ( tick <= states )
        {
            uint8_t bytes[4];

            if ( tick % ordered_every == 0 )
            {
                rs_state_bytes( rs_ordered_flag | ++position, bytes );
                rs_functional_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes, 4 ) != 0, "send ordered" );
                ordered_sent++;
            }

            rs_state_bytes( ++position, bytes );
            rs_functional_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, bytes, 4 ) != 0, "send state" );
        }

        if ( !rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) )
        {
            rs_functional_check( false, "connection lost" );
        }

        rocketnet_channel_delivery_t d = {};
        while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            rs_functional_check( d.bytes == 4, "payload length" );

            const uint32_t value = rs_state_read( d.data );
            const uint32_t at = value & ~rs_ordered_flag;
            rs_functional_check( at > last_position, "a delivery went backwards or repeated" );
            last_position = at;

            if ( value & rs_ordered_flag )
            {
                ordered_seen++;
            }
            else
            {
                states_seen++;
                newest_seen = at;
            }
        }

        have_last = tick > states && ordered_seen == ordered_sent && newest_seen == position;
        if ( have_last )
            break;
    }

    rocketnet_channel_harness_read_counters( &h );

    const uint32_t stale = rs_server_stale( &h );

    printf( "test_reliable_sequenced_long_run_across_epochs: sent=%u ordered=%u/%u states=%u newest=%u/%u stale=%u resends=%llu elapsed=%.3fs seed=0x%016llx\n",
            position, ordered_seen, ordered_sent, states_seen, newest_seen, position, stale,
            (unsigned long long) h.resends, h.now - 1000.0, (unsigned long long) h.seed );
    rs_functional_check( position > 1024 + 64, "the run is shorter than the receive window" );
    rs_functional_check( ordered_seen == ordered_sent, "an ordered record never arrived" );
    rs_functional_check( newest_seen == position, "the newest state never arrived" );
    rs_functional_check( have_last, "the run did not complete" );
    rs_functional_check( stale > 0, "no state arrived stale for its epoch" );
    rs_functional_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rs_functional_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "allocation after create" );
    rocketnet_channel_harness_destroy( &h );
}

// RNChannelTypes!Drain: a code-4 state that arrives ahead of its ordered epoch
// is held, and it is delivered when the ordered record that opens the epoch
// arrives. Without the drain the newest state is lost for good. The scripted
// order holds the client's payload packets and hands the state's packet to
// the server before the ordered record's.
void reliable_sequenced_held_record_released()
{
    rocketnet_channel_harness_t h;
    rs_functional_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );
    rs_functional_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );

    for ( int i = 0; i < 30; i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
    }

    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS, ROCKETNET_CHANNEL_WIRE_PASS );

    uint8_t bytes[4];
    rs_state_bytes( rs_ordered_flag | 1, bytes );
    rs_functional_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes, 4 ) != 0, "send ordered" );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

    const int ordered_end = h.held_count;

    rs_state_bytes( 2, bytes );
    rs_functional_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, bytes, 4 ) != 0, "send state" );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

    const int state_end = h.held_count;
    rs_functional_check( ordered_end >= 1 && state_end > ordered_end, "the scripted packets were not held" );

    int order[ROCKETNET_CHANNEL_HARNESS_MAX_HELD];
    int count = 0;

    for ( int i = ordered_end; i < state_end; i++ )
    {
        order[count++] = i;
    }

    rs_functional_check( rocketnet_channel_harness_deliver_held( &h, order, count ) == count, "deliver the state" );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

    rocketnet_channel_delivery_t d = {};
    rs_functional_check( !rocketnet_channel_harness_server_collect( &h, 0, &d ), "a state ahead of its epoch was delivered" );

    count = 0;

    for ( int i = 0; i < ordered_end; i++ )
    {
        order[count++] = i;
    }

    rs_functional_check( rocketnet_channel_harness_deliver_held( &h, order, count ) == count, "deliver the ordered record" );
    rocketnet_channel_harness_clear_held( &h );
    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_PASS, ROCKETNET_CHANNEL_WIRE_PASS );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
    rs_functional_check( rocketnet_channel_harness_server_collect( &h, 0, &d ) && d.bytes == 4 &&
                             rs_state_read( d.data ) == ( rs_ordered_flag | 1 ),
                         "the ordered record did not arrive first" );
    rs_functional_check( rocketnet_channel_harness_server_collect( &h, 0, &d ) && d.bytes == 4 &&
                             rs_state_read( d.data ) == 2,
                         "the held state was not released when its epoch opened" );

    // nothing more arrives: the resends the client makes are duplicates now

    for ( int i = 0; i < 60 * 3; i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rs_functional_check( !rocketnet_channel_harness_server_collect( &h, 0, &d ), "a message arrived twice" );
    }

    printf( "test_reliable_sequenced_held_record_released: held=%d ordered_packets=%d state_packets=%d\n",
            state_end, ordered_end, state_end - ordered_end );
    rs_functional_check( h.held_overflow == 0, "held packets overflowed" );
    rs_functional_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rs_functional_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "allocation after create" );
    rocketnet_channel_harness_destroy( &h );
}
