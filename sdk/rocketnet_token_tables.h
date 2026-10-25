/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TOKEN_TABLES_H
#define ROCKETNET_TOKEN_TABLES_H

#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"

// The used-token history from the standard uses two independent capacity
// tables. The response-binding table binds a token identifier to the first
// source address that opened a response under it; the binding lives until the
// token expires. The request-claim table binds the same identifier to the
// first source address that presented a valid request, before any asymmetric
// work, and lives only until the handshake deadline or the response binding:
// one token buys one half-open mapping, and a replay from a second address
// waits until the first mapping dies. The two tables are independent, so
// claim pressure cannot consume or evict response-binding capacity. Each
// table is fixed-capacity with bound-first eviction for bindings, no
// eviction of a live claim, and entries that expire with their deadlines.
// Lookup is constant time in the number of clients: chained buckets under a
// per-run SipHash of the identifier, so an attacker cannot craft identifiers
// that collide. The one-live-connection rule needs no table of its own: the
// claim and the history refuse a token from a second address before the
// signature verify, a connected address is refused at the top of the request
// path, and the client-id check refuses a second live connection for a
// token's client id (sdk/rocketnet_server_protocol.cpp).

struct rocketnet_used_token_history_t;

struct rocketnet_used_token_history_t * rocketnet_used_token_history_create( void * context, int capacity );

void rocketnet_used_token_history_destroy( struct rocketnet_used_token_history_t * history );

// returns 1 if this identifier may proceed from this address: either unseen,
// or seen first from this same address and port. returns 0 when the
// identifier is bound to a different address. expired entries are unseen.

int rocketnet_used_token_history_check( const struct rocketnet_used_token_history_t * history,
                                        const uint8_t * token_identifier,
                                        const struct rocketnet_address_t * address,
                                        uint64_t current_timestamp );

// records the identifier bound to this address, evicting an expired entry
// first, the oldest entry otherwise. recording an identifier already bound
// to this address refreshes nothing and is harmless.

void rocketnet_used_token_history_record( struct rocketnet_used_token_history_t * history,
                                          const uint8_t * token_identifier,
                                          const struct rocketnet_address_t * address,
                                          uint64_t expire_timestamp,
                                          uint64_t current_timestamp );

// returns 1 when this token has no live claim, or is claimed for this same
// address; 0 when another address holds the claim. The claim lives until the
// handshake deadline, the response binding, a refusal, or its mapping's
// removal -- whichever comes first -- so it never outlives the half-open
// mapping it stands for.

int rocketnet_used_token_history_claim_check( const struct rocketnet_used_token_history_t * history,
                                              const uint8_t * token_identifier,
                                              const struct rocketnet_address_t * address,
                                              double current_time );

// whether claim_record would succeed now, without writing anything: the
// claim rung refuses on capacity with this, read-only, before the signature,
// and the claim itself is written only once the signature verifies, so an
// unauthenticated request leaves nothing in the claim table.

int rocketnet_used_token_history_claim_can_record( const struct rocketnet_used_token_history_t * history,
                                                   const uint8_t * token_identifier,
                                                   double current_time );

// records the request claim for this token until the handshake deadline,
// after the signature verifies and before any seal or keypair work. returns 0
// when the claim table has no free or expired slot; a live claim is never
// replaced or evicted.

int rocketnet_used_token_history_claim_record( struct rocketnet_used_token_history_t * history,
                                               const uint8_t * token_identifier,
                                               const struct rocketnet_address_t * address,
                                               double deadline,
                                               double current_time );

// releases this address's claim when a request that holds one is refused
// before its mapping is added, or when the half-open mapping that holds it is
// removed before the handshake deadline.

void rocketnet_used_token_history_claim_release( struct rocketnet_used_token_history_t * history,
                                                 const uint8_t * token_identifier,
                                                 const struct rocketnet_address_t * address );

// the number of claim-table slots on the free list: a count kept on every
// push and pop, O(1), read by the unit tests.

int rocketnet_used_token_history_claim_free_count( const struct rocketnet_used_token_history_t * history );

#endif // #ifndef ROCKETNET_TOKEN_TABLES_H
