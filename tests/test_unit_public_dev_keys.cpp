/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The four published development keys of sdk/rocketnet.h:
// ROCKETNET_DEV_BACKEND_PUBLIC_KEY, ROCKETNET_DEV_BACKEND_SECRET_KEY,
// ROCKETNET_DEV_SERVER_PUBLIC_KEY and ROCKETNET_DEV_SERVER_SECRET_KEY. Each
// public macro is byte for byte the library's own copy, the pairs match, a
// server given the development secret reports the development public key,
// and rocketnet_create_connect_token mints with the pair. Development builds
// only: a production header compiles the macros out. No socket, no clock.

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_connect_token.h"
#include "loopback_hub.h"

#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

static void test_public_dev_keys_match_the_library()
{
    static const uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    static const uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES] = ROCKETNET_DEV_BACKEND_SECRET_KEY;
    static const uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_SERVER_PUBLIC_KEY;
    static const uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES] = ROCKETNET_DEV_SERVER_SECRET_KEY;

    check( memcmp( backend_public, rocketnet_dev_backend_public_key, sizeof( backend_public ) ) == 0 );
    check( memcmp( backend_secret, rocketnet_dev_backend_secret_key, sizeof( backend_secret ) ) == 0 );
    check( memcmp( server_public, rocketnet_dev_server_public_key, sizeof( server_public ) ) == 0 );
    check( memcmp( server_secret, rocketnet_dev_server_secret_key, sizeof( server_secret ) ) == 0 );

    // the Ed25519 secret carries its public half in its last 32 bytes

    check( memcmp( backend_secret + ROCKETNET_BACKEND_SECRET_KEY_BYTES - ROCKETNET_BACKEND_PUBLIC_KEY_BYTES, backend_public, sizeof( backend_public ) ) == 0 );

    // a server given the development secret derives the development public key

    rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    config.max_clients = 1;
    memcpy( config.backend_public_key, backend_public, sizeof( backend_public ) );
    memcpy( config.server_secret_key, server_secret, sizeof( server_secret ) );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( server != NULL );
    uint8_t read_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, read_public );
    check( memcmp( read_public, server_public, sizeof( server_public ) ) == 0 );
    rocketnet_server_destroy( server );

    // the public minter takes the development pair as a backend would

    rocketnet_connect_token_config_t token_config;
    check( rocketnet_connect_token_default_config( &token_config ) == ROCKETNET_STATUS_OK );
    memcpy( token_config.backend_secret_key, backend_secret, sizeof( backend_secret ) );
    memcpy( token_config.server_public_key, server_public, sizeof( server_public ) );
    token_config.server_address_ipv4 = "127.0.0.1:40000";
    token_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    token_config.client_id = 1;
    token_config.unix_timestamp = 1000;
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_create_connect_token( &token_config, token ) == ROCKETNET_STATUS_OK );
    check( strlen( token ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

REGISTER_UNIT_TEST( test_public_dev_keys_match_the_library );

#endif // #if ROCKETNET_DEV
