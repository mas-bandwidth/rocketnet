/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_gaps_secrets_tokens.cpp -- the token writers, the sealed packet
// argument checks, the client's session seals and the connection reset's
// key hooks:
//
//   - a seal that fails is a failed write: a server public key of small order
//     (all zero) makes crypto_box refuse, and both writers report it.
//   - a production token's private block opens with the server's secret key,
//     carries all-zero user data when none was given, and two fresh session
//     keys that differ per token and from each other.
//   - write_sealed_packet and open_sealed_packet refuse a client ephemeral key
//     given to a type whose associated data has none, and a missing one for a
//     type whose associated data carries it.
//   - the client's keep-alive carries its client index and max clients as its
//     8 type bytes, and the payload seals refuse a client that is not
//     connected.
//   - the connection reset keeps the key hooks and their context, so every
//     reset retires the I/O open copy.
//   - the cookie key rotation's edges: a 1-second period rotates, and a
//     rotation after more than a whole idle period wipes and forgets the
//     previous key.
//
// No socket, no clock: time is an argument, and the keys are drawn or fixed.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"
#include "rocketnet_counters.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// a check whose failure names the fault before the condition is printed
#define gaps_tokens_check( condition, fault )                                                                     \
do                                                                                                                \
{                                                                                                                 \
    if ( !( condition ) )                                                                                         \
    {                                                                                                             \
        printf( "mutation gap fault: %s\n", fault );                                                              \
        fflush( stdout );                                                                                         \
    }                                                                                                             \
    rocketnet_unit_check( condition );                                                                            \
} while ( 0 )

static bool gaps_tokens_all_zero( const uint8_t * bytes, size_t count )
{
    for ( size_t i = 0; i < count; i++ )
    {
        if ( bytes[i] != 0 )
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// the token writers
// ---------------------------------------------------------------------------

static enum rocketnet_status_t gaps_tokens_write( const uint8_t * backend_secret, const uint8_t * server_public,
                                                  const uint8_t * user_data, uint8_t * token )
{
    const char * addresses[] = { "127.0.0.1:40000" };
    return rocketnet_write_production_connect_token( backend_secret, server_public, 0x6A9500CE7ULL, addresses, 1,
                                                     0x5EC12E7ULL, 1000, 1ULL << 40, 300, user_data,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES );
}

// mutation gap: hunt 2/93
static void test_gaps_secrets_production_token_seal_failure_reported()
{
    uint8_t backend_public[ROCKETNET_KEY_BYTES];
    uint8_t backend_secret[64];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    // a small-order server public key: the seal's key agreement is all zero,
    // which crypto_box refuses

    uint8_t small_order[ROCKETNET_KEY_BYTES];
    memset( small_order, 0, sizeof( small_order ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const enum rocketnet_status_t status = gaps_tokens_write( backend_secret, small_order, NULL, token );
    sodium_memzero( backend_secret, sizeof( backend_secret ) );

    gaps_tokens_check( status == ROCKETNET_STATUS_CRYPTO_INIT_FAILED,
                       "a production token whose seal failed (small-order server key) was reported written" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_production_token_seal_failure_reported );

// mutation gap: hunt 2/105 hunt 2/106 hunt 2/107 hunt 2/219 hunt 2/222 hunt 2/223
static void test_gaps_secrets_production_token_private_block()
{
    uint8_t backend_public[ROCKETNET_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_connect_token_private_t opened[2];
    struct rocketnet_connect_token_t tokens[2];
    enum rocketnet_status_t status[2];
    int opens[2];

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        status[i] = gaps_tokens_write( backend_secret, server_public, NULL, token_data );
        rocketnet_read_connect_token( &tokens[i], token_data );
        memset( &opened[i], 0, sizeof( opened[i] ) );
        opens[i] = status[i] == ROCKETNET_STATUS_OK ? rocketnet_open_connect_token_private( &opened[i], tokens[i].signed_connect_data, server_public, server_secret ) : 0;
        sodium_memzero( token_data, sizeof( token_data ) );
    }

    sodium_memzero( backend_secret, sizeof( backend_secret ) );
    sodium_memzero( server_secret, sizeof( server_secret ) );

    gaps_tokens_check( status[0] == ROCKETNET_STATUS_OK && status[1] == ROCKETNET_STATUS_OK,
                       "a production token sealed to a valid server key was not written" );
    gaps_tokens_check( opens[0] == 1 && opens[1] == 1,
                       "a production token's private block did not open with the server's secret key" );

    bool user_data_zero = true;
    bool keys_nonzero = true;
    bool keys_distinct = true;
    bool keys_match_clear = true;
    for ( int i = 0; i < 2; i++ )
    {
        user_data_zero = user_data_zero && gaps_tokens_all_zero( opened[i].user_data, sizeof( opened[i].user_data ) );
        keys_nonzero = keys_nonzero && !gaps_tokens_all_zero( opened[i].client_to_server_key, ROCKETNET_KEY_BYTES ) &&
                       !gaps_tokens_all_zero( opened[i].server_to_client_key, ROCKETNET_KEY_BYTES );
        keys_distinct = keys_distinct && memcmp( opened[i].client_to_server_key, opened[i].server_to_client_key, ROCKETNET_KEY_BYTES ) != 0;
        keys_match_clear = keys_match_clear && memcmp( opened[i].client_to_server_key, tokens[i].client_to_server_key, ROCKETNET_KEY_BYTES ) == 0 &&
                           memcmp( opened[i].server_to_client_key, tokens[i].server_to_client_key, ROCKETNET_KEY_BYTES ) == 0;
    }
    const bool c2s_per_token = memcmp( opened[0].client_to_server_key, opened[1].client_to_server_key, ROCKETNET_KEY_BYTES ) != 0;
    const bool s2c_per_token = memcmp( opened[0].server_to_client_key, opened[1].server_to_client_key, ROCKETNET_KEY_BYTES ) != 0;

    sodium_memzero( opened, sizeof( opened ) );
    sodium_memzero( tokens, sizeof( tokens ) );

    gaps_tokens_check( user_data_zero, "a production token minted with no user data carries nonzero user data" );
    gaps_tokens_check( keys_nonzero, "a production token carries an all-zero session key" );
    gaps_tokens_check( keys_distinct, "a production token's client-to-server and server-to-client keys are the same" );
    gaps_tokens_check( c2s_per_token, "two production tokens carry the same client-to-server key" );
    gaps_tokens_check( s2c_per_token, "two production tokens carry the same server-to-client key" );
    gaps_tokens_check( keys_match_clear, "a production token's sealed session keys differ from the keys it carries for the client" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_production_token_private_block );

#if ROCKETNET_DEV

// mutation gap: hunt 2/69
static void test_gaps_secrets_deterministic_token_seal_failure_reported()
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
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );

    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 0x5EC12E7ULL;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    uint8_t ephemeral_secret[ROCKETNET_KEY_BYTES];
    memset( ephemeral_secret, 0x29, sizeof( ephemeral_secret ) );

    // the vector's server key seals; an all-zero (small-order) one does not

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    const int sealed = rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, ephemeral_secret, token );

    memset( signed_fields.server_public_key, 0, ROCKETNET_KEY_BYTES );
    const int refused = rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, ephemeral_secret, token );

    gaps_tokens_check( sealed == 1, "the deterministic writer did not write a token sealed to the vector's server key" );
    gaps_tokens_check( refused == 0, "the deterministic writer reported a token written when its seal failed (small-order server key)" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_deterministic_token_seal_failure_reported );

#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// the sealed packet's ephemeral-key argument
// ---------------------------------------------------------------------------

// mutation gap: hunt 2/3450 hunt 2/3500
static void test_gaps_secrets_sealed_packet_ephemeral_argument()
{
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    memset( salt, 0x17, sizeof( salt ) );
    uint8_t key[ROCKETNET_KEY_BYTES];
    memset( key, 0x6B, sizeof( key ) );
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];
    memset( ephemeral, 0x2E, sizeof( ephemeral ) );
    uint8_t nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memset( nonce, 0x4C, sizeof( nonce ) );
    uint8_t data[16];
    memset( data, 0x33, sizeof( data ) );
    const uint64_t protocol_id = 0x6A9500CE7ULL;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    // the response's associated data carries the ephemeral key; the
    // keep-alive's does not

    const int response_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_RESPONSE_PACKET, filter, 7, NULL,
                                                              data, (int) sizeof( data ), protocol_id, salt, ephemeral, key );
    gaps_tokens_check( response_bytes > 0, "a response sealed with its ephemeral key was not written" );

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    gaps_tokens_check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, response_bytes,
                                                     ROCKETNET_CONNECTION_RESPONSE_PACKET, 7, protocol_id, salt, ephemeral, key ) == (int) sizeof( data ),
                       "a response sealed with its ephemeral key did not open with it" );
    gaps_tokens_check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, response_bytes,
                                                     ROCKETNET_CONNECTION_RESPONSE_PACKET, 7, protocol_id, salt, NULL, key ) == -1,
                       "a response opened without the ephemeral key its associated data carries was not refused with -1" );

    const int keep_alive_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, filter, 8, NULL,
                                                                data, 8, protocol_id, salt, NULL, key );
    gaps_tokens_check( keep_alive_bytes > 0, "a keep-alive sealed without an ephemeral key was not written" );
    gaps_tokens_check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, keep_alive_bytes,
                                                     ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, protocol_id, salt, ephemeral, key ) == -1,
                       "a keep-alive opened with an ephemeral key its associated data does not carry was not refused with -1" );

    // the writer refuses the same mismatches, both ways

    gaps_tokens_check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET, filter, 9, nonce,
                                                      data, (int) sizeof( data ), protocol_id, salt, NULL, key ) == 0,
                       "a challenge sealed without the ephemeral key its associated data carries was reported written" );
    gaps_tokens_check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, filter, 10, NULL,
                                                      data, 8, protocol_id, salt, ephemeral, key ) == 0,
                       "a keep-alive sealed with an ephemeral key its associated data does not carry was reported written" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_sealed_packet_ephemeral_argument );

// ---------------------------------------------------------------------------
// the client's session seals
// ---------------------------------------------------------------------------

static struct rocketnet_client_protocol_t gaps_tokens_client;

// mutation gap: hunt 2/2085
static void test_gaps_secrets_keep_alive_carries_index_and_max()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t * client = &gaps_tokens_client;
    memset( client, 0, sizeof( *client ) );
    rocketnet_client_protocol_init( client, &counters );

    // a connected client: its session key, salt and protocol id, the slot the
    // server gave it, and the next session sequence

    client->state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    client->client_index = 3;
    client->max_clients = 8;
    client->signed_fields.protocol_id = 0x6A9500CE7ULL;
    memset( client->connection_salt, 0x17, sizeof( client->connection_salt ) );
    memset( client->keys.session_client_to_server, 0x6B, sizeof( client->keys.session_client_to_server ) );
    client->send_sequence_session = 1;
    client->last_send_time = 0.0;
    client->last_receive_time = 1000.0;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( client, 1000.0, packet, &packet_bytes );

    gaps_tokens_check( packet_bytes > 0 && packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                       "a connected client's update did not send a keep-alive" );

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, packet_bytes,
                                                           ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1, client->signed_fields.protocol_id,
                                                           client->connection_salt, NULL, client->keys.session_client_to_server );
    gaps_tokens_check( opened_bytes >= 8, "the client's keep-alive did not open with its session key" );

    const uint8_t * p = opened + opened_bytes - 8;
    const uint32_t client_index = rocketnet_read_uint32( &p );
    const uint32_t max_clients = rocketnet_read_uint32( &p );

    sodium_memzero( client, sizeof( *client ) );

    gaps_tokens_check( client_index == 3 && max_clients == 8,
                       "the client's keep-alive does not carry its client index and max clients as its type bytes" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_keep_alive_carries_index_and_max );

// mutation gap: hunt 2/2470 hunt 2/2492
static void test_gaps_secrets_payload_seal_refused_when_not_connected()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t * client = &gaps_tokens_client;
    memset( client, 0, sizeof( *client ) );
    rocketnet_client_protocol_init( client, &counters );
    client->send_sequence_session = 1;

    // disconnected: the keys and salt are the reset's zeros

    static uint8_t wire[4 * ROCKETNET_RECEIVE_CEILING];
    memset( wire, 0x21, sizeof( wire ) );
    gaps_tokens_check( rocketnet_client_protocol_seal_payload_frame( client, 1, 1, wire, 16, 1000.0 ) == 0,
                       "a client not connected sealed a payload frame (under its zeroed keys)" );

#if ROCKETNET_DEV
    uint8_t payload[16];
    memset( payload, 0x21, sizeof( payload ) );
    gaps_tokens_check( rocketnet_client_protocol_write_payload( client, payload, (int) sizeof( payload ), 1000.0, wire ) == 0,
                       "a client not connected wrote a sealed payload (under its zeroed keys)" );
#endif // #if ROCKETNET_DEV
}

REGISTER_UNIT_TEST( test_gaps_secrets_payload_seal_refused_when_not_connected );

// ---------------------------------------------------------------------------
// the connection reset keeps the key hooks
// ---------------------------------------------------------------------------

static int gaps_tokens_retired = 0;
static int gaps_tokens_retired_with_context = 0;
static int gaps_tokens_context_sentinel = 0;

static void gaps_tokens_retire_keys( void * context )
{
    gaps_tokens_retired++;
    gaps_tokens_retired_with_context += context == &gaps_tokens_context_sentinel;
}

static void gaps_tokens_publish_keys( void *, const struct rocketnet_client_protocol_t * )
{
}

// mutation gap: hunt 2/4154 hunt 2/4155
static void test_gaps_secrets_reset_keeps_key_hooks()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t * client = &gaps_tokens_client;
    memset( client, 0, sizeof( *client ) );
    rocketnet_client_protocol_init( client, &counters );

    client->publish_keys = gaps_tokens_publish_keys;
    client->retire_keys = gaps_tokens_retire_keys;
    client->keys_context = &gaps_tokens_context_sentinel;

    gaps_tokens_retired = 0;
    gaps_tokens_retired_with_context = 0;

    // two attempts end: each reset retires the I/O open copy through the hook

    client->state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED;
    rocketnet_client_connection_reset( client );
    client->state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT;
    rocketnet_client_connection_reset( client );

    gaps_tokens_check( gaps_tokens_retired == 2, "a second connection reset did not retire the keys (the reset dropped the retire hook)" );
    gaps_tokens_check( gaps_tokens_retired_with_context == 2, "a connection reset retired the keys with a context other than the transport's (the reset dropped keys_context)" );
    gaps_tokens_check( client->retire_keys == gaps_tokens_retire_keys, "the connection reset did not keep retire_keys" );
    gaps_tokens_check( client->publish_keys == gaps_tokens_publish_keys, "the connection reset did not keep publish_keys" );
    gaps_tokens_check( client->keys_context == &gaps_tokens_context_sentinel, "the connection reset did not keep keys_context" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_reset_keeps_key_hooks );

// ---------------------------------------------------------------------------
// the cookie key rotation's edges
// ---------------------------------------------------------------------------

static struct rocketnet_server_protocol_t gaps_tokens_server;

static void gaps_tokens_rotation_setup( int period_seconds )
{
    memset( &gaps_tokens_server, 0, sizeof( gaps_tokens_server ) );
    memset( gaps_tokens_server.cookie_key, 0x7C, ROCKETNET_COOKIE_KEY_BYTES );
    gaps_tokens_server.cookie_rotation_seconds = period_seconds;
    gaps_tokens_server.cookie_key_rotation_time = 100.0 + (double) period_seconds;
}

// mutation gap: hunt 2/1696
static void test_gaps_secrets_cookie_rotation_one_second_period()
{
    gaps_tokens_rotation_setup( 1 );

    uint8_t first[ROCKETNET_COOKIE_KEY_BYTES];
    memcpy( first, gaps_tokens_server.cookie_key, sizeof( first ) );

    rocketnet_server_protocol_rotate_cookie_key_if_due( &gaps_tokens_server, 101.0 );

    gaps_tokens_check( memcmp( gaps_tokens_server.cookie_key, first, sizeof( first ) ) != 0 && gaps_tokens_server.has_previous_cookie_key == 1,
                       "a 1-second cookie rotation period never rotated the cookie key" );

    sodium_memzero( &gaps_tokens_server, sizeof( gaps_tokens_server ) );
}

REGISTER_UNIT_TEST( test_gaps_secrets_cookie_rotation_one_second_period );

// mutation gap: hunt 2/1710 hunt 2/1713
static void test_gaps_secrets_cookie_rotation_after_idle_forgets_previous()
{
    gaps_tokens_rotation_setup( 60 );

    rocketnet_server_protocol_rotate_cookie_key_if_due( &gaps_tokens_server, 160.0 );
    gaps_tokens_check( gaps_tokens_server.has_previous_cookie_key == 1 && !gaps_tokens_all_zero( gaps_tokens_server.previous_cookie_key, ROCKETNET_COOKIE_KEY_BYTES ),
                       "the first rotation did not keep the outgoing cookie key as the previous key" );

    // idle past the previous key's whole window: the stall branch

    rocketnet_server_protocol_rotate_cookie_key_if_due( &gaps_tokens_server, 340.0 );

    const bool wiped = gaps_tokens_all_zero( gaps_tokens_server.previous_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    const bool forgotten = gaps_tokens_server.has_previous_cookie_key == 0;

    sodium_memzero( &gaps_tokens_server, sizeof( gaps_tokens_server ) );

    gaps_tokens_check( wiped, "a rotation after a whole idle period left the expired previous cookie key (a server secret) in place" );
    gaps_tokens_check( forgotten, "a rotation after a whole idle period wiped the previous cookie key but still marks it live" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_cookie_rotation_after_idle_forgets_previous );
