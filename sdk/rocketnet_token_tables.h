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

// The two token structures from the standard, separate because they carry
// two different guarantees and only one of them can tolerate eviction.
//
// The used-token history binds a token identifier to the first source address
// that presented it. Fixed capacity, oldest-first eviction, entries expire
// with their tokens. Lookup is constant time in the number of clients: an
// open-addressed table over a per-run SipHash of the identifier, so an
// attacker cannot craft identifiers that collide.
//
// The admitted-token set holds every token that has admitted a connection.
// An unexpired entry is NEVER evicted -- which is sound because an expired
// entry protects nothing the expire check does not -- and when full the
// server refuses new connections, failing closed.

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

struct rocketnet_admitted_token_set_t;

struct rocketnet_admitted_token_set_t * rocketnet_admitted_token_set_create( void * context, int capacity );

void rocketnet_admitted_token_set_destroy( struct rocketnet_admitted_token_set_t * set );

// returns 1 when this token has already admitted a connection and is not yet
// expired

int rocketnet_admitted_token_set_contains( const struct rocketnet_admitted_token_set_t * set,
                                           const uint8_t * token_identifier,
                                           uint64_t current_timestamp );

// returns 1 on success. returns 0 only when the set is full of unexpired
// entries: unexpired entries are never evicted, so a full set refuses --
// the caller denies the connection and counts ADMITTED_SET_FULL.

int rocketnet_admitted_token_set_add( struct rocketnet_admitted_token_set_t * set,
                                      const uint8_t * token_identifier,
                                      uint64_t expire_timestamp,
                                      uint64_t current_timestamp );

#endif // #ifndef ROCKETNET_TOKEN_TABLES_H
