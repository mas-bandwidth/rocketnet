/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Backend server: the server a backend sends clients to. It takes the
    backend public key from ROCKETNET_SERVER_BACKEND_PUBLIC_KEY (or the
    published development key in a development build) and listens on
    127.0.0.1 at the port from --port <n>, else ROCKETNET_EXAMPLE_PORT, else
    40000. The backend mints each client's token for this server's address and
    public key, so the server prints both and writes them to backend.env as
    the two environment variables the backends in examples/backend/ read. It
    echoes the client's message and exits after its first client disconnects.

    Run this server, then a backend, then example_backend_client. See
    examples/backend/README.md.
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates getenv and fopen

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV_FILE "backend.env"

// standard base64 with padding, the form the backends read keys in
static void write_base64( const uint8_t * bytes, int count, char * output )
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for ( int i = 0; i < count; i += 3, output += 4 )
    {
        const uint32_t b = i + 1 < count ? bytes[i + 1] : 0u;
        const uint32_t c = i + 2 < count ? bytes[i + 2] : 0u;
        const uint32_t triple = ( (uint32_t) bytes[i] << 16 ) | ( b << 8 ) | c;
        output[0] = digits[( triple >> 18 ) & 63];
        output[1] = digits[( triple >> 12 ) & 63];
        output[2] = i + 1 < count ? digits[( triple >> 6 ) & 63] : '=';
        output[3] = i + 2 < count ? digits[triple & 63] : '=';
    }
    *output = '\0';
}

int main( int argc, char ** argv )
{
    const char * port = getenv( "ROCKETNET_EXAMPLE_PORT" );
    for ( int i = 1; i + 1 < argc; i++ )
        if ( strcmp( argv[i], "--port" ) == 0 )
            port = argv[i + 1];
    char address[32];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", port ? atoi( port ) : 40000 );

    // a file left by an earlier run names a key this server does not use
    remove( ENV_FILE );

    rocketnet_init( NULL, NULL, NULL );

    // the packet filter is the title's, shared with its clients. A deployment's
    // server takes its backend's public key through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY
    // or in config; a release library refuses the development key
    const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = EXAMPLE_BACKEND_PACKET_FILTER;
    struct rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    config.max_clients = 1;
    config.transport = "portable";
    memcpy( config.packet_filter, packet_filter, sizeof( packet_filter ) );
#if ROCKETNET_DEV
    const uint8_t dev_backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( dev_backend_public_key ) );
#endif

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &status );
    if ( server && status == ROCKETNET_STATUS_OK )
        status = rocketnet_server_start( server );
    if ( !server || status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( status ) );
        rn_cli_server_stop_and_destroy( server );
        rocketnet_term();
        return 1;
    }
    printf( "backend_server: mode=%s transport=%s\n", ROCKETNET_DEV ? "development" : "production", rocketnet_server_transport_name( server ) );

    // a server registers its address and public key with the backend; this
    // one prints them and writes them as the backend's environment
    char served_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char server_public_key_base64[4 * ( ( ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 2 ) / 3 ) + 1];
    rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) );
    rocketnet_server_public_key( server, server_public_key );
    write_base64( server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES, server_public_key_base64 );
    FILE * env = fopen( ENV_FILE, "w" );
    if ( !env )
    {
        printf( "FAIL: could not write %s\n", ENV_FILE );
        rn_cli_server_stop_and_destroy( server );
        rocketnet_term();
        return 1;
    }
    fprintf( env, "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\nROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", served_address, server_public_key_base64 );
    fclose( env );

    printf( "server listening on %s with protocol id 0x%llx\n", served_address, (unsigned long long) EXAMPLE_PROTOCOL_ID );
    printf( "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\n", served_address );
    printf( "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", server_public_key_base64 );
    printf( "server wrote %s for the backend\n", ENV_FILE );
    fflush( stdout );

    int had_client = 0, echoed = 0;
    const double start = rocketnet_time();
    while ( rocketnet_time() - start < 60.0 )
    {
        rocketnet_server_update( server, 1000.0 + rocketnet_time() - start );

        // one queue across every client, each message naming its sender: echo each one back
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                const int client_index = batch->client_index[i];
                printf( "server received from client %llx: %.*s\n", (unsigned long long) rocketnet_server_client_id( server, client_index ), batch->bytes[i], (const char *) batch->data[i] );
                // echo: skip this message when there is no room
                if ( rocketnet_server_can_send_message( server, client_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] ) )
                {
                    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) client_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] );
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
