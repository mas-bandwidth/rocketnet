/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A whole population ending at once (roadmap rows 392 and 418): the server's
// rocketnet_server_disconnect_all_clients over 1000 connected slots, and 1000
// clients calling rocketnet_client_disconnect in the same update. Both run
// over the in-process loopback transport with time as an argument starting at
// 1000.0; the bound on each call's work is a count the transport keeps under
// ROCKETNET_TEST_SEAMS, never wall time (tla/ServerSession.tla DisconnectAll,
// DrainDisconnectBurst).

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "portable_crt.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_client_io.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_client_protocol.h"
#include "../sdk/rocketnet_server_protocol.h"
#include "../sdk/rocketnet_test_seams.h"
#include "../sdk/rocketnet_memory.h"
#include "suite_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// the transport's count of disconnect packets sealed (development seam,
// sdk/rocketnet_server_transport.cpp)
uint64_t rocketnet_server_transport_disconnect_seals_for_test( struct rocketnet_server_transport_t * server_transport );

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

static const uint64_t MASS_DISCONNECT_PROTOCOL_ID = 0x4D41535344495343ULL;
static const int MASS_DISCONNECT_CLIENTS = 1000;

static int mass_disconnect_setenv( const char * name, const char * value )
{
    return rocketnet_test_env_setenv( name, value, 1 );
}

static int mass_disconnect_unsetenv( const char * name )
{
    return rocketnet_test_env_unsetenv( name );
}

// a ROCKETNET_SERVER_TRANSPORT or ROCKETNET_CLIENT_TRANSPORT pin would replace
// "loopback" (sdk/rocketnet_server_transport.cpp overlays the environment), so
// each case clears both names for its run and restores them at its end
struct mass_disconnect_env_guard_t
{
    static const int NUM_NAMES = 2;
    const char * names[NUM_NAMES] = { "ROCKETNET_SERVER_TRANSPORT", "ROCKETNET_CLIENT_TRANSPORT" };
    char saved[NUM_NAMES][256] = {};
    bool present[NUM_NAMES] = {};

    mass_disconnect_env_guard_t()
    {
        for ( int i = 0; i < NUM_NAMES; i++ )
        {
            char prior[4096];
            present[i] = rocketnet_test_env_getenv( names[i], prior, sizeof( prior ) ) && prior[0] != '\0';
            if ( present[i] )
            {
                check( strlen( prior ) < sizeof( saved[i] ) );
                memcpy( saved[i], prior, strlen( prior ) + 1 );
            }
            check( mass_disconnect_unsetenv( names[i] ) == 0 );
        }
    }

    ~mass_disconnect_env_guard_t() noexcept( false ) // check() throws under the expected-fail harness
    {
        for ( int i = 0; i < NUM_NAMES; i++ )
        {
            if ( present[i] )
                check( mass_disconnect_setenv( names[i], saved[i] ) == 0 );
            else
                check( mass_disconnect_unsetenv( names[i] ) == 0 );
        }
    }
};

struct mass_disconnect_keys_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
};

static void mass_disconnect_make_keys( mass_disconnect_keys_t * keys )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5A ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, keys->backend_public, keys->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( keys->server_public, keys->server_secret ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_t * mass_disconnect_server_create( const mass_disconnect_keys_t * keys, double now )
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.protocol_id = MASS_DISCONNECT_PROTOCOL_ID;
    config.max_clients = MASS_DISCONNECT_CLIENTS;
    memcpy( config.backend_public_key, keys->backend_public, sizeof( config.backend_public_key ) );
    memcpy( config.server_public_key, keys->server_public, sizeof( config.server_public_key ) );
    memcpy( config.server_secret_key, keys->server_secret, sizeof( config.server_secret_key ) );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );

    int status = -1;
    rocketnet_server_transport_t * transport = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, now, &status );
    check( transport != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_server_transport_set_test_timestamp( transport, 1000 );

    rn_message_link_t link;
    rn_message_server_link( &link, transport );
    rocketnet_message_server_t * running = (rocketnet_message_server_t *) rn_message_object_create(
        NULL, &link, 1, MASS_DISCONNECT_CLIENTS, NULL, NULL, 0,
        ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, now );
    check( running != NULL );

    rocketnet_server_t * server = rocketnet_server_adopt( NULL, running );
    check( server != NULL );
    return server;
}

static double mass_disconnect_fixed_clock( void * ) { return 1000.0; }

static rocketnet_client_t * mass_disconnect_client_create( const mass_disconnect_keys_t * keys, uint16_t port, uint64_t client_id )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( keys->backend_secret, keys->server_public,
                                                     MASS_DISCONNECT_PROTOCOL_ID, addresses, 1, client_id,
                                                     1000, 1300, 10, NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.step_driven = 1;
    config.time_callback = mass_disconnect_fixed_clock;
    config.game_clock_epoch = 1000;

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_current(
        NULL, token, &config, 0.0, rocketnet_loopback_transport.client, NULL );
    check( transport != NULL );

    rn_message_link_t link;
    rn_message_client_link( &link, transport );
    rocketnet_message_client_t * connection = (rocketnet_message_client_t *) rn_message_object_create(
        NULL, &link, 0, 1, NULL, NULL, 0,
        ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    check( connection != NULL );

    rocketnet_client_t * client = rocketnet_client_adopt( NULL, connection );
    check( client != NULL );
    return client;
}

static rocketnet_server_transport_t * mass_disconnect_server_transport( rocketnet_server_t * server )
{
    return rocketnet_message_server_transport( rocketnet_server_message_server( server ) );
}

static const uint64_t * mass_disconnect_server_counters( rocketnet_server_t * server )
{
    return rocketnet_server_transport_counters( mass_disconnect_server_transport( server ) )->values;
}

static void mass_disconnect_client_step( rocketnet_client_t * client, double time )
{
    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( client ) );
    if ( !rocketnet_client_transport_client_io( transport ) )
        rocketnet_client_transport_step_legacy_for_test( transport, time );
    rocketnet_client_update( client, time );
    rocketnet_client_send_packets( client );
}

// The population: one server of MASS_DISCONNECT_CLIENTS slots and as many
// clients, every one connected, on a clock that started at 1000.0.
struct mass_disconnect_population_t
{
    mass_disconnect_keys_t keys;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[MASS_DISCONNECT_CLIENTS];
    double time;
};

static void mass_disconnect_connect_all( mass_disconnect_population_t * population )
{
    mass_disconnect_make_keys( &population->keys );
    population->time = 1000.0;
    population->server = mass_disconnect_server_create( &population->keys, population->time );
    const uint16_t port = rocketnet_server_port( population->server );
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
        population->clients[i] = mass_disconnect_client_create( &population->keys, port, 0x10000 + (uint64_t) i );

    for ( int step = 0; step < 400; step++ )
    {
        population->time += 0.01;
        for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
            mass_disconnect_client_step( population->clients[i], population->time );
        rocketnet_server_update( population->server, population->time );
        rocketnet_server_send_packets( population->server );
        int connected = 0;
        for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
            connected += rocketnet_client_state( population->clients[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED ? 1 : 0;
        if ( connected == MASS_DISCONNECT_CLIENTS && rocketnet_server_num_connected_clients( population->server ) == MASS_DISCONNECT_CLIENTS )
            return;
    }
    check( false );
}

static void mass_disconnect_destroy_all( mass_disconnect_population_t * population )
{
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
        rn_test_client_stop_and_destroy( population->clients[i] );
    rn_test_server_stop_and_destroy( population->server );
}

// The population's planned memory: the server transport's plan (planned with
// the portable transport, as the loopback transport has no row of its own),
// the message server's block for its slots, the loopback server queue sized
// from the plan, and for each client its transport's plan, its message
// client block and its loopback queue, plus a stated 32 MiB margin for the
// process. A regression above the plan fails the case.
static long mass_disconnect_rss_bound_mb()
{
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config( &server_config );
    server_config.max_clients = MASS_DISCONNECT_CLIENTS;
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    const uint64_t server_bytes = rocketnet_server_transport_required_memory( &server_config, "portable" );
    const uint64_t client_bytes = rocketnet_client_transport_required_memory( &client_config, "portable" );
    const uint64_t server_object = rn_message_object_required_bytes( NULL, MASS_DISCONNECT_CLIENTS, 1 );
    const uint64_t client_object = rn_message_object_required_bytes( NULL, 1, 0 );
    if ( !server_bytes || !client_bytes || !server_object || !client_object )
        return 0;                       // a refused plan keeps the suite-wide bound
    const uint64_t overhead = (uint64_t) rocketnet_allocation_overhead();
    const uint64_t server_queue = rocketnet_loopback_queue_bytes( rocketnet_loopback_server_queue_packets( MASS_DISCONNECT_CLIENTS ) ) + overhead;
    const uint64_t client_queue = rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS ) + overhead;
    uint64_t planned = 0;
    if ( !rocketnet_memory_add( planned, 1, server_bytes ) || !rocketnet_memory_add( planned, 1, server_object ) ||
         !rocketnet_memory_add( planned, 1, server_queue ) ||
         !rocketnet_memory_add( planned, (uint64_t) MASS_DISCONNECT_CLIENTS, client_bytes + client_object + client_queue ) )
        return 0;
    return suite_rss_bound_mb( planned, 32 );
}

SUITE_CASE_RSS_BOUND( test_server_disconnect_all_1000_real_time, mass_disconnect_rss_bound_mb );
SUITE_CASE_RSS_BOUND( test_clients_disconnect_together_server_count_exact, mass_disconnect_rss_bound_mb );

// rocketnet_server_disconnect_all_clients over 1000 connected slots: the call
// ends every slot at once and seals at most one update's allowance; each
// following update seals at most ROCKETNET_SERVER_DISCONNECT_SEALS_PER_UPDATE
// disconnect packets, so no
// call's work grows with the population, and every client still receives the
// server's disconnect and ends SERVER_DISCONNECTED, never on its timeout.
static void test_server_disconnect_all_1000_real_time()
{
    mass_disconnect_env_guard_t env_guard;
    rocketnet_quiet( 1 );
    static mass_disconnect_population_t population;
    mass_disconnect_connect_all( &population );
    rocketnet_server_t * server = population.server;
    rocketnet_server_transport_t * transport = mass_disconnect_server_transport( server );
    const uint64_t * counters = mass_disconnect_server_counters( server );
    const uint64_t timed_out_before = counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];

    // drain what the connect handshakes left queued before the pass
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
        mass_disconnect_client_step( population.clients[i], population.time );

    const uint64_t seals_before = rocketnet_server_transport_disconnect_seals_for_test( transport );
    rocketnet_server_disconnect_all_clients( server );
    const uint64_t call_seals = rocketnet_server_transport_disconnect_seals_for_test( transport ) - seals_before;
    check( call_seals <= (uint64_t) ROCKETNET_SERVER_DISCONNECT_SEALS_PER_UPDATE );

    // the slot state ends in the same call (tla/ServerSession.tla DisconnectAll)
    check( rocketnet_server_num_connected_clients( server ) == 0 );
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
    {
        check( !rocketnet_server_client_connected( server, i ) );
        check( rocketnet_server_disconnect_reason( server, i ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    // the bursts leave over the following updates, a bounded number of seals in each
    const uint64_t total = (uint64_t) MASS_DISCONNECT_CLIENTS * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
    const int max_updates = (int) ( ( total + ROCKETNET_SERVER_DISCONNECT_SEALS_PER_UPDATE - 1 ) / ROCKETNET_SERVER_DISCONNECT_SEALS_PER_UPDATE );
    uint64_t sealed = call_seals;
    int updates = 0;
    while ( sealed < total )
    {
        check( updates < max_updates );
        population.time += 0.01;
        const uint64_t before = rocketnet_server_transport_disconnect_seals_for_test( transport );
        rocketnet_server_update( server, population.time );
        rocketnet_server_send_packets( server );
        const uint64_t step = rocketnet_server_transport_disconnect_seals_for_test( transport ) - before;
        check( step <= (uint64_t) ROCKETNET_SERVER_DISCONNECT_SEALS_PER_UPDATE );
        sealed += step;
        updates++;
        for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
            mass_disconnect_client_step( population.clients[i], population.time );
    }
    check( sealed == total );
    check( rocketnet_server_num_connected_clients( server ) == 0 );

    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
        check( rocketnet_client_state( population.clients[i] ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    check( counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == timed_out_before );

    mass_disconnect_destroy_all( &population );
}

REGISTER_UNIT_TEST( test_server_disconnect_all_1000_real_time );

// 1000 clients call rocketnet_client_disconnect in the same update: every
// client's redundant burst fits the server's loopback queue, sized from the
// plan, and the server's connected count is exactly 0 within
// ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS updates, every slot ended by its
// client's disconnect and none on the timeout.
static void test_clients_disconnect_together_server_count_exact()
{
    mass_disconnect_env_guard_t env_guard;
    rocketnet_quiet( 1 );
    static mass_disconnect_population_t population;
    mass_disconnect_connect_all( &population );
    rocketnet_server_t * server = population.server;
    const uint64_t * counters = mass_disconnect_server_counters( server );
    const uint64_t timed_out_before = counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];
    const uint64_t accepted_before = counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];

    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
        rocketnet_client_disconnect( population.clients[i] );
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
        rn_test_client_wait_disconnected( population.clients[i] );

    int updates = 0;
    while ( rocketnet_server_num_connected_clients( server ) != 0 )
    {
        check( updates < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
        population.time += 0.01;
        rocketnet_server_update( server, population.time );
        rocketnet_server_send_packets( server );
        updates++;
    }
    check( rocketnet_server_num_connected_clients( server ) == 0 );
    check( counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == timed_out_before );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] - accepted_before == (uint64_t) MASS_DISCONNECT_CLIENTS );

    mass_disconnect_destroy_all( &population );
}

REGISTER_UNIT_TEST( test_clients_disconnect_together_server_count_exact );
