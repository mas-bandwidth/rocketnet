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

/** @brief Connect-token format version written into signed data. */
#define ROCKETNET_VERSION                       0x4E7C1A93

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

/** Data sealed to the server inside a connect token. Keys and user data are sensitive. */
struct rocketnet_connect_token_private_t
{
    uint64_t client_id; ///< Application-assigned client identifier.
    uint64_t protocol_id; ///< Protocol identifier, equal to the signed protocol_id.
    uint64_t expire_timestamp; ///< Unix expiry time, equal to the signed expire_timestamp.
    uint8_t client_to_server_key[ROCKETNET_KEY_BYTES]; ///< Key for client-to-server traffic.
    uint8_t server_to_client_key[ROCKETNET_KEY_BYTES]; ///< Key for server-to-client traffic.
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES]; ///< Opaque bytes owned by the application; this API does not interpret them.
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

/** Data the backend signs and the client carries verbatim in each connect request. */
struct rocketnet_signed_connect_data_t
{
    uint32_t version; ///< Token format version.
    uint64_t protocol_id; ///< Protocol identifier shared by client and server.
    uint64_t create_timestamp; ///< Unix creation time in seconds.
    uint64_t expire_timestamp; ///< Unix expiry time in seconds.
    int32_t timeout_seconds; ///< Connection timeout in seconds.
    uint8_t address_flags; ///< Bitmask of ROCKETNET_ADDRESS_FLAG_IPV4 and ROCKETNET_ADDRESS_FLAG_IPV6.
    uint8_t ipv4_address[6]; ///< Four IPv4 octets followed by a little-endian port.
    uint8_t ipv6_address[18]; ///< Eight little-endian 16-bit groups followed by a little-endian port.
    uint8_t server_public_key[ROCKETNET_KEY_BYTES]; ///< Public key to which the private block is sealed.
    uint8_t sealed_private_block[ROCKETNET_SEALED_BLOCK_BYTES]; ///< Sealed private block for this server.
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

/** @brief Mint a complete token using fresh sealing randomness.
 * @param signed_fields [in] Non-NULL signed fields; protocol_id and expire_timestamp are copied into the private block.
 * @param private_fields [in] Non-NULL private values and directional keys.
 * @param backend_secret_key [in] Non-NULL 64-byte signing secret key.
 * @param token [out] Non-NULL output buffer of at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return 1 when sealing and signing succeed; 0 when a cryptographic operation fails.
 */

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

/** The regions of a complete connect token, split without validation. */
struct rocketnet_connect_token_t
{
    uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES]; ///< Signed fields.
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES]; ///< Signature over signed_connect_data.
    uint8_t client_to_server_key[ROCKETNET_KEY_BYTES]; ///< Clear client-to-server session key.
    uint8_t server_to_client_key[ROCKETNET_KEY_BYTES]; ///< Clear server-to-client session key.
};

/** @brief Split a fixed-size token into its four byte regions.
 * @param output [out] Non-NULL destination structure.
 * @param token [in] Non-NULL input buffer of at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return Nothing; this layout operation does not verify the signature or token contents.
 */
void rocketnet_read_connect_token( struct rocketnet_connect_token_t * output, const uint8_t * token );

/** Verify, open, and compare tokens as separate server-side steps. Authenticity comes from the detached signature check. */
/** @brief Verify the signature over the complete signed-data region.
 * @param signed_connect_data [in] Non-NULL ROCKETNET_SIGNED_CONNECT_DATA_BYTES bytes.
 * @param signature [in] Non-NULL ROCKETNET_SIGNATURE_BYTES detached signature.
 * @param backend_public_key [in] Non-NULL 32-byte public signing key.
 * @return 1 when the signature verifies; 0 otherwise.
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

/** @brief Hash signed connect data into its 16-byte content identifier.
 * @param identifier [out] Non-NULL buffer with room for ROCKETNET_TOKEN_IDENTIFIER_BYTES.
 * @param signed_connect_data [in] Non-NULL ROCKETNET_SIGNED_CONNECT_DATA_BYTES region.
 * @return Nothing; the hash does not include signature bytes.
 */
void rocketnet_connect_token_identifier( uint8_t * identifier, const uint8_t * signed_connect_data );


/** @brief Public signing key used by the development-token path.
 * @details Development keys are public test credentials; deployments using them have no token authenticity or session confidentiality.
 */
extern const uint8_t rocketnet_dev_backend_public_key[32];
#if ROCKETNET_DEVELOPMENT
/** @brief Development signing secret, available only in development builds. Never use it for production credentials. */
extern const uint8_t rocketnet_dev_backend_secret_key[64];
#endif // #if ROCKETNET_DEVELOPMENT
/** @brief Public server key used by development tokens. */
extern const uint8_t rocketnet_dev_server_public_key[32];
/** @brief Secret server key used by development tokens. Never use it for production sessions. */
extern const uint8_t rocketnet_dev_server_secret_key[32];

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

#if ROCKETNET_DEVELOPMENT
/** @brief Mint a local development token for a numeric server address.
 * @param server_address_string [in] Non-NULL numeric IPv4 or IPv6 address accepted by the address parser.
 * @param client_id [in] Application client identifier.
 * @param current_timestamp [in] Current Unix timestamp in seconds.
 * @param connect_token [out] Non-NULL buffer of at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return 1 on success; 0 for a NULL required pointer, invalid address, or cryptographic failure. Uses public development credentials and is unsuitable for production.
 */
ROCKETNET_EXPORT_FUNC int rocketnet_write_dev_connect_token( const char * server_address_string,
                                                             uint64_t client_id,
                                                             uint64_t current_timestamp,
                                                             uint8_t * connect_token );
#endif // #if ROCKETNET_DEVELOPMENT

#endif // #ifndef ROCKETNET_CONNECT_TOKEN_H
