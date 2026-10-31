/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNChannelTypes!Dispatch and !Drain over the deterministic simulated link.
#include "channel_harness.h"

#include <stdio.h>
#include <stdlib.h>

static void sequenced_functional_check( bool condition, const char * label )
{
    if ( condition )
        return;
    printf( "state stream failed: %s\n", label );
    fflush( stdout );
    exit( 1 );
}

static void sequenced_state_bytes( uint32_t state, uint8_t bytes[4] )
{
    for ( int i = 0; i < 4; i++ )
    {
        bytes[i] = (uint8_t) ( state >> ( i * 8 ) );
    }
}

static uint32_t sequenced_state_read( const uint8_t * bytes )
{
    uint32_t state = 0;

    for ( int i = 0; i < 4; i++ )
    {
        state |= (uint32_t) bytes[i] << ( i * 8 );
    }

    return state;
}

void state_stream_never_goes_backwards_over_lossy_link()
{
    rocketnet_channel_harness_t h;
    sequenced_functional_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    sequenced_functional_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.025, 10.0f, 5.0f );

    uint32_t last = 0;
    uint32_t delivered = 0;
    uint32_t accepted = 0;

    for ( uint32_t tick = 1; tick <= 120 + 600; tick++ )
    {
        if ( tick <= 120 )
        {
            uint8_t bytes[4];
            sequenced_state_bytes( tick, bytes );

            const int type = tick == 60 ? ROCKETNET_RELIABILITY_RELIABLE_ORDERED : ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED;
            sequenced_functional_check( rocketnet_channel_harness_client_send( &h, 0, type, bytes, 4 ) != 0, "send" );
            accepted++;
        }

        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

        rocketnet_channel_delivery_t d;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            sequenced_functional_check( d.bytes == 4, "payload length" );

            const uint32_t state = sequenced_state_read( d.data );
            sequenced_functional_check( state > last, "state moved backwards or duplicated" );
            last = state;
            delivered++;
        }
    }

    rocketnet_channel_harness_read_counters( &h );
    sequenced_functional_check( delivered >= 20, "too few deliveries" );
    sequenced_functional_check( last >= 100, "latest state did not arrive" );
    sequenced_functional_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    sequenced_functional_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "allocation after create" );
    printf( "test_state_stream_never_goes_backwards_over_lossy_link: accepted=%u delivered=%u last=%u resends=%llu wire_to_server=%lld wire_to_client=%lld seed=0x5EED5EED5EED5EED\n",
            accepted, delivered, last, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    rocketnet_channel_harness_destroy( &h );
}

void large_unreliable_sequenced_promotes_to_reliable()
{
    // Run the message objects over the channel harness: the in-memory
    // simulator and the test clock (1000.0), no socket and no sleep. An
    // UNRELIABLE_SEQUENCED send too big for one packet's record is promoted
    // to RELIABLE_SEQUENCED (the promotion rule), and is
    // counted.

    rocketnet_channel_harness_t h;
    sequenced_functional_check( rocketnet_channel_harness_create( &h ) != 0, "large harness create" );
    sequenced_functional_check( rocketnet_channel_harness_connect( &h, 60 * 5 ) != 0, "large connect" );

    rocketnet_message_client_t * client = h.message_client;

    const int message_bytes = 2000;
    uint8_t * write = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, message_bytes );
    sequenced_functional_check( write != NULL, "large client start" );
    for ( int i = 0; i < message_bytes; i++ )
    {
        write[i] = (uint8_t) ( i * 37 + 11 );
    }

    sequenced_functional_check( rocketnet_message_client_finish( client, message_bytes ) != 0, "large client finish" );
    uint8_t * promoted = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED,
                                                         2 * ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    sequenced_functional_check( promoted != NULL, "oversize unreliable start promotes" );
    if ( promoted )
    {
        rocketnet_message_client_abort( client );
    }

    rocketnet_message_config_t default_config;
    rocketnet_message_default_config( &default_config );
    sequenced_functional_check( rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED,
                                                                (int) default_config.max_message_bytes + 1 ) == NULL,
                                "configured message maximum refused before enqueue" );

    // Five seconds of the test clock at 60 updates a second

    int received = 0;
    for ( int update = 0; update < 60 * 5 && !received; update++ )
    {
        sequenced_functional_check( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) != 0, "large progress" );

        rocketnet_channel_delivery_t d;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            sequenced_functional_check( d.bytes == message_bytes, "large delivered length" );
            for ( int i = 0; i < d.bytes; i++ )
            {
                sequenced_functional_check( d.data[i] == (uint8_t) ( i * 37 + 11 ), "large delivered bytes" );
            }

            received++;
        }
    }

    sequenced_functional_check( received == 1, "large promoted delivery" );
    sequenced_functional_check( rocketnet_channel_harness_overflows( &h ) == 0, "large delivery ring overflow" );

    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );
    sequenced_functional_check( counters->values[ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED] == 1, "large unreliable promotion counted" );
    printf( "test_large_unreliable_sequenced_promotes_to_reliable: bytes=%d delivered=%d promoted=%llu\n",
            message_bytes, received, (unsigned long long) counters->values[ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED] );

    rocketnet_channel_harness_destroy( &h );
}
