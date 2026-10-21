/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A server names one family or both. Refusals, the one-family path and the
// two-client exchange run on the loopback transport. The arrival-family token
// rule runs on the protocol object, with time passed in. No test sleeps,
// opens a socket, or reads a clock. The clock starts at 1000.0.

#include "test_unit_registry.h"
#include "sdk/rocketnet_connect_token.h"
#include "sdk/rocketnet_base64.h"

#include "sdk/rocketnet.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "test/counters_expect.h"

#define BOTH_FAMILIES_PROTOCOL 0xB071FA11A11E5001ULL

#if defined(_WIN32)
static int both_families_setenv( const char * name, const char * value ) { return _putenv_s( name, value ); }
static int both_families_unsetenv( const char * name ) { return _putenv_s( name, "" ); }
#else
static int both_families_setenv( const char * name, const char * value ) { return setenv( name, value, 1 ); }
static int both_families_unsetenv( const char * name ) { return unsetenv( name ); }
#endif

struct both_families_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static double both_families_now = 1000.0;
static double both_families_clock( void * ) { return both_families_now; }

static void both_families_identity( both_families_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static void both_families_fill( rocketnet_server_config_t * config, const both_families_identity_t * identity, int max_clients )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = BOTH_FAMILIES_PROTOCOL;
    config->max_clients = max_clients;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
}

static void both_families_client_config( rocketnet_client_test_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = both_families_clock;
}

static void both_families_token( const both_families_identity_t * identity, uint64_t client_id, const char * address, char * token_string )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
        BOTH_FAMILIES_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

// creates with the IPv4 and IPv6 addresses, then starts; a refused create
// returns its status with *out NULL
static enum rocketnet_status_t both_families_start( both_families_identity_t * identity, const char * ipv4, const char * ipv6, struct rocketnet_server_t ** out )
{
    rocketnet_server_config_t config;
    both_families_fill( &config, identity, 2 );
    both_families_now = 1000.0;
    rocketnet_quiet( 1 );
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, ipv4, ipv6, &config, &create_status );
    if ( !server )
    {
        *out = NULL;
        return create_status;
    }
    rocketnet_server_public_key( server, identity->server_public );
    check( create_status == ROCKETNET_STATUS_OK );
    enum rocketnet_status_t status = rocketnet_server_start( server );
    *out = server;
    return status;
}

static void both_families_stop( struct rocketnet_server_t * server )
{
    if ( !server )
        return;
    rocketnet_server_stop( server );
    rocketnet_server_destroy( server );
}

static void test_server_refuses_second_address_of_the_same_family()
{
    both_families_identity_t identity;
    both_families_identity( &identity );
    struct rocketnet_server_t * server = NULL;
    enum rocketnet_status_t status = both_families_start( &identity, "127.0.0.1:0", "10.0.0.1:0", &server );
    both_families_stop( server );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
}
REGISTER_UNIT_TEST( test_server_refuses_second_address_of_the_same_family );

static void test_server_refuses_second_address_on_a_different_port()
{
    both_families_identity_t identity;
    both_families_identity( &identity );
    struct rocketnet_server_t * server = NULL;
    enum rocketnet_status_t status = both_families_start( &identity, "127.0.0.1:0", "[::1]:1", &server );
    both_families_stop( server );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
}
REGISTER_UNIT_TEST( test_server_refuses_second_address_on_a_different_port );

static void test_server_refuses_an_unparseable_second_address()
{
    both_families_identity_t identity;
    both_families_identity( &identity );
    struct rocketnet_server_t * server = NULL;
    enum rocketnet_status_t status = both_families_start( &identity, "127.0.0.1:0", "not-an-address", &server );
    both_families_stop( server );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
}
REGISTER_UNIT_TEST( test_server_refuses_an_unparseable_second_address );

// creates a stopped server with the IPv4 and IPv6 addresses. Nothing binds.
static struct rocketnet_server_t * both_families_create( const both_families_identity_t * identity, const char * ipv4, const char * ipv6 )
{
    rocketnet_server_config_t config;
    both_families_fill( &config, identity, 2 );
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OUT_OF_MEMORY;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, ipv4, ipv6, &config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    return server;
}

static void both_families_check_served( const struct rocketnet_server_t * server, const char * ipv4, const char * ipv6 )
{
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    check( rocketnet_server_served_address( server, ROCKETNET_ADDRESS_IPV4, address, sizeof( address ) ) == ( ipv4 != NULL ) );
    if ( ipv4 )
        check( strcmp( address, ipv4 ) == 0 );
    check( rocketnet_server_served_address( server, ROCKETNET_ADDRESS_IPV6, address, sizeof( address ) ) == ( ipv6 != NULL ) );
    if ( ipv6 )
        check( strcmp( address, ipv6 ) == 0 );
}

static void test_server_address_environment_applies_to_every_server()
{
    both_families_identity_t identity;
    both_families_identity( &identity );
    rocketnet_quiet( 1 );

    check( both_families_setenv( "ROCKETNET_SERVER_IPV4_ADDRESS", "10.1.2.3:50123" ) == 0 );
    check( both_families_setenv( "ROCKETNET_SERVER_IPV6_ADDRESS", "[2001:db8::7]:50123" ) == 0 );

    // two servers created while the variables are set both take them,
    // whatever addresses their creates name
    struct rocketnet_server_t * first = both_families_create( &identity, "127.0.0.1:0", NULL );
    struct rocketnet_server_t * second = both_families_create( &identity, NULL, "[::1]:0" );
    both_families_check_served( first, "10.1.2.3:50123", "[2001:db8::7]:50123" );
    both_families_check_served( second, "10.1.2.3:50123", "[2001:db8::7]:50123" );
    rocketnet_server_destroy( first );
    rocketnet_server_destroy( second );

    check( both_families_unsetenv( "ROCKETNET_SERVER_IPV4_ADDRESS" ) == 0 );
    check( both_families_unsetenv( "ROCKETNET_SERVER_IPV6_ADDRESS" ) == 0 );

    // unset, each server serves its create arguments again
    struct rocketnet_server_t * third = both_families_create( &identity, "127.0.0.1:0", NULL );
    both_families_check_served( third, "127.0.0.1:0", NULL );
    rocketnet_server_destroy( third );
}
REGISTER_UNIT_TEST( test_server_address_environment_applies_to_every_server );

static void test_server_create_refuses_no_address()
{
    both_families_identity_t identity;
    both_families_identity( &identity );
    struct rocketnet_server_t * server = NULL;
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( both_families_start( &identity, NULL, NULL, &server ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( server == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    // each argument must be its own family
    check( both_families_start( &identity, "[::1]:0", NULL, &server ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( both_families_start( &identity, NULL, "127.0.0.1:0", &server ) == ROCKETNET_STATUS_INVALID_ADDRESS );
}
REGISTER_UNIT_TEST( test_server_create_refuses_no_address );

static void both_families_pump( struct rocketnet_server_t * server, struct rocketnet_client_t * ipv4, struct rocketnet_client_t * ipv6, double * time )
{
    both_families_now = *time;
    if ( ipv4 )
    {
        rocketnet_client_update( ipv4, *time );
        rocketnet_client_send_packets( ipv4 );
    }
    if ( ipv6 )
    {
        rocketnet_client_update( ipv6, *time );
        rocketnet_client_send_packets( ipv6 );
    }
    rocketnet_server_update( server, *time );
    rocketnet_server_send_packets( server );
    *time += 0.01;
}

static struct rocketnet_client_t * both_families_connect_client( const both_families_identity_t * identity, uint64_t client_id, const char * address )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    both_families_token( identity, client_id, address, token );
    rocketnet_client_test_config_t config;
    both_families_client_config( &config );
    enum rocketnet_status_t create_status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    return client;
}

static void test_server_serves_ipv4_and_ipv6_clients_on_one_port()
{
    both_families_identity_t identity;
    both_families_identity( &identity );
    struct rocketnet_server_t * server = NULL;
    enum rocketnet_status_t status = both_families_start( &identity, "127.0.0.1:0", "[::1]:0", &server );
    check( status == ROCKETNET_STATUS_OK );

    const int port = rocketnet_server_port( server );
    char ipv4_address[64];
    char ipv6_address[64];
    snprintf( ipv4_address, sizeof( ipv4_address ), "127.0.0.1:%d", port );
    snprintf( ipv6_address, sizeof( ipv6_address ), "[::1]:%d", port );

    struct rocketnet_client_t * ipv4 = both_families_connect_client( &identity, 0xB071, ipv4_address );
    double time = 1000.0;
    for ( int i = 0; i < 400 && rocketnet_client_state( ipv4 ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        both_families_pump( server, ipv4, NULL, &time );
    check( rocketnet_client_state( ipv4 ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server, 0 ) != 0 );

    struct rocketnet_client_t * ipv6 = both_families_connect_client( &identity, 0xB072, ipv6_address );
    for ( int i = 0; i < 400 && rocketnet_client_state( ipv6 ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        both_families_pump( server, ipv4, ipv6, &time );
    check( rocketnet_client_state( ipv6 ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server, 1 ) != 0 );

    const uint8_t from_ipv4[4] = { 1, 2, 3, 4 };
    const uint8_t from_ipv6[4] = { 5, 6, 7, 8 };
    const uint8_t to_ipv4[4] = { 9, 10, 11, 12 };
    const uint8_t to_ipv6[4] = { 13, 14, 15, 16 };

    uint8_t * out = rocketnet_client_start_message( ipv4, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( out != NULL );
    memcpy( out, from_ipv4, 4 );
    check( rocketnet_client_finish_message( ipv4, 4 ) != 0 );
    out = rocketnet_client_start_message( ipv6, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( out != NULL );
    memcpy( out, from_ipv6, 4 );
    check( rocketnet_client_finish_message( ipv6, 4 ) != 0 );
    out = rocketnet_server_start_message( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( out != NULL );
    memcpy( out, to_ipv4, 4 );
    check( rocketnet_server_finish_message( server, 4 ) != 0 );
    out = rocketnet_server_start_message( server, 1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( out != NULL );
    memcpy( out, to_ipv6, 4 );
    check( rocketnet_server_finish_message( server, 4 ) != 0 );

    int got_ipv4 = 0, got_ipv6 = 0, got_from_ipv4 = 0, got_from_ipv6 = 0;
    for ( int step = 0; step < 400 && ( got_ipv4 < 1 || got_ipv6 < 1 || got_from_ipv4 < 1 || got_from_ipv6 < 1 ); step++ )
    {
        both_families_pump( server, ipv4, ipv6, &time );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                check( batch->channel[i] == 0 && batch->bytes[i] == 4 );
                if ( batch->client_index[i] == 0 )
                {
                    check( memcmp( batch->data[i], from_ipv4, 4 ) == 0 );
                    got_from_ipv4++;
                }
                else
                {
                    check( batch->client_index[i] == 1 && memcmp( batch->data[i], from_ipv6, 4 ) == 0 );
                    got_from_ipv6++;
                }
            }
        }
        while ( ( batch = rocketnet_client_receive_messages( ipv4 ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                check( batch->channel[i] == 0 && batch->bytes[i] == 4 && memcmp( batch->data[i], to_ipv4, 4 ) == 0 );
                got_ipv4++;
            }
        }
        while ( ( batch = rocketnet_client_receive_messages( ipv6 ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                check( batch->channel[i] == 0 && batch->bytes[i] == 4 && memcmp( batch->data[i], to_ipv6, 4 ) == 0 );
                got_ipv6++;
            }
        }
    }
    check( got_from_ipv4 == 1 && got_from_ipv6 == 1 && got_ipv4 == 1 && got_ipv6 == 1 );

    rocketnet_client_destroy( ipv6 );
    rocketnet_client_destroy( ipv4 );
    both_families_stop( server );
}
REGISTER_UNIT_TEST( test_server_serves_ipv4_and_ipv6_clients_on_one_port );

static void test_one_family_server_is_unchanged()
{
    both_families_identity_t identity;
    both_families_identity( &identity );
    struct rocketnet_server_t * server = NULL;
    enum rocketnet_status_t status = both_families_start( &identity, "127.0.0.1:0", NULL, &server );
    check( status == ROCKETNET_STATUS_OK );

    const int port = rocketnet_server_port( server );
    char ipv4_address[64];
    char ipv6_address[64];
    snprintf( ipv4_address, sizeof( ipv4_address ), "127.0.0.1:%d", port );
    snprintf( ipv6_address, sizeof( ipv6_address ), "[::1]:%d", port );

    struct rocketnet_client_t * ipv4 = both_families_connect_client( &identity, 0xB071, ipv4_address );
    double time = 1000.0;
    for ( int i = 0; i < 400 && rocketnet_client_state( ipv4 ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        both_families_pump( server, ipv4, NULL, &time );
    check( rocketnet_client_state( ipv4 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    struct rocketnet_client_t * ipv6 = both_families_connect_client( &identity, 0xB072, ipv6_address );
    for ( int i = 0; i < 80; i++ )
        both_families_pump( server, ipv4, ipv6, &time );
    check( rocketnet_client_state( ipv6 ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server, 1 ) == 0 );

    rocketnet_client_destroy( ipv6 );
    rocketnet_client_destroy( ipv4 );
    both_families_stop( server );
}
REGISTER_UNIT_TEST( test_one_family_server_is_unchanged );

struct both_families_protocol_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
};

static void both_families_protocol_setup( struct both_families_protocol_t * test )
{
    memset( test, 0, sizeof( *test ) );
    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "127.0.0.1:9" ) );
    check( rocketnet_address_parse( &server->advertised_ipv6, "[::1]:9" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->max_token_lifetime_seconds = 600;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );
}

static void both_families_protocol_teardown( struct both_families_protocol_t * test )
{
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

static void both_families_mint( uint8_t * request, const uint8_t * ipv4, const uint8_t * ipv6 )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    memset( &private_fields, 0, sizeof( private_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = (uint8_t) ( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 );
    memcpy( signed_fields.ipv4_address, ipv4, 6 );
    memcpy( signed_fields.ipv6_address, ipv6, 18 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

// TokenAddressAdvertised: the arrival family's token address is the one
// that is checked. A foreign IPv6 beside a matching IPv4 is refused on an
// IPv6 arrival and accepted on an IPv4 arrival.

static void test_arrival_family_refuses_a_foreign_ipv6_address()
{
    struct both_families_protocol_t test;
    both_families_protocol_setup( &test );
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x09, 0x00 };
    const uint8_t foreign_ipv6[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x09, 0x00 };
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    both_families_mint( request, loopback_ipv4, foreign_ipv6 );
    struct rocketnet_address_t from;
    check( rocketnet_address_parse( &from, "[2001:db8::99]:5444" ) );
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int addr_counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED;
    const uint64_t before = test.counters.values[addr_counter];
    const int result = rocketnet_server_protocol_process_connection_request( &test.server, &from, request, sizeof( request ),
                                                                              ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( test.counters.values[addr_counter] == before + 1 );
    both_families_protocol_teardown( &test );
}
REGISTER_UNIT_TEST( test_arrival_family_refuses_a_foreign_ipv6_address );

static void test_arrival_family_accepts_the_matching_ipv4_address()
{
    struct both_families_protocol_t test;
    both_families_protocol_setup( &test );
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x09, 0x00 };
    const uint8_t foreign_ipv6[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x09, 0x00 };
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    both_families_mint( request, loopback_ipv4, foreign_ipv6 );
    struct rocketnet_address_t from;
    check( rocketnet_address_parse( &from, "198.51.100.7:5444" ) );
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int addr_counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED;
    const uint64_t before = test.counters.values[addr_counter];
    const int result = rocketnet_server_protocol_process_connection_request( &test.server, &from, request, sizeof( request ),
                                                                              ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes );
    check( result != ROCKETNET_SERVER_PROCESS_IGNORED );
    check( test.counters.values[addr_counter] == before );
    both_families_protocol_teardown( &test );
}
REGISTER_UNIT_TEST( test_arrival_family_accepts_the_matching_ipv4_address );

// rocketnet_server_address_is_advertised: the IPv4 loopback stands for the
// server only on an IPv6-wildcard ("[::]") server. A server that names a
// public IPv4 and a public IPv6 address refuses a token naming 127.0.0.1.

static void test_loopback_transport_named_two_family_server_refuses_a_token()
{
    struct both_families_protocol_t test;
    both_families_protocol_setup( &test );
    check( rocketnet_address_parse( &test.server.advertised_ipv4, "203.0.113.5:9" ) );
    check( rocketnet_address_parse( &test.server.advertised_ipv6, "[2001:db8::5]:9" ) );
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x09, 0x00 };
    const uint8_t foreign_ipv6[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x09, 0x00 };
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    both_families_mint( request, loopback_ipv4, foreign_ipv6 );
    struct rocketnet_address_t from;
    check( rocketnet_address_parse( &from, "198.51.100.7:5444" ) );
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int addr_counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED;
    const uint64_t before = test.counters.values[addr_counter];
    const int result = rocketnet_server_protocol_process_connection_request( &test.server, &from, request, sizeof( request ),
                                                                              ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( test.counters.values[addr_counter] == before + 1 );
    both_families_protocol_teardown( &test );
}
REGISTER_UNIT_TEST( test_loopback_transport_named_two_family_server_refuses_a_token );
