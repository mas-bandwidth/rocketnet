/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_WINDOW_ACK_H
#define ROCKETNET_WINDOW_ACK_H

#include "rocketnet_window_send.h"
#include "rocketnet_constants.h"

// Layer 3 of the message layer: the sent-packet map, the resend FIFO, RTT,
// RTT estimation, resend delay, failed-send recovery, and reliable timeout (docs/CHANNELS.md
// section 6.4; TLA+ actions WritePacket, WriteOldest, ReceiveAck and
// invariants TablesBounded and EveryReliableAckedAndDelivered in
// tla/RNWindow.tla).
//
// Pure over a carved region (rn_region_t, layer 1) and a sender window
// (rn_window_send_t, section 6.2): allocates nothing, takes no locks, and
// reads no wall clock. Callers pass the update's monotonic time `now` in
// seconds. Compiled with -ffp-contract=off (CMakeLists.txt) so the 5%
// smoothing arithmetic is deterministic across architectures.
//
// Every table this layer touches lives inside the connection's fixed region:
//   - region->sent_packets : P entries of 32 bytes (ring indexed by cursor % P)
//   - region->sent_index   : 2P entries of 16 bytes (open-addressed packet_id -> entry)
//   - region->sent_refs    : 16P u32 entries (ring of referenced message numbers)
//   - region->send_numbers : W entries of 32 bytes (sender per-number table)
//   - region->resend_fifo  : W entries of 8 bytes (intrusive prev/next links in 0..W-1),
//                            shared by the two resend FIFOs: a number is in at most one
//
// Each sent packet gets a fresh transport sequence `packet_id`, including
// every resend. An ack of `packet_id` therefore measures the exact send that
// produced that packet (study §7.2 item 21: no Karn ambiguity).

// the disconnect reasons (enum rocketnet_disconnect_reason_t)
#include "rocketnet.h"

#if ROCKETNET_DEV

// The resend gap record (development builds only; a release build has neither
// the fields nor the code). For each delay class (0: single-packet records,
// 1: chunks of a block) it holds the extremes of the gap between a number's
// send and its next resend, as measured where rn_window_ack_commit sets
// last_send, against the class delay in force (docs/CHANNELS.md 6.4 "Resend";
// tla/RNResendTiming.tla Walk). The walk resends a number at the first update
// whose delay has passed since its last send, so for every resend
//
//   delay at the resend <= gap < delay at the update before + the time between the two updates
//
// The delay of the earlier send is not a bound: an RTT sample between the two
// sends moves the resend delay, and the walk follows the delay in force at each update.
// Nothing allocates; a resend costs a class lookup and a few compares.
struct rn_resend_gaps_t
{
    double update_now;              // the update rn_window_ack_gaps_update last saw
    double update_delay[2];         // each class's delay at that update
    double prev_now;                // the update before it (0 before the second update)
    double prev_delay[2];           // each class's delay at the update before
    uint32_t count[2];              // resends measured
    double gap_min[2];              // smallest gap, seconds
    double gap_max[2];              // largest gap, seconds
    double early_min[2];            // min over resends of gap - delay at the resend (>= 0 by the rule)
    double late_max[2];             // max over resends of gap - delay at the update before (<= one update by the rule)
};

#endif // #if ROCKETNET_DEV

struct rn_window_ack_t
{
    rn_window_send_t * send;        // the sender's numbering window (U, U', next)
    rn_region_t * region;           // the connection's carved region

    double resend_delay_initial;             // seconds (default 2.0)
    double resend_delay_min;                 // optional resend delay floor (0.0 means no floor)
    double resend_delay_max;                 // seconds (default 2.0)
    double reliable_timeout;        // seconds (default 10.0)

    double srtt;                    // 5% smoothed RTT
    double rttvar;                  // deviation estimate
    double resend_delay;                     // adaptive resend delay; retries do not back off (tla/RNResendTiming.tla)
    uint8_t has_rtt;                // 0 before the first valid ack sample, 1 after
    uint8_t has_oldest;             // 1 while oldest_number is tracked at U
    uint16_t pad;
    int error;                      // ROCKETNET_DISCONNECT_REASON_NONE or ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT

    // Two resend FIFOs over the one link array region->resend_fifo, one per
    // delay class (docs/CHANNELS.md 6.4 "Resend"; tla/RNResendTiming.tla
    // SmallNeverHeldByChunk): single-packet records, and the chunks of a
    // block (a record whose send descriptor has more than one fragment). Each
    // is ordered by last send and walked from its head while due.
    uint32_t resend_head;           // oldest slot in the small-record FIFO (0..W-1), or RN_RESEND_NONE
    uint32_t resend_tail;           // newest slot in the small-record FIFO (0..W-1), or RN_RESEND_NONE
    uint32_t resend_count;          // number of slots linked in both FIFOs
    uint32_t chunk_head;            // oldest slot in the chunk FIFO (0..W-1), or RN_RESEND_NONE
    uint32_t chunk_tail;            // newest slot in the chunk FIFO (0..W-1), or RN_RESEND_NONE
    uint32_t walk_visits;           // FIFO entries the last resend walk examined (O(due): due + acked + one stop per FIFO)

    uint64_t pkt_head;              // oldest live or un-reclaimed slot in sent_packets
    uint64_t pkt_tail;              // next slot to write in sent_packets (pkt_tail - pkt_head <= P)
    uint64_t ref_head;              // start of pkt_head's references in sent_refs
    uint64_t ref_tail;              // next slot to write in sent_refs (ref_tail - ref_head <= 16P)

    uint64_t receipt_sweep;         // RNReceipts SweepLoss: next sent-packet entry to examine
    uint64_t receipt_head;          // oldest outcome still in the Rq ring
    uint64_t receipt_tail;          // next outcome slot
    uint64_t receipt_visible_head;  // this update's first outcome
    uint64_t receipt_visible_tail;  // this update's end; later send-side losses wait for the next update

    uint64_t oldest_number;         // message number U whose first_send is cached below
    double oldest_first_send;       // first_send time of oldest_number

#if ROCKETNET_DEV
    rn_resend_gaps_t gaps;          // test-visible resend gap record; development builds only
#endif // #if ROCKETNET_DEV
};

// The first deviation equals the first RTT sample. Later deviation
// moves 5% toward the absolute error from the prior smoothed RTT, then
// smoothed RTT moves 5% toward the ACK sample. Its resend delay is 2*srtt + 4*deviation + 30 ms,
// bounded by the configured minimum and maximum; default minimum 0 applies
// no floor (standard/MESSAGES.md m9.2).
static inline double rn_window_ack_compute_resend_delay( double srtt, double rttvar, double resend_delay_min, double resend_delay_max )
{
    const double candidate = 2.0 * srtt + 4.0 * rttvar + 0.030;
    return candidate < resend_delay_min ? resend_delay_min : ( candidate > resend_delay_max ? resend_delay_max : candidate );
}

// A single-packet record waits for its adaptive resend delay on every resend.
static inline double rn_window_ack_resend_delay( const rn_window_ack_t * ack )
{
    return ack->resend_delay;
}

// The resend delay of a chunk of a block uses the same adaptive resend delay,
// so a large block on a long path is not sent again before its acks can
// return (docs/CHANNELS.md 6.4 "Resend"; tla/RNResendTiming.tla Delay("c")).
// It is capped at reliable_timeout / ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS
// (ChunkCap) so the attempts rule holds for chunks under any accepted
// resend_delay_max; at defaults the 2 s resend delay stays below the 2.5 s chunk cap.
// The same for every resend, with no back-off.
static inline double rn_window_ack_chunk_resend_delay( const rn_window_ack_t * ack )
{
    const double cap = ack->reliable_timeout / (double) ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS;
    return ack->resend_delay < cap ? ack->resend_delay : cap;
}

// Whether the number in send_numbers slot `slot_index` is a chunk of a block:
// its send descriptor is in use with more than one fragment (docs/CHANNELS.md
// 8). It picks the number's resend FIFO and delay.
bool rn_window_ack_slot_is_chunk( const rn_window_ack_t * ack, uint32_t slot_index );

// The delay of the FIFO a slot is in: rn_window_ack_chunk_resend_delay for a
// chunk, rn_window_ack_resend_delay otherwise.
static inline double rn_window_ack_class_delay( const rn_window_ack_t * ack, bool chunk )
{
    return chunk ? rn_window_ack_chunk_resend_delay( ack ) : rn_window_ack_resend_delay( ack );
}

// Links a never-sent number into its class's resend FIFO right after `after`
// (a slot of the same FIFO), or at that FIFO's head when `after` is
// RN_RESEND_NONE. rn_ordered_write parks new numbers behind the never-sent
// ones already waiting, ahead of every number that was sent.
void rn_window_ack_park( rn_window_ack_t * ack, uint32_t after, uint32_t slot_index );

// Initializes the sent-packet map, resend FIFO, and RTT estimator over a
// carved region and an initialized sender window (docs/CHANNELS.md 6.4;
// tla/RNWindow.tla Init). NULL config uses rocketnet_message_default_config.
void rn_window_ack_init( rn_window_ack_t * ack, rn_window_send_t * send, rn_region_t * region, const rocketnet_message_config_t * config );

// Commits a sent packet carrying `count` reliable message numbers (1 <= count <= 16P)
// under `packet_id` at time `now` (docs/CHANNELS.md 6.4 "On send" and "Eviction";
// tla/RNWindow.tla WritePacket and WriteOldest).
// Updates each number's last_send = now (and first_send = now on its first send),
// links or moves each number to the tail of the resend FIFO, evicts the oldest
// sent-packet entry (counting MESSAGE_SENT_MAP_EVICTED) if the P-entry packet
// ring or 16P-entry reference ring needs room, and inserts packet_id -> entry
// into the 2P open-addressed sent_index.
void rn_window_ack_commit_packet( rn_window_ack_t * ack, uint64_t packet_id, const uint64_t * numbers, uint32_t count, double now );

// RNReceipts WritePacket: commit a packet containing code-5 serials to the
// same sent map as reliable numbers. Pure unreliable packets use count=0.
void rn_window_ack_commit_receipt_packet( rn_window_ack_t * ack, uint64_t packet_id, const uint32_t * serials, uint32_t count, double now );

// RNReceipts EmitAck/SweepLoss/TakeReceipt. Outcomes share the region's Rq
// ring; they are listed for one update without allocating or resending.
void rn_window_ack_receipt_begin_update( rn_window_ack_t * ack );
void rn_window_ack_receipt_end_update( rn_window_ack_t * ack );
void rn_window_ack_receipt_sweep( rn_window_ack_t * ack, double now );
void rn_window_ack_receipt_emit_loss( rn_window_ack_t * ack, uint32_t serial );
int rn_window_ack_receipts( const rn_window_ack_t * ack, rocketnet_message_receipt_t * out, int capacity );

// Processes one acked transport sequence `packet_id` at time `now`
// (docs/CHANNELS.md 6.4 "Ack" and "resend delay"; tla/RNWindow.tla ReceiveAck,
// tla/RNResendTiming.tla Sample).
// Looks up `packet_id` in sent_index in O(1). If absent (an ack of a keep-alive
// or pure-unreliable packet, a duplicate ack, or an ack arriving after eviction),
// does nothing and returns 0.
// If present: takes one RTT sample now - send_time, updates srtt, rttvar, and
// resend_delay = clamp(2*srtt + 4*rttvar + 30ms, resend_delay_min, resend_delay_max), marks
// each still-unacked referenced number >= U acked, unlinks it from the resend
// FIFO, frees its chunk in region->store (and frees its send descriptor back to Q
// when its last fragment is acked), advances U and U', and removes the entry
// from sent_index. Returns 1.
int rn_window_ack_on_ack( rn_window_ack_t * ack, uint64_t packet_id, double now );

// Processes a batch of `count` acked transport sequence numbers at time `now`
// (docs/CHANNELS.md 6.4 "On ack"). Returns the number of sent-packet entries matched.
uint32_t rn_window_ack_on_acks( rn_window_ack_t * ack, const uint64_t * packet_ids, uint32_t count, double now );

// Recovers reliable numbers from a packet that finish_packet refused
// (docs/CHANNELS.md 6.4 "Failed send"). The packet is not entered in the sent
// map; its reliable numbers keep their assigned numbers and ring offsets, are
// marked due now, and go to the head of the resend FIFO so the next update
// resends them first.
void rn_window_ack_packet_failed( rn_window_ack_t * ack, const uint64_t * numbers, uint32_t count, double now );

// Checks the per-connection reliable timeout before resending (docs/CHANNELS.md
// 6.4 "Giving up"). If U < next and the oldest unacked number at U has
// now - first_send >= reliable_timeout, sets ack->error to
// ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT and returns 1. Otherwise returns
// whether ack->error is already set.
int rn_window_ack_check_timeout( rn_window_ack_t * ack, double now );

// Test-only: the product's walk is rn_ordered_write (sdk/rocketnet_channel.cpp).
// Walks the small-record FIFO from its head while
// now >= last_send + rn_window_ack_resend_delay( ack ), then the chunk FIFO
// while now >= last_send + rn_window_ack_chunk_resend_delay( ack )
// (docs/CHANNELS.md 6.4 "Resend"; tla/RNWindow.tla WriteOldest,
// tla/RNResendTiming.tla Walk). Each walk stops at the first entry that is not
// yet due, so the work is O(due) and O(1) when nothing is due, and a chunk
// that is not due never holds back a small record that is. Each due number is committed in a new sent-packet entry
// with packet_id = (*io_next_packet_id)++, its last_send is set to `now`, it is
// moved to the tail of the resend FIFO, its number is written to `out_numbers`,
// and MESSAGE_RESENT is incremented. The delay is the same for every pass;
// nothing backs off.
// Returns the number of resent message numbers written to `out_numbers` (0 if
// none are due or if reliable_timeout has fired).
uint32_t rn_window_ack_resend( rn_window_ack_t * ack, double now, uint64_t * io_next_packet_id, uint64_t * out_numbers, uint32_t max_numbers );

// The resend gap record's clock (development builds only; a no-op otherwise):
// called once per update before the update's packets are written, with the
// update's time, after its acks have moved the resend delay. It keeps this update's and
// the previous update's class delays for rn_window_ack_commit's measure.
#if ROCKETNET_DEV
void rn_window_ack_gaps_update( rn_window_ack_t * ack, double now );
#else // #if ROCKETNET_DEV
static inline void rn_window_ack_gaps_update( rn_window_ack_t *, double ) {}
#endif // #if ROCKETNET_DEV

// Number of live (unacked, non-evicted) packet entries currently in the sent-packet map.
uint32_t rn_window_ack_entries_in_flight( const rn_window_ack_t * ack );

// Span of reference slots currently occupied between ref_head and ref_tail (<= 16P).
uint32_t rn_window_ack_refs_in_flight( const rn_window_ack_t * ack );

#endif // #ifndef ROCKETNET_WINDOW_ACK_H
