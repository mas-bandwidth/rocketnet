/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_msgx_receive_fill.h"

#include <errno.h>

// The receive worker's errno classes and its read size, out of line: the
// trace check's plant (ROCKETNET_MSGX_RECEIVE_FILL_PLANT) must be library code
// the plant binary compiles in place of the library's
// (proofs/tla/README-TraceCheck.md): only tests/tla_trace_msgx_receive_fill.cpp
// in the plant build defines it, and its traces must fail TraceConsistent.
// The rest of the receive path stays inline in its header.

// What a failed read on a socket means for the datagrams queued on it, the
// platform's receive weather (rocketnet_platform_receive_weather): a
// connected client socket answers its next read with an ICMP error the
// kernel queued for that client (port, host or network unreachable or
// down), a full buffer pool answers ENOBUFS, a signal EINTR. Each is cleared
// by the read that reports it and says nothing about the datagrams behind it,
// so the drain reads on (one read of the budget spent). Not the server's
// failure, and not a dead socket.
int rocketnet_msgx_receive_error_kind( int error )
{
    if ( error == EAGAIN || error == EWOULDBLOCK )
    {
        return ROCKETNET_MSGX_RECEIVE_EMPTY;
    }
    if ( error == EINTR || error == ENOBUFS || error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH || error == ENETDOWN )
    {
        return ROCKETNET_MSGX_RECEIVE_WEATHER;
    }
#if defined( EHOSTDOWN )
    if ( error == EHOSTDOWN )
    {
        return ROCKETNET_MSGX_RECEIVE_WEATHER;
    }
#endif // #if defined( EHOSTDOWN )
    if ( error == ENOTCONN || error == EPIPE || error == EDESTADDRREQ )
    {
        return ROCKETNET_MSGX_RECEIVE_DEAD;
    }
    return ROCKETNET_MSGX_RECEIVE_FAILED;
}

// proofs/tla/RNMsgxReceiveFill.tla Want: a read never asks for more
// datagrams than the bank has room for, so nothing it returns is dropped for
// want of room
int rocketnet_msgx_receive_want( int banked, int capacity, int batch )
{
#if defined( ROCKETNET_MSGX_RECEIVE_FILL_PLANT )
    // the trace check's plant: the read asks for a whole batch whatever the
    // room (RNMsgxReceiveFill ReadWhenFull), and what lands past the bank's
    // end is dropped, as the drain did before it kept the rule
    (void) banked;
    (void) capacity;
    return batch;
#else  // #if defined( ROCKETNET_MSGX_RECEIVE_FILL_PLANT )
    const int room = capacity - banked;
    if ( room <= 0 )
    {
        return 0;
    }
    return room < batch ? room : batch;
#endif // #if defined( ROCKETNET_MSGX_RECEIVE_FILL_PLANT )
}
