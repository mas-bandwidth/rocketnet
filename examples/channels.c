/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Channels: a reliable-ordered chat channel (0) and an unreliable-sequenced
    position channel (1) over loopback, with the development network simulator
    losing 35% of the client's packets. A lost chat message holds the later
    chat messages on channel 0 until it is resent, while newer positions on
    channel 1 keep arriving every frame. Run: ./bin/example_channels
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHAT_CHANNEL        0
#define POSITION_CHANNEL    1
#define NUM_CHAT            8

int main( void )
{
    rocketnet_init( NULL, NULL, NULL );

    // a short resend delay, so the example shows a resend within a few frames
    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";
    server_config.message_resend_delay_initial_ms = 120;
    server_config.message_resend_delay_min_ms = 120;

    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    if ( !server || rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start\n" );
        return 1;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) );
    rocketnet_server_public_key( server, server_public_key );
    rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.message_resend_delay_initial_ms = 120;
    client_config.message_resend_delay_min_ms = 120;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    rocketnet_client_network_simulator( client, 0, 0, 35, 0 );
    rocketnet_client_connect( client, connect_token );

    uint8_t chat_sent = 0, chat_received = 0;
    uint32_t position_sent = 0, last_position = 0;
    int positions_received = 0, stall_shown = 0, in_order = 1;
    const double start = rocketnet_time();

    while ( chat_received < NUM_CHAT && rocketnet_time() - start < 10.0 )
    {
        const double elapsed = rocketnet_time() - start;
        rocketnet_server_update( server, 1000.0 + elapsed );
        rocketnet_client_update( client, 1000.0 + elapsed );

        // the server's one queue names each message's channel; order holds within a channel
        int chat_this_frame = 0, positions_this_frame = 0;
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
        }
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                if ( batch->channel[i] == CHAT_CHANNEL )
                {
                    in_order &= batch->data[i][0] == chat_received + 1;
                    chat_received = batch->data[i][0];
                    chat_this_frame++;
                }
                else
                {
                    uint32_t position;
                    memcpy( &position, batch->data[i], sizeof( position ) );
                    in_order &= position > last_position;
                    last_position = position;
                    positions_received++;
                    positions_this_frame++;
                }
            }
        }

        if ( chat_this_frame > 0 )
            printf( "t=%.3fs positions=%d chat=%d/%d (chat delivered in order)\n", elapsed, positions_received, chat_received, chat_sent );
        else if ( chat_sent > chat_received && positions_this_frame > 0 && !stall_shown++ )
            printf( "t=%.3fs positions=%d chat=%d/%d (chat held while positions arrive)\n", elapsed, positions_received, chat_received, chat_sent );

        // a position every frame, and a chat message every frame until all are sent
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            position_sent++;
            uint8_t * p = rocketnet_client_start_message( client, POSITION_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, sizeof( position_sent ) );
            memcpy( p, &position_sent, sizeof( position_sent ) );
            rocketnet_client_finish_message( client, sizeof( position_sent ) );
            if ( chat_sent < NUM_CHAT )
            {
                p = rocketnet_client_start_message( client, CHAT_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
                p[0] = ++chat_sent;
                rocketnet_client_finish_message( client, 1 );
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.005 );
    }

    const int result = chat_received == NUM_CHAT && in_order ? 0 : 1;
    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    printf( result == 0 ? "PASS\n" : "FAIL: every chat message arrives, each channel in order\n" );
    return result;
}
