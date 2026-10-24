/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop server: one 60 Hz frame loop. Each frame starts the network step
    for what the last frame produced on its own thread, simulates this frame
    while it runs, then joins it and swaps the two buffers. The network step
    queues and sends the state updates the last frame wrote, then updates and
    takes every client's input. The simulation never calls RocketNet.
*/

#include "game.h"

// one side of the double buffer
struct server_frame_t
{
    int num_states;                             // written by the simulation, sent by the network step
    message_t states[MAX_CLIENTS];
    uint64_t state_slot[MAX_CLIENTS];
    int num_inputs;                             // taken by the network step, read by the simulation
    message_t inputs[4 * MAX_CLIENTS];
    uint64_t input_slot[4 * MAX_CLIENTS];
    int num_connected;
};

struct server_t
{
    rocketnet_server_t * server;
    server_args_t * args;
    int64_t input_taken[MAX_CLIENTS];           // the newest input frame taken from each client
    bool known[MAX_CLIENTS];                    // the simulation's players, by client number
    uint64_t slot[MAX_CLIENTS];
    float value[MAX_CLIENTS];
};

static void server_network_step( server_t * s, server_frame_t * f, double time )
{
    const double start = rocketnet_time();
    wire_t * wire = s->args->wire;

    // queue the state updates the last frame wrote, then send them
    for ( int i = 0; i < f->num_states; i++ )
    {
        uint8_t * buffer = rocketnet_server_start_message( s->server, f->state_slot[i], 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, MESSAGE_BYTES );
        if ( !buffer )
            continue;
        write_message( buffer, f->states[i] );
        rocketnet_server_finish_message( s->server, MESSAGE_BYTES );
    }
    rocketnet_server_send_packets( s->server );
    if ( wire )
        for ( int i = 0; i < f->num_states; i++ )
            wire->state_sent[f->states[i].sender] = f->states[i].frame;

    // what every client had sent when this update began
    int64_t visible[MAX_CLIENTS];
    for ( int i = 0; wire && i < MAX_CLIENTS; i++ )
        visible[i] = wire->input_sent[i];

    // update takes in what arrived; receive takes every message, in batches
    rocketnet_server_update( s->server, time );
    f->num_inputs = 0;
    const rocketnet_message_batch_t * batch;
    while ( ( batch = rocketnet_server_receive_messages( s->server ) ) != NULL )
    {
        for ( int m = 0; m < batch->num_messages; m++ )
        {
            message_t input;
            if ( !read_message( batch->data[m], batch->bytes[m], input ) )
                continue;
            if ( input.frame > s->input_taken[input.sender] || s->input_taken[input.sender] < 0 )
                s->input_taken[input.sender] = input.frame;
            if ( f->num_inputs < 4 * MAX_CLIENTS )
            {
                f->inputs[f->num_inputs] = input;
                f->input_slot[f->num_inputs] = (uint64_t) batch->client_index[m];
                f->num_inputs++;
            }
            s->args->inputs++;
        }
    }

    // every input sent before this update began was taken by it
    for ( int i = 0; wire && i < MAX_CLIENTS; i++ )
        if ( visible[i] > s->input_taken[i] && !wire->leaving[i] )
            wire->late++;

    f->num_connected = rocketnet_server_num_connected_clients( s->server );
    add_cost( s->args->cost, rocketnet_time() - start );
}

// the game's frame: take the inputs the network step brought, move every
// player, and write each one's state for the next network step to send
static void server_simulate( server_t * s, server_frame_t * f, uint32_t frame )
{
    for ( int i = 0; i < f->num_inputs; i++ )
    {
        const message_t & input = f->inputs[i];
        s->known[input.sender] = true;
        s->slot[input.sender] = f->input_slot[i];
        s->value[input.sender] = input.value;
    }
    f->num_states = 0;
    for ( uint32_t i = 0; i < MAX_CLIENTS; i++ )
    {
        if ( !s->known[i] )
            continue;
        message_t state = { i, frame, s->value[i] * 2.0f };
        f->states[f->num_states] = state;
        f->state_slot[f->num_states] = s->slot[i];
        f->num_states++;
    }
}

void server_main( server_args_t * args )
{
    static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

    // the published development backend key (rocketnet.h) and the examples
    // protocol id (dev_keys.h); the server makes its own keypair at create
    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    config.max_clients = args->max_clients;
    config.transport = args->transport;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );

    static server_t s;
    memset( &s, 0, sizeof( s ) );
    s.args = args;
    for ( int i = 0; i < MAX_CLIENTS; i++ )
        s.input_taken[i] = -1;
    rocketnet_status_t status = ROCKETNET_STATUS_OK;
    s.server = rocketnet_server_create( NULL, args->address, NULL, &config, &status );
    if ( !s.server || rocketnet_server_start( s.server ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server create: %s\n", rocketnet_status_name( status ) );
        rn_cli_server_stop_and_destroy( s.server );
        args->ready = -1;
        return;
    }
    args->port = rocketnet_server_port( s.server );
    rocketnet_server_public_key( s.server, args->public_key );
    printf( "server listening on port %d, transport %s\n", (int) args->port, rocketnet_server_transport_name( s.server ) );
    args->ready = 1;

    // over UDP the server runs until every client it served has left
    static server_frame_t frames[2];
    memset( frames, 0, sizeof( frames ) );
    int current = 0;
    const double give_up = rocketnet_time() + 120.0;
    double frame_end = rocketnet_time() + FRAME_DT;
    for ( uint32_t frame = 0; !*args->quit && rocketnet_time() < give_up; frame++ )
    {
        // the network step for what the last frame produced runs on its own
        // thread while this frame simulates
        std::thread network( server_network_step, &s, &frames[current ^ 1], rocketnet_time() );
        server_simulate( &s, &frames[current], frame );
        network.join();
        current ^= 1;

        const int connected = frames[current].num_connected;
        args->clients_seen = connected > args->clients_seen ? connected : args->clients_seen;
        if ( !args->wire && args->clients_seen > 0 && connected == 0 )
            break;

        wait_for_frame_end( frame_end );
    }

    rn_cli_server_stop_and_destroy( s.server );
    args->result = args->inputs > 0 ? 0 : 1;
}
