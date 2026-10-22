/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONNECT_TOKEN_H
#define ROCKETNET_CONNECT_TOKEN_H

#include <stdint.h>
#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_seams.h"

// The SDK's C tools use these internal token helpers as well as its C++ code.
#ifdef __cplusplus
extern "C" {
#endif

// The connect token, per STANDARD.md. All layouts are packed little-endian at
// the exact offsets the standard states; the byte sizes here are the wire
// format and never change independently of it.

/** @brief Connect-token wire-format version stored in signed data. */
#define ROCKETNET_WIRE_VERSION                       0x4E7C1A93

/** @brief Byte size of the sealed private data block. */
#define ROCKETNET_PRIVATE_BLOCK_BYTES           384
/** @brief Byte size of the private block after public-key sealing. */
#define ROCKETNET_SEALED_BLOCK_BYTES            432     // private block + crypto_box_SEALBYTES
/** @brief Byte size of the signed connect-data region. */
#define ROCKETNET_SIGNED_CONNECT_DATA_BYTES     576
/** @brief Byte size of the detached signature. */
#define ROCKETNET_SIGNATURE_BYTES               64
/** @brief Byte size of a complete connect token. */
#define ROCKETNET_CONNECT_TOKEN_BYTES           768
/** @brief Byte capacity for application data inside the sealed private block. */
#define ROCKETNET_USER_DATA_BYTES               256
/** @brief Byte size of each directional session key. */
#define ROCKETNET_KEY_BYTES                     32
/** @brief Byte size of the unkeyed token identifier. */
#define ROCKETNET_TOKEN_IDENTIFIER_BYTES        16

/** @brief Bit marking the IPv4 address field as present. */
#define ROCKETNET_ADDRESS_FLAG_IPV4             (1<<0)
/** @brief Bit marking the IPv6 address field as present. */
#define ROCKETNET_ADDRESS_FLAG_IPV6             (1<<1)

// offsets inside the signed connect data, from the standard
/** @brief Byte offset of the format version in signed data. */
#define ROCKETNET_SIGNED_OFFSET_VERSION         0
/** @brief Byte offset of the protocol identifier in signed data. */
#define ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID     4
/** @brief Byte offset of the creation timestamp in signed data. */
#define ROCKETNET_SIGNED_OFFSET_CREATE          12
/** @brief Byte offset of the expiry timestamp in signed data. */
#define ROCKETNET_SIGNED_OFFSET_EXPIRE          20
/** @brief Byte offset of the connection timeout in signed data. */
#define ROCKETNET_SIGNED_OFFSET_TIMEOUT         28
/** @brief Byte offset of address-presence flags in signed data. */
#define ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS   32
/** @brief Byte offset of the IPv4 endpoint bytes in signed data. */
#define ROCKETNET_SIGNED_OFFSET_IPV4            33
/** @brief Byte offset of the IPv6 endpoint bytes in signed data. */
#define ROCKETNET_SIGNED_OFFSET_IPV6            39
/** @brief Byte offset of the server public key in signed data. */
#define ROCKETNET_SIGNED_OFFSET_SERVER_KEY      57
/** @brief Byte offset of the sealed private block in signed data. */
#define ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK    89

// The private connect token data: what the backend seals to the server.

/** Data sealed to the server inside a connect token. Keys and user data are sensitive. */
struct rocketnet_connect_token_private_t
{
    /** @brief Application-assigned client identifier. */
    uint64_t client_id;
    /** @brief Protocol identifier shared by client and server. */
    uint64_t protocol_id;
    /** @brief Unix expiry time in seconds. */
    uint64_t expire_timestamp;
    /** @brief Private client-to-server session key. */
    uint8_t client_to_server_key[ROCKETNET_KEY_BYTES];
    /** @brief Private server-to-client session key. */
    uint8_t server_to_client_key[ROCKETNET_KEY_BYTES];
    /** @brief Opaque bytes owned by the application; this API does not interpret them. */
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
};

/** @brief Serialize private fields into the fixed-size private block.
 * @param input [in] Non-NULL fields to serialize.
 * @param buffer [out] Non-NULL output with room for ROCKETNET_PRIVATE_BLOCK_BYTES.
 * @return Nothing; integer fields are written little-endian and remaining bytes are zero-filled.
 */
void rocketnet_write_connect_token_private( const struct rocketnet_connect_token_private_t * input, uint8_t * buffer );

/** @brief Read private fields from a serialized private block.
 * @param output [out] Non-NULL destination structure.
 * @param buffer [in] Non-NULL block containing at least ROCKETNET_PRIVATE_BLOCK_BYTES.
 * @return Nothing.
 */
void rocketnet_read_connect_token_private( struct rocketnet_connect_token_private_t * output, const uint8_t * buffer );

// The signed connect data: what the backend signs, carried verbatim in the
// token and in every connection request.

/** Data the backend signs and the client carries verbatim in each connect request. */
struct rocketnet_signed_connect_data_t
{
    /** @brief Token format version. */
    uint32_t version;
    /** @brief Protocol identifier shared by client and server. */
    uint64_t protocol_id;
    /** @brief Unix creation time in seconds. */
    uint64_t create_timestamp;
    /** @brief Unix expiry time in seconds. */
    uint64_t expire_timestamp;
    /** @brief Connection timeout in seconds. */
    int32_t timeout_seconds;
    /** @brief Bitmask of ROCKETNET_ADDRESS_FLAG_IPV4 and ROCKETNET_ADDRESS_FLAG_IPV6. */
    uint8_t address_flags;
    /** @brief Four IPv4 octets and a little-endian port. */
    uint8_t ipv4_address[6];                        // a,b,c,d octets + port little-endian
    /** @brief Eight little-endian IPv6 groups and a little-endian port. */
    uint8_t ipv6_address[18];                       // 8 x uint16 groups little-endian + port little-endian
    /** @brief Public key to which the private block is sealed. */
    uint8_t server_public_key[ROCKETNET_KEY_BYTES];
    /** @brief Sealed private block for this server. */
    uint8_t sealed_private_block[ROCKETNET_SEALED_BLOCK_BYTES];
};

/** @brief Serialize signed fields into the fixed-size signed-data region.
 * @param input [in] Non-NULL fields to serialize.
 * @param buffer [out] Non-NULL output with room for ROCKETNET_SIGNED_CONNECT_DATA_BYTES.
 * @return Nothing; integer fields are little-endian and unused bytes are zero-filled.
 */
void rocketnet_write_signed_connect_data( const struct rocketnet_signed_connect_data_t * input, uint8_t * buffer );

/** @brief Read fields from serialized signed connect data.
 * @param output [out] Non-NULL destination structure.
 * @param buffer [in] Non-NULL region containing at least ROCKETNET_SIGNED_CONNECT_DATA_BYTES.
 * @return Nothing.
 */
void rocketnet_read_signed_connect_data( struct rocketnet_signed_connect_data_t * output, const uint8_t * buffer );

// Minting, the backend's job. Seals the private block to the server public
// key, fills and signs the signed connect data, appends the clear token keys,
// and pads to the fixed token size. The private fields' protocol id and
// expire are taken from the signed fields, as the standard requires them
// equal. Returns 1 on success, 0 on a crypto failure.

/** @brief Mint a complete token with fresh ephemeral sealing randomness.
 * @param signed_fields [in] Non-NULL signed data; protocol_id and expire_timestamp also set the private block values.
 * @param private_fields [in] Non-NULL client id, keys, and user data.
 * @param backend_secret_key [in] Non-NULL 64-byte signing secret key.
 * @param token [out] Non-NULL output buffer of at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return 1 on success; 0 when a cryptographic operation fails.
 */
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

/** @brief Mint a deterministic token for known-answer tests only.
 * @param signed_fields [in] Non-NULL signed fields.
 * @param private_fields [in] Non-NULL private fields and session keys.
 * @param backend_secret_key [in] Non-NULL 64-byte signing secret key.
 * @param ephemeral_secret_key [in] Non-NULL 32-byte ephemeral X25519 secret; reusing it links tokens.
 * @param token [out] Non-NULL output buffer of at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return 1 on success; 0 on a crypto failure or when ephemeral_secret_key is NULL. Do not use this test-only routine to mint production tokens.
 */
int rocketnet_write_connect_token_deterministic( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                 const struct rocketnet_connect_token_private_t * private_fields,
                                                 const uint8_t * backend_secret_key,
                                                 const uint8_t * ephemeral_secret_key,
                                                 uint8_t * token );

// Client-side split of a 768-byte token into its parts. Pure layout.

/** The regions of a complete connect token, split without validation. */
struct rocketnet_connect_token_t
{
    /** @brief Signed fields. */
    uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    /** @brief Signature over signed_connect_data. */
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    /** @brief Clear client-to-server session key. */
    uint8_t client_to_server_key[ROCKETNET_KEY_BYTES];
    /** @brief Clear server-to-client session key. */
    uint8_t server_to_client_key[ROCKETNET_KEY_BYTES];
};

/** @brief Split a fixed-size token into its four byte regions.
 * @param output [out] Non-NULL destination structure.
 * @param token [in] Non-NULL input buffer of at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return Nothing; this layout operation does not verify the signature or token contents.
 */
void rocketnet_read_connect_token( struct rocketnet_connect_token_t * output, const uint8_t * token );

// Server-side processing, in the standard's order: verify the signature over
// the signed connect data against the backend public key -- authenticity is
// this and only this -- then open the sealed block, then check the inner
// fields agree with the signed ones. Each returns 1 on success, 0 on refusal.

/** @brief Verify the detached signature over signed connect data.
 * @param signed_connect_data [in] Non-NULL signed region of ROCKETNET_SIGNED_CONNECT_DATA_BYTES.
 * @param signature [in] Non-NULL ROCKETNET_SIGNATURE_BYTES-byte detached signature.
 * @param backend_public_key [in] Non-NULL 32-byte backend signing public key.
 * @return 1 if the signature verifies; 0 otherwise. Verify before opening the sealed block.
 */
int rocketnet_verify_signed_connect_data( const uint8_t * signed_connect_data,
                                          const uint8_t * signature,
                                          const uint8_t * backend_public_key );

/** @brief Open the sealed private block from signed connect data.
 * @param output [out] Non-NULL destination; written only after successful open.
 * @param signed_connect_data [in] Non-NULL complete signed-data region.
 * @param server_public_key [in] Non-NULL 32-byte server public key.
 * @param server_secret_key [in] Non-NULL 32-byte matching server secret key.
 * @return 1 when the sealed block opens; 0 when authentication fails. Verify the signed data before opening it.
 */
int rocketnet_open_connect_token_private( struct rocketnet_connect_token_private_t * output,
                                          const uint8_t * signed_connect_data,
                                          const uint8_t * server_public_key,
                                          const uint8_t * server_secret_key );

/** @brief Check the private protocol and expiry values against signed fields.
 * @param private_fields [in] Non-NULL private data.
 * @param signed_connect_data [in] Non-NULL signed-data region.
 * @return 1 when both values match; 0 otherwise.
 */
int rocketnet_connect_token_private_consistent( const struct rocketnet_connect_token_private_t * private_fields,
                                                const uint8_t * signed_connect_data );

// The token identifier: the 16-byte unkeyed generichash of the signed connect
// data. Content-determined, never the signature bytes.

/** @brief Hash signed connect data into its 16-byte content identifier.
 * @param identifier [out] Non-NULL buffer with room for ROCKETNET_TOKEN_IDENTIFIER_BYTES.
 * @param signed_connect_data [in] Non-NULL ROCKETNET_SIGNED_CONNECT_DATA_BYTES region.
 * @return Nothing; the hash does not include signature bytes.
 */
void rocketnet_connect_token_identifier( uint8_t * identifier, const uint8_t * signed_connect_data );


// The published development keys, embedded and used by default so
// development works out of the box: run the server, point a client at its
// address, connected -- no backend, no configuration, no copying keys
// between machines. THE SECURITY OF A DEPLOYMENT THAT SHIPS WITH THESE IS
// ZERO: both secret halves are printed in the standard, so anyone can mint
// tokens your server accepts and open every session. The keys being public
// is the loud failure; nothing else is relaxed -- the development path runs
// the same handshake, sealing and replay protection that ships.

/** @brief Public signing key used by the development-token path.
 * @details Development keys are public test credentials; deployments using them have no token authenticity or session confidentiality.
 */
extern const uint8_t rocketnet_dev_backend_public_key[32];
#if ROCKETNET_DEV
/** @brief Development signing secret, available only in development builds. Never use it for production credentials. */
extern const uint8_t rocketnet_dev_backend_secret_key[64];
#endif // #if ROCKETNET_DEV
/** @brief Public server key used by development tokens. */
extern const uint8_t rocketnet_dev_server_public_key[32];
#if ROCKETNET_TEST_SEAMS
/** @brief Secret server key used by development tokens. Never use it for production sessions. Absent from a production build (ROCKETNET_DEV=0). */
extern const uint8_t rocketnet_dev_server_secret_key[32];
#endif // #if ROCKETNET_TEST_SEAMS

/** @brief Protocol identifier used by development tokens. */
#define ROCKETNET_DEV_PROTOCOL_ID 0x1122334455667788ULL

// A development token lives 30 seconds from its create timestamp, as a
// netcode.io token does by default: a connect token is meant to be used
// once, soon after it is minted, and the short expiry is what bounds reuse
// of a token whose connection has closed (docs/SESSION.md, Token reuse). A
// production backend sets its own expiry in the signed fields; 30 seconds is
// the recommended value.

/** @brief Lifetime in seconds of a development token. */
#define ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS 30

// The connection timeout the development token carries, in seconds.
/** @brief Connection timeout in seconds carried by a development token. */
#define ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS 10

// The base64 string of a token fills the public string buffer exactly.
// The sealed private block sits inside the signed connect data, which the
// connection request carries whole: each fits inside the request, and the
// request inside one IPv6 datagram (sdk/rocketnet_wire.h).
ROCKETNET_STATIC_ASSERT( ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES <= ROCKETNET_SIGNED_CONNECT_DATA_BYTES, "the sealed private block fits the signed connect data" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SIGNED_CONNECT_DATA_BYTES < ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, "the signed connect data fits the connection request" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES <= ROCKETNET_MAX_PACKET_BYTES, "the connection request fits one IPv6 datagram" );

ROCKETNET_STATIC_ASSERT( ( ( ROCKETNET_CONNECT_TOKEN_BYTES + 2 ) / 3 ) * 4 + 1 == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES, "ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES is the base64 of the token" );

// The key and token makers the public header does not carry: production
// minting and key generation belong to the game's backend; the SDK's own tests
// and tools use these. Not exported.
enum rocketnet_status_t rocketnet_generate_backend_keypair( uint8_t * public_key, uint8_t * secret_key );
enum rocketnet_status_t rocketnet_generate_backend_keypair_from_seed( const uint8_t * seed, uint8_t * public_key, uint8_t * secret_key );
enum rocketnet_status_t rocketnet_generate_server_keypair( uint8_t * public_key, uint8_t * secret_key );
enum rocketnet_status_t rocketnet_write_production_connect_token( const uint8_t * backend_secret_key,
                                                                  const uint8_t * server_public_key,
                                                                  uint64_t protocol_id,
                                                                  const char * const * server_address_strings,
                                                                  int server_address_count,
                                                                  uint64_t client_id,
                                                                  uint64_t create_timestamp,
                                                                  uint64_t expire_timestamp,
                                                                  int32_t timeout_seconds,
                                                                  const uint8_t * user_data,
                                                                  uint8_t * connect_token,
                                                                  size_t connect_token_bytes );

#if ROCKETNET_DEV
// The layer tests' binary token for one address, sealed to the fixed test
// server key (rocketnet_dev_server_public_key) that the internal transport
// dev config carries. Public programs use rocketnet_create_dev_connect_token.
enum rocketnet_status_t rocketnet_write_test_connect_token( const char * server_address_string, uint64_t client_id, uint64_t current_timestamp, uint8_t * connect_token, size_t connect_token_bytes );
#endif // #if ROCKETNET_DEV

#ifdef __cplusplus
}
#endif

#endif // #ifndef ROCKETNET_CONNECT_TOKEN_H
