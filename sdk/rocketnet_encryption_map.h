/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_ENCRYPTION_MAP_H
#define ROCKETNET_ENCRYPTION_MAP_H

#include <stdint.h>
#include "rocketnet_history.h"
#include "rocketnet_address.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_seams.h"

// The encryption mapping table: the server's per-address record of derived
// keys and handshake state, created only for a request that carried a valid
// backend signature. Constant time by address: chained buckets under a
// per-run SipHash of the canonical address bytes, parallel fixed arrays,
// the hot address array split from the cold mapping payload and walked
// alone by find.
//
// A mapping is bound by two independent deadlines: a handshake deadline
// fixed at creation and never refreshed -- connect within it or be
// discarded -- and an idle deadline refreshed by traffic, which bounds the
// mapping in either state and governs alone once the connection is
// established.

// TRANSPORT.md t12 rule 14 and t17 rule 4: a half-open mapping dies at
// whichever of its idle and handshake deadlines comes first. The idle clock
// starts when the mapping is created and every request from its address
// refreshes it.
#define ROCKETNET_HALF_OPEN_IDLE_TIMEOUT_SECONDS 10.0

struct rocketnet_replay_protection_t
{
    uint64_t highest_opened;
    uint64_t window[ROCKETNET_HISTORY_PACKETS / 64];
};

// Read on every server update for expiry. Keep this small and at the same
// index as the mapping's crypto/replay payload; the latter is never needed
// for the common non-expiring timeout check.
#define ROCKETNET_ENCRYPTION_MAPPING_CONTROL_LAYOUT_FIELDS( X ) \
    X( handshake_deadline, double handshake_deadline ) \
    X( last_receive_time, double last_receive_time ) \
    X( timeout_seconds, int32_t timeout_seconds ) \
    X( client_index, int client_index ) \
    X( established, int established )

struct rocketnet_encryption_mapping_control_t
{
#define ROCKETNET_ENCRYPTION_MAPPING_CONTROL_DECLARE( name, declaration ) declaration;
    ROCKETNET_ENCRYPTION_MAPPING_CONTROL_LAYOUT_FIELDS( ROCKETNET_ENCRYPTION_MAPPING_CONTROL_DECLARE )
#undef ROCKETNET_ENCRYPTION_MAPPING_CONTROL_DECLARE
};
static_assert( sizeof(struct rocketnet_encryption_mapping_control_t) == 32, "timeout control stays compact" );

/*
    The declaration manifest: the struct is declared from this list, so a
    member exists only as an entry here. The RESET/KEEP inventory in
    rocketnet_connection_reset.h is compared with it by name, and a member with
    no decision is named in test_connection_reset_inventory_covers_every_field.
*/
#define ROCKETNET_ENCRYPTION_MAPPING_LAYOUT_FIELDS( X ) \
    X( control, struct rocketnet_encryption_mapping_control_t * control ) \
    X( keys, struct rocketnet_connection_keys_t keys )                                                                                                             \
    X( connection_salt, uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES] )                                                                                 \
    X( session_nonce, uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES] )                                                                                       \
    X( server_ephemeral_public_key, uint8_t server_ephemeral_public_key[ROCKETNET_KEY_BYTES] )                                                                     \
    X( client_ephemeral_public_key, uint8_t client_ephemeral_public_key[ROCKETNET_KEY_BYTES] ) /* public; needed to build the type 3 AD */                         \
    X( challenge_packet, uint8_t challenge_packet[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES] )                                                                     \
    X( client_id, uint64_t client_id )                                                                                                                             \
    X( user_data, uint8_t user_data[ROCKETNET_USER_DATA_BYTES] )                                                                                                   \
    X( token_identifier, uint8_t token_identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES] )                                                                              \
    X( token_expire_timestamp, uint64_t token_expire_timestamp )                                                                                                   \
    X( last_send_time, double last_send_time ) /* refreshed by every send; suppresses keep-alives once one has been acknowledged */                                \
    X( last_keep_alive_send_time, double last_keep_alive_send_time ) /* refreshed by keep-alives alone; paces them until one has been acknowledged */              \
    X( send_sequence_handshake, uint64_t send_sequence_handshake ) /* next handshake-phase sequence to send */                                                     \
    X( send_sequence_session, uint64_t send_sequence_session ) /* next session-phase sequence to send */                                                           \
    X( send_fragment_payload_id, uint64_t send_fragment_payload_id ) /* next fragmented payload id to send, per _Fragmentation_ */                                 \
    X( replay_handshake, struct rocketnet_replay_protection_t replay_handshake )                                                                                   \
    X( replay_session, struct rocketnet_replay_protection_t replay_session )                                                                                       \
    X( accepted_session, struct rocketnet_replay_protection_t accepted_session ) \
    X( integrity_failures, uint64_t integrity_failures ) /* sealed packets that did not open under either key; ROCKETNET_INTEGRITY_LIMIT tears the mapping down */

struct rocketnet_encryption_mapping_t
{
#define ROCKETNET_ENCRYPTION_MAPPING_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    ROCKETNET_ENCRYPTION_MAPPING_LAYOUT_FIELDS( ROCKETNET_ENCRYPTION_MAPPING_LAYOUT_FIELDS_DECLARE )
#undef ROCKETNET_ENCRYPTION_MAPPING_LAYOUT_FIELDS_DECLARE
};

struct rocketnet_encryption_map_t;

struct rocketnet_encryption_map_t * rocketnet_encryption_map_create( void * context, int capacity );

void rocketnet_encryption_map_destroy( struct rocketnet_encryption_map_t * map );

// returns the mapping index for this address, or -1. one keyed hash, one
// short chain walk over the hot address array.

int rocketnet_encryption_map_find( const struct rocketnet_encryption_map_t * map, const struct rocketnet_address_t * address );

// 1 when no free mapping slot remains. A full map is refused on the request
// path before any signature, seal or keypair work.

int rocketnet_encryption_map_full( const struct rocketnet_encryption_map_t * map );

// Adds a mapping for this address, zero-initialized except the sending
// sequences, which start at one, and the client index, which starts at -1.
// Returns the index, or -1 when the table is full -- mappings are never
// evicted; they leave by their deadlines or by removal.

int rocketnet_encryption_map_add( struct rocketnet_encryption_map_t * map,
                                  const struct rocketnet_address_t * address,
                                  double handshake_deadline );

// the cold payload for an index returned by find or add

struct rocketnet_encryption_mapping_t * rocketnet_encryption_map_entry( struct rocketnet_encryption_map_t * map, int index );
struct rocketnet_encryption_mapping_control_t * rocketnet_encryption_map_control( struct rocketnet_encryption_map_t * map, int index );

// the address a live mapping is keyed on -- what the server's sends target

const struct rocketnet_address_t * rocketnet_encryption_map_address( const struct rocketnet_encryption_map_t * map, int index );

// Removes a mapping, zeroing all key material before the slot is freed.

void rocketnet_encryption_map_remove( struct rocketnet_encryption_map_t * map, int index );

// Iterates the live mappings: start with iterator = -1, call until it
// returns 0; each call yields the next live index. Removing the yielded
// index during iteration is safe -- that is what the server's sweep does.

int rocketnet_encryption_map_next( const struct rocketnet_encryption_map_t * map, int * iterator );

int rocketnet_encryption_map_num_mappings( const struct rocketnet_encryption_map_t * map );

#if ROCKETNET_TEST_SEAMS
// The keyed hashes this map has computed since create: one per find, add and
// remove. The unit suite pins the receive path's cost per packet with it.
uint64_t rocketnet_encryption_map_test_hashes( const struct rocketnet_encryption_map_t * map );
#endif // #if ROCKETNET_TEST_SEAMS

#endif // #ifndef ROCKETNET_ENCRYPTION_MAP_H
