/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CLIENT_SLOTS_H
#define ROCKETNET_CLIENT_SLOTS_H

#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"

// Client slots: the server's fixed set of connected clients, allocated once
// at create. A slot is assigned only in step 3 of the handshake, holds the
// identity recovered from the connect token and a reference to the
// encryption mapping, and is found by client id in constant time -- a
// chained map under a per-run SipHash key, because the client-id check runs
// on every connection request and must not be linear in the slot count.
// Per-address lookup belongs to the encryption map, which already owns the
// address; a slot never duplicates it.

struct rocketnet_client_slots_t;

struct rocketnet_client_slots_t * rocketnet_client_slots_create( void * context, int max_clients );

void rocketnet_client_slots_destroy( struct rocketnet_client_slots_t * slots );

int rocketnet_client_slots_max_clients( const struct rocketnet_client_slots_t * slots );

int rocketnet_client_slots_num_connected( const struct rocketnet_client_slots_t * slots );

int rocketnet_client_slots_num_free( const struct rocketnet_client_slots_t * slots );

// returns the client index for this client id, or -1

int rocketnet_client_slots_find( const struct rocketnet_client_slots_t * slots, uint64_t client_id );

// Assigns a free slot from the free list (LIFO): unconfirmed, identity copied in, bound to
// the mapping index. Returns the client index, or -1 when full. The caller
// checks for a duplicate client id before assigning; assigning a duplicate
// is a bug, and asserts.

int rocketnet_client_slots_assign( struct rocketnet_client_slots_t * slots,
                                   uint64_t client_id,
                                   int mapping_index,
                                   const uint8_t * user_data );

// whether this client index currently holds a connection. safe for any
// index; the field accessors below are valid only when this returns 1.

int rocketnet_client_slots_connected( const struct rocketnet_client_slots_t * slots, int client_index );

// slot fields, valid for a connected client index

uint64_t rocketnet_client_slots_client_id( const struct rocketnet_client_slots_t * slots, int client_index );

int rocketnet_client_slots_mapping_index( const struct rocketnet_client_slots_t * slots, int client_index );

const uint8_t * rocketnet_client_slots_user_data( const struct rocketnet_client_slots_t * slots, int client_index );

int rocketnet_client_slots_confirmed( const struct rocketnet_client_slots_t * slots, int client_index );

void rocketnet_client_slots_confirm( struct rocketnet_client_slots_t * slots, int client_index );

// Releases a slot: identity cleared, user data zeroed, immediately available
// for reuse. The caller destroys the associated encryption mapping; the slot
// does not reach into it.

void rocketnet_client_slots_release( struct rocketnet_client_slots_t * slots, int client_index );

// A counter that changes exactly when a slot binding changes (assign or
// release) and at no other time. A caller that polls slot bindings can
// skip its poll while the epoch is unchanged. Starts at 0, never resets.

uint64_t rocketnet_client_slots_binding_epoch( const struct rocketnet_client_slots_t * slots );

// Unit-test poison/release/reuse witness for the private slot arrays.
bool rocketnet_client_slots_reset_witness_for_test();

#endif // #ifndef ROCKETNET_CLIENT_SLOTS_H
