/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CHANNEL_H
#define ROCKETNET_CHANNEL_H

#include "rocketnet.h"
#include "rocketnet_message.h"
#include "rocketnet_allocator.h"

// Layer 3, first half: the UNRELIABLE paths of the message layer above the
// transports (docs/CHANNELS.md 5, 7.2, 9.3, 11.2). It is pure, per R4: its
// state is one struct plus the region that struct points into (layer 1), the
// events are an application send, a frame to fill and a received payload, and
// time and the socket are nowhere in it. Only layer 8 binds it to a transport
// and to a clock.
//
// Three send shapes, exactly as the card's STOP says:
//
//   - Small messages are copied into the connection's send store at send time
//     (R5) and aggregated by the connection's send step into one RECORDS frame
//     until the wire budget (ROCKETNET_MAX_PAYLOAD_BYTES). This is
//     rn_channel_send + rn_channel_write.
//   - A packet-sized message takes the zero-copy SINGLE form: the caller
//     reserved the frame, the application writes at the returned payload root
//     (the transport's 16-aligned frame), and rn_channel_finish_message writes
//     only the trailing form byte. Nothing is copied and nothing is queued
//     (CHANNELS.md 11.2). This is rn_channel_start_message +
//     rn_channel_finish_message.
//   - A message above the wire frame and at or under max_message_bytes is one
//     RECORDS payload holding one record, above ROCKETNET_MAX_PAYLOAD_BYTES;
//     the transport fragments it, and a lost fragment loses the message, which
//     is what unreliable means (L2.3). rn_channel_write marks it fragmented.
//
// The receive path has no state: rn_channel_receive parses one payload the
// transport already opened and hands every UNRELIABLE message up in arrival
// order, each pointing into the payload (zero copy). Anything it cannot do is
// a named refusal or the layer's violation code; the object layer counts the
// refusal it returns, beside the two message counters the region keeps
// (MESSAGE_SEND_STORE_FULL, MESSAGE_SEND_QUEUE_FULL).

// One refusal per distinct reason, as R8 asks. RN_CHANNEL_OK is 0, every
// refusal is positive. QUEUE_FULL and STORE_FULL are the region's exhaustion
// and are already counted there; the others are this machine's answer, which
// layer 8 maps to the MESSAGE_* counters of CHANNELS.md section 12.

#define RN_CHANNEL_OK                                   0
#define RN_CHANNEL_REFUSED_ARGUMENT                     1   // bytes < 1
#define RN_CHANNEL_REFUSED_TOO_LARGE                    2   // bytes > max_message_bytes
#define RN_CHANNEL_REFUSED_CHANNEL                      3   // channel outside 0..num_channels-1
#define RN_CHANNEL_REFUSED_TYPE_NOT_ENABLED             4   // this card carries UNRELIABLE only
#define RN_CHANNEL_REFUSED_QUEUE_FULL                   5   // the send descriptor pool (counted, layer 1)
#define RN_CHANNEL_REFUSED_STORE_FULL                   6   // the send store (counted, layer 1)
#define RN_CHANNEL_REFUSED_CONTEXT_OPEN                 7   // a zero-copy message is already open
#define RN_CHANNEL_REFUSED_SINGLE_TOO_LARGE             8   // request above one wire frame: use the copying path
#define RN_CHANNEL_REFUSED_DELIVERY_FULL                9   // unreliable_deliveries exhausted: the rest are dropped
#define RN_CHANNEL_VIOLATION                           10   // the peer broke the wire (a layer 2 violation code)

// The open zero-copy message: the frame the caller reserved and the room it
// asked for. finish seals the SINGLE form; abort drops it, writing nothing.

struct rn_channel_message_context_t
{
    uint8_t * payload;
    int capacity;
    int request_bytes;
    int active;
    int refusal;                        // the last RN_CHANNEL_* refusal, for the object to count
};

// One connection's UNRELIABLE machine. `region` owns the send descriptors and
// the send store; `limits` is layer 2's view of this end's configuration.
// The queue is a FIFO through the descriptors' next field, in send order.

struct rn_channel_t
{
    rn_region_t * region;
    struct rocketnet_message_limits_t limits;

    int max_message_bytes;
    int unreliable_deliveries;

    uint32_t queue_head;                // rn_send_descriptor_t index, or RN_DESCRIPTOR_NONE
    uint32_t queue_tail;
    uint32_t queued;

    struct rn_channel_message_context_t context;
};

// config NULL means the defaults. The region must already be carved.

void rn_channel_init( struct rn_channel_t * channel, rn_region_t * region, const rocketnet_message_config_t * config );

// A new connection on this machine: the queued sends are freed, allocating
// nothing. The region is carved by its owner before or after, as it likes.

void rn_channel_reset( struct rn_channel_t * channel );

// The application's copying send (CHANNELS.md 9.3): the message is copied into
// the region's send store and queued for the connection's send step. Only
// ROCKETNET_UNRELIABLE is enabled in this card; the channel is checked against
// the runtime count and is not on the wire for code 0 (RakNet ignores it),
// priority rides the descriptor for the scheduler of layer 6. Returns
// RN_CHANNEL_OK, or the refusal.

int rn_channel_send( struct rn_channel_t * channel, const uint8_t * data, int bytes,
                     int reliability, int priority, int channel_index );

// The caller's frame size for the next send step: 0 when nothing is queued,
// one wire frame for an aggregation, or the one-record payload above a frame
// for a large message. The caller asks the transport for that many bytes and
// hands the payload root to rn_channel_write.

int rn_channel_next_bytes( const struct rn_channel_t * channel );

// The connection's send step. Writes a payload into the caller's frame and
// returns its length, or 0 when nothing is queued or the frame is too small.
// `fragmented` is set nonzero exactly when the payload is above one wire frame
// and the transport will fragment it (L2.3).

int rn_channel_write( struct rn_channel_t * channel, uint8_t * payload, int capacity, int * fragmented );

// The zero-copy packet-sized send (CHANNELS.md 11.2). The caller has reserved
// a frame of `capacity` payload bytes; start reserves `request_bytes` at the
// payload root and returns it, or NULL refused, with the reason in
// context->refusal (the context is left inactive). finish seals the SINGLE
// form after message_bytes (1 to the reservation) and returns the payload
// length, or 0 refused. Neither copies the message.

uint8_t * rn_channel_start_message( struct rn_channel_t * channel, uint8_t * payload, int capacity,
                                    int request_bytes, struct rn_channel_message_context_t * context );
int rn_channel_finish_message( struct rn_channel_t * channel, struct rn_channel_message_context_t * context, int message_bytes );
void rn_channel_abort_message( struct rn_channel_t * channel, struct rn_channel_message_context_t * context );

// The receive step, stateless (R4): parses one payload and appends every
// UNRELIABLE message to out, in arrival order, each data pointer into the
// payload. Returns RN_CHANNEL_OK, RN_CHANNEL_REFUSED_DELIVERY_FULL when
// unreliable_deliveries or out_capacity filled (the rest are dropped), or
// RN_CHANNEL_VIOLATION when layer 2 refused the payload whole.

int rn_channel_receive( const struct rn_channel_t * channel, const uint8_t * payload, int bytes,
                        rn_delivery_t * out, int out_capacity, int * out_count );

#endif // #ifndef ROCKETNET_CHANNEL_H
