/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    simple_client.c — a client that sends input every tick and reads broadcast
    world state back.

    It pairs with examples/simple_server.c. The two use the two different receive
    shapes on purpose: the server takes the callback, which is the recommended
    default because the pointer's lifetime is over when the call returns; this
    client takes the batch form, because a client typically wants to drain a
    tick's arrivals into its own snapshot buffer and the batch form is the one
    that costs a single boundary crossing to do it.

    Build (once there is an implementation to link against):

        cc -o simple_client examples/simple_client.c -I. -lrocketnet
*/

#include "rocketnet.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>

#if defined( _WIN32 )
#include <windows.h>
static void sleep_ms( int ms ) { Sleep( ms ); }
#else
#include <time.h>
static void sleep_ms( int ms )
{
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = ( ms % 1000 ) * 1000000;
    nanosleep( &t, NULL );
}
#endif

// ---------------------------------------------------------------------------

#define PROTOCOL_ID     0x524F434B4E455401ULL

#define TICKS_PER_SECOND    60

// These MUST match simple_server.c. Index N is the same discipline on both ends
// or the handshake refuses the connection with ROCKETNET_ERROR_CHANNEL_MISMATCH.
#define CHANNEL_CONTROL     0
#define CHANNEL_STATE       1
#define CHANNEL_INPUT       2

#define MAX_STATE_BYTES     8192

#define MAX_PAYLOADS_PER_TICK   64

// ---------------------------------------------------------------------------

static volatile int quit = 0;

static void interrupt_handler( int signal ) { (void) signal; quit = 1; }

// ---------------------------------------------------------------------------
// Sending input
// ---------------------------------------------------------------------------

typedef struct player_input_t
{
    uint32_t tick;
    float move_x;
    float move_y;
    uint32_t buttons;

} player_input_t;

static void send_input( struct rocketnet_client_t * client, uint32_t tick )
{
    struct rocketnet_buffer_t buffer;

    int result = rocketnet_client_claim( client, sizeof( player_input_t ), &buffer );
    if ( result != ROCKETNET_OK )
        return;         // pool full; the next input supersedes this one anyway

    // Write straight into the transport's memory. No staging buffer, no copy.
    // A real client would run its bit packer over buffer.data.
    player_input_t * input = (player_input_t*) buffer.data;
    input->tick    = tick;
    input->move_x  = 0.0f;
    input->move_y  = 1.0f;
    input->buttons = 0;

    // CHANNEL_INPUT is most-recent-only, so if the last input has not gone out
    // yet this one replaces it rather than queueing behind it. A backlog of
    // stale input never accumulates and the bandwidth is saved outright.
    result = rocketnet_client_commit( client, CHANNEL_INPUT, &buffer, sizeof( player_input_t ) );
    if ( result != ROCKETNET_OK )
        printf( "commit failed: %s\n", rocketnet_error_string( result ) );
}

// ---------------------------------------------------------------------------
// Receiving, in the batch form
// ---------------------------------------------------------------------------

static void drain_received( struct rocketnet_client_t * client )
{
    struct rocketnet_payload_t payloads[MAX_PAYLOADS_PER_TICK];
    int num_payloads = 0;

    if ( rocketnet_client_receive( client, payloads, MAX_PAYLOADS_PER_TICK, &num_payloads ) != ROCKETNET_OK )
        return;

    for ( int i = 0; i < num_payloads; ++i )
    {
        // payloads[i].data points into the transport's memory and stays valid
        // until the release below. Fragments are already reassembled; a
        // multi-packet world state arrives here contiguous.

        if ( payloads[i].channel == CHANNEL_STATE )
        {
            printf( "state: %.*s\n", payloads[i].bytes, (const char*) payloads[i].data );
        }
        else if ( payloads[i].channel == CHANNEL_CONTROL )
        {
            printf( "control: %.*s\n", payloads[i].bytes, (const char*) payloads[i].data );
        }
    }

    // One call for the batch. Holding these longer holds pool memory on this
    // client, so release in the same tick unless there is a reason not to.
    if ( num_payloads > 0 )
        rocketnet_client_release( client, payloads, num_payloads );
}

// ---------------------------------------------------------------------------

int main( int argc, char ** argv )
{
    const char * server_address = ( argc > 1 ) ? argv[1] : "127.0.0.1:40000";

    signal( SIGINT, interrupt_handler );

    if ( rocketnet_init( NULL ) != ROCKETNET_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

    // -----------------------------------------------------------------------
    // The connect token.
    //
    // IN A REAL GAME THIS DOES NOT HAPPEN HERE. A backend the player cannot
    // reach into issues the token, seals the player's identity into it, and
    // hands it to the client over an authenticated channel. Generating it in
    // the client, as this example does, means the client holds the private key,
    // which defeats the entire security model. It is done inline only so the
    // example runs without a backend beside it.
    // -----------------------------------------------------------------------

    uint8_t private_key[ROCKETNET_KEY_BYTES];
    memset( private_key, 0, sizeof( private_key ) );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );
    uint64_t player_id = 12345;
    memcpy( user_data, &player_id, sizeof( player_id ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];

    const char * server_addresses[] = { server_address };

    if ( rocketnet_generate_connect_token( PROTOCOL_ID,
                                           player_id,
                                           45,                  // token expires in 45 seconds
                                           10,                  // connection times out after 10 idle seconds
                                           1,
                                           server_addresses,
                                           private_key,
                                           user_data,
                                           connect_token ) != ROCKETNET_OK )
    {
        printf( "error: failed to generate connect token\n" );
        rocketnet_term();
        return 1;
    }

    // -----------------------------------------------------------------------
    // The client
    // -----------------------------------------------------------------------

    struct rocketnet_client_config_t config;
    rocketnet_default_client_config( &config );

    strncpy( config.bind_address, "0.0.0.0:0", sizeof( config.bind_address ) - 1 );

    config.protocol_id = PROTOCOL_ID;
    config.pool_bytes  = 1 * 1024 * 1024;       // a client sends little
    config.backend     = ROCKETNET_BACKEND_AUTO;

    // The channel layout, which must match simple_server.c exactly.
    config.connection.num_channels = 3;

    rocketnet_default_channel_config( &config.connection.channel[CHANNEL_CONTROL], ROCKETNET_CHANNEL_RELIABLE );

    rocketnet_default_channel_config( &config.connection.channel[CHANNEL_STATE], ROCKETNET_CHANNEL_LATEST );
    config.connection.channel[CHANNEL_STATE].max_payload_bytes = MAX_STATE_BYTES;

    rocketnet_default_channel_config( &config.connection.channel[CHANNEL_INPUT], ROCKETNET_CHANNEL_LATEST );

    // NULL, because this client drains with rocketnet_client_receive instead.
    // Setting both is an error rather than a race.
    config.receive_callback = NULL;

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config );
    if ( !client )
    {
        printf( "error: failed to create client\n" );
        rocketnet_term();
        return 1;
    }

    if ( rocketnet_client_connect( client, connect_token ) != ROCKETNET_OK )
    {
        printf( "error: failed to start connecting\n" );
        rocketnet_client_destroy( client );
        rocketnet_term();
        return 1;
    }

    printf( "connecting to %s from port %d\n", server_address, rocketnet_client_port( client ) );

    uint32_t tick = 0;
    int last_state = ROCKETNET_CLIENT_STATE_DISCONNECTED;

    while ( !quit )
    {
        rocketnet_client_update( client );

        int state = rocketnet_client_state( client );

        if ( state != last_state )
        {
            if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
                printf( "connected\n" );
            else if ( state < 0 )
                printf( "connection failed, state %d\n", state );

            last_state = state;
        }

        // Negative is terminal: the token was bad, the server said no, or we
        // timed out. There is nothing to retry without a fresh token.
        if ( state < 0 )
            break;

        if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            drain_received( client );

            send_input( client, tick );

            if ( ( tick % ( TICKS_PER_SECOND * 5 ) ) == 0 )
            {
                struct rocketnet_connection_stats_t stats;
                if ( rocketnet_client_stats( client, &stats ) )
                {
                    printf( "rtt %.1fms  jitter %.1fms  loss %.1f%%  superseded %" PRIu64 "\n",
                            stats.rtt_ms,
                            stats.jitter_ms,
                            stats.packet_loss_percent,
                            stats.payloads_superseded );
                }
            }
        }

        tick++;

        sleep_ms( 1000 / TICKS_PER_SECOND );
    }

    printf( "shutting down\n" );

    // Tells the server rather than letting it wait out a timeout.
    rocketnet_client_disconnect( client );

    rocketnet_client_destroy( client );

    rocketnet_term();

    return 0;
}
