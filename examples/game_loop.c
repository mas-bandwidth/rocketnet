/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop: a 60 Hz fixed-step loop driving one server and N clients in one
    process, each frame in the order a game runs it: update, receive, simulate,
    queue, then send last. Each client sends its position every frame and an
    event twice a second; the server sends each client a snapshot every frame.
    Messages carry the frame they were queued in, so receivers measure latency
    in frames. Each client's network calls per frame are timed against a 100
    microsecond budget, and the server's tick too; the budget is printed, never
    failed on, because shared machines are noisy. --wrong-order sends before
    queuing, so every message waits one more frame.

    Run it from the repository root: ./bin/example_game_loop
    [--clients <1..1000>] [--seconds <1..3600>] [--wrong-order]
*/

#include "rocketnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// The header at this base declares start, finish, receive, release and
// send_packets; the lifecycle rename makes them *_start_message and friends.

#define FRAME_DT ( 1.0 / 60.0 )
#define BUDGET_US 100.0
#define BINS 10000                                          // 0.1 us bins up to 1 ms, then one overflow bin

typedef struct { uint32_t frame; float x, y; } message_t;   // the frame it was queued in, and a position
typedef struct { uint32_t bins[BINS + 1]; uint64_t count; double max, tick_sum, tick_max; } costs_t;
typedef struct { double sum; uint64_t count; uint32_t min, max; } latency_t;

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
    uint64_t seen = 0;
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

// fill a buffer that start returned, or note the latency of a message that arrived
static void write_message( uint8_t * buffer, uint32_t frame ) { message_t m = { frame, 1.0f, 2.0f }; memcpy( buffer, &m, sizeof( m ) ); }

static void read_message( latency_t * l, uint32_t frame, const uint8_t * data, int bytes )
{
    message_t m;
    if ( bytes != (int) sizeof( m ) ) return;
    memcpy( &m, data, sizeof( m ) );
    const uint32_t frames = frame - m.frame;                // received minus queued: 0 is the same frame
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
    if ( !clients || rocketnet_init( NULL ) != ROCKETNET_STATUS_OK ) { printf( "FAIL: init\n" ); return 1; }

    // the in-memory loopback transport holds ROCKETNET_LOOPBACK_MAX_ENDPOINTS
    // endpoints: one server and ROCKETNET_MAX_CLIENTS clients
    struct rocketnet_server_config_t server_config;
    rocketnet_server_dev_config( &server_config );
    server_config.max_clients = num_clients;
    server_config.transport = "loopback";
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    if ( !server ) { printf( "FAIL: server create\n" ); goto cleanup; }
    printf( "%d clients, server transport %s\n", num_clients, rocketnet_server_transport_name( server ) );

    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    for ( int i = 0; i < num_clients; i++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];       // a development token stamped far ahead, as in hello
        enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
        if ( rocketnet_write_dev_connect_token( server_address, (uint64_t) i, 4102444800ull, token, ROCKETNET_CONNECT_TOKEN_BYTES ) != ROCKETNET_STATUS_OK ||
             !( clients[i] = rocketnet_client_create( NULL, &client_config, NULL, 0, &client_status ) ) ||
             rocketnet_client_connect( clients[i], token ) != ROCKETNET_STATUS_OK )
            { printf( "FAIL: client %d create: %s\n", i, rocketnet_status_name( client_status ) ); goto cleanup; }
    }

    const uint32_t frames = (uint32_t) seconds * 60;
    uint64_t events_sent = 0, events_received = 0;
    double previous = rocketnet_time(), accumulator = 0.0;
    const uint8_t * data;
    int bytes;
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
            struct rocketnet_message_batch_t incoming;                      // (2) receive the snapshots,
            while ( rocketnet_client_receive_messages( client, &incoming ) )   // a batch per call
                for ( int m = 0; m < incoming.num_messages; m++ )
                    read_message( &to_client, frame, incoming.data[m], incoming.bytes[m] );
            // (3) simulate: nothing moves in this example
            if ( measuring && ( buffer = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, sizeof( message_t ) ) ) )
                { write_message( buffer, frame ); rocketnet_client_finish_message( client, sizeof( message_t ) ); }      // (4) queue
            if ( measuring && ( frame + i ) % 30 == 0 && frame + 60 < measure_from + frames )
            {
                if ( ( buffer = rocketnet_client_start_message( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, sizeof( message_t ) ) ) ) write_message( buffer, frame );
                if ( !buffer || !rocketnet_client_finish_message( client, sizeof( message_t ) ) ) { printf( "FAIL: client %d event refused\n", i ); goto cleanup; }
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
        struct rocketnet_message_batch_t batch;                             // (2) receive positions and events:
        while ( rocketnet_server_receive_messages( server, &batch ) )   // one queue across every client,
            for ( int m = 0; m < batch.num_messages; m++ )                             // in batches, no call per message
                { read_message( &to_server, frame, batch.data[m], batch.bytes[m] ); events_received += batch.channel[m] == 1; }
        // (3) simulate: nothing moves in this example
        for ( int i = 0; measuring && i < num_clients; i++ )                            // (4) queue each client's snapshot
            if ( ( buffer = rocketnet_server_start_message( server, i, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, sizeof( message_t ) ) ) )
                { write_message( buffer, frame ); rocketnet_server_finish_message( server, i, sizeof( message_t ) ); }
        if ( !wrong_order ) rocketnet_server_send_packets( server );                    // (5) last, so this frame's messages leave this frame

        if ( measuring && connected != num_clients ) { printf( "FAIL: a client disconnected\n" ); goto cleanup; }
        if ( measuring ) add_cost( ( rocketnet_time() - start ) * 1e6, true );
        if ( measuring && ( frame - measure_from + 1 ) % 60 == 0 ) { print_costs( "[1s]", &costs[0], 60 ); memset( &costs[0], 0, sizeof( costs[0] ) ); }
        if ( !measuring && connected == num_clients ) measure_from = frame + 1;
        if ( !measuring && frame == 30 * 60 ) { printf( "FAIL: %d of %d clients connected in 30 s\n", connected, num_clients ); goto cleanup; }
    }

    // clients run their frames before the server's, so the server reads a
    // client's message the frame it was sent (0) and a client reads its
    // snapshot the next frame (1): the extra latency is what the order adds
    if ( to_server.count == 0 || to_client.count == 0 ) { printf( "FAIL: no messages measured\n" ); goto cleanup; }
    print_costs( "[end]", &costs[1], frames );
    printf( "client frame budget %s at p99 (informational, never a failure)\n", percentile( &costs[1], 0.99 ) <= BUDGET_US ? "held" : "exceeded" );
    printf( "latency client->server frames: min %u mean %.2f max %u\n", to_server.min, to_server.sum / to_server.count, to_server.max );
    printf( "latency server->client frames: min %u mean %.2f max %u\n", to_client.min, to_client.sum / to_client.count, to_client.max );
    const double up = to_server.sum / to_server.count, down = to_client.sum / to_client.count - 1.0;   // less the snapshot's one frame
    const int extra = (int) ( ( up > down ? up : down ) + 0.5 );
    printf( "latency: %d extra frame(s) (%s order)\n", extra, wrong_order ? "wrong" : "correct" );

    if ( events_received != events_sent ) printf( "FAIL: %llu of %llu events arrived\n", (unsigned long long) events_received, (unsigned long long) events_sent );
    else if ( wrong_order ? extra < 1 : extra != 0 ) printf( "FAIL: %d extra frame(s) in the %s order\n", extra, wrong_order ? "wrong" : "correct" );
    else result = 0;

cleanup:
    for ( int i = 0; i < num_clients; i++ ) rocketnet_client_destroy( clients[i] );
    rocketnet_server_destroy( server );
    rocketnet_term();
    free( clients );
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
