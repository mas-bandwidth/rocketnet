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
int rocketnet_server_transport_disconnect_burst_key_for_test( struct rocketnet_server_transport_t * server_transport,
    int index, uint8_t * out, int out_bytes, int * head, int * count );

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

static const uint64_t MASS_DISCONNECT_PROTOCOL_ID = 0x4D41535344495343ULL;
static const int MASS_DISCONNECT_CLIENTS = 1000;

#if ROCKETNET_DEV
static int mass_disconnect_setenv( const char * name, const char * value )
{
    return rocketnet_test_env_setenv( name, value, 1 );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static int mass_disconnect_unsetenv( const char * name )
{
    return rocketnet_test_env_unsetenv( name );
}
#endif // #if ROCKETNET_DEV

// a ROCKETNET_SERVER_TRANSPORT or ROCKETNET_CLIENT_TRANSPORT pin would replace
// "loopback" (sdk/rocketnet_server_transport.cpp overlays the environment), so
// each case clears both names for its run and restores them at its end
#if ROCKETNET_DEV
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
#endif // #if ROCKETNET_DEV

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

static rocketnet_server_t * mass_disconnect_server_create( const mass_disconnect_keys_t * keys, double now, int disconnect_packets_per_update )
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.protocol_id = MASS_DISCONNECT_PROTOCOL_ID;
    config.max_clients = MASS_DISCONNECT_CLIENTS;
    config.disconnect_packets_per_update = disconnect_packets_per_update;
    memcpy( config.backend_public_key, keys->backend_public, sizeof( config.backend_public_key ) );
    memcpy( config.server_public_key, keys->server_public, sizeof( config.server_public_key ) );
    memcpy( config.server_secret_key, keys->server_secret, sizeof( config.server_secret_key ) );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );

    int status = -1;
    rocketnet_server_transport_t * transport = rocketnet_server_transport_create_resolved( NULL, "127.0.0.1:0", &config, 0, &status );
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

static void mass_disconnect_connect_all( mass_disconnect_population_t * population, int disconnect_packets_per_update = ROCKETNET_DEFAULT_SERVER_DISCONNECT_PACKETS_PER_UPDATE )
{
    mass_disconnect_make_keys( &population->keys );
    population->time = 1000.0;
    population->server = mass_disconnect_server_create( &population->keys, population->time, disconnect_packets_per_update );
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
    {
        if ( population->clients[i] )
            rn_test_client_stop_and_destroy( population->clients[i] );
    }
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
SUITE_CASE_RSS_BOUND( test_server_disconnect_full_queue_never_bypasses_budget, mass_disconnect_rss_bound_mb );
SUITE_CASE_RSS_BOUND( test_server_disconnect_prelink_kicks_share_update_allowance, mass_disconnect_rss_bound_mb );

static int mass_disconnect_asserts;
static int mass_disconnect_key_nonzero( const uint8_t key[ROCKETNET_KEY_BYTES] );

static void mass_disconnect_assert_hook( const char *, const char *, const char *, int )
{
    mass_disconnect_asserts++;
}

// Message-layer misuse kicks run before the transport phase of the same
// public server update. They and the queued drain must spend one shared
// allowance, rather than the transport phase refilling it in the middle.
static void test_server_disconnect_prelink_kicks_share_update_allowance()
{
#if ROCKETNET_DEV
    mass_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    rocketnet_quiet( 1 );
    static mass_disconnect_population_t population;
    const int allowance = 64; // independent expected default, not the implementation constant
    const int kicked = 13;
    mass_disconnect_connect_all( &population );
    rocketnet_server_transport_t * transport = mass_disconnect_server_transport( population.server );

    for ( int i = 0; i < kicked; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( population.clients[i], 0,
            ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
        check( out != NULL );
        out[0] = (uint8_t) i;
        check( rocketnet_client_finish_message( population.clients[i], 1 ) != 0 );
        rocketnet_client_send_packets( population.clients[i] );
    }

    // This update admits the messages into the server receive queue. Leaving
    // them there makes the following update kick all thirteen connections.
    population.time += 0.01;
    rocketnet_server_update( population.server, population.time );
    rocketnet_server_send_packets( population.server );
    check( rocketnet_server_num_connected_clients( population.server ) == MASS_DISCONNECT_CLIENTS );

    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( mass_disconnect_assert_hook );
    mass_disconnect_asserts = 0;
    const uint64_t before = rocketnet_server_transport_disconnect_seals_for_test( transport );
    population.time += 0.01;
    rocketnet_server_update( population.server, population.time );
    rocketnet_server_send_packets( population.server );
    const uint64_t first_update = rocketnet_server_transport_disconnect_seals_for_test( transport ) - before;
    rocketnet_assert_function( saved_assert );

#if ROCKETNET_ASSERTS
    check( mass_disconnect_asserts == kicked );
#else
    check( mass_disconnect_asserts == 0 );
#endif
    check( first_update == (uint64_t) allowance );
    check( rocketnet_server_num_connected_clients( population.server ) == MASS_DISCONNECT_CLIENTS - kicked );
    for ( int i = 0; i < kicked; i++ )
        check( !rocketnet_server_client_connected( population.server, i ) );

    int remaining = -1;
    uint8_t key[ROCKETNET_KEY_BYTES];
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, key,
        (int) sizeof( key ), NULL, &remaining ) == 1 );
    check( remaining == kicked - allowance / ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( mass_disconnect_key_nonzero( key ) > 0 );

    uint64_t sealed = first_update;
    for ( int update = 0; remaining > 0; update++ )
    {
        check( update < 3 );
        population.time += 0.01;
        const uint64_t frame_before = rocketnet_server_transport_disconnect_seals_for_test( transport );
        rocketnet_server_update( population.server, population.time );
        rocketnet_server_send_packets( population.server );
        const uint64_t frame_seals = rocketnet_server_transport_disconnect_seals_for_test( transport ) - frame_before;
        check( frame_seals <= (uint64_t) allowance );
        sealed += frame_seals;
        check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, key,
            (int) sizeof( key ), NULL, &remaining ) == 1 );
    }
    check( sealed == (uint64_t) kicked * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; i++ )
    {
        check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, i, key,
            (int) sizeof( key ), NULL, NULL ) == 1 );
        check( mass_disconnect_key_nonzero( key ) == 0 );
    }

    mass_disconnect_destroy_all( &population );
}

REGISTER_UNIT_TEST( test_server_disconnect_prelink_kicks_share_update_allowance );

// rocketnet_server_disconnect_all_clients over 1000 connected slots: the call
// ends every slot at once and seals at most one update's allowance; each
// following update seals at most the configured allowance
// disconnect packets, so no
// call's work grows with the population, and every client still receives the
// server's disconnect and ends SERVER_DISCONNECTED, never on its timeout.
static void test_server_disconnect_all_1000_real_time()
{
#if ROCKETNET_DEV
    mass_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
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
    const int allowance = ROCKETNET_DEFAULT_SERVER_DISCONNECT_PACKETS_PER_UPDATE;
    check( call_seals <= (uint64_t) allowance );

    // the slot state ends in the same call (tla/ServerSession.tla DisconnectAll)
    check( rocketnet_server_num_connected_clients( server ) == 0 );
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
    {
        check( !rocketnet_server_client_connected( server, i ) );
        check( rocketnet_server_disconnect_reason( server, i ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    // the bursts leave over the following updates, a bounded number of seals in each
    const uint64_t total = (uint64_t) MASS_DISCONNECT_CLIENTS * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
    const int max_updates = (int) ( ( total + allowance - 1 ) / allowance );
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
        check( step <= (uint64_t) allowance );
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
#if ROCKETNET_DEV
    mass_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
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

static int mass_disconnect_find_client( rocketnet_server_t * server, uint64_t client_id )
{
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
        if ( rocketnet_server_client_connected( server, i ) && rocketnet_server_client_id( server, i ) == client_id )
            return i;
    return -1;
}

static int mass_disconnect_key_nonzero( const uint8_t * key )
{
    int count = 0;
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
        count += key[i] != 0 ? 1 : 0;
    return count;
}

// A full ring must never turn a best-effort notification into synchronous
// work. The allowance is below one ten-packet burst, so a replacement can
// reconnect through a released slot while the first retained burst is only
// partly sent. Disconnecting that replacement therefore drops only its
// redundant wire copies while the full ring remains owned by older sessions.
static void test_server_disconnect_full_queue_never_bypasses_budget()
{
#if ROCKETNET_DEV
    mass_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    rocketnet_quiet( 1 );
    static mass_disconnect_population_t population;
    const int allowance = 9;
    mass_disconnect_connect_all( &population, allowance );
    rocketnet_server_t * server = population.server;
    rocketnet_server_transport_t * transport = mass_disconnect_server_transport( server );

    const uint64_t before_all = rocketnet_server_transport_disconnect_seals_for_test( transport );
    rocketnet_server_disconnect_all_clients( server );
    check( rocketnet_server_transport_disconnect_seals_for_test( transport ) - before_all <= (uint64_t) allowance );
    check( rocketnet_server_num_connected_clients( server ) == 0 );
    uint8_t key_before[ROCKETNET_KEY_BYTES];
    int head = -1;
    int count = -1;
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, key_before,
        (int) sizeof( key_before ), &head, &count ) == 1 );
    check( count == ROCKETNET_MAX_CLIENTS );
    check( head >= 0 && head < ROCKETNET_MAX_CLIENTS );
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, head, key_before,
        (int) sizeof( key_before ), NULL, NULL ) == 1 );
    check( mass_disconnect_key_nonzero( key_before ) > 0 );

    // The server slots are already closed, so release the old client-side
    // loopback queues before creating replacements for those same slots.
    for ( int i = 0; i < MASS_DISCONNECT_CLIENTS; i++ )
    {
        rn_test_client_stop_and_destroy( population.clients[i] );
        population.clients[i] = NULL;
    }

    const uint16_t port = rocketnet_server_port( server );
    const uint64_t client_id = 0x20000;
    rocketnet_client_t * replacement = mass_disconnect_client_create( &population.keys, port, client_id );
    int slot = -1;
    for ( int step = 0; step < 80 && slot < 0; step++ )
    {
        population.time += 0.01;
        mass_disconnect_client_step( replacement, population.time );
        const uint64_t before = rocketnet_server_transport_disconnect_seals_for_test( transport );
        rocketnet_server_update( server, population.time );
        rocketnet_server_send_packets( server );
        check( rocketnet_server_transport_disconnect_seals_for_test( transport ) - before <= (uint64_t) allowance );
        mass_disconnect_client_step( replacement, population.time );
        slot = mass_disconnect_find_client( server, client_id );
    }
    check( slot >= 0 );

    uint8_t retained_key[ROCKETNET_KEY_BYTES];
    int retained_head = -1;
    int retained_count = -1;
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, retained_key,
        (int) sizeof( retained_key ), &retained_head, &retained_count ) == 1 );
    check( retained_count == ROCKETNET_MAX_CLIENTS );
    check( retained_head == head );

    const uint64_t before_disconnect = rocketnet_server_transport_disconnect_seals_for_test( transport );
    rocketnet_server_disconnect_client( server, slot );
    check( !rocketnet_server_client_connected( server, slot ) );
    check( rocketnet_server_transport_disconnect_seals_for_test( transport ) == before_disconnect );

    int after_head = -1;
    int after_count = -1;
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, retained_head, retained_key,
        (int) sizeof( retained_key ), &after_head, &after_count ) == 1 );
    check( after_count == ROCKETNET_MAX_CLIENTS );
    check( after_head == retained_head );
    check( mass_disconnect_key_nonzero( retained_key ) > 0 );
    rn_test_client_stop_and_destroy( replacement );

    int remaining = ROCKETNET_MAX_CLIENTS;
    const int max_updates = ( ROCKETNET_MAX_CLIENTS * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + allowance - 1 ) / allowance + 1;
    for ( int update = 0; remaining > 0; update++ )
    {
        check( update < max_updates );
        population.time += 0.01;
        const uint64_t before = rocketnet_server_transport_disconnect_seals_for_test( transport );
        rocketnet_server_update( server, population.time );
        rocketnet_server_send_packets( server );
        const uint64_t sealed = rocketnet_server_transport_disconnect_seals_for_test( transport ) - before;
        check( sealed <= (uint64_t) allowance );
        uint8_t key[ROCKETNET_KEY_BYTES];
        check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, key,
            (int) sizeof( key ), NULL, &remaining ) == 1 );
    }

    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; i++ )
    {
        uint8_t key[ROCKETNET_KEY_BYTES];
        check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, i, key,
            (int) sizeof( key ), NULL, NULL ) == 1 );
        check( mass_disconnect_key_nonzero( key ) == 0 );
    }

    // Empty-ring recovery still honors a below-burst allowance: the local
    // close is immediate and the ten redundant packets leave over updates.
    const uint64_t recovery_id = 0x30000;
    rocketnet_client_t * recovery = mass_disconnect_client_create( &population.keys, port, recovery_id );
    int recovery_slot = -1;
    for ( int step = 0; step < 80 && recovery_slot < 0; step++ )
    {
        population.time += 0.01;
        mass_disconnect_client_step( recovery, population.time );
        rocketnet_server_update( server, population.time );
        rocketnet_server_send_packets( server );
        mass_disconnect_client_step( recovery, population.time );
        recovery_slot = mass_disconnect_find_client( server, recovery_id );
    }
    check( recovery_slot >= 0 );
    const uint64_t before_recovery = rocketnet_server_transport_disconnect_seals_for_test( transport );
    rocketnet_server_disconnect_client( server, recovery_slot );
    check( rocketnet_server_transport_disconnect_seals_for_test( transport ) == before_recovery );
    int recovery_remaining = 1;
    for ( int update = 0; recovery_remaining > 0; update++ )
    {
        check( update < 2 );
        population.time += 0.01;
        const uint64_t before = rocketnet_server_transport_disconnect_seals_for_test( transport );
        rocketnet_server_update( server, population.time );
        rocketnet_server_send_packets( server );
        check( rocketnet_server_transport_disconnect_seals_for_test( transport ) - before <= (uint64_t) allowance );
        uint8_t key[ROCKETNET_KEY_BYTES];
        check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, key,
            (int) sizeof( key ), NULL, &recovery_remaining ) == 1 );
    }
    check( rocketnet_server_transport_disconnect_seals_for_test( transport ) - before_recovery ==
        (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rn_test_client_stop_and_destroy( recovery );

    mass_disconnect_destroy_all( &population );
}

REGISTER_UNIT_TEST( test_server_disconnect_full_queue_never_bypasses_budget );
