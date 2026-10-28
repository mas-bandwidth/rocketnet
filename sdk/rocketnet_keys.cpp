/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// KDF context strings, exactly 8 ASCII bytes, with no terminating null in
// the hash input. The derive functions pass these macros; the conformance
// dump includes this file under ROCKETNET_CONFORMANCE_DEFAULTS_ONLY and
// expands the same ones.

#define ROCKETNET_KDF_CONTEXT_HANDSHAKE_C2S "RKTNHNDC"
#define ROCKETNET_KDF_CONTEXT_HANDSHAKE_S2C "RKTNHNDS"
#define ROCKETNET_KDF_CONTEXT_SESSION_C2S   "RKTNSESC"
#define ROCKETNET_KDF_CONTEXT_SESSION_S2C   "RKTNSESS"

#ifndef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

#include "rocketnet_keys.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_internal.h"

#include <sodium.h>
#include <string.h>

static void rocketnet_derive_key( uint8_t * out,
                                  const uint8_t * token_key,
                                  const uint8_t * input,
                                  int input_bytes,
                                  const char * context )
{
    uint8_t buffer[128];
    rocketnet_assert( input_bytes + 8 <= (int) sizeof( buffer ) );

    memcpy( buffer, input, (size_t) input_bytes );
    memcpy( buffer + input_bytes, context, 8 );

    crypto_generichash( out, 32, buffer, (unsigned long long) ( input_bytes + 8 ), token_key, 32 );

    sodium_memzero( buffer, sizeof( buffer ) );
}

void rocketnet_derive_handshake_keys( struct rocketnet_connection_keys_t * keys,
                                      const uint8_t * token_client_to_server_key,
                                      const uint8_t * token_server_to_client_key,
                                      const uint8_t * connection_salt,
                                      const uint8_t * session_nonce )
{
    rocketnet_assert( keys );
    rocketnet_assert( token_client_to_server_key );
    rocketnet_assert( token_server_to_client_key );
    rocketnet_assert( connection_salt );
    rocketnet_assert( session_nonce );

    uint8_t input[16];
    memcpy( input, connection_salt, 8 );
    memcpy( input + 8, session_nonce, 8 );

    rocketnet_derive_key( keys->handshake_client_to_server, token_client_to_server_key, input, 16, ROCKETNET_KDF_CONTEXT_HANDSHAKE_C2S );
    rocketnet_derive_key( keys->handshake_server_to_client, token_server_to_client_key, input, 16, ROCKETNET_KDF_CONTEXT_HANDSHAKE_S2C );
}

int rocketnet_derive_connection_keys( struct rocketnet_connection_keys_t * keys,
                                      const uint8_t * token_client_to_server_key,
                                      const uint8_t * token_server_to_client_key,
                                      const uint8_t * connection_salt,
                                      const uint8_t * session_nonce,
                                      const uint8_t * own_ephemeral_secret_key,
                                      const uint8_t * remote_ephemeral_public_key,
                                      const uint8_t * client_ephemeral_public_key,
                                      const uint8_t * server_ephemeral_public_key )
{
    rocketnet_assert( keys );
    rocketnet_assert( own_ephemeral_secret_key );
    rocketnet_assert( remote_ephemeral_public_key );
    rocketnet_assert( client_ephemeral_public_key );
    rocketnet_assert( server_ephemeral_public_key );

    uint8_t shared_secret[32];
    ROCKETNET_CRYPTO_TALLY( scalarmult );
    if ( crypto_scalarmult( shared_secret, own_ephemeral_secret_key, remote_ephemeral_public_key ) != 0 )
        return 0;

    rocketnet_derive_handshake_keys( keys, token_client_to_server_key, token_server_to_client_key, connection_salt, session_nonce );

    uint8_t input[32 + 32 + 32 + 8 + 8];
    memcpy( input, shared_secret, 32 );
    memcpy( input + 32, client_ephemeral_public_key, 32 );
    memcpy( input + 64, server_ephemeral_public_key, 32 );
    memcpy( input + 96, connection_salt, 8 );
    memcpy( input + 104, session_nonce, 8 );

    rocketnet_derive_key( keys->session_client_to_server, token_client_to_server_key, input, 112, ROCKETNET_KDF_CONTEXT_SESSION_C2S );
    rocketnet_derive_key( keys->session_server_to_client, token_server_to_client_key, input, 112, ROCKETNET_KDF_CONTEXT_SESSION_S2C );

    sodium_memzero( shared_secret, sizeof( shared_secret ) );
    sodium_memzero( input, sizeof( input ) );

    return 1;
}

#endif // #ifndef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY
