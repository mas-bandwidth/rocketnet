/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Ack receipts: send unreliable state updates with
    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, record the serial
    rocketnet_client_finish_message returns, and read rocketnet_client_receipts each
    frame for ROCKETNET_RECEIPT_STATUS_ACKED or ROCKETNET_RECEIPT_STATUS_LOSS so the sender
    knows which snapshot the receiver actually got. Run: ./bin/example_ack_receipts.
*/

#include "rocketnet.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// This example uses the send names the header at this base declares (start,
// finish, receive, release, send_packets); a later lifecycle rename makes
// them *_start_message and friends.

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

#define NUM_UPDATES 20

int main( void )
{
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_client_config_t client_config;
    struct rocketnet_server_config_t server_config;
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char server_address[64];
    uint32_t serials[NUM_UPDATES];
    uint32_t outcomes[NUM_UPDATES];
    int sent = 0;
    int resolved = 0;
    int acked = 0;
    int lost = 0;
    int result = 1;

    memset( serials, 0, sizeof( serials ) );
    memset( outcomes, 0, sizeof( outcomes ) );

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

    rocketnet_server_dev_config( &server_config );
    server_config.max_clients = 2;
    server_config.transport = "loopback";
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
    CHECK( rocketnet_write_dev_connect_token( server_address, 1, 4102444800ull, connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    CHECK( (rocketnet_client_default_config)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.simulated_loss_percent = 30;
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
    while ( rocketnet_time() - start < 60.0 && resolved < NUM_UPDATES )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );

        // (1) update pumps the transport and resolves this frame's receipts
        CHECK( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );
        CHECK( rocketnet_client_update( client, now ) == ROCKETNET_UPDATE_OK );

        // (2) receive and release incoming messages, and read this frame's receipts
        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_client_receive_message( client, 0, &data, &bytes ) )
            rocketnet_client_release_message( client, data );
        const int slot = rocketnet_client_index( client );
        while ( rocketnet_server_receive_message( server, slot, 0, &data, &bytes ) )
            rocketnet_server_release_message( server, slot, data );

        struct rocketnet_receipt_t receipts[32];
        const int count = rocketnet_client_receipts( client, receipts, 32 );
        for ( int r = 0; r < count; r++ )
        {
            int idx = -1;
            for ( int i = 0; i < sent; i++ )
            {
                if ( serials[i] == receipts[r].serial )
                {
                    idx = i;
                    break;
                }
            }
            CHECK( idx >= 0 && outcomes[idx] == 0 );
            CHECK( receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED || receipts[r].status == ROCKETNET_RECEIPT_STATUS_LOSS );
            outcomes[idx] = receipts[r].status;
            resolved++;
            if ( receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED )
                acked++;
            else
                lost++;
            printf( "update %d (serial %u): %s\n", idx, receipts[r].serial,
                    receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED ? "ACKED" : "LOSS" );
        }

        // (3) run the game simulation for the frame

        // (4) queue this frame's outgoing state update and return traffic
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( sent < NUM_UPDATES )
            {
                uint32_t snapshot_id = (uint32_t) sent;
                uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, (int) sizeof( snapshot_id ) );
                CHECK( out != NULL );
                memcpy( out, &snapshot_id, sizeof( snapshot_id ) );
                serials[sent] = rocketnet_client_finish_message( client, (int) sizeof( snapshot_id ) );
                CHECK( serials[sent] != 0 );
                sent++;
            }
            if ( rocketnet_server_client_connected( server, slot ) )
            {
                uint8_t * tick = rocketnet_server_start_message( server, slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1 );
                if ( tick )
                {
                    tick[0] = 0;
                    CHECK( rocketnet_server_finish_message( server, slot, 1 ) != 0 );
                }
            }
        }

        // (5) send_packets is last so this frame's queued messages leave this frame
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.005 );
    }

    printf( "receipts resolved: %d/%d (acked %d, loss %d)\n", resolved, NUM_UPDATES, acked, lost );
    CHECK( sent == NUM_UPDATES && resolved == NUM_UPDATES );
    CHECK( acked > 0 && lost > 0 );
    result = 0;

cleanup:
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_term();
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
