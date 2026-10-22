/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Backend server: the server a backend sends clients to. It takes the
    published development backend key, makes its own server keypair at
    create, and listens on 127.0.0.1 at the port from --port <n>, else the
    ROCKETNET_EXAMPLE_PORT environment variable, else the default 40000.

    The backend mints each client's connect token for this server's address
    and public key, so the server prints both, and writes them to
    backend.env as the two environment variables the backends in
    examples/backend/ read. The client gets its token from the backend over
    HTTP, connects, and sends one reliable-ordered message. The server echoes
    it and exits after its first client disconnects.

    Run this server, then a backend, then example_backend_client. See
    examples/backend/README.md.
*/

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

#define ENV_FILE "backend.env"
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

// standard base64 with padding, the form the backends read keys in; output
// holds 4 * ( ( bytes + 2 ) / 3 ) + 1 characters

static void write_base64( const uint8_t * bytes, int count, char * output )
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;
    for ( int i = 0; i < count; i += 3 )
    {
        const uint32_t a = bytes[i];
        const uint32_t b = i + 1 < count ? bytes[i + 1] : 0;
        const uint32_t c = i + 2 < count ? bytes[i + 2] : 0;
        const uint32_t triple = ( a << 16 ) | ( b << 8 ) | c;
        output[j++] = digits[( triple >> 18 ) & 63];
        output[j++] = digits[( triple >> 12 ) & 63];
        output[j++] = i + 1 < count ? digits[( triple >> 6 ) & 63] : '=';
        output[j++] = i + 2 < count ? digits[triple & 63] : '=';
    }
    output[j] = '\0';
}


int main( int argc, char ** argv )
{
    struct rocketnet_server_config_t config;
    struct rocketnet_server_t * server = NULL;
    static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char server_public_key_base64[4 * ( ( ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 2 ) / 3 ) + 1];
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

    // a file left by an earlier run names a key this server does not use;
    // remove it so nothing reads it before this run's file
    remove( ENV_FILE );

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    // the backend signs tokens with the published development backend key
    // unless ROCKETNET_BACKEND_PRIVATE_KEY gives it its own, so this server
    // takes the development public key. A deployment's server takes its
    // backend's public key instead, through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY
    // or in config, and a release library refuses the development key.

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
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    // the server registers its address and public key with the backend; here
    // it prints them and writes them as the backend's environment

    rocketnet_server_public_key( server, server_public_key );
    write_base64( server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES, server_public_key_base64 );
    CHECK( rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) ) );
    FILE * env = example_fopen( ENV_FILE, "w" );
    CHECK( env != NULL );
    fprintf( env, "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\n", served_address );
    fprintf( env, "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", server_public_key_base64 );
    CHECK( fclose( env ) == 0 );

    printf( "server listening on %s with protocol id 0x%llx\n", served_address, (unsigned long long) EXAMPLE_PROTOCOL_ID );
    printf( "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\n", served_address );
    printf( "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", server_public_key_base64 );
    printf( "server wrote %s for the backend\n", ENV_FILE );
    fflush( stdout );

    const double start = rocketnet_time();

    while ( rocketnet_time() - start < 60.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );

        // (1) update pumps the transport and takes in what arrived
        rocketnet_server_update( server, now );

        // (2) receive and release every received message
        // one queue across every client, each message naming its sender
        {
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            {
                for ( int j = 0; j < batch->num_messages; j++ )
                {
                    const uint8_t * data = batch->data[j];
                    const int bytes = batch->bytes[j];
                    const int i = batch->client_index[j];
                    printf( "server received from client %llx: %.*s\n", (unsigned long long) rocketnet_server_client_id( server, i ), bytes, (const char*) data );
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
        rocketnet_server_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
