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
// the publish under the receive mutex and the counting of a failed read are
// the caller's functions, so the unit tests run the real path over a fake
// kernel. proofs/tla/RNMsgxReceiveFill.tla models it.
//
// The rule (the gso transport's, proofs/tla/RNGsoHandoff.tla RxFill and
// RxPublish at 5a2dbf403): a datagram is read only into room. The drain asks the kernel
// for no more datagrams than the bank has room for; a full bank is handed to
// main at once when main has taken the last one (no ready bank waits and a
// free bank exists), and otherwise the drain stops and the socket keeps the
// rest. The kernel's receive buffer is the queue, and nothing read is ever
// dropped for want of room.
//
// The banks are indices 0 to 2 (ROCKETNET_MSGX_RECEIVE_NO_BANK for none):
// the worker fills one, at most one is ready for main, main holds one, and
// the last is free.
//
// proofs/tla/RNMsgxReceiveFill.tla actions: rocketnet_msgx_receive_drain_step
// is PublishFull, HoldFull, BudgetSpent, Read, ReadEmpty, ReadWeather and
// ReadFailed by its result; the worker loop around it is Poll, RoundPublish,
// RoundKeep and Sleep; main is Consume and Claim
// (rocketnet_msgx_receive_claim).

#include <stdint.h>

#define ROCKETNET_MSGX_RECEIVE_NO_BANK                     ( -1 )

struct rocketnet_msgx_receive_banks_t
{
    int worker;                     // the bank the receive worker fills
    int ready;                      // published, waiting for main, or NO_BANK
    int main;                       // main's view
    int free;                       // nobody's, or NO_BANK
};

// worker 0, main 1, free 2, none ready
inline void rocketnet_msgx_receive_banks_init( struct rocketnet_msgx_receive_banks_t * banks )
{
    banks->worker = 0;
    banks->main = 1;
    banks->free = 2;
    banks->ready = ROCKETNET_MSGX_RECEIVE_NO_BANK;
}

// RxPublish, under the receive mutex: the worker's bank becomes ready and the
// worker takes the free bank, only when none is ready and one is free;
// otherwise nothing moves and the worker keeps its bank
inline bool rocketnet_msgx_receive_publish( struct rocketnet_msgx_receive_banks_t * banks )
{
    if ( banks->ready != ROCKETNET_MSGX_RECEIVE_NO_BANK || banks->free == ROCKETNET_MSGX_RECEIVE_NO_BANK )
    {
        return false;
    }
    banks->ready = banks->worker;
    banks->worker = banks->free;
    banks->free = ROCKETNET_MSGX_RECEIVE_NO_BANK;
    return true;
}

// RxClaim, under the receive mutex, once main has consumed its whole bank:
// the ready bank becomes main's and main's old bank is free. False with
// nothing ready
inline bool rocketnet_msgx_receive_claim( struct rocketnet_msgx_receive_banks_t * banks )
{
    if ( banks->ready == ROCKETNET_MSGX_RECEIVE_NO_BANK )
    {
        return false;
    }
    banks->free = banks->main;
    banks->main = banks->ready;
    banks->ready = ROCKETNET_MSGX_RECEIVE_NO_BANK;
    return true;
}

// what a failed read's errno means (rocketnet_msgx_receive_error_kind)
#define ROCKETNET_MSGX_RECEIVE_EMPTY                         0  // EAGAIN, EWOULDBLOCK: the socket is drained
#define ROCKETNET_MSGX_RECEIVE_WEATHER                       1  // EINTR, ENOBUFS, or an ICMP error queued for the client: skipped, read on
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

    // the publish exchange (rocketnet_msgx_receive_publish) under the
    // receive mutex: true when the worker now fills an empty bank
    bool ( *publish )( void * user );

    // a failed read on fd, of a kind above but EMPTY, counted; DEAD reports
    // the socket to the table
    void ( *failed )( void * user, int fd, int kind, int error );
};

// the drain's place (RNMsgxReceiveFill pc)
#define ROCKETNET_MSGX_DRAIN_READ                            0  // reading the socket
#define ROCKETNET_MSGX_DRAIN_DONE                            1  // the socket is done for this round
#define ROCKETNET_MSGX_DRAIN_HELD                            2  // the bank is full and main has not taken the last: stop the round

// what one step did (the model action it is)
#define ROCKETNET_MSGX_DRAIN_STEP_PUBLISH_FULL               0  // PublishFull: a full bank handed over
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
// failed read spends one of the socket's budget for the round, so a flooded
// socket cannot hold the others back. *count is what a Read took. Returns
// the step (ROCKETNET_MSGX_DRAIN_STEP_*); drain->pc says what is next.
inline int rocketnet_msgx_receive_drain_step( struct rocketnet_msgx_receive_drain_t * drain, const struct rocketnet_msgx_receive_io_t * io,
                                              int capacity, int batch, int budget, int * count )
{
    *count = 0;
    const int banked = io->banked( io->user );
    if ( banked >= capacity )
    {
        if ( io->publish( io->user ) )
        {
            return ROCKETNET_MSGX_DRAIN_STEP_PUBLISH_FULL;
        }
        drain->pc = ROCKETNET_MSGX_DRAIN_HELD;
        return ROCKETNET_MSGX_DRAIN_STEP_HOLD_FULL;
    }
    if ( drain->reads >= budget )
    {
        drain->pc = ROCKETNET_MSGX_DRAIN_DONE;
        return ROCKETNET_MSGX_DRAIN_STEP_BUDGET;
    }

    const int want = rocketnet_msgx_receive_want( banked, capacity, batch );
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
// true when it is done, false when the bank is full and main has not taken
// the last one, so the round stops and every socket keeps what it holds
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
