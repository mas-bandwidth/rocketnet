/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// tools/connect_token_vectors/main.cpp -- mints the byte-stable 768-byte
// connect token vectors of standard/vectors/connect_token_v1.json from their fixed
// inputs, and writes them to the one path it is given:
//
//     ./bin/connect_token_vectors standard/vectors/connect_token_v1.json
//
// The write goes to <path>.tmp and is renamed over <path>, so a reader never
// sees half a file. Nothing else writes the file: the unit suite compiles the
// committed copy into ./bin/test and checks that two generations from this
// source are identical and equal to it byte for byte
// (tests/test_unit_connect_token_vectors.cpp, test_vectors_regenerate_byte_identical).
// The unit case compiles this file with ROCKETNET_CONNECT_TOKEN_VECTORS_NO_MAIN
// for the inputs and the generator, so the tool and the check mint from one
// source.
//
// rocketnet_write_connect_token seals the private block with crypto_box_seal,
// which draws a fresh ephemeral key, so its output can never be a vector. The
// vectors come from the test-only rocketnet_write_connect_token_deterministic:
// the same layout, the sealed box built from a caller-given ephemeral secret
// exactly as crypto_box_seal builds it (epk || crypto_box_easy, nonce =
// blake2b-192(epk || server_pk)). Every key is a fixed test key from
// sdk/rocketnet_test_vectors.h.

#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"

// Match the header to the sodium implementation linked into the SDK.
#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../../sodium/sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

#include <stdint.h>
#include <string.h>

#include <string>

// ---------------------------------------------------------------------------
// the vector inputs. integers are values; addresses are their wire bytes
// (ipv4: four octets then the port little-endian; ipv6: eight uint16 groups
// little-endian then the port little-endian), as the signed connect data
// carries them.

struct ctv_input_t
{
    const char * name;
    uint64_t client_id;
    uint64_t protocol_id;
    uint64_t create_timestamp;
    uint64_t expire_timestamp;
    int32_t timeout_seconds;
    uint8_t address_flags;
    uint8_t ipv4_address[6];
    uint8_t ipv6_address[18];
    int user_data_pattern;                      // see ctv_fill_user_data
    const uint8_t * client_to_server_key;
    const uint8_t * server_to_client_key;
    const uint8_t * ephemeral_secret;
};

static void ctv_fill_user_data( int pattern, uint8_t * user_data )
{
    static const char text[] = "rocketnet connect token vector user data ";

    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
    {
        switch ( pattern )
        {
            case 0: user_data[i] = 0; break;
            case 1: user_data[i] = (uint8_t) i; break;
            case 2: user_data[i] = 0xff; break;

            default: user_data[i] = (uint8_t) text[i % ( (int) sizeof( text ) - 1 )]; break;
        }
    }
}

static const struct ctv_input_t ctv_inputs[] = {
    // the appendix's token inputs, ipv4 127.0.0.1:40000, empty user data
    {
        "ipv4_appendix_inputs",
        ROCKETNET_TEST_CLIENT_ID,
        ROCKETNET_TEST_PROTOCOL_ID,
        ROCKETNET_TEST_CREATE_TS,
        ROCKETNET_TEST_EXPIRE_TS,
        ROCKETNET_TEST_TIMEOUT,
        ROCKETNET_ADDRESS_FLAG_IPV4,
        { 127, 0, 0, 1, 0x40, 0x9c },
        { 0 },
        0,
        rocketnet_test_token_c2s_key,
        rocketnet_test_token_s2c_key,
        rocketnet_test_token_vector_ephemeral_secret[0],
    },
    // ipv6 [::1]:50000, counting user data
    {
        "ipv6_loopback_counting_user_data",
        1ULL,
        0xfedcba9876543210ULL,
        0ULL,
        300ULL,
        5,
        ROCKETNET_ADDRESS_FLAG_IPV6,
        { 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0x50, 0xc3 },
        1,
        rocketnet_test_handshake_c2s_key,
        rocketnet_test_handshake_s2c_key,
        rocketnet_test_token_vector_ephemeral_secret[1],
    },
    // both families, 10.0.0.1:1 and [2001:db8::1]:65535, all-ones user data,
    // a negative timeout, which both ends refuse, to pin the int32 encoding
    {
        "dual_stack_negative_timeout",
        0x8000000000000001ULL,
        0x0000000000000001ULL,
        1756166400ULL,
        1756170000ULL,
        -1,
        ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6,
        { 10, 0, 0, 1, 0x01, 0x00 },
        { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0xff, 0xff },
        2,
        rocketnet_test_session_c2s_key,
        rocketnet_test_session_s2c_key,
        rocketnet_test_token_vector_ephemeral_secret[2],
    },
    // extremes: every integer at its top, text user data, the token keys swapped
    {
        "extreme_values_text_user_data",
        0xffffffffffffffffULL,
        0ULL,
        0x7fffffffffffffffULL,
        0xffffffffffffffffULL,
        0x7fffffff,
        ROCKETNET_ADDRESS_FLAG_IPV4,
        { 255, 255, 255, 255, 0xff, 0xff },
        { 0 },
        3,
        rocketnet_test_token_s2c_key,
        rocketnet_test_token_c2s_key,
        rocketnet_test_token_vector_ephemeral_secret[3],
    },
};

static const int ctv_input_count = (int) ( sizeof( ctv_inputs ) / sizeof( ctv_inputs[0] ) );

static void ctv_fields( const struct ctv_input_t * input,
                        struct rocketnet_signed_connect_data_t * signed_fields,
                        struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = input->protocol_id;
    signed_fields->create_timestamp = input->create_timestamp;
    signed_fields->expire_timestamp = input->expire_timestamp;
    signed_fields->timeout_seconds = input->timeout_seconds;
    signed_fields->address_flags = input->address_flags;
    memcpy( signed_fields->ipv4_address, input->ipv4_address, 6 );
    memcpy( signed_fields->ipv6_address, input->ipv6_address, 18 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( private_fields, 0, sizeof( *private_fields ) );
    private_fields->client_id = input->client_id;
    private_fields->protocol_id = input->protocol_id;
    private_fields->expire_timestamp = input->expire_timestamp;
    memcpy( private_fields->client_to_server_key, input->client_to_server_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields->server_to_client_key, input->server_to_client_key, ROCKETNET_KEY_BYTES );
    ctv_fill_user_data( input->user_data_pattern, private_fields->user_data );
}

static int ctv_mint( const struct ctv_input_t * input, uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    ctv_fields( input, &signed_fields, &private_fields );
    return rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields,
                                                        rocketnet_test_backend_secret_key,
                                                        input->ephemeral_secret, token );
}

// ---------------------------------------------------------------------------
// hex, and the file

static std::string ctv_hex( const uint8_t * bytes, int count )
{
    static const char digits[] = "0123456789abcdef";
    std::string out;

    for ( int i = 0; i < count; i++ )
    {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0xf];
    }

    return out;
}

static std::string ctv_hex_u64( uint64_t value )
{
    uint8_t bytes[8];

    for ( int i = 0; i < 8; i++ )
    {
        bytes[i] = (uint8_t) ( value >> ( 56 - 8 * i ) );
    }

    return ctv_hex( bytes, 8 );
}

static std::string ctv_hex_u32( uint32_t value )
{
    uint8_t bytes[4];

    for ( int i = 0; i < 4; i++ )
    {
        bytes[i] = (uint8_t) ( value >> ( 24 - 8 * i ) );
    }

    return ctv_hex( bytes, 4 );
}

static void ctv_field( std::string & out, const char * key, const std::string & value, bool last )
{
    out += "      \"";
    out += key;
    out += "\": \"";
    out += value;
    out += last ? "\"\n" : "\",\n";
}

// the whole file, minted from the inputs above. returns false, with out
// empty, when a key operation fails
static bool ctv_generate_json( std::string & out )
{
    out.clear();

    uint8_t backend_seed[32];
    if ( crypto_sign_ed25519_sk_to_seed( backend_seed, rocketnet_test_backend_secret_key ) != 0 )
        return false;

    out += "{\n";
    out += "  \"description\": \"RocketNet 768-byte connect token known answers, minted by rocketnet_write_connect_token_deterministic "
           "and regenerated only by the connect_token_vectors tool (tools/connect_token_vectors/main.cpp). Every field but description and name is lower-case hex. Integers are their value "
           "as fixed-width big-endian hex (u64: 16 digits; version: 8 digits; timeout_seconds: 8 digits, int32 two's complement). "
           "Byte fields are in wire order: ipv4_address is 4 octets then the port little-endian; ipv6_address is 8 uint16 groups "
           "little-endian then the port little-endian. The private block is sealed as crypto_box_seal does it, with ephemeral_secret as "
           "the ephemeral X25519 secret: epk = X25519(ephemeral_secret, basepoint), nonce = BLAKE2b-192(epk || server_public_key), "
           "sealed = epk || crypto_box_easy(private_block, nonce, server_public_key, ephemeral_secret). The token is signed with the "
           "Ed25519 key expanded from backend_signing_seed. All keys are the published test keys; never use them in a deployment.\",\n";
    out += "  \"version\": \"" + ctv_hex_u32( ROCKETNET_WIRE_VERSION ) + "\",\n";
    out += "  \"vectors\": [\n";

    for ( int v = 0; v < ctv_input_count; v++ )
    {
        const struct ctv_input_t * input = &ctv_inputs[v];
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
        ctv_fill_user_data( input->user_data_pattern, user_data );

        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        if ( ctv_mint( input, token ) != 1 )
        {
            sodium_memzero( backend_seed, sizeof( backend_seed ) );
            out.clear();
            return false;
        }

        uint8_t flags = input->address_flags;

        out += "    {\n";
        ctv_field( out, "name", input->name, false );
        ctv_field( out, "backend_signing_seed", ctv_hex( backend_seed, 32 ), false );
        ctv_field( out, "server_public_key", ctv_hex( rocketnet_test_server_public_key, 32 ), false );
        ctv_field( out, "ephemeral_secret", ctv_hex( input->ephemeral_secret, 32 ), false );
        ctv_field( out, "client_id", ctv_hex_u64( input->client_id ), false );
        ctv_field( out, "protocol_id", ctv_hex_u64( input->protocol_id ), false );
        ctv_field( out, "create_timestamp", ctv_hex_u64( input->create_timestamp ), false );
        ctv_field( out, "expire_timestamp", ctv_hex_u64( input->expire_timestamp ), false );
        ctv_field( out, "timeout_seconds", ctv_hex_u32( (uint32_t) input->timeout_seconds ), false );
        ctv_field( out, "address_flags", ctv_hex( &flags, 1 ), false );
        ctv_field( out, "ipv4_address", ctv_hex( input->ipv4_address, 6 ), false );
        ctv_field( out, "ipv6_address", ctv_hex( input->ipv6_address, 18 ), false );
        ctv_field( out, "user_data", ctv_hex( user_data, ROCKETNET_USER_DATA_BYTES ), false );
        ctv_field( out, "client_to_server_key", ctv_hex( input->client_to_server_key, 32 ), false );
        ctv_field( out, "server_to_client_key", ctv_hex( input->server_to_client_key, 32 ), false );
        ctv_field( out, "token", ctv_hex( token, ROCKETNET_CONNECT_TOKEN_BYTES ), true );
        out += ( v + 1 < ctv_input_count ) ? "    },\n" : "    }\n";
    }

    out += "  ]\n";
    out += "}\n";
    sodium_memzero( backend_seed, sizeof( backend_seed ) );

    return true;
}

#if !defined( ROCKETNET_CONNECT_TOKEN_VECTORS_NO_MAIN )

// ---------------------------------------------------------------------------
// the tool: write the generated file to the one path given, atomically

#include "../../tests/resource_file.h"

#include <stdio.h>

int main( int argc, char ** argv )
{
    if ( argc != 2 || argv[1][0] == '\0' )
    {
        fprintf( stderr, "usage: connect_token_vectors <output path>\n"
                         "writes the connect token known-answer vectors (standard/vectors/connect_token_v1.json) to <output path>\n" );
        return 2;
    }

    const std::string path = argv[1];

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "error: rocketnet_init failed\n" );
        return 1;
    }

    std::string generated;
    const bool minted = ctv_generate_json( generated );

    rocketnet_term();
    if ( !minted )
    {
        fprintf( stderr, "error: minting the vectors failed\n" );
        return 1;
    }

    // written beside the destination, then renamed over it, so a reader never
    // sees half a file

    const std::string temp = path + ".tmp";
    FILE * file = rocketnet_test_fopen( temp, "wb" );
    if ( !file )
    {
        fprintf( stderr, "error: cannot open %s for writing\n", temp.c_str() );
        return 1;
    }

    const bool written = fwrite( generated.data(), 1, generated.size(), file ) == generated.size();
    if ( fclose( file ) != 0 || !written )
    {
        fprintf( stderr, "error: writing %s failed\n", temp.c_str() );
        return 1;
    }
#if defined( _WIN32 )
    // the Windows CRT rename refuses to replace an existing destination

    const std::wstring wide_temp = rocketnet_test_wide_path( temp );
    const std::wstring wide_path = rocketnet_test_wide_path( path );
    if ( MoveFileExW( wide_temp.c_str(), wide_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) == 0 )
#else
    if ( rename( temp.c_str(), path.c_str() ) != 0 )
#endif // #if defined( _WIN32 )
    {
        fprintf( stderr, "error: renaming %s to %s failed\n", temp.c_str(), path.c_str() );
        return 1;
    }

    printf( "wrote %s: %d vectors, %d bytes\n", path.c_str(), ctv_input_count, (int) generated.size() );

    return 0;
}

#endif // !defined(ROCKETNET_CONNECT_TOKEN_VECTORS_NO_MAIN)
