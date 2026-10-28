/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// STANDARD.md _Reading packets_ runs the per-type wire-size check before the
// state gate and reserves the 2^36 AEAD integrity counter for seal-open
// failures. For a connection challenge (type 2, 66 wire bytes) and a
// connection denied (type 1, 34 wire bytes), a packet one byte short or one
// byte long in any client state counts that type's WRONG_SIZE refusal before
// the state check, and neither DID_NOT_OPEN nor integrity_failures moves. A
// correctly sized packet that fails to open still counts DID_NOT_OPEN and
// advances integrity_failures.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_protocol.h"
#include "rocketnet_test_vectors.h"

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define WSO_EXPECT( label, before, after, ... )                                                                   \
do                                                                                                                \
{                                                                                                                 \
    char wso_report[16384];                                                                                       \
    const bool wso_ok = rocketnet_counters_expect_core( (before), (after), __VA_ARGS__, wso_report, (int) sizeof(wso_report) ); \
    if ( !wso_ok )                                                                                                \
        printf( "case: %s\n%s", (label), wso_report );                                                            \
    check( wso_ok );                                                                                              \
} while(0)

static void wso_init_client( struct rocketnet_client_protocol_t * client,
                             struct rocketnet_counters_t * counters,
                             int state )
{
    static const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memset( counters, 0, sizeof( *counters ) );
    memset( client, 0, sizeof( *client ) );
    memcpy( client->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memset( client->token.client_to_server_key, 0x11, ROCKETNET_KEY_BYTES );
    memset( client->token.server_to_client_key, 0x22, ROCKETNET_KEY_BYTES );
    memset( client->keys.handshake_server_to_client, 0x33, ROCKETNET_KEY_BYTES );
    memset( client->connection_salt, 0x44, ROCKETNET_CONNECTION_SALT_BYTES );
    client->signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    client->send_rate = 10.0;
    client->counters = counters;
    client->state = state;
}

static void wso_write_junk_packet( uint8_t * packet, int packet_bytes, uint8_t packet_type )
{
    static const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memset( packet, 0x5A, (size_t) packet_bytes );

    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, packet_type, tag );
}

static void test_client_wrong_size_counts_wrong_size_not_integrity()
{
    static const int states[] = {
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE,
        ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST,
        ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED,
    };

    static const int deltas[2] = { -1, +1 };

    double time = 1000.0;
    uint8_t packet[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES + 16];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    // 1. One byte short and one byte long, in every client state: size check
    // runs before state check, counts WRONG_SIZE, and moves neither
    // DID_NOT_OPEN nor integrity_failures.

    for ( size_t s = 0; s < sizeof( states ) / sizeof( states[0] ); s++ )
    {
        const int state = states[s];

        for ( size_t d = 0; d < 2; d++ )
        {
            struct rocketnet_counters_t counters;
            struct rocketnet_client_protocol_t client;

            wso_init_client( &client, &counters, state );

            const int challenge_bytes = ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES + deltas[d];
            wso_write_junk_packet( packet, challenge_bytes, ROCKETNET_CONNECTION_CHALLENGE_PACKET );

            struct rocketnet_counters_t before = counters;
            time += 0.001;
            check( !rocketnet_client_protocol_process_packet( &client, packet, challenge_bytes, time,
                                                              payload, (int) sizeof( payload ), &payload_bytes ) );
            check( client.integrity_failures == 0 );
            check( client.state == state );
            WSO_EXPECT( "challenge wrong size", &before, &counters,
                        { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 },
                          { ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, 1 },
                          { ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_SIZE, 1 } } );

            wso_init_client( &client, &counters, state );

            const int denied_bytes = ROCKETNET_CONNECTION_DENIED_WIRE_BYTES + deltas[d];
            wso_write_junk_packet( packet, denied_bytes, ROCKETNET_CONNECTION_DENIED_PACKET );
            before = counters;
            time += 0.001;
            check( !rocketnet_client_protocol_process_packet( &client, packet, denied_bytes, time,
                                                              payload, (int) sizeof( payload ), &payload_bytes ) );
            check( client.integrity_failures == 0 );
            check( client.state == state );
            WSO_EXPECT( "denied wrong size", &before, &counters,
                        { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 },
                          { ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED, 1 },
                          { ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_SIZE, 1 } } );
        }
    }

    // 2. Correctly sized packets that fail to open still count DID_NOT_OPEN
    // and advance integrity_failures as before.
    {
        struct rocketnet_counters_t counters;
        struct rocketnet_client_protocol_t client;
        wso_init_client( &client, &counters, ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

        wso_write_junk_packet( packet, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES, ROCKETNET_CONNECTION_CHALLENGE_PACKET );

        struct rocketnet_counters_t before = counters;
        time += 0.001;
        check( !rocketnet_client_protocol_process_packet( &client, packet, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES, time,
                                                          payload, (int) sizeof( payload ), &payload_bytes ) );
        check( client.integrity_failures == 1 );
        WSO_EXPECT( "challenge did not open", &before, &counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 },
                      { ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, 1 },
                      { ROCKETNET_COUNTER_CLIENT_CHALLENGE_DID_NOT_OPEN, 1 } } );

        wso_write_junk_packet( packet, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES, ROCKETNET_CONNECTION_DENIED_PACKET );
        before = counters;
        time += 0.001;
        check( !rocketnet_client_protocol_process_packet( &client, packet, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES, time,
                                                          payload, (int) sizeof( payload ), &payload_bytes ) );
        check( client.integrity_failures == 2 );
        WSO_EXPECT( "denied did not open in sending_connection_request", &before, &counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 },
                      { ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED, 1 },
                      { ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN, 1 } } );

        client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE;
        before = counters;
        time += 0.001;
        check( !rocketnet_client_protocol_process_packet( &client, packet, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES, time,
                                                          payload, (int) sizeof( payload ), &payload_bytes ) );
        check( client.integrity_failures == 3 );
        WSO_EXPECT( "denied did not open in sending_connection_response", &before, &counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 },
                      { ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED, 1 },
                      { ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN, 1 } } );

        rocketnet_counters_assert_conserved( &counters );
    }
}

REGISTER_UNIT_TEST( test_client_wrong_size_counts_wrong_size_not_integrity );
