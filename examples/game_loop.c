/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop: a 60 Hz fixed-step loop driving one server and N clients over
    loopback. Each client's frame runs in the order a client runs it: update,
    receive, simulate, queue, then send last. The server's frame runs the way
    a game server runs it: join last frame's network work, a bit of
    networking on the main thread (send what last frame queued, update,
    receive), the world update, then the second half of the network update
    (queue every client's state update) split off to run on another thread
    in parallel with the main thread, joined at the start of the next frame.
    The state updates a server frame queues go on the wire at the start of
    the next frame: one frame of added send latency, 16.67 ms on average.

    Clients send a position every frame and an event twice a second; the
    server sends each a state update every frame. Each message carries its
    frame, so receivers measure latency in frames. Client network calls per
    frame are timed against a 100 microsecond budget, which is printed, never
    failed on. --wrong-order makes the clients send before queuing.

    Run: ./bin/example_game_loop [--clients <1..1000>] [--seconds <1..3600>] [--wrong-order]
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_DT        ( 1.0 / 60.0 )
#define BUDGET_US       100.0
#define BINS            10000       // 0.1 us bins up to 1 ms, then one overflow bin
#define MESSAGE_BYTES   12          // the frame it was queued in, and a position (two floats)

struct latency_t
{
    double sum;
    uint32_t count, min, max;
};

static struct latency_t to_server, to_client;
static uint32_t bins[BINS + 1], count;      // the client frame costs
static double max_us;
static double main_sum_us, main_max_us;     // the server's main thread in rocketnet: join, send, update, receive
static double split_sum_us, split_max_us;   // the server's split-off half: queue every state update

static void add_client_cost( double us )
{
    bins[us * 10.0 < BINS ? (int) ( us * 10.0 ) : BINS]++;
    count++;
    max_us = us > max_us ? us : max_us;
}

static double percentile( double p )
{
    uint32_t seen = 0;

    for ( int bin = 0; bin < BINS; bin++ )
    {
        if ( ( seen += bins[bin] ) > 0 && seen >= p * count )
        {
            return bin / 10.0;
        }
    }

    return max_us;
}

// A payload's integers are written and read one byte at a time, in one
// byte order (little-endian here), so a client and a server of different
// byte orders read the same numbers. Copying a uint32_t into a payload with
// memcpy would send the host's byte order.

static void write_u32( uint8_t * p, uint32_t value )
{
    p[0] = (uint8_t) ( value & 0xff );
    p[1] = (uint8_t) ( ( value >> 8 ) & 0xff );
    p[2] = (uint8_t) ( ( value >> 16 ) & 0xff );
    p[3] = (uint8_t) ( ( value >> 24 ) & 0xff );
}

static uint32_t read_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

// a float goes as the u32 of its bits
static void write_f32( uint8_t * p, float value )
{
    uint32_t bits;
    memcpy( &bits, &value, 4 );
    write_u32( p, bits );
}

// a message is the frame it was queued in, then a position
static int write_message( uint8_t * p, uint32_t frame )
{
    write_u32( p, frame );
    write_f32( p + 4, 1.0f );
    write_f32( p + 8, 2.0f );
    return MESSAGE_BYTES;
}

static void read_message( struct latency_t * l, uint32_t frame, const uint8_t * data )
{
    const uint32_t queued = read_u32( data );

    const uint32_t frames = frame - queued;     // 0 is the frame it was queued in
    l->min = l->count == 0 || frames < l->min ? frames : l->min;
    l->max = frames > l->max ? frames : l->max;
    l->sum += frames;
    l->count++;
}

// One server frame of the game's state: what the world update wrote for the
// second half of the network update to send. The server keeps two and swaps
// them at the frame boundary, so the main thread never writes the frame the
// other thread is sending from.

struct server_frame_t
{
    struct rocketnet_server_t * server;
    int num_clients;
    uint32_t frame;     // every client's state update this frame carries its frame number
    int send;           // 0 until every client is connected and the run is measured
    double queue_us;    // written by the other thread, read by the main thread after the join
};

// The second half of the server's network update, split off from the main
// thread: queue every client's state update. From the split until the join,
// this thread is the server's only user.

static void queue_state_updates( void * data )
{
    struct server_frame_t * f = (struct server_frame_t *) data;
    const double start = rocketnet_time();

    for ( int i = 0; f->send && i < f->num_clients; i++ )
    {
        if ( rocketnet_server_can_send_message( f->server, i, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES ) )
        {
            uint8_t * p = rocketnet_server_start_message( f->server, (uint64_t) i, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES );
            if ( p )
            {
                const uint32_t serial = rocketnet_server_finish_message( f->server, write_message( p, f->frame ) );
                (void) serial;
            }
        }
    }

    f->queue_us = ( rocketnet_time() - start ) * 1e6;
}

// a whole decimal argument, or -1 for anything else (a sign, no digits,
// trailing characters, more than a million), which the range check refuses

static int parse_whole( const char * text )
{
    char * end = NULL;
    const long parsed = strtol( text, &end, 10 );
    return text[0] >= '0' && text[0] <= '9' && *end == '\0' && parsed <= 1000000 ? (int) parsed : -1;
}

int main( int argc, char ** argv )
{
    int num_clients = 32, seconds = 5, wrong_order = 0;

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--wrong-order" ) == 0 )
        {
            wrong_order = 1;
        }
        else if ( strcmp( argv[i], "--clients" ) == 0 && i + 1 < argc )
        {
            num_clients = parse_whole( argv[++i] );
        }
        else if ( strcmp( argv[i], "--seconds" ) == 0 && i + 1 < argc )
        {
            seconds = parse_whole( argv[++i] );
        }
        else
        {
            seconds = 0;
        }
    }

    if ( num_clients < 1 || num_clients > ROCKETNET_MAX_CLIENTS || seconds < 1 || seconds > 3600 )
    {
        printf( "usage: example_game_loop [--clients <1..1000>] [--seconds <1..3600>] [--wrong-order]\n" );
        return 1;
    }

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the in-memory loopback transport keeps its endpoints in a hub the caller
    // owns, sized here for one server and num_clients clients; it is destroyed after them

    enum rocketnet_status_t hub_status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, num_clients, &hub_status );
    if ( !hub || hub_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: loopback hub create: %s\n", rocketnet_status_name( hub_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &server_config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.max_clients = num_clients;
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
    {
        server_status = rocketnet_server_start( server );
    }

    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( server_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    printf( "%d clients, server transport %s\n", num_clients, rocketnet_server_transport_name( server ) );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    rocketnet_server_public_key( server, server_public_key );

    struct rocketnet_client_config_t client_config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &client_config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    client_config.transport = "loopback";
    client_config.loopback_hub = hub;

    struct rocketnet_client_t ** clients = calloc( (size_t) num_clients, sizeof( clients[0] ) );
    if ( !clients )
    {
        printf( "FAIL: client array: out of memory\n" );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    for ( int i = 0; i < num_clients; i++ )
    {
        char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, (uint64_t) i, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );
        enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
        clients[i] = rocketnet_client_create( NULL, &client_config, &client_status );
        if ( token_status == ROCKETNET_STATUS_OK && clients[i] && client_status == ROCKETNET_STATUS_OK )
        {
            client_status = rocketnet_client_connect( clients[i], connect_token );
        }

        if ( token_status != ROCKETNET_STATUS_OK || !clients[i] || client_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: client %d create/connect: %s\n", i, rocketnet_status_name( token_status != ROCKETNET_STATUS_OK ? token_status : client_status ) );
            for ( int j = 0; j <= i; j++ )
            {
                rn_cli_client_stop_and_destroy( clients[j], 1000.0 );
            }

            free( clients );
            rn_cli_server_stop_and_destroy( server );

            rn_cli_loopback_hub_destroy( hub );
            const enum rocketnet_status_t term_status = rocketnet_term();
            if ( term_status != ROCKETNET_STATUS_OK )
            {
                printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
            }

            return 1;
        }
    }

    const uint32_t frames = (uint32_t) seconds * 60;
    uint32_t measure_from = UINT32_MAX;     // the first frame with every client connected
    uint64_t events_sent = 0, events_received = 0;
    double previous = rocketnet_time(), accumulator = 0.0;
    double last_now = 1000.0;     // the last frame's game time, for the shutdown's updates
    const char * failure = NULL;

    static struct server_frame_t server_frames[2];
    struct server_frame_t * split_off = NULL;     // the frame the other thread is sending from, until the join
    struct rn_cli_thread_t network;
    memset( &network, 0, sizeof( network ) );

    for ( uint32_t frame = 0; !failure && ( measure_from == UINT32_MAX || frame < measure_from + frames ); frame++ )
    {
        // the fixed step: one frame per 1/60 s of real time

        for ( ;; )
        {
            const double t = rocketnet_time();
            accumulator += t - previous;
            previous = t;
            if ( accumulator >= FRAME_DT )
            {
                break;
            }
            rocketnet_sleep( FRAME_DT - accumulator );
        }

        accumulator -= FRAME_DT;

        const double now = 1000.0 + frame * FRAME_DT;
        last_now = now;
        const int measuring = frame >= measure_from;
        int connected = 0;

        // each client's frame, timed: these calls are its network cost per frame. Players join 50 a frame

        for ( int i = 0; i < num_clients && frame * 50 >= (uint32_t) i; i++ )
        {
            struct rocketnet_client_t * client = clients[i];
            const double start = rocketnet_time();
            rocketnet_client_update( client, now );                                             // (1) update
            if ( wrong_order )
            {
                rocketnet_client_send_packets( client );                                        // the mistake: sends last frame's queue
            }

            const struct rocketnet_message_batch_t * batch;                                     // (2) receive the state updates
            while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
            {
                for ( int m = 0; m < batch->num_messages; m++ )
                {
                    read_message( &to_client, frame, batch->data[m] );
                }
            }

            // (3) simulate: nothing moves in this example
            // replaceable position: skip this frame when there is no room

            if ( measuring && rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES ) )
            {
                uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES );
                if ( p )
                {
                    const uint32_t serial = rocketnet_client_finish_message( client, write_message( p, frame ) );          // (4) queue a position,
                    (void) serial;
                }
            }

            if ( measuring && ( frame + (uint32_t) i ) % 30 == 0 && frame + 60 < measure_from + frames )
            {
                // reliable event: fail explicitly when there is no room, rather than spin

                if ( !rocketnet_client_can_send_message( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, MESSAGE_BYTES ) )
                {
                    failure = "a client event was refused";
                }
                else
                {
                    uint8_t * p = rocketnet_client_start_message( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, MESSAGE_BYTES );
                    if ( p )
                    {
                        if ( !rocketnet_client_finish_message( client, write_message( p, frame ) ) )    // and an event
                        {
                            failure = "a client event was refused";
                        }
                    }
                    else
                    {
                        failure = "a client event was refused";
                    }
                }

                events_sent++;
            }

            if ( !wrong_order )
            {
                rocketnet_client_send_packets( client );                                        // (5) send last
            }

            if ( measuring )
            {
                add_client_cost( ( rocketnet_time() - start ) * 1e6 );
            }

            connected += rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED;
        }

        // the server's frame, on the main thread. Steps 1 and 2 are what
        // rocketnet takes from the main thread, timed

        const double start = rocketnet_time();

        // 1. join: wait for last frame's second half of the network update.
        // The main thread owns the server again from here

        rn_cli_thread_join( &network );
        if ( split_off )
        {
            if ( split_off->send )
            {
                split_sum_us += split_off->queue_us;
                split_max_us = split_off->queue_us > split_max_us ? split_off->queue_us : split_max_us;
            }
            split_off = NULL;
        }

        // 2. a bit of networking on the main thread: put last frame's state
        // updates on the wire, take in what arrived, and read each message
        // into the game's state before the next update

        rocketnet_server_send_packets( server );
        rocketnet_server_update( server, now );

        const struct rocketnet_message_batch_t * batch;         // one queue across every client
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                read_message( &to_server, frame, batch->data[m] );
                events_received += batch->channel[m] == 1;
            }
        }

        const double main_us = ( rocketnet_time() - start ) * 1e6;

        // 3. the world update, on the main thread: nothing moves in this
        // example, so this frame's state is each client's state update,
        // stamped with the frame

        struct server_frame_t * f = &server_frames[frame % 2];
        f->server = server;
        f->num_clients = num_clients;
        f->frame = frame;
        f->send = measuring;
        f->queue_us = 0.0;

        // 4. split off the second half of the network update: another thread
        // queues this frame's state updates in parallel with the main thread,
        // which must not touch the server until the join next frame

        if ( !rn_cli_thread_start( &network, queue_state_updates, f ) )
        {
            failure = "could not start a thread";
        }
        else
        {
            split_off = f;
        }

        // 5. the main thread goes on: the frame edge, then next frame's clients

        if ( measuring )
        {
            main_sum_us += main_us;
            main_max_us = main_us > main_max_us ? main_us : main_max_us;

            if ( connected != num_clients )
            {
                failure = "a client disconnected";
            }
        }
        else if ( connected == num_clients )
        {
            measure_from = frame + 1;
        }
        else if ( frame == 30 * 60 )
        {
            failure = "the clients did not all connect in 30 s";
        }
    }

    // the server's last split-off half finishes before its server is destroyed

    rn_cli_thread_join( &network );
    if ( split_off && split_off->send )
    {
        split_sum_us += split_off->queue_us;
        split_max_us = split_off->queue_us > split_max_us ? split_off->queue_us : split_max_us;
    }

    // clients run their frames before the server's, so the server reads a
    // client's message no earlier than the frame it was sent (0). A state
    // update the server's frame F queues goes on the wire at the start of
    // server frame F + 1, after that frame's clients have run, so a client
    // reads it no earlier than frame F + 2 (2): one frame of that is the
    // added send latency of splitting off the second half of the network
    // update. Both directions also pass through a background handoff (the
    // client never blocks on its send), which can add frames: that cost is
    // reported, not capped. The clients' wrong order holds each client
    // message one frame more, so it raises the client-to-server minimum by
    // one; telling it apart exactly would need same-frame delivery, which the
    // handoff does not promise, so the check is the minimum and the printed
    // latency shows the rest

    if ( !failure && ( to_server.count == 0 || to_client.count == 0 ) )
    {
        failure = "no messages measured";
    }

    const uint32_t order_frames = wrong_order ? 1u : 0u;

    if ( !failure )
    {
        printf( "[end] client frame p50 %.1f us, p99 %.1f us, max %.1f us (budget %.0f us)\n",
                percentile( 0.5 ), percentile( 0.99 ), max_us, BUDGET_US );
        printf( "server main thread in rocketnet mean %.1f us, max %.1f us; split-off half mean %.1f us, max %.1f us\n",
                main_sum_us / frames, main_max_us, split_sum_us / frames, split_max_us );
        printf( "client frame budget %s at p99 (informational, never a failure)\n", percentile( 0.99 ) <= BUDGET_US ? "held" : "exceeded" );
        printf( "latency client->server frames: min %u mean %.2f max %u\n", to_server.min, to_server.sum / to_server.count, to_server.max );
        printf( "latency server->client frames: min %u mean %.2f max %u\n", to_client.min, to_client.sum / to_client.count, to_client.max );
        printf( "latency: %d extra frame(s) (%s order)\n", (int) ( to_server.sum / to_server.count + 0.5 ), wrong_order ? "wrong" : "correct" );

        if ( events_received != events_sent )
        {
            failure = "an event did not arrive";
        }
        else if ( to_server.min < order_frames )
        {
            failure = "a client message arrived before its causal minimum";
        }
        else if ( to_client.min < 2 )
        {
            failure = "a state update arrived before its causal minimum";
        }
    }

    for ( int i = 0; i < num_clients; i++ )
    {
        rn_cli_client_stop_and_destroy( clients[i], last_now );
    }

    free( clients );
    rn_cli_server_stop_and_destroy( server );

    rn_cli_loopback_hub_destroy( hub );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    if ( failure )
    {
        printf( "FAIL: %s\n", failure );
        return 1;
    }

    printf( "PASS\n" );

    return 0;
}
