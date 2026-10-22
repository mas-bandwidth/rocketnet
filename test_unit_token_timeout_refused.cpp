/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A connect token's timeout seconds is valid only when positive, at both
// ends (TRANSPORT.md t11 rule 2, t12 rule 6, t13.2). A client refuses a token
// whose timeout is zero or negative before it sends anything, in invalid
// connect token (-6) -- which the client transport create reports as
// ROCKETNET_STATUS_INVALID_CONNECT_TOKEN -- because a client that accepted one
// would never time out. A server ignores a request carrying one, counted
// CONNECTION_REQUEST_TIMEOUT_INVALID, and no server switch accepts it.
// Time is an argument; the clock starts at 1000.0 and nothing here sleeps,
// opens a socket or reads a clock.

#include "test_unit_registry.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static const int32_t token_timeout_refused_values[] = { 0, -1, INT32_MIN };

static void token_timeout_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                  struct rocketnet_connect_token_private_t * private_fields,
                                  int32_t timeout_seconds )
{
    memset( signed_fields, 0, sizeof(*signed_fields) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = timeout_seconds;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof(*private_fields) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

static void token_timeout_mint( uint8_t * token_data, int32_t timeout_seconds )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    token_timeout_fields( &signed_fields, &private_fields, timeout_seconds );
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void test_client_refuses_token_timeout_zero_or_negative()
{
    for ( size_t i = 0; i < sizeof( token_timeout_refused_values ) / sizeof( token_timeout_refused_values[0] ); i++ )
    {
        struct rocketnet_counters_t counters;
        memset( &counters, 0, sizeof(counters) );
        static struct rocketnet_client_protocol_t client;
        rocketnet_client_protocol_init( &client, &counters );

        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        token_timeout_mint( token_data, token_timeout_refused_values[i] );

        // refused at the point the token is read, with the named state
        check( !rocketnet_client_protocol_connect( &client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
        check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );

        // and nothing goes on the wire, now or later
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes = -1;
        rocketnet_client_protocol_update( &client, 1000.0, packet, &packet_bytes );
        check( packet_bytes == 0 );
        rocketnet_client_protocol_update( &client, 2000.0, packet, &packet_bytes );
        check( packet_bytes == 0 );
        check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );
    }

    // the control: the same token with a positive timeout begins an attempt
    {
        struct rocketnet_counters_t counters;
        memset( &counters, 0, sizeof(counters) );
        static struct rocketnet_client_protocol_t client;
        rocketnet_client_protocol_init( &client, &counters );
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        token_timeout_mint( token_data, 1 );
        check( rocketnet_client_protocol_connect( &client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
        check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
    }
}

static void test_server_refuses_token_timeout_zero_or_negative()
{
    for ( size_t i = 0; i < sizeof( token_timeout_refused_values ) / sizeof( token_timeout_refused_values[0] ); i++ )
    {
        static struct rocketnet_server_protocol_t server;
        static struct rocketnet_counters_t counters;
        memset( &server, 0, sizeof(server) );
        memset( &counters, 0, sizeof(counters) );
        server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
        const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
        memcpy( server.packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
        memcpy( server.backend_public_key, rocketnet_test_backend_public_key, 32 );
        memcpy( server.server_public_key, rocketnet_test_server_public_key, 32 );
        memcpy( server.server_secret_key, rocketnet_test_server_secret_key, 32 );
        check( rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" ) );
        memcpy( server.cookie_key, rocketnet_test_cookie_key, 32 );
        server.cookie_lifetime_seconds = 3;
        server.max_token_lifetime_seconds = 0;
        server.handshake_deadline_seconds = 5.0;
        server.max_timeout_seconds = 0;
        server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
        server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
        server.client_slots = rocketnet_client_slots_create( NULL, 4 );
        server.counters = &counters;
        check( server.encryption_map && server.used_token_history && server.client_slots );
        rocketnet_server_protocol_update( &server, 1000.0 );

        struct rocketnet_address_t client_address;
        check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );

        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        token_timeout_fields( &signed_fields, &private_fields, token_timeout_refused_values[i] );
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                                   rocketnet_test_client_eph_public,
                                                   token.signed_connect_data, token.signature );
        const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
        uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
        check( rocketnet_generate_cookie_mac( cookie_mac, &client_address, rocketnet_test_salt, cookie_expire, server.cookie_key ) );
        uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
        rocketnet_write_uint64( &cookie, cookie_expire );
        memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        check( rocketnet_server_protocol_process_connection_request( &server, &client_address,
                                                                     request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                     ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                     response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
        check( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID] == 1 );
        check( rocketnet_encryption_map_find( server.encryption_map, &client_address ) == -1 );

        rocketnet_encryption_map_destroy( server.encryption_map );
        rocketnet_used_token_history_destroy( server.used_token_history );
        rocketnet_client_slots_destroy( server.client_slots );
    }
}

REGISTER_UNIT_TEST( test_client_refuses_token_timeout_zero_or_negative );
REGISTER_UNIT_TEST( test_server_refuses_token_timeout_zero_or_negative );
