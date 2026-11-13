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

// The general ack system, per standard/TRANSPORT.md t15: the transport tells a
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
// largest 0, first range length 0 and no ranges (proofs/tla/AckBlockWrap.tla
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
    double send_time;
    uint32_t wire_bytes;
    uint8_t acked;
    uint8_t has_packet_id;
    uint8_t was_keep_alive;                 // a keep-alive's ack is the client's claim to hold its client index
    uint8_t pad[1];
};

struct rocketnet_received_packet_record_t
{
    uint64_t sequence;
    double receive_time;
    uint32_t wire_bytes;
    uint32_t stats_word;                   // one word of the received statistics storage
};

// An in-order reduction of received records: a generation's block summary,
// a suffix of the older half's first generation, or a cached piece of the
// older half. first_offset is the lowest counted offset inside a block
// summary, where records join in any order.

struct rocketnet_ack_received_aggregate_t
{
    uint64_t bytes;
    double low_time;
    double high_time;
    uint32_t count;
    uint8_t first_is_nan;
    uint8_t first_offset;
    uint8_t pad[2];
};

static_assert( sizeof( rocketnet_ack_received_aggregate_t ) == 32, "received aggregate size" );

static_assert( sizeof( rocketnet_received_packet_record_t ) == 24,
               "received block metadata reuses the former link storage" );

// The sent ring's shadow bitmaps: one bit per slot, held (written) and
// unacknowledged. The walk reads them instead of visiting every sequence in
// the named run, so acknowledged and never-written sequences cost nothing.

#define ROCKETNET_SENT_BITMAP_WORDS ( ROCKETNET_SENT_PACKET_BUFFER_SIZE / 64 )

// the most extra ranges a block may carry and still be remembered as the
// last applied (block_memo); a block with more is always applied in full
#define ROCKETNET_ACK_BLOCK_MEMO_RANGES 4

// the last block applied in full (rocketnet_ack_endpoint_t block_memo): the
// block's fields, the high-water mark it was applied at, and the unknown
// count it added
struct rocketnet_ack_block_memo_t
{
    uint64_t largest;
    uint64_t highest_sequence;
    uint64_t unknown;
    uint32_t first_range_length;
    uint8_t valid;
    uint8_t num_ranges;
    uint16_t range_gap[ROCKETNET_ACK_BLOCK_MEMO_RANGES];
    uint16_t range_length[ROCKETNET_ACK_BLOCK_MEMO_RANGES];
};
#define ROCKETNET_RECEIVED_WRITTEN_WORDS ( ( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 63 ) / 64 )
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

static_assert( sizeof( rocketnet_ack_stats_sent_t ) == 24, "stats sent projection size" );
static_assert( sizeof( rocketnet_ack_stats_received_t ) == 24, "stats received projection size" );

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

    void initialize()
    {
        epoch = 1;

        for ( unsigned i = 0; i < Words; ++i )
        {
            tags[i] = 0;
        }
    }

    void invalidate()
    {
        if ( epoch == UINT64_MAX )
        {
            initialize();
        }
        else
        {
            ++epoch;
        }
    }

    uint64_t word( int i ) const { return tags[i] == epoch ? bits[i] : 0; }
    bool test( int i ) const { return ( word( i / 64 ) & ( 1ull << ( i % 64 ) ) ) != 0; }

    void set( int i )
    {
        const int w = i / 64;
        if ( tags[w] != epoch )
        {
            bits[w] = 0;
            tags[w] = epoch;
        }

        bits[w] |= 1ull << ( i % 64 );
    }
};

struct rocketnet_ack_stats_validity_t
{
    rocketnet_stats_epoch_bits_t<ROCKETNET_SENT_BITMAP_WORDS> sent;
    rocketnet_stats_epoch_bits_t<ROCKETNET_RECEIVED_STATS_DIRTY_WORDS> received;
    rocketnet_stats_epoch_bits_t<ROCKETNET_RTT_STATS_DIRTY_WORDS> rtt;

    void initialize()
    {
        sent.initialize();

        received.initialize();

        rtt.initialize();
    }

    void invalidate()
    {
        sent.invalidate();

        received.invalidate();

        rtt.invalidate();
    }
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

    // The running aggregates (sdk/rocketnet_ack.cpp, proofs/tla/RNServerStats.tla).
    // Every counted window is a function of its high-water mark, the same
    // one update_stats walks, so nothing here stores an edge: the sent
    // window is [base, top] with base = max(1, highest - N + 1) and top =
    // min(highest, base + N/2 - 1); its loss-eligible part ends at
    // highest - N/2. Send times rise with sequence (update refuses a time
    // that goes backwards), so the sent spans are read at the lowest and
    // highest counted sequences. Receive times do not follow sequence
    // (reordering, late and duplicate arrivals), so the counted received
    // records are summarized per 32-sequence generation, each in its fixed
    // physical block and tagged with its generation. The summaries and the
    // older half's cached pieces occupy the stats_word padding reclaimed from
    // these records, so the endpoint does not grow. Exact sequence tags
    // reject stale aliases before a record is counted
    // (sdk/rocketnet_ack.cpp rocketnet_ack_stats_received_refresh).
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
#endif // #if ROCKETNET_DEV
    double stats_rtt_sum;                       // sum and count of the positive rtt samples
    uint32_t stats_rtt_count;
    uint32_t internal_error;                    // sticky: the sent-stats low scan found nothing in [base, old_top]

    // bit (slot) set: the record at slot was written since the reset. The two
    // record rings are never zeroed (176 KiB a connection at the default
    // history): a record is read only when its written bit is set, and an
    // unwritten one reads as the empty record (sequence 0, id 0), so the reset
    // clears only these bits
    uint64_t sent_written[ROCKETNET_SENT_BITMAP_WORDS];
    uint64_t received_written[ROCKETNET_RECEIVED_WRITTEN_WORDS];

    uint64_t sent_held[ROCKETNET_SENT_BITMAP_WORDS];     // bit (slot) set: the record at slot was written; fill under sent_lock
    uint64_t sent_unacked[ROCKETNET_SENT_BITMAP_WORDS];  // bit (slot) set: written and not yet acknowledged
    uint64_t sent_bits_checked;                          // update thread only: highest sequence whose slot's bits were reconciled

    // update thread only: the last block process_block applied in full, when
    // that apply deferred nothing (batch full) and nothing has been sent
    // since. Applying it again would visit no record (every unacknowledged
    // slot it names was cleared) and add only the same unknown count, so
    // process_block adds that count and skips the walk
    // (proofs/tla/SessionAcks.tla RecvAckRepeat). Beside sent_bits_checked,
    // whose line every block reads already. A cache, not state: state_equal
    // leaves it out; the reset and every recorded send clear valid.
    struct rocketnet_ack_block_memo_t block_memo;

    struct rocketnet_sent_packet_record_t sent[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    struct rocketnet_received_packet_record_t received[ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    float rtt_history[ROCKETNET_RTT_HISTORY_SIZE];      // milliseconds; negative = empty

    // the minimum positive rtt sample: a min tree over rtt_history, node k
    // the minimum of nodes 2k and 2k+1, a node at or past the history size
    // the history entry it names (an empty or non-positive entry reads as
    // ROCKETNET_ACK_RTT_NONE); node 1 is the minimum. A sample is O(log n).
    float stats_rtt_min_tree[ROCKETNET_RTT_HISTORY_SIZE];

};

#if ROCKETNET_HISTORY_PACKETS == 4096
static_assert( sizeof( ( (struct rocketnet_ack_endpoint_t *) 0 )->sent ) + sizeof( ( (struct rocketnet_ack_endpoint_t *) 0 )->received ) ==
                   (size_t) 176 * 1024,
               "the two record rings are 176 KiB at the default history" );
#endif // #if ROCKETNET_HISTORY_PACKETS == 4096

static_assert( ( ROCKETNET_RTT_HISTORY_SIZE & ( ROCKETNET_RTT_HISTORY_SIZE - 1 ) ) == 0,
               "the rtt min tree needs a power-of-two history" );
static_assert( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE % 32 == 0 && ( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2 ) % 32 == 0,
               "the received generations need 32-slot blocks" );

void rocketnet_ack_endpoint_reset( struct rocketnet_ack_endpoint_t * endpoint );

// true when two endpoints hold the same state: every field before the record
// rings, the rtt history, and each ring's records where both wrote them (an
// unwritten record is the empty record, whatever its bytes)
bool rocketnet_ack_endpoint_state_equal( const struct rocketnet_ack_endpoint_t * a, const struct rocketnet_ack_endpoint_t * b );

// records one sent session packet. packet_id is surfaced on ack only when
// has_packet_id is nonzero -- keep-alive, disconnect and standalone ack
// sequences have none and surface nothing. Concurrent aliases are serialized, with the
// newer sequence winning even if writers finish out of order. False reports
// history overflow (an unacknowledged record was evicted, or this writer fell
// behind the retained sequence). It is advisory: callers count it and still
// transmit. It never imposes a send-rate or ACK-rate requirement.
//
// record_sent_classified says what the write did to the record it replaced,
// the four outcomes of proofs/tla/RNSentHistory.tla:
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
                                                                                double time );

bool rocketnet_ack_endpoint_record_sent( struct rocketnet_ack_endpoint_t * endpoint,
                                         uint64_t sequence, int wire_bytes,
                                         uint64_t packet_id, int has_packet_id,
                                         double time );

// counts a record_sent_classified outcome on the two history counters:
// EVICTED_LOST on lost, EVICTED_UNKNOWN and REFUSED_STALE on overflow,
// RECORDED on neither

void rocketnet_ack_count_sent_result( enum rocketnet_ack_sent_result_t result, uint64_t * lost_counter, uint64_t * overflow_counter );

// Marks an already-recorded sent sequence as having been a keep-alive, so
// its acknowledgment latches keep_alive_acked. Called immediately after the
// keep-alive's record_sent; a sequence whose record has already been
// recycled is ignored, which only delays the latch to the next keep-alive.

void rocketnet_ack_endpoint_mark_keep_alive( struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence );

// Read hints for a packet about to be received on this endpoint, whose wire
// sequence (the low 32 bits) is sequence_low: the endpoint's scalar fields and
// the received record, block summary and cached pieces record_received will
// touch. No value is read for use and nothing is written.

void rocketnet_ack_endpoint_prefetch_receive( const struct rocketnet_ack_endpoint_t * endpoint, uint32_t sequence_low );

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
//
// A block equal to the last one applied in full, with nothing sent since and
// nothing deferred by that apply, only adds that apply's unknown count: the
// state it leaves is the state the full apply would (block_memo).

int rocketnet_ack_endpoint_process_block( struct rocketnet_ack_endpoint_t * endpoint,
                                          const struct rocketnet_ack_block_t * block,
                                          double time,
                                          uint64_t * acked_packet_ids, int acked_capacity, int * acked_count,
                                          uint64_t * unknown_sequence_counter,
                                          uint64_t * batch_full_counter );

// process_block without the block memo: always the full apply, and the memo
// neither read nor written. The reference the memo is tested against.

int rocketnet_ack_endpoint_process_block_full( struct rocketnet_ack_endpoint_t * endpoint,
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

// The portable popcount: the fallback used when no builtin is available, kept
// callable so the suite can exercise it on every host.
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

// The ack phase (SERVER-UPDATE-ACK.md P3; proofs/tla/RNAckPhase.tla). The
// receive loop does not apply a session packet's ack block: it defers it,
// appending an entry in arrival order and linking it onto its client's list.
// Each client's blocks are then applied by that client's ack call, in arrival
// order, into a segment of its own (any thread, one thread per client), and
// the gather, on main, copies every entry's ids into the public batch in
// arrival order and sums the per-client counts. The batch, the counters and
// every endpoint end as the serial apply leaves them:
//   - everything process_block writes is the client's own endpoint, its
//     segment and its counts; the batch never fills on the server (a client
//     surfaces at most ROCKETNET_SENT_PACKET_BUFFER_SIZE ids in an update,
//     the segment's size), so no apply defers;
//   - nothing between the receive loop and the ack phase sends to a client
//     with a deferred block (the only session send of the receive loop is
//     the first keep-alive of a slot assigned in it, and a slot freed in the
//     update is held until publish), and record_received touches the
//     received side only;
//   - a reset of a client inside the receive loop (its disconnect, the
//     integrity limit) applies that client's deferred blocks first
//     (rocketnet_server_acks_reset_slot), as the serial loop had.
// When the entries or the block storage run out, the update spills: every
// deferred block is applied and gathered there, on main, and the rest of the
// update's blocks are applied in the receive loop as before.

// one deferred block, in arrival order
struct rocketnet_server_ack_entry_t
{
    uint64_t connection_id;                     // the connection the block was received for (the batch's tag)
    int client_index;
    int next;                                   // the client's next entry, -1: none
    int block_offset;                           // the block in the store
    int acked_first;                            // its ids in the client's segment, written by the apply
    int acked_count;
    int padding;
};

// one client's ack-phase state: written by the receive loop (main), then by
// the client's ack call alone, then read by the gather (main); a line of its
// own, so ack calls of neighbouring clients never share one
#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4324 )
#endif // #if defined( _MSC_VER )
struct alignas( 64 ) rocketnet_server_ack_client_t
{
    int head;                                   // the first entry not yet applied, -1: none
    int tail;                                   // the last entry deferred, -1: none
    int segment_count;                          // ids in the client's segment
    int listed;                                 // 1: in pending_clients this update
    uint64_t connection_id;                     // the tag of the client's entries this update
    uint64_t unknown_sequence;                  // this update's ACK_UNKNOWN_SEQUENCE
    uint64_t batch_full;                        // this update's ACKED_BATCH_FULL
};
#if defined( _MSC_VER )
#pragma warning( pop )
#endif // #if defined( _MSC_VER )

#define ROCKETNET_SERVER_ACK_ENTRIES_PER_CLIENT         16
#define ROCKETNET_SERVER_ACK_STORE_BYTES_PER_CLIENT     512

// the deferring modes of an update
#define ROCKETNET_SERVER_ACKS_DIRECT            0       // blocks are applied in the receive loop (outside an update, or spilled)
#define ROCKETNET_SERVER_ACKS_DEFERRING         1       // blocks are deferred to the ack calls

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

    // the ack phase
    int mode;
    double time;                                // the update's time, which every deferred block applies at
    int num_entries;
    int entry_capacity;
    struct rocketnet_server_ack_entry_t * entries;
    int store_bytes;
    int store_capacity;
    uint8_t * store;                            // the deferred blocks, 8-aligned records
    int num_pending_clients;
    int * pending_clients;                      // the clients with an entry this update, in first-arrival order
    struct rocketnet_server_ack_client_t * clients;
    uint64_t * segments;                        // ROCKETNET_SENT_PACKET_BUFFER_SIZE ids a client
};

struct rocketnet_server_acks_t * rocketnet_server_acks_create( void * context, int max_clients );

void rocketnet_server_acks_destroy( struct rocketnet_server_acks_t * acks );

// resets one slot's endpoint when a client connects into it, or its
// connection ends; a client with deferred blocks has them applied first

void rocketnet_server_acks_reset_slot( struct rocketnet_server_acks_t * acks, int client_index );

// the update's start: deferring from here (main)

void rocketnet_server_acks_begin_update( struct rocketnet_server_acks_t * acks, double time );

// defers a client's block (main, the receive loop). false: no room, or not
// deferring: the caller spills (rocketnet_server_acks_gather) if deferring
// and applies the block itself

bool rocketnet_server_acks_defer( struct rocketnet_server_acks_t * acks, int client_index,
                                  const struct rocketnet_ack_block_t * block, uint64_t connection_id );

// the ack call's work: applies the client's deferred blocks in arrival order
// (any thread, one per client, between the receive loop and the gather)

void rocketnet_server_acks_apply_client( struct rocketnet_server_acks_t * acks, int client_index );

// the client's ids of this update so far, in arrival order, and the
// connection they were received for (0: none deferred); after its apply

int rocketnet_server_acks_client_acked( const struct rocketnet_server_acks_t * acks, int client_index,
                                        const uint64_t ** packet_ids, uint64_t * connection_id );

// main: applies every client not yet applied, copies each entry's ids into
// the batch in arrival order with its client and tag (connection_ids may be
// NULL), adds the per-client counts to the counters, and stops deferring.
// Nothing when not deferring.

void rocketnet_server_acks_gather( struct rocketnet_server_acks_t * acks, uint64_t * connection_ids,
                                   uint64_t * unknown_sequence_counter, uint64_t * batch_full_counter );

#if ROCKETNET_DEV
// the test seams (tests/server_ack_phase_cases.cpp), read at each update's
// start and each defer, on the updating thread: direct 1 leaves the update
// applying each block in the receive loop, the serial reference the ack
// phase is compared with; entry_capacity above 0 and below the entries an
// update defers before it spills
extern int rocketnet_server_acks_test_direct;
extern int rocketnet_server_acks_test_entry_capacity;
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_ACK_H
