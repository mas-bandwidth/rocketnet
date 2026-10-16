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
        bytes[i] = (uint8_t) ( state >> ( i * 8 ) );
}

static uint32_t rs_state_read( const uint8_t * bytes )
{
    uint32_t state = 0;
    for ( int i = 0; i < 4; i++ )
        state |= (uint32_t) bytes[i] << ( i * 8 );
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
            const int type = tick == 60 ? ROCKETNET_RELIABLE_ORDERED : ROCKETNET_RELIABLE_SEQUENCED;
            rs_functional_check( rocketnet_channel_harness_client_send( &h, 0, type, bytes, 4 ) != 0, "send" );
            accepted++;
        }
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
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
    rs_functional_check( rocketnet_channel_allocator_calls == h.allocation_snapshot, "allocation after create" );
    printf( "reliable_state_stream_over_lossy_link: accepted=%u delivered=%u last=%u resends=%llu wire_to_server=%lld wire_to_client=%lld seed=0x5EED5EED5EED5EED\n",
            accepted, delivered, last, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    rocketnet_channel_harness_destroy( &h );
}
