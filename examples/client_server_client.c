/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server client: the matchmaker stub and the client in one program.
    The stub reads the server's public key from the key file the server
    writes and mints a development connect token for that server. The
    client connects over real UDP on loopback, sends one reliable-ordered
    message, receives the echo, prints its state changes, disconnects, and
    prints the state that says how the connection ended.

    The server's address and public key both come from that key file: the
    server writes them once it has started, and no secret is in it.

    --wrong-protocol mints the token for another protocol id; the server
    refuses it, which shows the token is bound to the title. That refusal is
    the expected result. Run the server first, then this program. See
    docs/examples/client_server.md.
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// serialize text (without its terminator) straight into a started message
static int write_text( uint8_t * p, const char * text, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        p[i] = (uint8_t) text[i];
    return bytes;
}

#define KEY_FILE "client_server.keys"

// MSVC deprecates fopen, so this uses fopen_s there.

static FILE * example_fopen( const char * path, const char * mode )
{
#if defined( _MSC_VER )
    FILE * file = NULL;
    return fopen_s( &file, path, mode ) == 0 ? file : NULL;
#else
    return fopen( path, mode );
#endif
}

static int hex_value( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    return -1;
}

// reads one line, the label, a space, then count bytes as hex digits

static int read_hex( FILE * file, const char * label, uint8_t * bytes, int count )
{
    char line[256];
    if ( !fgets( line, sizeof( line ), file ) )
        return 0;
    line[strcspn( line, "\r\n" )] = '\0';
    const size_t label_length = strlen( label );
    if ( strncmp( line, label, label_length ) != 0 || line[label_length] != ' ' )
        return 0;
    const char * text = line + label_length + 1;
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
// reads one line, the label, a space, then the text up to the end of the line

static int read_text( FILE * file, const char * label, char * text, size_t text_bytes )
{
    char line[ROCKETNET_MAX_ADDRESS_STRING_BYTES + 64];
    if ( !fgets( line, sizeof( line ), file ) )
        return 0;
    line[strcspn( line, "\r\n" )] = '\0';
    const size_t label_length = strlen( label );
    if ( strncmp( line, label, label_length ) != 0 || line[label_length] != ' ' )
        return 0;
    const char * value = line + label_length + 1;
    const size_t value_length = strlen( value );
    if ( value_length == 0 || value_length >= text_bytes )
        return 0;
    memcpy( text, value, value_length + 1 );
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
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    struct rocketnet_client_config_t config;
    struct rocketnet_client_t * client = NULL;
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    int result = 1;
    int sent = 0;
    int received = 0;
    int refused = 0;

    int have_keys = 0;

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    // the server writes the key file once it has started, which can take
    // longer than a launcher's head start; wait up to 10 seconds for a
    // complete file rather than failing on the first look
    const double keys_start = rocketnet_time();
    while ( !have_keys && rocketnet_time() - keys_start < 10.0 )
    {
        FILE * keys = example_fopen( KEY_FILE, "r" );
        if ( keys )
        {
            have_keys = read_hex( keys, "server_public", server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES ) &&
                        read_text( keys, "server_ipv4", address, sizeof( address ) );
            fclose( keys );
        }
        if ( !have_keys )
            rocketnet_sleep( 0.05 );
    }
    CHECK( have_keys );

    // the matchmaker stub: a real backend mints with its own secret key on
    // its own server and returns the token to the game client; this one
    // mints a development token. The fixed timestamp keeps the example off
    // the wall clock; a backend stamps now.

    const uint64_t protocol_id = EXAMPLE_PROTOCOL_ID + ( wrong_protocol ? 1 : 0 );
    CHECK( rocketnet_create_dev_connect_token( address, NULL, server_public_key, 30, 5, 0x0123456789abcdefull,
                                               protocol_id, 4102444800ull, NULL, connect_token ) == ROCKETNET_STATUS_OK );
    rocketnet_client_default_config( &config );
    config.transport = "portable";
    config.state_change_callback = state_changed;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &config, &create_status );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
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
        rocketnet_client_update( client, now );

        // (2) receive and release every received message
        const uint8_t * data = NULL;
        int bytes = 0;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
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
                sent = rocketnet_client_finish_message( client, write_text( out, message, message_bytes ) ) != 0;
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
    // the state says how a connection ended: DISCONNECTED after our own
    // disconnect, a negative state when the server or the protocol ended it
    printf( "client state after disconnect: %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    result = 0;

cleanup:
    // destroy on NULL is a no-op, as free(NULL) is
    if ( client )
        rocketnet_client_destroy( client );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
