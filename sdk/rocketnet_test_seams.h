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

// The server transport's failed-create stop hold. While set, the unwind of a
// failed server transport create (rocketnet_server_transport_abort_create in
// sdk/rocketnet_server_transport.cpp) treats the transport as not stopped
// and calls this once per poll, with the transport, instead of sleeping on
// the platform clock: its 1 s deadline runs on a counter clock each poll
// advances by 1 ms. Return 1 to keep polling, 0 when the worker has stopped.
// A transport kept at the deadline goes to the create's owner (a server
// start keeps it in the server): while this hook is set it also answers
// rocketnet_server_transport_failed_create_stopped, so the server reports
// not stopped until the hook says 0 or is cleared. NULL is the transport's
// own stopped(). Per process, null by default.

struct rocketnet_server_transport_t;

typedef int rocketnet_dev_server_abort_create_hold_function_t( void * context, struct rocketnet_server_transport_t * transport );

void rocketnet_dev_set_server_abort_create_hold_hook( rocketnet_dev_server_abort_create_hold_function_t * function, void * context );

// The POSIX wake create failure seam: while nonzero, every
// rocketnet_platform_wake_create fails at its kernel object (the kqueue, the
// eventfd or the pipe) as if the call had set errno to this value, so a unit
// case reaches the create's failure status without exhausting the kernel.
// Per process, 0 by default. Defined in sdk/rocketnet_platform.cpp (POSIX).

void rocketnet_dev_fail_wake_create( int error );

// The server keygen failure seam: while set, rocketnet_server_create fails
// its X25519 key generation / derivation step as if
// rocketnet_generate_server_keypair / crypto_scalarmult_base had failed, so
// a unit case can reach the create failure path without a broken sodium.
// Per process, false by default. Defined in sdk/rocketnet_api.cpp.

void rocketnet_dev_set_server_keygen_failure( bool fail );

// The crypto source failure seam: while set, opening sodium's random source
// fails (rocketnet_crypto_open_locked in sdk/rocketnet.cpp, under the crypto
// mutex), so rocketnet_crypto_acquire returns false and every caller takes
// its real failure branch: rocketnet_generate_server_keypair answers
// CRYPTO_INIT_FAILED, and so does the generated-pair step of
// rocketnet_server_create. Per process, false by default. Defined in
// sdk/rocketnet.cpp.

void rocketnet_dev_set_crypto_source_failure( bool fail );
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
#define ROCKETNET_DEV_HASH_KEY_COOKIE_REPLY_SOURCES 4

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

// The client I/O stop-wait hold. While set, the I/O core's exit
// (rocketnet_client_io_discard_at_stop, sdk/rocketnet_client_io.h) calls this once per
// poll of its stop_woken wait instead of sleeping, so a unit case can run the
// exit inside stop's wake call on one thread and see that no close happens
// while the wake is still to return. The wait ends only when stop_woken is
// stored, and the hook replaces the sleep, so every function set here must
// store stop_woken itself within a bounded number of polls (and fail its
// case at that ceiling); one that never does spins forever. So the wait does
// not sleep and cannot hang the suite. NULL, the default, leaves the sleep.
// Per process. Defined in sdk/rocketnet_client_transport.cpp.

typedef void rocketnet_dev_client_io_stop_wait_function_t( void * context );

void rocketnet_dev_set_client_io_stop_wait_hook( rocketnet_dev_client_io_stop_wait_function_t * function, void * context );

// called by the wait: runs the hook and returns true when one is set, else
// returns false and the wait sleeps
bool rocketnet_dev_client_io_stop_wait_poll( void );

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_TEST_SEAMS_H
