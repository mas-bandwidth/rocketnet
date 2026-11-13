/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_MSGX_SEND_RUNS_H
#define ROCKETNET_SDK_ROCKETNET_MSGX_SEND_RUNS_H

// The macOS batched server transport's send path over one taken bank
// (sdk/rocketnet_server_msgx_apple.cpp): the bank sorted by socket slot, the
// runs of one slot and one generation, and the cursor a send result moves.
//
// Pure: no socket and no clock. The connected send, the shared send, the
// table's resolve and its dead report are the caller's functions, so the unit
// tests run the real path over a fake. proofs/tla/RNMsgxSendRuns.tla models
// it at this layout.
//
// A bank holds count packets in staged order (the fill threads' claim order).
// Each carries packet_bytes (zero or less: aborted, or never finished; never
// sent), the table slot its client had at the stamp (-1: the shared socket)
// and that slot's generation.
//
// - The sort orders the bank by slot, the shared socket's packets first, the
//   staged order kept within a slot (a counting sort, stable).
// - The send walks the sorted order from *cursor. A packet whose stamp no
//   longer resolves to a socket (its socket was retired since) leaves alone
//   through the shared socket. Otherwise the run is the following packets of
//   the same slot and the same generation, at most batch live ones, and goes
//   in one connected send.
// - What the connected send answers moves *cursor:
//   all taken: past the run;
//   some taken: past exactly that many live packets, the rest kept, false;
//   none taken, or the socket full: kept, false;
//   refused (a pending ICMP error answered the call and is now cleared): the
//   run goes again once, and a second refusal is an error;
//   dead (the socket was disconnected): reported, and the run leaves through
//   the shared socket one packet at a time (a full shared socket keeps the
//   rest, false);
//   any other error: the run is dropped, counted.
// - false means a socket was full: *cursor is the first packet not yet
//   taken, and the caller calls again later with the same cursor.

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

// the bank's packets in slot order, the shared socket's (slot -1, and any
// slot outside the table) first, staged order kept within a slot. bucket
// holds capacity + 2 ints, order count ints (proofs/tla/RNMsgxSendRuns.tla
// Sort; StableSort FALSE is mutation 2)
template <typename Info>
inline void rocketnet_msgx_send_runs_sort( const Info * info, int count, int capacity, int * bucket, int * order )
{
    const int buckets = capacity + 1;
    for ( int b = 0; b <= buckets; b++ )
    {
        bucket[b] = 0;
    }
    for ( int i = 0; i < count; i++ )
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
    for ( int i = 0; i < count; i++ )
    {
        const int slot = info[i].table_slot;
        const int key = slot >= 0 && slot < capacity ? slot + 1 : 0;
        order[--bucket[key + 1]] = i;
    }
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 2
    for ( int i = 0; i < count; i++ )
    {
        const int slot = info[i].table_slot;
        const int key = slot >= 0 && slot < capacity ? slot + 1 : 0;
        order[bucket[key]++] = i;
    }
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 2
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

// Sends the sorted bank from *cursor; run holds batch ints. Returns false
// when a socket is full: *cursor is then the first packet not yet taken.
// The caller is inside one table pass for the whole call.
// proofs/tla/RNMsgxSendRuns.tla: the loop's top is LoopTop (the run is
// Build), the answers SendAll, SendPartial (Advance), SendFull, SendRefused,
// SendDead and SendError, the dead run's resend Dead
template <typename Info>
inline bool rocketnet_msgx_send_runs_send( const Info * info, const int * order, int count, int batch, int * run_packets, int * cursor,
                                           const struct rocketnet_msgx_send_runs_io_t * io, struct rocketnet_msgx_send_runs_counters_t * counters )
{
    while ( *cursor < count )
    {
        const int first = order[*cursor];
        const Info * head = &info[first];

        if ( head->packet_bytes <= 0 )
        {
            ( *cursor )++;                      // aborted or never finished: nothing to send
            continue;
        }

        const int fd = head->table_slot >= 0 ? io->resolve( io->user, head->table_slot, head->generation ) : -1;
        if ( fd < 0 )
        {
            const int shared = io->send_shared( io->user, first );
            if ( shared == ROCKETNET_MSGX_SHARED_FULL )
            {
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 6
                if ( head->table_slot >= 0 )
                {
                    counters->retired++;
                }
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 6
                return false;
            }
            if ( head->table_slot >= 0 )
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
            ( *cursor )++;
            continue;
        }

        // the run: this slot's packets at this generation, in staged order
        int run = 0;
        int scan = *cursor;
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
            run_packets[run++] = i;
        }

        int attempts = 0;
        for ( ;; )
        {
            const int result = io->send( io->user, fd, run_packets, run );
            if ( result >= run )
            {
                counters->sent_connected += (uint64_t) run;
                *cursor = scan;
                break;
            }
            if ( result > 0 )
            {
                // a prefix went: past exactly that many live packets, the
                // rest kept for the next call
                counters->sent_connected += (uint64_t) result;
                int taken = 0;
                while ( taken < result )
                {
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 1
                    taken++;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 1
                    if ( info[order[*cursor]].packet_bytes > 0 )
                    {
                        taken++;
                    }
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 1
                    ( *cursor )++;
                }
                return false;
            }
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 5
            if ( result == ROCKETNET_MSGX_SEND_FULL )
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 5
            if ( result == 0 || result == ROCKETNET_MSGX_SEND_FULL )
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 5
            {
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 8
                counters->dropped += (uint64_t) run;
                *cursor = scan;
                break;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 8
                return false;                   // nothing taken: the run is kept
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 8
            }
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 7
            if ( result == ROCKETNET_MSGX_SEND_REFUSED && attempts < 0 )
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 7
            if ( result == ROCKETNET_MSGX_SEND_REFUSED && attempts == 0 )
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 7
            {
                // the socket's pending ICMP error answered this call and is
                // now cleared: the run goes again once
                counters->refused++;
                attempts++;
                continue;
            }
            if ( result == ROCKETNET_MSGX_SEND_DEAD )
            {
                // the socket was disconnected under us: main retires it, and
                // the run leaves through the shared socket now
                counters->dead++;
                io->report_dead( io->user, head->table_slot, head->generation );
#if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 4
                *cursor = scan;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 4
                while ( *cursor < scan )
                {
                    const int i = order[*cursor];
                    if ( info[i].packet_bytes > 0 )
                    {
                        const int shared = io->send_shared( io->user, i );
                        if ( shared == ROCKETNET_MSGX_SHARED_FULL )
                        {
                            return false;
                        }
                        if ( shared == ROCKETNET_MSGX_SHARED_SENT )
                        {
                            counters->sent_shared++;
                        }
                        else
                        {
                            counters->dropped++;
                        }
                    }
                    ( *cursor )++;
                }
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_MUTATION ) && ROCKETNET_MSGX_SEND_RUNS_MUTATION == 4
                break;
            }
            counters->dropped += (uint64_t) run;
            *cursor = scan;
            break;
        }
    }
    return true;
}

#endif // ROCKETNET_SDK_ROCKETNET_MSGX_SEND_RUNS_H
