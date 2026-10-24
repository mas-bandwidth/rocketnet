/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The loopback wire's frame order (tla/RNLoopbackWire.tla SentIsVisible): a
// client in its default mode, not step-driven, on the loopback transport runs
// no thread of its own, and a message it sends in send_packets is taken by
// the server's update that follows in the same frame. The server's message
// back reaches the client's next update. A disconnect completes in the call,
// and its burst still reaches the server after the client is destroyed. One
// thread, no socket, and time is an argument starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "token_string.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check
#define SAME_FRAME_PROTOCOL 0x5A3EF8A3E0000001ULL
#define SAME_FRAME_DT ( 1.0 / 60.0 )
#define SAME_FRAME_FRAMES 120

static double same_frame_now = 1000.0;
static double same_frame_clock( void * ) { return same_frame_now; }

struct same_frame_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
};

static void same_frame_world_create( same_frame_world_t * world )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    same_frame_now = 1000.0;
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5F ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = SAME_FRAME_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world->server != NULL );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    // the default mode: the config does not ask for step_driven
    rocketnet_client_test_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.time_callback = same_frame_clock;
    check( !client_config.step_driven );
    world->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( world->client != NULL );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( world->backend_secret, world->server, SAME_FRAME_PROTOCOL, addresses, 1, 0x5F01,
        1000, 1ULL << 40, 30, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( world->client, token ) == ROCKETNET_STATUS_OK );
}

static void same_frame_write( uint8_t * buffer, uint32_t frame )
{
    buffer[0] = (uint8_t) frame;
    buffer[1] = (uint8_t) ( frame >> 8 );
    buffer[2] = (uint8_t) ( frame >> 16 );
    buffer[3] = (uint8_t) ( frame >> 24 );
}

static uint32_t same_frame_read( const uint8_t * buffer )
{
    return (uint32_t) buffer[0] | ( (uint32_t) buffer[1] << 8 ) | ( (uint32_t) buffer[2] << 16 ) | ( (uint32_t) buffer[3] << 24 );
}

// one frame in the game's order, client first, then server: update,
// receive, queue, send. Returns the client's messages the server took and
// counts the frames each one and each state update waited.
static void same_frame_step( same_frame_world_t * world, uint32_t frame, bool send, int * to_server, int * to_server_late, int * to_client, int * to_client_late )
{
    const double time = 1000.0 + frame * SAME_FRAME_DT;
    same_frame_now = time;

    rocketnet_client_update( world->client, time );
    const struct rocketnet_message_batch_t * batch;
    while ( ( batch = rocketnet_client_receive_messages( world->client ) ) != NULL )
    {
        for ( int m = 0; m < batch->num_messages; m++ )
        {
            check( batch->bytes[m] == 4 );
            ( *to_client )++;
            if ( frame - same_frame_read( batch->data[m] ) != 1 )
                ( *to_client_late )++;
        }
    }
    uint8_t * buffer;
    if ( send && ( buffer = rocketnet_client_start_message( world->client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 4 ) ) != NULL )
    {
        same_frame_write( buffer, frame );
        check( rocketnet_client_finish_message( world->client, 4 ) != 0 );
    }
    rocketnet_client_send_packets( world->client );

    rocketnet_server_update( world->server, time );
    while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
    {
        for ( int m = 0; m < batch->num_messages; m++ )
        {
            check( batch->bytes[m] == 4 );
            ( *to_server )++;
            if ( same_frame_read( batch->data[m] ) != frame )
                ( *to_server_late )++;
        }
    }
    if ( send && ( buffer = rocketnet_server_start_message( world->server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 4 ) ) != NULL )
    {
        same_frame_write( buffer, frame );
        check( rocketnet_server_finish_message( world->server, 4 ) != 0 );
    }
    rocketnet_server_send_packets( world->server );
}

static void test_loopback_client_send_reaches_server_same_frame()
{
    same_frame_world_t world;
    same_frame_world_create( &world );

    int to_server = 0, to_server_late = 0, to_client = 0, to_client_late = 0;
    uint32_t frame = 0;
    for ( ; frame < 300 && rocketnet_client_state( world.client ) != ROCKETNET_CLIENT_STATE_CONNECTED; frame++ )
        same_frame_step( &world, frame, false, &to_server, &to_server_late, &to_client, &to_client_late );
    check( rocketnet_client_state( world.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    same_frame_step( &world, frame++, false, &to_server, &to_server_late, &to_client, &to_client_late );
    check( rocketnet_server_client_connected( world.server, 0 ) );

    const uint32_t first = frame;
    for ( ; frame < first + SAME_FRAME_FRAMES; frame++ )
        same_frame_step( &world, frame, true, &to_server, &to_server_late, &to_client, &to_client_late );

    // every frame's client message was taken by that frame's server update,
    // and every state update by the client's next one (the last is still in
    // flight)
    check( to_server == SAME_FRAME_FRAMES );
    check( to_server_late == 0 );
    check( to_client == SAME_FRAME_FRAMES - 1 );
    check( to_client_late == 0 );

    // disconnect completes in the call: no update, no worker
    rocketnet_client_disconnect( world.client );
    check( rocketnet_client_disconnected( world.client ) );
    rocketnet_client_destroy( world.client );
    world.client = NULL;

    // the disconnect burst outlives its sender: the server's next update
    // takes it from its own inbox and frees the slot
    rocketnet_server_update( world.server, 1000.0 + frame * SAME_FRAME_DT );
    check( !rocketnet_server_client_connected( world.server, 0 ) );

    rn_test_server_stop_and_destroy( world.server );
}
REGISTER_UNIT_TEST( test_loopback_client_send_reaches_server_same_frame );
