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

#include <stdint.h>

// ROCKETNET_DEV_SESSION_SEQUENCE_START: the first session-phase sequence
// every session in this process sends, in place of one, so a bounded run or a
// unit case carries the wire's 32-bit sequence field across 0xFFFFFFFF in
// seconds instead of days (standard/TRANSPORT.md t4 rules 1-3, vector
// t4.sequence-wrap). Read at init from init's environment source
// (rocketnet_init_ex_with_env, sdk/rocketnet_internal.h) and fixed until the
// next init, so both ends of a run and every session in it use one value: the
// server's encryption map (sdk/rocketnet_encryption_map.cpp) and the client's
// connect (sdk/rocketnet_client_protocol.cpp) read it here. Before the first
// init, unset or empty, it is one, the production start, and nothing changes;
// a value outside 1 .. 2^62 is refused with one error line and one is used.
// Decimal, or hex with 0x. Defined in sdk/rocketnet.cpp.

#define ROCKETNET_DEV_SESSION_SEQUENCE_START_MAX 0x4000000000000000ULL

uint64_t rocketnet_dev_session_sequence_start( void );

struct rocketnet_env_source_t;
void rocketnet_dev_session_sequence_start_load( const struct rocketnet_env_source_t * env );

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

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_TEST_SEAMS_H
