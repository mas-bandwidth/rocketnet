/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_packet_formats.cpp -- the assembled wire vectors of RN-1
// t8 (standard/TRANSPORT.md, Packet formats): one packet of every type 0 to
// 9, regenerated through the sdk code that writes it and compared byte for
// byte with the twin in the document. No socket, no thread, no clock.

#include "test_unit_standard.h"

#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_read_write.h"

static const rn_standard_vector_t standard_packet_formats_table[] = {
    RN_STANDARD_VECTOR( "t8.request",
                        "0000b75901000000000000000000000000000000000000000000000000003031"
                        "323334353637202122232425262728292a2b2c2d2e2f30313233343536373839"
                        "3a3b3c3d3e3f000102030405060708090a0b0c0d0e0f10111213141516171819"
                        "1a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373839"
                        "3a3b3c3d3e3f404142434445464748494a4b4c4d4e4f50515253545556575859"
                        "5a5b5c5d5e5f606162636465666768696a6b6c6d6e6f70717273747576777879"
                        "7a7b7c7d7e7f808182838485868788898a8b8c8d8e8f90919293949596979899"
                        "9a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9"
                        "babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9"
                        "dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9"
                        "fafbfcfdfeff000102030405060708090a0b0c0d0e0f10111213141516171819"
                        "1a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373839"
                        "3a3b3c3d3e3f404142434445464748494a4b4c4d4e4f50515253545556575859"
                        "5a5b5c5d5e5f606162636465666768696a6b6c6d6e6f70717273747576777879"
                        "7a7b7c7d7e7f808182838485868788898a8b8c8d8e8f90919293949596979899"
                        "9a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9"
                        "babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9"
                        "dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9"
                        "fafbfcfdfeff000102030405060708090a0b0c0d0e0f10111213141516171819"
                        "1a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373839"
                        "3a3b3c3d3e3ffffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6"
                        "e5e4e3e2e1e0dfdedddcdbdad9d8d7d6d5d4d3d2d1d0cfcecdcccbcac9c8c7c6"
                        "c5c4c3c2c1c0" ),
    RN_STANDARD_VECTOR( "t8.denied",
                        "0100b75901000100000040414243444546479cc7b0417c4d9352f9a198ef49ad"
                        "6fcb" ),
    RN_STANDARD_VECTOR( "t8.challenge",
                        "0200b7590100010000004041424344454647a350364e3a3a61a12a4c905dc639"
                        "c8a5491e7e8d4b7c585198cd4b944ee9d190f42ad8210fab19e41a3b96dc0608"
                        "3d31" ),
    RN_STANDARD_VECTOR( "t8.response",
                        "0300b759010001000000f60ffd9bd3d8bde8c38be5af01f3eba14755ea1a6aff"
                        "0c0471798cf74a64667dbcd6ea5f4ca5b997" ),
    RN_STANDARD_VECTOR( "t8.keep-alive",
                        "0400b75901000100000093f3ae3f054a38d041ec2f98387affbf5e314cdf500e"
                        "03e12804ec28001b99" ),
    RN_STANDARD_VECTOR( "t8.payload",
                        "0500b759010003000000d2ca227d0de19adc827dead44a38fd8426829a5db67b"
                        "80e585fe540c" ),
    RN_STANDARD_VECTOR( "t8.disconnect",
                        "0600b75901000400000017e737e8c4f6f2256fb776702543e959d309278570ed"
                        "91" ),
    RN_STANDARD_VECTOR( "t8.ack",
                        "0800b7590100050000001811d7e643542c6ea6b49ee8de56b33dba0d4f617e85"
                        "b5" ),
    RN_STANDARD_VECTOR( "t8.cookie",
                        "0700b759010030313233343536370807060504030201d0d1d2d3d4d5d6d7d8d9"
                        "dadbdcdddedf" ),
};

static const uint8_t standard_packet_formats_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0xb7, 0x59, 0x01, 0x00 };

static int standard_packet_formats_generate( const char * id, uint8_t * out, int capacity )
{
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    uint8_t client_ephemeral[ROCKETNET_KEY_BYTES];
    uint8_t server_ephemeral[ROCKETNET_KEY_BYTES];
    uint8_t handshake_c2s[ROCKETNET_KEY_BYTES];
    uint8_t handshake_s2c[ROCKETNET_KEY_BYTES];
    uint8_t session_c2s[ROCKETNET_KEY_BYTES];
    uint8_t session_s2c[ROCKETNET_KEY_BYTES];
    uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];

    for ( int i = 0; i < ROCKETNET_CONNECTION_SALT_BYTES; i++ )
    {
        salt[i] = (uint8_t) ( 0x30 + i );
        session_nonce[i] = (uint8_t) ( 0x40 + i );
    }

    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        client_ephemeral[i] = (uint8_t) ( 0x20 + i );
        server_ephemeral[i] = (uint8_t) ( 0x40 + i );
        handshake_c2s[i] = (uint8_t) ( 0x60 + i );
        handshake_s2c[i] = (uint8_t) ( 0x80 + i );
        session_c2s[i] = (uint8_t) ( 0xa0 + i );
        session_s2c[i] = (uint8_t) ( 0xc0 + i );
    }

    for ( int i = 0; i < ROCKETNET_SIGNED_CONNECT_DATA_BYTES; i++ )
    {
        signed_data[i] = (uint8_t) i;
    }

    for ( int i = 0; i < ROCKETNET_SIGNATURE_BYTES; i++ )
    {
        signature[i] = (uint8_t) ( 0xff - i );
    }

    for ( int i = 0; i < ROCKETNET_COOKIE_MAC_BYTES; i++ )
    {
        cookie_mac[i] = (uint8_t) ( 0xd0 + i );
    }

    if ( capacity < ROCKETNET_SEND_BUDGET )
        return -1;

    const uint64_t protocol_id = 0x1122334455667788ULL;

    if ( strcmp( id, "t8.request" ) == 0 )
    {
        rocketnet_write_connection_request_packet( out, standard_packet_formats_filter, NULL, salt,
                                                   client_ephemeral, signed_data, signature );
        return ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
    }

    if ( strcmp( id, "t8.denied" ) == 0 )
        return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_DENIED_PACKET, standard_packet_formats_filter,
                                              1, session_nonce, NULL, 0, protocol_id, salt, NULL, handshake_s2c );

    if ( strcmp( id, "t8.challenge" ) == 0 )
        return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_CHALLENGE_PACKET, standard_packet_formats_filter,
                                              1, session_nonce, server_ephemeral, ROCKETNET_KEY_BYTES,
                                              protocol_id, salt, client_ephemeral, handshake_s2c );

    if ( strcmp( id, "t8.response" ) == 0 )
    {
        const uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
        return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_RESPONSE_PACKET, standard_packet_formats_filter,
                                              1, NULL, padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                              protocol_id, salt, client_ephemeral, handshake_c2s );
    }

    uint8_t plaintext[ROCKETNET_SEND_BUDGET];
    for ( int i = 0; i < ROCKETNET_ACK_BLOCK_MIN_BYTES; i++ )
    {
        plaintext[i] = 0;
    }

    if ( strcmp( id, "t8.keep-alive" ) == 0 )
    {
        uint8_t * p = plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
        rocketnet_write_uint32( &p, 3 );
        rocketnet_write_uint32( &p, 1000 );
        return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, standard_packet_formats_filter,
                                              1, NULL, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + 8,
                                              protocol_id, salt, NULL, session_s2c );
    }

    if ( strcmp( id, "t8.payload" ) == 0 )
    {
        memcpy( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, "hello", 5 );
        return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_PAYLOAD_PACKET, standard_packet_formats_filter,
                                              3, NULL, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + 5,
                                              protocol_id, salt, NULL, session_c2s );
    }

    if ( strcmp( id, "t8.disconnect" ) == 0 )
        return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_DISCONNECT_PACKET, standard_packet_formats_filter,
                                              4, NULL, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES,
                                              protocol_id, salt, NULL, session_c2s );

    if ( strcmp( id, "t8.ack" ) == 0 )
        return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_ACK_PACKET, standard_packet_formats_filter,
                                              5, NULL, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES,
                                              protocol_id, salt, NULL, session_c2s );

    if ( strcmp( id, "t8.cookie" ) == 0 )
    {
        rocketnet_write_connection_cookie_packet( out, standard_packet_formats_filter, salt,
                                                  0x0102030405060708ULL, cookie_mac );
        return ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES;
    }

    return -1;
}

static void test_standard_packet_formats_vectors()
{
    const int count = (int) ( sizeof( standard_packet_formats_table ) / sizeof( standard_packet_formats_table[0] ) );
    rocketnet_unit_check( count == 9 );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_packet_formats_table, count,
                                                    standard_packet_formats_generate ) == count );

    // the generator refuses an id it does not know and a capacity too small
    // for the largest packet, so a missing twin cannot pass silently

    static uint8_t scratch[64];
    rocketnet_unit_check( standard_packet_formats_generate( "t8.no-such-vector", scratch, (int) sizeof( scratch ) ) == -1 );
    rocketnet_unit_check( standard_packet_formats_generate( "t8.request", scratch, (int) sizeof( scratch ) ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_packet_formats_vectors );
