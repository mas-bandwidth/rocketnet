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

// Needs ROCKETNET_DEV (sdk/rocketnet_driver_builtin.cpp).
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

// The two fixtures run only where the portable transport is built
// (ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE); elsewhere each returns false, so
// the case is left out with a SKIPPED line rather than passing with no logic run.
#if defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )
static constexpr bool send_errnos_platform_has_event_only_wake = true;
#else
static constexpr bool send_errnos_platform_has_event_only_wake = false;
#endif // #if defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )

UNIT_TEST_SKIPPED_UNLESS( test_send_banks_sized_to_clients, send_errnos_platform_has_event_only_wake );
UNIT_TEST_SKIPPED_UNLESS( test_send_drops_counted_by_path, send_errnos_platform_has_event_only_wake );

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
    {
        rocketnet_unit_check( !rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, EAGAIN ) );
    }

    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_REPLAY, EAGAIN ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 1000 );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_REPLAY, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 1 );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 0 );
    rocketnet_unit_check( rocketnet_platform_send_errnos_format( &errnos, text, sizeof( text ) ) == 2 );
    rocketnet_unit_check( strcmp( text, "sendmsg_x EAGAIN=1000 replay sendto EAGAIN=1" ) == 0 );

    // a buffer that holds the first record and its terminator exactly keeps
    // that record whole and drops the second; one byte less keeps none. No
    // partial word is left either way.

    char small_buffer[sizeof( "sendmsg_x EAGAIN=1000" )];
    memset( small_buffer, 'x', sizeof( small_buffer ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_format( &errnos, small_buffer, sizeof( small_buffer ) ) == 1 );
    rocketnet_unit_check( strcmp( small_buffer, "sendmsg_x EAGAIN=1000" ) == 0 );

    memset( small_buffer, 'x', sizeof( small_buffer ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_format( &errnos, small_buffer, sizeof( small_buffer ) - 1 ) == 0 );
    rocketnet_unit_check( small_buffer[0] == '\0' );

    rocketnet_unit_check( rocketnet_platform_send_errnos_format( &errnos, small_buffer, 0 ) == 0 );
}

REGISTER_UNIT_TEST( test_send_errnos_counted_by_call );

// Every errno the kind table names maps to its own kind, the pairs it folds
// (EWOULDBLOCK with EAGAIN, EACCES with EPERM) map together, and anything
// else is OTHER, its errno kept from the first OTHER failure only.

static void test_send_errno_kinds_and_the_first_other()
{
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EWOULDBLOCK ) == ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EHOSTUNREACH ) == ROCKETNET_PLATFORM_SEND_ERRNO_EHOSTUNREACH );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( ENETUNREACH ) == ROCKETNET_PLATFORM_SEND_ERRNO_ENETUNREACH );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( ECONNREFUSED ) == ROCKETNET_PLATFORM_SEND_ERRNO_ECONNREFUSED );
#if defined( EHOSTDOWN )
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EHOSTDOWN ) == ROCKETNET_PLATFORM_SEND_ERRNO_EHOSTDOWN );
#endif // #if defined( EHOSTDOWN )
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( ENETDOWN ) == ROCKETNET_PLATFORM_SEND_ERRNO_ENETDOWN );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EMSGSIZE ) == ROCKETNET_PLATFORM_SEND_ERRNO_EMSGSIZE );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EADDRNOTAVAIL ) == ROCKETNET_PLATFORM_SEND_ERRNO_EADDRNOTAVAIL );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EPERM ) == ROCKETNET_PLATFORM_SEND_ERRNO_EPERM );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EACCES ) == ROCKETNET_PLATFORM_SEND_ERRNO_EPERM );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EINTR ) == ROCKETNET_PLATFORM_SEND_ERRNO_EINTR );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EBADF ) == ROCKETNET_PLATFORM_SEND_ERRNO_OTHER );

    for ( int kind = 0; kind < ROCKETNET_PLATFORM_SEND_ERRNO_KINDS; kind++ )
    {
        rocketnet_unit_check( strcmp( rocketnet_platform_send_errno_kind_name( kind ), "unknown" ) != 0 );
    }
    rocketnet_unit_check( strcmp( rocketnet_platform_send_errno_kind_name( ROCKETNET_PLATFORM_SEND_ERRNO_EINTR ), "EINTR" ) == 0 );
    rocketnet_unit_check( strcmp( rocketnet_platform_send_errno_kind_name( ROCKETNET_PLATFORM_SEND_ERRNO_KINDS ), "unknown" ) == 0 );

    // on one call, the first OTHER failure keeps its errno and a later one,
    // of another errno, only counts. other_error is one field for every
    // call (rocketnet_platform.h, "the first OTHER failure"); a first OTHER
    // on a second call is checked by the next test

    rocketnet_platform_send_errnos_t errnos;
    memset( &errnos, 0, sizeof( errnos ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ENOTSUP ) );
    rocketnet_unit_check( errnos.other_error == (uint64_t) ENOTSUP );
    rocketnet_unit_check( !rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, EBADF ) );
    rocketnet_unit_check( errnos.other_error == (uint64_t) ENOTSUP );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ROCKETNET_PLATFORM_SEND_ERRNO_OTHER ) == 2 );

    // a named kind keeps no errno

    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ENETDOWN ) );
    rocketnet_unit_check( errnos.other_error == (uint64_t) ENOTSUP );
}

REGISTER_UNIT_TEST( test_send_errno_kinds_and_the_first_other );

// other_error holds the errno of the first OTHER failure on the socket,
// across every call: two calls that each fail first with an OTHER errno keep
// the first one's errno. Before, each call's first OTHER failure added its
// errno, so sendto ENOTSUP then sendmsg_x EBADF left their sum, naming
// neither.
static void test_send_errnos_keep_the_first_other_errno_across_calls()
{
    rocketnet_platform_send_errnos_t errnos;
    memset( &errnos, 0, sizeof( errnos ) );

    rocketnet_unit_check( rocketnet_platform_send_errno_kind( ENOTSUP ) == ROCKETNET_PLATFORM_SEND_ERRNO_OTHER );
    rocketnet_unit_check( rocketnet_platform_send_errno_kind( EBADF ) == ROCKETNET_PLATFORM_SEND_ERRNO_OTHER );

    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ENOTSUP ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, EBADF ) );
    rocketnet_unit_check( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_REPLAY, EBADF ) );

    rocketnet_unit_check( rocketnet_platform_atomic_load_u64( &errnos.other_error ) == (uint64_t) ENOTSUP );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ROCKETNET_PLATFORM_SEND_ERRNO_OTHER ) == 1 );
    rocketnet_unit_check( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, ROCKETNET_PLATFORM_SEND_ERRNO_OTHER ) == 1 );
}

REGISTER_UNIT_TEST( test_send_errnos_keep_the_first_other_errno_across_calls );

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// The errnos a connected socket's send call hands back for an ICMP error an
// earlier datagram drew (Linux icmp_err_convert and icmpv6_err_convert, the
// hard errors): the call sent nothing, so the message goes again once. A full
// socket, a signal, a local refusal or a too-long datagram is the message's
// own answer and never retries.

static void test_send_error_pending_icmp_errnos()
{
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( ECONNREFUSED ) );
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( EHOSTUNREACH ) );
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( ENETUNREACH ) );
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( EHOSTDOWN ) );
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( ENOPROTOOPT ) );
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( EPROTO ) );
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( EACCES ) );
#if defined( ENONET )
    rocketnet_unit_check( rocketnet_platform_send_error_is_pending( ENONET ) );
#endif // #if defined( ENONET )

    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( 0 ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( EAGAIN ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( ENOBUFS ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( EINTR ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( EPERM ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( EMSGSIZE ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( EINVAL ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( ENETDOWN ) );
    rocketnet_unit_check( !rocketnet_platform_send_error_is_pending( EDESTADDRREQ ) );
}

REGISTER_UNIT_TEST( test_send_error_pending_icmp_errnos );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
