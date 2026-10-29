/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Compile the actual key schedule with an observing wipe wrapper. The wrapper
// calls the real sodium_memzero and inspects each buffer while it is alive;
// reading an expired stack frame after return would be undefined behavior.
// Public function names are private to this test copy. The SDK stays unchanged.
#include "test_unit_registry.h"
#include "../sdk/rocketnet_keys.h"
#include "../sdk/rocketnet_test_vectors.h"
#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif
#include <string.h>

#define check rocketnet_unit_check

static uint8_t wipe_shared[32];
static rocketnet_connection_keys_t wipe_expected;
static rocketnet_connection_keys_t * wipe_output;
static int wipe_shared_count;
static int wipe_input_count;
static int wipe_kdf_count;

static void witness_observe_memzero( void * p, size_t bytes )
{
    const uint8_t * data = (const uint8_t *) p;
    if ( bytes == 32 || bytes == 112 )
    {
        check( memcmp( data, wipe_shared, sizeof( wipe_shared ) ) == 0 );
        // All four keys are available before the shared secret and its copy
        // are erased. The ordinary linked implementation supplies the oracle.
        check( memcmp( wipe_output, &wipe_expected, sizeof( wipe_expected ) ) == 0 );
        if ( bytes == 32 )
            wipe_shared_count++;
        else
            wipe_input_count++;
    }
    else
    {
        check( bytes == 128 );
        wipe_kdf_count++;
    }
    sodium_memzero( p, bytes );
    for ( size_t i = 0; i < bytes; i++ )
        check( data[i] == 0 );
}

#undef sodium_memzero
#define sodium_memzero witness_observe_memzero
#define rocketnet_derive_handshake_keys witness_derive_handshake_keys
#define rocketnet_derive_connection_keys witness_derive_connection_keys
#include "../sdk/rocketnet_keys.cpp"
#undef rocketnet_derive_connection_keys
#undef rocketnet_derive_handshake_keys
#undef sodium_memzero

static void test_key_schedule_erases_live_stack_buffers()
{
    const uint8_t * own_secrets[] = { rocketnet_test_client_eph_secret, rocketnet_test_server_eph_secret };
    const uint8_t * remote_public[] = { rocketnet_test_server_eph_public, rocketnet_test_client_eph_public };
    for ( int side = 0; side < 2; side++ )
    {
        check( crypto_scalarmult( wipe_shared, own_secrets[side], remote_public[side] ) == 0 );
        check( rocketnet_derive_connection_keys( &wipe_expected,
                                                 rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                 rocketnet_test_salt, rocketnet_test_session_nonce,
                                                 own_secrets[side], remote_public[side],
                                                 rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) == 1 );
        rocketnet_connection_keys_t keys;
        memset( &keys, 0xA5, sizeof( keys ) );
        wipe_output = &keys;
        wipe_shared_count = wipe_input_count = wipe_kdf_count = 0;
        check( witness_derive_connection_keys( &keys,
                                               rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                               rocketnet_test_salt, rocketnet_test_session_nonce,
                                               own_secrets[side], remote_public[side],
                                               rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) == 1 );
        check( wipe_shared_count == 1 );
        check( wipe_input_count == 1 );
        check( wipe_kdf_count == 4 );
        check( memcmp( &keys, &wipe_expected, sizeof( keys ) ) == 0 );
    }
    wipe_output = NULL;
}

REGISTER_UNIT_TEST( test_key_schedule_erases_live_stack_buffers );
