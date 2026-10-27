/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_ACK_H
#define ROCKETNET_SDK_ROCKETNET_ACK_H

#include <stdint.h>
#include "rocketnet_wire.h"
#include "rocketnet_constants.h"
#include "rocketnet_encryption_map.h"

// The general ack system, per STANDARD.md _Acks_: the transport tells a
// sender which of its packets arrived, and never resends anything. The
// sealed plaintext of every session packet begins with an ack block built
// from the receiver's ACK-eligible history; a sender that reads one marks its own
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

// Builds a block from the receiver's ACK-eligible history, independently of
// security replay protection, truncating oldest-first
// to the capacity: as many ranges as exist and fit, range count at most 255.
// capacity must be at least the 7-byte minimum, which always fits. returns
// the bytes written.

int rocketnet_ack_block_build( uint8_t * output, int capacity, const struct rocketnet_replay_protection_t * replay );

// Reconstruction of the block's largest received, per _Acks_: the wire
// sequence's rule exactly, with expected = the sender's own highest sent
// session sequence (not plus one).

uint64_t rocketnet_reconstruct_ack_largest( uint32_t wire_value, uint64_t highest_sent );

// Decodes and validates a block at the front of opened session data. The
// empty block is recognised on its wire fields at any highest sent: wire
// largest 0, first range length 0 and no ranges (tla/AckBlockWrap.tla
// ReadBlock); a zero first range length with anything else nonzero is
// malformed. Every other block is evaluated on the reconstructed 64-bit
// largest, by every rule in _Acks_/Validity: geometry contained in the opened
// size, first range length never exceeding largest, no zero gap or length
// entry, and the walk never falling below sequence one -- computed without
// unsigned underflow.
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
    uint8_t was_keep_alive;                 // a keep-alive's ack is the client's claim to hold its client index
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
    uint32_t stats_word;                   // one word of the fixed block-aggregate storage
};

static_assert( sizeof(rocketnet_received_packet_record_t) == 24,
               "received block metadata reuses the former link storage" );

// The sent ring's shadow bitmaps: one bit per slot, held (written) and
// unacknowledged. The walk reads them instead of visiting every sequence in
// the named run, so acknowledged and never-written sequences cost nothing.

#define ROCKETNET_SENT_BITMAP_WORDS ( ROCKETNET_SENT_PACKET_BUFFER_SIZE / 64 )
#define ROCKETNET_RECEIVED_WRITTEN_WORDS ( ( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 63 ) / 64 )
#define ROCKETNET_AGGREGATES_WRITTEN_WORDS ( ( ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE + 63 ) / 64 )
static_assert( ROCKETNET_SENT_PACKET_BUFFER_SIZE % 64 == 0,
               "the sent ring must be a multiple of 64 for the bitmaps" );

#if ROCKETNET_DEV

// The fields the statistics definition reads, as a standalone history for the
// reference recompute (rocketnet_ack_stats_history_compute). The live ACK
// endpoint keeps its own running aggregates; nothing copies into this form.
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

// Only epoch metadata is initialized. Reset invalidates every old word in O(1)
// without clearing bitmaps or history. A word is initialized lazily on write.
// At the unreachable-in-practice 2^64 epoch boundary, reinitialize only tags
// before epoch 1 can recur; otherwise stale epoch-1 words would become live.
template <unsigned Words> struct rocketnet_stats_epoch_bits_t
{
    uint64_t epoch;
    uint64_t tags[Words];
    uint64_t bits[Words];
    void initialize() { epoch = 1; for (unsigned i = 0; i < Words; ++i) tags[i] = 0; }
    void invalidate() { if (epoch == UINT64_MAX) initialize(); else ++epoch; }
    uint64_t word(int i) const { return tags[i] == epoch ? bits[i] : 0; }
    bool test(int i) const { return (word(i / 64) & (1ull << (i % 64))) != 0; }
    void set(int i) {
        const int w = i / 64;
        if (tags[w] != epoch) { bits[w] = 0; tags[w] = epoch; }
        bits[w] |= 1ull << (i % 64);
    }
};

struct rocketnet_ack_stats_validity_t
{
    rocketnet_stats_epoch_bits_t<ROCKETNET_SENT_BITMAP_WORDS> sent;
    rocketnet_stats_epoch_bits_t<ROCKETNET_RECEIVED_STATS_DIRTY_WORDS> received;
    rocketnet_stats_epoch_bits_t<ROCKETNET_RTT_STATS_DIRTY_WORDS> rtt;
    void initialize() { sent.initialize(); received.initialize(); rtt.initialize(); }
    void invalidate() { sent.invalidate(); received.invalidate(); rtt.invalidate(); }
};

#endif // #if ROCKETNET_DEV


struct rocketnet_ack_endpoint_t
{
    uint64_t sent_lock;                         // guards sent-record reservation and publication
    // highest_sequence folds under fill-phase threads with an atomic max;
    // ack_debt increments on the update thread alone and resets from fill
    // threads with an atomic store -- update and the fill phase never
    // overlap, per the transports' threading contract

    uint64_t highest_sequence;                  // highest session sequence recorded as sent
    uint64_t largest_acked_sequence;            // highest sent sequence the receiver has acknowledged; update thread only
    uint64_t highest_received_sequence;         // highest session sequence recorded as received
    uint64_t ack_debt;                          // eliciting packets received since the last block sent

    // published per-connection statistics. The running aggregates below keep
    // them current as each send, receive and ack lands. Every field is
    // exactly the number update_stats recomputes from the full histories,
    // except jitter, which matches it within floating-point rounding: the
    // running form subtracts count * min from a double sum, the recompute
    // sums sample - min in float. A server slot's stats are current at every
    // read and cost nothing per idle slot. The client still recomputes on
    // each stats read. rtt and jitter in milliseconds, loss as a percentage.
    float rtt;
    float jitter;
    float packet_loss;
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
    float bandwidth_acked_kbps;
    int has_rtt_sample;                         // any first-ack sample has landed

    // Latched by process_block the first time a sequence recorded as a
    // keep-alive is newly acknowledged. This is the client's CLAIM to have
    // received a keep-alive -- and so to hold its client index and be
    // connected, since a keep-alive carries the index and processing one is
    // the client's only transition to _connected_. It is a claim and not a
    // proof: an ack block is the client's own statement about its own receive
    // state, and no receiver can verify it. _Acks_ requires a block to name
    // only sequences its sender opened; a client that violates that latches
    // this bit without its index and strands only itself. It is what
    // _Keep-alive generation_ flips the suppression regime on; see
    // rocketnet_server_protocol_write_keep_alive_if_due. Cleared by reset,
    // so a reused slot starts with no claim recorded.

    uint8_t keep_alive_acked;

    // The running aggregates (sdk/rocketnet_ack.cpp, tla/RNServerStats.tla).
    // Every counted window is a function of its high-water mark, the same
    // one update_stats walks, so nothing here stores an edge: the sent
    // window is [base, top] with base = max(1, highest - N + 1) and top =
    // min(highest, base + N/2 - 1); its loss-eligible part ends at
    // highest - N/2. Send times rise with sequence (update refuses a time
    // that goes backwards), so the sent spans are read at the lowest and
    // highest counted sequences. Receive times do not follow sequence
    // (reordering, late and duplicate arrivals), so the counted received
    // records are summarized by fixed 32-slot physical blocks. Their compact
    // summaries occupy the stats_word padding reclaimed from these records,
    // so the endpoint memory budget does not grow. Exact sequence tags reject
    // stale aliases before a summary or edge record is counted.
    // All zero is the empty state.

    uint64_t stats_sent_bytes;                  // counted sent records' wire bytes
    uint64_t stats_acked_bytes;                 // of those, acknowledged
    uint64_t stats_sent_low;                    // lowest and highest counted sequence; 0 = none
    uint64_t stats_sent_high;
    uint64_t stats_acked_low;                   // lowest and highest acknowledged counted sequence; 0 = none
    uint64_t stats_acked_high;
    double stats_sent_low_time;                 // send times at those sequences, kept here so
    double stats_sent_high_time;                // publishing reads no cold record
    double stats_acked_low_time;
    double stats_acked_high_time;
    uint32_t stats_sent_count;                  // counted sent records
    uint32_t stats_acked_count;                 // of those, acknowledged
    uint32_t stats_eligible_count;              // counted records in the loss-eligible part
    uint32_t stats_eligible_unacked;            // of those, unacknowledged
    uint64_t stats_received_bytes;
    double stats_received_low_time;             // receive-time extrema reduced from the blocks
    double stats_received_high_time;
    uint32_t stats_received_count;
#if ROCKETNET_DEV
    uint32_t stats_received_operations;         // records/blocks visited by the latest receive; test-only
#else
    uint32_t stats_received_pad;
#endif
    double stats_rtt_sum;                       // sum and count of the positive rtt samples
    uint32_t stats_rtt_count;
    uint32_t stats_pad;

    volatile uint64_t num_aggregates;           // monotonic; the next aggregate's id is this plus one

    // bit (slot) set: the record at slot was written since the reset. The three
    // record rings are never zeroed (271 KB a connection): a record is read
    // only when its written bit is set, and an unwritten one reads as the
    // empty record (sequence 0, id 0), so the reset clears only these bits
    uint64_t sent_written[ROCKETNET_SENT_BITMAP_WORDS];
    uint64_t received_written[ROCKETNET_RECEIVED_WRITTEN_WORDS];
    uint64_t aggregates_written[ROCKETNET_AGGREGATES_WRITTEN_WORDS];

    uint64_t sent_held[ROCKETNET_SENT_BITMAP_WORDS];     // bit (slot) set: the record at slot was written; fill under sent_lock
    uint64_t sent_unacked[ROCKETNET_SENT_BITMAP_WORDS];  // bit (slot) set: written and not yet acknowledged
    uint64_t sent_bits_checked;                          // update thread only: highest sequence whose slot's bits were reconciled

    struct rocketnet_sent_packet_record_t sent[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    struct rocketnet_received_packet_record_t received[ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    struct rocketnet_packet_aggregate_t aggregates[ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
    float rtt_history[ROCKETNET_RTT_HISTORY_SIZE];      // milliseconds; negative = empty

    // the minimum positive rtt sample: a min tree over rtt_history, node k
    // the minimum of nodes 2k and 2k+1, a node at or past the history size
    // the history entry it names (an empty or non-positive entry reads as
    // ROCKETNET_ACK_RTT_NONE); node 1 is the minimum. A sample is O(log n).
    float stats_rtt_min_tree[ROCKETNET_RTT_HISTORY_SIZE];

};

static_assert( ( ROCKETNET_RTT_HISTORY_SIZE & ( ROCKETNET_RTT_HISTORY_SIZE - 1 ) ) == 0,
               "the rtt min tree needs a power-of-two history" );
static_assert( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE % 32 == 0,
               "the received block aggregate needs 32-slot blocks" );

void rocketnet_ack_endpoint_reset( struct rocketnet_ack_endpoint_t * endpoint );

// true when two endpoints hold the same state: every field before the record
// rings, the rtt history, and each ring's records where both wrote them (an
// unwritten record is the empty record, whatever its bytes)
bool rocketnet_ack_endpoint_state_equal( const struct rocketnet_ack_endpoint_t * a, const struct rocketnet_ack_endpoint_t * b );

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
//   EVICTED_LOST     the evicted record was never acknowledged and the receiver
//                    has acknowledged a sequence at least
//                    ROCKETNET_LOSS_PACKET_THRESHOLD above it (RFC 9002
//                    6.1.1): the packet is known lost on the path. An outcome,
//                    counted on SEND_HISTORY_LOST.
//   EVICTED_UNKNOWN  the evicted record was never acknowledged and no such
//                    later acknowledgment exists: its fate is unknown (a
//                    silent receiver, or a window too small for the path). A
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

// Internal unit-test access to the production popcount and its portable
// implementation. The latter is also the fallback used when no builtin is
// available, so the suite can exercise it on every host.
int rocketnet_ack_popcount64( uint64_t value );
int rocketnet_ack_popcount64_portable( uint64_t value );

#if ROCKETNET_DEV

// The six-field result of the reference recompute over a standalone history.
// The function never mutates the history.
struct rocketnet_ack_stats_result_t
{
    float rtt, jitter, packet_loss, sent_kbps, received_kbps, acked_kbps;
};

void rocketnet_ack_stats_history_compute( const rocketnet_ack_stats_history_t * history,
                                        rocketnet_ack_stats_result_t * result,
                                        const uint64_t * sent_written = NULL,
                                        const uint64_t * received_written = NULL,
                                        const rocketnet_ack_stats_validity_t * validity = NULL );

#endif // #if ROCKETNET_DEV

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

#endif // ROCKETNET_SDK_ROCKETNET_ACK_H
