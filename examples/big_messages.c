/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Big messages: a 200 KB reliable message and an oversize unreliable send.
    A server and a client in one process over the in-memory loopback transport,
    so it needs no socket and no network. The client sends a 200 KB reliable
    message once; the library fragments it and the server reassembles it, and
    the server checks every byte. The client then sends an unreliable message
    larger than one packet; the library promotes it to its reliable path, and
    the promotion shows as one acknowledged receipt in the public counters.

    Run it from the repository root: ./bin/example_big_messages
*/

#include "rocketnet.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

#define BIG_BYTES           ( 200 * 1024 )
#define OVERSIZE_BYTES      4096
#define LIMIT_BYTES         ( 256 * 1024 )
#define DEADLINE_SECONDS    20.0

static uint8_t expected[BIG_BYTES];

int main( void )
{
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_client_config_t client_config;
    struct rocketnet_server_config_t server_config;
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char server_address[64];
    int result = 1, big_sent = 0, big_arrived = 0, oversize_sent = 0;
    double big_sent_at = 0.0;
    uint64_t promotion_before = 0, promotion_after = 0;
    for ( int i = 0; i < BIG_BYTES; i++ )
        expected[i] = (uint8_t) ( i * 31 + 13 );
    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_server_dev_config( &server_config );
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    // the defaults (262144) already hold 200 KB; set both ends' limits here so
    // the example states the size it needs rather than relying on that default
    server_config.max_message_bytes = LIMIT_BYTES;
    server_config.receive_ring_bytes = LIMIT_BYTES;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.max_message_bytes = LIMIT_BYTES;
    client_config.receive_ring_bytes = LIMIT_BYTES;
    // the token names the server and carries the keys; this development token
    // expires thirty seconds after its timestamp, so it uses one far ahead
    CHECK( rocketnet_write_dev_connect_token( server_address, 1, 4102444800ull, connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );
    const double start = rocketnet_time();
    const int max_clients = rocketnet_server_max_clients( server );
    while ( rocketnet_time() - start < DEADLINE_SECONDS )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );
        // (1) update pumps the transport and takes in what arrived
        if ( rocketnet_server_update( server, now ) != ROCKETNET_UPDATE_OK )
            return -1;
        if ( rocketnet_client_update( client, now ) != ROCKETNET_UPDATE_OK )
            return -1;
        // (2) receive and release every received message, checking each byte
        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_client_receive_message( client, &data, &bytes ) )
            rocketnet_client_release_message( client, data );
        uint64_t sender = 0;
        while ( rocketnet_server_receive_message( server, &sender, &data, &bytes ) )
        {
            if ( bytes == BIG_BYTES )
            {
                CHECK( memcmp( data, expected, BIG_BYTES ) == 0 );
                big_arrived = 1;
                printf( "server received the %d-byte reliable message intact after %.3f s\n",
                        bytes, rocketnet_time() - start - big_sent_at );
            }
            rocketnet_server_release_message( server, data );
        }
        // (3) run the game simulation for the frame: nothing to simulate here
        // (4) queue this frame's outgoing messages, once the client is connected
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( !big_sent )
            {
                uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE, BIG_BYTES );
                CHECK( out != NULL );
                memcpy( out, expected, BIG_BYTES );
                CHECK( rocketnet_client_finish_message( client, BIG_BYTES ) != 0 );
                big_sent = 1;
                big_sent_at = rocketnet_time() - start;
            }
            if ( big_arrived && !oversize_sent )
            {
                promotion_before = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED );
                printf( "promotion counter before: %llu\n", (unsigned long long) promotion_before );
                // 4096 bytes is above one packet's record limit. A start that
                // returns a buffer here is the promotion: an oversize
                // unreliable send without a receipt is refused instead.
                uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, OVERSIZE_BYTES );
                CHECK( out != NULL );
                memset( out, 0x5a, OVERSIZE_BYTES );
                CHECK( rocketnet_client_finish_message( client, OVERSIZE_BYTES ) != 0 );
                oversize_sent = 1;
            }
            if ( oversize_sent )
                promotion_after = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED );
        }
        // (5) send_packets is last,
        // after this frame's messages are queued, so this frame's messages
        // leave this frame
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        if ( big_arrived && oversize_sent && promotion_after == promotion_before + 1 )
        {
            rocketnet_client_disconnect( client );
            printf( "promotion counter after: %llu\n", (unsigned long long) promotion_after );
            result = 0;
            break;
        }
        rocketnet_sleep( 0.001 );
    }
    if ( result != 0 )
        printf( "FAIL: the messages did not arrive before the deadline\n" );
cleanup:
    rocketnet_client_disconnect( client );
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
