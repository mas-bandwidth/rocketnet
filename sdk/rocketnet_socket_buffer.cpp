/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_socket_buffer.h"
#include "rocketnet_internal.h"

#include <errno.h>
#include <stdio.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include <sys/socket.h>
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

bool rocketnet_socket_buffer_note( char * line, int line_bytes, const char * transport, bool receive, int requested_bytes, int granted_bytes, bool force_tried,
                                   int force_error )
{
    rocketnet_assert( line );
    rocketnet_assert( line_bytes > 0 );
    rocketnet_assert( transport );

    const char * direction = receive ? "receive" : "send";
    const char * force_name = receive ? "SO_RCVBUFFORCE" : "SO_SNDBUFFORCE";
    const char * limit = receive ? "net.core.rmem_max" : "net.core.wmem_max";

    if ( rocketnet_socket_buffer_step( requested_bytes, granted_bytes, true ) == ROCKETNET_SOCKET_BUFFER_GRANTED )
    {
        if ( force_tried )
        {
            (void) snprintf( line, (size_t) line_bytes, "%s transport: socket %s buffer asked %d bytes, got %d (past %s with %s)", transport, direction, requested_bytes,
                      granted_bytes, limit, force_name );
        }
        else
        {
            (void) snprintf( line, (size_t) line_bytes, "%s transport: socket %s buffer asked %d bytes, got %d", transport, direction, requested_bytes, granted_bytes );
        }
        return false;
    }

    char force_text[96];
    if ( !force_tried )
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s not tried", force_name );
    }
    else if ( force_error == EPERM )
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s refused: the process lacks CAP_NET_ADMIN", force_name );
    }
    else if ( force_error != 0 )
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s refused: errno %d", force_name, force_error );
    }
    else
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s taken and still short", force_name );
    }

    const char * consequence = receive ? "the kernel drops datagrams that arrive past the grant between updates" : "sends wait on a full socket";

    (void) snprintf( line, (size_t) line_bytes, "%s transport: socket %s buffer asked %d bytes, got %d (%s); %s: set sysctl %s=%d", transport, direction, requested_bytes,
              granted_bytes, force_text, consequence, limit, requested_bytes );
    return true;
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// the grant of one buffer in the bytes requested: linux reports double its
// grant for its own bookkeeping, so this halves it. 0 when unreadable

static int rocketnet_socket_buffer_granted( int fd, int option )
{
    int granted = 0;
    socklen_t optlen = sizeof( granted );
    if ( getsockopt( fd, SOL_SOCKET, option, &granted, &optlen ) != 0 )
    {
        return 0;
    }
    return granted / 2;
}

bool rocketnet_socket_buffer_request( int fd, bool receive, int requested_bytes, struct rocketnet_socket_buffer_grant_t * grant )
{
    rocketnet_assert( grant );

    const int option = receive ? SO_RCVBUF : SO_SNDBUF;
    const int force_option = receive ? SO_RCVBUFFORCE : SO_SNDBUFFORCE;

    grant->granted_bytes = 0;
    grant->force_tried = false;
    grant->force_error = 0;

    int size = requested_bytes;
    if ( setsockopt( fd, SOL_SOCKET, option, &size, sizeof( size ) ) != 0 )
    {
        return false;
    }
    grant->granted_bytes = rocketnet_socket_buffer_granted( fd, option );

    if ( rocketnet_socket_buffer_step( requested_bytes, grant->granted_bytes, false ) == ROCKETNET_SOCKET_BUFFER_FORCE )
    {
        grant->force_tried = true;
        size = requested_bytes;
        if ( setsockopt( fd, SOL_SOCKET, force_option, &size, sizeof( size ) ) != 0 )
        {
            grant->force_error = errno;
        }
        grant->granted_bytes = rocketnet_socket_buffer_granted( fd, option );
    }
    return true;
}

void rocketnet_socket_buffer_log( const char * transport, bool receive, int requested_bytes, const struct rocketnet_socket_buffer_grant_t * grant )
{
    rocketnet_assert( grant );

    char line[512];
    if ( rocketnet_socket_buffer_note( line, (int) sizeof( line ), transport, receive, requested_bytes, grant->granted_bytes, grant->force_tried, grant->force_error ) )
    {
        rocketnet_warn( "%s", line );
    }
    else
    {
        rocketnet_info( "%s", line );
    }
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
