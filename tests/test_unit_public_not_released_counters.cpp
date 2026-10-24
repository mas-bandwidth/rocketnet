/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The unreleased-message refusal under the public client and server. The
// public API no longer reports it (the update returns void and the stats
// structs carry no MESSAGE_NOT_RELEASED count), so these cases assert the
// internal message layer beneath the public objects: a view held across the
// message layer's update is refused by name (ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED)
// and counted in the internal MESSAGE_NOT_RELEASED counter, and the public
// receive's batch, which the next update releases, never trips it. The pair
// runs over the in-process loopback transport: no socket, no receive thread
// (the client transport is created step-driven), and time is only ever an
// argument, starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "portable_crt.h"

#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_counters.h"
#include "rocketnet_driver.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the pair is built over the in-memory loopback transport, which this test names in
// the server transport config; the caller's ROCKETNET_SERVER_TRANSPORT pin wins
// over the struct by design, so a CI leg pinned to a real transport would open a
// socket the step-driven loopback client never reaches. The pin is set aside
// for the create and put back after, as test_unit_status_server_transport_create_ex.cpp does.
#if defined(_WIN32)
static int public_not_released_setenv( const char * name, const char * value ) { return rocketnet_test_env_putenv_s( name, value ); }
static int public_not_released_unsetenv( const char * name ) { return rocketnet_test_env_putenv_s( name, "" ); }
#else
static int public_not_released_setenv( const char * name, const char * value ) { return rocketnet_test_env_setenv( name, value, 1 ); }
static int public_not_released_unsetenv( const char * name ) { return rocketnet_test_env_unsetenv( name ); }
#endif

#define check rocketnet_unit_check

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

static void public_not_released_assert_hook( const char *, const char *, const char *, int )
{
}

struct public_not_released_pair_t
{
    struct rocketnet_client_t * client;
    struct rocketnet_server_t * server;
    struct rocketnet_client_transport_t * client_transport;
    double time;
};

static double public_not_released_clock = 1000.0;

static double public_not_released_time_callback( void * )
{
    return public_not_released_clock;
}

static bool public_not_released_wait( void *, bool, double )
{
    return false;
}

static void public_not_released_wake( void * )
{
}

static rocketnet_client_transport_io_operations_t public_not_released_client_operations;

static void public_not_released_client_operations_init()
{
    public_not_released_client_operations = *rocketnet_loopback_transport.client;
    public_not_released_client_operations.wait = public_not_released_wait;
    public_not_released_client_operations.wake = public_not_released_wake;
}

static bool public_not_released_pair_setup( struct public_not_released_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;

    const uint64_t protocol_id = 0x6e6f746e6f746e6fULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_transport_config;
    rocketnet_server_transport_default_config( &server_transport_config );
    server_transport_config.protocol_id = protocol_id;
    server_transport_config.max_clients = 1;
    memcpy( server_transport_config.backend_public_key, backend_public_key, sizeof( server_transport_config.backend_public_key ) );
    memcpy( server_transport_config.server_public_key, server_public_key, sizeof( server_transport_config.server_public_key ) );
    memcpy( server_transport_config.server_secret_key, server_secret_key, sizeof( server_transport_config.server_secret_key ) );
    snprintf( server_transport_config.transport, sizeof( server_transport_config.transport ), "%s", "loopback" );

    char saved_pin[256];
    const bool pinned = rocketnet_test_env_getenv( "ROCKETNET_SERVER_TRANSPORT", saved_pin, sizeof( saved_pin ) ) != 0;
    public_not_released_unsetenv( "ROCKETNET_SERVER_TRANSPORT" );

    int status = -1;
    struct rocketnet_server_transport_t * server_transport =
        rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_transport_config, pair->time, &status );

    if ( pinned )
        public_not_released_setenv( "ROCKETNET_SERVER_TRANSPORT", saved_pin );
    check( status == ROCKETNET_STATUS_OK );
    if ( !server_transport )
        return false;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( server_transport ) );

    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    message_config.num_channels = 4;
    message_config.reliable_messages_in_flight = 64;
    message_config.receive_message_buffer_bytes = 65536;
    message_config.max_message_bytes = 16384;
    message_config.send_message_buffer_bytes = 65536;
    message_config.send_queue_messages = 64;
    message_config.packets_in_flight = 64;
    message_config.packets_per_update = 8;
    message_config.unreliable_messages_per_update = 64;
    message_config.message_resend_delay_initial_ms = 100;
    message_config.message_resend_delay_min_ms = 20;
    message_config.message_resend_delay_max_ms = 400;
    message_config.reliable_message_timeout_ms = 120000;

    struct rn_message_link_t server_link;
    rn_message_server_link( &server_link, server_transport );
    struct rocketnet_message_server_t * server = (struct rocketnet_message_server_t *) rn_message_object_create(
        NULL, &server_link, 1, 1, &message_config, NULL, 0, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, pair->time );
    check( server != NULL );
    if ( !server )
        return false;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x9e3779b97f4a7c15ULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    public_not_released_client_operations_init();
    struct rocketnet_client_transport_config_t client_transport_config;
    rocketnet_client_transport_default_config( &client_transport_config );
    client_transport_config.time_callback = public_not_released_time_callback;
    struct rocketnet_client_transport_t * client_transport = rocketnet_client_transport_create_step_driven(
        NULL, token, &client_transport_config, 0.0, &public_not_released_client_operations, NULL );
    check( client_transport != NULL );
    if ( !client_transport )
        return false;

    struct rn_message_link_t client_link;
    rn_message_client_link( &client_link, client_transport );
    struct rocketnet_message_client_t * client = (struct rocketnet_message_client_t *) rn_message_object_create(
        NULL, &client_link, 0, 1, &message_config, NULL, 0, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, pair->time );
    check( client != NULL );
    if ( !client )
        return false;

    pair->server = rocketnet_server_adopt( NULL, server );
    pair->client = rocketnet_client_adopt( NULL, client );
    check( pair->server != NULL && pair->client != NULL );
    pair->client_transport = client_transport;
    return true;
}

static uint64_t public_not_released_client_count( struct public_not_released_pair_t * pair )
{
    return rocketnet_counter_value( rocketnet_message_client_counters( rocketnet_client_message_client( pair->client ) ),
                                    ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED );
}

static uint64_t public_not_released_server_count( struct public_not_released_pair_t * pair )
{
    return rocketnet_counter_value( rocketnet_message_server_counters( rocketnet_server_message_server( pair->server ) ),
                                    ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED );
}

static void public_not_released_pair_teardown( struct public_not_released_pair_t * pair )
{
    rn_test_client_stop_and_destroy( pair->client );
    rn_test_server_stop_and_destroy( pair->server );
}

static void public_not_released_pump( struct public_not_released_pair_t * pair )
{
    pair->time += 0.01;
    public_not_released_clock = pair->time;
    rocketnet_server_update( pair->server, pair->time );
    rocketnet_client_update( pair->client, pair->time );
    rocketnet_server_send_packets( pair->server );
    rocketnet_client_send_packets( pair->client );
    rocketnet_client_io_step( rocketnet_client_transport_client_io( pair->client_transport ), false );
}

static bool public_not_released_connect( struct public_not_released_pair_t * pair )
{
    for ( int i = 0; i < 500; i++ )
    {
        if ( rocketnet_client_state( pair->client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_client_connected( pair->server, 0 ) )
            return true;
        public_not_released_pump( pair );
    }
    return false;
}

static void public_not_released_send_from_server( struct public_not_released_pair_t * pair, uint8_t value )
{
    uint8_t * out = rocketnet_server_start_message( pair->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 );
    check( out != NULL );
    memset( out, value, 16 );
    check( rocketnet_server_finish_message( pair->server, 16 ) != 0 );
}

static void public_not_released_send_from_client( struct public_not_released_pair_t * pair, uint8_t value )
{
    uint8_t * out = rocketnet_client_start_message( pair->client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 );
    check( out != NULL );
    memset( out, value, 16 );
    check( rocketnet_client_finish_message( pair->client, 16 ) != 0 );
}

static bool public_not_released_receive_client( struct public_not_released_pair_t * pair, const uint8_t ** data, int * bytes )
{
    for ( int i = 0; i < 20; i++ )
    {
        // the public receive is a batch the next update releases, so a view
        // held across an update is taken through the client's message layer
        if ( rocketnet_message_client_receive( rocketnet_client_message_client( pair->client ), NULL, data, bytes ) )
            return true;
        public_not_released_pump( pair );
    }
    return false;
}

static bool public_not_released_receive_server( struct public_not_released_pair_t * pair, const uint8_t ** data, int * bytes )
{
    for ( int i = 0; i < 20; i++ )
    {
        int sender = -1;
        if ( rocketnet_message_server_receive( rocketnet_server_message_server( pair->server ), &sender, NULL, data, bytes ) )
        {
            check( sender == 0 );
            return true;
        }
        public_not_released_pump( pair );
    }
    return false;
}

static void test_public_not_released_counter_client()
{
    struct public_not_released_pair_t pair;
    check( public_not_released_pair_setup( &pair ) );
    check( public_not_released_connect( &pair ) );

    public_not_released_send_from_server( &pair, 7 );

    const uint8_t * held = NULL;
    int bytes = 0;
    check( public_not_released_receive_client( &pair, &held, &bytes ) );
    check( bytes == 16 );
    check( public_not_released_client_count( &pair ) == 0 );

    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( public_not_released_assert_hook );

    pair.time += 0.01;
    check( rocketnet_message_client_update( rocketnet_client_message_client( pair.client ), pair.time ) == ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED );
    check( public_not_released_client_count( &pair ) == 1 );

    // a later message is released normally, and the refusal's count stays
    public_not_released_send_from_server( &pair, 8 );
    const uint8_t * next = NULL;
    check( public_not_released_receive_client( &pair, &next, &bytes ) );
    rocketnet_message_client_release( rocketnet_client_message_client( pair.client ), next );
    pair.time += 0.01;
    rocketnet_client_update( pair.client, pair.time );
    check( public_not_released_client_count( &pair ) == 1 );

    rocketnet_assert_function( saved_assert );
    public_not_released_pair_teardown( &pair );
}
REGISTER_UNIT_TEST( test_public_not_released_counter_client );

static void test_public_not_released_counter_server()
{
    struct public_not_released_pair_t pair;
    check( public_not_released_pair_setup( &pair ) );
    check( public_not_released_connect( &pair ) );

    public_not_released_send_from_client( &pair, 9 );

    const uint8_t * held = NULL;
    int bytes = 0;
    check( public_not_released_receive_server( &pair, &held, &bytes ) );
    check( bytes == 16 );
    check( public_not_released_server_count( &pair ) == 0 );

    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( public_not_released_assert_hook );

    pair.time += 0.01;
    check( rocketnet_message_server_update( rocketnet_server_message_server( pair.server ), pair.time ) == ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED );
    check( public_not_released_server_count( &pair ) == 1 );

    // a later message is released normally, and the refusal's count stays
    public_not_released_send_from_client( &pair, 10 );
    const uint8_t * next = NULL;
    check( public_not_released_receive_server( &pair, &next, &bytes ) );
    rocketnet_message_server_release( rocketnet_server_message_server( pair.server ), next );
    pair.time += 0.01;
    rocketnet_server_update( pair.server, pair.time );
    check( public_not_released_server_count( &pair ) == 1 );

    rocketnet_assert_function( saved_assert );
    public_not_released_pair_teardown( &pair );
}
REGISTER_UNIT_TEST( test_public_not_released_counter_server );

static void test_public_not_released_counter_starts_at_zero()
{
    struct public_not_released_pair_t pair;
    check( public_not_released_pair_setup( &pair ) );
    check( public_not_released_client_count( &pair ) == 0 );
    check( public_not_released_server_count( &pair ) == 0 );
    public_not_released_pair_teardown( &pair );
}
REGISTER_UNIT_TEST( test_public_not_released_counter_starts_at_zero );

// the public receive's batch is released by the next update: holding it
// across the update is no refusal, and the counter does not move
static void test_public_not_released_batch_released_by_update()
{
    struct public_not_released_pair_t pair;
    check( public_not_released_pair_setup( &pair ) );
    check( public_not_released_connect( &pair ) );
    public_not_released_send_from_server( &pair, 11 );
    public_not_released_send_from_client( &pair, 12 );
    const struct rocketnet_message_batch_t * at_client = NULL;
    const struct rocketnet_message_batch_t * at_server = NULL;
    bool client_got = false, server_got = false;
    for ( int i = 0; i < 20 && !( client_got && server_got ); i++ )
    {
        public_not_released_pump( &pair );
        client_got = client_got || ( ( at_client = rocketnet_client_receive_messages( pair.client ) ) != NULL );
        server_got = server_got || ( ( at_server = rocketnet_server_receive_messages( pair.server ) ) != NULL );
    }
    check( client_got && server_got );
    pair.time += 0.01;
    rocketnet_client_update( pair.client, pair.time );
    rocketnet_server_update( pair.server, pair.time );
    check( public_not_released_client_count( &pair ) == 0 );
    check( public_not_released_server_count( &pair ) == 0 );
    public_not_released_pair_teardown( &pair );
}
REGISTER_UNIT_TEST( test_public_not_released_batch_released_by_update );
