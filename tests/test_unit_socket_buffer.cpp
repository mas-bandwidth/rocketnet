/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The socket buffer decision and start line (sdk/rocketnet_socket_buffer.h)
// the gso and io_uring transports' queue sockets share: a grant at or past the request
// is taken; a short grant asks once more with the force option; a grant still
// short after it is a warning naming why and the exact sysctl and value the
// operator sets. Pure: no socket, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_socket_buffer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static void test_socket_buffer_step()
{
    check( rocketnet_socket_buffer_step( 25000000, 25000000, false ) == ROCKETNET_SOCKET_BUFFER_GRANTED );
    check( rocketnet_socket_buffer_step( 25000000, 26000000, false ) == ROCKETNET_SOCKET_BUFFER_GRANTED );
    check( rocketnet_socket_buffer_step( 25000000, 4194304, false ) == ROCKETNET_SOCKET_BUFFER_FORCE );
    check( rocketnet_socket_buffer_step( 25000000, 4194304, true ) == ROCKETNET_SOCKET_BUFFER_SHORT );
    check( rocketnet_socket_buffer_step( 25000000, 25000000, true ) == ROCKETNET_SOCKET_BUFFER_GRANTED );
    check( rocketnet_socket_buffer_step( 1000000, 0, false ) == ROCKETNET_SOCKET_BUFFER_FORCE );
}

REGISTER_UNIT_TEST( test_socket_buffer_step );

static void test_socket_buffer_note()
{
    char line[512];

    // the c7i bar host: rmem_max 4 MB, no CAP_NET_ADMIN
    check( rocketnet_socket_buffer_note( line, (int) sizeof( line ), "io_uring", true, 25000000, 4194304, true, EPERM ) );
    check( strstr( line, "io_uring transport: socket receive buffer asked 25000000 bytes, got 4194304" ) != NULL );
    check( strstr( line, "SO_RCVBUFFORCE refused: the process lacks CAP_NET_ADMIN" ) != NULL );
    check( strstr( line, "set sysctl net.core.rmem_max=25000000" ) != NULL );

    // the line names the transport it was given (the a02 gso host)
    check( rocketnet_socket_buffer_note( line, (int) sizeof( line ), "gso", true, 25000000, 4194304, true, EPERM ) );
    check( strncmp( line, "gso transport: socket receive buffer asked 25000000 bytes, got 4194304", 70 ) == 0 );
    check( strstr( line, "io_uring" ) == NULL );

    // the send side names its own option and sysctl
    check( rocketnet_socket_buffer_note( line, (int) sizeof( line ), "io_uring", false, 1000000, 212992, true, EPERM ) );
    check( strstr( line, "send buffer asked 1000000 bytes, got 212992" ) != NULL );
    check( strstr( line, "SO_SNDBUFFORCE refused" ) != NULL );
    check( strstr( line, "set sysctl net.core.wmem_max=1000000" ) != NULL );
    check( strstr( line, "rmem_max" ) == NULL );

    // another refusal is named by its errno
    check( rocketnet_socket_buffer_note( line, (int) sizeof( line ), "io_uring", true, 25000000, 4194304, true, EINVAL ) );
    char errno_text[64];
    snprintf( errno_text, sizeof( errno_text ), "SO_RCVBUFFORCE refused: errno %d", EINVAL );
    check( strstr( line, errno_text ) != NULL );

    // force taken and the kernel still short
    check( rocketnet_socket_buffer_note( line, (int) sizeof( line ), "io_uring", true, 25000000, 4194304, true, 0 ) );
    check( strstr( line, "SO_RCVBUFFORCE taken and still short" ) != NULL );

    // short and force never tried
    check( rocketnet_socket_buffer_note( line, (int) sizeof( line ), "io_uring", true, 25000000, 4194304, false, 0 ) );
    check( strstr( line, "SO_RCVBUFFORCE not tried" ) != NULL );

    // met by the plain request: information, no sysctl
    check( !rocketnet_socket_buffer_note( line, (int) sizeof( line ), "io_uring", true, 25000000, 25000000, false, 0 ) );
    check( strstr( line, "receive buffer asked 25000000 bytes, got 25000000" ) != NULL );
    check( strstr( line, "sysctl" ) == NULL );

    // met by the force option: information that says so
    check( !rocketnet_socket_buffer_note( line, (int) sizeof( line ), "io_uring", true, 25000000, 25000000, true, 0 ) );
    check( strstr( line, "past net.core.rmem_max with SO_RCVBUFFORCE" ) != NULL );

    // a line that does not fit is cut and terminated
    char small[16];
    memset( small, 'x', sizeof( small ) );
    check( rocketnet_socket_buffer_note( small, (int) sizeof( small ), "io_uring", true, 25000000, 4194304, true, EPERM ) );
    check( small[sizeof( small ) - 1] == '\0' );
}

REGISTER_UNIT_TEST( test_socket_buffer_note );
