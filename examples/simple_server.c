/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    simple_server.c — a server that broadcasts world state to a flow and reads
    input back from each client.

    It is deliberately the shape a real game server has rather than the shape a
    minimal test has: three channels doing three different jobs, one broadcast
    payload written once and delivered to everyone, and per-client input read on
    a most-recent-only channel.

    Build (once there is an implementation to link against):

        cc -o simple_server examples/simple_server.c -I. -lrocketnet
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

#define PROTOCOL_ID     0x524F434B4E455401ULL      // "ROCKNET" and a version byte

#define TICKS_PER_SECOND    60

// The three jobs a game server actually has, one channel each. Channel indices
// are a contract between the two ends: index N here must be index N in
// simple_client.c or the handshake refuses the connection.

#define CHANNEL_CONTROL     0       // reliable, ordered: spawn, despawn, chat
#define CHANNEL_STATE       1       // most recent only: the broadcast world state
#define CHANNEL_INPUT       2       // most recent only: client input, read here

#define MAX_STATE_BYTES     8192    // bigger than a packet, so STATE fragments

// ---------------------------------------------------------------------------

typedef struct server_app_t
{
    struct rocketnet_server_t * server;
    rocketnet_flow_id_t world_flow;
    uint32_t tick;

} server_app_t;

static volatile int quit = 0;

static void interrupt_handler( int signal ) { (void) signal; quit = 1; }

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void on_connect( struct rocketnet_server_t * server,
                        void * context,
                        rocketnet_connection_id_t connection_id,
                        const uint8_t * user_data )
{
    server_app_t * app = (server_app_t*) context;

    (void) server;

    // user_data was sealed into the connect token by the backend, so this is a
    // player identity the client could not have forged or altered.
    uint64_t player_id;
    memcpy( &player_id, user_data, sizeof( player_id ) );

    printf( "client %" PRIu64 " connected as player %" PRIu64 "\n", connection_id, player_id );

    // Interest management is game logic; carrying the subscription is not. A
    // real server would decide WHICH flows this player belongs in. This one has
    // a single world flow, so everyone joins it.
    //
    // Note there is no client-side subscribe call anywhere. The server puts
    // players in flows; a client discovers what it is subscribed to by
    // receiving it.
    rocketnet_server_flow_subscribe( app->server, app->world_flow, connection_id );
}

static void on_disconnect( struct rocketnet_server_t * server,
                           void * context,
                           rocketnet_connection_id_t connection_id,
                           int reason )
{
    (void) server;
    (void) context;

    // Flow membership is already dropped by the time this fires; there is
    // nothing to unsubscribe.
    printf( "client %" PRIu64 " disconnected (%s)\n", connection_id, rocketnet_error_string( reason ) );
}

static void on_receive( struct rocketnet_server_t * server,
                        void * context,
                        rocketnet_connection_id_t connection_id,
                        int channel,
                        const uint8_t * payload,
                        int bytes )
{
    (void) server;
    (void) context;

    // `payload` is valid for the duration of this call and not after. Act on it
    // here, or copy it out. Nothing to release, nothing to remember.

    if ( channel == CHANNEL_INPUT )
    {
        // Whatever arrives here is the newest input this client sent. Anything
        // older was discarded by the channel rather than delivered late, so
        // there is no staleness check to write.
        printf( "input from %" PRIu64 ": %d bytes\n", connection_id, bytes );
    }
    else if ( channel == CHANNEL_CONTROL )
    {
        printf( "control from %" PRIu64 ": %.*s\n", connection_id, bytes, (const char*) payload );
    }
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

static void broadcast_world_state( server_app_t * app )
{
    // One payload, written once, delivered to every subscriber. This is the
    // whole reason the send API looks the way it does: a real encoder writes
    // its output straight into buffer.data and never assembles it anywhere
    // else, and the same bytes then go to a thousand connections without being
    // copied a thousand times.

    struct rocketnet_buffer_t buffer;

    int result = rocketnet_server_claim( app->server, MAX_STATE_BYTES, &buffer );
    if ( result != ROCKETNET_OK )
    {
        // The pool is full, which means everything in it is still waiting to be
        // sent or acknowledged. Skipping this tick is the right answer on a
        // most-recent-only channel: the next state supersedes this one anyway.
        return;
    }

    // Write in place. A real server hands buffer.data to its delta encoder.
    int bytes = snprintf( (char*) buffer.data, (size_t) buffer.bytes, "world state tick %u", app->tick );

    result = rocketnet_server_commit_flow( app->server, app->world_flow, &buffer, bytes );
    if ( result != ROCKETNET_OK )
    {
        printf( "broadcast failed: %s\n", rocketnet_error_string( result ) );
        // The buffer is already consumed. There is nothing to clean up.
    }
}

static void announce_tick_to_all( server_app_t * app,
                                  const rocketnet_connection_id_t * ids,
                                  int num_ids )
{
    // The explicit broadcast form: an ad-hoc recipient set rather than a flow.
    //
    // The first version of this example looped over the connections calling
    // rocketnet_server_send once each. That is one copy and one boundary
    // crossing PER CONNECTION for a payload that is byte-identical for all of
    // them, which is precisely the cost this library exists to remove. Claiming
    // once and committing to the set is the same amount of code and none of the
    // waste, and it is what a C# caller must do to avoid paying a managed
    // transition per player.
    //
    // Use a flow instead when the recipient set persists across ticks, which it
    // usually does. Use this when it does not.

    if ( num_ids == 0 )
        return;

    struct rocketnet_buffer_t buffer;

    if ( rocketnet_server_claim( app->server, 64, &buffer ) != ROCKETNET_OK )
        return;

    int bytes = snprintf( (char*) buffer.data, (size_t) buffer.bytes, "tick %u", app->tick );

    rocketnet_server_commit_many( app->server, ids, num_ids, CHANNEL_CONTROL, &buffer, bytes );
}

// ---------------------------------------------------------------------------

int main( int argc, char ** argv )
{
    (void) argc;
    (void) argv;

    signal( SIGINT, interrupt_handler );

    if ( rocketnet_init( NULL ) != ROCKETNET_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

    server_app_t app;
    memset( &app, 0, sizeof( app ) );

    struct rocketnet_server_config_t config;
    rocketnet_default_server_config( &config );

    strncpy( config.server_address, "127.0.0.1:40000", sizeof( config.server_address ) - 1 );
    strncpy( config.bind_address,   "0.0.0.0:40000",   sizeof( config.bind_address ) - 1 );

    config.protocol_id     = PROTOCOL_ID;
    config.max_connections = 1024;
    config.max_flows       = 64;

    // In production this comes from the same place the backend gets it. A
    // hardcoded key is fine in an example and is a vulnerability in a game.
    memset( config.private_key, 0, ROCKETNET_KEY_BYTES );

    // Sized for the target: a thousand connections holding references to a few
    // fragmented state payloads each, plus reliable traffic waiting on acks.
    config.pool_bytes                     = 64 * 1024 * 1024;
    config.max_pool_bytes_per_connection  = 256 * 1024;

    // Kernel bypass where it is available and permitted, ordinary sockets where
    // it is not. Asking for XDP specifically would mean handling the case where
    // the machine has not been prepared for it.
    config.backend = ROCKETNET_BACKEND_AUTO;

    // The channel layout, which simple_client.c must configure identically.
    config.connection.num_channels = 3;

    rocketnet_default_channel_config( &config.connection.channel[CHANNEL_CONTROL], ROCKETNET_CHANNEL_RELIABLE );

    rocketnet_default_channel_config( &config.connection.channel[CHANNEL_STATE], ROCKETNET_CHANNEL_LATEST );
    config.connection.channel[CHANNEL_STATE].max_payload_bytes = MAX_STATE_BYTES;   // fragments

    rocketnet_default_channel_config( &config.connection.channel[CHANNEL_INPUT], ROCKETNET_CHANNEL_LATEST );

    config.receive_callback    = on_receive;
    config.connect_callback    = on_connect;
    config.disconnect_callback = on_disconnect;

    app.server = rocketnet_server_create( &app, &config );
    if ( !app.server )
    {
        printf( "error: failed to create server\n" );
        rocketnet_term();
        return 1;
    }

    app.world_flow = rocketnet_server_flow_create( app.server, CHANNEL_STATE );
    if ( app.world_flow == ROCKETNET_INVALID_FLOW_ID )
    {
        printf( "error: failed to create world flow\n" );
        rocketnet_server_destroy( app.server );
        rocketnet_term();
        return 1;
    }

    printf( "server listening on port %d\n", rocketnet_server_port( app.server ) );

    while ( !quit )
    {
        // Receives, dispatches, acknowledges, retransmits, times out. Every
        // callback above fires from inside here, on this thread.
        rocketnet_server_update( app.server );

        broadcast_world_state( &app );

        if ( ( app.tick % TICKS_PER_SECOND ) == 0 )
        {
            rocketnet_connection_id_t ids[1024];
            int num_ids = 0;
            rocketnet_server_connection_ids( app.server, ids, 1024, &num_ids );

            announce_tick_to_all( &app, ids, num_ids );

            struct rocketnet_server_stats_t stats;
            if ( rocketnet_server_stats( app.server, &stats ) )
            {
                printf( "%d connections, pool %d/%d bytes, backend %s\n",
                        stats.num_connections,
                        stats.pool_bytes_in_use,
                        stats.pool_bytes_high_water,
                        rocketnet_backend_string( stats.backend ) );
            }
        }

        app.tick++;

        sleep_ms( 1000 / TICKS_PER_SECOND );
    }

    printf( "shutting down\n" );

    rocketnet_server_destroy( app.server );

    rocketnet_term();

    return 0;
}
