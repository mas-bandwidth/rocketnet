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
// clock, no transport), then scans the struct and the scratch for a surviving
// canary. A wiped field is all zero and cannot hold the canary; an unwiped
// field is exactly the canary, so the scan is deterministic -- it never
// depends on a random key happening to contain the byte.
//
// The client token keys and ephemeral secret are zeroed by the deliberate
// disconnect (sdk/rocketnet_client_protocol.cpp,
// rocketnet_client_protocol_disconnect) and by every automatic terminal
// transition (rnfix-client-terminal-states-wipe-their-secrets).

#include "test_unit_registry.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_keys.h"
#include "rocketnet_test_vectors.h"

#include "wycheproof.h"

// Match the header to the sodium implementation linked into the SDK.
#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

#include <stdint.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

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

    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( &private_fields, 0, sizeof( private_fields ) );
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
    rocketnet_write_connection_cookie_packet( cookie, client->packet_filter, client->connection_salt,
                                              0, cookie_mac );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( client, cookie, sizeof( cookie ),
                                                                    current_time, payload, sizeof( payload ), &payload_bytes ) );
    rocketnet_unit_check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
}

// ---------------------------------------------------------------------------
// green: the wipes that have landed
// ---------------------------------------------------------------------------

// The client's parsed token keys and its ephemeral secret: the deliberate
// disconnect zeroes the whole token (which holds both per-session keys), the
// four derived keys and the ephemeral secret. Plant the canary after connect
// and disconnect: every planted byte must be gone.
static void test_client_token_keys_are_wiped_on_disconnect()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

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
    rocketnet_counters_assert_conserved( &counters );
}

// ---------------------------------------------------------------------------
// expected-fail: the automatic terminal paths that still leak the ephemeral
// secret (model finding F4, fix card rnfix-client-terminal-states-wipe-their-secrets)
// ---------------------------------------------------------------------------

// The cookie-request timeout (-2): an attempt that never hears back from the
// server dies after the token's timeout. The ephemeral secret that sealed the
// requests MUST be zeroed when the attempt ends this way.
static void test_client_ephemeral_secret_wiped_on_cookie_timeout()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, 100.0 + ROCKETNET_TEST_TIMEOUT + 1.0, packet, &packet_bytes );

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
    rocketnet_counters_assert_conserved( &counters );
}

// The connect-token-expired terminal (-7): the token's lifetime elapses during
// the handshake and the whole attempt dies. The ephemeral secret MUST be gone.
static void test_client_ephemeral_secret_wiped_on_token_expired()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    const double deadline = (double) ( ROCKETNET_TEST_EXPIRE_TS - ROCKETNET_TEST_CREATE_TS );
    rocketnet_client_protocol_update( &client, 100.0 + deadline, packet, &packet_bytes );

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
    rocketnet_counters_assert_conserved( &counters );
}

// The denied terminal (-1): a sealed denied that opens with the handshake keys
// derived from this attempt's token keys and salt. The ephemeral secret MUST be
// gone once the attempt ends in the terminal denied state.
static void test_client_ephemeral_secret_wiped_on_denied()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );
    feed_cookie( &client, 100.5 );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    // the server seals the denied under the handshake keys derived from the
    // token keys, this attempt's salt and a carried nonce
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memset( session_nonce, 0x5A, sizeof( session_nonce ) );

    struct rocketnet_connection_keys_t keys;
    rocketnet_derive_handshake_keys( &keys, client.token.client_to_server_key, client.token.server_to_client_key,
                                     client.connection_salt, session_nonce );

    uint8_t denied[ROCKETNET_RECEIVE_CEILING];
    int denied_bytes = rocketnet_write_sealed_packet( denied, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                      client.packet_filter, 1, session_nonce, NULL, 0,
                                                      client.signed_fields.protocol_id, client.connection_salt,
                                                      NULL, keys.handshake_server_to_client );
    rocketnet_unit_check( denied_bytes == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, denied, denied_bytes,
                                                                    100.6, payload, sizeof( payload ), &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
    rocketnet_counters_assert_conserved( &counters );
}

// The connection-request timeout (-3): reach sending-connection-request via
// cookie, plant canary in the still-live ephemeral, let the per-state timeout
// fire. This path still leaks (F4) so expected-fail names the fix card.
static void test_client_ephemeral_secret_wiped_on_connection_request_timeout()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );
    feed_cookie( &client, 100.5 );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, 100.0 + ROCKETNET_TEST_TIMEOUT + 1.0, packet, &packet_bytes );

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
    rocketnet_counters_assert_conserved( &counters );
}

// The connection-response timeout (-4): feed a valid challenge from state 2;
// the challenge path zeroes the ephemeral (sdk/rocketnet_client_protocol.cpp:509)
// and enters state 3. Timeout from there leaves it zero (assert green).
static void test_client_ephemeral_secret_wiped_on_connection_response_timeout()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t client;
    connect_live_attempt( &client, &counters, 100.0 );
    feed_cookie( &client, 100.5 );

    // craft a valid challenge the client will accept (mirrors server construction)
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memset( session_nonce, 0x5A, sizeof( session_nonce ) );

    uint8_t server_eph_pub[ROCKETNET_KEY_BYTES];
    memcpy( server_eph_pub, rocketnet_test_server_eph_public, ROCKETNET_KEY_BYTES );

    struct rocketnet_connection_keys_t keys;
    rocketnet_derive_handshake_keys( &keys, client.token.client_to_server_key, client.token.server_to_client_key,
                                     client.connection_salt, session_nonce );

    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int ch_bytes = rocketnet_write_sealed_packet( challenge, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
                                                  client.packet_filter, 1, session_nonce,
                                                  server_eph_pub, ROCKETNET_KEY_BYTES,
                                                  client.signed_fields.protocol_id, client.connection_salt,
                                                  client.client_ephemeral_public_key,
                                                  keys.handshake_server_to_client );
    rocketnet_unit_check( ch_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    memset( &keys, 0, sizeof( keys ) );

    plant_canary( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, challenge, ch_bytes,
                                                                    100.6, payload, sizeof( payload ), &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    // zero happened on entry to state 3
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );

    // now drive timeout from state 3
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, 100.0 + ROCKETNET_TEST_TIMEOUT + 1.0, packet, &packet_bytes );

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT );
    rocketnet_unit_check( !canary_survives( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
    rocketnet_counters_assert_conserved( &counters );
}

// Server verify and decrypt failure paths (signature fail before open, sealed
// private-block decrypt fail inside open_connect_token_private): no per-attempt
// secret buffers are allocated on those early returns (server's long-term
// keys stay live across requests; server eph pair and derived keys are created
// only after successful token open). These paths are therefore not-applicable
// for a canary scan in this oracle.

// ---------------------------------------------------------------------------
// the vendored sodium subset, measured against every Wycheproof X25519 vector
// (rocketnet-wycheproof-x25519)
// ---------------------------------------------------------------------------

#if !defined( ROCKETNET_USE_SYSTEM_SODIUM )

// The tests below drive the vendored subset the SDK links (sodium/sodium.c),
// never a system libsodium, so a green run is a statement about the code
// rocketnet ships. Under ROCKETNET_USE_SYSTEM_SODIUM they are compiled out.

struct wp_x25519_tally_t
{
    int accepted;
    int refused_low_order;
};

static bool wp_x25519_all_zero( const uint8_t * bytes )
{
    for ( int i = 0; i < 32; i++ )
        if ( bytes[i] != 0 )
            return false;
    return true;
}

static bool wp_x25519_field( const wycheproof_test_t & test, const char * name, uint8_t out[32] )
{
    std::map<std::string, std::string>::const_iterator it = test.fields.find( name );
    if ( it == test.fields.end() )
        return false;
    std::vector<uint8_t> bytes;
    if ( !wycheproof_hex( it->second, bytes ) )
        return false;
    if ( bytes.size() != 32 )
        return false;
    memcpy( out, &bytes[0], 32 );
    return true;
}

static void wp_x25519_print_disagreement( const wycheproof_test_t & test, const char * outcome )
{
    printf( "wycheproof %s tcId %d %s %s", "x25519_test.json", test.tc_id, test.result.c_str(), outcome );
    for ( size_t i = 0; i < test.flags.size(); i++ )
        printf( " %s", test.flags[i].c_str() );
    printf( "\n" );
    fflush( stdout );
}

// The rule: every x25519_test.json vector is judged by the same
// crypto_scalarmult call sdk/rocketnet_keys.cpp makes in
// rocketnet_derive_connection_keys -- a non-zero result refuses the vector,
// and the 32 shared bytes are all zero exactly on the 31 low-order vectors.
// The runner returns the disagreements and fills the outcome tallies. When
// skip_zero_check is set it models a scalarmult with the all-zero refusal
// removed, recording those vectors accepted whatever the call returned.
static int wp_x25519_run( bool skip_zero_check, struct wp_x25519_tally_t * tally,
                          bool * every_disagreement_zero_shared )
{
    std::vector<wycheproof_test_t> tests;
    int number_of_tests = 0;
    int number_of_groups = 0;
    rocketnet_unit_check( wycheproof_load( "x25519_test.json", tests, &number_of_tests, &number_of_groups ) );
    rocketnet_unit_check( number_of_tests == 518 );
    rocketnet_unit_check( number_of_groups == 1 );
    rocketnet_unit_check( tests.size() == 518 );

    int disagreements = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        const wycheproof_test_t & test = tests[i];
        uint8_t private_key[32];
        uint8_t public_key[32];
        uint8_t shared[32];
        rocketnet_unit_check( wp_x25519_field( test, "private", private_key ) );
        rocketnet_unit_check( wp_x25519_field( test, "public", public_key ) );
        rocketnet_unit_check( wp_x25519_field( test, "shared", shared ) );

        uint8_t out[32];
        memset( out, 0, sizeof( out ) );
        const int result = crypto_scalarmult( out, private_key, public_key );
        const bool all_zero = wp_x25519_all_zero( shared );

        if ( all_zero )
        {
            if ( skip_zero_check )
            {
                tally->accepted++;
                disagreements++;
                wp_x25519_print_disagreement( test, "accepted" );
            }
            else if ( result != 0 )
            {
                tally->refused_low_order++;
            }
            else
            {
                tally->accepted++;
                disagreements++;
                wp_x25519_print_disagreement( test, "accepted" );
            }
        }
        else if ( result != 0 )
        {
            tally->refused_low_order++;
            disagreements++;
            *every_disagreement_zero_shared = false;
            wp_x25519_print_disagreement( test, "refused_low_order" );
        }
        else if ( memcmp( out, shared, sizeof( shared ) ) != 0 )
        {
            tally->accepted++;
            disagreements++;
            *every_disagreement_zero_shared = false;
            wp_x25519_print_disagreement( test, "accepted" );
        }
        else
        {
            tally->accepted++;
        }
    }
    return disagreements;
}

// The rule: the 31 low-order vectors in x25519_test.json must be refused, not
// accepted, and every other vector must yield exactly its shared value. The
// check under test is the crypto_scalarmult zero-shared refusal reached from
// rocketnet_derive_connection_keys in sdk/rocketnet_keys.cpp. The sdk wrapper
// is driven too: it returns 0 on those 31 and 1 on the other 487, and each
// success feeds the shared secret into session_client_to_server, so those 487
// session keys are pairwise distinct without any key being printed.
static void test_wycheproof_x25519_vectors()
{
    struct wp_x25519_tally_t tally;
    tally.accepted = 0;
    tally.refused_low_order = 0;
    bool every_disagreement_zero_shared = true;
    rocketnet_unit_check( wp_x25519_run( false, &tally, &every_disagreement_zero_shared ) == 0 );
    rocketnet_unit_check( every_disagreement_zero_shared );
    rocketnet_unit_check( tally.accepted == 487 );
    rocketnet_unit_check( tally.refused_low_order == 31 );

    std::vector<wycheproof_test_t> tests;
    int number_of_tests = 0;
    int number_of_groups = 0;
    rocketnet_unit_check( wycheproof_load( "x25519_test.json", tests, &number_of_tests, &number_of_groups ) );
    rocketnet_unit_check( tests.size() == 518 );

    std::set<std::string> session_client_to_server_keys;
    int sdk_accepted = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        const wycheproof_test_t & test = tests[i];
        uint8_t private_key[32];
        uint8_t public_key[32];
        uint8_t shared[32];
        rocketnet_unit_check( wp_x25519_field( test, "private", private_key ) );
        rocketnet_unit_check( wp_x25519_field( test, "public", public_key ) );
        rocketnet_unit_check( wp_x25519_field( test, "shared", shared ) );

        struct rocketnet_connection_keys_t keys;
        memset( &keys, 0, sizeof( keys ) );
        const int accepted = rocketnet_derive_connection_keys( &keys,
                                                               rocketnet_test_token_c2s_key,
                                                               rocketnet_test_token_s2c_key,
                                                               rocketnet_test_salt,
                                                               rocketnet_test_session_nonce,
                                                               private_key,
                                                               public_key,
                                                               rocketnet_test_client_eph_public,
                                                               rocketnet_test_server_eph_public );
        if ( wp_x25519_all_zero( shared ) )
        {
            rocketnet_unit_check( accepted == 0 );
        }
        else
        {
            rocketnet_unit_check( accepted == 1 );
            sdk_accepted++;
            session_client_to_server_keys.insert( std::string( (const char *) keys.session_client_to_server, 32 ) );
        }
    }
    rocketnet_unit_check( sdk_accepted == 487 );
    rocketnet_unit_check( session_client_to_server_keys.size() == 487 );
}

// The witness: with the zero-shared refusal removed the runner must go red on
// exactly the 31 low-order vectors, and no other vector may disagree. The
// removed check is the crypto_scalarmult all-zero refusal in
// sdk/rocketnet_keys.cpp, rocketnet_derive_connection_keys.
static void test_wycheproof_x25519_witness_skipped_zero_check_goes_red()
{
    struct wp_x25519_tally_t tally;
    tally.accepted = 0;
    tally.refused_low_order = 0;
    bool every_disagreement_zero_shared = true;
    rocketnet_unit_check( wp_x25519_run( true, &tally, &every_disagreement_zero_shared ) == 31 );
    rocketnet_unit_check( every_disagreement_zero_shared );
}

REGISTER_UNIT_TEST( test_wycheproof_x25519_vectors );
REGISTER_UNIT_TEST( test_wycheproof_x25519_witness_skipped_zero_check_goes_red );

#endif // !defined(ROCKETNET_USE_SYSTEM_SODIUM) -- rocketnet-wycheproof-x25519

REGISTER_UNIT_TEST( test_client_token_keys_are_wiped_on_disconnect );
REGISTER_UNIT_TEST( test_client_ephemeral_secret_wiped_on_connection_response_timeout );
REGISTER_UNIT_TEST( test_client_ephemeral_secret_wiped_on_cookie_timeout );
REGISTER_UNIT_TEST( test_client_ephemeral_secret_wiped_on_token_expired );
REGISTER_UNIT_TEST( test_client_ephemeral_secret_wiped_on_denied );
REGISTER_UNIT_TEST( test_client_ephemeral_secret_wiped_on_connection_request_timeout );
