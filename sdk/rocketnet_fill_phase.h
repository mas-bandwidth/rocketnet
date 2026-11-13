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
// operation does nothing and returns. It keeps no thread-local state: a
// thread is named by rocketnet_platform_thread_id.
//
//   - Per client, a word [epoch:56][depth:7][in:1] and the id of the thread
//     that last claimed it (owner). in is set while a thread is inside a fill
//     call for the client and is the lock on the pair: only the thread that
//     set it writes either until it clears it. depth counts that thread's
//     nested fill calls for the client; 0 with in set is a claim being
//     decided.
//   - fills_in: 64 counters of the fill calls inside the check, one cache
//     line each; a thread counts on the line its id hashes to (threads that
//     share a line share a counter, which stays exact).
//   - fill_begin loads the epoch (FillBegin), sets in with one
//     compare-and-swap (FillLock), then decides (FillDecide): it refuses when
//     another thread claimed the client in this epoch, restoring the word.
//     Otherwise it stores itself as owner and the word [epoch, 1, in]. It
//     then increments its fills_in line (FillCount) and loads main_calls and
//     the epoch again (FillLook), refusing, and restoring the owner and the
//     word, when a main operation is running or ran since FillBegin.
//     fill_end clears the word's in (FillUnclaim), then decrements its
//     fills_in line (FillLeave).
//   - main_begin increments main_calls (MainAnnounce), refusing when another
//     main operation had, then loads every fills_in line (MainLook), refusing
//     at the first that is not 0. main_end advances the epoch (MainEpoch), so
//     the next fill phase finds every client unclaimed without a reset pass,
//     then decrements main_calls (MainDrop).
//   - Each side announces itself (the increment) before it looks at the other
//     (the load), both sequentially consistent, so when a main operation and
//     a fill call overlap in time at least one of them sees the other
//     (MCRNFillPhaseBrokenLookFirst looks first and lets both in). The lock is
//     one CAS (MCRNFillPhaseBrokenLoadStore lets two fillers in); the epoch
//     ends a phase's claims (MCRNFillPhaseBrokenNoReset flags a client that
//     moved to another thread in the next phase); the look reads the epoch
//     again (MCRNFillPhaseBrokenNoEpochLook lets a call that a main
//     operation ran inside pass with the old stamp, and a second filler in
//     the new phase with it); a refused look puts the owner and the word
//     back (MCRNFillPhaseBrokenNoRestore leaves the refused claim, and the
//     client's next filler, honest, is refused).
//   - Calls nest. The message layer's start calls the transport's
//     start_packet on the same thread for the same client: a fill call that
//     finds the client's word in, with depth 1 or more, and itself as owner,
//     is the outer call's own work and only counts depth. A fill call made by
//     the thread inside a main operation (a send made from a callback of
//     update) is that operation's own work and skips the check; so does a
//     nested main operation (main_depth). A main operation made from inside a
//     fill call is refused (fills_in is not 0).
//   - A client_index outside [0, num_clients) claims nothing: the call checks
//     only the overlap with a main operation (FillCount, FillLook, FillLeave).
//
// Cost per fill call: one CAS, two loads and two stores on the client's own
// cache line; one add and one subtract on the calling thread's fills_in line;
// two loads of main_calls and the epoch, which main writes twice a frame. A
// filler writes nothing another client's filler writes, except where two
// threads' ids hash to one fills_in line. Per main operation: two adds, two
// stores and 64 loads, independent of max_clients.
//
// The ack phase (SERVER-UPDATE-ACK.md P3; proofs/tla/RNAckPhase.tla). The
// split update (update_begin, the per-client ack calls, update_end) is one
// main-thread operation in three pieces: update_begin's main operation opens
// an ack phase (rocketnet_fill_phase_open_ack_phase) and update_end's closes
// it. ack_phase is written only inside a main operation, before its epoch
// moves, and read by a call after its FillLook, so a call reads the value of
// the phase it is in.
//   - Inside an ack phase only ack calls (ack_begin, ack_end) are taken, one
//     thread per client as for fills (the same word, owner and epoch). A fill
//     call there is a fill inside update: refused, FILL_PHASE_OVERLAP. Any
//     main operation but the one that ends the phase (main_begin with
//     ends_ack_phase) is refused the same way: two main operations at once.
//   - Outside an ack phase an ack call is refused (FILL_PHASE_OVERLAP: it is
//     update's work, made outside update), and so is a main operation that
//     claims to end one.
//   - Nested calls are their outer call's own work and are not checked again.
//
// What the check does not see: an id is reused after its thread exits, so a
// thread that ends and a new thread that gets its id are one filler to the
// check. The epoch is 56 bits and does not wrap in practice (2^56 main
// operations is over a million years at 240 a second).

#define ROCKETNET_FILL_PHASE_IN                 1ULL
#define ROCKETNET_FILL_PHASE_DEPTH_SHIFT        1
#define ROCKETNET_FILL_PHASE_DEPTH_MASK         0xFEULL
#define ROCKETNET_FILL_PHASE_DEPTH_MAX          127
#define ROCKETNET_FILL_PHASE_EPOCH_SHIFT        8
#define ROCKETNET_FILL_PHASE_FILL_LINES         64

// Cache-line alignment pads these on MSVC (C4324); the padding is the point.
#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4324 )
#endif // #if defined( _MSC_VER )
// the fill calls inside the check of the threads whose ids hash here
struct alignas( 64 ) rocketnet_fill_phase_line_t
{
    std::atomic<uint32_t> fills_in;
};

// one client's word and owner, written only by its filler
struct alignas( 64 ) rocketnet_fill_phase_client_t
{
    std::atomic<uint64_t> claim;
    std::atomic<uint64_t> owner;
};

// Every member a filler writes is on a cache line of its own; the struct is
// 64-aligned, which the library's allocations are (ROCKETNET_ALLOCATION_ALIGNMENT).
struct alignas( 64 ) rocketnet_fill_phase_t
{
    std::atomic<uint32_t> main_calls;
    std::atomic<uint64_t> epoch;
    std::atomic<uint64_t> main_owner;           // the thread inside a main operation, 0: none
    std::atomic<uint32_t> ack_phase;            // 1 between update_begin and update_end: only ack calls run
    int main_depth;                             // written and read only by main_owner
    int num_clients;
    uint64_t * overlap_counter;                 // FILL_PHASE_OVERLAP, NULL: not counted
    uint64_t * two_fillers_counter;             // FILL_TWO_FILLERS, NULL: not counted
    struct rocketnet_fill_phase_line_t lines[ROCKETNET_FILL_PHASE_FILL_LINES];
    struct rocketnet_fill_phase_client_t clients[ROCKETNET_MAX_CLIENTS];
};
#if defined( _MSC_VER )
#pragma warning( pop )
#endif // #if defined( _MSC_VER )

// the fills_in line of a thread id: the top bits of a Fibonacci hash
inline int rocketnet_fill_phase_line_of( uint64_t thread_id )
{
    return (int) ( ( thread_id * 0x9E3779B97F4A7C15ULL ) >> 58 );
}

// num_clients is the server's max_clients; the counters may be NULL
void rocketnet_fill_phase_init( struct rocketnet_fill_phase_t * fill_phase, int num_clients,
                                uint64_t * overlap_counter, uint64_t * two_fillers_counter );

// false: refused and counted (and asserted): the operation must not run.
// Every true is matched by one main_end on the same thread. ends_ack_phase:
// the operation is update_end, the one main operation an ack phase takes
// (and the only one it refuses outside an ack phase).
bool rocketnet_fill_phase_main_begin( struct rocketnet_fill_phase_t * fill_phase, bool ends_ack_phase = false );
void rocketnet_fill_phase_main_end( struct rocketnet_fill_phase_t * fill_phase );

// inside a main operation only: the phase that follows it is an ack phase
// (open) or not (close)
void rocketnet_fill_phase_open_ack_phase( struct rocketnet_fill_phase_t * fill_phase );
void rocketnet_fill_phase_close_ack_phase( struct rocketnet_fill_phase_t * fill_phase );

// false: refused and counted (and asserted): the call must do nothing. Every
// true is matched by one fill_end for the same client on the same thread.
bool rocketnet_fill_phase_fill_begin( struct rocketnet_fill_phase_t * fill_phase, int client_index );
void rocketnet_fill_phase_fill_end( struct rocketnet_fill_phase_t * fill_phase, int client_index );

// the per-client ack call's check: as fill_begin and fill_end, taken only
// inside an ack phase
bool rocketnet_fill_phase_ack_begin( struct rocketnet_fill_phase_t * fill_phase, int client_index );
void rocketnet_fill_phase_ack_end( struct rocketnet_fill_phase_t * fill_phase, int client_index );

// RAII forms for the call sites
struct rocketnet_fill_phase_main_scope_t
{
    struct rocketnet_fill_phase_t * fill_phase;
    bool entered;

    explicit rocketnet_fill_phase_main_scope_t( struct rocketnet_fill_phase_t * phase, bool ends_ack_phase = false )
        : fill_phase( phase ), entered( phase ? rocketnet_fill_phase_main_begin( phase, ends_ack_phase ) : true ) {}
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

struct rocketnet_fill_phase_ack_scope_t
{
    struct rocketnet_fill_phase_t * fill_phase;
    int client_index;
    bool entered;

    rocketnet_fill_phase_ack_scope_t( struct rocketnet_fill_phase_t * phase, int client )
        : fill_phase( phase ), client_index( client ), entered( phase ? rocketnet_fill_phase_ack_begin( phase, client ) : true ) {}
    ~rocketnet_fill_phase_ack_scope_t()
    {
        if ( fill_phase && entered )
        {
            rocketnet_fill_phase_ack_end( fill_phase, client_index );
        }
    }
    rocketnet_fill_phase_ack_scope_t( const rocketnet_fill_phase_ack_scope_t & ) = delete;
    rocketnet_fill_phase_ack_scope_t & operator=( const rocketnet_fill_phase_ack_scope_t & ) = delete;
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
