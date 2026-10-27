/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// ROCKETNET_TEST_SEAMS: 1 in a development build (ROCKETNET_DEV=1), 0 in a
// production build (ROCKETNET_DEV=0). Enabling ROCKETNET_TEST_SEAMS or any
// specialized test-hook macro when ROCKETNET_DEV=0 is a compile error. What
// only tests, tools and the development flow use -- the network simulator,
// the published development server secret key and its dev config, and the
// unit seams -- compiles only where ROCKETNET_DEV is 1, so a production
// rocketnet and rocketnet_shared carry none of it.

#ifndef ROCKETNET_TEST_SEAMS_H
#define ROCKETNET_TEST_SEAMS_H

#include "rocketnet.h"

#if !defined( ROCKETNET_TEST_SEAMS )
#if ROCKETNET_DEV
#define ROCKETNET_TEST_SEAMS 1
#else // #if ROCKETNET_DEV
#define ROCKETNET_TEST_SEAMS 0
#endif // #if ROCKETNET_DEV
#endif // #if !defined( ROCKETNET_TEST_SEAMS )

#if !ROCKETNET_DEV

#if ROCKETNET_TEST_SEAMS
#error "ROCKETNET_TEST_SEAMS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_ACK_VISIT_TEST_HOOKS ) && ROCKETNET_ACK_VISIT_TEST_HOOKS
#error "ROCKETNET_ACK_VISIT_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_ACK_COST_TEST_HOOKS )
#error "ROCKETNET_ACK_COST_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_ACK_STATS_TEST_HOOKS )
#error "ROCKETNET_ACK_STATS_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_BLOCK_COST_TEST_HOOKS )
#error "ROCKETNET_BLOCK_COST_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_LINK_SIMULATOR_TEST_HOOKS )
#error "ROCKETNET_LINK_SIMULATOR_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_MESSAGE_COPY_TEST_HOOKS )
#error "ROCKETNET_MESSAGE_COPY_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_SECRET_WIPE_TEST_HOOKS )
#error "ROCKETNET_SECRET_WIPE_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_URING_REAP_TEST_HOOKS )
#error "ROCKETNET_URING_REAP_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_RECEIVE_TEST_HOOKS )
#error "ROCKETNET_RECEIVE_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_PORTABLE_TX_TEST_HOOKS )
#error "ROCKETNET_PORTABLE_TX_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_RIO_OVERFLOW_TEST_HOOKS )
#error "ROCKETNET_RIO_OVERFLOW_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_XDP_STAGING_TEST_HOOKS )
#error "ROCKETNET_XDP_STAGING_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_ENVIRONMENT_TEST_HOOKS )
#error "ROCKETNET_ENVIRONMENT_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_SEND_ERROR_TEST_HOOKS )
#error "ROCKETNET_SEND_ERROR_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_LOOPBACK_TEST_HOOKS )
#error "ROCKETNET_LOOPBACK_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_MSGX_TEST_HOOKS )
#error "ROCKETNET_MSGX_TEST_HOOKS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_WINDOWS_LIFECYCLE_TRACK ) && ROCKETNET_WINDOWS_LIFECYCLE_TRACK
#error "ROCKETNET_WINDOWS_LIFECYCLE_TRACK requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_IOCP_TESTS )
#error "ROCKETNET_IOCP_TESTS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_RIO_TESTS )
#error "ROCKETNET_RIO_TESTS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_XDP_TESTS )
#error "ROCKETNET_XDP_TESTS requires ROCKETNET_DEV=1"
#endif

#if defined( ROCKETNET_TEST_CLOCK_WRAP ) && ROCKETNET_TEST_CLOCK_WRAP
#error "ROCKETNET_TEST_CLOCK_WRAP requires ROCKETNET_DEV=1"
#endif

#endif // #if !ROCKETNET_DEV

#if ROCKETNET_TEST_SEAMS

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

#endif // #if ROCKETNET_TEST_SEAMS

#endif // #ifndef ROCKETNET_TEST_SEAMS_H
