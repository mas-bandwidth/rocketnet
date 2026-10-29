/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The shared world for the 1000-client server-stats cases: test_server_stats_*
// in tests/test_unit_server_stats_no_worker.cpp (unit) and
// functional/process_thread_counts.cpp (functional). The server runs on the
// in-memory loopback transport with development-only step-driven clients and a test clock
// starting at 1000.0; no socket, no sleep and no real time. The includer
// defines `check` before including.

#pragma once

#ifndef ROCKETNET_TESTS_NO_WORKER_WORLD_H
#define ROCKETNET_TESTS_NO_WORKER_WORLD_H

#include "unit_shutdown.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "loopback_hub.h"

#define NO_WORKER_PROTOCOL     0x5747A75D0C1E4A11ULL
#define NO_WORKER_CLIENTS      1000
#define NO_WORKER_CHANNEL      0

static double no_worker_now = 1000.0;
static double no_worker_clock( void * ) { return no_worker_now; }

struct no_worker_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_server_t * server;
    rocketnet_client_t * clients[NO_WORKER_CLIENTS];
    int num_clients;
    double time;
};

static no_worker_world_t g_no_worker_world;

// The smallest message tables both ends accept, so a thousand clients and a
// thousand-slot server fit the suite's resident-set bound: one 8-byte
// unreliable message each way per update needs no more.

static void no_worker_message_config( rocketnet_server_config_t * server, rocketnet_client_config_t * client )
{
    if ( server )
    {
        server->num_channels = 1;
        server->max_fragmented_messages = 1;
        server->reliable_messages_in_flight = 64;
        server->packets_in_flight = 64;                    // the sent map holds the window, no more
        server->receive_message_buffer_bytes = 4096;
        server->max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
        server->send_message_buffer_bytes = 12288;
        server->send_queue_messages = 64;
    }

    if ( client )
    {
        client->num_channels = 1;
        client->max_fragmented_messages = 1;
        client->reliable_messages_in_flight = 64;
        client->packets_in_flight = 64;                    // the sent map holds the window, no more
        client->receive_message_buffer_bytes = 4096;
        client->max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
        client->send_message_buffer_bytes = 12288;
        client->send_queue_messages = 16;
    }
}

static void no_worker_server_create( no_worker_world_t * world, int max_clients )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    no_worker_now = world->time;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( world->server_public, world->server_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.protocol_id = NO_WORKER_PROTOCOL;
    config.max_clients = max_clients;
    config.transport = "loopback";
    rocketnet_test_loopback_hub_clients( max_clients );
    config.loopback_hub = rocketnet_test_loopback_hub();
    no_worker_message_config( &config, NULL );
    memcpy( config.backend_public_key, world->backend_public, sizeof( config.backend_public_key ) );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    check( world->server != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_server_public_key( world->server, world->server_public );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );
}

static void no_worker_clients_create( no_worker_world_t * world, int num_clients )
{
    world->num_clients = num_clients;

    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_test_config_t config;
        check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
        config.transport = "loopback";
        config.loopback_hub = rocketnet_test_loopback_hub();
        config.step_driven = true;
        config.time_callback = no_worker_clock;
        no_worker_message_config( NULL, &config );

        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        world->clients[i] = rocketnet_client_create( NULL, &config, &status );
        check( world->clients[i] != NULL && status == ROCKETNET_STATUS_OK );
    }
}

static void no_worker_server_stop_and_wait( no_worker_world_t * world )
{
    rocketnet_server_stop( world->server );

    // poll the completion query within a fixed budget; do not sleep

    for ( int step = 0; step < 32 && !rocketnet_server_stopped( world->server ); step++ )
    {
    }

    check( rocketnet_server_stopped( world->server ) );
}

static void no_worker_world_destroy( no_worker_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rn_test_client_stop_and_destroy( world->clients[i] );
    }
    no_worker_server_stop_and_wait( world );
    rocketnet_server_destroy( world->server );
    world->num_clients = 0;
    world->server = NULL;
}

static void no_worker_connect_all( no_worker_world_t * world )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );

    const char * addresses[] = { address };

    for ( int i = 0; i < world->num_clients; i++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        check( rocketnet_write_production_connect_token( world->backend_secret, world->server_public,
                                                         NO_WORKER_PROTOCOL, addresses, 1, 0x7000 + (uint64_t) i, 1000, 1ULL << 40, 300, NULL,
                                                         token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
        check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, sizeof( token_string ) ) ==
               ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
        check( rocketnet_client_connect( world->clients[i], token_string ) == ROCKETNET_STATUS_OK );
    }
}

// One update of the whole world, the clients first: each client takes what
// arrived, sends one message and its packets (acking what the server sent
// last update); then the server updates (taking those packets and their acks),
// takes its messages, sends one message to every connected slot and its
// packets. dt is the step to the next update.

static void no_worker_step( no_worker_world_t * world, bool send_messages, double dt )
{
    no_worker_now = world->time;

    for ( int i = 0; i < world->num_clients; i++ )
    {
        rocketnet_client_update( world->clients[i], world->time );

        while ( rocketnet_client_receive_messages( world->clients[i] ) != NULL )
        {
        }

        if ( send_messages && rocketnet_client_state( world->clients[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            uint8_t * out = rocketnet_client_start_message( world->clients[i], NO_WORKER_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE, 8 );
            check( out != NULL );

            if ( out )
            {
                memset( out, 0x11, 8 );
                check( rocketnet_client_finish_message( world->clients[i], 8 ) != 0 );
            }
        }

        rocketnet_client_send_packets( world->clients[i] );
    }

    rocketnet_server_update( world->server, world->time );

    while ( rocketnet_server_receive_messages( world->server ) != NULL )
    {
    }

    if ( send_messages )
    {
        for ( int slot = 0; slot < NO_WORKER_CLIENTS; slot++ )
        {
            if ( !rocketnet_server_client_connected( world->server, slot ) )
                continue;

            uint8_t * out = rocketnet_server_start_message( world->server, (uint64_t) slot, NO_WORKER_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE, 8 );
            check( out != NULL );

            if ( out )
            {
                memset( out, 0x22, 8 );
                check( rocketnet_server_finish_message( world->server, 8 ) != 0 );
            }
        }
    }

    rocketnet_server_send_packets( world->server );
    world->time += dt;
}

static int no_worker_connected_slots( no_worker_world_t * world )
{
    int connected = 0;
    for ( int slot = 0; slot < NO_WORKER_CLIENTS; slot++ )
    {
        connected += rocketnet_server_client_connected( world->server, slot ) ? 1 : 0;
    }
    return connected;
}

#endif // ROCKETNET_TESTS_NO_WORKER_WORLD_H
