/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Hello: a server and a client in one process over the in-memory loopback
    transport. The client connects, sends one reliable-ordered message that the
    server prints, and disconnects; a second client then connects and
    disconnects, so the server keeps serving. Run: ./bin/example_hello
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int main( void )
{
    rocketnet_init( NULL, NULL, NULL );

    // the published development backend key; a real server takes its own backend's key
    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
        server_status = rocketnet_server_start( server );
    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( server_status ) );
        rn_cli_server_stop_and_destroy( server );
        rocketnet_term();
        return 1;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) );
    rocketnet_server_public_key( server, server_public_key );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";

    double now = 1000.0;
    for ( uint64_t client_id = 1; client_id <= 2; client_id++ )
    {
        // a backend mints the token in production; this one expires in 30 seconds
        char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, client_id, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );
        if ( token_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: connect token: %s\n", rocketnet_status_name( token_status ) );
            rn_cli_server_stop_and_destroy( server );
            rocketnet_term();
            return 1;
        }

        enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
        if ( !client || create_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: client create: %s\n", rocketnet_status_name( create_status ) );
            rn_cli_server_stop_and_destroy( server );
            rocketnet_term();
            return 1;
        }
        const enum rocketnet_status_t connect_status = rocketnet_client_connect( client, connect_token );
        if ( connect_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: client connect: %s\n", rocketnet_status_name( connect_status ) );
            rn_cli_client_stop_and_destroy( client );
            rn_cli_server_stop_and_destroy( server );
            rocketnet_term();
            return 1;
        }

        const int messages = client_id == 1 ? 1 : 0;     // the second client only connects
        int sent = 0, received = 0, frame = 0;

        for ( ; frame < 5000 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || received < messages ); frame++ )
        {
            rocketnet_server_update( server, now );
            rocketnet_client_update( client, now );

            // take every received message before the next update
            const struct rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL ) {}
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
                for ( int i = 0; i < batch->num_messages; i++, received++ )
                    printf( "server received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );

            if ( sent < messages && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                // write the message straight into the buffer start returns
                const char message[] = "hello from the client";
                // reliable: defer to a later frame when there is no room; the frame bound above diagnoses a stuck defer
                if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) ) )
                {
                    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) );
                    if ( p )
                    {
                        memcpy( p, message, strlen( message ) );
                        sent += rocketnet_client_finish_message( client, (int) strlen( message ) ) != 0;
                    }
                }
            }

            // send last, so the messages queued this frame leave this frame
            rocketnet_client_send_packets( client );
            rocketnet_server_send_packets( server );
            rocketnet_sleep( 0.001 );
            now += 0.001;
        }

        if ( frame == 5000 )
        {
            printf( "FAIL: client %d did not finish within 5000 frames\n", (int) client_id );
            return 1;
        }
        rn_cli_client_stop_and_destroy( client );
    }

    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    printf( "PASS\n" );
    return 0;
}
