/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server server: real UDP on loopback with production keys. The
    server generates the backend signing keypair and its own server keypair
    through the public keygen calls, writes the backend secret and its server
    public key to one small key file, and listens on 127.0.0.1 at the port
    from --port <n>, else the ROCKETNET_EXAMPLE_PORT environment variable,
    else the default 40000. The
    separate client process plays the backend, reads that file, mints a
    production connect token, connects, and sends one reliable-ordered
    message. The server echoes it and exits after its first client
    disconnects.

    Run the server first, then the client. See docs/examples/client_server.md.
*/

#include "rocketnet.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rn_public_receive_sender;

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

#define PROTOCOL_ID 0x636c69656e747331ull
#define KEY_FILE "client_server.keys"
#define DEFAULT_PORT 40000

// the port comes from --port <n>, else ROCKETNET_EXAMPLE_PORT, else the
// default; returns 0 for a value that is not 1 to 65535

static int example_port( int argc, char ** argv, int * port )
{
    const char * text = getenv( "ROCKETNET_EXAMPLE_PORT" );
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--port" ) == 0 )
        {
            if ( i + 1 >= argc ) return 0;
            text = argv[++i];
        }
    }
    if ( !text )
    {
        *port = DEFAULT_PORT;
        return 1;
    }
    char * end = NULL;
    const long value = strtol( text, &end, 10 );
    if ( end == text || *end != 0 || value < 1 || value > 65535 )
        return 0;
    *port = (int) value;
    return 1;
}

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
    return rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) reason );
}

int main( int argc, char ** argv )
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
    int port = 0;
    char address[32];

    if ( !example_port( argc, argv, &port ) )
    {
        printf( "FAIL: the port is not 1 to 65535 (--port <n> or ROCKETNET_EXAMPLE_PORT)\n" );
        return 1;
    }
    snprintf( address, sizeof( address ), "127.0.0.1:%d", port );

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

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

    server = rocketnet_server_create( NULL, address, &config, NULL, 0 );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
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
        if ( rocketnet_server_update( server, now ) != ROCKETNET_UPDATE_OK )
            break;

        // (2) receive and release every received message
        for ( int i = 0; i < max_clients; i++ )
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            while ( rocketnet_server_receive_message( server, &rn_public_receive_sender, &data, &bytes ) )
            {
                printf( "server received: %.*s\n", bytes, (const char*) data );
                uint8_t * out = rocketnet_server_start_message( server, i, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
                if ( out )
                {
                    memcpy( out, data, (size_t) bytes );
                    if ( rocketnet_server_finish_message( server, i, bytes ) )
                        echoed = 1;
                }
                rocketnet_server_release_message( server, data );
            }
        }

        // (3) run the game simulation for the frame: nothing to simulate here

        // (4) this frame's echo is queued above, as each message arrives

        // (5) send_packets is last,
        // after this frame's messages are queued, so they leave this frame

        rocketnet_server_send_packets( server );

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
    result = 0;

cleanup:
    rocketnet_server_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
