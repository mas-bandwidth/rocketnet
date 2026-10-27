/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_backend_keypair_from_seed.cpp -- the public seed-derived backend
// keypair in sdk/rocketnet.h, checked against the vendored library and the
// production token writer. Every case carries its own fixed seed and fixed
// timestamps: no case reads a clock, opens a socket or touches anything
// outside the process, and no case prints key material.

#include "test_unit_registry.h"

#include "rocketnet_connect_token.h"

#include <stdint.h>
#include <string.h>

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

static void backend_seed_fill( uint8_t * seed, uint8_t basis )
{
    for ( int i = 0; i < RN_TEST_BACKEND_SEED_BYTES; i++ )
        seed[i] = (uint8_t) ( basis + i * 13 + 7 );
}

static const uint64_t backend_seed_protocol_id = 0x1122334455667788ULL;
static const uint64_t backend_seed_client_id = 0x0123456789abcdefULL;
static const uint64_t backend_seed_create_timestamp = 1756166400ULL;
static const uint64_t backend_seed_expire_timestamp = 1756166700ULL;
static const int32_t backend_seed_timeout_seconds = 10;

static const char * const backend_seed_addresses[] = {
    "203.0.113.10:40000",
};

void test_backend_keypair_from_seed_round_trip()
{
    uint8_t seed[RN_TEST_BACKEND_SEED_BYTES];
    backend_seed_fill( seed, 0x21 );

    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );

    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    backend_seed_protocol_id, backend_seed_addresses, 1,
                                                                    backend_seed_client_id, backend_seed_create_timestamp,
                                                                    backend_seed_expire_timestamp, backend_seed_timeout_seconds,
                                                                    NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, token );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature,
                                                                backend_public_key ) == 1 );

    struct rocketnet_connect_token_private_t opened;
    rocketnet_unit_check( rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data,
                                                                server_public_key, server_secret_key ) == 1 );
    rocketnet_unit_check( opened.client_id == backend_seed_client_id );
    rocketnet_unit_check( opened.protocol_id == backend_seed_protocol_id );

    sodium_memzero( backend_secret_key, sizeof( backend_secret_key ) );
    sodium_memzero( server_secret_key, sizeof( server_secret_key ) );
    sodium_memzero( &opened, sizeof( opened ) );
}

void test_backend_keypair_from_seed_is_deterministic()
{
    uint8_t seed_a[RN_TEST_BACKEND_SEED_BYTES];
    uint8_t seed_b[RN_TEST_BACKEND_SEED_BYTES];
    backend_seed_fill( seed_a, 0x31 );
    backend_seed_fill( seed_b, 0x91 );

    uint8_t public_a[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t secret_a[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t public_b[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t secret_b[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t public_c[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t secret_c[RN_TEST_BACKEND_SECRET_KEY_BYTES];

    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed_a, public_a, secret_a ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed_a, public_b, secret_b ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed_b, public_c, secret_c ) == ROCKETNET_STATUS_OK );

    rocketnet_unit_check( memcmp( public_a, public_b, ROCKETNET_BACKEND_PUBLIC_KEY_BYTES ) == 0 );
    rocketnet_unit_check( memcmp( secret_a, secret_b, RN_TEST_BACKEND_SECRET_KEY_BYTES ) == 0 );
    rocketnet_unit_check( memcmp( public_a, public_c, ROCKETNET_BACKEND_PUBLIC_KEY_BYTES ) != 0 );
    rocketnet_unit_check( memcmp( secret_a, secret_c, RN_TEST_BACKEND_SECRET_KEY_BYTES ) != 0 );

    sodium_memzero( secret_a, sizeof( secret_a ) );
    sodium_memzero( secret_b, sizeof( secret_b ) );
    sodium_memzero( secret_c, sizeof( secret_c ) );
}

void test_backend_keypair_from_seed_matches_the_library()
{
    uint8_t seed[RN_TEST_BACKEND_SEED_BYTES];
    backend_seed_fill( seed, 0x51 );

    uint8_t public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed, public_key, secret_key ) == ROCKETNET_STATUS_OK );

    uint8_t library_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t library_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_unit_check( crypto_sign_seed_keypair( library_public_key, library_secret_key, seed ) == 0 );

    rocketnet_unit_check( memcmp( public_key, library_public_key, ROCKETNET_BACKEND_PUBLIC_KEY_BYTES ) == 0 );
    rocketnet_unit_check( memcmp( secret_key, library_secret_key, RN_TEST_BACKEND_SECRET_KEY_BYTES ) == 0 );

    sodium_memzero( secret_key, sizeof( secret_key ) );
    sodium_memzero( library_secret_key, sizeof( library_secret_key ) );
}

// a NULL argument is API misuse: the guard records it and fires rocketnet_assert, which traps in a build with asserts on.
// The refusal is what this case checks, so the assert goes to a counting hook for its span (the misuse pattern of
// test_unit_counter_classes.cpp and test_unit_public_not_released_counters.cpp) and the default is restored after.
static int backend_keypair_from_seed_asserts;

static void backend_keypair_from_seed_assert_hook( const char *, const char *, const char *, int )
{
    backend_keypair_from_seed_asserts++;
}

void test_backend_keypair_from_seed_null_refused()
{
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( backend_keypair_from_seed_assert_hook );
    backend_keypair_from_seed_asserts = 0;

    uint8_t seed[RN_TEST_BACKEND_SEED_BYTES];
    backend_seed_fill( seed, 0x61 );

    uint8_t public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t public_reference[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t secret_reference[RN_TEST_BACKEND_SECRET_KEY_BYTES];

    memset( public_reference, 0xa5, sizeof( public_reference ) );
    memset( secret_reference, 0xa5, sizeof( secret_reference ) );

    memcpy( public_key, public_reference, sizeof( public_key ) );
    memcpy( secret_key, secret_reference, sizeof( secret_key ) );
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( NULL, public_key, secret_key ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_unit_check( memcmp( public_key, public_reference, sizeof( public_key ) ) == 0 );
    rocketnet_unit_check( memcmp( secret_key, secret_reference, sizeof( secret_key ) ) == 0 );

    memcpy( public_key, public_reference, sizeof( public_key ) );
    memcpy( secret_key, secret_reference, sizeof( secret_key ) );
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed, NULL, secret_key ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_unit_check( memcmp( secret_key, secret_reference, sizeof( secret_key ) ) == 0 );

    memcpy( public_key, public_reference, sizeof( public_key ) );
    memcpy( secret_key, secret_reference, sizeof( secret_key ) );
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed, public_key, NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_unit_check( memcmp( public_key, public_reference, sizeof( public_key ) ) == 0 );

#if ROCKETNET_ASSERTS
    rocketnet_unit_check( backend_keypair_from_seed_asserts == 3 );
#endif
    rocketnet_assert_function( saved_assert );
}

REGISTER_UNIT_TEST( test_backend_keypair_from_seed_round_trip );
REGISTER_UNIT_TEST( test_backend_keypair_from_seed_is_deterministic );
REGISTER_UNIT_TEST( test_backend_keypair_from_seed_matches_the_library );
REGISTER_UNIT_TEST( test_backend_keypair_from_seed_null_refused );
