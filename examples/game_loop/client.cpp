/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop client: the same 60 Hz frame loop as the server. Each frame
    starts the network step for what the last frame produced on its own
    thread, simulates this frame while it runs, then joins it and swaps the
    two buffers. The network step queues and sends the input the last frame
    wrote, then updates and takes the server's state. The simulation never
    calls RocketNet.
*/

#include "game.h"

#include <math.h>
#include <time.h>

// one side of the double buffer
struct client_frame_t
{
    bool send;                                  // written by the simulation, sent by the network step
    message_t input;
    int num_states;                             // taken by the network step, read by the simulation
    message_t states[16];
    bool connected;
};

struct client_t
{
    rocketnet_client_t * client;
    client_args_t * args;
    int64_t state_taken;                        // the newest state frame taken
    bool failed;
};

static void client_network_step( client_t * c, client_frame_t * f, double time )
{
    const double start = rocketnet_time();
    wire_t * wire = c->args->wire;

    // queue the input the last frame wrote, then send it
    if ( f->send )
    {
        uint8_t * buffer = rocketnet_client_start_message( c->client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES );
        if ( buffer )
        {
            write_message( buffer, f->input );
            rocketnet_client_finish_message( c->client, MESSAGE_BYTES );
        }
        else
            f->send = false;
    }
    rocketnet_client_send_packets( c->client );
    if ( wire && f->send )
        wire->input_sent[c->args->number] = f->input.frame;

    // what the server had sent this client when this update began
    const int64_t visible = wire ? wire->state_sent[c->args->number].load() : -1;

    // update takes in what arrived; receive takes every message, in batches
    rocketnet_client_update( c->client, time );
    f->num_states = 0;
    const rocketnet_message_batch_t * batch;
    while ( ( batch = rocketnet_client_receive_messages( c->client ) ) != NULL )
    {
        for ( int m = 0; m < batch->num_messages; m++ )
        {
            message_t state;
            if ( !read_message( batch->data[m], batch->bytes[m], state ) || state.sender != (uint32_t) c->args->number )
                continue;
            if ( (int64_t) state.frame > c->state_taken )
                c->state_taken = state.frame;
            if ( f->num_states < 16 )
                f->states[f->num_states++] = state;
            c->args->states++;
        }
    }

    // every state sent before this update began was taken by it
    if ( visible > c->state_taken )
        wire->late++;

    const rocketnet_client_state_t state = rocketnet_client_state( c->client );
    f->connected = state == ROCKETNET_CLIENT_STATE_CONNECTED;
    c->failed = state < ROCKETNET_CLIENT_STATE_DISCONNECTED;
    add_cost( c->args->cost, rocketnet_time() - start );
}

// the game's frame: read the server's state the network step brought, and
// write this frame's input for the next network step to send
static void client_simulate( client_t * c, client_frame_t * f, uint32_t frame )
{
    (void) c;
    f->send = f->connected;
    f->input.sender = (uint32_t) c->args->number;
    f->input.frame = frame;
    f->input.value = (float) sin( frame * 0.1 );
}

void client_main( client_args_t * args )
{
    args->result = 1;

    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = args->transport;
    rocketnet_status_t status = ROCKETNET_STATUS_OK;
    client_t * c = new client_t();
    c->args = args;
    c->state_taken = -1;
    c->client = rocketnet_client_create( NULL, &config, &status );
    if ( !c->client )
    {
        printf( "FAIL: client %d create: %s\n", args->number, rocketnet_status_name( status ) );
        delete c;
        return;
    }

    // a development token for this server, stamped now, as a backend mints it
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( rocketnet_create_dev_connect_token( args->server_address, NULL, args->server_public_key, 30, 10, (uint64_t) args->number + 1,
                                             EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, token ) != ROCKETNET_STATUS_OK ||
         rocketnet_client_connect( c->client, token ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client %d connect\n", args->number );
        rn_cli_client_stop_and_destroy( c->client );
        delete c;
        return;
    }

    client_frame_t * frames = new client_frame_t[2]();
    int current = 0;
    double connected_at = 0.0;
    const double give_up = rocketnet_time() + 30.0;
    double frame_end = rocketnet_time() + FRAME_DT;
    for ( uint32_t frame = 0; rocketnet_time() < give_up; frame++ )
    {
        // the network step for what the last frame produced runs on its own
        // thread while this frame simulates
        std::thread network( client_network_step, c, &frames[current ^ 1], rocketnet_time() );
        client_simulate( c, &frames[current], frame );
        network.join();
        current ^= 1;

        if ( c->failed )
            break;
        if ( frames[current].connected && connected_at == 0.0 )
            connected_at = rocketnet_time();
        if ( connected_at > 0.0 && rocketnet_time() - connected_at >= args->seconds )
        {
            args->result = args->states > 0 ? 0 : 1;
            break;
        }

        wait_for_frame_end( frame_end );
    }

    if ( args->wire )
        args->wire->leaving[args->number] = true;
    rn_cli_client_stop_and_destroy( c->client );
    delete[] frames;
    delete c;
}
