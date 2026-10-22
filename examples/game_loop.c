/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop: a 60 Hz fixed-step loop driving one server and N clients in one
    process, each frame in the order a game runs it: update, receive, simulate,
    queue, then send last. Each client sends its position every frame and an
    event twice a second; the server sends each client a state update every
    frame.
    Messages carry the frame they were queued in, so receivers measure latency
    in frames. Each client's network calls per frame are timed against a 100
    microsecond budget, and the server's tick too; the budget is printed, never
    failed on, because shared machines are noisy. --wrong-order sends before
    queuing, so every message waits one more frame.

    Run it from the repository root: ./bin/example_game_loop
    [--clients <1..1000>] [--seconds <1..3600>] [--wrong-order]
*/

#include "shutdown.h"
#include "rocketnet.h"
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// The message calls: *_start_message, *_finish_message, *_receive_messages
// (one batch, released by the next receive or update) and *_send_packets.

#define FRAME_DT ( 1.0 / 60.0 )
#define BUDGET_US 100.0
#define BINS 10000                                          // 0.1 us bins up to 1 ms, then one overflow bin

#define MESSAGE_BYTES 12   // the frame it was queued in (uint32), and a position (two floats)
typedef struct { uint32_t bins[BINS + 1]; uint32_t count; double max, tick_sum, tick_max; } costs_t;
typedef struct { double sum; uint32_t count; uint32_t min, max; } latency_t;

static costs_t costs[2];                                    // [0] this second, [1] the whole run
static latency_t to_server, to_client;
static uint32_t measure_from = UINT32_MAX;                  // the first frame with every client connected

static void add_cost( double us, bool tick )
{
    for ( int i = 0; i < 2; i++ )
    {
        costs_t * c = &costs[i];
        if ( tick ) { c->tick_sum += us; c->tick_max = us > c->tick_max ? us : c->tick_max; continue; }
        c->bins[us * 10.0 < BINS ? (int) ( us * 10.0 ) : BINS]++; c->count++; c->max = us > c->max ? us : c->max;
    }
}

static double percentile( const costs_t * c, double p )
{
    uint32_t seen = 0;
    for ( int bin = 0; bin < BINS; bin++ )
        if ( ( seen += c->bins[bin] ) > 0 && seen >= p * c->count )
            return bin / 10.0;
    return c->max;
}

static void print_costs( const char * label, const costs_t * c, uint32_t ticks )
{
    printf( "%s client frame p50 %.1f us, p99 %.1f us, max %.1f us (budget %.0f us); server tick mean %.1f us, max %.1f us\n",
            label, percentile( c, 0.5 ), percentile( c, 0.99 ), c->max, BUDGET_US, c->tick_sum / ticks, c->tick_max );
}

// serialize a message straight into a buffer that start returned, or note the
// latency of a message that arrived; returns the bytes written
static int write_u32( uint8_t * p, uint32_t value ) { p[0] = (uint8_t) value; p[1] = (uint8_t) ( value >> 8 ); p[2] = (uint8_t) ( value >> 16 ); p[3] = (uint8_t) ( value >> 24 ); return 4; }
static int write_f32( uint8_t * p, float value ) { union { float f; uint32_t u; } bits; bits.f = value; return write_u32( p, bits.u ); }
static uint32_t read_u32( const uint8_t * p ) { return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 ); }
static int write_message( uint8_t * buffer, uint32_t frame )
{
    uint8_t * p = buffer;
    p += write_u32( p, frame );
    p += write_f32( p, 1.0f );
    p += write_f32( p, 2.0f );
    return (int) ( p - buffer );
}

static void read_message( latency_t * l, uint32_t frame, const uint8_t * data, int bytes )
{
    if ( bytes != MESSAGE_BYTES ) return;
    const uint32_t frames = frame - read_u32( data );                // received minus queued: 0 is the same frame
    l->min = l->count == 0 || frames < l->min ? frames : l->min;
    l->max = frames > l->max ? frames : l->max;
    l->sum += frames; l->count++;
}

int main( int argc, char ** argv )
{
    int num_clients = 32, seconds = 5, result = 1;
    bool wrong_order = false;
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--wrong-order" ) == 0 ) wrong_order = true;
        else if ( strcmp( argv[i], "--clients" ) == 0 && i + 1 < argc ) num_clients = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--seconds" ) == 0 && i + 1 < argc ) seconds = atoi( argv[++i] );
        else seconds = 0;
    }
    if ( num_clients < 1 || num_clients > ROCKETNET_MAX_CLIENTS || seconds < 1 || seconds > 3600 )
        { printf( "usage: example_game_loop [--clients <1..1000>] [--seconds <1..3600>] [--wrong-order]\n" ); return 1; }

    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t ** clients = calloc( (size_t) num_clients, sizeof( clients[0] ) );
    if ( !clients || rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK ) { printf( "FAIL: init\n" ); return 1; }

    // the in-memory loopback transport holds one server and ROCKETNET_MAX_CLIENTS clients
    struct rocketnet_server_config_t server_config;
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, dev_backend_public_key, sizeof( server_config.backend_public_key ) );
    server_config.max_clients = num_clients;
    server_config.transport = "loopback";
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    if ( !server ) { printf( "FAIL: server create\n" ); goto cleanup; }
    printf( "%d clients, server transport %s\n", num_clients, rocketnet_server_transport_name( server ) );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) ) { printf( "FAIL: server address\n" ); goto cleanup; }
    rocketnet_server_public_key( server, server_public_key );
    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    for ( int i = 0; i < num_clients; i++ )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];       // a development token stamped with current Unix time
        enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
        const time_t token_time = time( NULL );
        if ( token_time == (time_t) -1 ) { printf( "FAIL: token clock\n" ); goto cleanup; }
        if ( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, (uint64_t) i, EXAMPLE_PROTOCOL_ID, (uint64_t) token_time, NULL, token ) != ROCKETNET_STATUS_OK ||
             !( clients[i] = rocketnet_client_create( NULL, &client_config, &client_status ) ) ||
             rocketnet_client_connect( clients[i], token ) != ROCKETNET_STATUS_OK )
            { printf( "FAIL: client %d create: %s\n", i, rocketnet_status_name( client_status ) ); goto cleanup; }
    }

    const uint32_t frames = (uint32_t) seconds * 60;
    uint64_t events_sent = 0, events_received = 0;
    double previous = rocketnet_time(), accumulator = 0.0;
    uint8_t * buffer;

    for ( uint32_t frame = 0; measure_from == UINT32_MAX || frame < measure_from + frames; frame++ )
    {
        for ( ;; )                                          // the fixed step: one frame per 1/60 s of real time
        {
            const double now = rocketnet_time();
            accumulator += now - previous;
            previous = now;
            if ( accumulator >= FRAME_DT ) break;
            rocketnet_sleep( FRAME_DT - accumulator );
        }
        accumulator -= FRAME_DT;
        const double time = 1000.0 + frame * FRAME_DT;
        const bool measuring = frame >= measure_from;
        int connected = 0;

        // each client's frame, timed: these calls are its network cost per frame
        for ( int i = 0; i < num_clients && frame * 50 >= (uint32_t) i; i++ )          // players join 50 a frame
        {
            struct rocketnet_client_t * client = clients[i];
            const double start = rocketnet_time();
            rocketnet_client_update( client, time );                                    // (1) update
            if ( wrong_order ) rocketnet_client_send_packets( client );                 // the mistake: sends last frame's queue
            const struct rocketnet_message_batch_t * incoming = NULL;                      // (2) receive the state updates,
            while ( ( incoming = rocketnet_client_receive_messages( client ) ) != NULL )   // a batch per call
                for ( int m = 0; m < incoming->num_messages; m++ )
                    read_message( &to_client, frame, incoming->data[m], incoming->bytes[m] );
            // (3) simulate: nothing moves in this example
            if ( measuring && ( buffer = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES ) ) )
                rocketnet_client_finish_message( client, write_message( buffer, frame ) );      // (4) queue
            if ( measuring && ( frame + (uint32_t) i ) % 30 == 0 && frame + 60 < measure_from + frames )
            {
                buffer = rocketnet_client_start_message( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, MESSAGE_BYTES );
                if ( !buffer || !rocketnet_client_finish_message( client, write_message( buffer, frame ) ) ) { printf( "FAIL: client %d event refused\n", i ); goto cleanup; }
                events_sent++;
            }
            if ( !wrong_order ) rocketnet_client_send_packets( client );                // (5) last, so this frame's messages leave this frame
            if ( measuring ) add_cost( ( rocketnet_time() - start ) * 1e6, false );
            connected += rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED;
        }

        // the server's frame, timed as one tick, in the same order
        const double start = rocketnet_time();
        rocketnet_server_update( server, time );                                        // (1) update
        if ( wrong_order ) rocketnet_server_send_packets( server );
        const struct rocketnet_message_batch_t * batch = NULL;                             // (2) receive positions and events:
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )   // one queue across every client,
            for ( int m = 0; m < batch->num_messages; m++ )                             // in batches, no call per message
                { read_message( &to_server, frame, batch->data[m], batch->bytes[m] ); events_received += batch->channel[m] == 1; }
        // (3) simulate: nothing moves in this example
        for ( int i = 0; measuring && i < num_clients; i++ )                            // (4) queue each client's state update
            if ( ( buffer = rocketnet_server_start_message( server, (uint64_t) i, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES ) ) )
                rocketnet_server_finish_message( server, write_message( buffer, frame ) );
        if ( !wrong_order ) rocketnet_server_send_packets( server );                    // (5) last, so this frame's messages leave this frame

        if ( measuring && connected != num_clients ) { printf( "FAIL: a client disconnected\n" ); goto cleanup; }
        if ( measuring ) add_cost( ( rocketnet_time() - start ) * 1e6, true );
        if ( measuring && ( frame - measure_from + 1 ) % 60 == 0 ) { print_costs( "[1s]", &costs[0], 60 ); memset( &costs[0], 0, sizeof( costs[0] ) ); }
        if ( !measuring && connected == num_clients ) measure_from = frame + 1;
        if ( !measuring && frame == 30 * 60 ) { printf( "FAIL: %d of %d clients connected in 30 s\n", connected, num_clients ); goto cleanup; }
    }

    // clients run their frames before the server's, so the server reads a
    // client's message the frame it was sent (0) and a client reads its
    // state update no earlier than the next frame (1). Client to server has no
    // receive handoff (the client sends in its call, then the server updates),
    // so it measures the frame-order cost exactly. Server to client passes a
    // background receive handoff, so its observed maximum is informational:
    // scheduler delay can span any number of game frames when catch-up frames
    // run back to back, but it cannot make an update arrive before this order
    // permits.
    if ( to_server.count == 0 || to_client.count == 0 ) { printf( "FAIL: no messages measured\n" ); goto cleanup; }
    print_costs( "[end]", &costs[1], frames );
    printf( "client frame budget %s at p99 (informational, never a failure)\n", percentile( &costs[1], 0.99 ) <= BUDGET_US ? "held" : "exceeded" );
    printf( "latency client->server frames: min %u mean %.2f max %u\n", to_server.min, to_server.sum / to_server.count, to_server.max );
    printf( "latency server->client frames: min %u mean %.2f max %u\n", to_client.min, to_client.sum / to_client.count, to_client.max );
    const int extra = (int) ( to_server.sum / to_server.count + 0.5 );
    const uint32_t order_frames = wrong_order ? 1u : 0u;
    printf( "latency: %d extra frame(s) (%s order)\n", extra, wrong_order ? "wrong" : "correct" );

    if ( events_received != events_sent ) printf( "FAIL: %llu of %llu events arrived\n", (unsigned long long) events_received, (unsigned long long) events_sent );
    else if ( to_server.min != order_frames || to_server.max != order_frames )
        printf( "FAIL: client->server latency range %u..%u, expected %u frame(s) in the %s order\n",
                to_server.min, to_server.max, order_frames, wrong_order ? "wrong" : "correct" );
    else if ( to_client.min < 1 + order_frames )
        printf( "FAIL: server->client state arrived in %u frame(s), before the %u-frame causal minimum\n",
                to_client.min, 1 + order_frames );
    else result = 0;

cleanup:
    for ( int i = 0; i < num_clients; i++ ) rn_cli_client_stop_and_destroy( clients[i] );
    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    free( clients );
    // CLI cleanup helpers wait for shutdown completion before destroy.
    // PASS is printed afterward so it stays the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
