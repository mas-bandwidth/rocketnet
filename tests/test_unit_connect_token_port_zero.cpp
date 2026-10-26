/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A connect token naming a server address with port 0 can never reach a
// server: macOS refuses the socket connect, and Linux and Windows accept it
// and the packets reach no server. The client refuses such a token at create
// and at connect with ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO on every
// platform, for either flagged family, before any socket opens. A token whose
// flagged addresses all name a nonzero port is accepted.
//
// The token makers refuse the same address, so no maker mints a token the
// client refuses: the development maker returns ROCKETNET_STATUS_INVALID_ADDRESS
// for an address naming port 0 and writes nothing into the caller's token
// buffer, and the production writer refuses such an address before it writes
// any field.
//
// Every case runs in memory: the fake client driver or the loopback
// transport, no socket, and the game clock in place of the wall clock.

#include "test_unit_registry.h"

#include "unit_shutdown.h"
#include "fake_client_driver.h"
#include "token_string.h"

#include "rocketnet.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_layers.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV
// a token whose IPv4 address is 127.0.0.1:ipv4_port and whose IPv6 address
// is [::1]:ipv6_port, flagged by flags; ports are little-endian on the wire
static void port_zero_token( uint8_t * token, uint8_t flags, uint16_t ipv4_port, uint16_t ipv6_port )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = 1;
    signed_fields.create_timestamp = 1000;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = flags;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = (uint8_t) ( ipv4_port & 0xFF );
    signed_fields.ipv4_address[5] = (uint8_t) ( ipv4_port >> 8 );
    signed_fields.ipv6_address[14] = 1;
    signed_fields.ipv6_address[16] = (uint8_t) ( ipv6_port & 0xFF );
    signed_fields.ipv6_address[17] = (uint8_t) ( ipv6_port >> 8 );
    memset( signed_fields.server_public_key, 0x52, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

// the step-driven client transport create on the fake driver; returns the
// status, and destroys whatever was created
static int port_zero_client_transport_create( uint8_t flags, uint16_t ipv4_port, uint16_t ipv6_port )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    port_zero_token( token, flags, ipv4_port, ipv6_port );
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.game_clock_epoch = 1000;
    memset( config.packet_filter, 0x5A, sizeof( config.packet_filter ) );
    int status = -1;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_ex( fake, token, &config, 1000.0,
        &rocketnet_fake_client_operations_base, fake, &status );
    check( ( transport != NULL ) == ( status == ROCKETNET_STATUS_OK ) );
    if ( transport )
    {
        rn_test_client_transport_stop_and_destroy( transport );
        // a token naming both families may open the driver once per family
        // it tries (tests/test_unit_client_bind_family.cpp), so an accepted
        // create closes it at least once
        check( fake->destroy_calls >= 1 );
    }
    else
    {
        // refused before the driver opened anything
        check( fake->destroy_calls == 0 );
    }
    delete fake;
    return status;
}

static void test_client_transport_refuses_connect_token_port_zero()
{
    rocketnet_quiet( true );

    const uint8_t ipv4 = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t ipv6 = ROCKETNET_ADDRESS_FLAG_IPV6;
    const uint8_t both = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;

    // either family naming port 0 is refused
    check( port_zero_client_transport_create( ipv4, 0, 40000 ) == ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO );
    check( port_zero_client_transport_create( ipv6, 40000, 0 ) == ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO );

    // a token naming both families is refused when either names port 0,
    // whichever family this host would pick
    check( port_zero_client_transport_create( both, 0, 40000 ) == ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO );
    check( port_zero_client_transport_create( both, 40000, 0 ) == ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO );

    // an unflagged family's port is not read
    check( port_zero_client_transport_create( ipv4, 40000, 0 ) == ROCKETNET_STATUS_OK );

    // port 1, the smallest port, is a real port
    check( port_zero_client_transport_create( ipv4, 1, 0 ) == ROCKETNET_STATUS_OK );
    check( port_zero_client_transport_create( both, 40000, 40000 ) == ROCKETNET_STATUS_OK );

    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_client_transport_refuses_connect_token_port_zero );
#endif // #if ROCKETNET_DEV

// the byte a refusal must leave in the caller's token buffer: the makers
// write nothing into it, so every byte still holds the sentinel
#define PORT_ZERO_SENTINEL 0xA5

// true when every byte of the buffer still holds the sentinel, so a refusal
// that wrote nothing left the caller's bytes alone
static bool port_zero_buffer_untouched( const void * buffer, size_t bytes )
{
    const uint8_t * view = (const uint8_t*) buffer;
    for ( size_t i = 0; i < bytes; ++i )
    {
        if ( view[i] != PORT_ZERO_SENTINEL )
            return false;
    }
    return true;
}

static void test_token_makers_refuse_port_zero()
{
    rocketnet_quiet( true );

    #if ROCKETNET_DEV
    // the development maker refuses an address naming port 0, with or
    // without an explicit ":0", and writes nothing into the token buffer
    char dev_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];

    memset( dev_token, PORT_ZERO_SENTINEL, sizeof( dev_token ) );
    check( rocketnet_create_dev_connect_token( "127.0.0.1", NULL, rocketnet_dev_server_public_key, 60, 15, 1, 1, 1000, NULL, dev_token ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( port_zero_buffer_untouched( dev_token, sizeof( dev_token ) ) );

    memset( dev_token, PORT_ZERO_SENTINEL, sizeof( dev_token ) );
    check( rocketnet_create_dev_connect_token( "127.0.0.1:0", NULL, rocketnet_dev_server_public_key, 60, 15, 1, 1, 1000, NULL, dev_token ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( port_zero_buffer_untouched( dev_token, sizeof( dev_token ) ) );

    memset( dev_token, PORT_ZERO_SENTINEL, sizeof( dev_token ) );
    check( rocketnet_create_dev_connect_token( NULL, "[::1]:0", rocketnet_dev_server_public_key, 60, 15, 1, 1, 1000, NULL, dev_token ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( port_zero_buffer_untouched( dev_token, sizeof( dev_token ) ) );

    // an address with a nonzero port is accepted in either family, and an
    // IPv6 address is judged by its parsed port, not by the text of its
    // groups: "[2001:0db8::1]:40000" carries port 40000
    memset( dev_token, PORT_ZERO_SENTINEL, sizeof( dev_token ) );
    check( rocketnet_create_dev_connect_token( "127.0.0.1:40000", NULL, rocketnet_dev_server_public_key, 60, 15, 1, 1, 1000, NULL, dev_token ) == ROCKETNET_STATUS_OK );
    check( !port_zero_buffer_untouched( dev_token, sizeof( dev_token ) ) );

    memset( dev_token, PORT_ZERO_SENTINEL, sizeof( dev_token ) );
    check( rocketnet_create_dev_connect_token( NULL, "[::1]:40000", rocketnet_dev_server_public_key, 60, 15, 1, 1, 1000, NULL, dev_token ) == ROCKETNET_STATUS_OK );
    check( !port_zero_buffer_untouched( dev_token, sizeof( dev_token ) ) );

    memset( dev_token, PORT_ZERO_SENTINEL, sizeof( dev_token ) );
    check( rocketnet_create_dev_connect_token( NULL, "[2001:0db8::1]:40000", rocketnet_dev_server_public_key, 60, 15, 1, 1, 1000, NULL, dev_token ) == ROCKETNET_STATUS_OK );
    check( !port_zero_buffer_untouched( dev_token, sizeof( dev_token ) ) );
    #endif

    // the production writer refuses an address naming port 0 before it
    // writes any field, so the token buffer is left alone
    uint8_t production_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    uint8_t production_public_key[ROCKETNET_KEY_BYTES];
    uint8_t production_backend_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    memset( production_public_key, 0x52, sizeof( production_public_key ) );
    memset( production_backend_key, 0x53, sizeof( production_backend_key ) );

    const char * port_zero_addresses[] = { "127.0.0.1:0" };
    memset( production_token, PORT_ZERO_SENTINEL, sizeof( production_token ) );
    check( rocketnet_write_production_connect_token( production_backend_key, production_public_key, 1, port_zero_addresses, 1, 1, 1000, 1060, 15, NULL, production_token, sizeof( production_token ) ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( port_zero_buffer_untouched( production_token, sizeof( production_token ) ) );

    const char * port_less_addresses[] = { "127.0.0.1" };
    memset( production_token, PORT_ZERO_SENTINEL, sizeof( production_token ) );
    check( rocketnet_write_production_connect_token( production_backend_key, production_public_key, 1, port_less_addresses, 1, 1, 1000, 1060, 15, NULL, production_token, sizeof( production_token ) ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( port_zero_buffer_untouched( production_token, sizeof( production_token ) ) );

    const char * valid_addresses[] = { "127.0.0.1:40000" };
    memset( production_token, PORT_ZERO_SENTINEL, sizeof( production_token ) );
    check( rocketnet_write_production_connect_token( production_backend_key, production_public_key, 1, valid_addresses, 1, 1, 1000, 1060, 15, NULL, production_token, sizeof( production_token ) ) == ROCKETNET_STATUS_OK );
    check( !port_zero_buffer_untouched( production_token, sizeof( production_token ) ) );

    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_token_makers_refuse_port_zero );

#if ROCKETNET_DEV
static double port_zero_clock( void * ) { return 1000.0; }

static void test_client_connect_refuses_connect_token_port_zero()
{
    rocketnet_quiet( true );

    struct rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = 1;
    client_config.time_callback = port_zero_clock;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];

    port_zero_token( token, ROCKETNET_ADDRESS_FLAG_IPV4, 0, 0 );
    check( rn_test_token_to_string( token, token_string ) );
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_connect( client, token_string ) == ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_disconnected( client ) );

    // the refusal is not misuse, and the client takes the next connect the same way
    port_zero_token( token, ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6, 40000, 0 );
    check( rn_test_token_to_string( token, token_string ) );
    check( rocketnet_client_connect( client, token_string ) == ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_api_misuse_count() == misuse );

    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO ), "CONNECT_TOKEN_PORT_ZERO" ) == 0 );

    rocketnet_client_destroy( client );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_client_connect_refuses_connect_token_port_zero );
#endif // #if ROCKETNET_DEV
