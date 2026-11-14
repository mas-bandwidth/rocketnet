/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP server socket's replay high-water writer
// (rocketnet_server_socket_replay_forget, _replay_mark and _replay_commit,
// sdk/rocketnet_server_socket_linux.cpp), over the pending buffer of
// sdk/rocketnet_xdp_replay_marks.h and the reply MAC table of
// sdk/rocketnet_xdp_frame.h: the walk's forget and mark (proofs/tla/RNXdpReplay.tla
// Walk) and the commit (Commit). The commit takes the calls into the kernel
// map as functions, so the socket passes its BPF calls and the trace check
// of the model (tests/tla_trace_xdp_replay.cpp) passes a map of its own.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_XDP_REPLAY_SINK_H
#define ROCKETNET_SDK_ROCKETNET_XDP_REPLAY_SINK_H

#include <stdint.h>
#include <string.h>

#include "rocketnet_address.h"
#include "rocketnet_xdp_frame.h"
#include "rocketnet_xdp_replay_marks.h"

// Forget appends to the pending buffer; true when it was queued.

static inline bool rocketnet_xdp_replay_sink_forget( struct rocketnet_xdp_replay_marks_t * marks, struct rocketnet_xdp_reply_macs_t * reply_macs,
                                                     const rocketnet_address_t * address )
{
    // proofs/tla/RNXdpReplay.tla Walk (pendingMacForget' = accepted): only a
    // queued forget drops the learned reply MAC; a refused one leaves it with
    // the slot that still owes the forget

    const bool queued = rocketnet_xdp_replay_marks_forget( marks, address );
    if ( queued )
    {
        rocketnet_xdp_reply_macs_forget( reply_macs, address );
    }

    return queued;
}

// Mark appends to the pending buffer; true when it was queued. A zero mark
// the buffer refused sets *zero_mark_refused.

static inline bool rocketnet_xdp_replay_sink_mark( struct rocketnet_xdp_replay_marks_t * marks, const rocketnet_address_t * address, uint32_t high_water,
                                                   bool * zero_mark_refused )
{
    // a new session's zero mark waits while the forget array is full, a kept
    // mark of its key or not (proofs/tla/RNXdpReplay.tla QueueRefusable): a
    // forget refused there leaves the walk's slot owing it, and the walk
    // then keeps a skip mark's owner by marking nothing, so no skip mark may
    // be taken and the greeting must wait (proofs/tla/RNServerReplaySlots.tla
    // ZeroMarkWaitsForForgets, GreetedSlotHasEntry). A refused mark leaves
    // the kept mark of its key as it was. The refusal holds every greeting
    // below and counts in the commit failures, as a full mark array does;
    // every new session waits while kernel deletes keep failing.

    if ( high_water == 0 && marks->forget_count >= marks->capacity )
    {
        marks->refused++;
        *zero_mark_refused = true;
        return false;
    }

    const bool queued = rocketnet_xdp_replay_marks_mark( marks, address, high_water );

    // a new session's zero mark the buffer refused: its key is not held
    // anywhere, so the next publish holds every keep-alive (proofs/tla/RNXdpReplay.tla
    // HoldGreetingUntilMarked, markOwed); the walk owes the mark and asks again

    if ( !queued && high_water == 0 )
    {
        *zero_mark_refused = true;
    }
    return queued;
}

// The commit: every pending forget, then every pending mark, through the
// kernel map calls, then the reset keeping what the kernel refused.
// forget_failed and mark_applied are capacity entries each, the commit's
// per-forget and per-mark outcomes. Moves *zero_mark_refused into
// *hold_all_greetings and clears it. Returns the refusals the pending buffer
// counted since the last commit, for the caller's failure count.

static inline uint64_t rocketnet_xdp_replay_commit( struct rocketnet_xdp_replay_marks_t * marks, bool * forget_failed, bool * mark_applied,
                                                    rocketnet_xdp_replay_batch_t * delete_batch, rocketnet_xdp_replay_single_t * delete_single,
                                                    rocketnet_xdp_replay_batch_t * update_batch, rocketnet_xdp_replay_single_t * update_single,
                                                    void * context, bool * zero_mark_refused, bool * hold_all_greetings )
{
    // proofs/tla/RNXdpReplay.tla, Commit: every forget is applied, then every mark,
    // in one commit; this is the only writer of the kernel map. A forget the
    // kernel refuses stays pending for the next commit (ForgetOwedUntilApplied)
    // until it lands or a newer mark of its key lands (RetryNeverDeletesNewerMark);
    // a mark the kernel refuses stays pending too (MarkOwedUntilApplied), so a
    // reconnect's refused zero mark is retried rather than lost.

    // a batch the kernel stops (at a missing key, or a refusal) goes on past
    // the entry it stopped at, which takes one single call so its outcome is
    // known; a kernel without the batch command is walked one call an entry
    // (rocketnet_xdp_replay_apply). An applied mark supersedes a refused
    // forget of the same key (RetryNeverDeletesNewerMark).

    memset( forget_failed, 0, marks->capacity * sizeof( bool ) );
    rocketnet_xdp_replay_apply( marks->forget_count, delete_batch, delete_single, context, forget_failed );

    memset( mark_applied, 0, marks->capacity * sizeof( bool ) );
    rocketnet_xdp_replay_apply( marks->mark_count, update_batch, update_single, context, mark_applied );
    for ( uint32_t i = 0; i < marks->mark_count; i++ )
    {
        mark_applied[i] = !mark_applied[i];
    }

    // the trace check's plant (tests/tla_trace_xdp_replay.cpp): every forget
    // the kernel refused is dropped, not kept, the reversed witness
    // MCRNXdpReplayBrokenFailedForgetDropped.cfg (KeepFailedForgets = FALSE)
#if defined( ROCKETNET_XDP_REPLAY_PLANT )
    memset( forget_failed, 0, marks->capacity * sizeof( bool ) );
#endif // #if defined( ROCKETNET_XDP_REPLAY_PLANT )

    // a full buffer's refusals land in the commit failure count; a refused
    // forget is kept unless an applied mark of its key superseded it, and a
    // refused mark is kept for the next commit

    const uint64_t refused = rocketnet_xdp_replay_marks_reset_keeping( marks, forget_failed, mark_applied );

    // the publish after this commit holds the greeting of every session whose
    // zero mark is kept or was not queued (rocketnet_server_socket_hold_greetings)

    *hold_all_greetings = *zero_mark_refused;
    *zero_mark_refused = false;

    return refused;
}

// The publish's decision for one keep-alive (a session's greeting): held, so
// not sent, when the last commit left a zero mark unqueued (hold_all) or the
// destination's kept mark is 0 (proofs/tla/RNXdpReplay.tla Commit, the greeting
// held until its mark is applied). rocketnet_server_socket_hold_greetings calls
// it for every keep-alive of a publish and the xdp-replay trace check for every
// session after a commit.

static inline bool rocketnet_xdp_replay_sink_holds_greeting( const struct rocketnet_xdp_replay_marks_t * marks, bool hold_all, const struct rocketnet_address_t * to )
{
    return hold_all || rocketnet_xdp_replay_marks_holds_greeting( marks, to );
}

#endif // ROCKETNET_SDK_ROCKETNET_XDP_REPLAY_SINK_H
