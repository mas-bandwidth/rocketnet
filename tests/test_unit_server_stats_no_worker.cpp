/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server keeps per-slot statistics with no thread of its own: the running
// aggregates in sdk/rocketnet_ack.cpp (tla/RNServerStats.tla) move on every
// send, receive and ack, on the caller's thread inside update, and the stats
// read returns them. There is no background recompute and no schedule that
// reaches a few slots per update. The server runs on the in-memory loopback
// transport with step-driven clients and a test clock starting at 1000.0; no
// socket, no sleep and no real time.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"

#if defined(__linux__)
#include <dirent.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#define NO_WORKER_PROTOCOL     0x5747A75D0C1E4A11ULL
#define NO_WORKER_CLIENTS      1000
#define NO_WORKER_CHANNEL      0

// The threads of this process, counted from /proc/self/task; -1 where the
// platform has no such count, and the thread case then pins nothing there.
static int no_worker_thread_count()
{
#if defined(__linux__)
    DIR * dir = opendir( "/proc/self/task" );
    if ( !dir )
        return -1;
    int count = 0;
    struct dirent * entry;
    while ( ( entry = readdir( dir ) ) != NULL )
    {
        if ( entry->d_name[0] != '.' )
            count++;
    }
    closedir( dir );
    return count;
#else
    return -1;
#endif
}

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
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( world->server_public, world->server_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.protocol_id = NO_WORKER_PROTOCOL;
    config.max_clients = max_clients;
    config.transport = "loopback";
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
        config.step_driven = true;
        config.time_callback = no_worker_clock;
        no_worker_message_config( NULL, &config );
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        world->clients[i] = rocketnet_client_create( NULL, &config, &status );
        check( world->clients[i] != NULL && status == ROCKETNET_STATUS_OK );
    }
}

static void no_worker_world_destroy( no_worker_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
        rn_test_client_stop_and_destroy( world->clients[i] );
    rn_test_server_stop_and_destroy( world->server );
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
        while ( rocketnet_client_receive_messages( world->clients[i] ) != NULL ) {}
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
    while ( rocketnet_server_receive_messages( world->server ) != NULL ) {}
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
        connected += rocketnet_server_client_connected( world->server, slot ) ? 1 : 0;
    return connected;
}

// Creating, starting and updating a server starts no thread: the loopback
// transport has none and the statistics need none, yet the slot's statistics
// are live after the updates. A server that computed its statistics on a
// worker either adds a thread at create or, with the worker held back (the
// manual mode unit builds once defaulted to), publishes nothing.
static void test_server_stats_creates_no_thread()
{
    no_worker_world_t * world = &g_no_worker_world;
    const int before = no_worker_thread_count();

    no_worker_server_create( world, NO_WORKER_CLIENTS );
    const int after_create = no_worker_thread_count();
    no_worker_clients_create( world, 1 );
    no_worker_connect_all( world );
    for ( int step = 0; step < 400 && no_worker_connected_slots( world ) < 1; step++ )
        no_worker_step( world, false, 0.1 );
    check( no_worker_connected_slots( world ) == 1 );
    for ( int step = 0; step < 20; step++ )
        no_worker_step( world, true, 0.1 );
    const int after_updates = no_worker_thread_count();
    if ( before >= 0 )
    {
        check( after_create == before );
        check( after_updates == before );
    }

    struct rocketnet_server_client_stats_t stats;
    rocketnet_server_client_stats( world->server, rocketnet_client_index( world->clients[0] ), &stats );
    check( stats.rtt_ms > 0.0 && stats.bandwidth_sent_kbps > 0.0 && stats.bandwidth_received_kbps > 0.0 );

    no_worker_world_destroy( world );
}

REGISTER_UNIT_TEST( test_server_stats_creates_no_thread );

// Every one of 1000 connected slots reads statistics that already reflect the
// update that just ran. The step shrinks from 100 ms to 10 ms for one round
// trip: the server sends to every slot, every client receives and acks on the
// next update, and on that same server update every slot's rtt (the minimum
// sample) must read the new 10 ms sample, with the send, receive and ack
// counts all moved. A schedule that recomputes a few slots per update leaves
// almost every slot at the old 100 ms.
static void test_server_stats_current_every_update()
{
    no_worker_world_t * world = &g_no_worker_world;
    no_worker_server_create( world, NO_WORKER_CLIENTS );
    no_worker_clients_create( world, NO_WORKER_CLIENTS );
    no_worker_connect_all( world );
    for ( int step = 0; step < 400 && no_worker_connected_slots( world ) < NO_WORKER_CLIENTS; step++ )
        no_worker_step( world, false, 0.1 );
    check( no_worker_connected_slots( world ) == NO_WORKER_CLIENTS );

    // steady traffic at 100 ms steps until every slot has samples
    for ( int step = 0; step < 10; step++ )
        no_worker_step( world, true, 0.1 );

    static struct rocketnet_server_client_stats_t before[NO_WORKER_CLIENTS];
    for ( int slot = 0; slot < NO_WORKER_CLIENTS; slot++ )
    {
        rocketnet_server_client_stats( world->server, slot, &before[slot] );
        check( before[slot].rtt_ms > 90.0 );
    }

    // the server's last send went out at a 10 ms step; this update's acks
    // carry 10 ms samples for every slot
    no_worker_step( world, true, 0.01 );
    no_worker_step( world, true, 0.01 );

    int current = 0;
    for ( int slot = 0; slot < NO_WORKER_CLIENTS; slot++ )
    {
        struct rocketnet_server_client_stats_t stats;
        rocketnet_server_client_stats( world->server, slot, &stats );
        check( stats.packets_sent > before[slot].packets_sent );
        check( stats.packets_received > before[slot].packets_received );
        check( stats.bandwidth_sent_kbps > 0.0 );
        check( stats.bandwidth_received_kbps > 0.0 );
        check( stats.bandwidth_acked_kbps > 0.0 );
        if ( stats.rtt_ms > 9.0 && stats.rtt_ms < 11.0 )
            current++;
    }
    check( current == NO_WORKER_CLIENTS );

    no_worker_world_destroy( world );
}

REGISTER_UNIT_TEST( test_server_stats_current_every_update );
