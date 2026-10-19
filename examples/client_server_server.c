/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server server: real UDP on loopback with production keys. The
    server generates the backend signing keypair and its own server keypair
    through the public keygen calls, writes the backend secret and its server
    public key to one small key file, and listens on 127.0.0.1:40000. The
    separate client process plays the backend, reads that file, mints a
    production connect token, connects, and sends one reliable-ordered
    message. The server echoes it and exits after its first client
    disconnects.

    Run the server first, then the client. See docs/examples/client_server.md.
*/

#include "rocketnet.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

#define PROTOCOL_ID 0x636c69656e747331ull
#define KEY_FILE "client_server.keys"

static void write_hex( FILE * file, const char * label, const uint8_t * bytes, int count )
{
    static const char digits[] = "0123456789abcdef";
    fprintf( file, "%s ", label );
    for ( int i = 0; i < count; i++ )
    {
        fputc( digits[bytes[i] >> 4], file );
        fputc( digits[bytes[i] & 15], file );
    }
    fputc( '\n', file );
}

static const char * reason_name( int reason )
{
    static const char * names[] = { "none", "config mismatch", "config timeout", "protocol violation",
                                    "reliable timeout", "message memory exhausted" };
    return reason >= 0 && reason < 6 ? names[reason] : "unknown";
}

int main( void )
{
    struct rocketnet_server_config_t config;
    struct rocketnet_server_t * server = NULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    int result = 1;
    int had_client = 0;
    int echoed = 0;

    CHECK( rocketnet_init( NULL ) );

    // a deployment mints the backend keypair in the backend and gives servers
    // only the public half; here the server bootstraps it so the separate
    // client process can play the backend from the key file

    CHECK( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    CHECK( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    rocketnet_server_default_config( &config );
    config.protocol_id = PROTOCOL_ID;
    config.max_clients = 1;
    config.transport = "portable";
    memcpy( config.backend_public_key, backend_public_key, sizeof( config.backend_public_key ) );
    memcpy( config.server_public_key, server_public_key, sizeof( config.server_public_key ) );
    memcpy( config.server_secret_key, server_secret_key, sizeof( config.server_secret_key ) );

    server = rocketnet_server_create( NULL, "127.0.0.1:40000", &config, NULL, 0 );
    CHECK( server != NULL );

    FILE * keys = fopen( KEY_FILE, "w" );
    CHECK( keys != NULL );
    write_hex( keys, "backend_secret", backend_secret_key, ROCKETNET_BACKEND_SECRET_KEY_BYTES );
    write_hex( keys, "server_public", server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES );
    CHECK( fclose( keys ) == 0 );

    printf( "server listening on 127.0.0.1:%d with protocol id 0x%llx\n", (int) rocketnet_server_port( server ), (unsigned long long) PROTOCOL_ID );
    printf( "server wrote %s for the client\n", KEY_FILE );

    const double start = rocketnet_time();

    while ( rocketnet_time() - start < 30.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );
        const int max_clients = rocketnet_server_max_clients( server );

        // (1) update pumps the transport and takes in what arrived
        if ( rocketnet_server_update( server, now ) != ROCKETNET_MESSAGE_OK )
            break;

        // (2) receive and release every received message
        for ( int i = 0; i < max_clients; i++ )
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            while ( rocketnet_server_receive_message( server, i, 0, &data, &bytes ) )
            {
                printf( "server received: %.*s\n", bytes, (const char*) data );
                uint8_t * out = rocketnet_server_start_message( server, i, 0, ROCKETNET_RELIABLE_ORDERED, bytes );
                if ( out )
                {
                    memcpy( out, data, (size_t) bytes );
                    if ( rocketnet_server_finish_message( server, i, bytes ) )
                        echoed = 1;
                }
                rocketnet_server_release_message( server, i, data );
            }
        }

        // (3) run the game simulation for the frame: nothing to simulate here

        // (4) this frame's echo is queued above, as each message arrives

        // (5) send_packets (send_messages after the lifecycle rename) is last,
        // after this frame's messages are queued, so they leave this frame

        rocketnet_server_send_messages( server );

        if ( rocketnet_server_num_connected_clients( server ) > 0 )
            had_client = 1;
        else if ( had_client )
        {
            printf( "server client disconnected (reason %s)\n", reason_name( rocketnet_server_disconnect_reason( server, 0 ) ) );
            break;
        }

        rocketnet_sleep( 0.001 );
    }

    CHECK( had_client && echoed );
    printf( "PASS\n" );
    result = 0;

cleanup:
    rocketnet_server_destroy( server );
    rocketnet_term();
    return result;
}
