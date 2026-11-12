/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_FILL_PHASE_H
#define ROCKETNET_SDK_ROCKETNET_FILL_PHASE_H

#include "rocketnet_internal.h"

#include <atomic>
#if ROCKETNET_ASSERTS
#include <functional>
#include <thread>
#endif // #if ROCKETNET_ASSERTS

// The fill-phase contract (X0), and its debug-build check.
// Model: proofs/tla/RNFillPhase.tla (README-RNFillPhase.md).
//
// The contract, as rocketnet.h states it to the game:
//   - Main-thread operations: update, send_packets, stop, disconnect,
//     disconnect_client, and every other server call that is not a fill call.
//     One thread makes them, one at a time.
//   - Fill calls: start_packet, finish_packet and abort_packet, and the
//     message calls built on them. Any number of game threads make them,
//     strictly between one main-thread operation and the next; the handoff
//     both ways is a synchronizing operation (a join or a barrier).
//   - One filler per client: within one fill phase (from the end of one
//     main-thread operation to the start of the next), every fill call for a
//     given client index comes from one thread. Different clients may be
//     filled by different threads at the same time. The per-client state a
//     fill writes (the send history, the ack baseline, the order of that
//     client's staged packets) has one writer for that reason.
//
// What the transports get from it: start, finish and abort never run during
// flush, receive, stop or destroy, and every packet of one client in one
// fill phase was staged by one thread, in that thread's order.
//
// The check, in builds with ROCKETNET_ASSERTS (Debug), and nothing at all
// otherwise:
//   - each main-thread operation brackets itself with main_begin/main_end,
//     each fill call with fill_begin/fill_end. Each side announces itself
//     (an increment) before it looks at the other (a load), both
//     sequentially consistent, so when a main operation and a fill call
//     overlap in time at least one of them sees the other and asserts
//     (RNFillPhase Sound; the reversed witness MCRNFillPhaseBrokenLookFirst
//     looks first and lets an update run inside a fill unflagged);
//   - fill_begin claims the client for its thread with one compare-and-swap
//     on that client's owner word; a second thread's claim in the same fill
//     phase asserts (RNFillPhase OneFiller; reversed witness
//     MCRNFillPhaseBrokenLoadStore, a load then a store, lets two fillers in
//     unflagged). main_end clears every owner, so a client may move to
//     another thread in the next fill phase without a false alarm
//     (RNFillPhase NoFalseAlarm; reversed witness MCRNFillPhaseBrokenNoReset).
// A fill call is the call, not the lease: a lease that outlives a main
// operation (start, then update, then finish on one thread) is the
// sequential misuse the packet lease already refuses and counts as
// api_misuse, and is not asserted here.

#if ROCKETNET_ASSERTS

struct rocketnet_fill_phase_t
{
    std::atomic<uint32_t> main_calls{ 0 };
    std::atomic<uint32_t> fill_calls{ 0 };
    std::atomic<uint64_t> owners[ROCKETNET_MAX_CLIENTS];
};

// a nonzero token for the calling thread: its id, unique among live threads
static inline uint64_t rocketnet_fill_phase_thread_token()
{
    return (uint64_t) std::hash<std::thread::id>()( std::this_thread::get_id() ) | 1;
}

static inline void rocketnet_fill_phase_init( struct rocketnet_fill_phase_t * fill_phase )
{
    fill_phase->main_calls.store( 0 );
    fill_phase->fill_calls.store( 0 );
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        fill_phase->owners[i].store( 0, std::memory_order_relaxed );
    }
}

// RNFillPhase MainAnnounce, MainLook
static inline void rocketnet_fill_phase_main_begin( struct rocketnet_fill_phase_t * fill_phase )
{
    const uint32_t mains = fill_phase->main_calls.fetch_add( 1 );
    rocketnet_assert( mains == 0 );                              // two main-thread operations at once
    rocketnet_assert( fill_phase->fill_calls.load() == 0 );      // a main-thread operation inside a fill call
    (void) mains;
}

// RNFillPhase MainReset, MainLeave: the next fill phase begins with every
// client unowned
static inline void rocketnet_fill_phase_main_end( struct rocketnet_fill_phase_t * fill_phase )
{
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        fill_phase->owners[i].store( 0, std::memory_order_relaxed );
    }
    fill_phase->main_calls.fetch_sub( 1 );
}

// RNFillPhase FillAnnounce, FillLook, FillClaim. A client_index outside
// [0, ROCKETNET_MAX_CLIENTS) claims no client (a frame with no slot).
static inline void rocketnet_fill_phase_fill_begin( struct rocketnet_fill_phase_t * fill_phase, int client_index )
{
    fill_phase->fill_calls.fetch_add( 1 );
    rocketnet_assert( fill_phase->main_calls.load() == 0 );      // a fill call inside a main-thread operation

    if ( client_index < 0 || client_index >= ROCKETNET_MAX_CLIENTS )
    {
        return;
    }

    const uint64_t mine = rocketnet_fill_phase_thread_token();
    uint64_t owner = 0;
    if ( !fill_phase->owners[client_index].compare_exchange_strong( owner, mine ) )
    {
        rocketnet_assert( owner == mine );                       // two fillers on one client in one fill phase
    }
}

// RNFillPhase FillLeave
static inline void rocketnet_fill_phase_fill_end( struct rocketnet_fill_phase_t * fill_phase )
{
    const uint32_t fills = fill_phase->fill_calls.fetch_sub( 1 );
    rocketnet_assert( fills > 0 );
    (void) fills;
}

#else // #if ROCKETNET_ASSERTS

struct rocketnet_fill_phase_t
{
};

static inline void rocketnet_fill_phase_init( struct rocketnet_fill_phase_t * ) {}
static inline void rocketnet_fill_phase_main_begin( struct rocketnet_fill_phase_t * ) {}
static inline void rocketnet_fill_phase_main_end( struct rocketnet_fill_phase_t * ) {}
static inline void rocketnet_fill_phase_fill_begin( struct rocketnet_fill_phase_t *, int ) {}
static inline void rocketnet_fill_phase_fill_end( struct rocketnet_fill_phase_t * ) {}

#endif // #if ROCKETNET_ASSERTS

#endif // ROCKETNET_SDK_ROCKETNET_FILL_PHASE_H
