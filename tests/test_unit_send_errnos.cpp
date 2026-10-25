/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"

#include "rocketnet_platform.h"

#include <errno.h>
#include <string.h>

// A portable or msgx server counts a packet it could not send in
// SEND_FAILED. Two causes are counted apart: a socket call that failed, by
// call and errno, and a packet dropped because every send bank was in
// flight. No socket, worker or clock here: the counts are pure, and the bank
// case plays the worker by hand at exact handoff transitions.

// Compiled only into rocketnet_test_hooks (sdk/rocketnet_driver_builtin.cpp).
bool rocketnet_portable_tx_test_sizing();
bool rocketnet_portable_tx_test_drop_paths();

static void test_send_banks_sized_to_clients()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_sizing() );
}

REGISTER_UNIT_TEST( test_send_banks_sized_to_clients );

// Each way the send handoff drops a packet is counted on its own path (no
// free bank, handoff lock busy, stopping, invalid packet), and SEND_FAILED's
// handoff part is their sum, so a run's log names which path dropped.
static void test_send_drops_counted_by_path()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_drop_paths() );
}

REGISTER_UNIT_TEST( test_send_drops_counted_by_path );

static void test_send_errnos_counted_by_call()
{
    rocketnet_platform_send_errnos_t errnos;
    memset( &errnos, 0, sizeof( errnos ) );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( 0 ) == ROCKETNET_PLATFORM_SEND_ERRNO_NONE );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EAGAIN ) == ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( ENOBUFS ) == ROCKETNET_PLATFORM_SEND_ERRNO_ENOBUFS );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( ENOTSUP ) == ROCKETNET_PLATFORM_SEND_ERRNO_OTHER );

    char text[256];
    rocketnet_unit_check( rocketnet_platform_send_errnos_format( &errnos, text, sizeof( text ) ) == 0 && text[0] == '\0' );

    // the first failure of a call and errno reports first; later ones only count
    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, EAGAIN ) );
    for ( int i = 0; i < 999; ++i )
        rocketnet_unit_check( !rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, EAGAIN ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_REPLAY, EAGAIN ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 1000 );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_REPLAY, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 1 );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 0 );
    rocketnet_unit_check( rocketnet_platform_send_errnos_format( &errnos, text, sizeof( text ) ) == 2 );
    rocketnet_unit_check( strcmp( text, "sendmsg_x EAGAIN=1000 replay sendto EAGAIN=1" ) == 0 );
}

REGISTER_UNIT_TEST( test_send_errnos_counted_by_call );
