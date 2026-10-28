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
// t4.sequence-wrap). Read once per process, at the first create that asks
// (the server's encryption map, sdk/rocketnet_encryption_map.cpp, or the
// client's connect, sdk/rocketnet_client_protocol.cpp), and fixed from then
// on, so both ends of a run and every later session use one value. Unset or
// empty is one, the production start, and nothing changes; a value outside
// 1 .. 2^62 is refused with one error line and one is used. Decimal, or hex
// with 0x. Defined in sdk/rocketnet_encryption_map.cpp.

#define ROCKETNET_DEV_SESSION_SEQUENCE_START_MAX 0x4000000000000000ULL

uint64_t rocketnet_dev_session_sequence_start( void );

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_TEST_SEAMS_H
