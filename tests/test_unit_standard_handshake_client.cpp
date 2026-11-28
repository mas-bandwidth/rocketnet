/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0 or later, or the Más Bandwidth Source License 1.0 (MBSL-1.0). See LICENSE and MBSL.md.
*/

// test_unit_standard_handshake_client.cpp -- the vector twins of RN-1 t11
// (standard/TRANSPORT.md, Handshake: client), regenerated through the
// implementation. The transcript is the client's packets of one handshake:
// a request without a cookie, the request with the stored cookie, the same
// request retransmitted while the challenge is lost, and the response. No
// socket, no thread, no clock: time is passed in and starts at 1000.0, and
// every random field is replaced by the published test value before the
// first send.

#include "test_unit_standard.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_protocol.h"
#include "rocketnet_keys.h"
#include "rocketnet_ack.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_constants.h"
#include "rocketnet_test_vectors.h"

#include <string.h>

static const rn_standard_vector_t standard_handshake_client_table[] = {
    RN_STANDARD_VECTOR( "t11.client-transcript", "0000b759010000000000000000000000000000000000000000000000000053414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24d9145006887766554433221100f9ac68000000002cfaac68000000000a00000001cb00710a409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801cd622aaba9c50e763fee524359b346dad155bbcba8369e291f8a966c2338f2a04071c9df819c6f1ae0c3bacd7b6f5b1d0a8b7ca0489c01d03c1ea5c84738e35a2790595ed645aa7afbeea242277a7de07c83f9eefea8c2cbfc7bc2d298ff826a462c95fff2fa9d11ccc232b20990c5417e2b2674dfd871cf37c6e797aa7bc4d7cb417f690c5d0b1688f1f497f82860f705ebdf3a92376ea0fea507d18d73e4d7a21e10f88b9fcf8f981ea74a77d7531edca801bcb8e7684249c47400dcdbf5ca4ea6aaf3f4f97633db2b5d035315fde519f7852edb943e86ea2ca7152632fc02b481d0cfdb403adf23afe80e7989e635300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000ee93da92a342ff6caa56c6db74124ae50c74b6dc23d3ce654a355906f81ded1b0498a6cc65761edcbb35b55c55dce7079cf6e22fa6f3b6763cd839abbeec0b090000b759010003f9ac68000000000fb2b28f32dbd6c9eb364f577486f3e953414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24d9145006887766554433221100f9ac68000000002cfaac68000000000a00000001cb00710a409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801cd622aaba9c50e763fee524359b346dad155bbcba8369e291f8a966c2338f2a04071c9df819c6f1ae0c3bacd7b6f5b1d0a8b7ca0489c01d03c1ea5c84738e35a2790595ed645aa7afbeea242277a7de07c83f9eefea8c2cbfc7bc2d298ff826a462c95fff2fa9d11ccc232b20990c5417e2b2674dfd871cf37c6e797aa7bc4d7cb417f690c5d0b1688f1f497f82860f705ebdf3a92376ea0fea507d18d73e4d7a21e10f88b9fcf8f981ea74a77d7531edca801bcb8e7684249c47400dcdbf5ca4ea6aaf3f4f97633db2b5d035315fde519f7852edb943e86ea2ca7152632fc02b481d0cfdb403adf23afe80e7989e635300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000ee93da92a342ff6caa56c6db74124ae50c74b6dc23d3ce654a355906f81ded1b0498a6cc65761edcbb35b55c55dce7079cf6e22fa6f3b6763cd839abbeec0b090000b759010003f9ac68000000000fb2b28f32dbd6c9eb364f577486f3e953414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24d9145006887766554433221100f9ac68000000002cfaac68000000000a00000001cb00710a409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801cd622aaba9c50e763fee524359b346dad155bbcba8369e291f8a966c2338f2a04071c9df819c6f1ae0c3bacd7b6f5b1d0a8b7ca0489c01d03c1ea5c84738e35a2790595ed645aa7afbeea242277a7de07c83f9eefea8c2cbfc7bc2d298ff826a462c95fff2fa9d11ccc232b20990c5417e2b2674dfd871cf37c6e797aa7bc4d7cb417f690c5d0b1688f1f497f82860f705ebdf3a92376ea0fea507d18d73e4d7a21e10f88b9fcf8f981ea74a77d7531edca801bcb8e7684249c47400dcdbf5ca4ea6aaf3f4f97633db2b5d035315fde519f7852edb943e86ea2ca7152632fc02b481d0cfdb403adf23afe80e7989e635300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000ee93da92a342ff6caa56c6db74124ae50c74b6dc23d3ce654a355906f81ded1b0498a6cc65761edcbb35b55c55dce7079cf6e22fa6f3b6763cd839abbeec0b090300b7590100010000009c08609896b9d405d5149e1ed9dae4dc2bd24ae4cda19310f97be68d1ebb193bb922aed5d15e0aea" ),
};

static int standard_handshake_client_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t11.client-transcript" ) != 0 )
    {
        return -1;
    }

    if ( capacity < 4 * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES )
    {
        return -1;
    }

    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rocketnet_client_protocol_init( &client, &counters );

    // one attempt from the frozen connect token, at the test create time

    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, rocketnet_test_frozen_token,
                                                             1000.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    // the published test value for every random field of this attempt

    memcpy( client.connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( client.client_ephemeral_secret_key, rocketnet_test_client_eph_secret, ROCKETNET_KEY_BYTES );
    memcpy( client.client_ephemeral_public_key, rocketnet_test_client_eph_public, ROCKETNET_KEY_BYTES );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    int written = 0;

    // state 1: the request carries an all-zero return cookie

    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, 1000.0, out + written, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    written += packet_bytes;

    // a cookie arrives; the client stores it and moves to state 2. It is the
    // cookie of t12.cookie, a server's at 1756166400 with lifetime 3 to
    // 198.51.100.7:56789, so its expire and its MAC belong together

    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( cookie, tag, rocketnet_test_salt,
                                              ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_mac );
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, cookie, sizeof( cookie ), 1000.05,
                                                                    payload, sizeof( payload ), &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

    // state 2: the next scheduled request carries the stored cookie

    rocketnet_client_protocol_update( &client, 1000.1, out + written, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

    written += packet_bytes;

    // the challenge is lost: the next scheduled send retransmits the request

    rocketnet_client_protocol_update( &client, 1000.2, out + written, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

    written += packet_bytes;

    // the server's challenge answers the retransmission; the client opens it,
    // derives the session keys and moves to state 3

    struct rocketnet_connection_keys_t handshake_keys;
    rocketnet_derive_handshake_keys( &handshake_keys,
                                     rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, rocketnet_test_session_nonce );

    uint8_t challenge[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    rocketnet_unit_check( rocketnet_write_sealed_packet( challenge, ROCKETNET_CONNECTION_CHALLENGE_PACKET, tag, 1,
                                                         rocketnet_test_session_nonce,
                                                         rocketnet_test_server_eph_public, ROCKETNET_KEY_BYTES,
                                                         ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                         rocketnet_test_client_eph_public,
                                                         handshake_keys.handshake_server_to_client ) ==
                          ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, challenge, sizeof( challenge ), 1000.25,
                                                                    payload, sizeof( payload ), &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    // A delayed cookie with this attempt's salt but an arbitrary MAC cannot
    // rewind the authenticated challenge or alter its stored response.

    uint8_t stored_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    memcpy( stored_response, client.stored_response, sizeof( stored_response ) );
    const double received = client.last_receive_time;
    cookie[sizeof( cookie ) - 1] ^= 1;
    rocketnet_unit_check( !rocketnet_client_protocol_process_packet( &client, cookie, sizeof( cookie ), 1000.26,
                                                                     payload, sizeof( payload ), &payload_bytes ) );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    rocketnet_unit_check( client.last_receive_time == received );
    rocketnet_unit_check( memcmp( stored_response, client.stored_response, sizeof( stored_response ) ) == 0 );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE] == 1 );

    // state 3: the response is the stored bytes, sent at the scheduled rate

    rocketnet_client_protocol_update( &client, 1000.31, out + written, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    written += packet_bytes;

    // the server's keep-alive completes the handshake; the client state after
    // this packet is connected (4), and its next send is a keep-alive

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 8];
    struct rocketnet_replay_protection_t empty_replay;
    memset( &empty_replay, 0, sizeof( empty_replay ) );
    const int block_bytes = rocketnet_ack_block_build( plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES, &empty_replay );
    rocketnet_unit_check( block_bytes == ROCKETNET_ACK_BLOCK_MIN_BYTES );

    uint8_t * p = plaintext + block_bytes;
    rocketnet_write_uint32( &p, 0 );
    rocketnet_write_uint32( &p, 1 );

    uint8_t keep_alive[ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES];
    rocketnet_unit_check( rocketnet_write_sealed_packet( keep_alive, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, tag, 1,
                                                         NULL, plaintext, block_bytes + 8,
                                                         ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                         client.keys.session_server_to_client ) ==
                          ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, keep_alive, sizeof( keep_alive ),
                                                                    1000.36,
                                                                    payload, sizeof( payload ), &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_unit_check( client.client_index == 0 && client.max_clients == 1 );

    return written;
}

static void test_standard_handshake_client_vectors()
{
    const int count = (int) ( sizeof( standard_handshake_client_table ) / sizeof( standard_handshake_client_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_handshake_client_table, count, standard_handshake_client_generate ) == 1 );
}

REGISTER_UNIT_TEST( test_standard_handshake_client_vectors );

static void test_client_protocol_connect_ignored_while_attempt_live()
{
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rocketnet_client_protocol_init( &client, &counters );
    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, rocketnet_test_frozen_token,
                                                             1000.0, ROCKETNET_TEST_CREATE_TS ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    uint8_t original_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    memcpy( original_salt, client.connection_salt, sizeof( original_salt ) );

    const struct rocketnet_counters_t original_counters = counters;
    const uint64_t original_handshake_sequence = client.send_sequence_handshake;
    const uint64_t original_session_sequence = client.send_sequence_session;

    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, rocketnet_test_frozen_token,
                                                             1001.0, ROCKETNET_TEST_CREATE_TS ) == 0 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
    rocketnet_unit_check( memcmp( client.connection_salt, original_salt, sizeof( original_salt ) ) == 0 );
    rocketnet_unit_check( memcmp( &counters, &original_counters, sizeof( counters ) ) == 0 );
    rocketnet_unit_check( client.send_sequence_handshake == original_handshake_sequence );
    rocketnet_unit_check( client.send_sequence_session == original_session_sequence );
}

REGISTER_UNIT_TEST( test_client_protocol_connect_ignored_while_attempt_live );
