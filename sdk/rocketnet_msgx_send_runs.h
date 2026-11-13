/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_MSGX_SEND_RUNS_H
#define ROCKETNET_SDK_ROCKETNET_MSGX_SEND_RUNS_H

// The macOS batched server transport's send path over one taken bank
// (sdk/rocketnet_server_msgx_darwin.cpp): the bank sorted by fill phase and
// socket slot, the runs of one slot and one generation, and the cursor a
// send result moves.
//
// Pure: no socket and no clock. The connected send, the shared send, the
// table's resolve and its dead report are the caller's functions, so the unit
// tests and the trace check (tests/tla_trace_msgx_send_runs.cpp) run the real
// path over a fake. proofs/tla/RNMsgxSendRuns.tla models it at this layout;
// rocketnet_msgx_send_runs_step is one step of that model.
//
// A bank holds count packets in staged order (the fill threads' claim order).
// Each carries packet_bytes (zero or less: aborted, or never finished; never
// sent), the table slot its client had at the stamp (-1: the shared socket),
// that slot's generation, and the fill phase it was staged in (the update
// count main advances in commit).
//
// - The sort cuts the bank into runs of one fill phase, in staged order, and
//   orders each by slot, the shared socket's packets first, the staged order
//   kept within a slot (a counting sort, stable). Inside one fill phase a
//   client has one stamp, so its packets keep their staged order; across a
//   phase the table may have retired or opened its socket, so its stamp may
//   differ, and the phases go in staged order.
// - The send walks the sorted order from the cursor. A packet whose stamp no
//   longer resolves to a socket (its socket was retired since) leaves alone
//   through the shared socket. Otherwise the run is the following packets of
//   the same slot and the same generation, at most batch live ones, and goes
//   in one connected send.
// - What the connected send answers moves the cursor:
//   all taken: past the run;
//   some taken: past exactly that many live packets, the rest kept;
//   none taken, or the socket full: kept;
//   refused (a pending ICMP error answered the call and is now cleared): the
//   run goes again once, and a second refusal is an error;
//   dead (the socket was disconnected): reported, and the run leaves through
//   the shared socket one packet at a time (a full shared socket keeps the
//   rest);
//   any other error: the run is dropped, counted.
// - A kept bank stops with the cursor on the first packet not yet taken, and
//   the caller sends again later from the same cursor.

#include <stdint.h>

// the connected send's answer when nothing was taken (a count taken is >= 0)
#define ROCKETNET_MSGX_SEND_FULL                            -1  // EAGAIN, EWOULDBLOCK, ENOBUFS: try again later
#define ROCKETNET_MSGX_SEND_REFUSED                         -2  // ECONNREFUSED
#define ROCKETNET_MSGX_SEND_DEAD                            -3  // ENOTCONN, EPIPE, EDESTADDRREQ
#define ROCKETNET_MSGX_SEND_ERROR                           -4  // anything else

// the shared send's answer for one packet
#define ROCKETNET_MSGX_SHARED_SENT                           0
#define ROCKETNET_MSGX_SHARED_FULL                           1  // try again later
#define ROCKETNET_MSGX_SHARED_DROPPED                        2  // an error, or no address to send to

// the send's place (RNMsgxSendRuns pc)
#define ROCKETNET_MSGX_SEND_RUNS_HEAD                        0  // the loop's top
#define ROCKETNET_MSGX_SEND_RUNS_SEND                        1  // a run built, its connected send next
#define ROCKETNET_MSGX_SEND_RUNS_DEAD                        2  // a dead run's resend on the shared socket
#define ROCKETNET_MSGX_SEND_RUNS_KEPT                        3  // a socket was full: the rest is kept
#define ROCKETNET_MSGX_SEND_RUNS_DONE                        4  // every packet left

// ROCKETNET_MSGX_SEND_RUNS_MUTATION selects one deliberate defect for the
// mutation check of tests/test_unit_msgx_send_runs.cpp; never defined in a
// real build:
//  1 a partial send counts aborted packets as taken
//  2 an unstable sort (reversed within a slot)
//  3 a run crosses a generation
//  4 dead without a resend (the run is skipped)
//  5 a send that took nothing read as an error (the run dropped)
//  6 retired counted per shared attempt, a full retry counted twice
//  7 a refusal never retried
//  8 a full socket drops the run
//  9 an aborted packet inside a run is sent
// 10 the sort ignores the fill phase (the slot alone), as before the phase
// ROCKETNET_MSGX_SEND_RUNS_PLANT is the trace check's plant
// (proofs/tla/README-TraceCheck.md), the same change as mutation 10: the
// traces of tests/tla_trace_msgx_send_runs.cpp must then fail
// TraceConsistent. Never defined outside the plant build.

#if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT ) || ( defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 10 )
#define ROCKETNET_MSGX_SEND_RUNS_PHASE_BLIND 1
#endif

struct rocketnet_msgx_send_runs_io_t
{
    void * user;

    // the descriptor of the slot at that generation, or -1 (retired since)
    int ( *resolve )( void * user, int table_slot, uint32_t generation );

    // one batched send of packets[0..num_packets) on fd: the count taken
    // (a prefix), or one of ROCKETNET_MSGX_SEND_FULL .. _ERROR
    int ( *send )( void * user, int fd, const int * packets, int num_packets );

    // one packet on the shared socket: ROCKETNET_MSGX_SHARED_*
    int ( *send_shared )( void * user, int packet );

    // the socket of the slot at that generation answered dead
    void ( *report_dead )( void * user, int table_slot, uint32_t generation );
};

struct rocketnet_msgx_send_runs_counters_t
{
    uint64_t sent_connected;        // packets the connected sends took
    uint64_t sent_shared;           // packets the shared socket took
    uint64_t dropped;               // packets dropped on an error
    uint64_t refused;               // refusals retried
    uint64_t retired;               // packets that left through the shared socket because their socket was retired
    uint64_t dead;                  // dead answers
};

// the send's state across steps and calls (RNMsgxSendRuns pc, cursor, hd,
// scan, run, attempts); run_packets holds the batch
struct rocketnet_msgx_send_runs_state_t
{
    int pc;
    int cursor;
    int head;
    int scan;
    int run;
    int attempts;
    int fd;                         // the run's socket, resolved at the build (the table holds still for the call)
    int * run_packets;
};

inline void rocketnet_msgx_send_runs_start( struct rocketnet_msgx_send_runs_state_t * state, int * run_packets )
{
    state->pc = ROCKETNET_MSGX_SEND_RUNS_HEAD;
    state->cursor = 0;
    state->head = 0;
    state->scan = 0;
    state->run = 0;
    state->attempts = 0;
    state->fd = -1;
    state->run_packets = run_packets;
}

// order[lo..hi) = packets lo..hi-1 by slot, the shared socket's (slot -1, and
// any slot outside the table) first, staged order kept within a slot.
// bucket holds capacity + 2 ints.
template <typename Info>
inline void rocketnet_msgx_send_runs_sort_range( const Info * info, int lo, int hi, int capacity, int * bucket, int * order )
{
    const int buckets = capacity + 1;
    for ( int b = 0; b <= buckets; b++ )
    {
        bucket[b] = 0;
    }
    for ( int i = lo; i < hi; i++ )
    {
        const int slot = info[i].table_slot;
        const int key = slot >= 0 && slot < capacity ? slot + 1 : 0;
        bucket[key + 1]++;
    }
    for ( int b = 0; b < buckets; b++ )
    {
        bucket[b + 1] += bucket[b];
    }
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 2
    for ( int i = lo; i < hi; i++ )
    {
        const int slot = info[i].table_slot;
        const int key = slot >= 0 && slot < capacity ? slot + 1 : 0;
        order[lo + --bucket[key + 1]] = i;
    }
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 2
    for ( int i = lo; i < hi; i++ )
    {
        const int slot = info[i].table_slot;
        const int key = slot >= 0 && slot < capacity ? slot + 1 : 0;
        order[lo + bucket[key]++] = i;
    }
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 2
}

// the bank in send order: each run of one fill phase in staged order, sorted
// by slot inside (proofs/tla/RNMsgxSendRuns.tla Sort; PhaseSort FALSE is
// mutation 10 and the plant). bucket holds capacity + 2 ints, order count.
template <typename Info>
inline void rocketnet_msgx_send_runs_sort( const Info * info, int count, int capacity, int * bucket, int * order )
{
    int lo = 0;
    while ( lo < count )
    {
        int hi = lo + 1;
#if !defined( ROCKETNET_MSGX_SEND_RUNS_PHASE_BLIND )
        while ( hi < count && info[hi].phase == info[lo].phase )
        {
            hi++;
        }
#else  // #if !defined( ROCKETNET_MSGX_SEND_RUNS_PHASE_BLIND )
        hi = count;
#endif // #if !defined( ROCKETNET_MSGX_SEND_RUNS_PHASE_BLIND )
        rocketnet_msgx_send_runs_sort_range( info, lo, hi, capacity, bucket, order );
        lo = hi;
    }
}

// the live packets at sorted positions [cursor, count): what is still owed
template <typename Info>
inline int rocketnet_msgx_send_runs_owed( const Info * info, const int * order, int count, int cursor )
{
    int owed = 0;
    for ( int p = cursor; p < count; p++ )
    {
        if ( info[order[p]].packet_bytes > 0 )
        {
            owed++;
        }
    }
    return owed;
}

// the shared send of one packet, counted; false: full, nothing changed
template <typename Info>
inline bool rocketnet_msgx_send_runs_shared( const Info * info, int packet, bool retired, const struct rocketnet_msgx_send_runs_io_t * io,
                                             struct rocketnet_msgx_send_runs_counters_t * counters )
{
    const int shared = io->send_shared( io->user, packet );
    if ( shared == ROCKETNET_MSGX_SHARED_FULL )
    {
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 6
        if ( retired )
        {
            counters->retired++;
        }
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 6
        return false;
    }
    (void) info;
    if ( retired )
    {
        counters->retired++;
    }
    if ( shared == ROCKETNET_MSGX_SHARED_SENT )
    {
        counters->sent_shared++;
    }
    else
    {
        counters->dropped++;
    }
    return true;
}

// One step of the send (one action of proofs/tla/RNMsgxSendRuns.tla): at
// HEAD the loop's top (LoopTop: done, an aborted packet skipped, a packet
// whose stamp resolves to no socket sent alone on the shared socket, or the
// run built), at SEND one connected send and what its answer does (SendAll,
// SendPartial, SendFull, SendRefused, SendDead, SendError), at DEAD one
// packet of the dead run's resend (Dead). KEPT and DONE take no step.
// The caller is inside one table pass for every step of one call.
template <typename Info>
inline void rocketnet_msgx_send_runs_step( const Info * info, const int * order, int count, int batch, struct rocketnet_msgx_send_runs_state_t * state,
                                           const struct rocketnet_msgx_send_runs_io_t * io, struct rocketnet_msgx_send_runs_counters_t * counters )
{
    if ( state->pc == ROCKETNET_MSGX_SEND_RUNS_HEAD )
    {
        if ( state->cursor >= count )
        {
            state->pc = ROCKETNET_MSGX_SEND_RUNS_DONE;
            return;
        }
        const int first = order[state->cursor];
        const Info * head = &info[first];
        if ( head->packet_bytes <= 0 )
        {
            state->cursor++;                    // aborted or never finished: nothing to send
            return;
        }
        const int fd = head->table_slot >= 0 ? io->resolve( io->user, head->table_slot, head->generation ) : -1;
        if ( fd < 0 )
        {
            if ( !rocketnet_msgx_send_runs_shared( info, first, head->table_slot >= 0, io, counters ) )
            {
                state->pc = ROCKETNET_MSGX_SEND_RUNS_KEPT;
                return;
            }
            state->cursor++;
            return;
        }

        // the run: this slot's packets at this generation, in staged order
        int run = 0;
        int scan = state->cursor;
        while ( scan < count && run < batch )
        {
            const int i = order[scan];
            const Info * packet = &info[i];
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 3
            if ( packet->table_slot != head->table_slot )
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 3
            if ( packet->table_slot != head->table_slot || packet->generation != head->generation )
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 3
            {
                break;
            }
            scan++;
#if !defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) || ROCKETNET_MSGX_SEND_RUNS_MUTATION != 9
            if ( packet->packet_bytes <= 0 )
            {
                continue;
            }
#endif // #if !defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) || ROCKETNET_MSGX_SEND_RUNS_MUTATION != 9
            state->run_packets[run++] = i;
        }
        state->head = first;
        state->scan = scan;
        state->run = run;
        state->attempts = 0;
        state->fd = fd;
        state->pc = ROCKETNET_MSGX_SEND_RUNS_SEND;
        return;
    }

    if ( state->pc == ROCKETNET_MSGX_SEND_RUNS_SEND )
    {
        const Info * head = &info[state->head];
        const int run = state->run;
        const int result = io->send( io->user, state->fd, state->run_packets, run );
        if ( result >= run )
        {
            counters->sent_connected += (uint64_t) run;
            state->cursor = state->scan;
            state->pc = ROCKETNET_MSGX_SEND_RUNS_HEAD;
            return;
        }
        if ( result > 0 )
        {
            // a prefix went: past exactly that many live packets, the rest
            // kept for the next call
            counters->sent_connected += (uint64_t) result;
            int taken = 0;
            while ( taken < result )
            {
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 1
                taken++;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 1
                if ( info[order[state->cursor]].packet_bytes > 0 )
                {
                    taken++;
                }
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 1
                state->cursor++;
            }
            state->pc = ROCKETNET_MSGX_SEND_RUNS_KEPT;
            return;
        }
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 5
        if ( result == ROCKETNET_MSGX_SEND_FULL )
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 5
        if ( result == 0 || result == ROCKETNET_MSGX_SEND_FULL )
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 5
        {
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 8
            counters->dropped += (uint64_t) run;
            state->cursor = state->scan;
            state->pc = ROCKETNET_MSGX_SEND_RUNS_HEAD;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 8
            state->pc = ROCKETNET_MSGX_SEND_RUNS_KEPT;   // nothing taken: the run is kept
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 8
            return;
        }
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 7
        if ( result == ROCKETNET_MSGX_SEND_REFUSED && state->attempts < 0 )
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 7
        if ( result == ROCKETNET_MSGX_SEND_REFUSED && state->attempts == 0 )
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 7
        {
            // the socket's pending ICMP error answered this call and is now
            // cleared: the run goes again once
            counters->refused++;
            state->attempts++;
            return;
        }
        if ( result == ROCKETNET_MSGX_SEND_DEAD )
        {
            // the socket was disconnected under us: main retires it, and the
            // run leaves through the shared socket now
            counters->dead++;
            io->report_dead( io->user, head->table_slot, head->generation );
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 4
            state->cursor = state->scan;
            state->pc = ROCKETNET_MSGX_SEND_RUNS_HEAD;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 4
            state->pc = ROCKETNET_MSGX_SEND_RUNS_DEAD;
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 4
            return;
        }
        counters->dropped += (uint64_t) run;
        state->cursor = state->scan;
        state->pc = ROCKETNET_MSGX_SEND_RUNS_HEAD;
        return;
    }

    if ( state->pc == ROCKETNET_MSGX_SEND_RUNS_DEAD )
    {
        if ( state->cursor >= state->scan )
        {
            state->pc = ROCKETNET_MSGX_SEND_RUNS_HEAD;
            return;
        }
        const int i = order[state->cursor];
        if ( info[i].packet_bytes > 0 && !rocketnet_msgx_send_runs_shared( info, i, false, io, counters ) )
        {
            state->pc = ROCKETNET_MSGX_SEND_RUNS_KEPT;
            return;
        }
        state->cursor++;
    }
}

// A kept send goes on from its cursor at HEAD (RNMsgxSendRuns Kept, the
// branch that does not give up)
inline void rocketnet_msgx_send_runs_resume( struct rocketnet_msgx_send_runs_state_t * state )
{
    if ( state->pc == ROCKETNET_MSGX_SEND_RUNS_KEPT )
    {
        state->pc = ROCKETNET_MSGX_SEND_RUNS_HEAD;
    }
}

// Sends the sorted bank from the state's cursor, step by step. Returns false
// when a socket is full: the cursor is then the first packet not yet taken,
// and a later call goes on from it.
template <typename Info>
inline bool rocketnet_msgx_send_runs_send( const Info * info, const int * order, int count, int batch, struct rocketnet_msgx_send_runs_state_t * state,
                                           const struct rocketnet_msgx_send_runs_io_t * io, struct rocketnet_msgx_send_runs_counters_t * counters )
{
    rocketnet_msgx_send_runs_resume( state );
    while ( state->pc != ROCKETNET_MSGX_SEND_RUNS_KEPT && state->pc != ROCKETNET_MSGX_SEND_RUNS_DONE )
    {
        rocketnet_msgx_send_runs_step( info, order, count, batch, state, io, counters );
    }
    return state->pc == ROCKETNET_MSGX_SEND_RUNS_DONE;
}

// The worker after a kept call (RNMsgxSendRuns Kept): past limit retries
// after quit it gives up, true, and the caller drops what is still owed
// (rocketnet_msgx_send_runs_owed from the cursor), counted.
inline bool rocketnet_msgx_send_runs_give_up( bool shutdown, int * retries_after_quit, int limit )
{
    return shutdown && ++( *retries_after_quit ) > limit;
}

#endif // #ifndef ROCKETNET_SDK_ROCKETNET_MSGX_SEND_RUNS_H
