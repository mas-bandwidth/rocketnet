/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server server: real UDP on loopback. The server takes the
    published development backend key, makes its own server keypair at
    create, writes its public key to one small key file, and listens on
    127.0.0.1 at the port from --port <n>, else the ROCKETNET_EXAMPLE_PORT
    environment variable, else the default 40000. The separate client process
    plays the backend: it reads that file, mints a development connect token
    for this server, connects, and sends one reliable-ordered message. The
    server echoes it and exits after its first client disconnects.

    Run the server first, then the client. See docs/examples/client_server.md.
*/

#include "shutdown.h"
#include "rocketnet.h"
#include "dev_keys.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// serialize a received message's bytes straight into a started message
static int write_bytes( uint8_t * p, const uint8_t * data, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        p[i] = data[i];
    return bytes;
}

#define KEY_FILE "client_server.keys"
#define DEFAULT_PORT 40000

// MSVC deprecates getenv and fopen, so these use getenv_s and fopen_s there.
// example_getenv copies the variable into buffer and returns 1, or returns 0
// when it is unset or does not fit.

static int example_getenv( const char * name, char * buffer, size_t size )
{
#if defined( _MSC_VER )
    size_t length = 0;
    return getenv_s( &length, buffer, size, name ) == 0 && length > 0;
#else
    const char * value = getenv( name );
    if ( !value || strlen( value ) >= size )
        return 0;
    memcpy( buffer, value, strlen( value ) + 1 );
    return 1;
#endif
}

static FILE * example_fopen( const char * path, const char * mode )
{
#if defined( _MSC_VER )
    FILE * file = NULL;
    return fopen_s( &file, path, mode ) == 0 ? file : NULL;
#else
    return fopen( path, mode );
#endif
}

// the port comes from --port <n>, else ROCKETNET_EXAMPLE_PORT, else the
// default; returns 0 for a value that is not 1 to 65535

static int example_port( int argc, char ** argv, int * port )
{
    char environment[256];
    const char * text = example_getenv( "ROCKETNET_EXAMPLE_PORT", environment, sizeof( environment ) ) ? environment : NULL;
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


int main( int argc, char ** argv )
{
    struct rocketnet_server_config_t config;
    struct rocketnet_server_t * server = NULL;
    static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char served_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
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

    // a key file left by an earlier run holds keys this server does not use;
    // remove it so the client waits for this run's file
    remove( KEY_FILE );

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    // a deployment's server takes its own backend's public key; this one
    // takes the published development backend key (rocketnet.h), whose
    // tokens rocketnet_create_dev_connect_token mints. The server keypair is
    // the server's own, made at create.

    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    config.max_clients = 1;
    config.transport = "portable";
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, address, NULL, &config, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    // the backend registers each server's public key; here the key file
    // carries it to the client process, which plays the backend

    rocketnet_server_public_key( server, server_public_key );
    CHECK( rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) ) );
    FILE * keys = example_fopen( KEY_FILE, "w" );
    CHECK( keys != NULL );
    write_hex( keys, "server_public", server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES );
    fprintf( keys, "server_ipv4 %s\n", served_address );
    CHECK( fclose( keys ) == 0 );

    printf( "server listening on 127.0.0.1:%d with protocol id 0x%llx\n", (int) rocketnet_server_port( server ), (unsigned long long) EXAMPLE_PROTOCOL_ID );
    printf( "server wrote %s for the client\n", KEY_FILE );
    fflush( stdout );

    const double start = rocketnet_time();

    while ( rocketnet_time() - start < 30.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );

        // (1) update pumps the transport and takes in what arrived
        rocketnet_server_update( server, now );

        // (2) receive and release every received message
        // one queue across every client, each message naming its sender
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            int i = 0;
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            {
                for ( int j = 0; j < batch->num_messages; j++ )
                {
                    data = batch->data[j];
                    bytes = batch->bytes[j];
                    i = batch->client_index[j];
                    printf( "server received: %.*s\n", bytes, (const char*) data );
                    uint8_t * out = rocketnet_server_start_message( server, (uint64_t) i, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
                    if ( out )
                    {
                        if ( rocketnet_server_finish_message( server, write_bytes( out, data, bytes ) ) )
                            echoed = 1;
                    }
                }
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
            printf( "server client disconnected (reason %s)\n", rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, 0 ) ) );
            break;
        }

        rocketnet_sleep( 0.001 );
    }

    CHECK( had_client && echoed );
    result = 0;

cleanup:
    // destroy on NULL is a no-op, as free(NULL) is
    if ( server )
        rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    // CLI cleanup helpers wait for shutdown completion before destroy.
    // PASS is printed afterward so it stays the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
