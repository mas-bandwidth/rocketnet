/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CHANNEL_H
#define ROCKETNET_CHANNEL_H

#include <stdint.h>

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_window_ack.h"

// Layer 4 of the message layer: the per-channel state and the dispatch
// (docs/CHANNELS.md 7). tla/RNChannel.tla is the spec; each function below
// names the action it implements (owner rule R7).
//
// The state lives in the connection's region (layer 1): C rn_channel_send_t
// and C rn_channel_receive_t carved by rn_region_carve, so nothing here
// allocates (R2). RakNet's four counters and their coupling are present from
// the first version and run even though no sequenced type is enabled
// (CHANNELS.md 7.1, RNChannel's OrderedTraceUnchangedBySequenced).
//
// A record's dispatch is a switch over all eight types. Codes 0 and 3 are
// enabled; every other type is refused on send
// (ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED) and counted as a violation
// on receive (ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED), behind the enabled
// mask of 7.5. Each later type's case lives in its own file so its card runs
// without touching this one:
//
//   sdk/rocketnet_channel_reliable.cpp    code 2
//   sdk/rocketnet_channel_sequenced.cpp   codes 1 and 4
//   sdk/rocketnet_channel_receipts.cpp    codes 5, 6 and 7

// the eight application types (docs/CHANNELS.md 7.5, 17.1): the five wire
// codes, then the three receipt variants, which travel as 0, 2 and 3.

#define ROCKETNET_CHANNEL_TYPE_UNRELIABLE                       0
#define ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED             1
#define ROCKETNET_CHANNEL_TYPE_RELIABLE                         2
#define ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED                 3
#define ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED               4
#define ROCKETNET_CHANNEL_TYPE_UNRELIABLE_WITH_RECEIPT          5
#define ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT            6
#define ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT    7
#define ROCKETNET_CHANNEL_NUM_TYPES                             8

// the first version's mask: UNRELIABLE and RELIABLE_ORDERED (7.5)

#define ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION                 ( ( 1u << ROCKETNET_CHANNEL_TYPE_UNRELIABLE ) | ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED ) )

// a receipt type's base type: 5 travels as code 0, 6 as code 2, 7 as code 3

#define ROCKETNET_CHANNEL_RECEIPT_BASE( type )                  ( (type) < 5 ? (type) : (type) - 5 )

// what a dispatch did with one record (docs/CHANNELS.md 7.2)

#define ROCKETNET_CHANNEL_DISPATCH_NONE                         0   // nothing: refused, counted, or held by a later card
#define ROCKETNET_CHANNEL_DISPATCH_DELIVERED                    1   // a view into the batch, or a delivery from the ring
#define ROCKETNET_CHANNEL_DISPATCH_HELD                         2   // kept ahead of its turn, live (7.2, 7.3)
#define ROCKETNET_CHANNEL_DISPATCH_STALE                        3   // dropped, counted (7.3)
#define ROCKETNET_CHANNEL_DISPATCH_CONTROL                      4   // a control record: the endpoint's, never delivered

// a send's refusal, or 0 when the type may be sent (7.5)

#define ROCKETNET_CHANNEL_SEND_OK                               0
#define ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED                 1
#define ROCKETNET_CHANNEL_SEND_BAD_ARGUMENT                     2

struct rn_channels_t
{
    rn_region_t * region;             // the connection's carved region: the per-channel state lives here
    uint32_t num_channels;            // C, the runtime count, at most ROCKETNET_MAX_CHANNELS
    uint32_t enabled_types;           // bit t set: type t may be sent (7.5)
    uint32_t enabled_codes;           // the wire codes those types travel as (rocketnet_message_enabled_codes)
    uint32_t unreliable_sent;         // unreliable records written this update (RNChannel WritePacket, unrel)
    uint32_t unreliable_delivered;    // unreliable records delivered this update (RNChannel OpenPacket, uCount)
    uint32_t deliveries;              // deliveries written this update
    uint32_t delivery_capacity;       // W + Du, the region's delivery table
    uint32_t violations;              // records refused on receive this update
};

// Binds the dispatch to one connection's region. Allocates nothing: the
// per-channel state is already carved. enabled_types 0 means the first
// version's mask (7.5). num_channels outside 1..region->num_channels is
// refused and the struct is left zeroed, so it dispatches nothing.

bool rn_channels_init( struct rn_channels_t * channels, struct rn_region_t * region, uint32_t num_channels, uint32_t enabled_types );

// The wire codes this connection's enabled types travel as: the mask layer 2's
// reader checks against (rocketnet_message_limits_t::enabled_codes).

uint32_t rn_channels_enabled_codes( const struct rn_channels_t * channels );

// ---------------------------------------------------------------
//                        the send side, 7.1
// ---------------------------------------------------------------

// The refusal a send takes before anything else (7.5): 0 when the type is
// enabled, else ROCKETNET_CHANNEL_SEND_TYPE_NOT_ENABLED, counted once on the
// owner's counters. A bad argument is refused and counted nowhere: it is the
// caller's bug, not a protocol event.

int rn_channels_send_check( struct rn_channels_t * channels, int type, uint32_t channel );

// RNChannel!Enqueue, the ordered half: ordering = ordered_write++, then
// sequenced_write = 0. The reset runs although no sequenced type is enabled,
// so enabling one changes no ordered trace (7.1).

uint32_t rn_channels_send_ordered( struct rn_channels_t * channels, uint32_t channel );

// RNChannel!Enqueue, the sequenced half: ordering = ordered_write,
// sequencing = sequenced_write++. Disabled in the first version; the sequenced
// card calls it.

uint32_t rn_channels_send_sequenced( struct rn_channels_t * channels, uint32_t channel, uint32_t * out_ordering );

// ---------------------------------------------------------------
//                     the receive side, 7.1 and 7.2
// ---------------------------------------------------------------

// RNChannel!DeliverMessage, the ordered half: ordered_read = ordering + 1,
// then sequenced_watermark = 0. One function performs both steps, on the one
// path, whether the message is delivered as it arrives or released from a
// hold (7.1).

void rn_channels_deliver_ordered( struct rn_channels_t * channels, uint32_t channel, uint32_t ordering );

// RNChannel!DeliverMessage, the sequenced half: only when
// ordering == ordered_read and sequencing >= sequenced_watermark; then
// sequenced_watermark = sequencing + 1. Returns false when the record is not
// deliverable in this epoch, and changes nothing.

bool rn_channels_deliver_sequenced( struct rn_channels_t * channels, uint32_t channel, uint32_t ordering, uint32_t sequencing );

// The dispatch of one record (7.2), a switch over all eight types. The record
// is one layer 2 parsed, so its data points into the transport's batch and is
// delivered as a view, never copied (R3, THE COPIES (b)). out is one slot of
// the region's delivery table, valid until the next update. Returns what the
// dispatch did: ROCKETNET_CHANNEL_DISPATCH_*.
//
// Code 0 (UNRELIABLE) is complete here: at most once per update, a view into
// the batch, never a reliable number (I3), never blocked by the window (I13).
// Code 3 (RELIABLE_ORDERED) reaches rn_channels_dispatch_ordered, which the
// ordered card fills; the other six reach their own file's function and are
// refused there while their type is disabled.

int rn_channels_dispatch( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// Code 3's dispatch body: holds, drain and chunks (7.2). The ordered card
// fills this; until then it delivers only a record whose ordering is its
// channel's turn, and holds nothing.

int rn_channels_dispatch_ordered( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// Code 2's case (sdk/rocketnet_channel_reliable.cpp).

int rn_channels_dispatch_reliable( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// Codes 1 and 4's case (sdk/rocketnet_channel_sequenced.cpp).

int rn_channels_dispatch_sequenced( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// Codes 5, 6 and 7's case (sdk/rocketnet_channel_receipts.cpp).

int rn_channels_dispatch_receipt( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// Counts one receive violation on the owner's counters, once per record:
// ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED for a code its mask does not
// admit (7.5), the given counter for anything else.

void rn_channels_count_violation( struct rn_channels_t * channels, int counter );

// ---------------------------------------------------------------
//                  the ack feed, the transport's only
// ---------------------------------------------------------------

// R1: a message is known delivered when a transport packet that carried it is
// acked. This is the one function that takes the transport's acked packet ids
// for a connection (rocketnet_client_transport_acked_packets,
// rocketnet_server_transport_acked_packets) and hands them to layer 3's
// rn_window_ack_on_acks. Nothing else marks a message acked; there is no
// message-level ack protocol and no second ack channel.
//
// RNChannel!ReceiveAck. The ids are the transport's own uint64 packet ids, the
// same ids the packet builder committed each packet's reliable numbers under
// (rn_packet_builder_commit), so a resend's ack matches the send that produced
// it. Returns the number of sent-packet entries matched.

uint32_t rn_channels_feed_acks( struct rn_window_ack_t * ack, const uint64_t * packet_ids, uint32_t count, double now );

// Ends the update: the per-update counts start over (RNChannel!EndUpdate).
// The delivery table's views die with the update, so the caller releases them
// before this.

void rn_channels_end_update( struct rn_channels_t * channels );

#endif // #ifndef ROCKETNET_CHANNEL_H
