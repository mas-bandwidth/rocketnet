/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The reversed handshake witnesses of proofs/tla/ClientConnection.tla and
// proofs/tla/ServerConnection.tla, turned into tests on the real protocol objects,
// driven with time passed in and no sockets, no network and no real clock.
// Each witness names the model action it reverses and the config switch that
// reverses it (README-ClientConnection.md, README-ServerConnection.md).
//
// The four witnesses:
//   F4  EphemeralErasedOutsideHandshake (ClientConnection): an attempt that
//       fails from a waiting state leaves the client's ephemeral secret and
//       the token keys zero. set_state of a state <= DISCONNECTED resets
//       them. The four failure modes are denial and the three handshake
//       timeouts.
//   F8  TerminalAbsorbing (ClientConnection): a terminal state leaves only
//       by connect (to 1) or by disconnect (to 0). disconnect() from a
//       terminal state returns the client to DISCONNECTED.
//   One client id, one live connection (ServerConnection F9, History
//       "none"): two handshakes, each with its own token for one client id;
//       the client-id check at the response refuses the second while the
//       first lives, and seats it once the first closes. The same-token case
//       is test_server_refuses_a_token_connected_now (test_unit_token_churn.cpp).
//   RepeatKeepAlive = FALSE and KeepAliveClock = "anysend"
//       (ClientConnection, ServerConnection): the establishing keep-alive is
//       repeated on its own clock, and ordinary traffic does not suppress it.
//       These two are the reversed witnesses; the code's behaviour is
//       RepeatKeepAlive = TRUE, KeepAliveClock = "keepalive".
//
// F4 and F8 assert the code's behaviour and are ordinary cases.
// RepeatKeepAlive and KeepAliveClock assert the code's own behaviour and
// pass today.

#include "test_unit_registry.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_crypto_tally.h"

#include <stdint.h>
#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// ---------------------------------------------------------------------------
// The shared fixture: a server with the two token tables, one slot table and
// a small map, and the client driven through a perfect wire with mocked time.
// ---------------------------------------------------------------------------

struct rocketnet_handshake_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_address_t second_address;
};

static void rocketnet_handshake_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                                    struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof( *private_fields ) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

static void rocketnet_handshake_mint_token( uint8_t * token_data, uint64_t client_id,
                                            uint64_t create, uint64_t expire, int32_t timeout )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_handshake_fill_vector_fields( &signed_fields, &private_fields );
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = timeout;
    private_fields.client_id = client_id;
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void rocketnet_handshake_server_setup( struct rocketnet_handshake_server_t * test )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &test->counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history &&
                          server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );

    // reset the per-update handshake budget, exactly as the transport does at
    // the top of every update

    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );

    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test->second_address, "198.51.100.8:56789" ) );
}

static void rocketnet_handshake_server_teardown( struct rocketnet_handshake_server_t * test )
{
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

// the server's single receive entry, at this source address and the fixed
// mocked time

static int rocketnet_handshake_process_at( struct rocketnet_handshake_server_t * test,
                                           const struct rocketnet_address_t * from,
                                           const uint8_t * packet, int packet_bytes,
                                           uint8_t * response, int * response_bytes,
                                           uint8_t * payload, int * payload_bytes, int * payload_client_index )
{
    return rocketnet_server_protocol_process_packet( &test->server, from, packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, 100.0,
                                                     response, response_bytes,
                                                     payload, payload_bytes, payload_client_index );
}

// builds a valid request around a freshly minted token for this client id

static void rocketnet_handshake_build_request( uint8_t * request, uint64_t client_id, const uint8_t * salt )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_handshake_fill_vector_fields( &signed_fields, &private_fields );
    private_fields.client_id = client_id;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

// builds the same request around a freshly minted token with a chosen create
// timestamp, for the create-skew rung

static void rocketnet_handshake_build_request_with_create( uint8_t * request, uint64_t client_id,
                                                           const uint8_t * salt, uint64_t create )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_handshake_fill_vector_fields( &signed_fields, &private_fields );
    private_fields.client_id = client_id;
    signed_fields.create_timestamp = create;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

// the client's half of the handshake, from wire bytes alone: opens the
// challenge and derives the four keys

static void rocketnet_handshake_client_derive_from_challenge( const uint8_t * challenge, int challenge_bytes,
                                                              struct rocketnet_connection_keys_t * keys )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_unit_check( rocketnet_prefilter_check( challenge, challenge_bytes, tag ) == ROCKETNET_CONNECTION_CHALLENGE_PACKET );
    const uint8_t * p = challenge + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );

    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memcpy( session_nonce, challenge + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    struct rocketnet_connection_keys_t handshake_keys;
    rocketnet_derive_handshake_keys( &handshake_keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, session_nonce );

    uint8_t server_ephemeral[ROCKETNET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_open_sealed_packet( server_ephemeral, session_nonce, ROCKETNET_KEY_BYTES,
                                                        challenge, challenge_bytes,
                                                        ROCKETNET_CONNECTION_CHALLENGE_PACKET, sequence,
                                                        ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                        rocketnet_test_client_eph_public,
                                                        handshake_keys.handshake_server_to_client ) == ROCKETNET_KEY_BYTES );

    rocketnet_unit_check( rocketnet_derive_connection_keys( keys,
                                                            rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                            rocketnet_test_salt, session_nonce,
                                                            rocketnet_test_client_eph_secret, server_ephemeral,
                                                            rocketnet_test_client_eph_public, server_ephemeral ) );
}

static void rocketnet_handshake_client_build_response( uint8_t * response, const struct rocketnet_connection_keys_t * keys )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    const uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    rocketnet_unit_check( rocketnet_write_sealed_packet( response, ROCKETNET_CONNECTION_RESPONSE_PACKET, tag, 1,
                                                         NULL, padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                                         ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                         rocketnet_test_client_eph_public,
                                                         keys->handshake_client_to_server ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
}

// runs request -> cookie -> request -> challenge from this address and hands
// back the client's derived keys, leaving the server one response away from
// a connection

static void rocketnet_handshake_to_challenge( struct rocketnet_handshake_server_t * test,
                                              const struct rocketnet_address_t * from,
                                              uint64_t client_id,
                                              struct rocketnet_connection_keys_t * client_keys )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_handshake_build_request( request, client_id, rocketnet_test_salt );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    rocketnet_unit_check( rocketnet_handshake_process_at( test, from, request, sizeof( request ), response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    memcpy( request + ROCKETNET_PREFILTER_BYTES, response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );

    rocketnet_unit_check( rocketnet_handshake_process_at( test, from, request, sizeof( request ), response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    rocketnet_handshake_client_derive_from_challenge( response, response_bytes, client_keys );
}

// One pump: client and server exchanging packets through a perfect wire. the
// server's periodic keep-alives for connected clients stand in for the send
// pacing the socket layer owns. returns the client's state at exit.

static int rocketnet_handshake_pump( struct rocketnet_handshake_server_t * test,
                                     struct rocketnet_client_protocol_t * client,
                                     double max_seconds )
{
    double time = 100.0;
    double stop_time = time + max_seconds;
    double last_keep_alive_time = time;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    while ( time < stop_time )
    {
        rocketnet_client_protocol_update( client, time, packet, &packet_bytes );

        if ( client->state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            return client->state;

        if ( packet_bytes > 0 )
        {
            rocketnet_handshake_process_at( test, &test->client_address, packet, packet_bytes,
                                            response, &response_bytes, payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( client, response, response_bytes, time, payload, sizeof( payload ), &payload_bytes );
        }

        if ( time - last_keep_alive_time >= 0.1 )
        {
            last_keep_alive_time = time;
            for ( int i = 0; i < rocketnet_client_slots_max_clients( test->server.client_slots ); i++ )
            {
                if ( !rocketnet_client_slots_connected( test->server.client_slots, i ) )
                    continue;
                int bytes = rocketnet_server_write_keep_alive( &test->server, i, time, response );
                if ( bytes > 0 )
                    rocketnet_client_protocol_process_packet( client, response, bytes, time, payload, sizeof( payload ), &payload_bytes );
            }
        }

        if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            return client->state;

        time += 0.05;
    }

    return client->state;
}

// drives a fresh client into one of the terminal states, with mocked time as
// the only clock. the states that need a server build and tear one down here;
// the client itself outlives the call for the caller's assertion.

static void rocketnet_handshake_prepare_terminal( struct rocketnet_client_protocol_t * client,
                                                  struct rocketnet_counters_t * counters,
                                                  int terminal_state )
{
    memset( counters, 0, sizeof( *counters ) );
    rocketnet_client_protocol_init( client, counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;

    switch ( terminal_state )
    {
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED:
            rocketnet_handshake_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 10, 10 );
            rocketnet_unit_check( !rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS + 20 ) );
            break;

        case ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN:
        {
            struct rocketnet_signed_connect_data_t signed_fields;
            struct rocketnet_connect_token_private_t private_fields;
            rocketnet_handshake_fill_vector_fields( &signed_fields, &private_fields );
            signed_fields.address_flags = 0x05;
            rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
            rocketnet_unit_check( !rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
            break;
        }

        case ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT:
            rocketnet_handshake_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
            rocketnet_unit_check( rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
            for ( double time = 100.0; time < 112.0; time += 0.05 )
                rocketnet_client_protocol_update( client, time, packet, &packet_bytes );
            break;

        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT:
        {
            struct rocketnet_handshake_server_t test;
            rocketnet_handshake_server_setup( &test );
            rocketnet_handshake_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
            rocketnet_unit_check( rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0;
            uint8_t payload[ROCKETNET_RECEIVE_CEILING];
            int payload_bytes = 0;
            int payload_client_index = -1;

            for ( double time = 100.0; time < 115.0; time += 0.05 )
            {
                rocketnet_client_protocol_update( client, time, packet, &packet_bytes );
                if ( client->state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
                    break;
                if ( packet_bytes > 0 )
                {
                    rocketnet_handshake_process_at( &test, &test.client_address, packet, packet_bytes,
                                                    response, &response_bytes, payload, &payload_bytes, &payload_client_index );
                    // deliver cookies; swallow challenges, so the client waits
                    // in state 2 until its timeout
                    if ( response_bytes > 0 && response[0] == ROCKETNET_CONNECTION_COOKIE_PACKET )
                        rocketnet_client_protocol_process_packet( client, response, response_bytes, time, payload, sizeof( payload ), &payload_bytes );
                }
            }
            rocketnet_handshake_server_teardown( &test );
            break;
        }

        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT:
        {
            struct rocketnet_handshake_server_t test;
            rocketnet_handshake_server_setup( &test );
            rocketnet_handshake_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
            rocketnet_unit_check( rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0;
            uint8_t payload[ROCKETNET_RECEIVE_CEILING];
            int payload_bytes = 0;
            int payload_client_index = -1;

            for ( double time = 100.0; time < 115.0; time += 0.05 )
            {
                rocketnet_client_protocol_update( client, time, packet, &packet_bytes );
                if ( client->state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
                    break;
                if ( packet_bytes > 0 && client->state < ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
                {
                    rocketnet_handshake_process_at( &test, &test.client_address, packet, packet_bytes,
                                                    response, &response_bytes, payload, &payload_bytes, &payload_client_index );
                    if ( response_bytes > 0 )
                        rocketnet_client_protocol_process_packet( client, response, response_bytes, time, payload, sizeof( payload ), &payload_bytes );
                }
            }
            rocketnet_handshake_server_teardown( &test );
            break;
        }

        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT:
        {
            struct rocketnet_handshake_server_t test;
            rocketnet_handshake_server_setup( &test );
            rocketnet_handshake_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
            rocketnet_unit_check( rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
            rocketnet_unit_check( rocketnet_handshake_pump( &test, client, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
            double connected_time = client->last_receive_time;
            for ( double time = connected_time; time < connected_time + 12.0; time += 0.05 )
                rocketnet_client_protocol_update( client, time, packet, &packet_bytes );
            rocketnet_handshake_server_teardown( &test );
            break;
        }

        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED:
        {
            struct rocketnet_handshake_server_t test;
            rocketnet_handshake_server_setup( &test );
            uint8_t dummy_user_data[ROCKETNET_USER_DATA_BYTES];
            memset( dummy_user_data, 0, sizeof( dummy_user_data ) );
            for ( int i = 0; i < 4; i++ )
                rocketnet_unit_check( rocketnet_client_slots_assign( test.server.client_slots, 0xF100 + (uint64_t) i, -1,
                                                                     0xF100 + (uint64_t) i, dummy_user_data ) == i );

            rocketnet_handshake_mint_token( token_data, 0xDEDEULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
            rocketnet_unit_check( rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
            rocketnet_unit_check( rocketnet_handshake_pump( &test, client, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );
            rocketnet_handshake_server_teardown( &test );
            break;
        }

        default:
            rocketnet_unit_check( 0 );
            break;
    }

    rocketnet_unit_check( client->state == terminal_state );
}

// a byte scan: every byte of the buffer is zero

static int rocketnet_handshake_bytes_all_zero( const uint8_t * bytes, int count )
{
    for ( int i = 0; i < count; i++ )
        if ( bytes[i] != 0 )
            return 0;
    return 1;
}

static int rocketnet_handshake_ephemeral_and_token_scratch_zero( const struct rocketnet_client_protocol_t * client )
{
    if ( !rocketnet_handshake_bytes_all_zero( client->client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) )
        return 0;
    if ( !rocketnet_handshake_bytes_all_zero( client->token.client_to_server_key, ROCKETNET_KEY_BYTES ) )
        return 0;
    if ( !rocketnet_handshake_bytes_all_zero( client->token.server_to_client_key, ROCKETNET_KEY_BYTES ) )
        return 0;

    return 1;
}

static int rocketnet_handshake_disconnect_returns_disconnected( struct rocketnet_client_protocol_t * client )
{
    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_client_protocol_disconnect( client, burst, &burst_count, &burst_bytes );

    return client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
}

// ---------------------------------------------------------------------------
// F4, EphemeralErasedOutsideHandshake (ClientConnection.tla). a denial or a
// handshake timeout is set_state of a state <= DISCONNECTED, which zeroes
// the ephemeral secret and the token keys. the four failure modes are
// denial, request timeout, challenge timeout and response timeout.
// ---------------------------------------------------------------------------

static void test_client_ephemeral_erased_outside_handshake()
{
    const int failure_states[4] = {
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED,                 // -1 denial
        ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT,          // -2 request timeout
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT,      // -3 challenge timeout
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT,     // -4 response timeout
    };

    int zeroed = 0;

    for ( int i = 0; i < 4; i++ )
    {
        struct rocketnet_client_protocol_t client;
        struct rocketnet_counters_t counters;
        rocketnet_handshake_prepare_terminal( &client, &counters, failure_states[i] );
        if ( rocketnet_handshake_ephemeral_and_token_scratch_zero( &client ) )
        {
            zeroed++;
        }
        rocketnet_counters_assert_conserved( &counters );
    }

    rocketnet_unit_check( zeroed == 4 );
}

// ---------------------------------------------------------------------------
// F8, TerminalAbsorbing (ClientConnection.tla). a terminal state leaves
// only by connect or by disconnect. disconnect() from each terminal state
// returns DISCONNECTED.
// ---------------------------------------------------------------------------

static void test_disconnect_absorbs_terminal_state()
{
    const int terminal_states[7] = {
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED,             // -7
        ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN,             // -6
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT,              // -5
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT,     // -4
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT,      // -3
        ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT,          // -2
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED,                 // -1
    };

    int returned = 0;

    for ( int i = 0; i < 7; i++ )
    {
        struct rocketnet_client_protocol_t client;
        struct rocketnet_counters_t counters;
        rocketnet_handshake_prepare_terminal( &client, &counters, terminal_states[i] );
        if ( rocketnet_handshake_disconnect_returns_disconnected( &client ) )
        {
            returned++;
        }
        rocketnet_counters_assert_conserved( &counters );
    }

    rocketnet_unit_check( returned == 7 );
}

// ---------------------------------------------------------------------------
// OneTokenOneLiveConnection with History "none" (RNSecurityTables.tla;
// ServerConnection.tla, item 8, F9): two clients at two addresses present
// one client id (each handshake here mints its own token for that id); the
// used-token history is flushed between their requests, so the client-id
// check at the response alone refuses the second admission while the first
// connection lives. once it closes, the second may connect: a slot is freed
// when its connection closes, and the accepted residual risk is reuse inside
// the short expiry (docs/SESSION.md 1.6).
// ---------------------------------------------------------------------------

static void test_one_connection_per_token_after_history_flush()
{
    struct rocketnet_handshake_server_t test;
    rocketnet_handshake_server_setup( &test );

    struct rocketnet_connection_keys_t keys_a;
    struct rocketnet_connection_keys_t keys_b;

    // the first client's request passes the history and creates a mapping
    // (ServerRecvRequest, the two half-open mappings of the model)

    rocketnet_handshake_to_challenge( &test, &test.client_address, 0x5A0CULL, &keys_a );

    // history "none": flush the used-token history so the second client's
    // request no longer sees the binding (S:2782-2786)
    {
        uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];

        for ( int i = 0; i < 64; i++ )
        {
            memset( identifier, 0, sizeof( identifier ) );
            identifier[0] = (uint8_t) ( i + 1 );
            rocketnet_used_token_history_record( test.server.used_token_history, identifier, &test.second_address,
                                                 ROCKETNET_TEST_EXPIRE_TS, ROCKETNET_TEST_CREATE_TS );
        }
    }

    // the second client's request, from another address, passes the history
    // (nobody has connected yet) and gets a mapping

    rocketnet_handshake_to_challenge( &test, &test.second_address, 0x5A0CULL, &keys_b );

    // the first client's response is seated
    {
        uint8_t response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
        rocketnet_handshake_client_build_response( response, &keys_a );

        uint8_t out[ROCKETNET_RECEIVE_CEILING];
        int out_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;
        rocketnet_unit_check( rocketnet_handshake_process_at( &test, &test.client_address, response, sizeof( response ),
                                                              out, &out_bytes, payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
        rocketnet_unit_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    }

    // the second client's response arrives while the first connection lives:
    // its client id is connected now, so the client-id check refuses it
    {
        uint8_t response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
        rocketnet_handshake_client_build_response( response, &keys_b );

        uint8_t out[ROCKETNET_RECEIVE_CEILING];
        int out_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;
        const uint64_t refused_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED];
        int result = rocketnet_handshake_process_at( &test, &test.second_address, response, sizeof( response ),
                                                     out, &out_bytes, payload, &payload_bytes, &payload_client_index );
        rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED] == refused_before + 1 );
        rocketnet_unit_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    }

    // the first connection is disconnected; the slot and mapping go
    {
        uint8_t burst[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
        int burst_count = 0;
        int burst_bytes = 0;
        rocketnet_server_protocol_disconnect_client( &test.server, 0, burst, &burst_count, &burst_bytes );
        rocketnet_unit_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 0 );
    }

    // the second client's retransmitted response arrives within its mapping's
    // handshake deadline. no connection holds the client id now, so it is
    // seated: a slot is freed when its connection closes
    {
        uint8_t response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
        rocketnet_handshake_client_build_response( response, &keys_b );

        uint8_t out[ROCKETNET_RECEIVE_CEILING];
        int out_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;
        int result = rocketnet_handshake_process_at( &test, &test.second_address, response, sizeof( response ),
                                                     out, &out_bytes, payload, &payload_bytes, &payload_client_index );
        rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
        rocketnet_unit_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    }

    rocketnet_handshake_server_teardown( &test );
}

// ---------------------------------------------------------------------------
// RepeatKeepAlive, the code's behaviour (ServerConnection.tla). the reversed
// witness RepeatKeepAlive = FALSE removes
// rocketnet_server_protocol_write_keep_alive_if_due, so the establishing
// keep-alive is sent once and never again; the client in state 3 waits for a
// packet nobody will send again. here the client has not acknowledged a
// keep-alive, and the server keeps generating one per send interval while the
// wire is otherwise idle.
// ---------------------------------------------------------------------------

static void test_server_repeats_keep_alive_while_idle()
{
    struct rocketnet_handshake_server_t test;
    rocketnet_handshake_server_setup( &test );
    test.server.send_rate = 10.0;

    struct rocketnet_connection_keys_t client_keys;
    rocketnet_handshake_to_challenge( &test, &test.client_address, 0x9A11EULL, &client_keys );

    uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    rocketnet_handshake_client_build_response( client_response, &client_keys );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    // the response seats the client and sends the first keep-alive at 100.0,
    // stamping the keep-alive clock

    rocketnet_unit_check( rocketnet_handshake_process_at( &test, &test.client_address, client_response, sizeof( client_response ),
                                                          response, &response_bytes, payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    rocketnet_unit_check( !rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) );

    // while the client has not acknowledged a keep-alive, the server keeps
    // generating one per send interval, idle or not

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    rocketnet_unit_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.11, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    rocketnet_unit_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.22, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    rocketnet_unit_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.33, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    rocketnet_handshake_server_teardown( &test );
}

// ---------------------------------------------------------------------------
// KeepAliveClock, the code's behaviour (ClientConnection.tla). the reversed
// witness KeepAliveClock = "anysend" paces the keep-alive off the last send of
// anything, so a payload stream suppresses the repair keep-alive (the stranding
// regression). here a payload while the client is still unacknowledged does
// not restamp the keep-alive clock, and the keep-alive is still due on it.
// ---------------------------------------------------------------------------

#if ROCKETNET_DEV
static void test_keep_alive_clock_counts_only_keep_alives()
{
    struct rocketnet_handshake_server_t test;
    rocketnet_handshake_server_setup( &test );
    test.server.send_rate = 10.0;

    struct rocketnet_connection_keys_t client_keys;
    rocketnet_handshake_to_challenge( &test, &test.client_address, 0x90C0CULL, &client_keys );

    uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    rocketnet_handshake_client_build_response( client_response, &client_keys );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    rocketnet_unit_check( rocketnet_handshake_process_at( &test, &test.client_address, client_response, sizeof( client_response ),
                                                          response, &response_bytes, payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    rocketnet_unit_check( !rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    // the second keep-alive at 100.11 stamps the keep-alive clock

    rocketnet_unit_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.11, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    // a payload at 100.15 restamps last_send_time but not the keep-alive
    // clock; the keep-alive is still due one interval after the last one at
    // 100.11. under "anysend" the payload would push it past 100.22.

    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_unit_check( rocketnet_server_protocol_write_payload( &test.server, 0, data, 4, 100.15, packet ) > 0 );
    rocketnet_unit_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.16, packet ) == 0 );
    rocketnet_unit_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.22, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    rocketnet_handshake_server_teardown( &test );
}
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// Create-skew refusal: a connect token created more than thirty seconds in the
// future is refused as CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE, always:
// the fixture runs the production configuration, which has no token lifetime
// limit and never had one
// (sdk/rocketnet_server_protocol.cpp,
// rocketnet_server_protocol_process_connection_request)
// ---------------------------------------------------------------------------

static void test_token_created_past_skew_is_refused()
{
    struct rocketnet_handshake_server_t test;
    rocketnet_handshake_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_handshake_build_request_with_create( request, 0x5A0CULL, rocketnet_test_salt,
                                                   ROCKETNET_TEST_CREATE_TS + 31 );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint64_t before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE];

    int result = rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                       request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                       ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                       response, &response_bytes );
    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( response_bytes == 0 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE] == before + 1 );
    rocketnet_unit_check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );

    rocketnet_handshake_server_teardown( &test );
}

static void test_token_created_within_skew_passes_the_rung()
{
    struct rocketnet_handshake_server_t test;
    rocketnet_handshake_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_handshake_build_request_with_create( request, 0x5A0CULL, rocketnet_test_salt,
                                                   ROCKETNET_TEST_CREATE_TS + 30 );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint64_t before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED];

    int result = rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                       request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                       ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                       response, &response_bytes );
    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] == before );

    rocketnet_handshake_server_teardown( &test );
}

static void test_token_create_skew_enforced_without_lifetime_cap()
{
    struct rocketnet_handshake_server_t test;
    rocketnet_handshake_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_handshake_build_request_with_create( request, 0x5A0CULL, rocketnet_test_salt,
                                                   ROCKETNET_TEST_CREATE_TS + 31 );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = -1;
    const struct rocketnet_counters_t before = test.counters;

    int result = rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                       request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                       ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                       response, &response_bytes );
    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( response_bytes == 0 );
    rocketnet_counters_expect( &before, &test.counters,
                               { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE, 1 } } );
    rocketnet_unit_check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );

    // the boundary remains admissible: this is a creation-skew refusal, not
    // a blanket refusal of long-lived tokens

    rocketnet_handshake_build_request_with_create( request, 0x5A0CULL, rocketnet_test_salt,
                                                   ROCKETNET_TEST_CREATE_TS + 30 );

    const struct rocketnet_counters_t after_refusal = test.counters;
    result = rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                   request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                   ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                   response, &response_bytes );
    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    rocketnet_counters_expect( &after_refusal, &test.counters,
                               { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE, 1 } } );

    rocketnet_handshake_server_teardown( &test );
}

// ---------------------------------------------------------------------------
// The claim is written only after the signature verifies
// (sdk/rocketnet_server_protocol.cpp, rocketnet_used_token_history_claim_can_record
// at the claim rung, rocketnet_used_token_history_claim_record after
// rocketnet_verify_signed_connect_data; proofs/tla/ReceiveOrder.tla,
// ClaimOnlyAfterSignature). A request whose signature fails touches nothing
// in the claim table, so it cannot disturb a live claim either: here the
// first address holds a live claim on the token (written directly: only a
// request that reaches its challenge keeps a claim, and such an address then
// has a mapping that answers its requests before the claim rung), a
// forged-signature request from that same address follows, and the captured
// valid request from a second address must still be refused at the claim
// rung, with no signature work. Before the split the forged request wrote the
// claim and its failed signature released it, freeing the token for the
// second address.
// ---------------------------------------------------------------------------

#if ROCKETNET_DEV
static int rocketnet_handshake_request_past_cookie( struct rocketnet_handshake_server_t * test,
                                                    const struct rocketnet_address_t * from,
                                                    uint8_t * request, uint8_t * response, int * response_bytes )
{
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    memset( request + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_COOKIE_FIELD_BYTES );
    rocketnet_unit_check( rocketnet_handshake_process_at( test, from, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                          response, response_bytes, payload, &payload_bytes,
                                                          &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    memcpy( request + ROCKETNET_PREFILTER_BYTES, response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );

    return rocketnet_handshake_process_at( test, from, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                           response, response_bytes, payload, &payload_bytes, &payload_client_index );
}

static void test_claim_written_only_after_signature_verifies()
{
    struct rocketnet_handshake_server_t test;
    rocketnet_handshake_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_handshake_build_request( request, 0xC1A1ULL, rocketnet_test_salt );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    // the first address holds a live claim on the token

    const uint8_t * signed_data = request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                                  ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_connect_token_identifier( identifier, signed_data );
    rocketnet_unit_check( rocketnet_used_token_history_claim_record( test.server.used_token_history, identifier, &test.client_address,
                                                                     100.0 + test.server.handshake_deadline_seconds, 100.0 ) );

    // the same request from the same address with its signature corrupted

    uint8_t forged[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    memcpy( forged, request, sizeof( forged ) );
    forged[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1] ^= 0x01;

    const uint64_t bad_signatures = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY];
    rocketnet_unit_check( rocketnet_handshake_request_past_cookie( &test, &test.client_address, forged, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == bad_signatures + 1 );

    // the captured valid request from a second address: the first address's
    // claim still stands, so the claim rung refuses it before any signature

    const uint64_t already_used = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED];
    memset( &rocketnet_crypto_tally, 0, sizeof( rocketnet_crypto_tally ) );
    rocketnet_unit_check( rocketnet_handshake_request_past_cookie( &test, &test.second_address, request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( response_bytes == 0 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] == already_used + 1 );
    rocketnet_unit_check( rocketnet_crypto_tally.signature_verify == 0 );

    rocketnet_handshake_server_teardown( &test );
}
#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_client_ephemeral_erased_outside_handshake );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_claim_written_only_after_signature_verifies );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_token_created_past_skew_is_refused );
REGISTER_UNIT_TEST( test_token_created_within_skew_passes_the_rung );
REGISTER_UNIT_TEST( test_token_create_skew_enforced_without_lifetime_cap );
REGISTER_UNIT_TEST( test_disconnect_absorbs_terminal_state );
REGISTER_UNIT_TEST( test_one_connection_per_token_after_history_flush );
REGISTER_UNIT_TEST( test_server_repeats_keep_alive_while_idle );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_keep_alive_clock_counts_only_keep_alives );
#endif // #if ROCKETNET_DEV
