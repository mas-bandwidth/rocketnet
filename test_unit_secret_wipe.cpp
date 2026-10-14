/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_secret_wipe.cpp -- the key-zeroisation oracle. Every terminal path
// an attempt can take MUST leave the attempt's secret material zeroed:
// STANDARD.md's ephemeral-secret erasure (both sides zero their own ephemeral
// secret once it is no longer needed for a handshake in progress) and the
// denied path's "release all connection state", read at
// sdk/rocketnet_client_protocol.cpp.
//
// The oracle plants a 0xA5 canary in each secret buffer, drives one terminal
// path through the protocol functions (time is an argument; no socket, no
// clock, no driver), then scans the struct and the scratch for a surviving
// canary. A wiped field is all zero and cannot hold the canary; an unwiped
// field is exactly the canary, so the scan is deterministic -- it never
// depends on a random key happening to contain the byte.
//
// Two fixes have landed and are asserted green:
//   * the connect-token mint scratch (sdk/rocketnet_connect_token.cpp, the
//     private_block/private_data buffers zeroed on every return), and
//   * the client token keys, zeroed by the deliberate disconnect
//     (sdk/rocketnet_client_protocol.cpp, rocketnet_client_protocol_disconnect).
// The ephemeral secret on a *failed* attempt is still open (the model finding
// F4): the automatic terminal transitions -- denied, every handshake timeout,
// connect-token expired -- reach rocketnet_client_set_state without zeroing,
// so that path carries the expected-fail marker and names its fix card.

#include "test_unit_registry.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_keys.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

// the connect-token mint scratch: the mint copies its zeroed scratch here, so
// the test can see whether the wipe ran. Declared by sdk/rocketnet_connect_token.cpp.
extern uint8_t rocketnet_test_private_block_copy[ROCKETNET_PRIVATE_BLOCK_BYTES];
extern bool rocketnet_test_private_block_copied;

static const uint8_t k_canary = 0xA5;

static void plant_canary( uint8_t * bytes, int count )
{
    memset( bytes, k_canary, (size_t) count );
}

// 1 when the canary survives anywhere in the region -- i.e. the field was not
// zeroed after the canary was planted
static int canary_survives( const uint8_t * bytes, int count )
{
    for ( int i = 0; i < count; i++ )
        if ( bytes[i] == k_canary )
            return 1;
    return 0;
}

// a token minted with the frozen appendix vectors: the token keys are known
// constants, so a test can seal and open packets against the same derivation
// the client runs
static void mint_test_token( uint8_t * token_data, uint64_t client_id )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;

    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = client_id;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields,
                                                         rocketnet_test_backend_secret_key, token_data ) );
}

// a fresh client, connected just far enough to hold a live attempt: the token
// is parsed, the ephemeral keypair exists, and the state is requesting cookie.
// The mint helper above and this one both stay inside the function; the client
// and its counters are the caller's so a test can plant and scan them.
static void connect_live_attempt( struct rocketnet_client_protocol_t * client,
                                  struct rocketnet_counters_t * counters,
                                  double current_time )
{
    rocketnet_client_protocol_init( client, counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    mint_test_token( token_data, 0xC0DEULL );

    rocketnet_unit_check( rocketnet_client_protocol_connect( client, token_data, current_time, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_unit_check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
}

// Advance a requesting-cookie attempt to sending-connection-request by feeding
// it a cookie that echoes this attempt's salt, exactly as the server would.
static void feed_cookie( struct rocketnet_client_protocol_t * client, double current_time )
{
    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    const uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES] = { 0 };
    rocketnet_write_connection_cookie_packet( cookie, client->filter_value, client->connection_salt,
                                              0, cookie_mac );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( client, cookie, sizeof(cookie),
                                                                    current_time, payload, &payload_bytes ) );
    rocketnet_unit_check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
}

// ---------------------------------------------------------------------------
// green: the wipes that have landed
// ---------------------------------------------------------------------------

// The connect-token mint scratch: rocketnet_write_connect_token fills a local
// private_block from the input and MUST zero it (and the private_data copy)
// before every return, so no copy of the token keys outlives the call. Plant
// the canary in the input keys; if the wipe is missing the scratch copy is the
// canary, and this test goes red.
static void TestConnectTokenScratchIsWiped()
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;

    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    plant_canary( private_fields.client_to_server_key, ROCKETNET_KEY_BYTES );
    plant_canary( private_fields.server_to_client_key, ROCKETNET_KEY_BYTES );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_private_block_copied = false;
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields,
                                                         rocketnet_test_backend_secret_key, token_data ) );
    rocketnet_unit_check( rocketnet_test_private_block_copied );

    // the scratch returned to its zeroed state: no canary survives
    rocketnet_unit_check( !canary_survives( rocketnet_test_private_block_copy, ROCKETNET_PRIVATE_BLOCK_BYTES ) );
}

// The client's parsed token keys and its ephemeral secret: the deliberate
// disconnect zeroes the whole token (which holds both per-session keys), the
// four derived keys and the ephemeral secret. Plant the canary after connect
// and disconnect: every planted byte must be gone.
static void TestClientTokenKeysAreWipedOnDisconnect()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );

    plant_canary( &client.token.client_to_server_key[0], ROCKETNET_KEY_BYTES );
    plant_canary( &client.token.server_to_client_key[0], ROCKETNET_KEY_BYTES );
    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_client_protocol_disconnect( &client, burst, &burst_count, &burst_bytes );

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    rocketnet_unit_check( !canary_survives( client.token.client_to_server_key, ROCKETNET_KEY_BYTES ) );
    rocketnet_unit_check( !canary_survives( client.token.server_to_client_key, ROCKETNET_KEY_BYTES ) );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
}

// ---------------------------------------------------------------------------
// expected-fail: the automatic terminal paths that still leak the ephemeral
// secret (model finding F4, fix card rnfix-client-terminal-states-wipe-their-secrets)
// ---------------------------------------------------------------------------

// The cookie-request timeout (-2): an attempt that never hears back from the
// server dies after the token's timeout. The ephemeral secret that sealed the
// requests MUST be zeroed when the attempt ends this way.
static void TestClientEphemeralSecretWipedOnCookieTimeout()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, 100.0 + ROCKETNET_TEST_TIMEOUT + 1.0, packet, &packet_bytes );

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
}

// The connect-token-expired terminal (-7): the token's lifetime elapses during
// the handshake and the whole attempt dies. The ephemeral secret MUST be gone.
static void TestClientEphemeralSecretWipedOnTokenExpired()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    const double deadline = (double) ( ROCKETNET_TEST_EXPIRE_TS - ROCKETNET_TEST_CREATE_TS );
    rocketnet_client_protocol_update( &client, 100.0 + deadline, packet, &packet_bytes );

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
}

// The denied terminal (-1): a sealed denied that opens with the handshake keys
// derived from this attempt's token keys and salt. The ephemeral secret MUST be
// gone once the attempt ends in the terminal denied state.
static void TestClientEphemeralSecretWipedOnDenied()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );
    feed_cookie( &client, 100.5 );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    // the server seals the denied under the handshake keys derived from the
    // token keys, this attempt's salt and a carried nonce
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memset( session_nonce, 0x5A, sizeof(session_nonce) );

    struct rocketnet_connection_keys_t keys;
    rocketnet_derive_handshake_keys( &keys, client.token.client_to_server_key, client.token.server_to_client_key,
                                     client.connection_salt, session_nonce );

    uint8_t denied[ROCKETNET_RECEIVE_CEILING];
    int denied_bytes = rocketnet_write_sealed_packet( denied, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                      client.filter_value, 1, session_nonce, NULL, 0,
                                                      client.signed_fields.protocol_id, client.connection_salt,
                                                      NULL, keys.handshake_server_to_client );
    rocketnet_unit_check( denied_bytes == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, denied, denied_bytes,
                                                                    100.6, payload, &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
}

REGISTER_UNIT_TEST( TestConnectTokenScratchIsWiped );
REGISTER_UNIT_TEST( TestClientTokenKeysAreWipedOnDisconnect );
REGISTER_UNIT_TEST_EXPECTED_FAIL( TestClientEphemeralSecretWipedOnCookieTimeout, "rnfix-client-terminal-states-wipe-their-secrets" );
REGISTER_UNIT_TEST_EXPECTED_FAIL( TestClientEphemeralSecretWipedOnTokenExpired, "rnfix-client-terminal-states-wipe-their-secrets" );
REGISTER_UNIT_TEST_EXPECTED_FAIL( TestClientEphemeralSecretWipedOnDenied, "rnfix-client-terminal-states-wipe-their-secrets" );
