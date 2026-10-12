/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_UNRELIABLE_H
#define ROCKETNET_UNRELIABLE_H

#include <stdint.h>
#include <stddef.h>

#include "rocketnet.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_allocator.h"

// The UNRELIABLE send and receive paths of the message layer (docs/CHANNELS.md
// sections 1 R1/R4/R5, 5, 6.3, 6.4, 9.3, 11.2). This is a pure module (R4):
// its state is one struct plus the connection's region (layer 1) it points
// into; it holds no socket, reads no clock and allocates nothing.
//
// Send (9.3): a bounded FIFO of unreliable messages copied into the
// connection's send store. A message is dropped and counted
// (ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL) when the store is full, exactly
// as R1 says for unreliable data, and a message longer than the record's u16
// length is dropped and counted (ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED)
// at the send call, so the FIFO never holds a message it can never send.
// rn_unreliable_fill aggregates the queued messages as code-0 records into one
// RECORDS payload with the layer 2 writer until the next one does not fit -- a
// record is never split across packets -- frees each message's chunk the moment
// it is written (9.3 step 3), and plans a block (a message longer than one
// large store chunk) alone in its own large payload (L2.3), copying the whole
// chain so the record never reads across a chunk boundary. A packet that fails
// finish_packet drops the unreliable records it carried and counts them (6.4,
// Failed send).
//
// The zero-copy form (11.2) touches no module state: the caller asks for a
// frame of capacity request_bytes + 1, rn_unreliable_single_begin hands back
// the 16-aligned payload root, and rn_unreliable_single_finish writes the form
// byte last.
//
// Receive (5, 6.3): given a parsed layer 2 payload (SINGLE or RECORDS) the
// module delivers each code-0 record as a pointer into the payload into the
// caller's delivered list. The list is the caller's Du entries
// (rocketnet_message_config_t.unreliable_deliveries). When Du is full the
// rest are dropped and counted
// (ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL), which unreliable
// semantics allow (6.3 step 4). Records of any other code are left for the
// layers that own them.
//
// An unreliable record takes no message number, no received bit, no receive
// descriptor and no ring span (5, 6.1); this module keeps no such state. It
// shares the send store of layer 1 and, once layer 6 mixes them, the packet.
// There is no window state here, so an ack for a packet that carried only
// unreliable records is ignored at O(1) by layer 3 (6.4).

// One queued unreliable message: a chunk handle into the region's send store
// (layer 1), its byte count, its ordering channel and priority. It carries no
// number and no descriptor.

struct rn_unreliable_message_t
{
    uint32_t chunk;                     // the send store chunk handle; RN_CHUNK_NONE when the slot is free
    uint32_t bytes;                     // 1 to max_message_bytes
    uint16_t channel;
    uint8_t priority;
    uint8_t in_use;
};

// The module's whole state. The FIFO's storage is the caller's -- a carve-out
// of the connection's region -- so the module itself allocates nothing.

struct rn_unreliable_t
{
    rn_region_t * region;               // layer 1; its store and its counters
    rn_unreliable_message_t * queue;    // capacity entries, the caller's
    uint32_t capacity;
    uint32_t head;                      // next to fill (oldest)
    uint32_t tail;                      // next to write (newest)
    uint32_t count;
};

// Binds the module to a region and to the caller's FIFO storage. No bytes are
// written; the caller seeds the entries to zero.

void rn_unreliable_init( rn_unreliable_t * u, rn_region_t * region, rn_unreliable_message_t * queue, uint32_t capacity );

// Copies one message into the region's send store and appends it to the FIFO.
// Returns 1, or 0 when the store had no chunk of the class it needs (or the
// FIFO is full), in which case nothing changes and
// ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL is counted once. A message longer
// than ROCKETNET_MESSAGE_MAX_RECORD_BYTES is refused and counted once as
// ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED, so the FIFO never wedges.
// R4/R1: no allocation outside the region.

int rn_unreliable_send( rn_unreliable_t * u, const uint8_t * data, int bytes, int channel, int priority );

// Writes one payload into the caller's buffer. Payload capacity counts every
// byte the payload may take, including the trailing form byte. When the head
// message is a single store chunk it aggregates as many queued messages as fit
// as code-0 records into one RECORDS payload, freeing each message's chunk as
// soon as it is written (9.3 step 3, rule R5); a record is never split. When
// the head message is a block (longer than one large store chunk) it is
// planned alone in its own large payload (L2.3), its chain copied whole.
// Returns the payload length L, or 0 when nothing was written. On return
// u->count and the FIFO reflect what was written.

int rn_unreliable_fill( rn_unreliable_t * u, uint8_t * payload, int capacity );

// The payload capacity a packet needs to carry the head message: the packet
// size for a single-chunk message, or the whole large payload (header, data
// and the trailing form byte) for a block (L2.3). This is what layer 6 asks
// before it calls start_packet, and it is the capacity to hand
// rn_unreliable_fill. Zero when nothing is queued.

int rn_unreliable_next_packet_bytes( const rn_unreliable_t * u );

// A packet that finished with finish_packet == 0 drops the unreliable records
// it carried; the caller passes how many records rn_unreliable_fill wrote and
// this counts each (6.4, Failed send). The chunks are already freed; only the
// count changes.

void rn_unreliable_packet_failed( rn_unreliable_t * u, int records );

// -----------------------------------------
//                              zero copy
// -----------------------------------------

// The message room in a payload of payload_capacity bytes (one byte is the
// form byte). Zero when there is no room.

int rn_unreliable_single_capacity( int payload_capacity );

// The zero-copy SINGLE form (11.2): the caller writes the message at the
// 16-aligned payload root, then calls this to write the form byte last and
// returns the payload length L (or 0 when the message is empty or does not
// leave room for the form byte). Touches no module state.

uint8_t * rn_unreliable_single_begin( uint8_t * frame_payload, int payload_capacity, int request_bytes );

int rn_unreliable_single_finish( uint8_t * payload, int payload_capacity, int message_bytes );

// -----------------------------------------
//                               receive
// -----------------------------------------

// Parses one layer 2 payload (SINGLE or RECORDS) and delivers each code-0
// record as a pointer into the payload into out[0..out_capacity). Records of
// any other code are left for the layers that own them. When the delivered
// list is full the rest are dropped and counted
// (ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL). Returns the layer 2
// violation (ROCKETNET_MESSAGE_VIOLATION_NONE on success) and leaves the
// delivered count in *out_count.

int rn_unreliable_receive( rn_unreliable_t * u, const uint8_t * payload, int bytes,
                           const struct rocketnet_message_limits_t * limits,
                           rn_delivery_t * out, int out_capacity, int * out_count );

#endif // #ifndef ROCKETNET_UNRELIABLE_H
