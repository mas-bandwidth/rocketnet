/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Server transport packet lease reset contract (proofs/tla/RNDriverSendStaging.tla):
// finishing a packet after its lease was reset by send_packets, update or stop
// is caller misuse: rocketnet_server_transport_finish_packet refuses with 0 in
// every build, counts one api_misuse, logs the reason, and does not hit
// rocketnet_assert(false). No real time, sockets or threads.

#include "test_unit_registry.h"
#include "native_shutdown.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_driver.h"
#include "rocketnet_fragment.h"
#include "rocketnet_layers.h"

#include <stdio.h>
#include <string.h>
#include "rocketnet_config.h"
#include "loopback_hub.h"

#define check rocketnet_unit_check

#define LEASE_TEST_PROTOCOL 0x7A11EA5E5B000001ULL
#define LEASE_TEST_EPOCH    1700000000ULL

static int g_lease_test_asserts = 0;
static void lease_test_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    fprintf( stderr, "assert failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stderr );
    g_lease_test_asserts++;
}

struct lease_assert_scope_t
{
    rocketnet_assert_function_t saved;

    lease_assert_scope_t()
    {
        saved = rn_assert_function;
        g_lease_test_asserts = 0;
        rocketnet_assert_function( lease_test_assert_hook );
    }

    ~lease_assert_scope_t()
    {
        rocketnet_assert_function( saved );
    }
};

struct lease_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[32];
    rocketnet_server_transport_t * server;
    rocketnet_client_transport_io_t client_io;
    rocketnet_client_protocol_t client;
    rocketnet_counters_t client_counters;
    rocketnet_reassembly_set_t * client_reassembly;
    uint8_t last_rx_payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int last_rx_payload_bytes;
    double time;
};

static void lease_world_pump( lease_world_t * world, double dt = 0.01 )
{
    uint8_t pkt[ROCKETNET_RECEIVE_CEILING];
    int pkt_bytes = 0;
    rocketnet_client_protocol_update( &world->client, world->time, pkt, &pkt_bytes );
    if ( pkt_bytes > 0 )
        world->client_io.operations->send( world->client_io.instance, &world->client.server_address, pkt, pkt_bytes );

    rocketnet_server_transport_update( world->server, world->time );
    rocketnet_server_transport_send_packets( world->server );

    rocketnet_address_t from;
    while ( ( pkt_bytes = world->client_io.operations->receive( world->client_io.instance, &from, pkt, sizeof( pkt ) ) ) > 0 )
    {
        uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        int payload_bytes = 0;
        if ( rocketnet_client_protocol_process_packet( &world->client, pkt, pkt_bytes, world->time,
                                                       payload, sizeof( payload ), &payload_bytes ) &&
             payload_bytes > 0 )
        {
            memcpy( world->last_rx_payload, payload, (size_t) payload_bytes );
            world->last_rx_payload_bytes = payload_bytes;
        }
    }

    world->time += dt;
}

static int lease_world_create( lease_world_t * world, uint8_t seed_tag )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( seed_tag ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( world->server_public, world->server_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = LEASE_TEST_PROTOCOL;
    server_config.max_clients = 2;
    server_config.game_clock_epoch = LEASE_TEST_EPOCH;
    memset( server_config.packet_filter, 0x5A, sizeof( server_config.packet_filter ) );
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, world->server_public, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, world->server_secret, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    int status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:0", &server_config, world->time, &status );
    check( world->server != NULL );
    check( status == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", rocketnet_server_transport_port( world->server ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const uint64_t now_ts = LEASE_TEST_EPOCH + (uint64_t) world->time;
    check( rocketnet_write_production_connect_token( world->backend_secret, world->server_public,
                                                     LEASE_TEST_PROTOCOL, addresses, 1, 0x1EA5E01ULL,
                                                     now_ts, now_ts + 300, 30, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    const rocketnet_transport_definition_t * loopback_def = rocketnet_transport_definition( ROCKETNET_TRANSPORT_LOOPBACK );
    check( loopback_def != NULL && loopback_def->backend != NULL && loopback_def->backend->client != NULL );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    memset( client_config.packet_filter, 0x5A, sizeof( client_config.packet_filter ) );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    rocketnet_client_transport_io_config_t io_config = { "loopback", &client_config, NULL };
    rocketnet_address_t client_bind;
    check( rocketnet_address_parse( &client_bind, "127.0.0.1:0" ) );
    check( rocketnet_client_transport_io_open( loopback_def->backend->client, NULL, &io_config, &client_bind, &world->client_io ) );

    world->client_reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, 4 );
    check( world->client_reassembly != NULL );
    rocketnet_client_protocol_init( &world->client, &world->client_counters );
    world->client.reassembly = world->client_reassembly;
    memcpy( world->client.packet_filter, server_config.packet_filter, ROCKETNET_FILTER_VALUE_BYTES );

    check( rocketnet_client_protocol_connect( &world->client, token, world->time, now_ts ) != 0 );

    for ( int i = 0; i < 400; i++ )
    {
        if ( world->client.state == ROCKETNET_CLIENT_STATE_CONNECTED &&
             world->client.client_index >= 0 &&
             rocketnet_server_transport_is_client_connected( world->server, world->client.client_index ) )
            break;
        lease_world_pump( world );
    }
    check( world->client.state == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int slot = world->client.client_index;
    check( slot >= 0 );
    check( rocketnet_server_transport_is_client_connected( world->server, slot ) != 0 );
    return slot;
}

static void lease_world_destroy( lease_world_t * world )
{
    rocketnet_client_transport_io_close( &world->client_io );
    rocketnet_reassembly_set_destroy( world->client_reassembly );
    rn_native_server_transport_stop_and_destroy( world->server );
}

// 1. A packet started before a send flush (send_packets or update) and finished
//    after it is refused as caller misuse: finish returns 0, increments
//    api_misuses, and never triggers rocketnet_assert(false). A subsequent
//    valid start and finish still delivers.
static void test_server_transport_finish_after_flush()
{
    lease_world_t world;
    const int slot = lease_world_create( &world, 0x61 );
    rocketnet_server_transport_t * server = world.server;

    {
        lease_assert_scope_t scope;

        // 1a. Start a packet, flush via send_packets (which resets packet_leases),
        //     then finish the stale context: returns 0, counts api_misuse, no assert.
        rocketnet_packet_context_t ctx_a;
        uint8_t * frame_a = rocketnet_server_transport_start_packet( server, slot, 16, &ctx_a );
        check( frame_a != NULL );
        memset( frame_a + ctx_a.payload_index, 0xAA, 16 );

        rocketnet_server_transport_send_packets( server );

        const uint64_t misuses_before_a = rocketnet_api_misuse_count();
        check( rocketnet_server_transport_finish_packet( server, &ctx_a, 16 ) == 0 );
        check( g_lease_test_asserts == 0 );
        check( rocketnet_api_misuse_count() == misuses_before_a + 1 );

        // 1b. Start a packet, flush via update (which also resets packet_leases),
        //     then finish the stale context: returns 0, counts api_misuse, no assert.
        rocketnet_packet_context_t ctx_b;
        uint8_t * frame_b = rocketnet_server_transport_start_packet( server, slot, 16, &ctx_b );
        check( frame_b != NULL );
        memset( frame_b + ctx_b.payload_index, 0xBB, 16 );

        world.time += 0.01;
        rocketnet_server_transport_update( server, world.time );

        const uint64_t misuses_before_b = rocketnet_api_misuse_count();
        check( rocketnet_server_transport_finish_packet( server, &ctx_b, 16 ) == 0 );
        check( g_lease_test_asserts == 0 );
        check( rocketnet_api_misuse_count() == misuses_before_b + 1 );

        // 1c. A subsequent valid start and finish succeeds and delivers.
        world.last_rx_payload_bytes = 0;
        rocketnet_packet_context_t ctx_ok;
        uint8_t * frame_ok = rocketnet_server_transport_start_packet( server, slot, 16, &ctx_ok );
        check( frame_ok != NULL );
        memset( frame_ok + ctx_ok.payload_index, 0xCC, 16 );
        const int wire_bytes = rocketnet_server_transport_finish_packet( server, &ctx_ok, 16 );
        check( wire_bytes > 0 );
        check( g_lease_test_asserts == 0 );
        rocketnet_server_transport_send_packets( server );

        for ( int step = 0; step < 20 && world.last_rx_payload_bytes == 0; step++ )
            lease_world_pump( &world );
        check( world.last_rx_payload_bytes == 16 && world.last_rx_payload[0] == 0xCC && world.last_rx_payload[15] == 0xCC );

        // 1d. A lease already consumed by finish_packet or abort_packet is in
        //     state 3, not an active lease retired by reset: after a later
        //     reset (send_packets or update), finishing a saved copy of that
        //     context must NOT be treated as retired(), must NOT increment
        //     api_misuses, and must assert in assert-enabled builds.
        rocketnet_packet_context_t ctx_fin;
        uint8_t * frame_fin = rocketnet_server_transport_start_packet( server, slot, 16, &ctx_fin );
        check( frame_fin != NULL );
        memset( frame_fin + ctx_fin.payload_index, 0xCE, 16 );
        rocketnet_packet_context_t saved_fin = ctx_fin;
        check( rocketnet_server_transport_finish_packet( server, &ctx_fin, 16 ) > 0 );
        check( g_lease_test_asserts == 0 );
        rocketnet_server_transport_send_packets( server );
        const uint64_t misuses_before_fin = rocketnet_api_misuse_count();
        check( rocketnet_server_transport_finish_packet( server, &saved_fin, 16 ) == 0 );
        check( saved_fin.frame == NULL );
        check( rocketnet_api_misuse_count() == misuses_before_fin );
#if ROCKETNET_ASSERTS
        check( g_lease_test_asserts == 1 );
#else
        check( g_lease_test_asserts == 0 );
#endif
        g_lease_test_asserts = 0;

        rocketnet_packet_context_t ctx_abt;
        uint8_t * frame_abt = rocketnet_server_transport_start_packet( server, slot, 16, &ctx_abt );
        check( frame_abt != NULL );
        rocketnet_packet_context_t saved_abt = ctx_abt;
        rocketnet_server_transport_abort_packet( server, &ctx_abt );
        check( g_lease_test_asserts == 0 );
        rocketnet_server_transport_send_packets( server );
        const uint64_t misuses_before_abt = rocketnet_api_misuse_count();
        check( rocketnet_server_transport_finish_packet( server, &saved_abt, 16 ) == 0 );
        check( saved_abt.frame == NULL );
        check( rocketnet_api_misuse_count() == misuses_before_abt );
#if ROCKETNET_ASSERTS
        check( g_lease_test_asserts == 1 );
#else
        check( g_lease_test_asserts == 0 );
#endif
        g_lease_test_asserts = 0;
    }

    lease_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_transport_finish_after_flush );

// 2. A packet started before rocketnet_server_transport_stop and finished after
//    it is refused as caller misuse: finish returns 0, increments api_misuses,
//    and never triggers rocketnet_assert(false).
static void test_server_transport_finish_after_stop()
{
    lease_world_t world;
    const int slot = lease_world_create( &world, 0x62 );
    rocketnet_server_transport_t * server = world.server;

    {
        lease_assert_scope_t scope;

        rocketnet_packet_context_t ctx;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, slot, 16, &ctx );
        check( frame != NULL );
        memset( frame + ctx.payload_index, 0xDD, 16 );

        rocketnet_server_transport_stop( server );
        rn_native_server_transport_wait_stopped( server );

        const uint64_t misuses_before = rocketnet_api_misuse_count();
        check( rocketnet_server_transport_finish_packet( server, &ctx, 16 ) == 0 );
        check( g_lease_test_asserts == 0 );
        check( rocketnet_api_misuse_count() == misuses_before + 1 );
    }

    lease_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_transport_finish_after_stop );
