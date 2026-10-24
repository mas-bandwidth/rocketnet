/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "portable_crt.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_fragment.h"
#include "../sdk/rocketnet_memory.h"
#include <stdlib.h>
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
    check( client.max_message_bytes == ROCKETNET_MAX_MESSAGE_BYTES );
    check( server.max_message_bytes == ROCKETNET_MAX_MESSAGE_BYTES );
    check( client.receive_message_buffer_bytes == ROCKETNET_MAX_MESSAGE_BYTES );
    check( server.receive_message_buffer_bytes == ROCKETNET_MAX_MESSAGE_BYTES );
    check( client.send_message_buffer_bytes == 4 * ROCKETNET_MAX_MESSAGE_BYTES );
    check( server.send_message_buffer_bytes == 4 * ROCKETNET_MAX_MESSAGE_BYTES );
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
    const int payload = transport.max_payload_bytes ? transport.max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    check( payload == 16384 );
    // at the build's most clients, since the budget scales with them (512 MiB per 1000)
    check( rocketnet_reassembly_memory_allowed_delivered( ROCKETNET_MAX_CLIENTS, 8, payload,
        rocketnet_server_reassembly_delivered_capacity( ROCKETNET_MAX_CLIENTS, 8 ), rocketnet_server_reassembly_delivered_bytes( ROCKETNET_MAX_CLIENTS, 8, payload ) ) );
    // A naive assignment of the application default to the transport cap
    // exceeds its budget before any allocation. Exercise the actual planner.
    check( !rocketnet_reassembly_memory_allowed_delivered( ROCKETNET_MAX_CLIENTS, 8, 262144,
        rocketnet_server_reassembly_delivered_capacity( ROCKETNET_MAX_CLIENTS, 8 ), rocketnet_server_reassembly_delivered_bytes( ROCKETNET_MAX_CLIENTS, 8, 262144 ) ) );
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
    check( !client_transport.max_payload_bytes || client_transport.max_payload_bytes == 16384 );
    check( rocketnet_server_config_resolve( &server, true, &server_transport, &message, &kbps ) );
    check( message.max_message_bytes == 65536 );
    check( !server_transport.max_payload_bytes || server_transport.max_payload_bytes == 16384 );
    client_env.set( "131072junk" );
    check( !rocketnet_client_config_resolve( &client, true, &client_transport, &message, &kbps ) );
    check( rocketnet_server_config_resolve( &server, true, &server_transport, &message, &kbps ) );
    check( message.max_message_bytes == 65536 );
}
REGISTER_UNIT_TEST( test_public_message_limit_env_is_endpoint_scoped );
