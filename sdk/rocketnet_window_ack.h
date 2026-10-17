/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_WINDOW_ACK_H
#define ROCKETNET_WINDOW_ACK_H

#include "rocketnet_window_send.h"

// Layer 3 of the message layer: the sent-packet map, the resend FIFO, RTT,
// RTO, resend delay, failed-send recovery, and reliable timeout (docs/CHANNELS.md
// section 6.4; TLA+ actions WritePacket, WriteOldest, ReceiveAck and
// invariants TablesBounded and EveryReliableAckedAndDelivered in
// tla/RNWindow.tla).
//
// Pure over a carved region (rn_region_t, layer 1) and a sender window
// (rn_window_send_t, section 6.2): allocates nothing, takes no locks, and
// reads no wall clock. Callers pass the update's monotonic time `now` in
// seconds. Compiled with -ffp-contract=off (CMakeLists.txt) so RFC 6298
// floating-point arithmetic is deterministic across architectures.
//
// Every table this layer touches lives inside the connection's fixed region:
//   - region->sent_packets : P entries of 32 bytes (ring indexed by cursor % P)
//   - region->sent_index   : 2P entries of 16 bytes (open-addressed packet_id -> entry)
//   - region->sent_refs    : 16P u32 entries (ring of referenced message numbers)
//   - region->send_numbers : W entries of 32 bytes (sender per-number table)
//   - region->resend_fifo  : W entries of 8 bytes (intrusive prev/next links in 0..W-1)
//
// Each sent packet gets a fresh transport sequence `packet_id`, including
// every resend. An ack of `packet_id` therefore measures the exact send that
// produced that packet (study §7.2 item 21: no Karn ambiguity).

#ifndef ROCKETNET_MESSAGE_ERROR_NONE
#define ROCKETNET_MESSAGE_ERROR_NONE                0
#define ROCKETNET_MESSAGE_ERROR_CONFIG_MISMATCH     1
#define ROCKETNET_MESSAGE_ERROR_CONFIG_TIMEOUT      2
#define ROCKETNET_MESSAGE_ERROR_PROTOCOL_VIOLATION  3
#define ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT    4
#endif

struct rn_window_ack_t
{
    rn_window_send_t * send;        // the sender's numbering window (U, U', next)
    rn_region_t * region;           // the connection's carved region

    double rto_initial;             // seconds (default 0.1)
    double rto_min;                 // seconds (default 0.01)
    double rto_max;                 // seconds (default 0.25)
    double reliable_timeout;        // seconds (default 10.0)
    double resend_interval;         // seconds (default 0.1): the cap on the resend delay
    double rto_scale;               // rto_scale_percent / 100 (default 1.25)

    double srtt;                    // smoothed RTT (RFC 6298)
    double rttvar;                  // RTT variation (RFC 6298)
    double rto;                     // current clamped RTO; there is no back-off (tla/RNResendTiming.tla)
    uint8_t has_rtt;                // 0 before the first valid ack sample, 1 after
    uint8_t has_oldest;             // 1 while oldest_number is tracked at U
    uint16_t pad;
    int error;                      // ROCKETNET_MESSAGE_ERROR_NONE or ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT

    uint32_t resend_head;           // oldest slot in the resend FIFO (0..W-1), or RN_RESEND_NONE
    uint32_t resend_tail;           // newest slot in the resend FIFO (0..W-1), or RN_RESEND_NONE
    uint32_t resend_count;          // number of unacked slots currently linked in the resend FIFO

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
};

// The resend delay of every record: min( rto, resend_interval ), the same for
// every resend, with no back-off (docs/CHANNELS.md 6.4 "Resend";
// tla/RNResendTiming.tla Delay with MaxBackoff = 1). The three resend walks
// (rn_window_ack_resend, rn_ordered_write, rn_message_ordered_due) read it.
static inline double rn_window_ack_resend_delay( const rn_window_ack_t * ack )
{
    return ack->rto < ack->resend_interval ? ack->rto : ack->resend_interval;
}

// Initializes the sent-packet map, resend FIFO, and RFC 6298 estimator over a
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
// (docs/CHANNELS.md 6.4 "Ack" and "RTO"; tla/RNWindow.tla ReceiveAck,
// tla/RNResendTiming.tla Sample).
// Looks up `packet_id` in sent_index in O(1). If absent (an ack of a keep-alive
// or pure-unreliable packet, a duplicate ack, or an ack arriving after eviction),
// does nothing and returns 0.
// If present: takes one RTT sample now - send_time, updates srtt, rttvar, and
// rto = clamp(srtt * rto_scale + 2 * rttvar, rto_min, rto_max), marks
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
// ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT and returns 1. Otherwise returns
// whether ack->error is already set.
int rn_window_ack_check_timeout( rn_window_ack_t * ack, double now );

// Walks the resend FIFO from its head while
// now >= last_send + rn_window_ack_resend_delay( ack ) (docs/CHANNELS.md 6.4
// "Resend"; tla/RNWindow.tla WriteOldest, tla/RNResendTiming.tla Resend).
// Stops at the first entry that is not yet due, so the walk is O(due) and O(1)
// when nothing is due. Each due number is committed in a new sent-packet entry
// with packet_id = (*io_next_packet_id)++, its last_send is set to `now`, it is
// moved to the tail of the resend FIFO, its number is written to `out_numbers`,
// and MESSAGE_RESENT is incremented. The delay is the same for every pass;
// nothing backs off.
// Returns the number of resent message numbers written to `out_numbers` (0 if
// none are due or if reliable_timeout has fired).
uint32_t rn_window_ack_resend( rn_window_ack_t * ack, double now, uint64_t * io_next_packet_id, uint64_t * out_numbers, uint32_t max_numbers );

// Number of live (unacked, non-evicted) packet entries currently in the sent-packet map.
uint32_t rn_window_ack_entries_in_flight( const rn_window_ack_t * ack );

// Span of reference slots currently occupied between ref_head and ref_tail (<= 16P).
uint32_t rn_window_ack_refs_in_flight( const rn_window_ack_t * ack );

#endif // #ifndef ROCKETNET_WINDOW_ACK_H
