/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_msgx_send_runs.h"

#include <errno.h>

// What one connected send's errno means for that one client's run, in line
// with the platform's receive weather (rocketnet_platform_receive_weather)
// and the gso transport's send:
// - a full socket, a full buffer pool or an interrupted call keeps the run
//   for the next attempt (FULL);
// - an ICMP error the socket queued for the client (port, host or network
//   unreachable or down), or the route to it gone, answered this call and is
//   cleared by it: the run goes again once, and a second answer drops it,
//   counted (REFUSED). The socket is not dead for good and the server is not
//   at fault;
// - a disconnected socket is dead (DEAD);
// - anything else drops the run, counted (ERROR).
int rocketnet_msgx_send_error_kind( int error )
{
    if ( error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS || error == EINTR )
    {
        return ROCKETNET_MSGX_SEND_FULL;
    }
    if ( error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH || error == ENETDOWN )
    {
        return ROCKETNET_MSGX_SEND_REFUSED;
    }
#if defined( EHOSTDOWN )
    if ( error == EHOSTDOWN )
    {
        return ROCKETNET_MSGX_SEND_REFUSED;
    }
#endif // #if defined( EHOSTDOWN )
    if ( error == ENOTCONN || error == EPIPE || error == EDESTADDRREQ )
    {
        return ROCKETNET_MSGX_SEND_DEAD;
    }
    return ROCKETNET_MSGX_SEND_ERROR;
}

// The run builder's generation test, out of line: the trace check's plant
// (ROCKETNET_MSGX_SEND_RUNS_PLANT) must be library code the plant binary
// compiles in place of the library's (proofs/tla/README-TraceCheck.md): only
// tests/tla_trace_msgx_send_runs.cpp in the plant build defines it, and its
// traces must fail TraceConsistent. The rest of the send path stays inline in
// its header.

// proofs/tla/RNMsgxSendRuns.tla Build (GenBreak): a run ends at a change of
// generation, so two packets of one slot are in one run only at one
// generation
bool rocketnet_msgx_send_runs_same_generation( uint32_t generation, uint32_t other )
{
#if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
    // the trace check's plant: a run crosses a generation (RNMsgxSendRuns
    // GenBreak FALSE), as mutation 3
    (void) generation;
    (void) other;
    return true;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
    return generation == other;
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
}
