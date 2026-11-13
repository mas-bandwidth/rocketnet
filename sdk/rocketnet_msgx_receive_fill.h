/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_MSGX_RECEIVE_FILL_H
#define ROCKETNET_SDK_ROCKETNET_MSGX_RECEIVE_FILL_H

// The macOS batched server transport's receive worker over its three banks
// (sdk/rocketnet_server_msgx_apple.cpp): the drain of one ready socket into
// the worker's bank, and the bank handoff to main.
//
// Pure: no socket, no clock, no lock. The read, the banking of one datagram,
// the commit under the receive mutex, the counting of a failed read and, on
// main's side, the mutex try and the handing of a datagram to the server are
// the caller's functions, so the unit tests run the real path over a fake
// kernel. proofs/tla/RNMsgxReceiveFill.tla models it.
//
// The rule (the gso transport's, proofs/tla/RNGsoHandoff.tla RxFill): a
// datagram is read only into room. The drain asks the kernel for no more
// datagrams than the bank has room for; a full bank is left for an empty one
// at once when main has started on it, and otherwise the drain stops and the
// socket keeps the rest. The kernel's receive buffer is the queue, and nothing
// read is ever dropped for want of room.
//
// The handoff of the banks to main is the shared receive handoff
// (sdk/rocketnet_receive_handoff.h, proofs/tla/RNReceiveHandoff.tla): the
// worker commits its bank's count under the receive mutex at the end of each
// round and when its bank is full; main, at each receive call, tries the
// mutex once and reads the worker's bank in place up to the committed count,
// after the unread tail of the bank the worker left. A datagram the worker
// committed before main's call took the mutex is returned by that call when
// the call began with no datagrams left from a cut call and its capacity did
// not cut it (DeliveredByNextCall); a busy mutex, a cut call or a backlog
// leaves it to the calls after, under the usual backlog and capacity rules.
//
// proofs/tla/RNMsgxReceiveFill.tla actions: rocketnet_msgx_receive_drain_step
// is MoveFull, HoldFull, BudgetSpent, Read, ReadEmpty, ReadWeather and
// ReadFailed by its result; the worker loop around it is Poll, RoundCommit,
// RoundKeep and Sleep; main is Call (rocketnet_msgx_receive_call) and, after
// the stop, StopCount (rocketnet_msgx_receive_stop_count). The stop is
// MainStop, main's (rocketnet_server_msgx_stop sets the receiver's quit and
// rocketnet_server_msgx_receive_packets then hands the server nothing), and
// Exit, the worker's (rocketnet_server_msgx_receive_worker leaves its loop at
// the top once quit is set and stores stopped), both in
// sdk/rocketnet_server_msgx_apple.cpp.

#include "rocketnet_receive_handoff.h"

#include <stdint.h>

// main's side of one receive call (rocketnet_msgx_receive_call)
struct rocketnet_msgx_receive_call_io_t
{
    void * user;

    // the receive mutex: one try, never a wait (the main thread never blocks)
    bool ( *try_lock )( void * user );
    void ( *unlock )( void * user );

    // bank b emptied: main's finished tail, before a begin may free it for the
    // worker (its view expired when this call began)
    void ( *reset )( void * user, int bank );

    // the worker has left bank b: its count and the tallies in it are final,
    // so they are folded into main's totals, once
    void ( *fold )( void * user, int bank );

    // datagram index of bank b handed to the server this update; false when
    // the call has no room left (the update's capacity)
    bool ( *take )( void * user, int bank, uint32_t index );
};

// RNMsgxReceiveFill Call (RNReceiveHandoff Call), main's, at each receive
// call: a finished tail is emptied; one try of the mutex brings main up to
// what the worker has committed (rocketnet_receive_handoff_begin); then main
// hands the server the unread tail of the bank the worker left and its own
// bank up to the committed count, as room allows. A busy mutex leaves main
// with what it already held: the next call catches up. Returns the datagrams
// main saw for the first time in this call (each is counted received once);
// *busy says the mutex was busy
inline uint64_t rocketnet_msgx_receive_call( struct rocketnet_receive_handoff_t * handoff, const struct rocketnet_msgx_receive_call_io_t * io, bool * busy )
{
    uint64_t received = 0;
    *busy = false;

    if ( rocketnet_receive_handoff_tail_finished( handoff ) )
    {
        io->reset( io->user, handoff->tail );
    }

    if ( io->try_lock( io->user ) )
    {
        const uint32_t seen = handoff->main_limit;
        const int left = rocketnet_receive_handoff_begin( handoff );
        io->unlock( io->user );
        if ( left >= 0 )
        {
            received = (uint64_t) ( handoff->tail_end - seen ) + handoff->main_limit;
            io->fold( io->user, left );
        }
        else
        {
            received = (uint64_t) ( handoff->main_limit - seen );
        }
    }
    else
    {
        *busy = true;
    }

    while ( handoff->tail_cursor < handoff->tail_end && io->take( io->user, handoff->tail, handoff->tail_cursor ) )
    {
        handoff->tail_cursor++;
    }
    while ( handoff->main_cursor < handoff->main_limit && io->take( io->user, handoff->main, handoff->main_cursor ) )
    {
        handoff->main_cursor++;
    }

    return received;
}

// RNMsgxReceiveFill StopCount, main's, once, when it first finds the worker
// exited after the stop (main calls nothing after the stop): every datagram
// the worker banked that main never handed the server, the unread tail of the
// bank the worker left, main's bank past its cursor (committed or not) and the
// worker's bank when it had moved on, is returned to be counted
// RECEIVE_DROP_STOPPED. count[b] is bank b's count. The caller empties main's
// and the worker's banks and folds their tallies (the tail's were folded when
// the worker left it)
inline uint64_t rocketnet_msgx_receive_stop_count( struct rocketnet_receive_handoff_t * handoff, const uint32_t * count )
{
    uint64_t left = (uint64_t) ( handoff->tail_end - handoff->tail_cursor );
    handoff->tail_cursor = handoff->tail_end;
    if ( handoff->main_cursor < count[handoff->main] )
    {
        left += (uint64_t) ( count[handoff->main] - handoff->main_cursor );
    }
    handoff->main_cursor = 0;
    handoff->main_limit = 0;
    if ( handoff->worker != handoff->main )
    {
        left += (uint64_t) count[handoff->worker];
    }
    return left;
}

// what a failed read's errno means (rocketnet_msgx_receive_error_kind)
#define ROCKETNET_MSGX_RECEIVE_EMPTY                         0  // EAGAIN, EWOULDBLOCK: the socket is drained
#define ROCKETNET_MSGX_RECEIVE_WEATHER                       1  // EINTR, ENOBUFS, or an ICMP error queued for the client (EMSGSIZE too): skipped, read on
#define ROCKETNET_MSGX_RECEIVE_DEAD                          2  // ENOTCONN, EPIPE, EDESTADDRREQ: the connected socket is gone
#define ROCKETNET_MSGX_RECEIVE_FAILED                        3  // anything else

// the errno of a failed read as one of the kinds above
// (sdk/rocketnet_msgx_receive_fill.cpp)
int rocketnet_msgx_receive_error_kind( int error );

// the most datagrams the next read may ask for: the batch, or the room left
// in the bank when that is less; 0 when the bank is full
// (sdk/rocketnet_msgx_receive_fill.cpp, the trace check's plant)
int rocketnet_msgx_receive_want( int banked, int capacity, int batch );

struct rocketnet_msgx_receive_io_t
{
    void * user;

    // the datagrams in the worker's bank now
    int ( *banked )( void * user );

    // one batched read of at most want datagrams on fd: the count read
    // (0 for none), or -errno
    int ( *read )( void * user, int fd, int want );

    // datagram i of the last read: banked, or counted as a drop by its
    // reason (the prefilter's, an unreadable source). There is always room
    void ( *take )( void * user, int i );

    // the commit of the worker's bank count under the receive mutex
    // (rocketnet_receive_handoff_commit): true when the worker moved to an
    // empty bank (main had started on its bank)
    bool ( *commit )( void * user );

    // a failed read on fd, of a kind above but EMPTY, counted; DEAD reports
    // the socket to the table
    void ( *failed )( void * user, int fd, int kind, int error );
};

// the drain's place (RNMsgxReceiveFill pc)
#define ROCKETNET_MSGX_DRAIN_READ                            0  // reading the socket
#define ROCKETNET_MSGX_DRAIN_DONE                            1  // the socket is done for this round
#define ROCKETNET_MSGX_DRAIN_HELD                            2  // the bank is full and main has not started on it: stop the round

// what one step did (the model action it is)
#define ROCKETNET_MSGX_DRAIN_STEP_MOVE_FULL                  0  // MoveFull: a full bank committed and left for an empty one
#define ROCKETNET_MSGX_DRAIN_STEP_HOLD_FULL                  1  // HoldFull: a full bank kept, the socket keeps the rest
#define ROCKETNET_MSGX_DRAIN_STEP_BUDGET                     2  // BudgetSpent: this socket's reads for the round are spent
#define ROCKETNET_MSGX_DRAIN_STEP_READ                       3  // Read(n): n datagrams read and taken
#define ROCKETNET_MSGX_DRAIN_STEP_EMPTY                      4  // ReadEmpty: the socket is drained
#define ROCKETNET_MSGX_DRAIN_STEP_WEATHER                    5  // ReadWeather: skipped, one read spent
#define ROCKETNET_MSGX_DRAIN_STEP_FAILED                     6  // ReadFailed: counted, the socket left for the round

struct rocketnet_msgx_receive_drain_t
{
    int pc;
    int fd;
    int reads;                      // reads spent on this socket this round
};

inline void rocketnet_msgx_receive_drain_start( struct rocketnet_msgx_receive_drain_t * drain, int fd )
{
    drain->pc = ROCKETNET_MSGX_DRAIN_READ;
    drain->fd = fd;
    drain->reads = 0;
}

// One step of the drain of one socket (one action of
// proofs/tla/RNMsgxReceiveFill.tla). Every datagram a read returns and every
// failed read spends one of the socket's budget for the round, and a read
// asks for no more than the budget leaves, so a socket's reads in a round
// never pass the budget and a flooded socket cannot hold the others back.
// *count is what a Read took. Returns the step (ROCKETNET_MSGX_DRAIN_STEP_*);
// drain->pc says what is next.
inline int rocketnet_msgx_receive_drain_step( struct rocketnet_msgx_receive_drain_t * drain, const struct rocketnet_msgx_receive_io_t * io,
                                              int capacity, int batch, int budget, int * count )
{
    *count = 0;
    const int banked = io->banked( io->user );
    if ( banked >= capacity )
    {
        if ( io->commit( io->user ) )
        {
            return ROCKETNET_MSGX_DRAIN_STEP_MOVE_FULL;
        }
        drain->pc = ROCKETNET_MSGX_DRAIN_HELD;
        return ROCKETNET_MSGX_DRAIN_STEP_HOLD_FULL;
    }
    if ( drain->reads >= budget )
    {
        drain->pc = ROCKETNET_MSGX_DRAIN_DONE;
        return ROCKETNET_MSGX_DRAIN_STEP_BUDGET;
    }

    // no more than the room, and no more than the budget leaves: a socket's
    // reads in a round never pass the budget (RNMsgxReceiveFill Want)
    const int room_want = rocketnet_msgx_receive_want( banked, capacity, batch );
    const int want = room_want < budget - drain->reads ? room_want : budget - drain->reads;
    const int result = io->read( io->user, drain->fd, want );
    if ( result < 0 )
    {
        const int error = -result;
        const int kind = rocketnet_msgx_receive_error_kind( error );
        if ( kind == ROCKETNET_MSGX_RECEIVE_EMPTY )
        {
            drain->pc = ROCKETNET_MSGX_DRAIN_DONE;
            return ROCKETNET_MSGX_DRAIN_STEP_EMPTY;
        }
        io->failed( io->user, drain->fd, kind, error );
        drain->reads++;
        if ( kind == ROCKETNET_MSGX_RECEIVE_WEATHER )
        {
            return ROCKETNET_MSGX_DRAIN_STEP_WEATHER;
        }
        drain->pc = ROCKETNET_MSGX_DRAIN_DONE;
        return ROCKETNET_MSGX_DRAIN_STEP_FAILED;
    }
    if ( result == 0 )
    {
        drain->pc = ROCKETNET_MSGX_DRAIN_DONE;
        return ROCKETNET_MSGX_DRAIN_STEP_EMPTY;
    }

    const int n = result > want ? want : result;
    for ( int i = 0; i < n; i++ )
    {
        io->take( io->user, i );
    }
    drain->reads += n;
    *count = n;
    if ( n < want )
    {
        drain->pc = ROCKETNET_MSGX_DRAIN_DONE;      // a short read: the socket is drained
    }
    return ROCKETNET_MSGX_DRAIN_STEP_READ;
}

// The drain of one ready socket, step by step until it is done or held:
// true when it is done, false when the bank is full and main has not started
// on it, so the round stops and every socket keeps what it holds
inline bool rocketnet_msgx_receive_drain( const struct rocketnet_msgx_receive_io_t * io, int fd, int capacity, int batch, int budget )
{
    struct rocketnet_msgx_receive_drain_t drain;
    rocketnet_msgx_receive_drain_start( &drain, fd );
    while ( drain.pc == ROCKETNET_MSGX_DRAIN_READ )
    {
        int count = 0;
        rocketnet_msgx_receive_drain_step( &drain, io, capacity, batch, budget, &count );
    }
    return drain.pc == ROCKETNET_MSGX_DRAIN_DONE;
}

#endif // ROCKETNET_SDK_ROCKETNET_MSGX_RECEIVE_FILL_H
