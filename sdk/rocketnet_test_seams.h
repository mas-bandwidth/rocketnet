/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// What only tests, tools and the development flow use compiles only where
// ROCKETNET_DEV is 1 (sdk/rocketnet.h), so a production rocketnet and
// rocketnet_shared carry none of it. ROCKETNET_DEV is the one switch; this
// header holds the one seam that has a public name, the session sequence start.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_TEST_SEAMS_H
#define ROCKETNET_SDK_ROCKETNET_TEST_SEAMS_H

#include "rocketnet.h"

#if ROCKETNET_DEV

#include <stddef.h>
#include <stdint.h>

// ROCKETNET_DEV_SESSION_SEQUENCE_START: the first session-phase sequence
// every session in this process sends, in place of one, so a bounded run or a
// unit case carries the wire's 32-bit sequence field across 0xFFFFFFFF in
// seconds instead of days (standard/TRANSPORT.md t4 rules 1-3, vector
// t4.sequence-wrap). Read once per process, at the first create that asks
// (the server's encryption map, sdk/rocketnet_encryption_map.cpp, or the
// client's connect, sdk/rocketnet_client_protocol.cpp), and fixed from then
// on, so both ends of a run and every later session use one value. Unset or
// empty is one, the production start, and nothing changes; a value outside
// 1 .. 2^62 is refused with one error line and one is used. Decimal, or hex
// with 0x. Defined in sdk/rocketnet_encryption_map.cpp.

#define ROCKETNET_DEV_SESSION_SEQUENCE_START_MAX 0x4000000000000000ULL

uint64_t rocketnet_dev_session_sequence_start( void );

// The sequence-taken seam: every control-path session send (keep-alive,
// standalone ack, disconnect, and the DEV write_payload) calls
// rocketnet_dev_session_sequence_taken with the sequence it has just taken,
// before it seals. A unit case installs a function here to run a fill-phase
// start inside that window on one thread, the interleaving that would reuse a
// sequence if taking it were not one atomic step. Per thread, null by
// default, so it costs one load when unset. Defined in
// sdk/rocketnet_encryption_map.cpp.

typedef void rocketnet_dev_sequence_taken_function_t( void * context, uint64_t sequence );

void rocketnet_dev_set_sequence_taken_hook( rocketnet_dev_sequence_taken_function_t * function, void * context );

void rocketnet_dev_session_sequence_taken( uint64_t sequence );

// The link stop-and-wait hold. While set, rn_message_link_stop_and_wait
// (sdk/rocketnet_message_server.cpp) treats the transport worker as not
// stopped and calls this once per poll instead of sleeping. Return 1 to
// keep polling, 0 when the worker has stopped. A unit test counts polls
// and fails at its own ceiling, so a wait with no deadline cannot hang
// the suite and does not sleep. NULL is the transport's own stopped().
// Per process, null by default. Defined in sdk/rocketnet_message_server.cpp.

typedef int rocketnet_dev_link_stop_wait_hold_function_t( void * context );

void rocketnet_dev_set_link_stop_wait_hold_hook( rocketnet_dev_link_stop_wait_hold_function_t * function, void * context );
// The server keygen failure seam: while set, rocketnet_server_create fails
// its X25519 key generation / derivation step as if
// rocketnet_generate_server_keypair / crypto_scalarmult_base had failed, so
// a unit case can reach the create failure path without a broken sodium.
// Per process, false by default. Defined in sdk/rocketnet_api.cpp.

void rocketnet_dev_set_server_keygen_failure( bool fail );
// The hash-key seed seam: the client slots, the token tables and the
// encryption map draw their SipHash keys from OS entropy at create. A unit
// case that depends on a table's layout sets a nonzero seed here, and every
// table it then creates on this thread derives its key from that seed and the
// table's kind instead, so the case is reproducible from the seed it prints.
// Per thread, zero (OS entropy, the production draw) by default; a production
// build has no seam and always draws from OS entropy. Defined in
// sdk/rocketnet_encryption_map.cpp.

#define ROCKETNET_DEV_HASH_KEY_CLIENT_SLOTS   1
#define ROCKETNET_DEV_HASH_KEY_TOKEN_TABLE    2
#define ROCKETNET_DEV_HASH_KEY_ENCRYPTION_MAP 3

void rocketnet_dev_set_hash_key_seed( uint64_t seed );

uint64_t rocketnet_dev_hash_key_seed( void );

// called by each create after its OS entropy draw: replaces the key with one
// derived from the seed and the table kind when this thread set a seed
void rocketnet_dev_seed_hash_key( uint8_t * key, size_t key_bytes, int table_kind );

// The disconnect-drain sealed-write seam. While count is positive, the next
// seal of a deferred disconnect burst fails: rocketnet_write_sealed_packet
// has already succeeded, and the drain treats that seal as a failed write.
// The count falls only when that burst is abandoned, so one armed failure
// keeps failing the same head entry on every later stop step until then.
// Zero, the default, leaves every write alone. Defined in
// sdk/rocketnet_server_transport.cpp.

void rocketnet_dev_fail_disconnect_sealed_writes( int count );

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_TEST_SEAMS_H
