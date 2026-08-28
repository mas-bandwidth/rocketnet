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

// Per-connection delivery state: a ring of sent session packets keyed by
// sequence, retaining at least one second of the transport's maximum send
// rate (the same sizing rule as the replay window), an rtt history filled
// by first-ack samples, and a received ring for the per-client bandwidth
// numbers. Fixed storage, spent up front.

struct rocketnet_sent_packet_record_t
{
    uint64_t sequence;
    uint64_t packet_id;
    double send_time;
    uint32_t wire_bytes;
    uint8_t acked;
    uint8_t has_packet_id;
    uint8_t pad[2];
};

struct rocketnet_received_packet_record_t
{
    uint64_t sequence;
    double receive_time;
    uint32_t wire_bytes;
    uint32_t pad;
};

struct rocketnet_ack_endpoint_t
{
    // highest_sequence folds under fill-phase threads with an atomic max;
    // ack_debt increments on the update thread alone and resets from fill
    // threads with an atomic store -- update and the fill phase never
    // overlap, per the transports' threading contract

    uint64_t highest_sequence;                  // highest session sequence recorded as sent
    uint64_t highest_received_sequence;         // highest session sequence recorded as received
    uint64_t ack_debt;                          // eliciting packets received since the last block sent

    // published per-connection statistics, recomputed by update_stats --
    // amortized round-robin across slots on the server, every update on the
    // client. rtt and jitter in milliseconds, loss as a percentage.
    float rtt;
    float jitter;
    float packet_loss;
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
    float bandwidth_acked_kbps;
    int has_rtt_sample;                         // any first-ack sample has landed

    struct rocketnet_sent_packet_record_t sent[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    struct rocketnet_received_packet_record_t received[ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    float rtt_history[ROCKETNET_RTT_HISTORY_SIZE];      // milliseconds; negative = empty
};

void rocketnet_ack_endpoint_reset( struct rocketnet_ack_endpoint_t * endpoint );

// records one sent session packet. packet_id is surfaced on ack only when
// has_packet_id is nonzero -- keep-alive, disconnect and standalone ack
// sequences have none and surface nothing. safe from fill-phase threads:
// distinct sequences land in distinct ring entries.

void rocketnet_ack_endpoint_record_sent( struct rocketnet_ack_endpoint_t * endpoint,
                                         uint64_t sequence, int wire_bytes,
                                         uint64_t packet_id, int has_packet_id, double time );

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

// The per-connection statistics recompute, the space reference's shape: rtt
// is the minimum over the rtt history, jitter the mean deviation from it,
// packet loss the unacked fraction of the sent window's older half -- the
// newer half is excluded because its acks are still in flight -- and the
// bandwidth numbers integrate the same windows.

void rocketnet_ack_endpoint_update_stats( struct rocketnet_ack_endpoint_t * endpoint );

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
