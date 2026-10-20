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

// The used-token history from the standard. The one-live-connection rule
// needs no table of its own: the history refuses a bound token from any other
// address before the signature verify, a connected address is refused at the
// top of the request path, and the client-id check refuses a second live
// connection for a token's client id (sdk/rocketnet_server_protocol.cpp). The
// admitted-token set that held every token until it expired is gone, because
// it denied ordinary churn (docs/SESSION.md 1.6).
//
// The used-token history binds a token identifier to the first source address
// whose response opens under the token's keys. The shorter request-time claim
// that stops one token buying many half-open mappings lives in the encryption
// map, on the mapping it created, and expires with that mapping's handshake
// deadline (sdk/rocketnet_encryption_map.h). Fixed capacity, oldest-first
// eviction, entries expire with their tokens. Lookup is constant time in the
// number of clients: an open-addressed table over a per-run SipHash of the
// identifier, so an attacker cannot craft identifiers that collide.

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

#endif // #ifndef ROCKETNET_TOKEN_TABLES_H
