/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_connect_token_vectors.cpp -- byte-stable 768-byte connect token
// vectors in tests/connect_token_vectors.json, the known answers another
// implementation of the backend's minting (a Go backend first) is checked
// against byte for byte.
//
// rocketnet_write_connect_token seals the private block with crypto_box_seal,
// which draws a fresh ephemeral key, so its output can never be a vector. The
// vectors come from the test-only rocketnet_write_connect_token_deterministic:
// the same layout, the sealed box built from a caller-given ephemeral secret
// exactly as crypto_box_seal builds it (epk || crypto_box_easy, nonce =
// blake2b-192(epk || server_pk)). Every key is a fixed test key from
// sdk/rocketnet_test_vectors.h.
//
// A normal run compares the committed vectors file in memory against what the
// inputs below mint; the file is rewritten only under the explicit regenerate
// flag ROCKETNET_REGENERATE_CONNECT_TOKEN_VECTORS=1. No case reads a clock,
// opens a socket or touches anything outside the repository's tests/ dir.

#include "test_unit_registry.h"

#include "rocketnet_connect_token.h"
#include "rocketnet_base64.h"
#include "rocketnet_test_vectors.h"
#include "wycheproof.h"
#include "resource_file.h"
#include "portable_crt.h"

// Match the header to the sodium implementation linked into the SDK.
#if defined(ROCKETNET_USE_SYSTEM_SODIUM)
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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
            case 0:  user_data[i] = 0;                                                  break;
            case 1:  user_data[i] = (uint8_t) i;                                        break;
            case 2:  user_data[i] = 0xff;                                               break;
            default: user_data[i] = (uint8_t) text[ i % ( (int) sizeof(text) - 1 ) ];   break;
        }
    }
}

static const struct ctv_input_t ctv_inputs[] =
{
    // the appendix's token inputs, ipv4 127.0.0.1:40000, empty user data
    {
        "ipv4_appendix_inputs",
        ROCKETNET_TEST_CLIENT_ID, ROCKETNET_TEST_PROTOCOL_ID,
        ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, ROCKETNET_TEST_TIMEOUT,
        ROCKETNET_ADDRESS_FLAG_IPV4,
        { 127, 0, 0, 1, 0x40, 0x9c },
        { 0 },
        0,
        rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
        rocketnet_test_token_vector_ephemeral_secret[0],
    },
    // ipv6 [::1]:50000, counting user data
    {
        "ipv6_loopback_counting_user_data",
        1ULL, 0xfedcba9876543210ULL,
        0ULL, 300ULL, 5,
        ROCKETNET_ADDRESS_FLAG_IPV6,
        { 0 },
        { 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 1,0, 0x50, 0xc3 },
        1,
        rocketnet_test_handshake_c2s_key, rocketnet_test_handshake_s2c_key,
        rocketnet_test_token_vector_ephemeral_secret[1],
    },
    // both families, 10.0.0.1:1 and [2001:db8::1]:65535, all-ones user data,
    // a negative timeout, which both ends refuse, to pin the int32 encoding
    {
        "dual_stack_negative_timeout",
        0x8000000000000001ULL, 0x0000000000000001ULL,
        1756166400ULL, 1756170000ULL, -1,
        ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6,
        { 10, 0, 0, 1, 0x01, 0x00 },
        { 0x01,0x20, 0xb8,0x0d, 0,0, 0,0, 0,0, 0,0, 0,0, 1,0, 0xff, 0xff },
        2,
        rocketnet_test_session_c2s_key, rocketnet_test_session_s2c_key,
        rocketnet_test_token_vector_ephemeral_secret[2],
    },
    // extremes: every integer at its top, text user data, the token keys swapped
    {
        "extreme_values_text_user_data",
        0xffffffffffffffffULL, 0ULL,
        0x7fffffffffffffffULL, 0xffffffffffffffffULL, 0x7fffffff,
        ROCKETNET_ADDRESS_FLAG_IPV4,
        { 255, 255, 255, 255, 0xff, 0xff },
        { 0 },
        3,
        rocketnet_test_token_s2c_key, rocketnet_test_token_c2s_key,
        rocketnet_test_token_vector_ephemeral_secret[3],
    },
};

static const int ctv_input_count = (int) ( sizeof(ctv_inputs) / sizeof(ctv_inputs[0]) );

static void ctv_fields( const struct ctv_input_t * input,
                        struct rocketnet_signed_connect_data_t * signed_fields,
                        struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof(*signed_fields) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = input->protocol_id;
    signed_fields->create_timestamp = input->create_timestamp;
    signed_fields->expire_timestamp = input->expire_timestamp;
    signed_fields->timeout_seconds = input->timeout_seconds;
    signed_fields->address_flags = input->address_flags;
    memcpy( signed_fields->ipv4_address, input->ipv4_address, 6 );
    memcpy( signed_fields->ipv6_address, input->ipv6_address, 18 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( private_fields, 0, sizeof(*private_fields) );
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
        bytes[i] = (uint8_t) ( value >> ( 56 - 8 * i ) );
    return ctv_hex( bytes, 8 );
}

static std::string ctv_hex_u32( uint32_t value )
{
    uint8_t bytes[4];
    for ( int i = 0; i < 4; i++ )
        bytes[i] = (uint8_t) ( value >> ( 24 - 8 * i ) );
    return ctv_hex( bytes, 4 );
}

static int ctv_unhex_digit( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    return -1;
}

// strict lower-case hex of exactly count bytes; 1 on success
static int ctv_unhex( const std::string & text, uint8_t * bytes, int count )
{
    if ( (int) text.size() != count * 2 )
        return 0;
    for ( int i = 0; i < count; i++ )
    {
        int hi = ctv_unhex_digit( text[(size_t) ( 2*i )] );
        int lo = ctv_unhex_digit( text[(size_t) ( 2*i+1 )] );
        if ( hi < 0 || lo < 0 )
            return 0;
        bytes[i] = (uint8_t) ( ( hi << 4 ) | lo );
    }
    return 1;
}

static uint64_t ctv_unhex_uint( const std::string & text, int count )
{
    uint8_t bytes[8];
    rocketnet_unit_check( count <= 8 );
    rocketnet_unit_check( ctv_unhex( text, bytes, count ) );
    uint64_t value = 0;
    for ( int i = 0; i < count; i++ )
        value = ( value << 8 ) | bytes[i];
    return value;
}

static void ctv_field( std::string & out, const char * key, const std::string & value, bool last )
{
    out += "      \"";
    out += key;
    out += "\": \"";
    out += value;
    out += last ? "\"\n" : "\",\n";
}

static std::string ctv_generate_json()
{
    uint8_t backend_seed[32];
    rocketnet_unit_check( crypto_sign_ed25519_sk_to_seed( backend_seed, rocketnet_test_backend_secret_key ) == 0 );

    std::string out;
    out += "{\n";
    out += "  \"description\": \"RocketNet 768-byte connect token known answers, minted by rocketnet_write_connect_token_deterministic "
           "and regenerated only with ROCKETNET_REGENERATE_CONNECT_TOKEN_VECTORS=1 (test_unit_connect_token_vectors.cpp). Every field but description and name is lower-case hex. Integers are their value "
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
        rocketnet_unit_check( ctv_mint( input, token ) == 1 );
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
    sodium_memzero( backend_seed, sizeof(backend_seed) );
    return out;
}

// the vectors file, beside this source: tests/connect_token_vectors.json in
// the repository, wherever the suite is run from
static std::string ctv_path()
{
    std::string source = __FILE__;
    size_t slash = source.find_last_of( "/\\" );
    std::string root = ( slash == std::string::npos ) ? std::string( "." ) : source.substr( 0, slash );
    return root + "/connect_token_vectors.json";
}

static int ctv_read_file( const std::string & path, std::string & out )
{
    out.clear();
    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( !file )
        return 0;
    char buffer[4096];
    size_t n;
    while ( ( n = fread( buffer, 1, sizeof(buffer), file ) ) > 0 )
        out.append( buffer, n );
    fclose( file );
    return 1;
}

// every string value of "key" in the file, in order of appearance
static std::vector<std::string> ctv_values( const std::string & text, const char * key )
{
    std::vector<std::string> values;
    std::string needle = std::string( "\"" ) + key + "\": \"";
    size_t at = 0;
    while ( ( at = text.find( needle, at ) ) != std::string::npos )
    {
        at += needle.size();
        size_t end = text.find( '"', at );
        rocketnet_unit_check( end != std::string::npos );
        values.push_back( text.substr( at, end - at ) );
        at = end + 1;
    }
    return values;
}

struct ctv_file_vector_t
{
    uint8_t backend_seed[32];
    uint8_t server_public_key[32];
    uint8_t ephemeral_secret[32];
    uint64_t client_id;
    uint64_t protocol_id;
    uint64_t create_timestamp;
    uint64_t expire_timestamp;
    int32_t timeout_seconds;
    uint8_t address_flags;
    uint8_t ipv4_address[6];
    uint8_t ipv6_address[18];
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    uint8_t client_to_server_key[32];
    uint8_t server_to_client_key[32];
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
};

static std::vector<ctv_file_vector_t> ctv_load()
{
    std::string text;
    rocketnet_unit_check( ctv_read_file( ctv_path(), text ) );

    std::vector<std::string> seed = ctv_values( text, "backend_signing_seed" );
    std::vector<std::string> server_pk = ctv_values( text, "server_public_key" );
    std::vector<std::string> esk = ctv_values( text, "ephemeral_secret" );
    std::vector<std::string> client_id = ctv_values( text, "client_id" );
    std::vector<std::string> protocol_id = ctv_values( text, "protocol_id" );
    std::vector<std::string> create = ctv_values( text, "create_timestamp" );
    std::vector<std::string> expire = ctv_values( text, "expire_timestamp" );
    std::vector<std::string> timeout = ctv_values( text, "timeout_seconds" );
    std::vector<std::string> flags = ctv_values( text, "address_flags" );
    std::vector<std::string> ipv4 = ctv_values( text, "ipv4_address" );
    std::vector<std::string> ipv6 = ctv_values( text, "ipv6_address" );
    std::vector<std::string> user_data = ctv_values( text, "user_data" );
    std::vector<std::string> c2s = ctv_values( text, "client_to_server_key" );
    std::vector<std::string> s2c = ctv_values( text, "server_to_client_key" );
    std::vector<std::string> token = ctv_values( text, "token" );

    size_t count = token.size();
    rocketnet_unit_check( count >= 4 );
    rocketnet_unit_check( seed.size() == count && server_pk.size() == count && esk.size() == count );
    rocketnet_unit_check( client_id.size() == count && protocol_id.size() == count );
    rocketnet_unit_check( create.size() == count && expire.size() == count && timeout.size() == count );
    rocketnet_unit_check( flags.size() == count && ipv4.size() == count && ipv6.size() == count );
    rocketnet_unit_check( user_data.size() == count && c2s.size() == count && s2c.size() == count );

    std::vector<ctv_file_vector_t> vectors( count );
    for ( size_t i = 0; i < count; i++ )
    {
        ctv_file_vector_t & v = vectors[i];
        rocketnet_unit_check( ctv_unhex( seed[i], v.backend_seed, 32 ) );
        rocketnet_unit_check( ctv_unhex( server_pk[i], v.server_public_key, 32 ) );
        rocketnet_unit_check( ctv_unhex( esk[i], v.ephemeral_secret, 32 ) );
        v.client_id = ctv_unhex_uint( client_id[i], 8 );
        v.protocol_id = ctv_unhex_uint( protocol_id[i], 8 );
        v.create_timestamp = ctv_unhex_uint( create[i], 8 );
        v.expire_timestamp = ctv_unhex_uint( expire[i], 8 );
        v.timeout_seconds = (int32_t) (uint32_t) ctv_unhex_uint( timeout[i], 4 );
        v.address_flags = (uint8_t) ctv_unhex_uint( flags[i], 1 );
        rocketnet_unit_check( ctv_unhex( ipv4[i], v.ipv4_address, 6 ) );
        rocketnet_unit_check( ctv_unhex( ipv6[i], v.ipv6_address, 18 ) );
        rocketnet_unit_check( ctv_unhex( user_data[i], v.user_data, ROCKETNET_USER_DATA_BYTES ) );
        rocketnet_unit_check( ctv_unhex( c2s[i], v.client_to_server_key, 32 ) );
        rocketnet_unit_check( ctv_unhex( s2c[i], v.server_to_client_key, 32 ) );
        rocketnet_unit_check( ctv_unhex( token[i], v.token, ROCKETNET_CONNECT_TOKEN_BYTES ) );
    }
    return vectors;
}

// ---------------------------------------------------------------------------
// the cases

// the file is exactly what the inputs above mint. a normal run reads the
// committed copy and compares it in memory; the file is rewritten (via a
// rename, so a reader never sees half a file) only when the explicit
// ROCKETNET_REGENERATE_CONNECT_TOKEN_VECTORS=1 flag is set, so a read-only
// checkout never fails
static void test_vectors_regenerate_byte_identical()
{
    std::string generated = ctv_generate_json();
    std::string generated_again = ctv_generate_json();
    rocketnet_unit_check( generated == generated_again );

    std::string path = ctv_path();

    char regenerate[256];
    if ( rocketnet_test_getenv( "ROCKETNET_REGENERATE_CONNECT_TOKEN_VECTORS", regenerate, sizeof( regenerate ) ) && strcmp( regenerate, "1" ) == 0 )
    {
        std::string temp = path + ".tmp";
        FILE * file = rocketnet_test_fopen( temp, "wb" );
        rocketnet_unit_check( file != NULL );
        rocketnet_unit_check( fwrite( generated.data(), 1, generated.size(), file ) == generated.size() );
        rocketnet_unit_check( fclose( file ) == 0 );
#if defined(_WIN32)
        // The Windows CRT rename refuses to replace an existing destination.
        const std::wstring wide_temp = rocketnet_test_wide_path( temp );
        const std::wstring wide_path = rocketnet_test_wide_path( path );
        rocketnet_unit_check( MoveFileExW( wide_temp.c_str(), wide_path.c_str(),
                                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != 0 );
#else
        rocketnet_unit_check( rename( temp.c_str(), path.c_str() ) == 0 );
#endif
    }

    std::string existing;
    int existed = ctv_read_file( path, existing );
    rocketnet_unit_check( existed );
    rocketnet_unit_check( existing == generated );
}

// every vector in the file: its seed expands to the test backend key, its
// signature verifies, its sealed block opens with the test server secret, and
// every field read back out of the token equals the field the file states
static void test_each_vector_verifies_and_opens()
{
    std::vector<ctv_file_vector_t> vectors = ctv_load();
    rocketnet_unit_check( (int) vectors.size() == ctv_input_count );

    for ( size_t i = 0; i < vectors.size(); i++ )
    {
        const ctv_file_vector_t & v = vectors[i];

        uint8_t backend_public_key[32];
        uint8_t backend_secret_key[64];
        rocketnet_unit_check( crypto_sign_seed_keypair( backend_public_key, backend_secret_key, v.backend_seed ) == 0 );
        rocketnet_unit_check( memcmp( backend_public_key, rocketnet_test_backend_public_key, 32 ) == 0 );
        rocketnet_unit_check( memcmp( backend_secret_key, rocketnet_test_backend_secret_key, 64 ) == 0 );
        sodium_memzero( backend_secret_key, sizeof(backend_secret_key) );
        rocketnet_unit_check( memcmp( v.server_public_key, rocketnet_test_server_public_key, 32 ) == 0 );

        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, v.token );
        rocketnet_unit_check( rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, backend_public_key ) );

        struct rocketnet_signed_connect_data_t signed_fields;
        rocketnet_read_signed_connect_data( &signed_fields, token.signed_connect_data );
        rocketnet_unit_check( signed_fields.version == ROCKETNET_WIRE_VERSION );
        rocketnet_unit_check( signed_fields.protocol_id == v.protocol_id );
        rocketnet_unit_check( signed_fields.create_timestamp == v.create_timestamp );
        rocketnet_unit_check( signed_fields.expire_timestamp == v.expire_timestamp );
        rocketnet_unit_check( signed_fields.timeout_seconds == v.timeout_seconds );
        rocketnet_unit_check( signed_fields.address_flags == v.address_flags );
        rocketnet_unit_check( memcmp( signed_fields.ipv4_address, v.ipv4_address, 6 ) == 0 );
        rocketnet_unit_check( memcmp( signed_fields.ipv6_address, v.ipv6_address, 18 ) == 0 );
        rocketnet_unit_check( memcmp( signed_fields.server_public_key, v.server_public_key, 32 ) == 0 );

        // the sealed box's first 32 bytes are the ephemeral public key the
        // stated ephemeral secret derives
        uint8_t epk[32];
        rocketnet_unit_check( crypto_scalarmult_base( epk, v.ephemeral_secret ) == 0 );
        rocketnet_unit_check( memcmp( signed_fields.sealed_private_block, epk, 32 ) == 0 );

        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_unit_check( rocketnet_open_connect_token_private( &private_fields, token.signed_connect_data,
                                                                    rocketnet_test_server_public_key,
                                                                    rocketnet_test_server_secret_key ) );
        rocketnet_unit_check( rocketnet_connect_token_private_consistent( &private_fields, token.signed_connect_data ) );
        rocketnet_unit_check( private_fields.client_id == v.client_id );
        rocketnet_unit_check( private_fields.protocol_id == v.protocol_id );
        rocketnet_unit_check( private_fields.expire_timestamp == v.expire_timestamp );
        rocketnet_unit_check( memcmp( private_fields.client_to_server_key, v.client_to_server_key, 32 ) == 0 );
        rocketnet_unit_check( memcmp( private_fields.server_to_client_key, v.server_to_client_key, 32 ) == 0 );
        rocketnet_unit_check( memcmp( private_fields.user_data, v.user_data, ROCKETNET_USER_DATA_BYTES ) == 0 );

        // the clear token keys after the signature, then zero padding
        rocketnet_unit_check( memcmp( token.client_to_server_key, v.client_to_server_key, 32 ) == 0 );
        rocketnet_unit_check( memcmp( token.server_to_client_key, v.server_to_client_key, 32 ) == 0 );
        for ( int b = ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES + 64; b < ROCKETNET_CONNECT_TOKEN_BYTES; b++ )
            rocketnet_unit_check( v.token[b] == 0 );
        // and the zero padding after the sealed block inside the signed data
        for ( int b = ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES; b < ROCKETNET_SIGNED_CONNECT_DATA_BYTES; b++ )
            rocketnet_unit_check( v.token[b] == 0 );
    }
}

// the deterministic seal is crypto_box_seal's construction byte for byte: it
// opens with libsodium's own crypto_box_seal_open and with an independent
// crypto_box_open_easy under the blake2b-192(epk || pk) nonce; the same
// inputs mint the same token twice; and outside the sealed block and the
// signature it is the production writer's token exactly
static void test_deterministic_seal_matches_libsodium_open()
{
    for ( int v = 0; v < ctv_input_count; v++ )
    {
        const struct ctv_input_t * input = &ctv_inputs[v];

        uint8_t token_a[ROCKETNET_CONNECT_TOKEN_BYTES];
        uint8_t token_b[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_unit_check( ctv_mint( input, token_a ) == 1 );
        rocketnet_unit_check( ctv_mint( input, token_b ) == 1 );
        rocketnet_unit_check( memcmp( token_a, token_b, ROCKETNET_CONNECT_TOKEN_BYTES ) == 0 );

        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        ctv_fields( input, &signed_fields, &private_fields );
        uint8_t expected_private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
        rocketnet_write_connect_token_private( &private_fields, expected_private_block );

        const uint8_t * sealed = token_a + ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK;

        uint8_t epk[32];
        rocketnet_unit_check( crypto_scalarmult_base( epk, input->ephemeral_secret ) == 0 );
        rocketnet_unit_check( memcmp( sealed, epk, 32 ) == 0 );

        uint8_t opened[ROCKETNET_PRIVATE_BLOCK_BYTES];
        rocketnet_unit_check( crypto_box_seal_open( opened, sealed, ROCKETNET_SEALED_BLOCK_BYTES,
                                                    rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) == 0 );
        rocketnet_unit_check( memcmp( opened, expected_private_block, ROCKETNET_PRIVATE_BLOCK_BYTES ) == 0 );

        uint8_t nonce[crypto_box_NONCEBYTES];
        crypto_generichash_state state;
        rocketnet_unit_check( crypto_generichash_init( &state, NULL, 0, crypto_box_NONCEBYTES ) == 0 );
        rocketnet_unit_check( crypto_generichash_update( &state, epk, 32 ) == 0 );
        rocketnet_unit_check( crypto_generichash_update( &state, rocketnet_test_server_public_key, 32 ) == 0 );
        rocketnet_unit_check( crypto_generichash_final( &state, nonce, crypto_box_NONCEBYTES ) == 0 );
        memset( opened, 0, sizeof(opened) );
        rocketnet_unit_check( crypto_box_open_easy( opened, sealed + 32, ROCKETNET_SEALED_BLOCK_BYTES - 32, nonce,
                                                    epk, rocketnet_test_server_secret_key ) == 0 );
        rocketnet_unit_check( memcmp( opened, expected_private_block, ROCKETNET_PRIVATE_BLOCK_BYTES ) == 0 );

        // re-sealing the same block from the same ephemeral secret by hand is
        // the vector's sealed block exactly
        uint8_t resealed[ROCKETNET_SEALED_BLOCK_BYTES];
        memcpy( resealed, epk, 32 );
        rocketnet_unit_check( crypto_box_easy( resealed + 32, expected_private_block, ROCKETNET_PRIVATE_BLOCK_BYTES, nonce,
                                               rocketnet_test_server_public_key, input->ephemeral_secret ) == 0 );
        rocketnet_unit_check( memcmp( resealed, sealed, ROCKETNET_SEALED_BLOCK_BYTES ) == 0 );

        // the production writer's token differs only in the sealed block and
        // the signature over it
        uint8_t token_random[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields,
                                                             rocketnet_test_backend_secret_key, token_random ) == 1 );
        const int sealed_end = ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES;
        const int signature_end = ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES;
        rocketnet_unit_check( memcmp( token_random, token_a, ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK ) == 0 );
        rocketnet_unit_check( memcmp( token_random + sealed_end, token_a + sealed_end, ROCKETNET_SIGNED_CONNECT_DATA_BYTES - sealed_end ) == 0 );
        rocketnet_unit_check( memcmp( token_random + signature_end, token_a + signature_end, ROCKETNET_CONNECT_TOKEN_BYTES - signature_end ) == 0 );
        rocketnet_unit_check( memcmp( token_random + ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK, sealed, 32 ) != 0 );
    }
}

// one flipped byte anywhere in the signed connect data or the signature is
// refused by the signature check; one flipped byte in the sealed block,
// re-signed so the signature passes, is refused by the seal open
static void test_tampered_vector_refused()
{
    std::vector<ctv_file_vector_t> vectors = ctv_load();
    rocketnet_unit_check( vectors.size() >= 4 );

    for ( size_t i = 0; i < vectors.size(); i++ )
    {
        const ctv_file_vector_t & v = vectors[i];
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

        memcpy( token, v.token, sizeof(token) );
        rocketnet_unit_check( rocketnet_verify_signed_connect_data( token, token + ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                                    rocketnet_test_backend_public_key ) );

        for ( int b = 0; b < ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES; b++ )
        {
            memcpy( token, v.token, sizeof(token) );
            token[b] ^= 0x01;
            rocketnet_unit_check( !rocketnet_verify_signed_connect_data( token, token + ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                                         rocketnet_test_backend_public_key ) );
        }

        // the ephemeral public key, the first ciphertext byte, the last mac byte
        const int positions[] = { 0, 31, 32, 33 + ROCKETNET_PRIVATE_BLOCK_BYTES / 2, ROCKETNET_SEALED_BLOCK_BYTES - 1 };
        for ( size_t p = 0; p < sizeof(positions) / sizeof(positions[0]); p++ )
        {
            memcpy( token, v.token, sizeof(token) );
            token[ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + positions[p]] ^= 0x01;
            rocketnet_unit_check( crypto_sign_detached( token + ROCKETNET_SIGNED_CONNECT_DATA_BYTES, NULL, token,
                                                        ROCKETNET_SIGNED_CONNECT_DATA_BYTES, rocketnet_test_backend_secret_key ) == 0 );
            rocketnet_unit_check( rocketnet_verify_signed_connect_data( token, token + ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                                        rocketnet_test_backend_public_key ) );
            struct rocketnet_connect_token_private_t private_fields;
            rocketnet_unit_check( !rocketnet_open_connect_token_private( &private_fields, token,
                                                                         rocketnet_test_server_public_key,
                                                                         rocketnet_test_server_secret_key ) );
        }
    }
}

// The unit suite links the release archive, whose development minter is
// compiled out; the development shared library built beside it carries
// rocketnet_create_dev_connect_token, so load that library and mint through
// its own symbol.
typedef enum rocketnet_status_t ( *ctv_dev_token_minter_t )( const char * server_address_ipv4, const char * server_address_ipv6,
                                                             const uint8_t * server_public_key, int expire_seconds,
                                                             int timeout_seconds, uint64_t client_id, uint64_t protocol_id,
                                                             uint64_t unix_timestamp, const uint8_t * user_data,
                                                             char * connect_token );

static ctv_dev_token_minter_t ctv_dev_token_minter()
{
    ctv_dev_token_minter_t minter = NULL;
#if defined(_WIN32)
    HMODULE library = LoadLibraryA( ROCKETNET_DEV_LIBRARY_PATH );
    rocketnet_unit_check( library != NULL );
    minter = (ctv_dev_token_minter_t) (void *) GetProcAddress( library, "rocketnet_create_dev_connect_token" );
#else
    void * library = dlopen( ROCKETNET_DEV_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL );
    rocketnet_unit_check( library != NULL );
    minter = (ctv_dev_token_minter_t) dlsym( library, "rocketnet_create_dev_connect_token" );
#endif
    rocketnet_unit_check( minter != NULL );
    return minter;
}

// The dev connect token carries the lifetime and connection timeout it is
// given. Mint one with the development minter at timestamp 1000 using the
// typical values (the named constants), decode the string and read its signed
// fields back with the development backend key, pinning both values.
static void test_dev_connect_token_timeout_is_the_named_constant()
{
    const uint64_t now = 1000;
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    ctv_dev_token_minter_t create_dev = ctv_dev_token_minter();
    rocketnet_unit_check( create_dev( "127.0.0.1:40000", NULL, rocketnet_test_server_public_key,
                                      ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS, ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS,
                                      1, ROCKETNET_DEV_PROTOCOL_ID, now, NULL, token_string ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( strlen( token_string ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    rocketnet_unit_check( rocketnet_base64_decode_data( token_string, token, sizeof( token ) ) == ROCKETNET_CONNECT_TOKEN_BYTES );

    struct rocketnet_connect_token_t read_token;
    rocketnet_read_connect_token( &read_token, token );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( read_token.signed_connect_data, read_token.signature,
                                                                rocketnet_dev_backend_public_key ) );

    struct rocketnet_signed_connect_data_t parsed;
    rocketnet_read_signed_connect_data( &parsed, read_token.signed_connect_data );
    rocketnet_unit_check( parsed.create_timestamp == now );
    rocketnet_unit_check( parsed.protocol_id == ROCKETNET_DEV_PROTOCOL_ID );
    rocketnet_unit_check( parsed.timeout_seconds == ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS );
    rocketnet_unit_check( parsed.expire_timestamp - parsed.create_timestamp == ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS );
}

REGISTER_UNIT_TEST( test_vectors_regenerate_byte_identical );
REGISTER_UNIT_TEST( test_dev_connect_token_timeout_is_the_named_constant );
REGISTER_UNIT_TEST( test_each_vector_verifies_and_opens );
REGISTER_UNIT_TEST( test_deterministic_seal_matches_libsodium_open );
REGISTER_UNIT_TEST( test_tampered_vector_refused );

// ---------------------------------------------------------------------------
// Wycheproof Ed25519. The rule these tests pin is RFC 8032 section 5.1.7 as
// the shipped code applies it: sdk/rocketnet_connect_token.cpp,
// rocketnet_verify_signed_connect_data accepts a signature only when
// crypto_sign_verify_detached returns 0, and the verifier is the vendored
// subset in sodium/sodium.c that the SDK links. The vectors are
// tests/wycheproof/ed25519_test.json at the pin the vendor card landed.

#if !defined(ROCKETNET_USE_SYSTEM_SODIUM) // rocketnet-wycheproof-ed25519

struct wp_ed25519_tally_t
{
    int verified;
    int refused_verify;
    int sig_size;
};

struct wp_ed25519_disagreement_t
{
    int tc_id;
    std::string result;
    std::string outcome;
    size_t sig_bytes;
};

static int wp_ed25519_run( bool accept_all, wp_ed25519_tally_t * tally,
                           std::vector<wp_ed25519_disagreement_t> * disagreements )
{
    tally->verified = 0;
    tally->refused_verify = 0;
    tally->sig_size = 0;
    if ( disagreements )
        disagreements->clear();

    std::vector<wycheproof_test_t> tests;
    int number_of_tests = 0;
    int number_of_groups = 0;
    if ( !wycheproof_load( "ed25519_test.json", tests, &number_of_tests, &number_of_groups ) )
        return -1;

    int failed = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        const wycheproof_test_t & test = tests[i];

        const std::map<std::string, std::string>::const_iterator pk_text = test.group.find( "publicKey.pk" );
        const std::map<std::string, std::string>::const_iterator msg_text = test.fields.find( "msg" );
        const std::map<std::string, std::string>::const_iterator sig_text = test.fields.find( "sig" );
        if ( pk_text == test.group.end() || msg_text == test.fields.end() || sig_text == test.fields.end() )
            return -1;

        std::vector<uint8_t> pk;
        std::vector<uint8_t> msg;
        std::vector<uint8_t> sig;
        if ( !wycheproof_hex( pk_text->second, pk ) || pk.size() != 32 )
            return -1;
        if ( !wycheproof_hex( msg_text->second, msg ) )
            return -1;
        if ( !wycheproof_hex( sig_text->second, sig ) )
            return -1;

        const bool expected_valid = ( test.result == "valid" );
        bool actual_valid = false;
        std::string outcome;

        if ( sig.size() != (size_t) ROCKETNET_SIGNATURE_BYTES )
        {
            // rocketnet reads exactly ROCKETNET_SIGNATURE_BYTES: the size is
            // the SDK's own refusal, before any verify is attempted
            outcome = "sig_size";
            actual_valid = false;
            tally->sig_size++;
        }
        else if ( accept_all )
        {
            // the planted bug: reach the verify branch and never look at its
            // result, as a verify that does not check would
            outcome = "verified";
            actual_valid = true;
            tally->verified++;
        }
        else
        {
            static const uint8_t empty_message = 0;
            const uint8_t * message = msg.empty() ? &empty_message : msg.data();
            if ( crypto_sign_verify_detached( sig.data(), message, (unsigned long long) msg.size(), pk.data() ) == 0 )
            {
                outcome = "verified";
                actual_valid = true;
                tally->verified++;
            }
            else
            {
                outcome = "refused_verify";
                actual_valid = false;
                tally->refused_verify++;
            }
        }

        if ( actual_valid != expected_valid )
        {
            failed++;
            printf( "wycheproof ed25519_test.json tcId %d %s %s\n",
                    test.tc_id, test.result.c_str(), outcome.c_str() );
            printf( "wycheproof ed25519_test.json tcId %d flags", test.tc_id );
            for ( size_t f = 0; f < test.flags.size(); f++ )
                printf( " %s", test.flags[f].c_str() );
            printf( "\n" );
            if ( disagreements )
            {
                wp_ed25519_disagreement_t d;
                d.tc_id = test.tc_id;
                d.result = test.result;
                d.outcome = outcome;
                d.sig_bytes = sig.size();
                disagreements->push_back( d );
            }
        }
    }

    return failed;
}

// Pins sdk/rocketnet_connect_token.cpp rocketnet_verify_signed_connect_data
// against all 151 Ed25519 vectors, then drives the SDK wrapper itself: S
// replaced by S + L (RFC 8032 section 5.1.7), a flipped R bit, flipped data
// and the identity public key encoding are all refused.
static void test_wycheproof_ed25519_vectors()
{
    wp_ed25519_tally_t tally;
    rocketnet_unit_check( wp_ed25519_run( false, &tally, NULL ) == 0 );
    rocketnet_unit_check( tally.verified == 88 );
    rocketnet_unit_check( tally.refused_verify == 51 );
    rocketnet_unit_check( tally.sig_size == 12 );

    uint8_t data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    for ( int i = 0; i < ROCKETNET_SIGNED_CONNECT_DATA_BYTES; i++ )
        data[i] = (uint8_t) ( i & 0xff );

    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    rocketnet_unit_check( crypto_sign_detached( signature, NULL, data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                rocketnet_test_backend_secret_key ) == 0 );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( data, signature,
                                                                rocketnet_test_backend_public_key ) == 1 );

    // L = 2^252 + 27742317777372353535851937790883648493, little endian
    static const uint8_t group_order[32] =
    {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    };

    uint8_t malleable[ROCKETNET_SIGNATURE_BYTES];
    memcpy( malleable, signature, sizeof(malleable) );
    int carry = 0;
    for ( int i = 0; i < 32; i++ )
    {
        int sum = (int) malleable[32 + i] + (int) group_order[i] + carry;
        malleable[32 + i] = (uint8_t) ( sum & 0xff );
        carry = sum >> 8;
    }
    rocketnet_unit_check( carry == 0 );
    rocketnet_unit_check( !rocketnet_verify_signed_connect_data( data, malleable,
                                                                 rocketnet_test_backend_public_key ) );

    uint8_t flipped_r[ROCKETNET_SIGNATURE_BYTES];
    memcpy( flipped_r, signature, sizeof(flipped_r) );
    flipped_r[0] ^= 0x01;
    rocketnet_unit_check( !rocketnet_verify_signed_connect_data( data, flipped_r,
                                                                 rocketnet_test_backend_public_key ) );

    uint8_t flipped_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    memcpy( flipped_data, data, sizeof(flipped_data) );
    flipped_data[100] ^= 0x01;
    rocketnet_unit_check( !rocketnet_verify_signed_connect_data( flipped_data, signature,
                                                                 rocketnet_test_backend_public_key ) );

    uint8_t identity[32];
    memset( identity, 0, sizeof(identity) );
    identity[0] = 0x01;
    rocketnet_unit_check( !rocketnet_verify_signed_connect_data( data, signature, identity ) );
}

// The witness: wp_ed25519_run with the planted accept-all flag must go red on
// exactly the 51 invalid vectors whose 64-byte signature the verify branch in
// sdk/rocketnet_connect_token.cpp rocketnet_verify_signed_connect_data exists
// to refuse. A green witness means the main test would not catch that bug.
static void test_wycheproof_ed25519_witness_accept_all_goes_red()
{
    wp_ed25519_tally_t tally;
    std::vector<wp_ed25519_disagreement_t> disagreements;
    rocketnet_unit_check( wp_ed25519_run( true, &tally, &disagreements ) == 51 );
    rocketnet_unit_check( disagreements.size() == 51 );
    for ( size_t i = 0; i < disagreements.size(); i++ )
    {
        rocketnet_unit_check( disagreements[i].result == "invalid" );
        rocketnet_unit_check( disagreements[i].outcome == "verified" );
        rocketnet_unit_check( disagreements[i].sig_bytes == (size_t) ROCKETNET_SIGNATURE_BYTES );
    }
}

REGISTER_UNIT_TEST( test_wycheproof_ed25519_vectors );
REGISTER_UNIT_TEST( test_wycheproof_ed25519_witness_accept_all_goes_red );

#endif // !defined(ROCKETNET_USE_SYSTEM_SODIUM), rocketnet-wycheproof-ed25519
