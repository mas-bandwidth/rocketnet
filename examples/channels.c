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

#include "shutdown.h"
#include "rocketnet.h"
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

// Uses the public API names as declared in sdk/rocketnet.h: client/server
// start_message, finish_message, receive_messages (one batch, no release
// call), send_packets and client_connected.
#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// serialize a uint32_t little-endian straight into a started message
static int write_u32( uint8_t * p, uint32_t value )
{
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) ( value >> 8 );
    p[2] = (uint8_t) ( value >> 16 );
    p[3] = (uint8_t) ( value >> 24 );
    return 4;
}

static uint32_t read_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

#define CHAT_CHANNEL     0
#define POSITION_CHANNEL 1
#define NUM_CHAT         8

int main( void )
{
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    int result = 1;

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, dev_backend_public_key, sizeof( server_config.backend_public_key ) );
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    server_config.message_resend_delay_initial_ms = 120;
    server_config.message_resend_delay_min_ms = 120;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    CHECK( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );
    rocketnet_server_public_key( server, server_public_key );
    const time_t token_time = time( NULL );
    CHECK( token_time != (time_t) -1 );
    CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1ull, EXAMPLE_PROTOCOL_ID, (uint64_t) token_time, NULL, connect_token ) == ROCKETNET_STATUS_OK );

    CHECK( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.message_resend_delay_initial_ms = 120;
    client_config.message_resend_delay_min_ms = 120;
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
    // the development network simulator: lose 35% of the client's packets
    if ( client )
        rocketnet_client_network_simulator( client, 0, 0, 35, 0 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_client_stop_and_destroy( client );
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
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        // (2) receive and release every received message on both channels
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            // the server sends this client nothing, so there is nothing to read
        }

        uint32_t chat_this_frame = 0, pos_this_frame = 0;
        if ( rocketnet_server_client_connected( server, 0 ) )
        {
            // the server's one queue, in batches: each entry names its sender
            // and the channel it was delivered on, and order holds within a
            // channel; the next batch call or update releases a batch
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            {
                for ( int i = 0; i < batch->num_messages; i++ )
                {
                    CHECK( batch->client_index[i] == 0 );
                    if ( batch->channel[i] == CHAT_CHANNEL )
                    {
                        uint32_t seq = 0;
                        CHECK( batch->bytes[i] == (int) sizeof( seq ) );
                        seq = read_u32( batch->data[i] );
                        CHECK( seq == chat_received + 1 );
                        chat_received = seq;
                        chat_this_frame++;
                    }
                    else
                    {
                        CHECK( batch->channel[i] == POSITION_CHANNEL );
                        uint32_t pos = 0;
                        CHECK( batch->bytes[i] == (int) sizeof( pos ) );
                        pos = read_u32( batch->data[i] );
                        CHECK( pos > last_pos );
                        last_pos = pos;
                        positions_received++;
                        pos_this_frame++;
                    }
                }
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
            CHECK( rocketnet_client_finish_message( client, write_u32( buf, pos_sent ) ) != 0 );
            if ( chat_sent < NUM_CHAT )
            {
                chat_sent++;
                buf = rocketnet_client_start_message( client, CHAT_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, sizeof( chat_sent ) );
                CHECK( buf != NULL );
                CHECK( rocketnet_client_finish_message( client, write_u32( buf, chat_sent ) ) != 0 );
            }
        }

        // (5) send_packets is last so this frame's messages leave this frame
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        if ( chat_received == NUM_CHAT ) { result = 0; break; }
        rocketnet_sleep( 0.005 );
    }

cleanup:
    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
