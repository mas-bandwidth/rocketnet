/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SOCKET_BUFFER_H
#define ROCKETNET_SDK_ROCKETNET_SOCKET_BUFFER_H

// Socket buffers on a native server transport's queue sockets. Linux clamps
// an SO_SNDBUF / SO_RCVBUF request to net.core.wmem_max / net.core.rmem_max
// without an error; SO_SNDBUFFORCE / SO_RCVBUFFORCE pass that limit for a
// process with CAP_NET_ADMIN and are refused (EPERM) for any other. Every
// queue socket asks with the plain option, reads the grant back, and when
// the grant is short asks once more with the force option. A grant still
// short is not fatal: the transport runs, the kernel drops what arrives past
// a short receive grant (UDP RcvbufErrors) and a short send grant makes sends
// wait on a full socket, so the transport names it at start with the sysctl
// the operator sets.
//
// The decision and the start line are pure and shared by the transports
// that ask this way (the io_uring transport's queue sockets); the requests
// themselves are each transport's own.

#include <stdbool.h>

#define ROCKETNET_SOCKET_BUFFER_GRANTED                                      0       // the grant meets the request
#define ROCKETNET_SOCKET_BUFFER_FORCE                                        1       // short, force not tried: ask again with the force option
#define ROCKETNET_SOCKET_BUFFER_SHORT                                        2       // short after the force option: name the sysctl at start

// the decision for one buffer: the request, the grant read back (halved:
// linux reports double), and whether the force option was already tried

static inline int rocketnet_socket_buffer_step( int requested_bytes, int granted_bytes, bool force_tried )
{
    if ( granted_bytes >= requested_bytes )
    {
        return ROCKETNET_SOCKET_BUFFER_GRANTED;
    }
    return force_tried ? ROCKETNET_SOCKET_BUFFER_SHORT : ROCKETNET_SOCKET_BUFFER_FORCE;
}

// the start line for one socket buffer (receive or send) of the named
// transport, written into line and always terminated: asked, got, how, and
// when the grant is short the sysctl and the value the operator sets (the
// request itself: linux compares the request, not its doubled charge,
// against the limit). force_error is the errno the force option was refused
// with, 0 when it was taken or never tried. Returns true when the line is a
// warning (the grant is short).

bool rocketnet_socket_buffer_note( char * line, int line_bytes, const char * transport, bool receive, int requested_bytes, int granted_bytes, bool force_tried,
                                   int force_error );

#endif // ROCKETNET_SDK_ROCKETNET_SOCKET_BUFFER_H
