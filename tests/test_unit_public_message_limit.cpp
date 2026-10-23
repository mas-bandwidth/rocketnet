/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "portable_crt.h"
#include "unit_shutdown.h"
#include "token_string.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_fragment.h"
#include "../sdk/rocketnet_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define check rocketnet_unit_check

// This compatibility definition lets the same regression compile on the old
// header and fail by assertion, rather than failing its baseline build.
#ifdef ROCKETNET_MAX_MESSAGE_BYTES
static const bool message_limit_is_public = true;
#else
static const bool message_limit_is_public = false;
#define ROCKETNET_MAX_MESSAGE_BYTES ( 1 << 20 )
#endif

static void test_public_message_limit_defaults_and_constant()
{
    check( message_limit_is_public );
    check( ROCKETNET_MAX_MESSAGE_BYTES == 1048576 );
    rocketnet_client_config_t client;
    rocketnet_server_config_t server;
    check( rocketnet_client_default_config( &client ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_default_config( &server ) == ROCKETNET_STATUS_OK );
    check( client.max_message_bytes == 262144 );
    check( server.max_message_bytes == 262144 );
}
REGISTER_UNIT_TEST( test_public_message_limit_defaults_and_constant );

static void test_public_message_limit_cross_field_bounds()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = ROCKETNET_MAX_MESSAGE_BYTES;
    config.send_message_buffer_bytes = 8 * ROCKETNET_MAX_MESSAGE_BYTES;
    config.max_message_bytes = ROCKETNET_MAX_MESSAGE_BYTES;
    check( rocketnet_message_validate_config( &config ) );
    config.max_message_bytes++;
    check( !rocketnet_message_validate_config( &config ) );
    config.max_message_bytes = ROCKETNET_MAX_MESSAGE_BYTES;
    config.receive_message_buffer_bytes /= 2;
    check( !rocketnet_message_validate_config( &config ) );
    config.receive_message_buffer_bytes = ROCKETNET_MAX_MESSAGE_BYTES;
    config.reliable_messages_in_flight = 512;
    check( !rocketnet_message_validate_config( &config ) );
    config.reliable_messages_in_flight = 1024;
    config.send_message_buffer_bytes = 12288;
    check( !rocketnet_message_validate_config( &config ) );
    rocketnet_message_default_config( &config );
    config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    check( rocketnet_message_validate_config( &config ) );
    config.max_message_bytes--;
    check( !rocketnet_message_validate_config( &config ) );
}
REGISTER_UNIT_TEST( test_public_message_limit_cross_field_bounds );

static void test_public_message_limit_does_not_expand_transport_memory()
{
    rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    rocketnet_server_transport_config_t transport;
    rocketnet_message_config_t message;
    uint32_t kbps = 0;
    check( rocketnet_server_config_resolve( &config, false, &transport, &message, &kbps ) );
    rocketnet_server_memory_plan_t before, after;
    check( rocketnet_server_transport_memory_plan( &transport, "portable", before ) );
    check( before.reassembly_bytes < 512ULL * 1024 * 1024 );
    config.max_message_bytes = ROCKETNET_MAX_MESSAGE_BYTES;
    config.receive_message_buffer_bytes = ROCKETNET_MAX_MESSAGE_BYTES;
    config.send_message_buffer_bytes = 8 * ROCKETNET_MAX_MESSAGE_BYTES;
    check( rocketnet_server_config_resolve( &config, false, &transport, &message, &kbps ) );
    check( rocketnet_message_validate_config( &message ) );
    check( message.max_message_bytes == ROCKETNET_MAX_MESSAGE_BYTES );
    check( rocketnet_server_transport_memory_plan( &transport, "portable", after ) );
    check( after.reassembly_bytes == before.reassembly_bytes );
    check( after.send_arena_bytes == before.send_arena_bytes );
    check( after.total_bytes == before.total_bytes );
    const int payload = ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    check( payload == 16384 );
    check( rocketnet_reassembly_memory_allowed_delivered( 1000, 8, payload,
        rocketnet_server_reassembly_delivered_capacity( 1000, 8 ), rocketnet_server_reassembly_delivered_bytes( 1000, 8, payload ) ) );
    // A naive assignment of the application default to the transport cap
    // exceeds its budget before any allocation. Exercise the actual planner.
    check( !rocketnet_reassembly_memory_allowed_delivered( 1000, 8, 262144,
        rocketnet_server_reassembly_delivered_capacity( 1000, 8 ), rocketnet_server_reassembly_delivered_bytes( 1000, 8, 262144 ) ) );
}
REGISTER_UNIT_TEST( test_public_message_limit_does_not_expand_transport_memory );

struct message_limit_env_t
{
    const char * name;
    bool present;
    std::string previous;
    explicit message_limit_env_t( const char * key ) : name( key ), present( false )
    {
        char buffer[4096];
        present = rocketnet_test_getenv( key, buffer, sizeof( buffer ) ) != 0;
        previous = buffer;
    }
    void set( const char * value )
    {
#if defined( _WIN32 )
        check( _putenv_s( name, value ? value : "" ) == 0 );
#else
        check( value ? setenv( name, value, 1 ) == 0 : unsetenv( name ) == 0 );
#endif
    }
    ~message_limit_env_t() { set( present ? previous.c_str() : NULL ); }
};

static void test_public_message_limit_env_is_endpoint_scoped()
{
    message_limit_env_t client_env( "ROCKETNET_CLIENT_MAX_MESSAGE_BYTES" );
    message_limit_env_t server_env( "ROCKETNET_SERVER_MAX_MESSAGE_BYTES" );
    client_env.set( "131072" );
    server_env.set( "65536" );
    rocketnet_client_config_t client;
    rocketnet_server_config_t server;
    check( rocketnet_client_default_config( &client ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_default_config( &server ) == ROCKETNET_STATUS_OK );
    rocketnet_client_transport_config_t client_transport;
    rocketnet_server_transport_config_t server_transport;
    rocketnet_message_config_t message;
    uint32_t kbps = 0;
    check( rocketnet_client_config_resolve( &client, true, &client_transport, &message, &kbps ) );
    check( message.max_message_bytes == 131072 );
    check( !client_transport.max_fragmented_messages || client_transport.max_fragmented_messages == ROCKETNET_DEFAULT_REASSEMBLY_SLOTS );
    check( rocketnet_server_config_resolve( &server, true, &server_transport, &message, &kbps ) );
    check( message.max_message_bytes == 65536 );
    check( !server_transport.max_fragmented_messages || server_transport.max_fragmented_messages == ROCKETNET_DEFAULT_REASSEMBLY_SLOTS );
    client_env.set( "131072junk" );
    check( !rocketnet_client_config_resolve( &client, true, &client_transport, &message, &kbps ) );
    check( rocketnet_server_config_resolve( &server, true, &server_transport, &message, &kbps ) );
    check( message.max_message_bytes == 65536 );
}
REGISTER_UNIT_TEST( test_public_message_limit_env_is_endpoint_scoped );

static double message_limit_now = 1000.0;
static double message_limit_clock( void * ) { return message_limit_now; }

static void message_limit_pump( rocketnet_server_t * server, rocketnet_client_t * client )
{
    rocketnet_client_update( client, message_limit_now );
    rocketnet_client_send_packets( client );
    if ( rocketnet_server_running( server ) )
    {
        rocketnet_server_update( server, message_limit_now );
        rocketnet_server_send_packets( server );
    }
    message_limit_now += 0.01;
}

static void test_public_message_limit_unreliable_up_to_max_message_bytes()
{
    rocketnet_quiet( 1 );
    message_limit_now = 1000.0;

    // one size field: neither public config carries a second payload limit,
    // and the transport's fragmentation capacity is a derived constant

#ifdef ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES
    check( ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES == 361728 );
#else
    check( false );                              // the transport's fragmentation capacity is a define
#endif
    int num_client_fields = 0;
    const struct rocketnet_config_field_t * client_fields = rocketnet_client_transport_config_fields( &num_client_fields );
    check( num_client_fields > 0 );
    for ( int i = 0; i < num_client_fields; i++ )
        check( strcmp( client_fields[i].name, "max_payload_bytes" ) != 0 );
    int num_server_fields = 0;
    const struct rocketnet_config_field_t * server_fields = rocketnet_server_transport_config_fields( &num_server_fields );
    check( num_server_fields > 0 );
    for ( int i = 0; i < num_server_fields; i++ )
        check( strcmp( server_fields[i].name, "max_payload_bytes" ) != 0 );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    const uint64_t protocol_id = 0x7A11C1C1E5EED001ULL;
    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( backend_public ) );
    server_config.transport = "loopback";
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = message_limit_clock;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, server, protocol_id, addresses, 1, 42, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        message_limit_pump( server, client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int payload = 20000;
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, payload );
    check( out != NULL );
    for ( int i = 0; i < payload; i++ )
        out[i] = (uint8_t) i;
    check( rocketnet_client_finish_message( client, payload ) != 0 );

    bool received_whole = false;
    for ( int tick = 0; tick < 800 && !received_whole; tick++ )
    {
        message_limit_pump( server, client );
        const rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                if ( batch->bytes[i] == payload )
                {
                    bool ok = true;
                    for ( int j = 0; j < payload; j++ )
                        if ( batch->data[i][j] != (uint8_t) j )
                            ok = false;
                    received_whole = ok;
                }
            }
        }
    }
    check( received_whole );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_public_message_limit_unreliable_up_to_max_message_bytes );
