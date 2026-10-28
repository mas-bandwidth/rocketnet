/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The IOCP transport's read of the UDP_COALESCED_INFO control message
// (rocketnet_iocp_coalesced_stride) over a WSAMSG built in memory: pure, no
// socket, no wall clock. The stride is copied only when the header's
// cmsg_len covers a whole DWORD, WSA_CMSG_LEN( sizeof( DWORD ) ).

#include "test_unit_registry.h"

#if defined( _WIN32 )

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

// Implemented by the IOCP transport (rocketnet_server_iocp_windows.cpp).
DWORD rocketnet_iocp_coalesced_stride( WSAMSG * message );

struct coalesced_cmsg_buffer_t
{
    WSAMSG message;
    // the padding to control is written out, so the compiler adds none (MSVC C4324)
    char pad[( sizeof( WSAMSG ) + 15 ) / 16 * 16 - sizeof( WSAMSG )];
    alignas( 16 ) char control[64];
};

// One UDP_COALESCED_INFO header of the given cmsg_len, with the DWORD 1234
// in its data slot. The slot lies inside the control area whatever
// cmsg_len says, so a read past the claimed length stays in this buffer.
static void coalesced_cmsg_build( coalesced_cmsg_buffer_t * b, int level, int type, size_t cmsg_len )
{
    memset( b, 0, sizeof( *b ) );
    b->message.Control.buf = b->control;

    // The control area holds exactly one header and its DWORD, as the stack
    // fills it for a coalesced receive, so the walk ends after that header.

    b->message.Control.len = WSA_CMSG_SPACE( sizeof( DWORD ) );
    WSACMSGHDR * cmsg = WSA_CMSG_FIRSTHDR( &b->message );
    check( cmsg != NULL );
    if ( !cmsg )
        return;
    cmsg->cmsg_len = cmsg_len;
    cmsg->cmsg_level = level;
    cmsg->cmsg_type = type;

    const DWORD stride = 1234;
    memcpy( WSA_CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
}

static void test_windows_coalesced_cmsg_whole_dword_is_read()
{
    coalesced_cmsg_buffer_t b;
    coalesced_cmsg_build( &b, IPPROTO_UDP, UDP_COALESCED_INFO, WSA_CMSG_LEN( sizeof( DWORD ) ) );
    check( rocketnet_iocp_coalesced_stride( &b.message ) == 1234 );
}

static void test_windows_coalesced_cmsg_short_length_is_not_read()
{
    // cmsg_len one byte short of a DWORD, and a header with no data at all:
    // neither carries a whole stride, so neither is copied.

    coalesced_cmsg_buffer_t b;
    coalesced_cmsg_build( &b, IPPROTO_UDP, UDP_COALESCED_INFO, WSA_CMSG_LEN( sizeof( DWORD ) ) - 1 );
    check( rocketnet_iocp_coalesced_stride( &b.message ) == 0 );

    coalesced_cmsg_build( &b, IPPROTO_UDP, UDP_COALESCED_INFO, WSA_CMSG_LEN( 0 ) );
    check( rocketnet_iocp_coalesced_stride( &b.message ) == 0 );
}

static void test_windows_coalesced_cmsg_other_headers_are_skipped()
{
    coalesced_cmsg_buffer_t b;
    coalesced_cmsg_build( &b, IPPROTO_IP, UDP_COALESCED_INFO, WSA_CMSG_LEN( sizeof( DWORD ) ) );
    check( rocketnet_iocp_coalesced_stride( &b.message ) == 0 );

    coalesced_cmsg_build( &b, IPPROTO_UDP, UDP_COALESCED_INFO + 1, WSA_CMSG_LEN( sizeof( DWORD ) ) );
    check( rocketnet_iocp_coalesced_stride( &b.message ) == 0 );

    // an empty control area has no header

    memset( &b, 0, sizeof( b ) );
    check( rocketnet_iocp_coalesced_stride( &b.message ) == 0 );
}

REGISTER_UNIT_TEST( test_windows_coalesced_cmsg_whole_dword_is_read );
REGISTER_UNIT_TEST( test_windows_coalesced_cmsg_short_length_is_not_read );
REGISTER_UNIT_TEST( test_windows_coalesced_cmsg_other_headers_are_skipped );

#endif // #if defined(_WIN32)
