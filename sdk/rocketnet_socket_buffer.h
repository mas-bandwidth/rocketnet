/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SOCKET_BUFFER_H
#define ROCKETNET_SDK_ROCKETNET_SOCKET_BUFFER_H

// Socket buffers on a native Linux server transport's queue sockets (the gso
// and io_uring transports). Linux clamps an SO_SNDBUF / SO_RCVBUF request to
// net.core.wmem_max / net.core.rmem_max without an error; SO_SNDBUFFORCE /
// SO_RCVBUFFORCE pass that limit for a process with CAP_NET_ADMIN and are
// refused (EPERM) for any other. Every queue socket asks with the plain
// option, reads the grant back, and when the grant is short asks once more
// with the force option (rocketnet_socket_buffer_request). A grant still
// short is not fatal: the transport runs, the kernel drops what arrives past
// a short receive grant (UDP RcvbufErrors) and a short send grant makes sends
// wait on a full socket, so the transport names it at start with the sysctl
// the operator sets (rocketnet_socket_buffer_log).
//
// The decision (rocketnet_socket_buffer_step) and the start line
// (rocketnet_socket_buffer_note) are pure and built on every platform, so the
// unit suite checks them anywhere; the request and the log are Linux only.

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

// what one buffer request did: the grant (halved; 0 when unreadable),
// whether the force option was tried, and the errno it was refused with

struct rocketnet_socket_buffer_grant_t
{
    int granted_bytes;
    bool force_tried;
    int force_error;
};

// Linux: one buffer of one socket (rocketnet_socket_buffer_step): the plain
// request, the grant read back, and when short the force option once. false
// only when the plain request itself is refused (errno set); a refused force
// is a fact for the start line, kept in grant->force_error

bool rocketnet_socket_buffer_request( int fd, bool receive, int requested_bytes, struct rocketnet_socket_buffer_grant_t * grant );

// Linux: the start line of one buffer at the log level its grant earns (a
// warning when short, information when met)

void rocketnet_socket_buffer_log( const char * transport, bool receive, int requested_bytes, const struct rocketnet_socket_buffer_grant_t * grant );

#endif // ROCKETNET_SDK_ROCKETNET_SOCKET_BUFFER_H
