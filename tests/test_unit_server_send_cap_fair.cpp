/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server send pass at a cap below the client count: sdk/rocketnet_
// message_server.cpp rotates the first connection served, so a cap of one
// packet per tick serves two or three clients in turn instead of starving
// all but the first. The server runs on the in-memory loopback transport with
// step-driven clients and a test clock starting at 1000.0; no socket, no
// thread, no sleep and no real time. Each client sends one reliable and one
// unreliable message per tick for 60 s of test time at 10 Hz, and the
// reliable echoes must arrive in order at every client without a disconnect.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include "rocketnet_message_object.h"
#include "suite_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rocketnet_config.h"
#include "loopback_hub.h"

#define check rocketnet_unit_check

#define FAIR_PROTOCOL      0x7A11C1C1E5EED0F1ULL
#define FAIR_MAX_CLIENTS   4
#define FAIR_DATA_TICKS    600
#define FAIR_DRAIN_TICKS   120
#define FAIR_DT            0.1
#define FAIR_CHANNEL_RELIABLE    0
#define FAIR_CHANNEL_UNRELIABLE  1

#if ROCKETNET_DEV
static double fair_now = 1000.0;
static double fair_clock( void * ) { return fair_now; }

struct fair_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static void fair_identity( fair_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }

    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static void fair_message_config( rocketnet_server_config_t * server, rocketnet_client_config_t * client )
{
    // the queues hold one reliable and one unreliable message for every data
    // tick, both directions, without a refusal

    if ( server )
    {
        server->send_message_buffer_bytes = 4 * 1024 * 1024;
        server->send_queue_messages = 4096;
        server->reliable_messages_in_flight = 4096;
        server->packets_in_flight = 4096;            // the sent map holds the window
    }

    if ( client )
    {
        client->send_message_buffer_bytes = 4 * 1024 * 1024;
        client->send_queue_messages = 4096;
        client->reliable_messages_in_flight = 4096;
        client->packets_in_flight = 4096;            // the sent map holds the window
    }
}

struct fair_world_t
{
    fair_identity_t identity;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[FAIR_MAX_CLIENTS];
    int server_index[FAIR_MAX_CLIENTS];
    int num_clients;
    double time;
};

static void fair_world_create( fair_world_t * world, int num_clients, int capacity, int max_clients = FAIR_MAX_CLIENTS )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    fair_now = world->time;
    world->num_clients = num_clients;
    fair_identity( &world->identity );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );

    server_config.protocol_id = FAIR_PROTOCOL;
    server_config.max_clients = max_clients;
    memcpy( server_config.backend_public_key, world->identity.backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    fair_message_config( &server_config, NULL );

    if ( max_clients >= 256 )
    {
        server_config.send_message_buffer_bytes = 12288;
        server_config.send_queue_messages = 64;
        server_config.reliable_messages_in_flight = 64;
        server_config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    }

    rocketnet_server_transport_set_create_send_capacity_for_test( capacity );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( world->server != NULL );

    rocketnet_server_public_key( world->server, world->identity.server_public );

    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_running( world->server ) == 1 );

    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = true;
        client_config.time_callback = fair_clock;
        fair_message_config( NULL, &client_config );

        if ( max_clients >= 256 )
        {
            client_config.reliable_messages_in_flight = 64;
            client_config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
        }

        create_status = ROCKETNET_STATUS_OK;
        world->clients[i] = rocketnet_client_create( NULL, &client_config, &create_status );
        check( world->clients[i] != NULL );
        check( create_status == ROCKETNET_STATUS_OK );
    }
}

static void fair_world_destroy( fair_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rn_test_client_stop_and_destroy( world->clients[i] );
    }

    rn_test_server_stop_and_destroy( world->server );
}

static void fair_token( fair_world_t * world, uint64_t client_id, char * token_string )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( world->identity.backend_secret, world->identity.server_public,
                                                     FAIR_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

static void fair_update_clients( fair_world_t * world )
{
    fair_now = world->time;
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rocketnet_client_update( world->clients[i], world->time );
    }
}

static void fair_send_clients( fair_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rocketnet_client_send_packets( world->clients[i] );
    }
}

static void fair_server_update( fair_world_t * world )
{
    rocketnet_server_update( world->server, world->time );
}

static void fair_connect( fair_world_t * world, int client, uint64_t client_id )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    fair_token( world, client_id, token );
    check( rocketnet_client_connect( world->clients[client], token ) == ROCKETNET_STATUS_OK );
    for ( int step = 0; step < 400 && rocketnet_client_state( world->clients[client] ) != ROCKETNET_CLIENT_STATE_CONNECTED; step++ )
    {
        fair_update_clients( world );
        fair_send_clients( world );
        fair_server_update( world );
        rocketnet_server_send_packets( world->server );
        world->time += FAIR_DT;
    }

    check( rocketnet_client_state( world->clients[client] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    world->server_index[client] = rocketnet_client_index( world->clients[client] );
    for ( int step = 0; step < 400 && !rocketnet_server_client_connected( world->server, world->server_index[client] ); step++ )
    {
        fair_update_clients( world );
        fair_send_clients( world );
        fair_server_update( world );
        rocketnet_server_send_packets( world->server );
        world->time += FAIR_DT;
    }

    check( rocketnet_server_client_connected( world->server, world->server_index[client] ) != 0 );
}

static uint32_t fair_client_send( fair_world_t * world, int client, int channel, rocketnet_reliability_t reliability, uint16_t value )
{
    uint8_t * out = rocketnet_client_start_message( world->clients[client], channel, reliability, 2 );
    check( out != NULL );
    if ( !out )
    {
        return 0;
    }
    out[0] = (uint8_t) ( value >> 8 );
    out[1] = (uint8_t) value;

    return rocketnet_client_finish_message( world->clients[client], 2 );
}

static uint32_t fair_server_send( fair_world_t * world, int client, int channel, rocketnet_reliability_t reliability, uint16_t value )
{
    uint8_t * out = rocketnet_server_start_message( world->server, (uint64_t) world->server_index[client], channel, reliability, 2 );
    check( out != NULL );
    if ( !out )
    {
        return 0;
    }
    out[0] = (uint8_t) ( value >> 8 );
    out[1] = (uint8_t) value;

    return rocketnet_server_finish_message( world->server, 2 );
}

static void fair_run_case( int num_clients, int capacity = 1 )
{
    fair_world_t world;
    fair_world_create( &world, num_clients, capacity );
    for ( int i = 0; i < num_clients; i++ )
    {
        fair_connect( &world, i, 0x9000 + (uint64_t) i );
    }

    int server_reliable[FAIR_MAX_CLIENTS];
    int server_unreliable[FAIR_MAX_CLIENTS];
    int client_reliable[FAIR_MAX_CLIENTS];
    int client_unreliable[FAIR_MAX_CLIENTS];
    int last_reliable_tick[FAIR_MAX_CLIENTS];
    memset( server_reliable, 0, sizeof( server_reliable ) );
    memset( server_unreliable, 0, sizeof( server_unreliable ) );
    memset( client_reliable, 0, sizeof( client_reliable ) );
    memset( client_unreliable, 0, sizeof( client_unreliable ) );

    for ( int c = 0; c < num_clients; c++ )
    {
        last_reliable_tick[c] = -1;
    }

    for ( int tick = 0; tick < FAIR_DATA_TICKS + FAIR_DRAIN_TICKS; tick++ )
    {
        fair_update_clients( &world );

        for ( int c = 0; c < num_clients; c++ )
        {
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( world.clients[c] ) ) != NULL )
            {
                for ( int i = 0; i < batch->num_messages; i++ )
                {
                    if ( batch->channel[i] == FAIR_CHANNEL_RELIABLE )
                    {
                        check( batch->bytes[i] == 2 );

                        const uint8_t * data = batch->data[i];
                        const uint16_t value = (uint16_t) ( ( data[0] << 8 ) | data[1] );
                        check( value == (uint16_t) client_reliable[c] );

                        client_reliable[c]++;
                        last_reliable_tick[c] = tick;
                    }
                    else if ( batch->channel[i] == FAIR_CHANNEL_UNRELIABLE )
                    {
                        client_unreliable[c]++;
                    }
                }
            }
        }

        // With N clients and capacity K, one-frame rounds serve each client
        // within ceil(N/K) ticks, even when each client has multiple packets
        // due. Allow the first rotation to reach every client.

        if ( tick >= num_clients && tick < FAIR_DATA_TICKS )
        {
            for ( int c = 0; c < num_clients; c++ )
            {
                check( last_reliable_tick[c] >= 0 );
                check( tick - last_reliable_tick[c] <= ( num_clients + capacity - 1 ) / capacity );
            }
        }

        if ( tick < FAIR_DATA_TICKS )
        {
            for ( int c = 0; c < num_clients; c++ )
            {
                check( fair_client_send( &world, c, FAIR_CHANNEL_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (uint16_t) tick ) != 0 );
                check( fair_client_send( &world, c, FAIR_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, (uint16_t) tick ) != 0 );
            }
        }

        fair_send_clients( &world );
        fair_server_update( &world );

        {
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_server_receive_messages( world.server ) ) != NULL )
            {
                for ( int i = 0; i < batch->num_messages; i++ )
                {
                    int c = 0;
                    while ( c < num_clients && world.server_index[c] != batch->client_index[i] )
                    {
                        c++;
                    }

                    check( c < num_clients );

                    if ( batch->channel[i] == FAIR_CHANNEL_RELIABLE )
                    {
                        check( batch->bytes[i] == 2 );

                        const uint8_t * data = batch->data[i];
                        const uint16_t value = (uint16_t) ( ( data[0] << 8 ) | data[1] );
                        check( value == (uint16_t) server_reliable[c] );

                        server_reliable[c]++;
                    }
                    else if ( batch->channel[i] == FAIR_CHANNEL_UNRELIABLE )
                    {
                        server_unreliable[c]++;
                    }
                }
            }
        }

        if ( tick < FAIR_DATA_TICKS )
        {
            for ( int c = 0; c < num_clients; c++ )
            {
                // Queue unreliable first to exercise in-place admission before
                // the reliable work for this tick is visible.

                check( fair_server_send( &world, c, FAIR_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, (uint16_t) tick ) != 0 );
                check( fair_server_send( &world, c, FAIR_CHANNEL_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (uint16_t) tick ) != 0 );
            }
        }

        // the cap itself: one update's send pass hands at most capacity
        // frames to the transport, with every client's work due

        uint64_t sent_before = 0;
        for ( int c = 0; c < num_clients; c++ )
        {
            sent_before += rn_server_client_stats_of( world.server, world.server_index[c] ).packets_sent;
        }

        rocketnet_server_send_packets( world.server );

        uint64_t sent_after = 0;
        for ( int c = 0; c < num_clients; c++ )
        {
            sent_after += rn_server_client_stats_of( world.server, world.server_index[c] ).packets_sent;
        }
        check( sent_after - sent_before <= (uint64_t) capacity );

        world.time += FAIR_DT;
    }

    // one last client update delivers the final server send

    fair_update_clients( &world );

    for ( int c = 0; c < num_clients; c++ )
    {
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( world.clients[c] ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                if ( batch->channel[i] != FAIR_CHANNEL_RELIABLE )
                {
                    continue;
                }
                check( batch->bytes[i] == 2 );

                const uint8_t * data = batch->data[i];
                const uint16_t value = (uint16_t) ( ( data[0] << 8 ) | data[1] );
                check( value == (uint16_t) client_reliable[c] );

                client_reliable[c]++;
            }
        }
    }

    bool ok = true;
    for ( int c = 0; c < num_clients; c++ )
    {
        if ( rocketnet_client_state( world.clients[c] ) != ROCKETNET_CLIENT_STATE_CONNECTED ||
             !rocketnet_server_client_connected( world.server, world.server_index[c] ) ||
             server_reliable[c] != FAIR_DATA_TICKS || client_reliable[c] != FAIR_DATA_TICKS )
        {
            ok = false;
        }
    }

    if ( !ok )
    {
        printf( "cap %d, %d clients: connected", capacity, num_clients );
        for ( int c = 0; c < num_clients; c++ )
        {
            printf( " [%d]=%d", c, rocketnet_client_state( world.clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        }

        printf( " server_reliable" );

        for ( int c = 0; c < num_clients; c++ )
        {
            printf( " [%d]=%d", c, server_reliable[c] );
        }

        printf( " client_reliable" );

        for ( int c = 0; c < num_clients; c++ )
        {
            printf( " [%d]=%d", c, client_reliable[c] );
        }

        printf( " server_unreliable" );

        for ( int c = 0; c < num_clients; c++ )
        {
            printf( " [%d]=%d", c, server_unreliable[c] );
        }

        printf( " client_unreliable" );

        for ( int c = 0; c < num_clients; c++ )
        {
            printf( " [%d]=%d", c, client_unreliable[c] );
        }

        printf( "\n" );
        fflush( stdout );
    }

    for ( int c = 0; c < num_clients; c++ )
    {
        check( rocketnet_client_state( world.clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( world.server, world.server_index[c] ) != 0 );
        check( server_reliable[c] == FAIR_DATA_TICKS );
        check( client_reliable[c] == FAIR_DATA_TICKS );
        check( server_unreliable[c] > 0 );
        check( client_unreliable[c] > 0 );
    }

    fair_world_destroy( &world );
}

static void test_server_send_cap_one_two_clients()
{
    fair_run_case( 2 );
}

static void test_server_send_cap_one_three_clients()
{
    fair_run_case( 3 );
}

static void test_server_send_cap_two_three_clients()
{
    fair_run_case( 3, 2 );
}

// More than one payload frame is due at each client. Tiny messages can share a
// frame, so the service assertion here observes packets rather than delivery
// of a whole message that itself needs several turns to finish.
static void test_server_send_cap_two_multi_packet_service()
{
    fair_world_t world;
    fair_world_create( &world, 3, 2 );

    for ( int c = 0; c < 3; c++ )
    {
        fair_connect( &world, c, 0x9200 + (uint64_t) c );
    }

    int last_service[3] = { -1, -1, -1 };

    for ( int tick = 0; tick < 24; tick++ )
    {
        fair_update_clients( &world );

        for ( int c = 0; c < 3; c++ )
        {
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( world.clients[c] ) ) != NULL )
            {
                ;
            }
        }

        fair_send_clients( &world );
        fair_server_update( &world );

        uint64_t before[3];
        for ( int c = 0; c < 3; c++ )
        {
            before[c] = rn_server_client_stats_of( world.server, world.server_index[c] ).packets_sent;
            uint8_t * out = rocketnet_server_start_message( world.server, (uint64_t) world.server_index[c], FAIR_CHANNEL_RELIABLE,
                                                            ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 3000 );
            check( out != NULL );

            memset( out, tick, 3000 );

            check( rocketnet_server_finish_message( world.server, 3000 ) != 0 );
        }

        rocketnet_server_send_packets( world.server );

        // each client has more than one frame due: the pass hands exactly
        // the cap of two frames to the transport, never more

        uint64_t sent = 0;
        for ( int c = 0; c < 3; c++ )
        {
            sent += rn_server_client_stats_of( world.server, world.server_index[c] ).packets_sent - before[c];
        }
        check( sent == 2 );

        for ( int c = 0; c < 3; c++ )
        {
            if ( rn_server_client_stats_of( world.server, world.server_index[c] ).packets_sent > before[c] )
            {
                last_service[c] = tick;
            }

            if ( tick >= 1 )
            {
                check( tick - last_service[c] < 2 );
            }
        }

        world.time += FAIR_DT;
    }

    fair_world_destroy( &world );
}

REGISTER_UNIT_TEST( test_server_send_cap_two_multi_packet_service );

// Per-client scope (standard/MESSAGES.md m1.8) under probe pacing (m9.3): the
// default turns the reliable timeout off, so a client that goes dark with
// reliable messages unacknowledged is held after a second, and only its
// oldest number U is resent, once per resend delay. With the cap at the
// client count, the healthy client is offered more than the cap every tick,
// so every frame of the cap must carry a packet: a dark client takes one
// only in an update its probe is due, and the healthy client takes the rest,
// exactly the cap with idle neighbours.

static rn_message_connection_t * fair_server_connection( fair_world_t * world, int client )
{
    rocketnet_message_server_t * messages = rocketnet_server_message_server( world->server );
    return &messages->object.connections[world->server_index[client]];
}

static void fair_dark_run( bool dark, int * received, int * probes, int ticks )
{
    const int healthy = FAIR_MAX_CLIENTS - 1;
    fair_world_t world;
    fair_world_create( &world, FAIR_MAX_CLIENTS, FAIR_MAX_CLIENTS );
    for ( int c = 0; c < FAIR_MAX_CLIENTS; c++ )
    {
        fair_connect( &world, c, 0x9300 + (uint64_t) c );
    }

    for ( int tick = 0; tick < ticks; tick++ )
    {
        received[tick] = 0;
        probes[tick] = 0;

        fair_now = world.time;
        for ( int c = 0; c < FAIR_MAX_CLIENTS; c++ )
        {
            if ( dark && c != healthy )
            {
                continue;
            }
            rocketnet_client_update( world.clients[c], world.time );
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( world.clients[c] ) ) != NULL )
            {
                received[tick] += c == healthy ? batch->num_messages : 0;
            }
            rocketnet_client_send_packets( world.clients[c] );
        }

        fair_server_update( &world );
        while ( rocketnet_server_receive_messages( world.server ) != NULL )
        {
        }

        if ( tick == 0 )
        {
            for ( int c = 0; c < healthy; c++ )
            {
                for ( int k = 0; k < 3; k++ )
                {
                    check( fair_server_send( &world, c, FAIR_CHANNEL_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE, (uint16_t) k ) != 0 );
                }
            }
        }

        for ( int k = 0; k < FAIR_MAX_CLIENTS; k++ )
        {
            uint8_t * out = rocketnet_server_start_message( world.server, (uint64_t) world.server_index[healthy], FAIR_CHANNEL_RELIABLE,
                                                            ROCKETNET_RELIABILITY_RELIABLE, 1200 );
            check( out != NULL );
            if ( out )
            {
                memset( out, tick, 1200 );
                check( rocketnet_server_finish_message( world.server, 1200 ) != 0 );
            }
        }

        // the probes due in this update's send pass: a dark client held by
        // probe pacing whose U has waited its resend delay

        for ( int c = 0; dark && c < healthy; c++ )
        {
            const rn_window_ack_t * ack = &fair_server_connection( &world, c )->ordered.ack;
            const rn_region_t * region = ack->region;
            const rn_send_number_t * u = &region->send_numbers[ack->send->U & ( region->window - 1 )];
            if ( tick >= 20 )
            {
                check( rn_window_ack_probing( ack, world.time ) );
            }
            probes[tick] += world.time >= u->last_send + rn_window_ack_resend_delay( ack ) ? 1 : 0;
        }

        rocketnet_server_send_packets( world.server );
        world.time += FAIR_DT;

        for ( int c = 0; c < FAIR_MAX_CLIENTS; c++ )
        {
            check( rocketnet_server_client_connected( world.server, world.server_index[c] ) != 0 );
        }
    }

    fair_world_destroy( &world );
}

static void test_server_send_cap_dark_neighbours_take_only_their_probes()
{
    const int ticks = 80;                           // 8 s, inside the 10 s silence timeout
    int idle_received[80];
    int idle_probes[80];
    int dark_received[80];
    int dark_probes[80];
    fair_dark_run( false, idle_received, idle_probes, ticks );
    fair_dark_run( true, dark_received, dark_probes, ticks );

    int probes = 0;
    for ( int tick = 20; tick < ticks - 1; tick++ )
    {
        // what the server sent in update tick, the healthy client receives in update tick + 1

        check( idle_received[tick + 1] == FAIR_MAX_CLIENTS );
        check( dark_received[tick + 1] == idle_received[tick + 1] - dark_probes[tick] );
        probes += dark_probes[tick];
    }

    check( probes > 0 );
}

REGISTER_UNIT_TEST( test_server_send_cap_dark_neighbours_take_only_their_probes );

// Per-client scope (standard/MESSAGES.md m1.8) under each neighbour's own
// send budget (m9.4): three live neighbours at 16 kbps, each queued more
// than its bucket pays every tick, so most updates its budget refuses the
// packet it has due (reliable, or unreliable records all over budget). A
// refused packet is never sent, so it must take no frame of the shared
// per-update capacity: the healthy client, offered more than the cap every
// tick, gets exactly the cap less the packets the neighbours really sent.

enum fair_neighbour_mode_t
{
    FAIR_NEIGHBOURS_IDLE,
    FAIR_NEIGHBOURS_RELIABLE_OVER_BUDGET,
    FAIR_NEIGHBOURS_UNRELIABLE_OVER_BUDGET,
};

static void fair_budget_run( fair_neighbour_mode_t mode, int * received, int * neighbour_sent, int ticks, uint64_t * frames_aborted )
{
    const int healthy = FAIR_MAX_CLIENTS - 1;
    fair_world_t world;
    fair_world_create( &world, FAIR_MAX_CLIENTS, FAIR_MAX_CLIENTS );
    for ( int c = 0; c < FAIR_MAX_CLIENTS; c++ )
    {
        fair_connect( &world, c, 0x9400 + (uint64_t) c );
    }

    for ( int c = 0; c < healthy; c++ )
    {
        check( rocketnet_server_set_send_kbps( world.server, world.server_index[c], 16 ) == ROCKETNET_STATUS_OK );
    }

    for ( int tick = 0; tick < ticks; tick++ )
    {
        received[tick] = 0;
        neighbour_sent[tick] = 0;

        fair_now = world.time;
        for ( int c = 0; c < FAIR_MAX_CLIENTS; c++ )
        {
            rocketnet_client_update( world.clients[c], world.time );
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( world.clients[c] ) ) != NULL )
            {
                received[tick] += c == healthy ? batch->num_messages : 0;
            }
            rocketnet_client_send_packets( world.clients[c] );
        }

        fair_server_update( &world );
        while ( rocketnet_server_receive_messages( world.server ) != NULL )
        {
        }

        for ( int c = 0; mode != FAIR_NEIGHBOURS_IDLE && c < healthy; c++ )
        {
            const bool reliable = mode == FAIR_NEIGHBOURS_RELIABLE_OVER_BUDGET;
            const int bytes = reliable ? 1000 : 600;
            uint8_t * out = rocketnet_server_start_message( world.server, (uint64_t) world.server_index[c], reliable ? FAIR_CHANNEL_RELIABLE : FAIR_CHANNEL_UNRELIABLE,
                                                            reliable ? ROCKETNET_RELIABILITY_RELIABLE : ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
            check( out != NULL );
            if ( out )
            {
                memset( out, tick, (size_t) bytes );
                check( rocketnet_server_finish_message( world.server, bytes ) != 0 );
            }
        }

        for ( int k = 0; k < FAIR_MAX_CLIENTS; k++ )
        {
            uint8_t * out = rocketnet_server_start_message( world.server, (uint64_t) world.server_index[healthy], FAIR_CHANNEL_RELIABLE,
                                                            ROCKETNET_RELIABILITY_RELIABLE, 1200 );
            check( out != NULL );
            if ( out )
            {
                memset( out, tick, 1200 );
                check( rocketnet_server_finish_message( world.server, 1200 ) != 0 );
            }
        }

        uint64_t before[FAIR_MAX_CLIENTS];
        for ( int c = 0; c < healthy; c++ )
        {
            before[c] = rn_server_client_stats_of( world.server, world.server_index[c] ).packets_sent;
        }

        rn_send_pass_cost_reset();
        rocketnet_server_send_packets( world.server );
        *frames_aborted += rn_send_pass_cost_read().frames_aborted;

        for ( int c = 0; c < healthy; c++ )
        {
            neighbour_sent[tick] += (int) ( rn_server_client_stats_of( world.server, world.server_index[c] ).packets_sent - before[c] );
        }

        world.time += FAIR_DT;

        for ( int c = 0; c < FAIR_MAX_CLIENTS; c++ )
        {
            check( rocketnet_server_client_connected( world.server, world.server_index[c] ) != 0 );
        }
    }

    fair_world_destroy( &world );
}

static void fair_budget_case( fair_neighbour_mode_t mode )
{
    const int ticks = 60;
    int idle_received[60];
    int idle_sent[60];
    int busy_received[60];
    int busy_sent[60];
    uint64_t idle_aborted = 0;
    uint64_t busy_aborted = 0;
    fair_budget_run( FAIR_NEIGHBOURS_IDLE, idle_received, idle_sent, ticks, &idle_aborted );
    fair_budget_run( mode, busy_received, busy_sent, ticks, &busy_aborted );

    // the send pass started no frame it then aborted (the development seam
    // counts every abort in rn_message_connection_write)

    check( idle_aborted == 0 );
    check( busy_aborted == 0 );

    int refused_updates = 0;
    int healthy_total = 0;
    int neighbour_total = 0;
    for ( int tick = 10; tick < ticks - 1; tick++ )
    {
        refused_updates += busy_sent[tick] < FAIR_MAX_CLIENTS - 1 ? 1 : 0;
        healthy_total += busy_received[tick + 1];
        neighbour_total += busy_sent[tick];
    }

    printf( "    [%s] healthy %.2f per tick, neighbours sent %.2f per tick (idle: 4.00, 0.00) over %d updates\n",
            mode == FAIR_NEIGHBOURS_RELIABLE_OVER_BUDGET ? "reliable over budget" : "unreliable over budget",
            (double) healthy_total / (double) ( ticks - 11 ), (double) neighbour_total / (double) ( ticks - 11 ), ticks - 11 );

    for ( int tick = 10; tick < ticks - 1; tick++ )
    {
        // what the server sent in update tick, the healthy client receives in update tick + 1

        check( idle_sent[tick] == 0 );
        check( idle_received[tick + 1] == FAIR_MAX_CLIENTS );
        check( busy_received[tick + 1] == idle_received[tick + 1] - busy_sent[tick] );
    }

    // the budget refused a neighbour's due packet in most updates
    check( refused_updates > ( ticks - 11 ) / 2 );
}

static void test_server_send_cap_budget_refused_reliable_takes_no_slot()
{
    fair_budget_case( FAIR_NEIGHBOURS_RELIABLE_OVER_BUDGET );
}

static void test_server_send_cap_unreliable_over_budget_takes_no_slot()
{
    fair_budget_case( FAIR_NEIGHBOURS_UNRELIABLE_OVER_BUDGET );
}

REGISTER_UNIT_TEST( test_server_send_cap_budget_refused_reliable_takes_no_slot );
REGISTER_UNIT_TEST( test_server_send_cap_unreliable_over_budget_takes_no_slot );
// Per client on the server (property P): a server with 16 clients and a send
// capacity of one frame for each client with work plus one spare, where a
// subset of the clients is dark (no longer updated), never acknowledges (still
// updated, its outgoing loss 100), or floods (sends as fast as it can, and
// the server sends to it as fast as it can). Every other client's run is
// identical to a control run in which the subset is connected and idle: the
// messages it receives in each update, the messages the server receives from
// it in each update, the packets the server sends to it in each update, and
// its refusals on both ends. Each member of the subset is sent three reliable
// messages when its behaviour starts (in the control run too), three updates
// apart, and the server's resend delay is held at 2 s, so the members' probes
// fall in different updates and the spare frame carries each. With the
// v1.1.1 probe-slot fix reverted, a held member takes a frame every update
// and the healthy clients lose theirs.

#define PC_CLIENTS          16      // the per-commit cases
#define PC_MAX_CLIENTS      ROCKETNET_MAX_CLIENTS   // the nightly case
#define PC_TICKS            90
#define PC_FIRST_TICK       5
#define PC_STAGGER          3
#define PC_BYTES            1000

enum pc_kind_t
{
    PC_HEALTHY,
    PC_IDLE,
    PC_DARK,
    PC_NO_ACK,
    PC_FLOOD,
};

struct pc_record_t
{
    int client_received[PC_MAX_CLIENTS][PC_TICKS];
    int server_received[PC_MAX_CLIENTS][PC_TICKS];
    uint64_t server_packets[PC_MAX_CLIENTS][PC_TICKS];
    uint64_t client_refused[PC_MAX_CLIENTS];
    uint64_t server_refused[PC_MAX_CLIENTS];
};

struct pc_world_t
{
    fair_identity_t identity;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[PC_MAX_CLIENTS];
    int slot[PC_MAX_CLIENTS];
    int num_clients;
    double time;
};

static void pc_world_create( pc_world_t * world, int num_clients, int capacity )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    fair_now = world->time;
    world->num_clients = num_clients;
    fair_identity( &world->identity );
    rocketnet_test_loopback_hub_clients( num_clients );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = FAIR_PROTOCOL;
    server_config.max_clients = num_clients;
    memcpy( server_config.backend_public_key, world->identity.backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    server_config.message_resend_delay_min_ms = 2000;      // every resend, and so every probe, 2 s after the last send
    rocketnet_server_transport_set_create_send_capacity_for_test( capacity );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( world->server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rocketnet_server_public_key( world->server, world->identity.server_public );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = true;
        client_config.time_callback = fair_clock;
        create_status = ROCKETNET_STATUS_OK;
        world->clients[i] = rocketnet_client_create( NULL, &client_config, &create_status );
        check( world->clients[i] != NULL );
        check( create_status == ROCKETNET_STATUS_OK );
    }

    // connect every client, one after another

    for ( int i = 0; i < num_clients; i++ )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        uint8_t raw[ROCKETNET_CONNECT_TOKEN_BYTES];
        char address[64];
        snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
        const char * addresses[] = { address };
        check( rocketnet_write_production_connect_token( world->identity.backend_secret, world->identity.server_public, FAIR_PROTOCOL, addresses, 1,
                                                         0xA000 + (uint64_t) i, 1000, 1ULL << 40, 300, NULL, raw, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        check( rocketnet_base64_encode_data( raw, sizeof( raw ), token, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
        check( rocketnet_client_connect( world->clients[i], token ) == ROCKETNET_STATUS_OK );
        for ( int step = 0; step < 400 && ( rocketnet_client_state( world->clients[i] ) != ROCKETNET_CLIENT_STATE_CONNECTED ||
                                            !rocketnet_server_client_connected( world->server, rocketnet_client_index( world->clients[i] ) ) );
              step++ )
        {
            fair_now = world->time;
            for ( int c = 0; c <= i; c++ )
            {
                rocketnet_client_update( world->clients[c], world->time );
                while ( rocketnet_client_receive_messages( world->clients[c] ) != NULL )
                {
                }
                rocketnet_client_send_packets( world->clients[c] );
            }
            rocketnet_server_update( world->server, world->time );
            while ( rocketnet_server_receive_messages( world->server ) != NULL )
            {
            }
            rocketnet_server_send_packets( world->server );
            world->time += FAIR_DT;
        }
        check( rocketnet_client_state( world->clients[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        world->slot[i] = rocketnet_client_index( world->clients[i] );
        check( rocketnet_server_client_connected( world->server, world->slot[i] ) );
    }
}

static void pc_world_destroy( pc_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rn_test_client_stop_and_destroy( world->clients[i] );
    }
    rn_test_server_stop_and_destroy( world->server );
}

static bool pc_client_send( rocketnet_client_t * client, uint8_t fill )
{
    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, PC_BYTES );
    if ( !p )
    {
        return false;
    }
    memset( p, fill, PC_BYTES );
    check( rocketnet_client_finish_message( client, PC_BYTES ) != 0 );
    return true;
}

static bool pc_server_send( pc_world_t * world, int c, enum rocketnet_reliability_t reliability, int bytes, uint8_t fill )
{
    uint8_t * p = rocketnet_server_start_message( world->server, (uint64_t) world->slot[c], 0, reliability, bytes );
    if ( !p )
    {
        return false;
    }
    memset( p, fill, (size_t) bytes );
    check( rocketnet_server_finish_message( world->server, bytes ) != 0 );
    return true;
}

static int pc_client_of_slot( const pc_world_t * world, int slot )
{
    for ( int c = 0; c < world->num_clients; c++ )
    {
        if ( world->slot[c] == slot )
        {
            return c;
        }
    }
    return -1;
}

// one run: kinds[c] is client c's behaviour from its start tick on (the
// subset's members start PC_STAGGER updates apart, in client order)

static void pc_run( const pc_kind_t * kinds, int num_clients, int capacity, pc_record_t * record )
{
    memset( record, 0, sizeof( *record ) );
    pc_world_t world;
    pc_world_create( &world, num_clients, capacity );

    int start[PC_MAX_CLIENTS];
    int member = 0;
    for ( int c = 0; c < num_clients; c++ )
    {
        start[c] = kinds[c] == PC_HEALTHY ? 0 : PC_FIRST_TICK + PC_STAGGER * member++;
    }

    for ( int tick = 0; tick < PC_TICKS; tick++ )
    {
        fair_now = world.time;

        for ( int c = 0; c < num_clients; c++ )
        {
            const bool started = kinds[c] != PC_HEALTHY && tick >= start[c];
            if ( started && kinds[c] == PC_DARK )
            {
                continue;
            }
            if ( started && kinds[c] == PC_NO_ACK && tick == start[c] )
            {
                rocketnet_client_network_simulator( world.clients[c], 0, 0, 100, 0 );
            }

            rocketnet_client_update( world.clients[c], world.time );
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( world.clients[c] ) ) != NULL )
            {
                record->client_received[c][tick] += batch->num_messages;
            }

            if ( kinds[c] == PC_HEALTHY )
            {
                check( pc_client_send( world.clients[c], (uint8_t) tick ) );
            }
            else if ( started && kinds[c] == PC_FLOOD )
            {
                for ( int k = 0; k < 100000 && pc_client_send( world.clients[c], 0xF1 ); k++ )
                {
                }
            }
            rocketnet_client_send_packets( world.clients[c] );
        }

        rocketnet_server_update( world.server, world.time );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( world.server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                const int c = pc_client_of_slot( &world, batch->client_index[i] );
                check( c >= 0 );
                if ( c >= 0 )
                {
                    record->server_received[c][tick]++;
                }
            }
        }

        for ( int c = 0; c < num_clients; c++ )
        {
            if ( kinds[c] == PC_HEALTHY )
            {
                check( pc_server_send( &world, c, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, PC_BYTES, (uint8_t) tick ) );
            }
            else if ( tick == start[c] )
            {
                for ( int k = 0; k < 3; k++ )
                {
                    check( pc_server_send( &world, c, ROCKETNET_RELIABILITY_RELIABLE, 2, (uint8_t) k ) );
                }
            }
            else if ( tick > start[c] && kinds[c] == PC_FLOOD )
            {
                for ( int k = 0; k < 100000 && pc_server_send( &world, c, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, PC_BYTES, 0xF2 ); k++ )
                {
                }
            }
        }

        rocketnet_server_send_packets( world.server );

        for ( int c = 0; c < num_clients; c++ )
        {
            struct rocketnet_server_client_stats_t stats;
            memset( &stats, 0, sizeof( stats ) );
            rocketnet_server_client_stats( world.server, world.slot[c], &stats );
            record->server_packets[c][tick] = stats.packets_sent;
            check( rocketnet_server_client_connected( world.server, world.slot[c] ) );
        }

        world.time += FAIR_DT;
    }

    for ( int c = 0; c < num_clients; c++ )
    {
        struct rocketnet_conditions_t conditions;
        memset( &conditions, 0, sizeof( conditions ) );
        check( rocketnet_server_client_conditions( world.server, world.slot[c], &conditions, (int) sizeof( conditions ) ) );
        record->server_refused[c] = conditions.send_refused;
        if ( kinds[c] != PC_DARK )
        {
            check( rocketnet_client_state( world.clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        }
        memset( &conditions, 0, sizeof( conditions ) );
        (void) rocketnet_client_conditions( world.clients[c], &conditions, (int) sizeof( conditions ) );
        record->client_refused[c] = conditions.send_refused;
    }

    pc_world_destroy( &world );
}

static void pc_expect_healthy_identical( const pc_kind_t * kinds, int num_clients, const pc_record_t * control, const pc_record_t * run )
{
    for ( int c = 0; c < num_clients; c++ )
    {
        if ( kinds[c] != PC_HEALTHY )
        {
            continue;
        }
        check( memcmp( control->client_received[c], run->client_received[c], sizeof( control->client_received[c] ) ) == 0 );
        check( memcmp( control->server_received[c], run->server_received[c], sizeof( control->server_received[c] ) ) == 0 );
        check( memcmp( control->server_packets[c], run->server_packets[c], sizeof( control->server_packets[c] ) ) == 0 );
        check( control->client_refused[c] == 0 && run->client_refused[c] == 0 );
        check( control->server_refused[c] == 0 && run->server_refused[c] == 0 );
    }
}

// the subset: four clients spread over the slots, all of one kind, or (mixed)
// two dark, one never acknowledging, one flooding

static void pc_kinds( pc_kind_t * kinds, int num_clients, pc_kind_t kind, bool mixed )
{
    for ( int c = 0; c < num_clients; c++ )
    {
        kinds[c] = PC_HEALTHY;
    }
    // at 16 clients the members are 1, 6, 10 and 13; at more, the same
    // places in the slot range
    const int members[] = { num_clients / 16, 6 * num_clients / 16, 10 * num_clients / 16, 13 * num_clients / 16 };
    const pc_kind_t mix[] = { PC_DARK, PC_FLOOD, PC_NO_ACK, PC_DARK };
    for ( int m = 0; m < 4; m++ )
    {
        kinds[members[m]] = mixed ? mix[m] : kind;
    }
}

static void pc_case( pc_kind_t kind, bool mixed, int num_clients = PC_CLIENTS )
{
    // one frame for each client with work in an update, and one spare: the
    // healthy clients (twelve of 16) take one each, a flooding member one (or more,
    // from what the round leaves), and a member's probe the spare

    pc_kind_t kinds[PC_MAX_CLIENTS];
    pc_kinds( kinds, num_clients, kind, mixed );
    int flooding = 0;
    for ( int c = 0; c < num_clients; c++ )
    {
        flooding += kinds[c] == PC_FLOOD ? 1 : 0;
    }
    const int capacity = ( num_clients - 4 ) + flooding + 1;

    pc_kind_t idle[PC_MAX_CLIENTS];
    for ( int c = 0; c < num_clients; c++ )
    {
        idle[c] = kinds[c] == PC_HEALTHY ? PC_HEALTHY : PC_IDLE;
    }

    static pc_record_t control;
    static pc_record_t run;
    pc_run( idle, num_clients, capacity, &control );
    pc_run( kinds, num_clients, capacity, &run );

    // the healthy clients were served every update, in both runs

    for ( int c = 0; c < num_clients; c++ )
    {
        if ( kinds[c] == PC_HEALTHY )
        {
            for ( int tick = PC_FIRST_TICK; tick < PC_TICKS; tick++ )
            {
                check( control.client_received[c][tick] == 1 );
            }
        }
    }
    pc_expect_healthy_identical( kinds, num_clients, &control, &run );
}

static void test_server_per_client_dark_subset_changes_nothing_for_the_others()
{
    pc_case( PC_DARK, false );
}

static void test_server_per_client_never_acking_subset_changes_nothing_for_the_others()
{
    pc_case( PC_NO_ACK, false );
}

static void test_server_per_client_flooding_subset_changes_nothing_for_the_others()
{
    pc_case( PC_FLOOD, false );
}

static void test_server_per_client_mixed_subset_changes_nothing_for_the_others()
{
    pc_case( PC_DARK, true );
}

static void test_server_per_client_mixed_subset_at_max_clients()
{
    pc_case( PC_DARK, true, PC_MAX_CLIENTS );
}

REGISTER_UNIT_TEST( test_server_per_client_dark_subset_changes_nothing_for_the_others );
REGISTER_UNIT_TEST( test_server_per_client_never_acking_subset_changes_nothing_for_the_others );
REGISTER_UNIT_TEST( test_server_per_client_flooding_subset_changes_nothing_for_the_others );
REGISTER_UNIT_TEST( test_server_per_client_mixed_subset_changes_nothing_for_the_others );
REGISTER_UNIT_TEST( test_server_per_client_mixed_subset_at_max_clients );

// A freed slot is clean (property X): a one-slot server whose client A builds
// up history (reliable messages with receipts unacknowledged, a send side
// filled until refused, messages of its own, then 1.5 s dark so it is held by
// probe pacing) and is dropped for cause: the server disconnects it, or it
// times out. A new client C takes the slot at once. Everything C and the
// server's slot show for 3 s of traffic (the messages C receives, the
// messages the server receives from the slot, the receipts read for the slot,
// the slot's stats and conditions, refusal counts on both ends) is identical
// to a control run in which C connects at the same time to a server that never
// had A.

#define REUSE_TICKS         30
#define REUSE_TIMEOUT       3
#define REUSE_RECEIPTS      8

struct reuse_record_t
{
    int client_received[REUSE_TICKS];
    int server_received[REUSE_TICKS];
    struct rocketnet_server_client_stats_t stats[REUSE_TICKS];
    struct rocketnet_conditions_t server_conditions[REUSE_TICKS];
    struct rocketnet_conditions_t client_conditions[REUSE_TICKS];
    int receipts[REUSE_TICKS];
    uint32_t receipt_serials[REUSE_TICKS][REUSE_RECEIPTS];
    int receipt_status[REUSE_TICKS][REUSE_RECEIPTS];
    int slot;
};

struct reuse_world_t
{
    fair_identity_t identity;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[2];   // A, C
    double time;
};

static void reuse_token( reuse_world_t * world, uint64_t client_id, char * token_string )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( world->identity.backend_secret, world->identity.server_public, FAIR_PROTOCOL, addresses, 1, client_id,
                                                     1000, 1ULL << 40, REUSE_TIMEOUT, NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

// one update of the pair: clients in `live` are updated and drained; the
// server is updated and drained (`from_slot` counts what it received)

static int reuse_frame( reuse_world_t * world, const bool * live, int * client_received )
{
    fair_now = world->time;
    for ( int i = 0; i < 2; i++ )
    {
        if ( !live[i] )
        {
            continue;
        }
        rocketnet_client_update( world->clients[i], world->time );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( world->clients[i] ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                if ( i == 1 )
                {
                    check( batch->bytes[m] == 4 && batch->data[m][0] == 0xC5 );    // only what the server sent C
                    ( *client_received )++;
                }
            }
        }
        rocketnet_client_send_packets( world->clients[i] );
    }

    int server_received = 0;
    rocketnet_server_update( world->server, world->time );
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
    {
        for ( int m = 0; m < batch->num_messages; m++ )
        {
            server_received += batch->data[m][0] == 0xCC ? 1 : 0;
            check( batch->data[m][0] == 0xCC || batch->data[m][0] == 0xAA );
        }
    }
    world->time += FAIR_DT;
    return server_received;
}

// cause 0: the server disconnects A; cause 1: A times out. with_a false is
// the control: the same clock, and C alone

static void reuse_run( bool with_a, int cause, reuse_record_t * record, int * drop_ticks )
{
    memset( record, 0, sizeof( *record ) );
    reuse_world_t world;
    memset( &world, 0, sizeof( world ) );
    rocketnet_quiet( 1 );
    world.time = 1000.0;
    fair_now = world.time;
    fair_identity( &world.identity );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = FAIR_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, world.identity.backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    server_config.send_message_buffer_bytes = 65536;
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( world.server != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_server_public_key( world.server, world.identity.server_public );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 2; i++ )
    {
        rocketnet_client_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = true;
        client_config.time_callback = fair_clock;
        status = ROCKETNET_STATUS_OK;
        world.clients[i] = rocketnet_client_create( NULL, &client_config, &status );
        check( world.clients[i] != NULL && status == ROCKETNET_STATUS_OK );
    }
    rocketnet_client_t * a = world.clients[0];
    rocketnet_client_t * c = world.clients[1];
    int unused = 0;

    // A's history, or the same updates with no client

    bool live[2] = { with_a, false };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( with_a )
    {
        reuse_token( &world, 0xA1, token );
        check( rocketnet_client_connect( a, token ) == ROCKETNET_STATUS_OK );
    }
    for ( int tick = 0; tick < 20; tick++ )
    {
        (void) reuse_frame( &world, live, &unused );
        rocketnet_server_send_packets( world.server );
    }
    if ( with_a )
    {
        check( rocketnet_client_state( a ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( world.server, 0 ) );

        // messages both ways, receipts asked for, some of them acknowledged

        for ( int tick = 0; tick < 5; tick++ )
        {
            for ( int k = 0; k < 4; k++ )
            {
                uint8_t * p = rocketnet_server_start_message( world.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 4 );
                check( p != NULL );
                if ( p )
                {
                    memset( p, 0xA5, 4 );
                    check( rocketnet_server_finish_message( world.server, 4 ) != 0 );
                }
                p = rocketnet_client_start_message( a, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
                check( p != NULL );
                if ( p )
                {
                    memset( p, 0xAA, 4 );
                    check( rocketnet_client_finish_message( a, 4 ) != 0 );
                }
            }
            (void) reuse_frame( &world, live, &unused );
            rocketnet_server_send_packets( world.server );
        }

        // A goes dark: what the server sends it now is never acknowledged,
        // and its send side to A is filled until a start is refused; A's own
        // sends wait in its queue

        live[0] = false;
        for ( int k = 0; k < 8; k++ )
        {
            uint8_t * p = rocketnet_client_start_message( a, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
            check( p != NULL );
            if ( p )
            {
                memset( p, 0xAA, 4 );
                check( rocketnet_client_finish_message( a, 4 ) != 0 );
            }
        }
        rocketnet_client_send_packets( a );     // in the hub, never read before the slot is C's
        for ( int tick = 0; tick < 15; tick++ )
        {
            for ( int k = 0; k < 4; k++ )
            {
                uint8_t * p = rocketnet_server_start_message( world.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 4 );
                if ( p )
                {
                    memset( p, 0xA5, 4 );
                    check( rocketnet_server_finish_message( world.server, 4 ) != 0 );
                }
            }
            (void) reuse_frame( &world, live, &unused );
            rocketnet_server_send_packets( world.server );
        }
        for ( int k = 0; k < 100000; k++ )
        {
            uint8_t * p = rocketnet_server_start_message( world.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000 );
            if ( !p )
            {
                break;
            }
            memset( p, 0xA5, 1000 );
            check( rocketnet_server_finish_message( world.server, 1000 ) != 0 );
        }

        struct rocketnet_conditions_t conditions;
        check( rocketnet_server_client_conditions( world.server, 0, &conditions, (int) sizeof( conditions ) ) );
        check( conditions.send_refused > 0 && conditions.reliable_in_flight > 0 && conditions.seconds_since_progress > 1.0 );
        struct rocketnet_server_client_stats_t stats;
        rocketnet_server_client_stats( world.server, 0, &stats );
        check( stats.packets_sent > 0 && stats.packets_received > 0 );
    }
    else
    {
        for ( int tick = 0; tick < 20; tick++ )
        {
            (void) reuse_frame( &world, live, &unused );
            rocketnet_server_send_packets( world.server );
        }
    }

    // A is dropped for cause (the control waits the same updates)

    if ( cause == 0 )
    {
        if ( with_a )
        {
            rocketnet_server_disconnect_client( world.server, 0 );
            check( !rocketnet_server_client_connected( world.server, 0 ) );
        }
    }
    else
    {
        // the run counts the updates A takes to time out; the control waits as many
        int ticks = 0;
        for ( ; with_a ? ( ticks < 10 * REUSE_TIMEOUT + 20 && rocketnet_server_client_connected( world.server, 0 ) ) : ticks < *drop_ticks; ticks++ )
        {
            (void) reuse_frame( &world, live, &unused );
            rocketnet_server_send_packets( world.server );
        }
        if ( with_a )
        {
            *drop_ticks = ticks;
        }
        if ( with_a )
        {
            check( !rocketnet_server_client_connected( world.server, 0 ) );
            check( rocketnet_server_disconnect_reason( world.server, 0 ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );
        }
    }

    // C takes the slot at once

    reuse_token( &world, 0xC1, token );
    check( rocketnet_client_connect( c, token ) == ROCKETNET_STATUS_OK );
    live[1] = true;
    for ( int tick = 0; tick < 400 && rocketnet_client_state( c ) != ROCKETNET_CLIENT_STATE_CONNECTED; tick++ )
    {
        (void) reuse_frame( &world, live, &unused );
        rocketnet_server_send_packets( world.server );
    }
    check( rocketnet_client_state( c ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    record->slot = rocketnet_client_index( c );
    check( record->slot == 0 );

    for ( int tick = 0; tick < REUSE_TICKS; tick++ )
    {
        uint8_t * p = rocketnet_server_start_message( world.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 4 );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0xC5, 4 );
            check( rocketnet_server_finish_message( world.server, 4 ) != 0 );
        }
        p = rocketnet_client_start_message( c, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0xCC, 4 );
            check( rocketnet_client_finish_message( c, 4 ) != 0 );
        }

        record->server_received[tick] = reuse_frame( &world, live, &record->client_received[tick] );
        rocketnet_server_send_packets( world.server );

        rocketnet_server_client_stats( world.server, 0, &record->stats[tick] );
        check( rocketnet_server_client_conditions( world.server, 0, &record->server_conditions[tick], (int) sizeof( record->server_conditions[tick] ) ) );
        check( rocketnet_client_conditions( c, &record->client_conditions[tick], (int) sizeof( record->client_conditions[tick] ) ) );
        struct rocketnet_receipt_t receipts[REUSE_RECEIPTS];
        memset( receipts, 0, sizeof( receipts ) );
        record->receipts[tick] = rocketnet_server_receipts( world.server, 0, receipts, REUSE_RECEIPTS );
        for ( int r = 0; r < REUSE_RECEIPTS && r < record->receipts[tick]; r++ )
        {
            record->receipt_serials[tick][r] = receipts[r].serial;
            record->receipt_status[tick][r] = (int) receipts[r].status;
        }
    }

    rn_test_client_stop_and_destroy( a );
    rn_test_client_stop_and_destroy( c );
    rn_test_server_stop_and_destroy( world.server );
}

static void reuse_case( int cause )
{
    static reuse_record_t control;
    static reuse_record_t run;
    int drop_ticks = 0;
    reuse_run( true, cause, &run, &drop_ticks );
    reuse_run( false, cause, &control, &drop_ticks );

    int received = 0;
    for ( int tick = 0; tick < REUSE_TICKS; tick++ )
    {
        received += run.client_received[tick];
    }
    check( received > 0 );
    check( memcmp( control.client_received, run.client_received, sizeof( control.client_received ) ) == 0 );
    check( memcmp( control.server_received, run.server_received, sizeof( control.server_received ) ) == 0 );
    check( memcmp( control.stats, run.stats, sizeof( control.stats ) ) == 0 );
    check( memcmp( control.server_conditions, run.server_conditions, sizeof( control.server_conditions ) ) == 0 );
    check( memcmp( control.client_conditions, run.client_conditions, sizeof( control.client_conditions ) ) == 0 );
    check( memcmp( control.receipts, run.receipts, sizeof( control.receipts ) ) == 0 );
    check( memcmp( control.receipt_serials, run.receipt_serials, sizeof( control.receipt_serials ) ) == 0 );
    check( memcmp( control.receipt_status, run.receipt_status, sizeof( control.receipt_status ) ) == 0 );
}

static void test_server_slot_reused_after_disconnect_shows_nothing_of_the_old_client()
{
    reuse_case( 0 );
}

static void test_server_slot_reused_after_timeout_shows_nothing_of_the_old_client()
{
    reuse_case( 1 );
}

REGISTER_UNIT_TEST( test_server_slot_reused_after_disconnect_shows_nothing_of_the_old_client );
REGISTER_UNIT_TEST( test_server_slot_reused_after_timeout_shows_nothing_of_the_old_client );


static void test_server_send_capacity_spare_slots_keep_in_place()
{
    fair_world_t world;

    // Keep fewer connected clients than configured clients: surplus is measured
    // against the one connected client, not the four configured slots. Four
    // send slots preserve that distinction without reserving 256 clients.

    fair_world_create( &world, 1, 4, 4 );
    fair_connect( &world, 0, 0x9100 );
    fair_update_clients( &world );
    fair_send_clients( &world );
    fair_server_update( &world );
    rocketnet_message_server_t * messages = rocketnet_server_message_server( world.server );
    const uint64_t before = rocketnet_counter_value( rocketnet_message_server_counters( messages ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );
    check( fair_server_send( &world, 0, FAIR_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE, 1 ) != 0 );
    check( rocketnet_counter_value( rocketnet_message_server_counters( messages ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == before );

    fair_world_destroy( &world );
}

REGISTER_UNIT_TEST( test_server_send_capacity_spare_slots_keep_in_place );
REGISTER_UNIT_TEST( test_server_send_cap_two_three_clients );
REGISTER_UNIT_TEST( test_server_send_cap_one_two_clients );
REGISTER_UNIT_TEST( test_server_send_cap_one_three_clients );
#endif // #if ROCKETNET_DEV

// The configuration test_server_send_capacity_default_covers_clients creates: the
// default, at ROCKETNET_MAX_CLIENTS clients, on the loopback transport. The case
// and its bound read it, so they cannot drift apart.
static void send_capacity_default_config( rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config_current( config );
    config->protocol_id = FAIR_PROTOCOL;
    config->max_clients = ROCKETNET_MAX_CLIENTS;
}

// test_server_send_capacity_default_covers_clients's peak resident bound. The case
// creates a full-capacity server transport, whose per-client memory is
// allocated up front by design, and the Windows Debug CRT fills every block
// it returns, so the whole plan is resident there. The bound is
// rocketnet_server_transport_memory_plan's total for the case's
// configuration, planned with the portable transport because the plan has no
// row for the loopback transport, plus the case's loopback hub
// (rocketnet_loopback_hub_bytes at ROCKETNET_TEST_LOOPBACK_HUB_CLIENTS),
// plus a stated 32 MiB margin for the process itself. A regression above the
// plan fails the case.
static long send_capacity_default_rss_bound_mb()
{
    rocketnet_server_transport_config_t config;
    send_capacity_default_config( &config );

    rocketnet_server_memory_plan_t plan;
    if ( !rocketnet_server_transport_memory_plan( &config, "portable", plan ) )
    {
        return 0;
    }

    const uint64_t hub = rocketnet_loopback_hub_bytes( ROCKETNET_TEST_LOOPBACK_HUB_CLIENTS ) + rocketnet_allocation_overhead();
    return suite_rss_bound_mb( plan.total_bytes + hub, 32 );
}

SUITE_CASE_RSS_BOUND( test_server_send_capacity_default_covers_clients, send_capacity_default_rss_bound_mb );

// The default capacity is sized from the configured client count, so a
// 1000-client server can start one payload frame for every client in one
// update. This reads the transport directly; no client and no clock.
static void test_server_send_capacity_default_covers_clients()
{
    rocketnet_server_transport_config_t config;
    send_capacity_default_config( &config );

    uint8_t backend_secret_key[64];
    check( rocketnet_generate_backend_keypair( config.backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( config.server_public_key, config.server_secret_key ) == ROCKETNET_STATUS_OK );

    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    struct rocketnet_server_transport_t * transport = rocketnet_server_transport_create_current( NULL, "127.0.0.1:0", &config );
    check( transport != NULL );
    check( rocketnet_server_transport_send_capacity( transport ) >= config.max_clients );

    rn_test_server_transport_stop_and_destroy( transport );
}

REGISTER_UNIT_TEST( test_server_send_capacity_default_covers_clients );
