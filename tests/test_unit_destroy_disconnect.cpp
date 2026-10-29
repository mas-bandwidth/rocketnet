/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "fake_client_driver.h"
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
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_client_protocol.h"
#include "../sdk/rocketnet_server_protocol.h"
#include "../sdk/rocketnet_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

static const uint64_t DESTROY_DISCONNECT_PROTOCOL_ID = 0x44455354524F5901ULL;
static const int DESTROY_DISCONNECT_MAX_CLIENTS = 4;

// the cases build both endpoints over the loopback transport their config
// structs name (destroy_disconnect_server_create and
// destroy_disconnect_client_create below). the environment overlays the
// struct (sdk/rocketnet_server_transport.cpp:955 and
// sdk/rocketnet_client_transport.cpp:586), so a ROCKETNET_SERVER_TRANSPORT
// or ROCKETNET_CLIENT_TRANSPORT pin replaces "loopback": the server binds a
// portable socket, and the step-driven client create is refused at
// sdk/rocketnet_client_transport.cpp:968 because portable has no
// non-blocking step. each case clears both names for its run and restores
// them at its end.
#if ROCKETNET_DEV
static int destroy_disconnect_setenv( const char * name, const char * value )
{
    return rocketnet_test_env_setenv( name, value, 1 );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static int destroy_disconnect_unsetenv( const char * name )
{
    return rocketnet_test_env_unsetenv( name );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
struct destroy_disconnect_env_guard_t
{
    static const int NUM_NAMES = 2;
    const char * names[NUM_NAMES] = { "ROCKETNET_SERVER_TRANSPORT", "ROCKETNET_CLIENT_TRANSPORT" };
    char saved[NUM_NAMES][256] = {};
    bool present[NUM_NAMES] = {};

    destroy_disconnect_env_guard_t()
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

            check( destroy_disconnect_unsetenv( names[i] ) == 0 );
        }
    }

    ~destroy_disconnect_env_guard_t() noexcept( false ) // check() throws under the expected-fail harness
    {
        for ( int i = 0; i < NUM_NAMES; i++ )
        {
            if ( present[i] )
                check( destroy_disconnect_setenv( names[i], saved[i] ) == 0 );
            else
                check( destroy_disconnect_unsetenv( names[i] ) == 0 );
        }
    }
};
#endif // #if ROCKETNET_DEV

struct destroy_disconnect_keys_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
};

static void destroy_disconnect_make_keys( destroy_disconnect_keys_t * keys )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, keys->backend_public, keys->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( keys->server_public, keys->server_secret ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_t * destroy_disconnect_server_create( const destroy_disconnect_keys_t * keys, double now )
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.protocol_id = DESTROY_DISCONNECT_PROTOCOL_ID;
    config.max_clients = DESTROY_DISCONNECT_MAX_CLIENTS;
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
        NULL, &link, 1, DESTROY_DISCONNECT_MAX_CLIENTS, NULL, NULL, 0,
        ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, now );
    check( running != NULL );

    // the public server wraps the running message server: its destroy is
    // rocketnet_server_destroy, the path the cases exercise

    rocketnet_server_t * server = rocketnet_server_adopt( NULL, running );
    check( server != NULL );

    return server;
}

static void destroy_disconnect_mint_token( const destroy_disconnect_keys_t * keys, uint16_t port, uint64_t client_id, uint8_t * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );

    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( keys->backend_secret, keys->server_public,
                                                     DESTROY_DISCONNECT_PROTOCOL_ID, addresses, 1, client_id,
                                                     1000, 1030, 10, NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
}

static double destroy_disconnect_fixed_clock( void * ) { return 1000.0; }

static rocketnet_client_t * destroy_disconnect_client_create( const destroy_disconnect_keys_t * keys, uint16_t port, uint64_t client_id,
                                                              bool io_core = true, bool simulator = false )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    destroy_disconnect_mint_token( keys, port, client_id, token );

    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.step_driven = io_core ? 1 : 0;
    config.time_callback = destroy_disconnect_fixed_clock;
    config.game_clock_epoch = 1000;
    config.simulated_latency_ms = simulator ? 1 : 0;

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_current(
        NULL, token, &config, 0.0, rocketnet_loopback_transport.client, NULL );
    check( transport != NULL );

    rn_message_link_t link;
    rn_message_client_link( &link, transport );

    rocketnet_message_client_t * connection = (rocketnet_message_client_t *) rn_message_object_create(
        NULL, &link, 0, 1, NULL, NULL, 0,
        ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    check( connection != NULL );

    // the public client wraps the message client: its destroy is
    // rocketnet_client_destroy, the path the cases exercise

    rocketnet_client_t * client = rocketnet_client_adopt( NULL, connection );
    check( client != NULL );

    return client;
}

static int destroy_disconnect_connect_client( rocketnet_server_t * server, rocketnet_client_t * client, double * time )
{
    for ( int i = 0; i < 200; i++ )
    {
        *time += 0.01;

        rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( client ) );

        // The legacy fixture suppresses its native worker. Explicitly run its
        // receive iteration; public update only drives the configured I/O core.

        if ( !rocketnet_client_transport_client_io( transport ) )
        {
            rocketnet_client_transport_step_legacy_for_test( transport, *time );
        }
        rocketnet_client_update( client, *time );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, *time );
        rocketnet_server_send_packets( server );

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const int slot = (int) rocketnet_client_index( client );
            if ( rocketnet_server_client_connected( server, slot ) )
                return slot;
        }
    }

    check( false );

    return -1;
}

static const uint64_t * destroy_disconnect_server_counters( rocketnet_server_t * server )
{
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( server ) );
    return rocketnet_server_transport_counters( transport )->values;
}

static const uint64_t * destroy_disconnect_client_counters( rocketnet_client_t * client )
{
    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( client ) );
    return rocketnet_client_transport_counters( transport )->values;
}

// An observed client runs on the loopback client operations, with create
// wrapped to hand the test the client's loopback endpoint. Draining that
// endpoint directly counts every datagram any sender handed the loopback
// transport for the client's address, whatever state the client is in.
static void * destroy_disconnect_observed_endpoint = NULL;

static void * destroy_disconnect_observed_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    void * instance = rocketnet_loopback_transport.client->create( context, config, bind );
    destroy_disconnect_observed_endpoint = instance;
    return instance;
}

static rocketnet_client_transport_io_operations_t destroy_disconnect_observed_operations()
{
    rocketnet_client_transport_io_operations_t operations = *rocketnet_loopback_transport.client;
    operations.create = destroy_disconnect_observed_create;
    return operations;
}

static rocketnet_client_t * destroy_disconnect_observed_client_create( const destroy_disconnect_keys_t * keys, uint16_t port, uint64_t client_id,
                                                                       const rocketnet_client_transport_io_operations_t * operations, void ** endpoint )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    destroy_disconnect_mint_token( keys, port, client_id, token );

    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.step_driven = 1;

    destroy_disconnect_observed_endpoint = NULL;

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_at_timestamp(
        NULL, token, &config, 0.0, 1000, operations, NULL );
    check( transport != NULL );
    check( destroy_disconnect_observed_endpoint != NULL );
    *endpoint = destroy_disconnect_observed_endpoint;

    rn_message_link_t link;
    rn_message_client_link( &link, transport );

    rocketnet_message_client_t * connection = (rocketnet_message_client_t *) rn_message_object_create(
        NULL, &link, 0, 1, NULL, NULL, 0,
        ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    check( connection != NULL );

    // the public client wraps the message client: its destroy is
    // rocketnet_client_destroy, the path the cases exercise

    rocketnet_client_t * client = rocketnet_client_adopt( NULL, connection );
    check( client != NULL );

    return client;
}

// Drains the observed endpoint and returns the number of datagrams it held,
// including any the endpoint dropped since the last drain.

static uint64_t destroy_disconnect_drain( void * endpoint, uint64_t * dropped )
{
    uint64_t count = 0;

    for ( ;; )
    {
        rocketnet_address_t from;
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        if ( rocketnet_loopback_transport.client->receive( endpoint, &from, packet, (int) sizeof( packet ) ) <= 0 )
            break;
        count++;
    }

    const uint64_t dropped_now = rocketnet_loopback_endpoint_dropped( endpoint );
    count += dropped_now - *dropped;
    *dropped = dropped_now;

    return count;
}

// DONE-WHEN (2): disconnecting a connected client before destroy sends the same redundant
// disconnect packets that rocketnet_client_disconnect sends (the same count
// and encryption), before its sockets close, and the server frees that
// client's slot on receipt, not on timeout: a loopback case with time as an
// argument starting at 1000.0 asserts the server's slot is free within one
// server update after the client's destroy, and the timed-out counter does not
// move.

static void test_disconnect_then_destroy_connected_client_sends_disconnect()
{
#if ROCKETNET_DEV
    destroy_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    rocketnet_quiet( 1 );

    destroy_disconnect_keys_t keys;
    destroy_disconnect_make_keys( &keys );

    // reference client: explicit rocketnet_client_disconnect then destroy

    double time_explicit = 1000.0;
    rocketnet_server_t * server_explicit = destroy_disconnect_server_create( &keys, time_explicit );
    rocketnet_client_t * client_explicit = destroy_disconnect_client_create( &keys, rocketnet_server_port( server_explicit ), 0xC101ULL );
    const int slot_explicit = destroy_disconnect_connect_client( server_explicit, client_explicit, &time_explicit );

    const uint64_t * sc_explicit = destroy_disconnect_server_counters( server_explicit );
    const uint64_t explicit_rx_before = rn_server_stats_of( server_explicit ).packets_received;
    const uint64_t explicit_disc_rx_before = sc_explicit[ROCKETNET_COUNTER_DISCONNECT_RECEIVED];
    const uint64_t explicit_disc_ok_before = sc_explicit[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];
    const uint64_t explicit_timeout_before = sc_explicit[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];

    rocketnet_client_disconnect( client_explicit );
    rn_test_client_wait_disconnected( client_explicit );
    rocketnet_server_update( server_explicit, time_explicit );
    check( !rocketnet_server_client_connected( server_explicit, slot_explicit ) );
    check( rocketnet_server_disconnect_reason( server_explicit, slot_explicit ) == ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED );

    const uint64_t explicit_rx_delta = rn_server_stats_of( server_explicit ).packets_received - explicit_rx_before;
    const uint64_t explicit_disc_rx_delta = sc_explicit[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] - explicit_disc_rx_before;
    const uint64_t explicit_disc_ok_delta = sc_explicit[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] - explicit_disc_ok_before;
    check( explicit_rx_delta == (uint64_t) ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    check( explicit_disc_rx_delta == (uint64_t) ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    check( explicit_disc_ok_delta == 1 );
    check( sc_explicit[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == explicit_timeout_before );

    rn_test_client_stop_and_destroy( client_explicit );
    rn_test_server_stop_and_destroy( server_explicit );

    // connected client: request disconnect, complete it, then destroy

    double time = 1000.0;
    rocketnet_server_t * server = destroy_disconnect_server_create( &keys, time );
    rocketnet_client_t * client = destroy_disconnect_client_create( &keys, rocketnet_server_port( server ), 0xC102ULL );
    const int slot = destroy_disconnect_connect_client( server, client, &time );
    check( rocketnet_server_num_connected_clients( server ) == 1 );

    const uint64_t * sc = destroy_disconnect_server_counters( server );
    const uint64_t rx_before = rn_server_stats_of( server ).packets_received;
    const uint64_t disc_rx_before = sc[ROCKETNET_COUNTER_DISCONNECT_RECEIVED];
    const uint64_t disc_ok_before = sc[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];
    const uint64_t disc_open_fail_before = sc[ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN];
    const uint64_t timeout_before = sc[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];

    rn_test_client_stop_and_destroy( client );

    // one server update after the client's destroy frees the slot on receipt
    // of the sealed disconnect burst, with zero open failures and no timeout

    rocketnet_server_update( server, time );
    check( !rocketnet_server_client_connected( server, slot ) );
    check( rocketnet_server_num_connected_clients( server ) == 0 );
    check( rocketnet_server_disconnect_reason( server, slot ) == ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED );
    check( sc[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == timeout_before );
    check( sc[ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN] == disc_open_fail_before );

    const uint64_t rx_delta = rn_server_stats_of( server ).packets_received - rx_before;
    const uint64_t disc_rx_delta = sc[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] - disc_rx_before;
    const uint64_t disc_ok_delta = sc[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] - disc_ok_before;
    check( rx_delta == explicit_rx_delta );
    check( disc_rx_delta == explicit_disc_rx_delta );
    check( disc_ok_delta == explicit_disc_ok_delta );

    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_disconnect_then_destroy_connected_client_sends_disconnect );

// DONE-WHEN (3): destroying a server with connected clients sends each
// connected client the server's disconnect packets before the transport
// closes, and each client reaches its disconnected state with the
// server-disconnect reason within one client update: a loopback case asserts
// it for at least two clients.

static void test_stop_then_destroy_server_sends_disconnect_to_every_connected_client()
{
#if ROCKETNET_DEV
    destroy_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    rocketnet_quiet( 1 );

    destroy_disconnect_keys_t keys;
    destroy_disconnect_make_keys( &keys );

    double time = 1000.0;
    rocketnet_server_t * server = destroy_disconnect_server_create( &keys, time );
    const uint16_t port = rocketnet_server_port( server );

    enum
    {
        NUM_CLIENTS = 3
    };

    rocketnet_client_t * clients[NUM_CLIENTS] = {};
    int slots[NUM_CLIENTS] = {};

    for ( int i = 0; i < NUM_CLIENTS; i++ )
    {
        clients[i] = destroy_disconnect_client_create( &keys, port, 0xD200 + (uint64_t) i );
        slots[i] = destroy_disconnect_connect_client( server, clients[i], &time );
        check( slots[i] >= 0 && slots[i] < DESTROY_DISCONNECT_MAX_CLIENTS );
        check( rocketnet_client_state( clients[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    }

    check( rocketnet_server_num_connected_clients( server ) == NUM_CLIENTS );

    // drain any keep-alive packets queued for earlier clients while later clients connected

    for ( int i = 0; i < NUM_CLIENTS; i++ )
    {
        rocketnet_client_update( clients[i], time );
    }

    uint64_t client_rx_before[NUM_CLIENTS] = {};
    uint64_t client_disc_rx_before[NUM_CLIENTS] = {};
    uint64_t client_disc_ok_before[NUM_CLIENTS] = {};

    for ( int i = 0; i < NUM_CLIENTS; i++ )
    {
        const uint64_t * cc = destroy_disconnect_client_counters( clients[i] );
        client_rx_before[i] = rn_client_stats_of( clients[i] ).packets_received;
        client_disc_rx_before[i] = cc[ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED];
        client_disc_ok_before[i] = cc[ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED];
    }

    rn_test_server_stop_and_destroy( server );

    for ( int i = 0; i < NUM_CLIENTS; i++ )
    {
        rocketnet_client_update( clients[i], time );
        check( rocketnet_client_state( clients[i] ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

        const uint64_t * cc = destroy_disconnect_client_counters( clients[i] );
        const uint64_t rx_delta = rn_client_stats_of( clients[i] ).packets_received - client_rx_before[i];
        const uint64_t disc_rx_delta = cc[ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED] - client_disc_rx_before[i];
        const uint64_t disc_ok_delta = cc[ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED] - client_disc_ok_before[i];
        check( rx_delta == (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
        check( disc_rx_delta == 1 );
        check( disc_ok_delta == 1 );
    }

    for ( int i = 0; i < NUM_CLIENTS; i++ )
    {
        rn_test_client_stop_and_destroy( clients[i] );
    }
}

REGISTER_UNIT_TEST( test_stop_then_destroy_server_sends_disconnect_to_every_connected_client );

// Steps a public client against a loopback server until the client reaches
// no handshake target: destroy at once, before any handshake state
#define DESTROY_DISCONNECT_NO_TARGET -100

// the target handshake state (or, for DESTROY_DISCONNECT_NO_TARGET, not at
// all: the client is destroyed right after create), hands off whatever the client has queued, and lets the server
// read it. Then destroys the client with rocketnet_client_destroy and runs
// one server update: the server must receive nothing across the destroy.

static void destroy_disconnect_public_client_destroy_sends_nothing( const destroy_disconnect_keys_t * keys, uint64_t client_id, int target_state )
{
    double time = 1000.0;
    rocketnet_server_t * server = destroy_disconnect_server_create( keys, time );
    rocketnet_client_t * client = destroy_disconnect_client_create( keys, rocketnet_server_port( server ), client_id );

    if ( target_state != DESTROY_DISCONNECT_NO_TARGET )
    {
        bool reached = false;
        for ( int i = 0; i < 200 && !reached; i++ )
        {
            time += 0.01;
            rocketnet_client_update( client, time );

            if ( rocketnet_client_state( client ) == target_state )
            {
                reached = true;
                break;
            }

            rocketnet_client_send_packets( client );
            rocketnet_server_update( server, time );
            rocketnet_server_send_packets( server );
        }

        check( reached );
    }

    check( rocketnet_server_num_connected_clients( server ) == 0 );

    // hand off and read everything the client queued before the destroy

    rocketnet_client_send_packets( client );
    rocketnet_server_update( server, time );

    const uint64_t rx_before = rn_server_stats_of( server ).packets_received;

    rn_test_client_stop_and_destroy( client );
    rocketnet_server_update( server, time );
    check( rn_server_stats_of( server ).packets_received == rx_before );

    rn_test_server_stop_and_destroy( server );
}

// DONE-WHEN (4): destroying a client or server with no live connection sends
// nothing (a case counts datagrams handed to the fake transport: zero).

static void test_shutdown_without_live_connection_sends_nothing()
{
#if ROCKETNET_DEV
    destroy_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    rocketnet_quiet( 1 );

    destroy_disconnect_keys_t keys;
    destroy_disconnect_make_keys( &keys );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    destroy_disconnect_mint_token( &keys, 40000, 0xE301ULL, token );

    // each fake transport is static and zeroed in place: one fake holds two 256
    // datagram rings (tests/fake_client_driver.h:37), and an unoptimised MSVC
    // build gives both blocks their own stack slot, past the default 1 MB stack

    // client on the base fake transport: destroyed before any session keys exist
    {
        static rocketnet_fake_client_transport_io_t fake;
        memset( &fake, 0, sizeof( fake ) );

        rocketnet_client_transport_config_t config;
        rocketnet_client_transport_default_config( &config );

        rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_at_timestamp(
            &fake, token, &config, 1000.0, 1000, &rocketnet_fake_client_operations_base, &fake );
        check( transport != NULL );
        check( fake.num_sent == 0 );

        rn_test_client_transport_stop_and_destroy( transport );
        check( fake.destroy_calls == 1 );
        check( fake.num_sent == 0 );
        check( fake.send_calls == 0 );
        check( fake.send_batch_calls == 0 );
    }

    // client on the I/O-step fake transport: destroyed with no live connection
    {
        static rocketnet_fake_client_transport_io_t fake;
        memset( &fake, 0, sizeof( fake ) );

        rocketnet_client_transport_config_t config;
        rocketnet_client_transport_default_config( &config );
        config.step_driven = 1;

        rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_at_timestamp(
            &fake, token, &config, 1000.0, 1000, &rocketnet_fake_client_operations_batch, &fake );
        check( transport != NULL );
        check( fake.num_sent == 0 );

        rn_test_client_transport_stop_and_destroy( transport );
        check( fake.destroy_calls == 1 );
        check( fake.num_sent == 0 );
        check( fake.send_calls == 0 );
        check( fake.send_batch_calls == 0 );
    }

    // Public clients destroyed with rocketnet_client_destroy after shutdown completion and no live
    // connection: right after create, while requesting a cookie, and while
    // sending the connection request. Before the session keys exist there is
    // nothing to seal, so the server receives nothing across the destroy.

    destroy_disconnect_public_client_destroy_sends_nothing( &keys, 0xE320ULL, DESTROY_DISCONNECT_NO_TARGET );
    destroy_disconnect_public_client_destroy_sends_nothing( &keys, 0xE321ULL, ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE );
    destroy_disconnect_public_client_destroy_sends_nothing( &keys, 0xE322ULL, ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST );

    // the server cases count datagrams across rocketnet_server_destroy at
    // the loopback endpoint of every client the server has heard from: each
    // endpoint is drained just before the destroy and again just after it,
    // and the second drain must be empty

    const rocketnet_client_transport_io_operations_t observed = destroy_disconnect_observed_operations();

    // server with zero clients: a server that never heard from a client,
    // beside a client endpoint that never sent, sends nothing on destroy
    {
        double time = 1000.0;
        rocketnet_server_t * server = destroy_disconnect_server_create( &keys, time );
        void * endpoint = NULL;
        uint64_t dropped = 0;
        rocketnet_client_t * bystander = destroy_disconnect_observed_client_create( &keys, rocketnet_server_port( server ), 0xE302ULL, &observed, &endpoint );
        check( rocketnet_server_num_connected_clients( server ) == 0 );
        rocketnet_server_update( server, time );
        check( rn_server_stats_of( server ).packets_sent == 0 );
        check( destroy_disconnect_drain( endpoint, &dropped ) == 0 );

        rn_test_server_stop_and_destroy( server );
        check( destroy_disconnect_drain( endpoint, &dropped ) == 0 );
        rn_test_client_stop_and_destroy( bystander );
    }

    // Server with zero connected clients and one client mid-handshake: the
    // server has answered the client's connection request, and the client
    // has not yet sent its connection response. Destroy sends nothing.
    {
        double time = 1000.0;
        rocketnet_server_t * server = destroy_disconnect_server_create( &keys, time );
        void * endpoint = NULL;
        uint64_t dropped = 0;
        rocketnet_client_t * client = destroy_disconnect_observed_client_create( &keys, rocketnet_server_port( server ), 0xE303ULL, &observed, &endpoint );

        bool pending = false;
        for ( int i = 0; i < 200 && !pending; i++ )
        {
            time += 0.01;
            rocketnet_client_update( client, time );

            if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE )
            {
                pending = true;
                break;
            }

            rocketnet_client_send_packets( client );
            rocketnet_server_update( server, time );
            rocketnet_server_send_packets( server );
        }

        check( pending );
        check( rocketnet_server_num_connected_clients( server ) == 0 );
        check( rn_server_stats_of( server ).packets_sent > 0 );
        destroy_disconnect_drain( endpoint, &dropped );

        rn_test_server_stop_and_destroy( server );
        check( destroy_disconnect_drain( endpoint, &dropped ) == 0 );
        rn_test_client_stop_and_destroy( client );
    }

    // Server after all clients disconnected: one client disconnects itself
    // and the server disconnects the other, so both slots are free before
    // the destroy. Destroy sends nothing to either client.
    {
        double time = 1000.0;
        rocketnet_server_t * server = destroy_disconnect_server_create( &keys, time );
        const uint16_t port = rocketnet_server_port( server );

        enum
        {
            NUM_CLIENTS = 2
        };

        rocketnet_client_t * clients[NUM_CLIENTS] = {};
        void * endpoints[NUM_CLIENTS] = {};
        uint64_t dropped[NUM_CLIENTS] = {};
        int slots[NUM_CLIENTS] = {};

        for ( int i = 0; i < NUM_CLIENTS; i++ )
        {
            clients[i] = destroy_disconnect_observed_client_create( &keys, port, 0xE310 + (uint64_t) i, &observed, &endpoints[i] );
            slots[i] = destroy_disconnect_connect_client( server, clients[i], &time );
        }

        check( rocketnet_server_num_connected_clients( server ) == NUM_CLIENTS );

        rocketnet_client_disconnect( clients[0] );
        rn_test_client_wait_disconnected( clients[0] );
        rocketnet_server_disconnect_client( server, slots[1] );
        rocketnet_server_send_packets( server );
        time += 0.01;
        rocketnet_server_update( server, time );
        rocketnet_server_send_packets( server );
        rocketnet_client_update( clients[1], time );

        check( !rocketnet_server_client_connected( server, slots[0] ) );
        check( !rocketnet_server_client_connected( server, slots[1] ) );
        check( rocketnet_server_num_connected_clients( server ) == 0 );

        // the client that left reads DISCONNECTED; the one the server ended
        // reads SERVER_DISCONNECTED

        check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        check( rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
        for ( int i = 0; i < NUM_CLIENTS; i++ )
        {
            destroy_disconnect_drain( endpoints[i], &dropped[i] );
        }

        rn_test_server_stop_and_destroy( server );

        for ( int i = 0; i < NUM_CLIENTS; i++ )
        {
            check( destroy_disconnect_drain( endpoints[i], &dropped[i] ) == 0 );
            rn_test_client_stop_and_destroy( clients[i] );
        }
    }
}

REGISTER_UNIT_TEST( test_shutdown_without_live_connection_sends_nothing );

// A repeated protocol disconnect must retain the first sealed burst even
// before full transport stop. The legacy worker is stepped explicitly here.

static void test_transport_disconnect_twice_delivers_original_burst()
{
#if ROCKETNET_DEV
    destroy_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    destroy_disconnect_keys_t keys;
    destroy_disconnect_make_keys( &keys );

    double now = 1000.0;
    rocketnet_server_t * server = destroy_disconnect_server_create( &keys, now );
    rocketnet_client_t * client = destroy_disconnect_client_create( &keys, rocketnet_server_port( server ), 0xE401ULL, false );
    const int slot = destroy_disconnect_connect_client( server, client, &now );
    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( client ) );
    const uint64_t * counters = destroy_disconnect_server_counters( server );
    const uint64_t received = counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED];
    const uint64_t accepted = counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];
    const uint64_t timed_out = counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];

    rocketnet_client_transport_disconnect( transport );
    rocketnet_client_transport_disconnect( transport );
    check( !rocketnet_client_transport_stopped( transport ) );
    rocketnet_client_update( client, now );
    rocketnet_server_update( server, now );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] - received == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] - accepted == 1 );
    check( counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == timed_out );
    check( !rocketnet_server_client_connected( server, slot ) );
    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_transport_disconnect_twice_delivers_original_burst );

// Disconnect is still outgoing traffic: an already allocated simulator's
// 100-percent loss policy applies to the burst, including repeated requests.

static void test_transport_disconnect_preserves_simulator_loss()
{
#if ROCKETNET_DEV
    destroy_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    rocketnet_public_link_simulator_test_development( 1 );

    destroy_disconnect_keys_t keys;
    destroy_disconnect_make_keys( &keys );

    double now = 1000.0;
    rocketnet_server_t * server = destroy_disconnect_server_create( &keys, now );
    rocketnet_client_t * client = destroy_disconnect_client_create( &keys, rocketnet_server_port( server ), 0xE402ULL, false, true );
    const int slot = destroy_disconnect_connect_client( server, client, &now );
    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( client ) );
    check( rocketnet_client_transport_network_simulator( transport, 0, 0, 100, 0 ) );

    const uint64_t * counters = destroy_disconnect_server_counters( server );
    const uint64_t received = counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED];
    rocketnet_client_transport_disconnect( transport );
    rocketnet_client_transport_disconnect( transport );
    check( !rocketnet_client_transport_stopped( transport ) );
    now += 0.01;
    rocketnet_client_update( client, now );
    rocketnet_server_update( server, now );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] == received );
    check( rocketnet_server_client_connected( server, slot ) );
    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    rocketnet_public_link_simulator_test_development( 0 );
}
REGISTER_UNIT_TEST( test_transport_disconnect_preserves_simulator_loss );

// The public disconnect requests stop right after sealing the burst. With a
// link simulator the burst is staged in the caller-owned queue. Stop transfers
// the queue to the worker, which drains it before publishing completion; the
// step-driven fixture explicitly runs that completion. Shutdown holds the
// clock fixed, so latency must not keep the burst back (tla/RNClientLinkStop.tla).

static void test_client_disconnect_sends_burst_staged_in_link_simulator()
{
#if ROCKETNET_DEV
    destroy_disconnect_env_guard_t env_guard;
#endif // #if ROCKETNET_DEV
    rocketnet_quiet( 1 );
    rocketnet_public_link_simulator_test_development( 1 );

    destroy_disconnect_keys_t keys;
    destroy_disconnect_make_keys( &keys );

    const int latency_ms[] = { 0, 50 };

    for ( int i = 0; i < 2; i++ )
    {
        double now = 1000.0;
        rocketnet_server_t * server = destroy_disconnect_server_create( &keys, now );
        rocketnet_client_t * client = destroy_disconnect_client_create( &keys, rocketnet_server_port( server ), UINT64_C( 0xE410 ) + (uint64_t) i, false, true );
        const int slot = destroy_disconnect_connect_client( server, client, &now );
        rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( client ) );
        check( rocketnet_client_transport_network_simulator( transport, latency_ms[i], 0, 0, 0 ) );

        const uint64_t * counters = destroy_disconnect_server_counters( server );
        const uint64_t received = counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED];
        const uint64_t accepted = counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];
        const uint64_t timed_out = counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];

        rocketnet_client_disconnect( client );
        rn_test_client_wait_disconnected( client );
        rocketnet_server_update( server, now );

        check( counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] - received == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
        check( counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] - accepted == 1 );
        check( counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == timed_out );
        check( !rocketnet_server_client_connected( server, slot ) );
        check( rocketnet_server_num_connected_clients( server ) == 0 );

        rn_test_client_stop_and_destroy( client );
        rn_test_server_stop_and_destroy( server );
    }

    rocketnet_public_link_simulator_test_development( 0 );
}
REGISTER_UNIT_TEST( test_client_disconnect_sends_burst_staged_in_link_simulator );
