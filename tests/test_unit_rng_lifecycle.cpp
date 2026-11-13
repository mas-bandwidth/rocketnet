/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */

#include "test_unit_registry.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_connect_token.h"

#include <stdint.h>
#include <stdlib.h>

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

// The bundled-RNG descriptor lifecycle case reads the live process's open
// descriptors through proc_pidinfo, which is OS state, so it is a functional
// case: test_bundled_rng_closes_and_reopens_on_mac in
// tests/functional/platform_primitives.cpp. The system-sodium case below replaces
// the RNG with a fake and reads nothing from the OS, so it stays a unit case.

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#define check rocketnet_unit_check

static int rng_lifecycle_system_close_calls = 0;
static const char * rng_lifecycle_name() { return "rocketnet-test-rng"; }
static uint32_t rng_lifecycle_random() { return 42; }
static void rng_lifecycle_buf( void * p, size_t bytes )
{
    unsigned char * out = (unsigned char *) p;
    for ( size_t i = 0; i < bytes; ++i )
    {
        out[i] = (unsigned char) ( i + 1 );
    }
}

static int rng_lifecycle_close()
{
    ++rng_lifecycle_system_close_calls;
    return 0;
}

static void test_system_sodium_rng_remains_application_owned()
{
    static randombytes_implementation rng = {};
    rng.implementation_name = rng_lifecycle_name;
    rng.random = rng_lifecycle_random;
    rng.buf = rng_lifecycle_buf;
    rng.close = rng_lifecycle_close;
    check( randombytes_set_implementation( &rng ) == 0 );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rng_lifecycle_system_close_calls == 0 );

    unsigned char output[4] = {};
    randombytes_buf( output, sizeof( output ) );
    check( output[0] == 1 && output[1] == 2 && output[2] == 3 && output[3] == 4 );

    // keep the implementation alive through the worker's final term

    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_system_sodium_rng_remains_application_owned );
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

#if ROCKETNET_DEV

// The key and token generators hold the crypto source across their draws
// (rocketnet_crypto_acquire, rocketnet_crypto_release, sdk/rocketnet.cpp).
// On macOS with the bundled sodium rocketnet_term waits for the holder count
// to reach zero before it closes the source, so a count that starts above
// zero, or that a generator leaves above or below where it found it, makes
// term wait forever there. Elsewhere term never waits, so the count itself
// is read (a DEV accessor) and checked on every platform: the leak is red on
// Linux too, not only as a macOS hang. Each generator runs, then the count
// must read zero; then term returns and init restores the worker's state.

static void rng_lifecycle_generate_each( enum rocketnet_status_t want, bool check_holders_between )
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t seed[RN_TEST_BACKEND_SEED_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
    for ( size_t i = 0; i < sizeof( seed ); ++i )
    {
        seed[i] = (uint8_t) ( i * 7 + 3 );
    }

    // mutation gap: hunt 2/131 (early return deleted), hunt 2/132 (release deleted)
    const enum rocketnet_status_t backend_status = rocketnet_generate_backend_keypair( backend_public, backend_secret );
    const bool backend_keypair_status_is_expected = backend_status == want;
    rocketnet_unit_check( backend_keypair_status_is_expected );
    if ( check_holders_between )
    {
        const bool backend_keypair_left_no_crypto_holder = rocketnet_crypto_holders_for_test() == 0;
        rocketnet_unit_check( backend_keypair_left_no_crypto_holder );
    }

    // mutation gap: hunt 2/140 (early return deleted), hunt 2/141 (release deleted)
    const enum rocketnet_status_t seed_status = rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret );
    const bool seed_keypair_status_is_expected = seed_status == want;
    rocketnet_unit_check( seed_keypair_status_is_expected );
    if ( check_holders_between )
    {
        const bool seed_keypair_left_no_crypto_holder = rocketnet_crypto_holders_for_test() == 0;
        rocketnet_unit_check( seed_keypair_left_no_crypto_holder );
    }

    // mutation gap: hunt 2/148 (early return deleted), hunt 2/149 (release deleted)
    const enum rocketnet_status_t server_status = rocketnet_generate_server_keypair( server_public, server_secret );
    const bool server_keypair_status_is_expected = server_status == want;
    rocketnet_unit_check( server_keypair_status_is_expected );
    if ( check_holders_between )
    {
        const bool server_keypair_left_no_crypto_holder = rocketnet_crypto_holders_for_test() == 0;
        rocketnet_unit_check( server_keypair_left_no_crypto_holder );
    }
}

static void test_crypto_holders_return_to_zero_after_each_generator()
{
    // mutation gap: me rocketnet.cpp:773 holders starts at 1
    const bool crypto_holders_start_at_zero = rocketnet_crypto_holders_for_test() == 0;
    rocketnet_unit_check( crypto_holders_start_at_zero );

    // mutation gap: me rocketnet.cpp:810 holders++ deleted (each release takes the count below zero)
    rng_lifecycle_generate_each( ROCKETNET_STATUS_OK, true );

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );

    const char * addresses[] = { "127.0.0.1:40000" };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret, server_public, 0x1234ULL, addresses, 1, 7,
                                                                    1000, 2000, 10, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    const bool connect_token_left_no_crypto_holder = rocketnet_crypto_holders_for_test() == 0;
    rocketnet_unit_check( connect_token_left_no_crypto_holder );

    // with no holder left, term returns (on macOS with the bundled sodium it
    // waits for the holder count to reach zero, so a leaked holder hangs it)
    rocketnet_unit_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_crypto_holders_return_to_zero_after_each_generator );

// An acquire that fails holds nothing: each generator refuses with
// CRYPTO_INIT_FAILED, draws nothing, and does not release what it never held
// (a release without its acquire takes the count below zero). The fault
// stands for a sodium_init failure, which no test can cause.

static void test_crypto_acquire_failure_holds_nothing()
{
    rocketnet_crypto_fail_acquire_for_test( true );
    rng_lifecycle_generate_each( ROCKETNET_STATUS_CRYPTO_INIT_FAILED, false );
    const int holders_after_refusals = rocketnet_crypto_holders_for_test();
    rocketnet_crypto_fail_acquire_for_test( false );

    const bool refused_generators_left_crypto_holders_unchanged = holders_after_refusals == 0;
    rocketnet_unit_check( refused_generators_left_crypto_holders_unchanged );

    // the source acquires again once the fault is cleared
    rng_lifecycle_generate_each( ROCKETNET_STATUS_OK, true );
}

REGISTER_UNIT_TEST( test_crypto_acquire_failure_holds_nothing );

#endif // #if ROCKETNET_DEV
