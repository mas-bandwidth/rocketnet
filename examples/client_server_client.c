/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server client: the backend stub and the client in one program. The
    stub reads the server's address and public key from the key file the
    server writes, and mints a development connect token for that server. The
    client connects over real UDP on 127.0.0.1, sends one reliable-ordered
    message, receives the echo, prints its state changes, disconnects, and
    prints the state that says how the connection ended.

    --wrong-protocol mints the token for another protocol id: the server
    ignores it, which shows the token is bound to the title, and that refusal
    is the expected result. Run the server first, then this program. See
    docs/examples/client_server.md.
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates fopen and sscanf

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define KEY_FILE "client_server.keys"

static void state_changed( void * context, enum rocketnet_client_state_t previous, enum rocketnet_client_state_t current )
{
    (void) context;
    printf( "client state: %s -> %s\n", rocketnet_client_state_name( previous ), rocketnet_client_state_name( current ) );
}

// the key file is "server_public <hex>" then "server_ipv4 <address>"
static int read_keys( uint8_t * server_public_key, char * address )
{
    FILE * file = fopen( KEY_FILE, "r" );
    if ( !file )
        return 0;

    char hex[ROCKETNET_SERVER_PUBLIC_KEY_BYTES * 2 + 1] = "";
    int ok = fscanf( file, "server_public %64s server_ipv4 %63s", hex, address ) == 2 && strlen( hex ) == sizeof( hex ) - 1;

    fclose( file );
    for ( int i = 0; ok && i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        unsigned int byte;
        ok = sscanf( hex + i * 2, "%2x", &byte ) == 1;
        server_public_key[i] = (uint8_t) byte;
    }

    return ok;
}

int main( int argc, char ** argv )
{
    const int wrong_protocol = argc > 1 && strcmp( argv[1], "--wrong-protocol" ) == 0;

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the server writes the key file once it has started: wait up to 10 seconds for it

    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char address[64];
    const double keys_start = rocketnet_time();
    while ( !read_keys( server_public_key, address ) )
    {
        if ( rocketnet_time() - keys_start > 10.0 )
        {
            printf( "FAIL: no complete %s within 10 seconds\n", KEY_FILE );

            const enum rocketnet_status_t term_status = rocketnet_term();
            if ( term_status != ROCKETNET_STATUS_OK )
            {
                printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
            }

            return 1;
        }

        rocketnet_sleep( 0.05 );
    }

    // a real backend mints the token with its own secret key and hands it to
    // the client; this stub mints a development token, stamped with Unix time

    const uint64_t protocol_id = EXAMPLE_PROTOCOL_ID + ( wrong_protocol ? 1 : 0 );
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( address, NULL, server_public_key, 30, 5, 0x0123456789abcdefull, protocol_id, (uint64_t) time( NULL ), NULL, connect_token );
    if ( token_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect token: %s\n", rocketnet_status_name( token_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    struct rocketnet_client_config_t config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    config.transport = "portable";
    config.state_change_callback = state_changed;

    enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &client_status );
    if ( !client || client_status != ROCKETNET_STATUS_OK || rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create/connect: %s\n", rocketnet_status_name( client_status ) );
        rn_cli_client_stop_and_destroy( client, 1000.0 );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    int sent = 0, received = 0;
    const double start = rocketnet_time();
    while ( !received && rocketnet_client_state( client ) >= 0 && rocketnet_time() - start < 10.0 )
    {
        rocketnet_client_update( client, 1000.0 + rocketnet_time() - start );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++, received++ )
            {
                printf( "client received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
            }
        }

        if ( !sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const char message[] = "hello from the client";
            if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) ) )
            {
                uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) );
                if ( p )
                {
                    memcpy( p, message, strlen( message ) );
                    sent = rocketnet_client_finish_message( client, (int) strlen( message ) ) != 0;
                }
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_sleep( 0.001 );
    }

    // a negative state is a connection the server or the protocol ended

    const enum rocketnet_client_state_t state = rocketnet_client_state( client );
    if ( wrong_protocol && state < 0 )
    {
        printf( "expected refusal: the server ignored a token for protocol id 0x%llx (state %s)\n", (unsigned long long) protocol_id, rocketnet_client_state_name( state ) );
    }

    const int result = wrong_protocol ? state < 0 : received > 0;

    // the state says how a connection ended: DISCONNECTED after our own disconnect

    rocketnet_client_disconnect( client );
    printf( "client state after disconnect: %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    rn_cli_client_stop_and_destroy( client, 1000.0 + rocketnet_time() - start );

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( result ? "PASS\n" : "FAIL: the echo arrived, or with --wrong-protocol the server refused the token\n" );
    return result ? 0 : 1;
}
