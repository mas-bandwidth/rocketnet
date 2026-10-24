/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_packet_protection.cpp -- the vector twins of RN-1 t9
// (standard/TRANSPORT.md, Packet protection), regenerated through the
// implementation. Each associated data vector comes from the sdk builder,
// each sealed vector from the sdk sealed-packet writer, and each tamper
// vector is the challenge associated data with one field altered. The test
// also opens the challenge vector under every tamper vector and requires the
// tag to fail.

#include "test_unit_standard.h"

#include "rocketnet.h"
#include "rocketnet_wire.h"
#include "rocketnet_read_write.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"

#if defined(ROCKETNET_USE_SYSTEM_SODIUM)
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

static const rn_standard_vector_t standard_packet_protection_table[] =
{
    RN_STANDARD_VECTOR( "t9.challenge-ad", "931a7c4e88776655443322110253414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24" ),
    RN_STANDARD_VECTOR( "t9.challenge-sealed", "686fb709435ff00e1ac260eeeafbb29120201086c8276255d7d387c39232947b3da13ddd6856b09415f3086c59f76f88" ),
    RN_STANDARD_VECTOR( "t9.denied-ad", "931a7c4e88776655443322110153414c5430303031" ),
    RN_STANDARD_VECTOR( "t9.denied-sealed", "e712b88ee236cfd905387cbc0f9dee13" ),
    RN_STANDARD_VECTOR( "t9.response-ad", "931a7c4e88776655443322110353414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24" ),
    RN_STANDARD_VECTOR( "t9.response-sealed", "9c08609896b9d405d5149e1ed9dae4dc2bd24ae4cda193105cbfa9ce7461b397ce4b50c01b2f8f7e" ),
    RN_STANDARD_VECTOR( "t9.keepalive-ad", "931a7c4e88776655443322110453414c5430303031" ),
    RN_STANDARD_VECTOR( "t9.keepalive-sealed", "d248d810b069788527a051d06ebceada25a6663835c471ac2166383ba8421c" ),
    RN_STANDARD_VECTOR( "t9.payload-ad", "931a7c4e88776655443322110553414c5430303031" ),
    RN_STANDARD_VECTOR( "t9.payload-sealed", "e3e6e1abc8d733fef9d5e1e2af0df949ae6d5ebcaadff19489744cb4" ),
    RN_STANDARD_VECTOR( "t9.disconnect-ad", "931a7c4e88776655443322110653414c5430303031" ),
    RN_STANDARD_VECTOR( "t9.disconnect-sealed", "e4db4c3d2d40303931ba1355325bfd8f2f3a2b3175bda2" ),
    RN_STANDARD_VECTOR( "t9.ack-ad", "931a7c4e88776655443322110853414c5430303031" ),
    RN_STANDARD_VECTOR( "t9.ack-sealed", "cbaca39867d9e72a0e3fb604855128e15bd4b118bdd59638f79ae2" ),
    RN_STANDARD_VECTOR( "t9.fragment-ad", "931a7c4e88776655443322110953414c5430303031" ),
    RN_STANDARD_VECTOR( "t9.fragment-sealed", "4d11935455cd7bdfe3973505ee2042143337a0b1eba2ced353f8ddb378a4e5edff7f" ),
    RN_STANDARD_VECTOR( "t9.version-tampered", "921a7c4e88776655443322110253414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24" ),
    RN_STANDARD_VECTOR( "t9.protocol-id-tampered", "931a7c4e89776655443322110253414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24" ),
    RN_STANDARD_VECTOR( "t9.packet-type-tampered", "931a7c4e88776655443322110353414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24" ),
    RN_STANDARD_VECTOR( "t9.connection-salt-tampered", "931a7c4e88776655443322110252414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24" ),
    RN_STANDARD_VECTOR( "t9.client-ephemeral-tampered", "931a7c4e88776655443322110253414c54303030313b553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24" ),
};

static int standard_packet_protection_ad( uint8_t * out, int capacity, uint8_t packet_type )
{
    if ( capacity < ROCKETNET_AD_WITH_EPHEMERAL_BYTES )
        return -1;
    const uint8_t * client_ephemeral = ( packet_type == ROCKETNET_CONNECTION_CHALLENGE_PACKET ||
                                         packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
                                           ? rocketnet_test_client_eph_public : NULL;
    return rocketnet_write_packet_associated_data( out, ROCKETNET_TEST_PROTOCOL_ID, packet_type,
                                                   rocketnet_test_salt, client_ephemeral );
}

static int standard_packet_protection_sealed( uint8_t * out, int capacity, uint8_t packet_type,
                                               uint64_t sequence, const uint8_t * session_nonce,
                                               const uint8_t * data, int data_bytes,
                                               const uint8_t * client_ephemeral, const uint8_t * key )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int bytes = rocketnet_write_sealed_packet( packet, packet_type, rocketnet_test_filter_value,
                                                     sequence, session_nonce, data, data_bytes,
                                                     ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                     client_ephemeral, key );
    if ( bytes <= 0 )
        return -1;
    const int header_bytes = ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES +
                             ( session_nonce ? ROCKETNET_SESSION_NONCE_BYTES : 0 );
    const int sealed_bytes = bytes - header_bytes;
    if ( sealed_bytes < 0 || capacity < sealed_bytes )
        return -1;
    memcpy( out, packet + header_bytes, (size_t) sealed_bytes );
    return sealed_bytes;
}

static int standard_packet_protection_tamper( const char * id, uint8_t * out, int capacity )
{
    if ( standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_CHALLENGE_PACKET ) != ROCKETNET_AD_WITH_EPHEMERAL_BYTES )
        return -1;
    if ( strcmp( id, "t9.version-tampered" ) == 0 )
        out[0] ^= 0x01;
    else if ( strcmp( id, "t9.protocol-id-tampered" ) == 0 )
        out[4] ^= 0x01;
    else if ( strcmp( id, "t9.packet-type-tampered" ) == 0 )
        out[12] = ROCKETNET_CONNECTION_RESPONSE_PACKET;
    else if ( strcmp( id, "t9.connection-salt-tampered" ) == 0 )
        out[13] ^= 0x01;
    else if ( strcmp( id, "t9.client-ephemeral-tampered" ) == 0 )
        out[21] ^= 0x01;
    else
        return -1;
    return ROCKETNET_AD_WITH_EPHEMERAL_BYTES;
}

static int standard_packet_protection_generate( const char * id, uint8_t * out, int capacity )
{
    const uint8_t response_plaintext[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };

    if ( strcmp( id, "t9.challenge-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_CHALLENGE_PACKET );
    if ( strcmp( id, "t9.denied-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_DENIED_PACKET );
    if ( strcmp( id, "t9.response-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_RESPONSE_PACKET );
    if ( strcmp( id, "t9.keepalive-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );
    if ( strcmp( id, "t9.payload-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    if ( strcmp( id, "t9.disconnect-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_DISCONNECT_PACKET );
    if ( strcmp( id, "t9.ack-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_ACK_PACKET );
    if ( strcmp( id, "t9.fragment-ad" ) == 0 )
        return standard_packet_protection_ad( out, capacity, ROCKETNET_CONNECTION_FRAGMENT_PACKET );

    if ( strcmp( id, "t9.challenge-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_CHALLENGE_PACKET, 1,
                                                   rocketnet_test_session_nonce, rocketnet_test_server_eph_public,
                                                   ROCKETNET_KEY_BYTES, rocketnet_test_client_eph_public,
                                                   rocketnet_test_handshake_s2c_key );
    if ( strcmp( id, "t9.denied-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_DENIED_PACKET, 2,
                                                   rocketnet_test_session_nonce, NULL, 0, NULL,
                                                   rocketnet_test_handshake_s2c_key );
    if ( strcmp( id, "t9.response-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_RESPONSE_PACKET, 1,
                                                   NULL, response_plaintext, ROCKETNET_RESPONSE_PADDING_BYTES,
                                                   rocketnet_test_client_eph_public, rocketnet_test_handshake_c2s_key );
    if ( strcmp( id, "t9.keepalive-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1,
                                                   NULL, rocketnet_test_keepalive_plaintext,
                                                   (int) sizeof( rocketnet_test_keepalive_plaintext ), NULL,
                                                   rocketnet_test_session_s2c_key );
    if ( strcmp( id, "t9.payload-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3,
                                                   NULL, rocketnet_test_payload_plaintext,
                                                   (int) sizeof( rocketnet_test_payload_plaintext ), NULL,
                                                   rocketnet_test_session_c2s_key );
    if ( strcmp( id, "t9.disconnect-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_DISCONNECT_PACKET, 4,
                                                   NULL, rocketnet_test_disconnect_plaintext,
                                                   (int) sizeof( rocketnet_test_disconnect_plaintext ), NULL,
                                                   rocketnet_test_session_c2s_key );
    if ( strcmp( id, "t9.ack-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_ACK_PACKET, 5,
                                                   NULL, rocketnet_test_ack_plaintext,
                                                   (int) sizeof( rocketnet_test_ack_plaintext ), NULL,
                                                   rocketnet_test_session_c2s_key );
    if ( strcmp( id, "t9.fragment-sealed" ) == 0 )
        return standard_packet_protection_sealed( out, capacity, ROCKETNET_CONNECTION_FRAGMENT_PACKET, 6,
                                                   NULL, rocketnet_test_fragment_plaintext,
                                                   (int) sizeof( rocketnet_test_fragment_plaintext ), NULL,
                                                   rocketnet_test_session_c2s_key );

    return standard_packet_protection_tamper( id, out, capacity );
}

static const char * const standard_packet_protection_tamper_ids[] =
{
    "t9.version-tampered",
    "t9.protocol-id-tampered",
    "t9.packet-type-tampered",
    "t9.connection-salt-tampered",
    "t9.client-ephemeral-tampered",
};

static void standard_packet_protection_opens()
{
    uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
    memset( nonce, 0, 4 );
    uint8_t * p = nonce + 4;
    rocketnet_write_uint64( &p, 1 );

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    unsigned long long opened_bytes = 0;

    rocketnet_unit_check( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_bytes, NULL,
                                                                     rocketnet_test_challenge_sealed,
                                                                     sizeof( rocketnet_test_challenge_sealed ),
                                                                     rocketnet_test_challenge_ad,
                                                                     sizeof( rocketnet_test_challenge_ad ),
                                                                     nonce, rocketnet_test_handshake_s2c_key ) == 0 );
    rocketnet_unit_check( opened_bytes == ROCKETNET_KEY_BYTES );
    rocketnet_unit_check( memcmp( opened, rocketnet_test_server_eph_public, ROCKETNET_KEY_BYTES ) == 0 );

    const int tamper_count = (int) ( sizeof( standard_packet_protection_tamper_ids ) /
                                     sizeof( standard_packet_protection_tamper_ids[0] ) );
    for ( int i = 0; i < tamper_count; i++ )
    {
        uint8_t tampered_ad[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
        const int tampered_bytes = standard_packet_protection_generate( standard_packet_protection_tamper_ids[i],
                                                                        tampered_ad, (int) sizeof( tampered_ad ) );
        rocketnet_unit_check( tampered_bytes == ROCKETNET_AD_WITH_EPHEMERAL_BYTES );
        rocketnet_unit_check( rn_standard_first_difference( tampered_ad, tampered_bytes,
                                                            rocketnet_test_challenge_ad,
                                                            ROCKETNET_AD_WITH_EPHEMERAL_BYTES ) >= 0 );

        opened_bytes = 0;
        rocketnet_unit_check( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_bytes, NULL,
                                                                         rocketnet_test_challenge_sealed,
                                                                         sizeof( rocketnet_test_challenge_sealed ),
                                                                         tampered_ad, (unsigned long long) tampered_bytes,
                                                                         nonce, rocketnet_test_handshake_s2c_key ) != 0 );
    }
}

static void test_standard_packet_protection_vectors()
{
    const int count = (int) ( sizeof( standard_packet_protection_table ) /
                              sizeof( standard_packet_protection_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_packet_protection_table, count,
                                                    standard_packet_protection_generate ) == count );

    standard_packet_protection_opens();
}

static void test_open_sealed_packet_refuses_plaintext_over_capacity()
{
    uint8_t plaintext[64];
    for ( int i = 0; i < (int) sizeof( plaintext ); i++ )
        plaintext[i] = (uint8_t) i;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int packet_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                            rocketnet_test_filter_value, 1, NULL,
                                                            plaintext, (int) sizeof( plaintext ),
                                                            ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                            NULL, rocketnet_test_session_s2c_key );
    rocketnet_unit_check( packet_bytes > 0 );

    uint8_t sentinel[64];
    memset( sentinel, 0xA5, sizeof( sentinel ) );

    uint8_t opened[64];
    memcpy( opened, sentinel, sizeof( opened ) );
    int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, 63, packet, packet_bytes,
                                                     ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1,
                                                     ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                     NULL, rocketnet_test_session_s2c_key );
    rocketnet_unit_check( opened_bytes == -1 );
    rocketnet_unit_check( memcmp( opened, sentinel, sizeof( opened ) ) == 0 );

    opened_bytes = rocketnet_open_sealed_packet( opened, NULL, 64, packet, packet_bytes,
                                                 ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1,
                                                 ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                 NULL, rocketnet_test_session_s2c_key );
    rocketnet_unit_check( opened_bytes == 64 );
    rocketnet_unit_check( memcmp( opened, plaintext, sizeof( plaintext ) ) == 0 );
}

REGISTER_UNIT_TEST( test_standard_packet_protection_vectors );
REGISTER_UNIT_TEST( test_open_sealed_packet_refuses_plaintext_over_capacity );
