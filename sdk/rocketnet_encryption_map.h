/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_ENCRYPTION_MAP_H
#define ROCKETNET_ENCRYPTION_MAP_H

#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"

// The encryption mapping table: the server's per-address record of derived
// keys and handshake state, created only for a request that carried a valid
// backend signature. Constant time by address: chained buckets under a
// per-run SipHash of the canonical address bytes, parallel fixed arrays,
// the hot address array split from the cold mapping payload and walked
// alone by find.
//
// A mapping is bound by two independent deadlines: a handshake deadline
// fixed at creation and never refreshed -- connect within it or be
// discarded -- and an idle deadline refreshed by traffic, which governs
// alone once the connection is established.

struct rocketnet_replay_protection_t
{
    uint64_t highest_opened;
    uint64_t window[4];                     // 256-entry bitmask, the standard's floor
};

struct rocketnet_encryption_mapping_t
{
    struct rocketnet_connection_keys_t keys;
    uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    uint8_t server_ephemeral_public_key[ROCKETNET_KEY_BYTES];
    uint8_t client_ephemeral_public_key[ROCKETNET_KEY_BYTES];   // public; needed to build the type 3 AD
    uint8_t challenge_packet[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    uint64_t client_id;
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    uint8_t token_identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    uint64_t token_expire_timestamp;
    int32_t timeout_seconds;
    int established;
    int client_index;                       // -1 until a slot is assigned
    double handshake_deadline;              // absolute, never refreshed
    double last_receive_time;               // refreshed by every opened packet
    double last_send_time;                  // refreshed by every send; suppresses keep-alives once the client is confirmed
    double last_keep_alive_send_time;       // refreshed by keep-alives alone; paces them while the client is unconfirmed
    uint64_t send_sequence_handshake;       // next handshake-phase sequence to send
    uint64_t send_sequence_session;         // next session-phase sequence to send
    uint64_t send_fragment_payload_id;      // next fragmented payload id to send, per _Fragmentation_
    struct rocketnet_replay_protection_t replay_handshake;
    struct rocketnet_replay_protection_t replay_session;
};

struct rocketnet_encryption_map_t;

struct rocketnet_encryption_map_t * rocketnet_encryption_map_create( void * context, int capacity );

void rocketnet_encryption_map_destroy( struct rocketnet_encryption_map_t * map );

// returns the mapping index for this address, or -1. one keyed hash, one
// short chain walk over the hot address array.

int rocketnet_encryption_map_find( const struct rocketnet_encryption_map_t * map, const struct rocketnet_address_t * address );

// Adds a mapping for this address, zero-initialized except the sending
// sequences, which start at one, and the client index, which starts at -1.
// Returns the index, or -1 when the table is full -- mappings are never
// evicted; they leave by their deadlines or by removal.

int rocketnet_encryption_map_add( struct rocketnet_encryption_map_t * map,
                                  const struct rocketnet_address_t * address,
                                  double handshake_deadline );

// the cold payload for an index returned by find or add

struct rocketnet_encryption_mapping_t * rocketnet_encryption_map_entry( struct rocketnet_encryption_map_t * map, int index );

// the address a live mapping is keyed on -- what the server's sends target

const struct rocketnet_address_t * rocketnet_encryption_map_address( const struct rocketnet_encryption_map_t * map, int index );

// Removes a mapping, zeroing all key material before the slot is freed.

void rocketnet_encryption_map_remove( struct rocketnet_encryption_map_t * map, int index );

// Iterates the live mappings: start with iterator = -1, call until it
// returns 0; each call yields the next live index. Removing the yielded
// index during iteration is safe -- that is what the server's sweep does.

int rocketnet_encryption_map_next( const struct rocketnet_encryption_map_t * map, int * iterator );

int rocketnet_encryption_map_num_mappings( const struct rocketnet_encryption_map_t * map );

#endif // #ifndef ROCKETNET_ENCRYPTION_MAP_H
