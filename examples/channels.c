/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Channels: a reliable-ordered chat channel (channel 0) and an
    unreliable-sequenced position channel (channel 1) over loopback with the
    development link simulator dropping outgoing client packets. A lost chat
    message holds later chat messages on channel 0 until it is resent, while
    newer position updates on channel 1 keep arriving every frame.

    Run it from the repository root: ./bin/example_channels
*/

#include "rocketnet.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Uses the public API names as declared in sdk/rocketnet.h: client/server
// start_message, finish_message, receive_message, release_message,
// send_packets and client_connected.
#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )
#define CHAT_CHANNEL     0
#define POSITION_CHANNEL 1
#define NUM_CHAT         8

int main( void )
{
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char server_address[64];
    int result = 1;

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    CHECK( (rocketnet_server_dev_config)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    server_config.rto_initial_ms = 120;
    server_config.rto_min_ms = 120;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    CHECK( rocketnet_write_dev_connect_token( server_address, 1ull, 4102444800ull, connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    CHECK( (rocketnet_client_default_config)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.simulated_loss_percent = 35;
    client_config.rto_initial_ms = 120;
    client_config.rto_min_ms = 120;
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, NULL, 0, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );

    const double start = rocketnet_time();
    uint32_t chat_sent = 0, chat_received = 0, positions_received = 0, last_pos = 0;
    uint32_t pos_sent = 0, stall_positions = 0, positions_during_stall = 0;

    while ( rocketnet_time() - start < 10.0 )
    {
        const double elapsed = rocketnet_time() - start;
        const double now = 1000.0 + elapsed;

        // (1) update pumps the transport and takes in what arrived
        CHECK( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );
        CHECK( rocketnet_client_update( client, now ) == ROCKETNET_UPDATE_OK );

        // (2) receive and release every received message on both channels
        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_client_receive_message( client, CHAT_CHANNEL, &data, &bytes ) )
            rocketnet_client_release_message( client, data );

        uint32_t chat_this_frame = 0, pos_this_frame = 0;
        if ( rocketnet_server_client_connected( server, 0 ) )
        {
            while ( rocketnet_server_receive_message( server, 0, CHAT_CHANNEL, &data, &bytes ) )
            {
                uint32_t seq = 0;
                CHECK( bytes == (int) sizeof( seq ) );
                memcpy( &seq, data, sizeof( seq ) );
                rocketnet_server_release_message( server, 0, data );
                CHECK( seq == chat_received + 1 );
                chat_received = seq;
                chat_this_frame++;
            }
            while ( rocketnet_server_receive_message( server, 0, POSITION_CHANNEL, &data, &bytes ) )
            {
                uint32_t pos = 0;
                CHECK( bytes == (int) sizeof( pos ) );
                memcpy( &pos, data, sizeof( pos ) );
                rocketnet_server_release_message( server, 0, data );
                CHECK( pos > last_pos );
                last_pos = pos;
                positions_received++;
                pos_this_frame++;
            }
        }

        // (3) run the game simulation for the frame: record timeline and stall progress
        if ( chat_sent > chat_received && chat_this_frame == 0 && pos_this_frame > 0 )
        {
            stall_positions += pos_this_frame;
            if ( stall_positions >= 2 && positions_during_stall == 0 ) {
                positions_during_stall = stall_positions;
                printf( "t=%.3fs positions=%u chat=%u/%u (chat held while positions arrive)\n", elapsed, positions_received, chat_received, chat_sent );
            }
        }
        if ( chat_this_frame > 0 ) {
            stall_positions = 0;
            printf( "t=%.3fs positions=%u chat=%u/%u (chat delivered in order)\n", elapsed, positions_received, chat_received, chat_sent );
        }

        // (4) queue this frame's outgoing messages once connected
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            pos_sent++;
            uint8_t * buf = rocketnet_client_start_message( client, POSITION_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, sizeof( pos_sent ) );
            CHECK( buf != NULL );
            memcpy( buf, &pos_sent, sizeof( pos_sent ) );
            CHECK( rocketnet_client_finish_message( client, sizeof( pos_sent ) ) != 0 );
            if ( chat_sent < NUM_CHAT )
            {
                chat_sent++;
                buf = rocketnet_client_start_message( client, CHAT_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, sizeof( chat_sent ) );
                CHECK( buf != NULL );
                memcpy( buf, &chat_sent, sizeof( chat_sent ) );
                CHECK( rocketnet_client_finish_message( client, sizeof( chat_sent ) ) != 0 );
            }
        }

        // (5) send_packets is last so this frame's messages leave this frame
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        if ( chat_received == NUM_CHAT ) { result = 0; break; }
        rocketnet_sleep( 0.005 );
    }

cleanup:
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
