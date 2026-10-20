/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server client: the matchmaker stub and the client in one program.
    The stub holds the backend signing key and the server's public key from
    the key file the server writes, and mints a production connect token. The
    client connects over real UDP on loopback, sends one reliable-ordered
    message, receives the echo, prints its state changes and disconnect
    reason, and disconnects.

    The server address is 127.0.0.1 at the port from --port <n>, else the
    ROCKETNET_EXAMPLE_PORT environment variable, else the default 40000.

    --wrong-protocol mints the token for another protocol id; the server
    refuses it, which shows the token is bound to the title. That refusal is
    the expected result. Run the server first, then this program. See
    docs/examples/client_server.md.
*/

#include "rocketnet.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

#define PROTOCOL_ID 0x636c69656e747331ull
#define DEFAULT_PORT 40000
#define KEY_FILE "client_server.keys"

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
static int hex_value( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    return -1;
}

static int read_hex( FILE * file, const char * label, uint8_t * bytes, int count )
{
    char read_label[32];
    char text[129];
    if ( fscanf( file, "%31s %128s", read_label, text ) != 2 || strcmp( read_label, label ) != 0 )
        return 0;
    if ( strlen( text ) != (size_t) count * 2 )
        return 0;
    for ( int i = 0; i < count; i++ )
    {
        const int high = hex_value( text[i * 2] );
        const int low = hex_value( text[i * 2 + 1] );
        if ( high < 0 || low < 0 ) return 0;
        bytes[i] = (uint8_t) ( high * 16 + low );
    }
    return 1;
}
static void state_changed( void * context, enum rocketnet_client_state_t previous, enum rocketnet_client_state_t current )
{
    (void) context;
    printf( "client state: %s -> %s\n", rocketnet_client_state_name( previous ), rocketnet_client_state_name( current ) );
}

int main( int argc, char ** argv )
{
    int wrong_protocol = 0;
    for ( int i = 1; i < argc; i++ )
        if ( strcmp( argv[i], "--wrong-protocol" ) == 0 )
            wrong_protocol = 1;
    int port = 0;
    char address[32];
    if ( !example_port( argc, argv, &port ) )
    {
        printf( "FAIL: the port is not 1 to 65535 (--port <n> or ROCKETNET_EXAMPLE_PORT)\n" );
        return 1;
    }
    snprintf( address, sizeof( address ), "127.0.0.1:%d", port );
    struct rocketnet_client_config_t config;
    struct rocketnet_client_t * client = NULL;
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * server_addresses[1] = { address };
    int result = 1;
    int sent = 0;
    int received = 0;
    int refused = 0;

    int have_keys = 0;

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

    // the server writes the key file once it has started, which can take
    // longer than a launcher's head start; wait up to 10 seconds for a
    // complete file rather than failing on the first look
    const double keys_start = rocketnet_time();
    while ( !have_keys && rocketnet_time() - keys_start < 10.0 )
    {
        FILE * keys = fopen( KEY_FILE, "r" );
        if ( keys )
        {
            have_keys = read_hex( keys, "backend_secret", backend_secret_key, ROCKETNET_BACKEND_SECRET_KEY_BYTES ) &&
                        read_hex( keys, "server_public", server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES );
            fclose( keys );
        }
        if ( !have_keys )
            rocketnet_sleep( 0.05 );
    }
    CHECK( have_keys );

    // the matchmaker stub: a real backend runs this on its own server, keeps
    // the secret key, and returns the token to the game client. The fixed
    // timestamps keep the example off the wall clock; a backend stamps now.

    const uint64_t protocol_id = PROTOCOL_ID + ( wrong_protocol ? 1 : 0 );
    CHECK( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id,
                                                     server_addresses, 1, 0x0123456789abcdefull, 0, 4102444800ull, 5, NULL,
                                                     connect_token, sizeof( connect_token ) ) == ROCKETNET_STATUS_OK );
    rocketnet_client_default_config( &config );
    config.transport = "portable";
    config.state_change_callback = state_changed;
    client = rocketnet_client_create( NULL, &config, NULL, 0 );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );
    const double start = rocketnet_time();

    while ( rocketnet_time() - start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );

        // (1) update pumps the transport and takes in what arrived
        if ( rocketnet_client_update( client, now ) != ROCKETNET_UPDATE_OK ) break;

        // (2) receive and release every received message
        const uint8_t * data = NULL;
        int bytes = 0;
        struct rocketnet_message_batch_t batch;
        while ( rocketnet_client_receive_messages( client, &batch ) )
        {
            for ( int i = 0; i < batch.num_messages; i++ )
            {
                data = batch.data[i];
                bytes = batch.bytes[i];
                printf( "client received: %.*s\n", bytes, (const char*) data );

                received = 1;
            }
        }

        // (3) run the game simulation for the frame: nothing to simulate here

        // (4) queue this frame's outgoing message, once the client is connected
        const int state = rocketnet_client_state( client );
        if ( !wrong_protocol && !sent && state == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const char message[] = "hello from the client";
            const int message_bytes = (int) sizeof( message ) - 1;
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message_bytes );
            if ( out )
            {
                memcpy( out, message, (size_t) message_bytes );
                sent = rocketnet_client_finish_message( client, message_bytes ) != 0;
            }
        }

        // (5) send_packets is last,
        // after this frame's messages are queued, so they leave this frame

        rocketnet_client_send_packets( client );

        if ( received ) break;
        if ( wrong_protocol && state < 0 )
        {
            printf( "expected refusal: the server ignored a token for protocol id 0x%llx (state %s)\n", (unsigned long long) protocol_id, rocketnet_client_state_name( state ) );
            refused = 1;
            break;
        }
        rocketnet_sleep( 0.001 );
    }

    CHECK( wrong_protocol ? refused : received );
    rocketnet_client_disconnect( client );
    printf( "client disconnect reason: %s\n", rocketnet_disconnect_reason_name( rocketnet_client_disconnect_reason( client ) ) );
    result = 0;

cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (rocketnet_api_null_guard)
    if ( client )
        rocketnet_client_destroy( client );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
