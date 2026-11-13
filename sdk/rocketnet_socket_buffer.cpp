/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_socket_buffer.h"
#include "rocketnet_internal.h"

#include <errno.h>
#include <stdio.h>

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
            snprintf( line, (size_t) line_bytes, "%s transport: socket %s buffer asked %d bytes, got %d (past %s with %s)", transport, direction, requested_bytes,
                      granted_bytes, limit, force_name );
        }
        else
        {
            snprintf( line, (size_t) line_bytes, "%s transport: socket %s buffer asked %d bytes, got %d", transport, direction, requested_bytes, granted_bytes );
        }
        return false;
    }

    char force_text[96];
    if ( !force_tried )
    {
        snprintf( force_text, sizeof( force_text ), "%s not tried", force_name );
    }
    else if ( force_error == EPERM )
    {
        snprintf( force_text, sizeof( force_text ), "%s refused: the process lacks CAP_NET_ADMIN", force_name );
    }
    else if ( force_error != 0 )
    {
        snprintf( force_text, sizeof( force_text ), "%s refused: errno %d", force_name, force_error );
    }
    else
    {
        snprintf( force_text, sizeof( force_text ), "%s taken and still short", force_name );
    }

    const char * consequence = receive ? "the kernel drops datagrams that arrive past the grant between updates" : "sends wait on a full socket";

    snprintf( line, (size_t) line_bytes, "%s transport: socket %s buffer asked %d bytes, got %d (%s); %s: set sysctl %s=%d", transport, direction, requested_bytes,
              granted_bytes, force_text, consequence, limit, requested_bytes );
    return true;
}
