/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
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

static const rn_standard_vector_t standard_handshake_client_table[] =
{
    RN_STANDARD_VECTOR( "t11.client-transcript", "0000b759010000000000000000000000"
    "00000000000000000000000000005341"
    "4c54303030313a553d74792d727efa9b"
    "9a4cde3da1ad93f1a2d0c09cb639b1a3"
    "c0fda14cbe24931a7c4e887766554433"
    "221100f9ac68000000002cfaac680000"
    "00000a00000001cb00710a409c000000"
    "0000000000000000000000000000003d"
    "2490889d374c296fab1d5770cb71476a"
    "06f74ef2f2540cf2215f7614f4801cb3"
    "10fdd303d6aad8c7900dfc4252d94d21"
    "34f1e6e17320f4c5b1bd4f2879da3ce2"
    "f287720861887378edf01de9befc09f1"
    "bf01fd3df19e2a79e60859e896346eb6"
    "0c08f686721d81946e781e1ae2c3716a"
    "465880a3a5f068ede0b039cf5281a97c"
    "d36717766b5bb13869838ecb333d9eeb"
    "1042bf7a7b8f62f9cbb020ddbda6cb5f"
    "44e4def43666642ff8e6f835a5f295dd"
    "8d7bf2268684b2a93ec17ac8fdc4c4f0"
    "5e5b09927b6afac3ec0f00508e9d4331"
    "6600c6b745d6dbb9c68f12d569a19573"
    "d460ba34e801d744d6db6a0a01bd35b4"
    "e08d5203bbbd44367ef89bcbb3827fd8"
    "0295f2b9404cdd2979e3aa17513ff9f4"
    "2b6706248ea762282028d4fe0cf71156"
    "554f26bf90cb2bfcfef438b5e68ad250"
    "1f9e3be24bc0b33651ec756f60f423f3"
    "24d90323adc9f6c8f1762b02ba31c7e7"
    "9626067fe7f109594a803d330502feb0"
    "3f61e9297ec11aadebcd69bac0bfd05c"
    "1c5c144ae23bcd114c35322e797843e9"
    "5ccb8bc9748fda08197ce6d5a10c83bc"
    "b0eb7ff471d738a742f68fa3eb77dfad"
    "d320a11a55420e3ccd03f15efc183a5d"
    "d35cf8657fa712b7c97496000e6a5468"
    "d800d49d1345ce185dc0f7bc7147a900"
    "00000000000000000000000000000000"
    "00000000000000000000000000000000"
    "00000000000000000000000000000000"
    "0000000000006b3692b49d36b7a27630"
    "3340ab8399b98561ac6881fc7aa2bb60"
    "ffe07732ea7121fe5044ebb5f53dbe76"
    "8b800c30d75370eb9beb4914ae133da6"
    "4ea6b09e760d0000b75901002cfaac68"
    "000000000fb2b28f32dbd6c9eb364f57"
    "7486f3e953414c54303030313a553d74"
    "792d727efa9b9a4cde3da1ad93f1a2d0"
    "c09cb639b1a3c0fda14cbe24931a7c4e"
    "887766554433221100f9ac6800000000"
    "2cfaac68000000000a00000001cb0071"
    "0a409c00000000000000000000000000"
    "00000000003d2490889d374c296fab1d"
    "5770cb71476a06f74ef2f2540cf2215f"
    "7614f4801cb310fdd303d6aad8c7900d"
    "fc4252d94d2134f1e6e17320f4c5b1bd"
    "4f2879da3ce2f287720861887378edf0"
    "1de9befc09f1bf01fd3df19e2a79e608"
    "59e896346eb60c08f686721d81946e78"
    "1e1ae2c3716a465880a3a5f068ede0b0"
    "39cf5281a97cd36717766b5bb1386983"
    "8ecb333d9eeb1042bf7a7b8f62f9cbb0"
    "20ddbda6cb5f44e4def43666642ff8e6"
    "f835a5f295dd8d7bf2268684b2a93ec1"
    "7ac8fdc4c4f05e5b09927b6afac3ec0f"
    "00508e9d43316600c6b745d6dbb9c68f"
    "12d569a19573d460ba34e801d744d6db"
    "6a0a01bd35b4e08d5203bbbd44367ef8"
    "9bcbb3827fd80295f2b9404cdd2979e3"
    "aa17513ff9f42b6706248ea762282028"
    "d4fe0cf71156554f26bf90cb2bfcfef4"
    "38b5e68ad2501f9e3be24bc0b33651ec"
    "756f60f423f324d90323adc9f6c8f176"
    "2b02ba31c7e79626067fe7f109594a80"
    "3d330502feb03f61e9297ec11aadebcd"
    "69bac0bfd05c1c5c144ae23bcd114c35"
    "322e797843e95ccb8bc9748fda08197c"
    "e6d5a10c83bcb0eb7ff471d738a742f6"
    "8fa3eb77dfadd320a11a55420e3ccd03"
    "f15efc183a5dd35cf8657fa712b7c974"
    "96000e6a5468d800d49d1345ce185dc0"
    "f7bc7147a90000000000000000000000"
    "00000000000000000000000000000000"
    "00000000000000000000000000000000"
    "0000000000000000000000006b3692b4"
    "9d36b7a276303340ab8399b98561ac68"
    "81fc7aa2bb60ffe07732ea7121fe5044"
    "ebb5f53dbe768b800c30d75370eb9beb"
    "4914ae133da64ea6b09e760d0000b759"
    "01002cfaac68000000000fb2b28f32db"
    "d6c9eb364f577486f3e953414c543030"
    "30313a553d74792d727efa9b9a4cde3d"
    "a1ad93f1a2d0c09cb639b1a3c0fda14c"
    "be24931a7c4e887766554433221100f9"
    "ac68000000002cfaac68000000000a00"
    "000001cb00710a409c00000000000000"
    "00000000000000000000003d2490889d"
    "374c296fab1d5770cb71476a06f74ef2"
    "f2540cf2215f7614f4801cb310fdd303"
    "d6aad8c7900dfc4252d94d2134f1e6e1"
    "7320f4c5b1bd4f2879da3ce2f2877208"
    "61887378edf01de9befc09f1bf01fd3d"
    "f19e2a79e60859e896346eb60c08f686"
    "721d81946e781e1ae2c3716a465880a3"
    "a5f068ede0b039cf5281a97cd3671776"
    "6b5bb13869838ecb333d9eeb1042bf7a"
    "7b8f62f9cbb020ddbda6cb5f44e4def4"
    "3666642ff8e6f835a5f295dd8d7bf226"
    "8684b2a93ec17ac8fdc4c4f05e5b0992"
    "7b6afac3ec0f00508e9d43316600c6b7"
    "45d6dbb9c68f12d569a19573d460ba34"
    "e801d744d6db6a0a01bd35b4e08d5203"
    "bbbd44367ef89bcbb3827fd80295f2b9"
    "404cdd2979e3aa17513ff9f42b670624"
    "8ea762282028d4fe0cf71156554f26bf"
    "90cb2bfcfef438b5e68ad2501f9e3be2"
    "4bc0b33651ec756f60f423f324d90323"
    "adc9f6c8f1762b02ba31c7e79626067f"
    "e7f109594a803d330502feb03f61e929"
    "7ec11aadebcd69bac0bfd05c1c5c144a"
    "e23bcd114c35322e797843e95ccb8bc9"
    "748fda08197ce6d5a10c83bcb0eb7ff4"
    "71d738a742f68fa3eb77dfadd320a11a"
    "55420e3ccd03f15efc183a5dd35cf865"
    "7fa712b7c97496000e6a5468d800d49d"
    "1345ce185dc0f7bc7147a90000000000"
    "00000000000000000000000000000000"
    "00000000000000000000000000000000"
    "00000000000000000000000000000000"
    "00006b3692b49d36b7a276303340ab83"
    "99b98561ac6881fc7aa2bb60ffe07732"
    "ea7121fe5044ebb5f53dbe768b800c30"
    "d75370eb9beb4914ae133da64ea6b09e"
    "760d0300b7590100010000009c086098"
    "96b9d405d5149e1ed9dae4dc2bd24ae4"
    "cda193105cbfa9ce7461b397ce4b50c0"
    "1b2f8f7e" ),
};

static int standard_handshake_client_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t11.client-transcript" ) != 0 )
        return -1;

    if ( capacity < 4 * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES )
        return -1;

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

    // a cookie arrives; the client stores it and moves to state 2
    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( cookie, tag, rocketnet_test_salt,
                                              ROCKETNET_TEST_EXPIRE_TS, rocketnet_test_cookie_mac );
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
    memcpy( stored_response, client.stored_response, sizeof(stored_response) );
    const double received = client.last_receive_time;
    cookie[sizeof(cookie) - 1] ^= 1;
    rocketnet_unit_check( !rocketnet_client_protocol_process_packet( &client, cookie, sizeof(cookie), 1000.26,
                                                                     payload, sizeof(payload), &payload_bytes ) );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    rocketnet_unit_check( client.last_receive_time == received );
    rocketnet_unit_check( memcmp( stored_response, client.stored_response, sizeof(stored_response) ) == 0 );
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
    memset( &counters, 0, sizeof(counters) );
    rocketnet_client_protocol_init( &client, &counters );
    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, rocketnet_test_frozen_token,
                                                            1000.0, ROCKETNET_TEST_CREATE_TS ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    uint8_t original_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    memcpy( original_salt, client.connection_salt, sizeof(original_salt) );
    const struct rocketnet_counters_t original_counters = counters;
    const uint64_t original_handshake_sequence = client.send_sequence_handshake;
    const uint64_t original_session_sequence = client.send_sequence_session;

    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, rocketnet_test_frozen_token,
                                                            1001.0, ROCKETNET_TEST_CREATE_TS ) == 0 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
    rocketnet_unit_check( memcmp( client.connection_salt, original_salt, sizeof(original_salt) ) == 0 );
    rocketnet_unit_check( memcmp( &counters, &original_counters, sizeof(counters) ) == 0 );
    rocketnet_unit_check( client.send_sequence_handshake == original_handshake_sequence );
    rocketnet_unit_check( client.send_sequence_session == original_session_sequence );
}

REGISTER_UNIT_TEST( test_client_protocol_connect_ignored_while_attempt_live );
