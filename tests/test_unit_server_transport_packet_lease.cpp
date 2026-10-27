/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A server packet started, then retired by send_packets or stop, then finished.
// Loopback, a step-driven client and a caller clock: no socket, no thread, no
// sleep, no real time. Development only, because a production client starts an
// I/O thread and this case must not.

#include "unit_shutdown.h"
#include "token_string.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_testing.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

#define LEASE_MISS_PROTOCOL 0xF1115A1E00000001ULL

static double g_lease_now = 1000.0;
static double lease_clock( void * ) { return g_lease_now; }

struct lease_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    double time;
};

static void lease_pump( lease_world_t * world )
{
    g_lease_now = world->time;
    rocketnet_client_update( world->client, world->time );
    while ( rocketnet_client_receive_messages( world->client ) != NULL ) {}
    rocketnet_client_send_packets( world->client );
    rocketnet_server_update( world->server, world->time );
    while ( rocketnet_server_receive_messages( world->server ) != NULL ) {}
    rocketnet_server_send_packets( world->server );
    world->time += 0.01;
}

static void lease_world_create( lease_world_t * world )
{
    memset( world, 0, sizeof( *world ) );
    world->time = 1000.0;
    g_lease_now = world->time;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0xA5 ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = LEASE_MISS_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( world->server != NULL && status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = lease_clock;
    world->client = rocketnet_client_create( NULL, &client_config, &status );
    check( world->client != NULL && status == ROCKETNET_STATUS_OK );
}

static void lease_world_connect( lease_world_t * world )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( world->backend_secret, world->server, LEASE_MISS_PROTOCOL, addresses, 1,
                                        701, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rn_test_client_connect_after_completion( world->client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( world->client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        lease_pump( world );
    check( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int index = rocketnet_client_index( world->client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world->server, index ); i++ )
        lease_pump( world );
    check( rocketnet_server_client_connected( world->server, index ) );
}

static rocketnet_server_transport_t * lease_transport( lease_world_t * world )
{
    rocketnet_message_server_t * message = rocketnet_server_message_server( world->server );
    check( message != NULL );
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( message );
    check( transport != NULL );
    return transport;
}

static void lease_world_destroy( lease_world_t * world )
{
    rn_test_client_stop_and_destroy( world->client );
    rn_test_server_stop_and_destroy( world->server );
}

// Finish after the flush that resets the lease. The caller still holds the
// context start_packet returned. Return 0 and one api_misuses count; do not
// trap. A debug-only assert here is the defect this case measures.
static void test_server_transport_finish_after_flush()
{
    lease_world_t world;
    lease_world_create( &world );
    lease_world_connect( &world );

    rocketnet_server_transport_t * transport = lease_transport( &world );
    const int slot = rocketnet_client_index( world.client );
    rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( transport, slot, 16, &context );
    check( frame != NULL );
    memset( frame + context.payload_index, 0x5A, 16 );

    rocketnet_server_transport_send_packets( transport );

    const uint64_t misuses = rocketnet_api_misuse_count();
    check( rocketnet_server_transport_finish_packet( transport, &context, 16 ) == 0 );
    check( context.frame == NULL );
    check( rocketnet_api_misuse_count() == misuses + 1 );

    lease_world_destroy( &world );
}

// Finish after stop resets the lease. Same refusal as the flush: visible in
// every build, not an assert that disappears when ROCKETNET_ASSERTS is off.
static void test_server_transport_finish_after_stop()
{
    lease_world_t world;
    lease_world_create( &world );
    lease_world_connect( &world );

    rocketnet_server_transport_t * transport = lease_transport( &world );
    const int slot = rocketnet_client_index( world.client );
    rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( transport, slot, 16, &context );
    check( frame != NULL );
    memset( frame + context.payload_index, 0x5B, 16 );

    rocketnet_server_transport_stop( transport );
    check( rocketnet_server_transport_stopped( transport ) );

    const uint64_t misuses = rocketnet_api_misuse_count();
    check( rocketnet_server_transport_finish_packet( transport, &context, 16 ) == 0 );
    check( context.frame == NULL );
    check( rocketnet_api_misuse_count() == misuses + 1 );

    lease_world_destroy( &world );
}

REGISTER_UNIT_TEST( test_server_transport_finish_after_flush );
REGISTER_UNIT_TEST( test_server_transport_finish_after_stop );

#endif // #if ROCKETNET_DEV
