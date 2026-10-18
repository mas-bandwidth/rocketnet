/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONNECT_TOKEN_H
#define ROCKETNET_CONNECT_TOKEN_H

#include <stdint.h>
#include "rocketnet.h"

// The connect token, per STANDARD.md. All layouts are packed little-endian at
// the exact offsets the standard states; the byte sizes here are the wire
// format and never change independently of it.

#define ROCKETNET_VERSION                       0x4E7C1A93

#define ROCKETNET_PRIVATE_BLOCK_BYTES           384
#define ROCKETNET_SEALED_BLOCK_BYTES            432     // private block + crypto_box_SEALBYTES
#define ROCKETNET_SIGNED_CONNECT_DATA_BYTES     576
#define ROCKETNET_SIGNATURE_BYTES               64
#define ROCKETNET_CONNECT_TOKEN_BYTES           768
#define ROCKETNET_USER_DATA_BYTES               256
#define ROCKETNET_KEY_BYTES                     32
#define ROCKETNET_TOKEN_IDENTIFIER_BYTES        16

#define ROCKETNET_ADDRESS_FLAG_IPV4             (1<<0)
#define ROCKETNET_ADDRESS_FLAG_IPV6             (1<<1)

// offsets inside the signed connect data, from the standard
#define ROCKETNET_SIGNED_OFFSET_VERSION         0
#define ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID     4
#define ROCKETNET_SIGNED_OFFSET_CREATE          12
#define ROCKETNET_SIGNED_OFFSET_EXPIRE          20
#define ROCKETNET_SIGNED_OFFSET_TIMEOUT         28
#define ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS   32
#define ROCKETNET_SIGNED_OFFSET_IPV4            33
#define ROCKETNET_SIGNED_OFFSET_IPV6            39
#define ROCKETNET_SIGNED_OFFSET_SERVER_KEY      57
#define ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK    89

// The private connect token data: what the backend seals to the server.

struct rocketnet_connect_token_private_t
{
    uint64_t client_id;
    uint64_t protocol_id;
    uint64_t expire_timestamp;
    uint8_t client_to_server_key[ROCKETNET_KEY_BYTES];
    uint8_t server_to_client_key[ROCKETNET_KEY_BYTES];
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
};

void rocketnet_write_connect_token_private( const struct rocketnet_connect_token_private_t * input, uint8_t * buffer );

void rocketnet_read_connect_token_private( struct rocketnet_connect_token_private_t * output, const uint8_t * buffer );

// The signed connect data: what the backend signs, carried verbatim in the
// token and in every connection request.

struct rocketnet_signed_connect_data_t
{
    uint32_t version;
    uint64_t protocol_id;
    uint64_t create_timestamp;
    uint64_t expire_timestamp;
    int32_t timeout_seconds;
    uint8_t address_flags;
    uint8_t ipv4_address[6];                        // a,b,c,d octets + port little-endian
    uint8_t ipv6_address[18];                       // 8 x uint16 groups little-endian + port little-endian
    uint8_t server_public_key[ROCKETNET_KEY_BYTES];
    uint8_t sealed_private_block[ROCKETNET_SEALED_BLOCK_BYTES];
};

void rocketnet_write_signed_connect_data( const struct rocketnet_signed_connect_data_t * input, uint8_t * buffer );

void rocketnet_read_signed_connect_data( struct rocketnet_signed_connect_data_t * output, const uint8_t * buffer );

// Minting, the backend's job. Seals the private block to the server public
// key, fills and signs the signed connect data, appends the clear token keys,
// and pads to the fixed token size. The private fields' protocol id and
// expire are taken from the signed fields, as the standard requires them
// equal. Returns 1 on success, 0 on a crypto failure.

int rocketnet_write_connect_token( const struct rocketnet_signed_connect_data_t * signed_fields,
                                   const struct rocketnet_connect_token_private_t * private_fields,
                                   const uint8_t * backend_secret_key,
                                   uint8_t * token );

// TEST-ONLY. Exists for the known-answer vectors in
// test/connect_token_vectors.json, which another implementation of the
// backend's minting is checked against byte for byte; nothing ships calling
// it. The same token as rocketnet_write_connect_token, except the private
// block is sealed with the caller's 32-byte ephemeral X25519 secret instead
// of a freshly drawn one -- exactly crypto_box_seal's construction:
// epk = crypto_scalarmult_base(esk), nonce = crypto_generichash(24 bytes,
// epk || server_pk), output epk || crypto_box_easy(block, nonce, server_pk,
// esk). The token keys come from private_fields, as in the production writer.
// A reused ephemeral secret ties tokens together, so it is never exported
// (kept out of tools/exports.txt) and never used to mint a real token.
// Returns 1 on success, 0 on a crypto failure.

int rocketnet_write_connect_token_deterministic( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                 const struct rocketnet_connect_token_private_t * private_fields,
                                                 const uint8_t * backend_secret_key,
                                                 const uint8_t * ephemeral_secret_key,
                                                 uint8_t * token );

// Client-side split of a 768-byte token into its parts. Pure layout.

struct rocketnet_connect_token_t
{
    uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    uint8_t client_to_server_key[ROCKETNET_KEY_BYTES];
    uint8_t server_to_client_key[ROCKETNET_KEY_BYTES];
};

void rocketnet_read_connect_token( struct rocketnet_connect_token_t * output, const uint8_t * token );

// Server-side processing, in the standard's order: verify the signature over
// the signed connect data against the backend public key -- authenticity is
// this and only this -- then open the sealed block, then check the inner
// fields agree with the signed ones. Each returns 1 on success, 0 on refusal.

int rocketnet_verify_signed_connect_data( const uint8_t * signed_connect_data,
                                          const uint8_t * signature,
                                          const uint8_t * backend_public_key );

int rocketnet_open_connect_token_private( struct rocketnet_connect_token_private_t * output,
                                          const uint8_t * signed_connect_data,
                                          const uint8_t * server_public_key,
                                          const uint8_t * server_secret_key );

int rocketnet_connect_token_private_consistent( const struct rocketnet_connect_token_private_t * private_fields,
                                                const uint8_t * signed_connect_data );

// The token identifier: the 16-byte unkeyed generichash of the signed connect
// data. Content-determined, never the signature bytes.

void rocketnet_connect_token_identifier( uint8_t * identifier, const uint8_t * signed_connect_data );


// The published development keys, embedded and used by default so
// development works out of the box: run the server, point a client at its
// address, connected -- no backend, no configuration, no copying keys
// between machines. THE SECURITY OF A DEPLOYMENT THAT SHIPS WITH THESE IS
// ZERO: both secret halves are printed in the standard, so anyone can mint
// tokens your server accepts and open every session. The keys being public
// is the loud failure; nothing else is relaxed -- the development path runs
// the same handshake, sealing and replay protection that ships.

extern const uint8_t rocketnet_dev_backend_public_key[32];
#if ROCKETNET_DEVELOPMENT
extern const uint8_t rocketnet_dev_backend_secret_key[64];
#endif // #if ROCKETNET_DEVELOPMENT
extern const uint8_t rocketnet_dev_server_public_key[32];
extern const uint8_t rocketnet_dev_server_secret_key[32];

#define ROCKETNET_DEV_PROTOCOL_ID 0x1122334455667788ULL

// A development token lives 30 seconds from its create timestamp, as a
// netcode.io token does by default: a connect token is meant to be used
// once, soon after it is minted, and the short expiry is what bounds reuse
// of a token whose connection has closed (docs/SESSION.md, Token reuse). A
// production backend sets its own expiry in the signed fields; 30 seconds is
// the recommended value.

#define ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS 30

// Mints a development connect token for a server at this address, signed
// with the published development backend key and sealed to the published
// development server key. What a real backend does with real keys, done
// locally so a first connect needs nothing else. Returns 1, or 0 for an
// address that does not parse.

// C linkage: this one is also the public dev helper on the rocketnet.h
// surface, and the two declarations must agree -- via the same macro, because
// in the shared build ROCKETNET_EXPORT_FUNC carries __declspec(dllexport) and
// MSVC hard-errors (C2375) on a plain extern "C" redeclaration of it. gcc and
// clang merge the two silently, which is how this shipped unnoticed until the
// first windows build.

#if ROCKETNET_DEVELOPMENT
ROCKETNET_EXPORT_FUNC int rocketnet_write_dev_connect_token( const char * server_address_string,
                                                             uint64_t client_id,
                                                             uint64_t current_timestamp,
                                                             uint8_t * connect_token );
#endif // #if ROCKETNET_DEVELOPMENT

#endif // #ifndef ROCKETNET_CONNECT_TOKEN_H
