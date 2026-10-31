/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Connect-token create and expire checks can read the game clock: with a
// configured token_epoch_unix_seconds the token timestamp is epoch plus the caller's
// time, floored to whole seconds, and no wall clock is read; with the field
// zero the check reads the wall clock as before. Time is an argument starting
// at 1000.0; client refusals and the in-memory server protocol need no socket.

#include "test_unit_registry.h"
#include "native_shutdown.h"

#include "rocketnet_internal.h"
#include "rocketnet_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_ack.h"
#include "rocketnet_keys.h"
#include "rocketnet_read_write.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rocketnet_config.h"
#include "loopback_hub.h"

#define check rocketnet_unit_check

#define TOKEN_EPOCH_UNIX_SECONDS    1700000000ULL
#define GAME_CLOCK_NOW      1000.0
#define GAME_CLOCK_LATER    1011.0
#define TOKEN_LIFETIME      10ULL

struct token_time_game_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
};

static void token_time_game_mint( uint8_t * token_data, uint64_t create, uint64_t expire )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = expire;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// the client transport reads its epoch at create: the token is admitted only
// while create <= epoch + time < expire

static int token_time_game_client_create( uint64_t create, uint64_t expire, uint64_t epoch, double time )
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    token_time_game_mint( token_data, create, expire );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    memset( config.packet_filter, 0x5A, sizeof( config.packet_filter ) );
    config.token_epoch_unix_seconds = epoch;

    int status = 0;
    struct rocketnet_client_transport_t * transport = rocketnet_client_transport_create_current_ex( NULL, token_data, &config, time, &status );
    if ( transport )
    {
        rn_native_client_transport_stop_and_destroy( transport );
        return ROCKETNET_STATUS_OK;
    }

    return status;
}

static void token_time_game_server_setup( struct token_time_game_server_t * t )
{
    memset( t, 0, sizeof( *t ) );
    struct rocketnet_server_protocol_t * server = &t->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );

    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 8 );
    server->client_slots = rocketnet_client_slots_create( NULL, 1 );
    server->acks = rocketnet_server_acks_create( NULL, 1 );
    server->counters = &t->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots &&
           server->acks );

    rocketnet_server_protocol_update( server, 1 );
}

static void token_time_game_server_teardown( struct token_time_game_server_t * t )
{
    rocketnet_encryption_map_destroy( t->server.encryption_map );
    rocketnet_used_token_history_destroy( t->server.used_token_history );
    rocketnet_client_slots_destroy( t->server.client_slots );
    rocketnet_server_acks_destroy( t->server.acks );
}

static int token_time_game_server_request( struct token_time_game_server_t * t, uint64_t create, uint64_t expire, uint64_t timestamp )
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    token_time_game_mint( token_data, create, expire );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );

    struct rocketnet_address_t client_address;
    check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    return rocketnet_server_protocol_process_connection_request( &t->server, &client_address, request,
                                                                 ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 timestamp, 1000.0, response, &response_bytes );
}

// a token minted at epoch + 1000 and expiring ten seconds later connects at
// update time 1000.0 on both ends, whatever the wall clock says

static void test_token_time_game_clock_admits_a_fixed_token()
{
    const uint64_t create = TOKEN_EPOCH_UNIX_SECONDS + 1000;
    const uint64_t expire = create + TOKEN_LIFETIME;

    check( ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_ORIGINAL == offsetof( struct rocketnet_client_transport_config_t, time_callback ) + sizeof( void * ) );
    check( ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2 == sizeof( struct rocketnet_client_transport_config_t ) );
    {
        static const size_t known[] = { ROCKETNET_CLIENT_TRANSPORT_CONFIG_KNOWN_BYTES };
        struct rocketnet_client_transport_config_t defaults, old_caller, read;
        rocketnet_client_transport_default_config( &defaults );
        memset( &old_caller, 0xEE, sizeof( old_caller ) );
        memcpy( &old_caller, &defaults, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_ORIGINAL );
        old_caller.packet_filter[0] = (uint8_t) ( defaults.packet_filter[0] ^ 0x5A );
        memset( &read, 0xAB, sizeof( read ) );
        check( rocketnet_config_read_sized( "rocketnet_client_transport_config_t", &read, &defaults, sizeof( defaults ), &old_caller, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_ORIGINAL, known, 2 ) == 1 );
        check( read.packet_filter[0] == (uint8_t) ( defaults.packet_filter[0] ^ 0x5A ) );
        check( read.state_change_callback == NULL );
        check( read.time_callback == NULL );
        check( read.token_epoch_unix_seconds == 0 );
    }
    struct token_time_game_server_t t;
    token_time_game_server_setup( &t );
    check( token_time_game_server_request( &t, create, expire, TOKEN_EPOCH_UNIX_SECONDS + 1000 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    token_time_game_server_teardown( &t );

    printf( "test_token_time_game_clock_admits_a_fixed_token\n" );
}

// the same token is refused once epoch + time reaches expire, on both ends

static void test_token_time_game_clock_refuses_after_expire()
{
    const uint64_t create = TOKEN_EPOCH_UNIX_SECONDS + 1000;
    const uint64_t expire = create + TOKEN_LIFETIME;

    check( token_time_game_client_create( create, expire, TOKEN_EPOCH_UNIX_SECONDS, GAME_CLOCK_LATER ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );

    struct token_time_game_server_t t;
    token_time_game_server_setup( &t );
    check( token_time_game_server_request( &t, create, expire, TOKEN_EPOCH_UNIX_SECONDS + 1011 ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] == 1 );

    token_time_game_server_teardown( &t );

    printf( "test_token_time_game_clock_refuses_after_expire\n" );
}

// with the field zero a token stamped from a fixed past epoch is refused as
// expired, as before: the wall clock is the clock

static void test_token_time_game_clock_off_reads_the_wall_clock()
{
    check( token_time_game_client_create( 1000, 1010, 0, GAME_CLOCK_NOW ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );

    struct token_time_game_server_t t;
    token_time_game_server_setup( &t );
    check( token_time_game_server_request( &t, 1000, 1010, TOKEN_EPOCH_UNIX_SECONDS + 2000000000ULL ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] == 1 );

    token_time_game_server_teardown( &t );

    printf( "test_token_time_game_clock_off_reads_the_wall_clock\n" );
}

REGISTER_UNIT_TEST( test_token_time_game_clock_admits_a_fixed_token );
REGISTER_UNIT_TEST( test_token_time_game_clock_refuses_after_expire );
REGISTER_UNIT_TEST( test_token_time_game_clock_off_reads_the_wall_clock );
