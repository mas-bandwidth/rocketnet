/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_PROTOCOL_H
#define ROCKETNET_SDK_ROCKETNET_PROTOCOL_H

#include <stdint.h>

// The connection protocol wire layer, per STANDARD.md. This module grows with
// the standard's own build order: the prefilter header, then packet layouts,
// sealing, and the reading order.

// The packet types, the prefilter header, the receive ceiling and the wire
// size table live in rocketnet_wire.h, shared with the server_xdp kernel
// program so the two filters can never drift apart.

#include "rocketnet_wire.h"

void rocketnet_write_prefilter_header( uint8_t ** p, uint8_t packet_type, const uint8_t * packet_filter );

// The prefilter structural check from the standard's reading order: the
// packet is at least a prefilter header, no larger than the receive ceiling,
// and carries this title's tag. Returns the packet type on success, -1 on
// refusal. Deliberately knows nothing about types or per-type sizes; those
// are later steps of the reading order.

// the refusal codes, one per distinct reason, so a caller counts which branch
// it hit rather than collapsing them -- the standard requires one counter per
// reason, and the kernel filter already splits them the same way

#define ROCKETNET_PREFILTER_TOO_SHORT       -1
#define ROCKETNET_PREFILTER_TOO_LARGE       -2
#define ROCKETNET_PREFILTER_WRONG_EPOCH     -3
#define ROCKETNET_PREFILTER_WRONG_VALUE     -4

int rocketnet_prefilter_check( const uint8_t * packet_data, int packet_bytes, const uint8_t * packet_filter );

int rocketnet_prefilter_counter( int refusal_code );

#define ROCKETNET_SEQUENCE_BYTES                    4
#define ROCKETNET_SESSION_NONCE_BYTES               8
#define ROCKETNET_PACKET_TAG_BYTES                  16
#define ROCKETNET_PACKET_OVERHEAD_BYTES             ( ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + ROCKETNET_PACKET_TAG_BYTES )      // 26
#define ROCKETNET_RESPONSE_PADDING_BYTES            24
#define ROCKETNET_COOKIE_FIELD_BYTES                24      // expire + mac, echoed in requests
#define ROCKETNET_COOKIE_MAC_BYTES                  16
#define ROCKETNET_CONNECTION_SALT_BYTES             8

// the associated data: version, protocol id, packet type and connection salt,
// plus the client ephemeral public key on types 2 and 3 only. constructed by
// both ends, never on the wire.

#define ROCKETNET_AD_BYTES                          21
#define ROCKETNET_AD_WITH_EPHEMERAL_BYTES           53

// returns the associated data length written: 53 when client_ephemeral_public_key
// is provided (types 2 and 3), 21 otherwise

int rocketnet_write_packet_associated_data( uint8_t * associated_data,
                                            uint64_t protocol_id,
                                            uint8_t packet_type,
                                            const uint8_t * connection_salt,
                                            const uint8_t * client_ephemeral_public_key );

// Sealed packet construction. Builds prefilter header, the low 32 bits of the
// sequence, the clear session nonce on types 1 and 2, then the data sealed
// with ChaCha20-Poly1305 under the sequence-derived nonce and the associated
// data above. Returns the wire size, or 0 on refusal. session_nonce is
// required for types 1 and 2 and must be NULL otherwise;
// client_ephemeral_public_key is required for types 2 and 3 and must be NULL
// otherwise.

// The payload seal in place: the payload sits at wire_start + 10 + the
// 7-byte minimum ack block, and the block (built by the caller from its
// receive state, truncated to the budget's room) lands in front of it --
// exactly there when it is the minimum, moving the payload up by the extra
// bytes otherwise. The frame layout in rocketnet_constants.h reserves the
// room.

int rocketnet_write_sealed_payload_in_place( uint8_t * wire_start,
                                             const uint8_t * packet_filter,
                                             uint64_t sequence,
                                             const uint8_t * ack_block,
                                             int ack_block_bytes,
                                             int payload_bytes,
                                             uint64_t protocol_id,
                                             const uint8_t * connection_salt,
                                             const uint8_t * key );

int rocketnet_write_sealed_packet( uint8_t * packet_data,
                                   uint8_t packet_type,
                                   const uint8_t * packet_filter,
                                   uint64_t sequence,
                                   const uint8_t * session_nonce,
                                   const uint8_t * data,
                                   int data_bytes,
                                   uint64_t protocol_id,
                                   const uint8_t * connection_salt,
                                   const uint8_t * client_ephemeral_public_key,
                                   const uint8_t * key );

// Sealed packet opening, the inverse. The caller has already run the
// prefilter check and reconstructed the 64-bit sequence; this locates the
// sealed region for the type, reads the clear session nonce on types 1 and 2
// into out_session_nonce (which those types require), and opens. Returns the
// opened data length, or -1 on refusal. Refusal reasons are deliberately not
// distinguished here; the reading order counts them at its own steps.

int rocketnet_open_sealed_packet( uint8_t * out_data,
                                  uint8_t * out_session_nonce,
                                  int out_capacity,
                                  const uint8_t * packet_data,
                                  int packet_bytes,
                                  uint8_t packet_type,
                                  uint64_t sequence,
                                  uint64_t protocol_id,
                                  const uint8_t * connection_salt,
                                  const uint8_t * client_ephemeral_public_key,
                                  const uint8_t * key );

// the two unsealed packets

void rocketnet_write_connection_request_packet( uint8_t * packet_data,
                                                const uint8_t * packet_filter,
                                                const uint8_t * return_cookie,        // 24 bytes, or NULL for all zero
                                                const uint8_t * connection_salt,
                                                const uint8_t * client_ephemeral_public_key,
                                                const uint8_t * signed_connect_data,
                                                const uint8_t * signature );

void rocketnet_write_connection_cookie_packet( uint8_t * packet_data,
                                               const uint8_t * packet_filter,
                                               const uint8_t * connection_salt,
                                               uint64_t cookie_expire_timestamp,
                                               const uint8_t * cookie_mac );

// Sequence reconstruction, from the standard: candidates share the wire
// value's low 32 bits; take the one nearest to the highest successfully
// opened sequence plus one, and on an exact tie take the larger. Never
// returns a wrong acceptance -- a wrong reconstruction fails the open -- so
// this is for determinism between implementations.

uint64_t rocketnet_reconstruct_sequence( uint32_t wire_sequence, uint64_t highest_opened );

// the nearest-candidate rule alone, for the ack block's largest received,
// whose expected value is the sender's own highest sent sequence directly

uint64_t rocketnet_reconstruct_near( uint32_t wire_value, uint64_t expected );

// The cookie: address validation before any asymmetric work. The MAC is a
// 16-byte keyed generichash over the exact input encoding from the standard:
// family byte, the address bytes as the token encodes them, the port
// little-endian, then the connection salt and the cookie expire timestamp.

#define ROCKETNET_COOKIE_KEY_BYTES              32
#define ROCKETNET_COOKIE_MAC_INPUT_IPV4_BYTES   23
#define ROCKETNET_COOKIE_MAC_INPUT_IPV6_BYTES   35

struct rocketnet_address_t;

// the canonical address byte encoding: family byte, address bytes as the
// token encodes them, port little-endian. 7 bytes IPv4, 19 IPv6. used by the
// cookie mac input and by address hashing.

#define ROCKETNET_ADDRESS_BYTES_IPV4    7
#define ROCKETNET_ADDRESS_BYTES_IPV6    19

// returns the length written, or 0 for an address that is not IPv4 or IPv6

int rocketnet_write_address_bytes( uint8_t * output, const struct rocketnet_address_t * address );

// returns the input length written, or 0 for an address that is not IPv4 or IPv6

int rocketnet_write_cookie_mac_input( uint8_t * input,
                                      const struct rocketnet_address_t * address,
                                      const uint8_t * connection_salt,
                                      uint64_t cookie_expire_timestamp );

// returns 1 and writes the 16-byte mac, or 0 for a bad address family

int rocketnet_generate_cookie_mac( uint8_t * mac,
                                   const struct rocketnet_address_t * address,
                                   const uint8_t * connection_salt,
                                   uint64_t cookie_expire_timestamp,
                                   const uint8_t * cookie_key );

#endif // ROCKETNET_SDK_ROCKETNET_PROTOCOL_H
