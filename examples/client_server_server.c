/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server server: real UDP on 127.0.0.1, at the port from --port <n>,
    else the ROCKETNET_EXAMPLE_PORT environment variable, else 40000. The
    server writes its address and public key to a key file for the client
    process, which plays the backend: it mints a development connect token
    for this server, connects, and sends one reliable-ordered message. The
    server echoes it and exits after its first client disconnects.

    Run the server first, then the client. See docs/examples/client_server.md.
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates getenv and fopen

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_FILE "client_server.keys"

int main( int argc, char ** argv )
{
    const char * port = getenv( "ROCKETNET_EXAMPLE_PORT" );
    for ( int i = 1; i + 1 < argc; i++ )
        if ( strcmp( argv[i], "--port" ) == 0 )
            port = argv[i + 1];
    char address[32];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", port ? atoi( port ) : 40000 );

    // a key file left by an earlier run names keys this server does not use
    remove( KEY_FILE );

    rocketnet_init( NULL, NULL, NULL );

    // a deployment's server takes its own backend's public key; this one takes
    // the published development key, whose tokens the client mints
    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    config.max_clients = 1;
    config.transport = "portable";

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
        server_status = rocketnet_server_start( server );
    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        // the example runner reads this line as a port already taken, and retries on another
        printf( "FAIL: server != NULL\n" );
        rn_cli_server_stop_and_destroy( server );
        rocketnet_term();
        return 1;
    }

    // a backend learns each server's address and public key; here the key
    // file carries them to the client. No secret is in it
    char served_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) );
    rocketnet_server_public_key( server, server_public_key );
    FILE * keys = fopen( KEY_FILE, "w" );
    if ( !keys )
    {
        printf( "FAIL: could not write %s\n", KEY_FILE );
        rn_cli_server_stop_and_destroy( server );
        rocketnet_term();
        return 1;
    }
    fprintf( keys, "server_public " );
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
        fprintf( keys, "%02x", server_public_key[i] );
    fprintf( keys, "\nserver_ipv4 %s\n", served_address );
    fclose( keys );

    printf( "server listening on %s with protocol id 0x%llx\n", served_address, (unsigned long long) EXAMPLE_PROTOCOL_ID );
    printf( "server wrote %s for the client\n", KEY_FILE );
    fflush( stdout );

    int had_client = 0, echoed = 0;
    const double start = rocketnet_time();
    while ( rocketnet_time() - start < 30.0 )
    {
        rocketnet_server_update( server, 1000.0 + rocketnet_time() - start );

        // one queue across every client, each message naming its sender: echo each one back
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                printf( "server received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
                if ( rocketnet_server_can_send_message( server, batch->client_index[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] ) )
                {
                    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) batch->client_index[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] );
                    if ( p )
                    {
                        memcpy( p, batch->data[i], (size_t) batch->bytes[i] );
                        echoed |= rocketnet_server_finish_message( server, batch->bytes[i] ) != 0;
                    }
                }
            }
        }

        rocketnet_server_send_packets( server );

        if ( rocketnet_server_num_connected_clients( server ) > 0 )
            had_client = 1;
        else if ( had_client )
        {
            printf( "server client disconnected (reason %s)\n", rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, 0 ) ) );
            break;
        }

        rocketnet_sleep( 0.001 );
    }

    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    printf( had_client && echoed ? "PASS\n" : "FAIL: a client connected and its message was echoed\n" );
    return had_client && echoed ? 0 : 1;
}
