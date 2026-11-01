/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// THE BIG-ENDIAN WIRE CHECK, the big-endian leg of the wire fuzz.
//
// Every byte-order-sensitive surface rocketnet has, driven end to end, with
// NO SOCKETS ANYWHERE. That constraint is the whole design of this file and
// it is not squeamishness: the leg runs under qemu-user, where the socket
// fast paths are the EMULATOR's behaviour and not the platform's -- a
// UDP_SEGMENT burst is not segmented, and the portable rung's receive loop
// stalls -- so a suite run there would be reporting on qemu. What this file
// touches is arithmetic and memory, which qemu reproduces exactly, on real
// s390x instructions against real big-endian memory. The rest is
// the full suite on real big-endian hardware.
//
// What it covers, and the defect each part was written against:
//
//   the endian helpers      rocketnet_endian_fix(uint64_t*) swapped
//                           the POINTER. Uncalled by the sdk and by the
//                           suite, so a no-op on every machine we own.
//   address parsing         inet_pton's network-order result read by
//                           host arithmetic, reversing every address.
//   the byte-order helpers  ntohs/htons swapping unconditionally.
//   the wire read/write     the standard's little-endian encoding, by shift
//                           and mask, which must be identical on both hosts.
//   the sealed packet       construction and opening, in memory: the
//                           associated data, the sequence, the AEAD, the
//                           clear header bytes.
//   the parsers             prefilter, base64, the config value parses.
//
// It exits nonzero, loudly, naming the first check that failed.

#include "rocketnet_internal.h"
#include "rocketnet_read_write.h"
#include "rocketnet_address.h"
#include "rocketnet_base64.h"
#include "rocketnet_protocol.h"
#include "rocketnet_config.h"
#include "rocketnet_platform.h"
#include "rocketnet_connect_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK( condition )                                                              \
do                                                                                      \
{                                                                                       \
    checks++;                                                                           \
    if ( !( condition ) )                                                               \
    {                                                                                   \
        printf( "FAILED: %s (line %d)\n", #condition, __LINE__ );                        \
        fflush( stdout );                                                               \
        failures++;                                                                     \
    }                                                                                   \
}                                                                                       \
while ( 0 )

// the standard's wire order, little-endian, as literal bytes: every value
// below is checked against THESE, never against a host integer, so each
// check means the same thing on either endianness

static const uint8_t wire64[8] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };

static void check_endian_helpers()
{
    uint16_t v16 = 0;
    memcpy( &v16, wire64, 2 );
    rocketnet_endian_fix( &v16 );
    CHECK( v16 == 0x7788 );

    uint32_t v32 = 0;
    memcpy( &v32, wire64, 4 );
    rocketnet_endian_fix( &v32 );
    CHECK( v32 == 0x55667788 );

    uint64_t v64 = 0;
    memcpy( &v64, wire64, 8 );
    rocketnet_endian_fix( &v64 );
    CHECK( v64 == 0x1122334455667788ULL );

    // its own inverse: back out to the wire, in the standard's order

    rocketnet_endian_fix( &v64 );

    uint8_t out[8];
    memcpy( out, &v64, 8 );
    CHECK( memcmp( out, wire64, 8 ) == 0 );

#if ROCKETNET_BIG_ENDIAN

    // THE ASSERTION THAT MAKES THIS LEG A GATE. Everything above passes on
    // a machine where all three overloads do nothing, which is exactly the
    // little-endian case. Here the helper must MOVE BYTES.

    uint16_t swap16 = 0x1122;
    rocketnet_endian_fix( &swap16 );
    CHECK( swap16 == 0x2211 );

    uint32_t swap32 = 0x11223344;
    rocketnet_endian_fix( &swap32 );
    CHECK( swap32 == 0x44332211 );

    uint64_t swap64 = 0x1122334455667788ULL;
    rocketnet_endian_fix( &swap64 );
    CHECK( swap64 == 0x8877665544332211ULL );

    // and the platform's network-order helpers are the IDENTITY here,
    // because network order is host order on this machine

    CHECK( rocketnet_platform_htons( 0x1234 ) == 0x1234 );
    CHECK( rocketnet_platform_ntohs( 0x1234 ) == 0x1234 );

    printf( "  big-endian host: the endian path executed\n" );

#else // #if ROCKETNET_BIG_ENDIAN

    CHECK( rocketnet_platform_htons( 0x1234 ) == 0x3412 );
    CHECK( rocketnet_platform_ntohs( 0x1234 ) == 0x3412 );

    printf( "  little-endian host: the endian helpers are no-ops here, by design\n" );

#endif // #if ROCKETNET_BIG_ENDIAN
}

static void check_read_write()
{
    uint8_t buffer[64];
    uint8_t * w = buffer;
    rocketnet_write_uint8( &w, 0xA5 );
    rocketnet_write_uint16( &w, 0xBEEF );
    rocketnet_write_uint32( &w, 0x12345678 );
    rocketnet_write_uint64( &w, 0x1122334455667788ULL );
    CHECK( w - buffer == 15 );

    // the bytes themselves, in the standard's order

    const uint8_t expected[15] = { 0xA5, 0xEF, 0xBE, 0x78, 0x56, 0x34, 0x12,
                                   0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    CHECK( memcmp( buffer, expected, 15 ) == 0 );

    const uint8_t * r = buffer;
    CHECK( rocketnet_read_uint8( &r ) == 0xA5 );
    CHECK( rocketnet_read_uint16( &r ) == 0xBEEF );
    CHECK( rocketnet_read_uint32( &r ) == 0x12345678 );
    CHECK( rocketnet_read_uint64( &r ) == 0x1122334455667788ULL );

    // STANDARD.md Appendix A's associated data prefix: version then
    // protocol id, little-endian, exactly these bytes

    uint8_t ad[12];
    uint8_t * a = ad;
    rocketnet_write_uint32( &a, ROCKETNET_WIRE_VERSION );
    rocketnet_write_uint64( &a, 0x1122334455667788ULL );
    const uint8_t expected_ad[12] = { 0x93, 0x1a, 0x7c, 0x4e,
                                      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    CHECK( memcmp( ad, expected_ad, 12 ) == 0 );
}

static void check_addresses()
{
    struct rocketnet_address_t address;

    // the octets, in the order they were written

    CHECK( rocketnet_address_parse( &address, "107.77.207.77" ) );
    CHECK( address.type == ROCKETNET_ADDRESS_IPV4 );
    CHECK( address.data.ipv4[0] == 107 );
    CHECK( address.data.ipv4[1] == 77 );
    CHECK( address.data.ipv4[2] == 207 );
    CHECK( address.data.ipv4[3] == 77 );
    CHECK( address.port == 0 );

    CHECK( rocketnet_address_parse( &address, "127.0.0.1:40000" ) );
    CHECK( address.data.ipv4[0] == 127 && address.data.ipv4[3] == 1 );
    CHECK( address.port == 40000 );

    // and back to a string, which is where a wrong order would have been
    // visible to a human all along

    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    CHECK( strcmp( rocketnet_address_to_string( &address, buffer ), "127.0.0.1:40000" ) == 0 );

    // the ipv6 groups are host-order uint16 after the parse

    CHECK( rocketnet_address_parse( &address, "[fe80::1]:50000" ) );
    CHECK( address.type == ROCKETNET_ADDRESS_IPV6 );
    CHECK( address.data.ipv6[0] == 0xfe80 );
    CHECK( address.data.ipv6[7] == 0x0001 );
    CHECK( address.port == 50000 );

    // the ipv4-in-ipv6 conversions, both directions

    CHECK( rocketnet_address_parse( &address, "10.20.30.40" ) );
    rocketnet_address_convert_ipv4_to_ipv6( &address );
    CHECK( address.data.ipv6[5] == 0xFFFF );
    CHECK( address.data.ipv6[6] == 0x0A14 );
    CHECK( address.data.ipv6[7] == 0x1E28 );
    CHECK( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    rocketnet_address_convert_ipv6_to_ipv4( &address );
    CHECK( address.data.ipv4[0] == 10 && address.data.ipv4[1] == 20 &&
           address.data.ipv4[2] == 30 && address.data.ipv4[3] == 40 );

    // the loader's convention, stated: a network-order uint32 lands as the
    // dotted octets in order

    uint8_t network_order[4] = { 192, 168, 1, 250 };
    uint32_t big_endian_ipv4;
    memcpy( &big_endian_ipv4, network_order, 4 );
    rocketnet_address_load_ipv4( &address, big_endian_ipv4, 0 );
    CHECK( address.data.ipv4[0] == 192 && address.data.ipv4[3] == 250 );
    CHECK( rocketnet_address_ipv4( &address ) == big_endian_ipv4 );
}

static void check_prefilter()
{
    const uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    memset( packet, 0, sizeof( packet ) );

    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, packet_filter );
    CHECK( p - packet == ROCKETNET_PREFILTER_BYTES );

    // the header's bytes: type, epoch, then the value

    CHECK( packet[0] == ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    CHECK( packet[1] == ROCKETNET_FILTER_EPOCH_UNKEYED );
    CHECK( memcmp( packet + 2, packet_filter, ROCKETNET_FILTER_VALUE_BYTES ) == 0 );

    CHECK( rocketnet_prefilter_check( packet, 64, packet_filter ) == ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    CHECK( rocketnet_prefilter_check( packet, ROCKETNET_PREFILTER_BYTES - 1, packet_filter ) == ROCKETNET_PREFILTER_TOO_SHORT );
    CHECK( rocketnet_prefilter_check( packet, ROCKETNET_RECEIVE_CEILING + 1, packet_filter ) == ROCKETNET_PREFILTER_TOO_LARGE );

    packet[1] = 1;
    CHECK( rocketnet_prefilter_check( packet, 64, packet_filter ) == ROCKETNET_PREFILTER_WRONG_EPOCH );
    packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;
    packet[2] ^= 0xFF;
    CHECK( rocketnet_prefilter_check( packet, 64, packet_filter ) == ROCKETNET_PREFILTER_WRONG_VALUE );
}

static void check_sealed_packet()
{
    // a session packet sealed and opened in memory: the associated data,
    // the sequence on the wire, the AEAD, the clear header. Sockets are not
    // involved and neither is any transport.

    const uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    uint8_t key[ROCKETNET_KEY_BYTES];

    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        key[i] = (uint8_t) ( i * 7 + 1 );
    }

    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];

    for ( int i = 0; i < ROCKETNET_CONNECTION_SALT_BYTES; i++ )
    {
        salt[i] = (uint8_t) ( 0xA0 + i );
    }

    const uint64_t protocol_id = 0x1122334455667788ULL;
    const uint64_t sequence = 0x0000000012345678ULL;

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 16];
    for ( size_t i = 0; i < sizeof( plaintext ); i++ )
    {
        plaintext[i] = (uint8_t) ( i * 3 );
    }

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                      packet_filter, sequence, NULL,
                                                      plaintext, (int) sizeof( plaintext ),
                                                      protocol_id, salt, NULL, key );
    CHECK( packet_bytes == (int) sizeof( plaintext ) + ROCKETNET_PACKET_OVERHEAD_BYTES );

    // the clear header, byte for byte: the prefilter, then the low 32 bits
    // of the sequence in the standard's little-endian order

    CHECK( packet[0] == ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    CHECK( memcmp( packet + 2, packet_filter, ROCKETNET_FILTER_VALUE_BYTES ) == 0 );
    CHECK( packet[6] == 0x78 && packet[7] == 0x56 && packet[8] == 0x34 && packet[9] == 0x12 );

    CHECK( rocketnet_prefilter_check( packet, packet_bytes, packet_filter ) == ROCKETNET_CONNECTION_PAYLOAD_PACKET );

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, packet_bytes,
                                                     ROCKETNET_CONNECTION_PAYLOAD_PACKET, sequence,
                                                     protocol_id, salt, NULL, key );
    CHECK( opened_bytes == (int) sizeof( plaintext ) );
    CHECK( opened_bytes > 0 && memcmp( opened, plaintext, sizeof( plaintext ) ) == 0 );

    // a flipped byte anywhere inside must refuse: the seal is over the
    // whole packet, on this host as on any other

    packet[packet_bytes / 2] ^= 0x01;
    CHECK( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, packet_bytes,
                                         ROCKETNET_CONNECTION_PAYLOAD_PACKET, sequence,
                                         protocol_id, salt, NULL, key ) < 0 );

    // the associated data lengths the standard publishes

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    CHECK( rocketnet_write_packet_associated_data( associated_data, protocol_id,
                                                   ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                   salt, NULL ) == ROCKETNET_AD_BYTES );
    CHECK( associated_data[0] == 0x93 && associated_data[1] == 0x1a &&
           associated_data[2] == 0x7c && associated_data[3] == 0x4e );
}

static void check_base64_and_config()
{
    uint8_t bytes[64];
    CHECK( rocketnet_base64_decode_data( "AAECAw==", bytes, sizeof( bytes ) ) == 4 );
    CHECK( bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x02 && bytes[3] == 0x03 );
    CHECK( rocketnet_base64_decode_data( "AA!A", bytes, sizeof( bytes ) ) == 0 );

    uint8_t raw[32];

    for ( int i = 0; i < 32; i++ )
    {
        raw[i] = (uint8_t) ( i * 5 + 3 );
    }

    char encoded[128];
    CHECK( rocketnet_base64_encode_data( raw, sizeof( raw ), encoded, sizeof( encoded ) ) > 0 );

    uint8_t decoded[64];
    CHECK( rocketnet_base64_decode_data( encoded, decoded, sizeof( decoded ) ) == 32 );
    CHECK( memcmp( raw, decoded, 32 ) == 0 );

    int value = 0;
    CHECK( rocketnet_config_parse_int_value( "1232", &value ) && value == 1232 );
    CHECK( !rocketnet_config_parse_int_value( "12x", &value ) );

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    struct rocketnet_transport_ladder_t ladder;
    CHECK( rocketnet_transport_ladder_parse( "auto", available, num_available, &ladder ) );

    int auto_available = 0;

    for ( int i = 0; i < num_available; i++ )
        if ( !rocketnet_transport_pin_only( available[i] ) )
        {
            auto_available++;
        }

    CHECK( ladder.num_candidates == auto_available );
    CHECK( rocketnet_transport_ladder_parse( "portable", available, num_available, &ladder ) );
    CHECK( ladder.num_candidates == 1 && ladder.candidates[0] == ROCKETNET_TRANSPORT_PORTABLE );
    CHECK( !rocketnet_transport_ladder_parse( "rocket", available, num_available, &ladder ) );
}

int main()
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    printf( "\nbig-endian wire check (%s):\n\n",
            ROCKETNET_BIG_ENDIAN ? "big-endian host" : "little-endian host" );

    check_endian_helpers();
    check_read_write();
    check_addresses();
    check_prefilter();
    check_sealed_packet();
    check_base64_and_config();

    rocketnet_term();

    printf( "\n%d checks, %d failed\n\n", checks, failures );
    fflush( stdout );

    return failures == 0 ? 0 : 1;
}
