/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_FRAGMENT_H
#define ROCKETNET_FRAGMENT_H

#include <stdint.h>
#include <stddef.h>
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"

// Fragmentation and reassembly, per STANDARD.md _Fragmentation_: a payload
// larger than one packet's capacity is split into fragments, each riding an
// ordinary sealed session packet (type 9), and reassembled per connection on
// the far side. This is the transport's own self-contained mechanism: it
// consumes nothing from the ack notification system, keeps its own
// bookkeeping, and delivers bytes, once, or nothing -- a lost fragment loses
// the whole payload, and the counters make it visible.

// the fragment header, inside the seal after the ack block:
// [payload id u32] [fragment index u8] [fragment count - 1 u8]

#define ROCKETNET_FRAGMENT_HEADER_BYTES         6

#define ROCKETNET_MAX_FRAGMENTS                 256

// The sender's fragment capacity: budget - 26 - the minimum block - the
// fragment header. The stride is chosen against the minimum block, so a
// current block with ranges truncates there; the stride never shrinks to
// make room. The receive-side maximum is the same arithmetic against the
// fixed receive ceiling, and sizes each reassembly slot's staging buffer.

#define ROCKETNET_MAX_FRAGMENT_BYTES            ( ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES - ROCKETNET_FRAGMENT_HEADER_BYTES )            // 1193
#define ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES    ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES - ROCKETNET_FRAGMENT_HEADER_BYTES )        // 1387

// the largest payload the sender can fragment: 256 fragments at the full
// stride. a deployment's configured maximum payload must fit under it.

#define ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES  ( ROCKETNET_MAX_FRAGMENTS * ROCKETNET_MAX_FRAGMENT_BYTES )

void rocketnet_write_fragment_header( uint8_t * output, uint32_t payload_id, int fragment_index, int fragment_count );

void rocketnet_read_fragment_header( const uint8_t * data, uint32_t * payload_id, int * fragment_index, int * fragment_count );

// The split plan for a payload too large for one packet: as few fragments as
// the stride cap allows, balanced so the last fragment (the remainder) is
// never empty -- a payload one byte over a single packet's capacity splits
// into two near-halves rather than a full fragment and a runt of nothing.
// Every fragment but the last is exactly stride bytes; the last is
// payload_bytes - stride * (count - 1), between 1 and stride by
// construction. Returns 0 when the payload cannot be fragmented (too small
// to need it, or over the 256-fragment maximum).

int rocketnet_fragment_plan( int payload_bytes, int * out_fragment_count, int * out_fragment_stride );

// -----------------------------------------
//                              reassembly
// -----------------------------------------

// Reassembly is per connection, keyed by payload id in a fixed-size sequence
// buffer of slots -- the modulo-indexed structure this protocol already uses
// for replay state. Each slot owns capacity for the configured maximum
// payload plus one staging buffer at the maximum receive fragment size, for
// the one case that needs it: only the final fragment can arrive before the
// stride is known, so a final arriving first is staged, checked against the
// stride when the first non-final fragment fixes it, and placed then.
// Storage is allocated once, at create, and never grown.

struct rocketnet_reassembly_slot_t
{
    uint32_t payload_id;                            // 0 = empty
    int fragment_count;                             // pinned by the first fragment of the payload
    int fragment_stride;                            // 0 until the first non-final fragment arrives
    int final_bytes;                                // -1 until the final fragment arrives
    int final_staged;                               // the final waits in staging until the stride is known
    int num_received;
    uint8_t received[ROCKETNET_MAX_FRAGMENTS / 8];
};

// The counter indices the reassembly fires, so the one engine serves the
// server's counter family and the client's: the eight refusal reasons from
// the standard in their check order, the per-packet accepted outcome, and
// the payload-level side counters. batch_full is this implementation's own:
// a completed payload dropped because the update's delivery batch was full
// -- it stays zero in ordinary operation.

struct rocketnet_reassembly_counters_t
{
    int zero_payload_id;
    int stale;
    int count_invalid;
    int index_out_of_range;
    int count_mismatch;
    int size_mismatch;
    int payload_too_large;
    int duplicate_index;
    int accepted;
    int reassembled;
    int abandoned;
    int batch_full;
};

// One set serves one direction of reassembly: the client holds one
// connection, the server one per client slot. The delivered arena is the
// destination of the one copy fragmentation inherently costs -- a completed
// payload is copied out of its slot at the moment of completion, so the slot
// is immediately reusable while the delivered bytes stay valid until the
// next update. completed_payload and completed_bytes report the last
// process call's completion, so a caller delivers in arrival position.

struct rocketnet_reassembly_set_t
{
    void * context;
    int num_connections;
    int num_slots;
    int max_payload_bytes;
    size_t slot_arena_bytes;                        // max payload + the staging buffer
    struct rocketnet_reassembly_slot_t * slots;     // num_connections * num_slots
    uint8_t * arena;                                // num_connections * num_slots * slot_arena_bytes
    int delivered_capacity;                         // completed payloads deliverable per update
    int num_delivered;
    uint8_t * delivered;                            // delivered_capacity * max_payload_bytes
    uint8_t * completed_payload;                    // the last process call's completion, into delivered
    int completed_bytes;
};

struct rocketnet_reassembly_set_t * rocketnet_reassembly_set_create( void * context, int num_connections, int num_slots, int max_payload_bytes );

void rocketnet_reassembly_set_destroy( struct rocketnet_reassembly_set_t * set );

// resets one connection's slots -- on client connect into a slot, and on a
// fresh client-side attempt

void rocketnet_reassembly_set_reset_connection( struct rocketnet_reassembly_set_t * set, int connection_index );

// opens the next update's delivery window: delivered payloads from the last
// update are invalidated here

void rocketnet_reassembly_set_begin_update( struct rocketnet_reassembly_set_t * set );

// Processes one opened fragment's type data (the 6-byte header plus the
// fragment bytes) through the standard's checks in order, each refusal
// firing its named counter, and places accepted bytes in the slot's arena.
// When the fragment completes its payload, the payload is copied into the
// delivered arena, completed_payload/completed_bytes point at it, the slot
// clears, and reassembled fires. Returns 1 when the fragment was accepted,
// 0 when it was refused.

int rocketnet_reassembly_process_fragment( struct rocketnet_reassembly_set_t * set,
                                           int connection_index,
                                           const uint8_t * type_data,
                                           int type_data_bytes,
                                           uint64_t * counters,
                                           const struct rocketnet_reassembly_counters_t * table );

// -----------------------------------------
//                          the send arena
// -----------------------------------------

// The send-side staging for payloads requested above the wire's capacity at
// start_packet: a fixed number of maximum-payload slots, claimed with one
// atomic add from any fill thread, reset by the flush. Allocated once at
// create to the configured maximum, per the arena arithmetic in the
// standard's sending rules -- never grown, never shared between transports.

struct rocketnet_send_arena_t
{
    void * context;
    volatile uint64_t num_used;
    int num_slots;
    int slot_bytes;
    uint8_t * data;
};

struct rocketnet_send_arena_t * rocketnet_send_arena_create( void * context, int num_slots, int slot_bytes );

void rocketnet_send_arena_destroy( struct rocketnet_send_arena_t * arena );

// claims one slot, or NULL when the arena is exhausted for this fill phase

uint8_t * rocketnet_send_arena_acquire( struct rocketnet_send_arena_t * arena );

// nonzero when the pointer is a frame inside this arena -- how finish and
// abort route an arena-backed packet apart from a wire frame

int rocketnet_send_arena_owns( const struct rocketnet_send_arena_t * arena, const uint8_t * p );

// releases every slot; the flush calls this once per frame

void rocketnet_send_arena_reset( struct rocketnet_send_arena_t * arena );

#endif // #ifndef ROCKETNET_FRAGMENT_H
