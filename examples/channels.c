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
    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the in-memory loopback transport keeps its endpoints in a hub the caller
    // owns, sized here for one server and one client; it is destroyed after them

    enum rocketnet_status_t hub_status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 1, &hub_status );
    if ( !hub || hub_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: loopback hub create: %s\n", rocketnet_status_name( hub_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    // a short resend delay, so the example shows a resend within a few frames

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &server_config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;
    server_config.message_resend_delay_initial_ms = 120;
    server_config.message_resend_delay_min_ms = 120;

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
    {
        server_status = rocketnet_server_start( server );
    }

    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( server_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( !rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    rocketnet_server_public_key( server, server_public_key );

    const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );
    if ( token_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect token: %s\n", rocketnet_status_name( token_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    struct rocketnet_client_config_t client_config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &client_config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    client_config.transport = "loopback";
    client_config.loopback_hub = hub;
    client_config.message_resend_delay_initial_ms = 120;
    client_config.message_resend_delay_min_ms = 120;

    enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &client_status );
    if ( client )
    {
        rocketnet_client_network_simulator( client, 0, 0, 35, 0 );
    }

    if ( client && client_status == ROCKETNET_STATUS_OK )
    {
        client_status = rocketnet_client_connect( client, connect_token );
    }

    if ( !client || client_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create/connect: %s\n", rocketnet_status_name( client_status ) );
        rn_cli_client_stop_and_destroy( client, 1000.0 );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

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
        {
            printf( "t=%.3fs positions=%d chat=%d/%d (chat delivered in order)\n", elapsed, positions_received, chat_received, chat_sent );
        }
        else if ( chat_sent > chat_received && positions_this_frame > 0 && !stall_shown++ )
        {
            printf( "t=%.3fs positions=%d chat=%d/%d (chat held while positions arrive)\n", elapsed, positions_received, chat_received, chat_sent );
        }

        // a position every frame, and a chat message every frame until all are sent

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            position_sent++;

            // replaceable position: skip this frame when there is no room

            if ( rocketnet_client_can_send_message( client, POSITION_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, (int) sizeof( position_sent ) ) )
            {
                uint8_t * p = rocketnet_client_start_message( client, POSITION_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, (int) sizeof( position_sent ) );
                if ( p )
                {
                    memcpy( p, &position_sent, sizeof( position_sent ) );

                    const uint32_t serial = rocketnet_client_finish_message( client, (int) sizeof( position_sent ) );
                    (void) serial;
                }
            }

            if ( chat_sent < NUM_CHAT )
            {
                // reliable chat: defer to a later frame when there is no room

                if ( rocketnet_client_can_send_message( client, CHAT_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 ) )
                {
                    uint8_t * p = rocketnet_client_start_message( client, CHAT_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
                    if ( p )
                    {
                        p[0] = (uint8_t) ( chat_sent + 1 );

                        if ( rocketnet_client_finish_message( client, 1 ) )
                        {
                            chat_sent++;
                        }
                    }
                }
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.005 );
    }

    const int result = chat_received == NUM_CHAT && in_order ? 0 : 1;

    rn_cli_client_stop_and_destroy( client, 1000.0 + rocketnet_time() - start );
    rn_cli_server_stop_and_destroy( server );

    rn_cli_loopback_hub_destroy( hub );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( result == 0 ? "PASS\n" : "FAIL: every chat message arrives, each channel in order\n" );
    return result;
}
