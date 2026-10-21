/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_ACK_H
#define ROCKETNET_ACK_H

#include <stdint.h>
#include "rocketnet_wire.h"
#include "rocketnet_constants.h"
#include "rocketnet_encryption_map.h"

// The general ack system, per STANDARD.md _Acks_: the transport tells a
// sender which of its packets arrived, and never resends anything. The
// sealed plaintext of every session packet begins with an ack block built
// from the receiver's replay state; a sender that reads one marks its own
// sent packets acknowledged, surfaces the newly-acknowledged packet ids to
// the layer above, and samples round trip time from the first block that
// acknowledges each sequence.

// -----------------------------------------
//                           the ack block
// -----------------------------------------

// The decoded block: _largest_ is the reconstructed 64-bit largest received
// session sequence; the first range is a received run ending at largest,
// inclusive; each entry below states a gap of missing sequences then a run
// of received ones. Below the last entry the block is silent.

struct rocketnet_ack_block_t
{
    uint64_t largest;
    uint32_t first_range_length;
    int num_ranges;
    uint16_t range_gap[ROCKETNET_ACK_BLOCK_MAX_RANGES];
    uint16_t range_length[ROCKETNET_ACK_BLOCK_MAX_RANGES];
};

// encodes a block. the caller guarantees the fields are a valid block and
// output holds 7 + 4 * num_ranges bytes. returns the bytes written.

int rocketnet_ack_block_write( uint8_t * output, const struct rocketnet_ack_block_t * block );

// Builds a block from the receiver's replay state -- the one received state
// the standard names, no second high-water mark -- truncating oldest-first
// to the capacity: as many ranges as exist and fit, range count at most 255.
// capacity must be at least the 7-byte minimum, which always fits. returns
// the bytes written.

int rocketnet_ack_block_build( uint8_t * output, int capacity, const struct rocketnet_replay_protection_t * replay );

// Reconstruction of the block's largest received, per _Acks_: the wire
// sequence's rule exactly, with expected = the sender's own highest sent
// session sequence (not plus one).

uint64_t rocketnet_reconstruct_ack_largest( uint32_t wire_value, uint64_t highest_sent );

// Decodes and validates a block at the front of opened session data,
// evaluating validity on the reconstructed 64-bit largest. Every rule in
// _Acks_/Validity: geometry contained in the opened size, first range
// length zero exactly when largest is zero, first range length never
// exceeding largest, no zero gap or length entry, and the walk never
// falling below sequence one -- computed without unsigned underflow.
// Returns the block's byte length (7 + 4 * count), or 0 when the block is
// malformed, which refuses its whole packet as BAD_ACK_BLOCK.

int rocketnet_ack_block_read( struct rocketnet_ack_block_t * block, const uint8_t * data, int data_bytes, uint64_t highest_sent );

// -----------------------------------------
//                     sender bookkeeping
// -----------------------------------------

// Per-connection delivery state retains a bounded number of session sequences.
// This is a memory budget, independent of frame, send and ACK cadence. Entries
// do not expire with time. Capacity overflow evicts the oldest sequence history
// without throttling transmission; lost history is counted explicitly.

struct rocketnet_sent_packet_record_t
{
    uint64_t sequence;
    uint64_t packet_id;
    uint64_t aggregate_id;                  // nonzero: a fragment of an aggregated payload
    double send_time;
    uint32_t wire_bytes;
    uint8_t acked;
    uint8_t has_packet_id;
    uint8_t was_keep_alive;                 // a keep-alive's ack is the peer's claim to hold its client index
    uint8_t pad[1];
};

// A fragmented payload carries ONE application packet id, surfaced
// acknowledged only when every fragment's wire packet is acknowledged --
// sender-internal aggregation, nothing on the wire. A payload is surfaced once
// all its recorded fragments acknowledge. Aggregate and sequence history are
// finite: a lapped aggregate is recognized by its id, never misattributed to a
// newer payload. Fill writers serialize reservation and publication.

#define ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE      ( ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 )

struct rocketnet_packet_aggregate_t
{
    uint64_t id;                            // monotonic, 0 = empty
    uint64_t packet_id;
    uint32_t remaining;
    uint32_t pad;
};

struct rocketnet_received_packet_record_t
{
    uint64_t sequence;
    double receive_time;
    uint32_t wire_bytes;
    uint32_t pad;
};

// The sent ring's shadow bitmaps: one bit per slot, held (written) and
// unacknowledged. The walk reads them instead of visiting every sequence in
// the named run, so acknowledged and never-written sequences cost nothing.

#define ROCKETNET_SENT_BITMAP_WORDS ( ROCKETNET_SENT_PACKET_BUFFER_SIZE / 64 )
static_assert( ROCKETNET_SENT_PACKET_BUFFER_SIZE % 64 == 0,
               "the sent ring must be a multiple of 64 for the bitmaps" );

// Server statistics observe only these fields. The live ACK endpoint remains
// the protocol authority; a worker merges dirty projections into its own
// history and never reads an endpoint concurrently with the update/fill paths.
struct rocketnet_ack_stats_sent_t
{
    uint64_t sequence;
    double send_time;
    uint32_t wire_bytes;
    uint8_t acked;
    uint8_t pad[3];
};

struct rocketnet_ack_stats_received_t
{
    uint64_t sequence;
    double receive_time;
    uint32_t wire_bytes;
    uint32_t pad;
};

static_assert( sizeof(rocketnet_ack_stats_sent_t) == 24, "stats sent projection size" );
static_assert( sizeof(rocketnet_ack_stats_received_t) == 24, "stats received projection size" );

struct rocketnet_ack_stats_history_t
{
    uint64_t highest_sent;
    uint64_t highest_received;
    rocketnet_ack_stats_sent_t sent[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    rocketnet_ack_stats_received_t received[ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    float rtt_history[ROCKETNET_RTT_HISTORY_SIZE];
};

#define ROCKETNET_RECEIVED_STATS_DIRTY_WORDS ( ( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 63 ) / 64 )
#define ROCKETNET_RTT_STATS_DIRTY_WORDS ( ( ROCKETNET_RTT_HISTORY_SIZE + 63 ) / 64 )

struct rocketnet_ack_stats_delta_t
{
    rocketnet_ack_stats_history_t values;
    uint64_t sent_dirty[ROCKETNET_SENT_BITMAP_WORDS];
    uint64_t received_dirty[ROCKETNET_RECEIVED_STATS_DIRTY_WORDS];
    uint64_t rtt_dirty[ROCKETNET_RTT_STATS_DIRTY_WORDS];
    uint64_t generation;
    uint8_t reset;
};

struct rocketnet_ack_endpoint_t
{
    // NULL for clients and worker scratch. The server binds this pointer
    // after endpoint initialization and preserves it across slot reset.
    rocketnet_ack_stats_delta_t * stats_delta;
    uint64_t sent_lock;                         // guards sent-record reservation and publication
    // highest_sequence folds under fill-phase threads with an atomic max;
    // ack_debt increments on the update thread alone and resets from fill
    // threads with an atomic store -- update and the fill phase never
    // overlap, per the transports' threading contract

    uint64_t highest_sequence;                  // highest session sequence recorded as sent
    uint64_t largest_acked_sequence;            // highest sent sequence the peer has acknowledged; update thread only
    uint64_t highest_received_sequence;         // highest session sequence recorded as received
    uint64_t ack_debt;                          // eliciting packets received since the last block sent

    // published per-connection statistics, recomputed by update_stats --
    // amortized round-robin across slots on the server, on each stats read on the
    // client. rtt and jitter in milliseconds, loss as a percentage.
    float rtt;
    float jitter;
    float packet_loss;
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
    float bandwidth_acked_kbps;
    int has_rtt_sample;                         // any first-ack sample has landed

    // Latched by process_block the first time a sequence recorded as a
    // keep-alive is newly acknowledged. This is the peer's CLAIM to have
    // received a keep-alive -- and so to hold its client index and be
    // connected, since a keep-alive carries the index and processing one is
    // the client's only transition to _connected_. It is a claim and not a
    // proof: an ack block is the peer's own statement about its own receive
    // state, and no receiver can verify it. _Acks_ requires a block to name
    // only sequences its sender opened; a peer that violates that latches
    // this bit without its index and strands only itself. It is what
    // _Keep-alive generation_ flips the suppression regime on; see
    // rocketnet_server_protocol_write_keep_alive_if_due. Cleared by reset,
    // so a reused slot starts with no claim recorded.

    uint8_t keep_alive_acked;

    volatile uint64_t num_aggregates;           // monotonic; the next aggregate's id is this plus one

    uint64_t sent_held[ROCKETNET_SENT_BITMAP_WORDS];     // bit (slot) set: the record at slot was written; fill under sent_lock
    uint64_t sent_unacked[ROCKETNET_SENT_BITMAP_WORDS];  // bit (slot) set: written and not yet acknowledged
    uint64_t sent_bits_checked;                          // update thread only: highest sequence whose slot's bits were reconciled

    struct rocketnet_sent_packet_record_t sent[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    struct rocketnet_received_packet_record_t received[ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    struct rocketnet_packet_aggregate_t aggregates[ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
    float rtt_history[ROCKETNET_RTT_HISTORY_SIZE];      // milliseconds; negative = empty
};

void rocketnet_ack_endpoint_reset( struct rocketnet_ack_endpoint_t * endpoint );

// records one sent session packet. packet_id is surfaced on ack only when
// has_packet_id is nonzero -- keep-alive, disconnect and standalone ack
// sequences have none and surface nothing. aggregate_id names the
// fragmented payload the packet belongs to, or zero; a fragment record
// carries no packet id of its own. Concurrent aliases are serialized, with the
// newer sequence winning even if writers finish out of order. False reports
// history overflow (an unacknowledged record was evicted, or this writer fell
// behind the retained sequence). It is advisory: callers count it and still
// transmit. It never imposes a send-rate or ACK-rate requirement.
//
// record_sent_classified says what the write did to the record it replaced,
// the four outcomes of tla/RNSentHistory.tla:
//
//   RECORDED         the slot was empty or its record was acknowledged:
//                    nothing was lost from the history.
//   EVICTED_LOST     the evicted record was never acknowledged and the peer
//                    has acknowledged a sequence at least
//                    ROCKETNET_LOSS_PACKET_THRESHOLD above it (RFC 9002
//                    6.1.1): the packet is known lost on the path. An outcome,
//                    counted on SEND_HISTORY_LOST.
//   EVICTED_UNKNOWN  the evicted record was never acknowledged and no such
//                    later acknowledgment exists: its fate is unknown (a
//                    silent peer, or a window too small for the path). A
//                    refusal of history, counted on SEND_HISTORY_OVERFLOW.
//   REFUSED_STALE    the write was not recorded: the slot already holds this
//                    or a newer sequence, or the writer fell behind the
//                    retained window. Counted on SEND_HISTORY_OVERFLOW.
//
// The packet is transmitted in every case. record_sent is the same call
// reduced to whether the history lost nothing: true only for RECORDED.

enum rocketnet_ack_sent_result_t
{
    ROCKETNET_ACK_SENT_RECORDED,
    ROCKETNET_ACK_SENT_EVICTED_LOST,
    ROCKETNET_ACK_SENT_EVICTED_UNKNOWN,
    ROCKETNET_ACK_SENT_REFUSED_STALE
};

enum rocketnet_ack_sent_result_t rocketnet_ack_endpoint_record_sent_classified( struct rocketnet_ack_endpoint_t * endpoint,
                                                                                uint64_t sequence, int wire_bytes,
                                                                                uint64_t packet_id, int has_packet_id,
                                                                                uint64_t aggregate_id, double time );

bool rocketnet_ack_endpoint_record_sent( struct rocketnet_ack_endpoint_t * endpoint,
                                         uint64_t sequence, int wire_bytes,
                                         uint64_t packet_id, int has_packet_id,
                                         uint64_t aggregate_id, double time );

// counts a record_sent_classified outcome on the two history counters:
// EVICTED_LOST on lost, EVICTED_UNKNOWN and REFUSED_STALE on overflow,
// RECORDED on neither

void rocketnet_ack_count_sent_result( enum rocketnet_ack_sent_result_t result, uint64_t * lost_counter, uint64_t * overflow_counter );

// opens one aggregate for a fragmented payload: fragment_count wire
// packets stand between the payload's packet id and its acknowledgment.
// returns the aggregate id the fragments' sent records carry. safe from
// fill-phase threads: reservation and publication share the endpoint lock.

uint64_t rocketnet_ack_endpoint_open_aggregate( struct rocketnet_ack_endpoint_t * endpoint,
                                                uint64_t packet_id, int fragment_count );

// Marks an already-recorded sent sequence as having been a keep-alive, so
// its acknowledgment latches keep_alive_acked. Called immediately after the
// keep-alive's record_sent; a sequence whose record has already been
// recycled is ignored, which only delays the latch to the next keep-alive.

void rocketnet_ack_endpoint_mark_keep_alive( struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence );

void rocketnet_ack_endpoint_record_received( struct rocketnet_ack_endpoint_t * endpoint,
                                             uint64_t sequence, int wire_bytes, double time );

// Applies one validated block to the delivery state. The walk is bounded by
// the retained window, never by the block's claims: everything the block
// names below the window is skipped arithmetically without being visited;
// within the window, a named sequence that was never sent counts on the
// unknown_sequence side counter and is otherwise ignored. Newly-acknowledged
// packets are marked, sampled for rtt (the first block that acknowledges a
// sequence yields the sample), and their packet ids appended to the batch.
// A packet id that cannot fit the batch is deferred, never dropped: the
// record stays unacknowledged, counted on batch_full, and a later block --
// every block restates the recent interval -- surfaces it into a drained
// batch. Returns the number of newly-acknowledged packets.

int rocketnet_ack_endpoint_process_block( struct rocketnet_ack_endpoint_t * endpoint,
                                          const struct rocketnet_ack_block_t * block,
                                          double time,
                                          uint64_t * acked_packet_ids, int acked_capacity, int * acked_count,
                                          uint64_t * unknown_sequence_counter,
                                          uint64_t * batch_full_counter );

// The per-connection statistics recompute, the reference's shape: rtt
// is the minimum over positive rtt history, jitter their mean deviation from it,
// packet loss the unacked fraction of the sent window's older half -- the
// newest half-window is excluded even during warmup; no eligible sends
// publishes zero. The window is sequence-based, without time expiry. The
// bandwidth numbers retain their original cohorts, including during warmup.

void rocketnet_ack_endpoint_update_stats( struct rocketnet_ack_endpoint_t * endpoint );

// Shared six-field result for direct worker-history calculation. The caller
// owns the history exclusively while computing; this function never mutates it.
struct rocketnet_ack_stats_result_t
{
    float rtt, jitter, packet_loss, sent_kbps, received_kbps, acked_kbps;
};

void rocketnet_ack_stats_history_compute( const rocketnet_ack_stats_history_t * history,
                                           rocketnet_ack_stats_result_t * result );


// -----------------------------------------
//                    the server's acks
// -----------------------------------------

// One endpoint per client slot, and the shared per-update batch of newly
// acknowledged packet ids with the client index of each -- drained by the
// game once per update. Storage is allocated once at create, to the
// deployment's own max_clients.

struct rocketnet_server_acks_t
{
    void * context;
    int max_clients;
    uint64_t update_count;                      // phases the per-slot stats recompute
    int num_acked;
    int acked_capacity;
    int * acked_client_index;
    uint64_t * acked_packet_ids;
    struct rocketnet_ack_endpoint_t * endpoints;
};

struct rocketnet_server_acks_t * rocketnet_server_acks_create( void * context, int max_clients );

void rocketnet_server_acks_destroy( struct rocketnet_server_acks_t * acks );

// resets one slot's endpoint when a client connects into it

void rocketnet_server_acks_reset_slot( struct rocketnet_server_acks_t * acks, int client_index );

#endif // #ifndef ROCKETNET_ACK_H
