/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_FILL_PHASE_H
#define ROCKETNET_SDK_ROCKETNET_FILL_PHASE_H

#include "rocketnet_internal.h"

#include <atomic>

// The fill-phase contract (X0) and its check, in every build.
// Model: proofs/tla/RNFillPhase.tla (README-RNFillPhase.md); trace check
// tests/tla_trace_fill_phase.cpp (TraceRNFillPhase.tla).
//
// The contract, as rocketnet.h states it to the game:
//   - Main-thread operations: update, send_packets, receive, stop, destroy,
//     disconnect, and the other server calls that change a client's state.
//     One thread makes them, one at a time.
//   - Fill calls: start_packet, finish_packet and abort_packet, and the
//     message calls built on them (start, finish and abort of a message).
//     Any number of game threads make them, strictly between one main-thread
//     operation and the next; the handoff both ways is a synchronizing
//     operation (a join or a barrier).
//   - One filler per client: within one fill phase (from the end of one
//     main-thread operation to the start of the next), every fill call for a
//     given client index comes from one thread. Different clients may be
//     filled by different threads at the same time. The per-client state a
//     fill writes (the open message, the unreliable build, the send history,
//     the ack baseline, the order of that client's staged packets) has one
//     writer for that reason.
//
// The check. A break of the contract is refused and counted in every build
// (FILL_PHASE_OVERLAP, FILL_TWO_FILLERS) and asserts in a build with
// ROCKETNET_ASSERTS. A refused fill call does nothing (start returns NULL,
// finish 0, abort leaves the lease to the next reset); a refused main-thread
// operation does nothing and returns.
//
//   - one word per client: [epoch:32][thread:31][in:1]. thread is the
//     claiming thread's token (nonzero, unique per thread for the process),
//     epoch the fill phase it was claimed in, in set while that thread is
//     inside a fill call for the client.
//   - fill_begin claims the client with one compare-and-swap that sets in,
//     then looks at main_calls (RNFillPhase FillClaim, FillLook). It refuses
//     when another thread is inside a call for the client, or claimed it in
//     this epoch (OneFiller), or when a main operation is running (Sound).
//     fill_end clears in (FillLeave).
//   - main_begin increments main_calls, then looks at every client's in bit
//     (MainAnnounce, MainLook), and refuses when one is set (Sound) or when a
//     second main operation is running. main_end advances the epoch, so the
//     next fill phase finds every client unclaimed without a reset pass
//     (MainLeave; NoFalseAlarm), then decrements main_calls.
//   - Each side announces itself (the CAS, the increment) before it looks at
//     the other (the load), both sequentially consistent, so when a main
//     operation and a fill call overlap in time at least one of them sees the
//     other (the reversed witness MCRNFillPhaseBrokenLookFirst looks first and
//     lets both in). The claim is one CAS (MCRNFillPhaseBrokenLoadStore lets
//     two fillers in); the epoch ends a phase's claims
//     (MCRNFillPhaseBrokenNoReset flags a client that moved to another thread
//     in the next phase).
//   - Calls nest: the message layer's start calls the transport's
//     start_packet on the same thread for the same client. A per-thread depth
//     makes only the outermost call of each kind check; an inner call is the
//     outer call's own work.
//
// Cost per fill call: one CAS and one load on shared words that main writes
// twice a frame, and one store; the client's word is written only by its
// filler. Per main operation: one atomic add and a scan of max_clients words.
//
// The epoch wraps after 2^32 main operations (about 200 days at four a
// frame and 60 Hz): a client last claimed exactly 2^32 phases earlier by
// another thread, and not since, would read as claimed for one call. Stated,
// not handled.

#define ROCKETNET_FILL_PHASE_IN                 1ULL
#define ROCKETNET_FILL_PHASE_THREAD_MASK        0xFFFFFFFEULL
#define ROCKETNET_FILL_PHASE_EPOCH_SHIFT        32

struct rocketnet_fill_phase_t
{
    std::atomic<uint32_t> main_calls;
    std::atomic<uint32_t> epoch;
    int num_clients;
    uint64_t * overlap_counter;                 // FILL_PHASE_OVERLAP, NULL: not counted
    uint64_t * two_fillers_counter;             // FILL_TWO_FILLERS, NULL: not counted
    std::atomic<uint64_t> claims[ROCKETNET_MAX_CLIENTS];
};

// num_clients is the server's max_clients; the counters may be NULL
void rocketnet_fill_phase_init( struct rocketnet_fill_phase_t * fill_phase, int num_clients,
                                uint64_t * overlap_counter, uint64_t * two_fillers_counter );

// false: refused and counted (and asserted): the operation must not run.
// Every true is matched by one main_end on the same thread.
bool rocketnet_fill_phase_main_begin( struct rocketnet_fill_phase_t * fill_phase );
void rocketnet_fill_phase_main_end( struct rocketnet_fill_phase_t * fill_phase );

// false: refused and counted (and asserted): the call must do nothing. Every
// true is matched by one fill_end for the same client on the same thread. A
// client_index outside [0, num_clients) claims no client and checks only the
// overlap with a main operation.
bool rocketnet_fill_phase_fill_begin( struct rocketnet_fill_phase_t * fill_phase, int client_index );
void rocketnet_fill_phase_fill_end( struct rocketnet_fill_phase_t * fill_phase, int client_index );

// the calling thread's token: nonzero, even, unique among the process's threads
uint32_t rocketnet_fill_phase_thread_token();

// RAII forms for the call sites
struct rocketnet_fill_phase_main_scope_t
{
    struct rocketnet_fill_phase_t * fill_phase;
    bool entered;

    explicit rocketnet_fill_phase_main_scope_t( struct rocketnet_fill_phase_t * phase )
        : fill_phase( phase ), entered( phase ? rocketnet_fill_phase_main_begin( phase ) : true ) {}
    ~rocketnet_fill_phase_main_scope_t()
    {
        if ( fill_phase && entered )
        {
            rocketnet_fill_phase_main_end( fill_phase );
        }
    }
    rocketnet_fill_phase_main_scope_t( const rocketnet_fill_phase_main_scope_t & ) = delete;
    rocketnet_fill_phase_main_scope_t & operator=( const rocketnet_fill_phase_main_scope_t & ) = delete;
};

struct rocketnet_fill_phase_fill_scope_t
{
    struct rocketnet_fill_phase_t * fill_phase;
    int client_index;
    bool entered;

    rocketnet_fill_phase_fill_scope_t( struct rocketnet_fill_phase_t * phase, int client )
        : fill_phase( phase ), client_index( client ), entered( phase ? rocketnet_fill_phase_fill_begin( phase, client ) : true ) {}
    ~rocketnet_fill_phase_fill_scope_t()
    {
        if ( fill_phase && entered )
        {
            rocketnet_fill_phase_fill_end( fill_phase, client_index );
        }
    }
    rocketnet_fill_phase_fill_scope_t( const rocketnet_fill_phase_fill_scope_t & ) = delete;
    rocketnet_fill_phase_fill_scope_t & operator=( const rocketnet_fill_phase_fill_scope_t & ) = delete;
};

#if ROCKETNET_DEV
// the trace check's recorder (tests/tla_trace_fill_phase.cpp): called at the
// end of each step proofs/tla/RNFillPhase.tla names, after all of that step's
// shared effects (a refusal's count included), on the thread taking it, so a
// recorder that parks the thread there sees the state the model's step
// leaves; NULL (the default): nothing
typedef void rocketnet_fill_phase_trace_function_t( void * context, const char * action, int client_index, uint64_t word, bool refused );
void rocketnet_fill_phase_set_trace( rocketnet_fill_phase_trace_function_t * function, void * context );
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_FILL_PHASE_H
