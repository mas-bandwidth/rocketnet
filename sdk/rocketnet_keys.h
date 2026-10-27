/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_KEYS_H
#define ROCKETNET_SDK_ROCKETNET_KEYS_H

#include <stdint.h>

// Per-connection key derivation, per TRANSPORT.md t7 Key schedule. Token keys never seal a
// packet; every connection derives four working keys -- two handshake, two
// session -- from the token keys, the client's connection salt, the server's
// session nonce, and (for the session keys) the ephemeral exchange. All four
// are keyed BLAKE2b with the standard's context strings.

struct rocketnet_connection_keys_t
{
    uint8_t handshake_client_to_server[32];
    uint8_t handshake_server_to_client[32];
    uint8_t session_client_to_server[32];
    uint8_t session_server_to_client[32];
};

// derive the two handshake keys alone: enough to open a challenge or a denied

void rocketnet_derive_handshake_keys( struct rocketnet_connection_keys_t * keys,
                                      const uint8_t * token_client_to_server_key,
                                      const uint8_t * token_server_to_client_key,
                                      const uint8_t * connection_salt,
                                      const uint8_t * session_nonce );

// Derive all four keys. Computes the shared secret from the ephemeral pair,
// derives, then erases the shared secret from the stack before returning.
// Returns 0 -- refusing the whole derivation -- when the remote endpoint's ephemeral
// public key is a small-order point, which both sides MUST treat as fatal to
// the handshake. The caller erases its ephemeral secret once this returns.

int rocketnet_derive_connection_keys( struct rocketnet_connection_keys_t * keys,
                                      const uint8_t * token_client_to_server_key,
                                      const uint8_t * token_server_to_client_key,
                                      const uint8_t * connection_salt,
                                      const uint8_t * session_nonce,
                                      const uint8_t * own_ephemeral_secret_key,
                                      const uint8_t * remote_ephemeral_public_key,
                                      const uint8_t * client_ephemeral_public_key,
                                      const uint8_t * server_ephemeral_public_key );

#endif // ROCKETNET_SDK_ROCKETNET_KEYS_H
