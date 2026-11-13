/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Client/server server: real UDP on 127.0.0.1, at the port from --port <n>,
    else the ROCKETNET_EXAMPLE_PORT environment variable, else 40000. The
    server writes its address and public key to a key file for the client
    process, which plays the backend: it mints a development connect token
    for this server, connects, and sends one reliable-ordered message. The
    server echoes it and exits after its first client disconnects.

    The server runs a game server's 60 Hz frame. The main thread does a bit
    of networking at the start of the frame, then the second half of the
    network update is split off to run on another thread in parallel with
    the main thread, and joined at the start of the next frame. A frame's
    echoes go on the wire at the start of the next frame: one frame of added
    send latency, 16.67 ms on average.

    Run the server first, then the client. See docs/examples/client_server.md.
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates getenv and fopen

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_FILE "client_server.keys"

#define FRAME_DT        ( 1.0 / 60.0 )
#define MAX_ECHOES      64          // messages echoed in one frame
#define MAX_ECHO_BYTES  256         // a longer message is printed, not echoed

// One frame of the game's state: what the main thread took from the network,
// for the second half of the network update to send back. The server keeps
// two and swaps them at the frame boundary, so the main thread never writes
// the frame the other thread is sending from.

struct echo_frame_t
{
    struct rocketnet_server_t * server;
    int num_messages;
    int client_index[MAX_ECHOES];
    int bytes[MAX_ECHOES];
    uint8_t data[MAX_ECHOES][MAX_ECHO_BYTES];
    int echoed;     // written by the other thread, read by the main thread after the join
};

// The second half of the network update, split off from the main thread:
// queue this frame's echoes. From the split until the join, this thread is
// the server's only user.

static void queue_echoes( void * data )
{
    struct echo_frame_t * frame = (struct echo_frame_t *) data;

    for ( int i = 0; i < frame->num_messages; i++ )
    {
        // echo: skip this message when there is no room

        if ( rocketnet_server_can_send_message( frame->server, frame->client_index[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, frame->bytes[i] ) )
        {
            uint8_t * p = rocketnet_server_start_message( frame->server, (uint64_t) frame->client_index[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, frame->bytes[i] );
            if ( p )
            {
                memcpy( p, frame->data[i], (size_t) frame->bytes[i] );
                frame->echoed |= rocketnet_server_finish_message( frame->server, frame->bytes[i] ) != 0;
            }
        }
    }
}

int main( int argc, char ** argv )
{
    const char * port = getenv( "ROCKETNET_EXAMPLE_PORT" );

    for ( int i = 1; i + 1 < argc; i++ )
    {
        if ( strcmp( argv[i], "--port" ) == 0 )
        {
            port = argv[i + 1];
        }
    }

    char * end = NULL;
    const long port_number = port ? strtol( port, &end, 10 ) : 40000;
    if ( port && ( port[0] < '0' || port[0] > '9' || *end != '\0' || port_number < 1 || port_number > 65535 ) )
    {
        printf( "FAIL: the port takes 1 .. 65535, not \"%s\"\n", port );
        return 1;
    }

    char address[32];
    snprintf( address, sizeof( address ), "127.0.0.1:%ld", port_number );

    // a key file left by an earlier run names keys this server does not use

    remove( KEY_FILE );

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // a deployment's server takes its own backend's public key; this one takes
    // the published development key, whose tokens the client mints

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    config.max_clients = 1;
    config.transport = "portable";

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
    {
        server_status = rocketnet_server_start( server );
    }

    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        // the example runner reads this line as a port already taken, and retries on another

        printf( "FAIL: server != NULL\n" );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    // a backend learns each server's address and public key; here the key
    // file carries them to the client. No secret is in it

    char served_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    rocketnet_server_public_key( server, server_public_key );

    FILE * keys = fopen( KEY_FILE, "w" );
    if ( !keys )
    {
        printf( "FAIL: could not write %s\n", KEY_FILE );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    fprintf( keys, "server_public " );

    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        fprintf( keys, "%02x", server_public_key[i] );
    }

    fprintf( keys, "\nserver_ipv4 %s\n", served_address );
    fclose( keys );

    printf( "server listening on %s with protocol id 0x%llx\n", served_address, (unsigned long long) EXAMPLE_PROTOCOL_ID );
    printf( "server wrote %s for the client\n", KEY_FILE );
    fflush( stdout );

    static struct echo_frame_t frames[2];
    struct echo_frame_t * split_off = NULL;     // the frame the other thread is sending from, until the join
    struct rn_cli_thread_t network;
    memset( &network, 0, sizeof( network ) );
    int had_client = 0, echoed = 0, thread_failed = 0;
    const double start = rocketnet_time();
    double frame_end = start + FRAME_DT;
    for ( uint32_t frame_number = 0; rocketnet_time() - start < 30.0; frame_number++ )
    {
        // 1. join: wait for last frame's second half of the network update.
        // The main thread owns the server again from here

        rn_cli_thread_join( &network );
        if ( split_off )
        {
            echoed |= split_off->echoed;
            split_off = NULL;
        }

        // 2. a bit of networking on the main thread: put last frame's echoes
        // on the wire, take in what arrived, and copy each message into this
        // frame's state, so nothing reads the batch after the next update

        rocketnet_server_send_packets( server );
        rocketnet_server_update( server, 1000.0 + rocketnet_time() - start );

        struct echo_frame_t * frame = &frames[frame_number % 2];
        frame->server = server;
        frame->num_messages = 0;
        frame->echoed = 0;

        // one queue across every client, each message naming its sender

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                printf( "server received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
                if ( frame->num_messages < MAX_ECHOES && batch->bytes[i] <= MAX_ECHO_BYTES )
                {
                    frame->client_index[frame->num_messages] = batch->client_index[i];
                    frame->bytes[frame->num_messages] = batch->bytes[i];
                    memcpy( frame->data[frame->num_messages], batch->data[i], (size_t) batch->bytes[i] );
                    frame->num_messages++;
                }
            }
        }

        // 3. the world update, on the main thread: a game simulates here.
        // This server only notes whether its client has come and gone

        if ( rocketnet_server_num_connected_clients( server ) > 0 )
        {
            had_client = 1;
        }
        else if ( had_client )
        {
            printf( "server client disconnected (reason %s)\n", rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, 0 ) ) );
            break;
        }

        // 4. split off the second half of the network update: another thread
        // queues this frame's echoes in parallel with the main thread, which
        // must not touch the server until the join at the start of the next frame

        if ( !rn_cli_thread_start( &network, queue_echoes, frame ) )
        {
            thread_failed = 1;
            break;
        }

        split_off = frame;

        // 5. the main thread waits for the frame edge; a game's main thread
        // goes on with the rest of its frame here

        const double now = rocketnet_time();
        if ( now < frame_end )
        {
            rocketnet_sleep( frame_end - now );
        }

        frame_end = now > frame_end + FRAME_DT ? now + FRAME_DT : frame_end + FRAME_DT;
    }

    rn_cli_thread_join( &network );
    if ( split_off )
    {
        echoed |= split_off->echoed;
    }

    if ( thread_failed )
    {
        printf( "FAIL: could not start a thread\n" );
    }

    rn_cli_server_stop_and_destroy( server );

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    const int passed = had_client && echoed && !thread_failed;
    printf( passed ? "PASS\n" : "FAIL: a client connected and its message was echoed\n" );
    return passed ? 0 : 1;
}
