/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_connect_token_vectors.cpp -- byte-stable 768-byte connect token
// vectors in tests/connect_token_vectors.json, the known answers another
// implementation of the backend's minting (a Go backend first) is checked
// against byte for byte.
//
// The inputs and the generator live in tools/connect_token_vectors/main.cpp,
// compiled here without its main, so the check and the tool that writes the
// file mint from one source. The committed file is compiled into ./bin/test
// at build time (cmake/EmbedTestFixtures.cmake) and read from memory: no case
// reads or writes a file, reads a clock or opens a socket. The file is
// regenerated only by running the tool, ./bin/connect_token_vectors
// tests/connect_token_vectors.json.

#include "test_unit_registry.h"

#include "rocketnet_connect_token.h"
#include "rocketnet_base64.h"
#include "rocketnet_test_vectors.h"
#include "wycheproof.h"

// Match the header to the sodium implementation linked into the SDK.
#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

// the vector inputs, ctv_mint and ctv_generate_json
#define ROCKETNET_CONNECT_TOKEN_VECTORS_NO_MAIN
#include "../tools/connect_token_vectors/main.cpp"

static int ctv_unhex_digit( char c )
{
    if ( c >= '0' && c <= '9' )
        return c - '0';
    if ( c >= 'a' && c <= 'f' )
        return c - 'a' + 10;

    return -1;
}

// strict lower-case hex of exactly count bytes; 1 on success

static int ctv_unhex( const std::string & text, uint8_t * bytes, int count )
{
    if ( (int) text.size() != count * 2 )
        return 0;

    for ( int i = 0; i < count; i++ )
    {
        int hi = ctv_unhex_digit( text[(size_t) ( 2 * i )] );
        int lo = ctv_unhex_digit( text[(size_t) ( 2 * i + 1 )] );
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
    {
        value = ( value << 8 ) | bytes[i];
    }

    return value;
}

// the committed tests/connect_token_vectors.json, compiled into this binary
// at build time (cmake/EmbedTestFixtures.cmake); 1 when it is there

static int ctv_read_embedded( std::string & out )
{
    out.clear();

    size_t bytes = 0;
    const unsigned char * data = rocketnet_test_fixture( "tests/connect_token_vectors.json", &bytes );
    if ( !data )
        return 0;
    out.assign( reinterpret_cast<const char *>( data ), bytes );

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
    rocketnet_unit_check( ctv_read_embedded( text ) );

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

// the file is exactly what the inputs mint. the committed copy is compiled
// into this binary and compared in memory; nothing here writes it. the file
// is rewritten only by the tool, ./bin/connect_token_vectors, from the same
// generator (tools/connect_token_vectors/main.cpp)

static void test_vectors_regenerate_byte_identical()
{
    std::string generated;
    std::string generated_again;
    rocketnet_unit_check( ctv_generate_json( generated ) );
    rocketnet_unit_check( ctv_generate_json( generated_again ) );
    rocketnet_unit_check( generated == generated_again );

    std::string existing;
    int existed = ctv_read_embedded( existing );
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
        sodium_memzero( backend_secret_key, sizeof( backend_secret_key ) );
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
        {
            rocketnet_unit_check( v.token[b] == 0 );
        }

        // and the zero padding after the sealed block inside the signed data

        for ( int b = ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES; b < ROCKETNET_SIGNED_CONNECT_DATA_BYTES; b++ )
        {
            rocketnet_unit_check( v.token[b] == 0 );
        }
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
        memset( opened, 0, sizeof( opened ) );
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

        memcpy( token, v.token, sizeof( token ) );
        rocketnet_unit_check( rocketnet_verify_signed_connect_data( token, token + ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                                    rocketnet_test_backend_public_key ) );

        for ( int b = 0; b < ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES; b++ )
        {
            memcpy( token, v.token, sizeof( token ) );
            token[b] ^= 0x01;
            rocketnet_unit_check( !rocketnet_verify_signed_connect_data( token, token + ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                                         rocketnet_test_backend_public_key ) );
        }

        // the ephemeral public key, the first ciphertext byte, the last mac byte

        const int positions[] = { 0, 31, 32, 33 + ROCKETNET_PRIVATE_BLOCK_BYTES / 2, ROCKETNET_SEALED_BLOCK_BYTES - 1 };
        for ( size_t p = 0; p < sizeof( positions ) / sizeof( positions[0] ); p++ )
        {
            memcpy( token, v.token, sizeof( token ) );
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

REGISTER_UNIT_TEST( test_vectors_regenerate_byte_identical );
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

#if !defined( ROCKETNET_USE_SYSTEM_SODIUM ) // rocketnet-wycheproof-ed25519

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
    {
        disagreements->clear();
    }

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
            {
                printf( " %s", test.flags[f].c_str() );
            }
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
    {
        data[i] = (uint8_t) ( i & 0xff );
    }

    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    rocketnet_unit_check( crypto_sign_detached( signature, NULL, data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                rocketnet_test_backend_secret_key ) == 0 );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( data, signature,
                                                                rocketnet_test_backend_public_key ) == 1 );

    // L = 2^252 + 27742317777372353535851937790883648493, little endian

    static const uint8_t group_order[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    };

    uint8_t malleable[ROCKETNET_SIGNATURE_BYTES];
    memcpy( malleable, signature, sizeof( malleable ) );

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
    memcpy( flipped_r, signature, sizeof( flipped_r ) );
    flipped_r[0] ^= 0x01;
    rocketnet_unit_check( !rocketnet_verify_signed_connect_data( data, flipped_r,
                                                                 rocketnet_test_backend_public_key ) );

    uint8_t flipped_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    memcpy( flipped_data, data, sizeof( flipped_data ) );
    flipped_data[100] ^= 0x01;
    rocketnet_unit_check( !rocketnet_verify_signed_connect_data( flipped_data, signature,
                                                                 rocketnet_test_backend_public_key ) );

    uint8_t identity[32];
    memset( identity, 0, sizeof( identity ) );
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
