/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Ack receipts: send unreliable state updates with
    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, record the serial
    rocketnet_client_finish_message returns, and read rocketnet_client_receipts each
    frame for ROCKETNET_RECEIPT_STATUS_ACKED or ROCKETNET_RECEIPT_STATUS_LOSS so the sender
    knows which snapshot the receiver actually got. Run: ./bin/example_ack_receipts.
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

// This example uses the send names the header at this base declares (start,
// finish, receive, release, send_packets); a later lifecycle rename makes
// them *_start_message and friends.

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

#define NUM_UPDATES 20

int main( void )
{
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_client_config_t client_config;
    struct rocketnet_server_config_t server_config;
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint32_t serials[NUM_UPDATES];
    uint32_t outcomes[NUM_UPDATES];
    int sent = 0;
    int resolved = 0;
    int acked = 0;
    int lost = 0;
    int result = 1;

    memset( serials, 0, sizeof( serials ) );
    memset( outcomes, 0, sizeof( outcomes ) );

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, dev_backend_public_key, sizeof( server_config.backend_public_key ) );
    server_config.max_clients = 2;
    server_config.transport = "loopback";
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
    CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) token_time, NULL, connect_token ) == ROCKETNET_STATUS_OK );

    CHECK( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
    // the development network simulator: lose 30% of the client's packets
    if ( client )
        rocketnet_client_network_simulator( client, 0, 0, 30, 0 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_client_stop_and_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );

    const double start = rocketnet_time();
    while ( rocketnet_time() - start < 60.0 && resolved < NUM_UPDATES )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );

        // (1) update pumps the transport and resolves this frame's receipts
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        // (2) receive and release incoming messages, and read this frame's receipts
        const uint8_t * data = NULL;
        int bytes = 0;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
            }
        }
        const int slot = rocketnet_client_index( client ); // -1 until CONNECTED
        int sender = -1;
        const struct rocketnet_message_batch_t * arrived = NULL;
        while ( ( arrived = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < arrived->num_messages; i++ )
            {
                data = arrived->data[i];
                bytes = arrived->bytes[i];
                sender = arrived->client_index[i];
            }
        }

        (void) data; (void) bytes; (void) sender;
        struct rocketnet_receipt_t receipts[32];
        const int count = rocketnet_client_receipts( client, receipts, 32 );
        CHECK( count <= 32 );
        for ( int r = 0; r < count && r < 32; r++ )
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
                serials[sent] = rocketnet_client_finish_message( client, write_u32( out, snapshot_id ) );
                CHECK( serials[sent] != 0 );
                sent++;
            }
            if ( slot >= 0 && rocketnet_server_client_connected( server, slot ) )
            {
                uint8_t * tick = rocketnet_server_start_message( server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1 );
                if ( tick )
                {
                    tick[0] = 0;
                    CHECK( rocketnet_server_finish_message( server, 1 ) != 0 );
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
    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
