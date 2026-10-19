/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNChannelTypes!Dispatch and !Drain over the deterministic simulated link.
#include "rocketnet_config.h"
#include "channel_harness.h"
#include "rocketnet_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
        bytes[i] = (uint8_t) ( state >> ( i * 8 ) );
}

static uint32_t sequenced_state_read( const uint8_t * bytes )
{
    uint32_t state = 0;
    for ( int i = 0; i < 4; i++ )
        state |= (uint32_t) bytes[i] << ( i * 8 );
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
            const int type = tick == 60 ? ROCKETNET_RELIABLE_ORDERED : ROCKETNET_UNRELIABLE_SEQUENCED;
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
    sequenced_functional_check( rocketnet_channel_allocator_calls == h.allocation_snapshot, "allocation after create" );
    printf( "state_stream_never_goes_backwards_over_lossy_link: accepted=%u delivered=%u last=%u resends=%llu wire_to_server=%lld wire_to_client=%lld seed=0x5EED5EED5EED5EED\n",
            accepted, delivered, last, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    rocketnet_channel_harness_destroy( &h );
}

void large_unreliable_sequenced_uses_transport_fragments()
{
    // Use the production socket transports: the channel harness intentionally
    // models only unfragmented frames. One code-1 RECORDS payload must travel
    // through type-9 transport fragments and reassemble before channel dispatch.
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config_current( &server_config );
    server_config.max_clients = 2;
    rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "127.0.0.1", &server_config, NULL, NULL, 0, 0 );
    sequenced_functional_check( server != NULL, "large server create" );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", rocketnet_server_transport_port( rocketnet_message_server_transport( server ) ) );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    sequenced_functional_check( rocketnet_write_dev_connect_token( address, 0x51E9ULL, (uint64_t) time( NULL ), token ) != 0, "large token" );
    rocketnet_message_client_t * client = rocketnet_message_client_create( NULL, token, NULL, NULL, NULL, 0, 0 );
    sequenced_functional_check( client != NULL, "large client create" );

    const double wall_start = rocketnet_platform_time();
    double now = 1000.0;
    for ( ; rocketnet_platform_time() - wall_start < 5.0; )
    {
        rocketnet_platform_sleep( 0.001 );
        now = 1000.0 + rocketnet_platform_time() - wall_start;
        sequenced_functional_check( rocketnet_message_client_update( client, now ) == ROCKETNET_MESSAGE_OK, "large client update" );
        sequenced_functional_check( rocketnet_message_server_update( server, now ) == ROCKETNET_MESSAGE_OK, "large server update" );
        rocketnet_message_client_send_packets( client );
        rocketnet_message_server_send_packets( server );
        if ( rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_message_server_client_connected( server, 0 ) )
            break;
    }
    sequenced_functional_check( rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                                rocketnet_message_server_client_connected( server, 0 ), "large connect" );

    const int message_bytes = 2000;
    uint8_t * write = rocketnet_message_client_start( client, 0, ROCKETNET_UNRELIABLE_SEQUENCED, message_bytes );
    sequenced_functional_check( write != NULL, "large client start" );
    for ( int i = 0; i < message_bytes; i++ )
        write[i] = (uint8_t) ( i * 37 + 11 );
    sequenced_functional_check( rocketnet_message_client_finish( client, message_bytes ) != 0, "large client finish" );
    sequenced_functional_check( rocketnet_message_client_start( client, 0, ROCKETNET_UNRELIABLE_SEQUENCED,
                                                                 ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES ) == NULL,
                                "configured transport maximum refused before enqueue" );
    sequenced_functional_check( rocketnet_message_client_start( client, 0, ROCKETNET_UNRELIABLE_SEQUENCED, 65536 ) == NULL,
                                "u16 record maximum refused before enqueue" );

    int received = 0;
    const double send_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - send_start < 5.0 && !received )
    {
        rocketnet_message_client_send_packets( client );
        rocketnet_message_server_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
        now = 1000.0 + rocketnet_platform_time() - wall_start;
        sequenced_functional_check( rocketnet_message_client_update( client, now ) == ROCKETNET_MESSAGE_OK, "large client progress" );
        sequenced_functional_check( rocketnet_message_server_update( server, now ) == ROCKETNET_MESSAGE_OK, "large server progress" );
        const uint8_t * data;
        int bytes;
        while ( rocketnet_message_server_receive( server, 0, 0, &data, &bytes ) )
        {
            sequenced_functional_check( bytes == message_bytes, "large delivered length" );
            for ( int i = 0; i < bytes; i++ )
                sequenced_functional_check( data[i] == (uint8_t) ( i * 37 + 11 ), "large delivered bytes" );
            rocketnet_message_server_release( server, 0, data );
            received++;
        }
    }
    sequenced_functional_check( received == 1, "large fragment reassembly" );
    const rocketnet_counters_t * counters = rocketnet_server_transport_counters( rocketnet_message_server_transport( server ) );
    sequenced_functional_check( counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] >= 1, "large transport fragments" );
    printf( "large_unreliable_sequenced_uses_transport_fragments: bytes=%d delivered=%d reassembled=%llu\n",
            message_bytes, received, (unsigned long long) counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] );

    rocketnet_message_client_destroy( client );
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( server );
    rocketnet_server_transport_stop( transport );
    const double stop_start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( transport ) != ROCKETNET_SERVER_TRANSPORT_STOPPED &&
            rocketnet_platform_time() - stop_start < 5.0 )
    {
        rocketnet_platform_sleep( 0.001 );
        rocketnet_message_server_update( server, 1000.0 + rocketnet_platform_time() - wall_start );
    }
    sequenced_functional_check( rocketnet_server_transport_state( transport ) == ROCKETNET_SERVER_TRANSPORT_STOPPED, "large server stop" );
    rocketnet_message_server_destroy( server );
}
