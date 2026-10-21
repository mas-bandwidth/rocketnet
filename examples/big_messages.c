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
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// serialize the known pattern straight into a started message
static int write_pattern( uint8_t * p, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        p[i] = (uint8_t) ( i * 31 + 13 );
    return bytes;
}

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
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    int result = 1, big_sent = 0, big_arrived = 0, oversize_sent = 0;
    double big_sent_at = 0.0;
    uint64_t promotion_before = 0, promotion_after = 0;
    for ( int i = 0; i < BIG_BYTES; i++ )
        expected[i] = (uint8_t) ( i * 31 + 13 );
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, dev_backend_public_key, sizeof( server_config.backend_public_key ) );
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    // the defaults (262144) already hold 200 KB; set both ends' limits here so
    // the example states the size it needs rather than relying on that default
    server_config.max_message_bytes = LIMIT_BYTES;
    server_config.receive_message_buffer_bytes = LIMIT_BYTES;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );
    CHECK( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );
    rocketnet_server_public_key( server, server_public_key );
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.max_message_bytes = LIMIT_BYTES;
    client_config.receive_message_buffer_bytes = LIMIT_BYTES;
    // the token names the server and carries the keys; this development token
    // expires thirty seconds after its creation timestamp, using the token clock
    const time_t token_time = time( NULL );
    CHECK( token_time != (time_t) -1 );
    CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) token_time, NULL, connect_token ) == ROCKETNET_STATUS_OK );
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );
    const double start = rocketnet_time();
    while ( rocketnet_time() - start < DEADLINE_SECONDS )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );
        // (1) update pumps the transport and takes in what arrived
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        // (2) receive and release every received message, checking each byte
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            // the server sends this client nothing, so there is nothing to read
        }
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < incoming->num_messages; i++ )
            {
                const uint8_t * data = incoming->data[i];
                const int bytes = incoming->bytes[i];
                if ( bytes == BIG_BYTES )
                {
                    CHECK( memcmp( data, expected, BIG_BYTES ) == 0 );
                    big_arrived = 1;
                    printf( "server received the %d-byte reliable message intact after %.3f s\n",
                            bytes, rocketnet_time() - start - big_sent_at );
                }
            }
        }
        // (3) run the game simulation for the frame: nothing to simulate here
        // (4) queue this frame's outgoing messages, once the client is connected
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( !big_sent )
            {
                uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE, BIG_BYTES );
                CHECK( out != NULL );
                CHECK( rocketnet_client_finish_message( client, write_pattern( out, BIG_BYTES ) ) != 0 );
                big_sent = 1;
                big_sent_at = rocketnet_time() - start;
            }
            if ( big_arrived && !oversize_sent )
            {
                struct rocketnet_client_stats_t stats;
                rocketnet_client_stats( client, &stats );
                promotion_before = stats.receipts_acked;
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
            {
                struct rocketnet_client_stats_t stats;
                rocketnet_client_stats( client, &stats );
                promotion_after = stats.receipts_acked;
            }
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
