/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_ipv4_mapped_refused.cpp -- an IPv4-mapped IPv6 address
// ([::ffff:a.b.c.d]:port) is an IPv4 client's address as a dual-stack socket
// reports it, never a server's IPv6 address. Given as the IPv6 address of a
// connect token or of rocketnet_server_create, every handshake would be lost:
// the client sends to an IPv4 server that has no such IPv6 address, and the
// server advertises an address no client reaches. Both refuse it with
// ROCKETNET_STATUS_INVALID_ADDRESS, as the bind-address rule refuses it for
// bind_ipv6_address; a plain IPv6 address still passes.
//
// No socket, no clock: server create binds nothing (start does), and the
// token writer reads its time from the config.

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"

#include <string.h>

#define check rocketnet_unit_check

static const char * const MAPPED = "[::ffff:127.0.0.1]:40000";

static enum rocketnet_status_t mapped_server_create( const char * ipv4, const char * ipv6 )
{
    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = 0x4D41505045440001ULL;
    config.max_clients = 2;
    static const uint8_t filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x4D, 0x41, 0x50, 0x50 };
    memcpy( config.packet_filter, filter, sizeof( config.packet_filter ) );   // a production build refuses the test filter

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, ipv4, ipv6, &config, &status );
    check( ( server != NULL ) == ( status == ROCKETNET_STATUS_OK ) );
    if ( server )
    {
        rocketnet_server_destroy( server );
    }
    return status;
}

static void test_server_create_refuses_ipv4_mapped_ipv6_address()
{
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    const enum rocketnet_status_t mapped_alone = mapped_server_create( NULL, MAPPED );
    const enum rocketnet_status_t mapped_second = mapped_server_create( "127.0.0.1:40000", MAPPED );
    const enum rocketnet_status_t plain = mapped_server_create( NULL, "[::1]:40000" );
    const enum rocketnet_status_t both = mapped_server_create( "127.0.0.1:40000", "[::1]:40000" );

    rocketnet_log_level( saved_level );

    check( mapped_alone == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( mapped_second == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( plain == ROCKETNET_STATUS_OK );
    check( both == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_server_create_refuses_ipv4_mapped_ipv6_address );

static enum rocketnet_status_t mapped_token( const char * ipv4, const char * ipv6, char * token )
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_connect_token_config_t config;
    rocketnet_connect_token_default_config( &config );
    memcpy( config.backend_secret_key, backend_secret, sizeof( config.backend_secret_key ) );
    memset( config.server_public_key, 0x42, sizeof( config.server_public_key ) );
    config.server_address_ipv4 = ipv4;
    config.server_address_ipv6 = ipv6;
    config.client_id = 1;
    config.protocol_id = 0x4D41505045440001ULL;
    config.expire_seconds = 30;
    config.timeout_seconds = 10;
    config.unix_timestamp = 1000;

    const enum rocketnet_status_t status = rocketnet_create_connect_token( &config, token );
    memset( &config, 0, sizeof( config ) );
    memset( backend_secret, 0, sizeof( backend_secret ) );
    return status;
}

static void test_connect_token_refuses_ipv4_mapped_ipv6_address()
{
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    static char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    memset( token, 'x', sizeof( token ) );
    const enum rocketnet_status_t mapped_alone = mapped_token( NULL, MAPPED, token );
    const bool unchanged = token[0] == 'x' && token[sizeof( token ) - 1] == 'x';
    const enum rocketnet_status_t mapped_second = mapped_token( "127.0.0.1:40000", MAPPED, token );
    const enum rocketnet_status_t plain = mapped_token( NULL, "[::1]:40000", token );
    const enum rocketnet_status_t both = mapped_token( "127.0.0.1:40000", "[::1]:40000", token );

    rocketnet_log_level( saved_level );

    check( mapped_alone == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( unchanged );
    check( mapped_second == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( plain == ROCKETNET_STATUS_OK );
    check( both == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_connect_token_refuses_ipv4_mapped_ipv6_address );
