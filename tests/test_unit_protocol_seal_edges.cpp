/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The sealed packet writers and the opener at their own refusals
// (sdk/rocketnet_protocol.cpp). Every caller refuses these inputs first, so
// each check here is the second guard; these cases hold it on its own.
// Sequence 0 is never sealed or opened (standard/TRANSPORT.md: sequences start
// at 1): a packet sealed by hand under sequence 0, byte for byte as the
// writer would seal it, is refused by the opener. Pure functions: no time,
// no socket.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_read_write.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_address.h"

#include <sodium.h>
#include <string.h>

#define check rocketnet_unit_check

static const uint64_t seal_edges_protocol = 0x5EA1ED9E5ULL;
static const uint8_t seal_edges_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 1, 2, 3, 4 };
static const uint8_t seal_edges_salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 9, 8, 7, 6, 5, 4, 3, 2 };

static void seal_edges_key( uint8_t * key )
{
    for ( int i = 0; i < 32; i++ )
    {
        key[i] = (uint8_t) ( 0x40 + i );
    }
}

// the writer's seal, done by hand for any sequence (0 included): header,
// the sequence's low 32 bits, then the data sealed under the sequence nonce
static int seal_edges_by_hand( uint8_t * packet, uint8_t type, uint64_t sequence, const uint8_t * data, int data_bytes,
                               const uint8_t * key )
{
    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, type, seal_edges_filter );
    rocketnet_write_uint32( &p, (uint32_t) ( sequence & 0xFFFFFFFF ) );

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    const int associated_data_bytes = rocketnet_write_packet_associated_data( associated_data, seal_edges_protocol, type, seal_edges_salt, NULL );

    uint8_t nonce[12];
    memset( nonce, 0, sizeof( nonce ) );
    uint8_t * n = nonce + 4;
    rocketnet_write_uint64( &n, sequence );

    unsigned long long sealed_bytes = 0;
    check( crypto_aead_chacha20poly1305_ietf_encrypt( p, &sealed_bytes, data, (unsigned long long) data_bytes, associated_data,
                                                      (unsigned long long) associated_data_bytes, NULL, nonce, key ) == 0 );
    return (int) ( p - packet ) + (int) sealed_bytes;
}

static void test_protocol_open_refuses_sequence_zero()
{
    uint8_t key[32];
    seal_edges_key( key );
    uint8_t data[16];
    memset( data, 0x3C, sizeof( data ) );
    const uint8_t type = ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET;

    // the hand seal is the writer's seal: byte-identical at sequence 1

    uint8_t written[128];
    uint8_t by_hand[128];
    const int written_bytes = rocketnet_write_sealed_packet( written, type, seal_edges_filter, 1, NULL, data, (int) sizeof( data ),
                                                             seal_edges_protocol, seal_edges_salt, NULL, key );
    check( written_bytes > 0 );
    check( seal_edges_by_hand( by_hand, type, 1, data, (int) sizeof( data ), key ) == written_bytes );
    check( memcmp( written, by_hand, (size_t) written_bytes ) == 0 );

    uint8_t opened[64];
    check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), by_hand, written_bytes, type, 1, seal_edges_protocol,
                                         seal_edges_salt, NULL, key ) == (int) sizeof( data ) );
    check( memcmp( opened, data, sizeof( data ) ) == 0 );

    // the writer never seals sequence 0; sealed by hand, the opener refuses it

    check( rocketnet_write_sealed_packet( written, type, seal_edges_filter, 0, NULL, data, (int) sizeof( data ), seal_edges_protocol,
                                          seal_edges_salt, NULL, key ) == 0 );
    const int zero_bytes = seal_edges_by_hand( by_hand, type, 0, data, (int) sizeof( data ), key );
    check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), by_hand, zero_bytes, type, 0, seal_edges_protocol,
                                         seal_edges_salt, NULL, key ) == -1 );

    // a packet one byte short of a header and a tag is refused before any
    // open, whatever its bytes; at exactly that size it opens to 0 bytes

    const int empty_bytes = rocketnet_write_sealed_packet( written, type, seal_edges_filter, 2, NULL, NULL, 0, seal_edges_protocol,
                                                           seal_edges_salt, NULL, key );
    check( empty_bytes == ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + ROCKETNET_PACKET_TAG_BYTES );
    check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), written, empty_bytes, type, 2, seal_edges_protocol,
                                         seal_edges_salt, NULL, key ) == 0 );
    check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), written, empty_bytes - 1, type, 2, seal_edges_protocol,
                                         seal_edges_salt, NULL, key ) == -1 );
}

REGISTER_UNIT_TEST( test_protocol_open_refuses_sequence_zero );

// the general writer's data arguments: a length with no data, and data with
// no length, are refused; 1 byte with data and 0 with none are sealed

static void test_protocol_seal_refuses_data_without_length()
{
    uint8_t key[32];
    seal_edges_key( key );
    uint8_t data[4] = { 1, 2, 3, 4 };
    uint8_t packet[128];
    const uint8_t type = ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET;

    check( rocketnet_write_sealed_packet( packet, type, seal_edges_filter, 1, NULL, NULL, 1, seal_edges_protocol, seal_edges_salt, NULL, key ) == 0 );
    check( rocketnet_write_sealed_packet( packet, type, seal_edges_filter, 1, NULL, NULL, 4, seal_edges_protocol, seal_edges_salt, NULL, key ) == 0 );
    check( rocketnet_write_sealed_packet( packet, type, seal_edges_filter, 1, NULL, data, 0, seal_edges_protocol, seal_edges_salt, NULL, key ) == 0 );
    check( rocketnet_write_sealed_packet( packet, type, seal_edges_filter, 1, NULL, data, -1, seal_edges_protocol, seal_edges_salt, NULL, key ) == 0 );
    check( rocketnet_write_sealed_packet( packet, type, seal_edges_filter, 1, NULL, data, 1, seal_edges_protocol, seal_edges_salt, NULL, key ) > 0 );
    check( rocketnet_write_sealed_packet( packet, type, seal_edges_filter, 1, NULL, NULL, 0, seal_edges_protocol, seal_edges_salt, NULL, key ) > 0 );
}

REGISTER_UNIT_TEST( test_protocol_seal_refuses_data_without_length );

// the in-place payload seal: sequence 0, an ack block outside its bounds, an
// empty payload and one past the budget's room are refused; the largest
// payload the room holds is sealed

static void test_protocol_payload_seal_at_its_bounds()
{
    uint8_t key[32];
    seal_edges_key( key );
    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES + 1];
    memset( ack_block, 0, sizeof( ack_block ) );
    uint8_t wire[ROCKETNET_SEND_BUDGET + 64];

    const int room = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES;
    check( room > 1 );

    memset( wire, 0x77, sizeof( wire ) );
    check( rocketnet_write_sealed_payload_in_place( wire, seal_edges_filter, 0, ack_block, ROCKETNET_ACK_BLOCK_MIN_BYTES, 8,
                                                    seal_edges_protocol, seal_edges_salt, key ) == 0 );
    check( rocketnet_write_sealed_payload_in_place( wire, seal_edges_filter, 1, ack_block, ROCKETNET_ACK_BLOCK_MIN_BYTES - 1, 8,
                                                    seal_edges_protocol, seal_edges_salt, key ) == 0 );
    check( rocketnet_write_sealed_payload_in_place( wire, seal_edges_filter, 1, ack_block, ROCKETNET_ACK_BLOCK_MAX_BYTES + 1, 8,
                                                    seal_edges_protocol, seal_edges_salt, key ) == 0 );
    check( rocketnet_write_sealed_payload_in_place( wire, seal_edges_filter, 1, ack_block, ROCKETNET_ACK_BLOCK_MIN_BYTES, 0,
                                                    seal_edges_protocol, seal_edges_salt, key ) == 0 );
    check( rocketnet_write_sealed_payload_in_place( wire, seal_edges_filter, 1, ack_block, ROCKETNET_ACK_BLOCK_MIN_BYTES, room + 1,
                                                    seal_edges_protocol, seal_edges_salt, key ) == 0 );

    check( rocketnet_write_sealed_payload_in_place( wire, seal_edges_filter, 1, ack_block, ROCKETNET_ACK_BLOCK_MIN_BYTES, 1,
                                                    seal_edges_protocol, seal_edges_salt, key ) ==
           ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + 1 );
    check( rocketnet_write_sealed_payload_in_place( wire, seal_edges_filter, 2, ack_block, ROCKETNET_ACK_BLOCK_MIN_BYTES, room,
                                                    seal_edges_protocol, seal_edges_salt, key ) == ROCKETNET_SEND_BUDGET );
}

REGISTER_UNIT_TEST( test_protocol_payload_seal_at_its_bounds );

// an address that is neither IPv4 nor IPv6 writes no bytes, so it has no
// cookie mac input and no cookie mac

static void test_protocol_cookie_mac_refuses_an_address_of_no_family()
{
    uint8_t key[ROCKETNET_COOKIE_KEY_BYTES];
    memset( key, 0x21, sizeof( key ) );
    struct rocketnet_address_t none;
    memset( &none, 0, sizeof( none ) );
    none.type = ROCKETNET_ADDRESS_NONE;

    uint8_t input[ROCKETNET_COOKIE_MAC_INPUT_IPV6_BYTES];
    memset( input, 0xEE, sizeof( input ) );
    check( rocketnet_write_cookie_mac_input( input, &none, seal_edges_salt, 1000 ) == 0 );

    uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES];
    memset( mac, 0xEE, sizeof( mac ) );
    check( rocketnet_generate_cookie_mac( mac, &none, seal_edges_salt, 1000, key ) == 0 );
    for ( int i = 0; i < ROCKETNET_COOKIE_MAC_BYTES; i++ )
    {
        check( mac[i] == 0xEE );
    }

    struct rocketnet_address_t v4;
    check( rocketnet_address_parse( &v4, "127.0.0.1:40000" ) );
    check( rocketnet_write_cookie_mac_input( input, &v4, seal_edges_salt, 1000 ) == ROCKETNET_ADDRESS_BYTES_IPV4 + ROCKETNET_CONNECTION_SALT_BYTES + 8 );
    check( rocketnet_generate_cookie_mac( mac, &v4, seal_edges_salt, 1000, key ) == 1 );
}

REGISTER_UNIT_TEST( test_protocol_cookie_mac_refuses_an_address_of_no_family );
