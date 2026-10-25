/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client's handshake deadline is the token's lifetime measured from the
// attempt start, as in netcode 1.02, not the token's expire timestamp on the
// token's own clock. The client refuses to begin at or after expire
// (sdk/rocketnet_client_protocol.cpp:148), gives the attempt the whole
// lifetime (sdk/rocketnet_client_protocol.cpp:187) and ends it once that
// lifetime has elapsed since the attempt began
// (sdk/rocketnet_client_protocol.cpp:205-206). netcode 1.02 does the same:
// netcode_client_update abandons with NETCODE_CLIENT_STATE_CONNECT_TOKEN_EXPIRED
// once client->time - client->connect_start_time >= expire_timestamp -
// create_timestamp, at tag v1.2.1, netcode.c:3254-3262.
//
// The clock is an argument: the test starts at 1000.0 and uses no socket, no
// network and no real clock.

#include "test_unit_registry.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#define HANDSHAKE_DEADLINE_CREATE    ROCKETNET_TEST_CREATE_TS
#define HANDSHAKE_DEADLINE_EXPIRE    ( HANDSHAKE_DEADLINE_CREATE + 5 )
#define HANDSHAKE_DEADLINE_TIMEOUT   60     // a valid timeout, longer than the token's lifetime

static void handshake_deadline_mint( uint8_t * token_data, uint64_t create, uint64_t expire, int32_t timeout )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = timeout;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = expire;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields,
                                                         rocketnet_test_backend_secret_key, token_data ) );
}

// Begin at C + 3 with a token of create C and expire C + 5: the token has two
// seconds left on its own clock, but the attempt owns the token's whole
// lifetime, five seconds, from the moment it begins. The per-state timeout is
// sixty seconds, longer than the lifetime, so only the total deadline can end
// it. An update just before five
// seconds is still requesting cookie (1), past the token's expire timestamp on
// the token's own clock; the update at five seconds is connect token expired
// (-7). These are sdk/rocketnet_client_protocol.cpp:148, :187 and :205-206,
// and netcode.c:3254-3262 at tag v1.2.1.
static void test_client_attempt_runs_the_token_lifetime_from_its_start()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    handshake_deadline_mint( token_data, HANDSHAKE_DEADLINE_CREATE, HANDSHAKE_DEADLINE_EXPIRE, HANDSHAKE_DEADLINE_TIMEOUT );

    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, token_data, 1000.0,
                                                             HANDSHAKE_DEADLINE_CREATE + 3 ) );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;

    rocketnet_client_protocol_update( &client, 1004.9, packet, &packet_bytes );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    rocketnet_client_protocol_update( &client, 1005.0, packet, &packet_bytes );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );

    rocketnet_counters_assert_conserved( &counters );
}

// A token whose expire timestamp is not later than the current time is refused
// before an attempt begins, with connect token expired (-7) and nothing on the
// wire: connect at C + 5 for a token that expires at C + 5. These are
// sdk/rocketnet_client_protocol.cpp:148, :187 and :205-206, and
// netcode.c:3254-3262 at tag v1.2.1.
static void test_client_connect_at_the_expire_timestamp_is_refused()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    handshake_deadline_mint( token_data, HANDSHAKE_DEADLINE_CREATE, HANDSHAKE_DEADLINE_EXPIRE, HANDSHAKE_DEADLINE_TIMEOUT );

    rocketnet_unit_check( !rocketnet_client_protocol_connect( &client, token_data, 1000.0,
                                                              HANDSHAKE_DEADLINE_EXPIRE ) );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = -1;
    rocketnet_client_protocol_update( &client, 1000.0, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );

    rocketnet_counters_assert_conserved( &counters );
}

REGISTER_UNIT_TEST( test_client_attempt_runs_the_token_lifetime_from_its_start );
REGISTER_UNIT_TEST( test_client_connect_at_the_expire_timestamp_is_refused );
