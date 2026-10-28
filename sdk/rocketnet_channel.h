/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CHANNEL_H
#define ROCKETNET_SDK_ROCKETNET_CHANNEL_H

#include <stdint.h>

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_packet_builder.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_window_ack.h"
#include "rocketnet_window_receive.h"
#include "rocketnet_window_send.h"

// Layer 4 of the message layer: the per-channel state and the dispatch
// (docs/CHANNELS.md 7). tla/RNChannel.tla is the spec; each function below
// names the action it implements (owner rule R7).
//
// The state lives in the connection's region (layer 1): C rn_channel_send_t
// and C rn_channel_receive_t carved by rn_region_carve, so nothing here
// allocates (R2). The four counters and their coupling are present from
// the first version and run even though no sequenced type is enabled
// (CHANNELS.md 7.1, RNChannel's OrderedTraceUnchangedBySequenced).
//
// A record's dispatch is a switch over all eight types. The enabled mask of
// 7.5 admits a type; a disabled type is refused on send
// (ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED) and counted as a violation
// on receive (ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED). Each later type's
// case lives in its own file:
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

// the enabled application types: all eight, from the first version (7.5)

#define ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION                 ( ( 1u << ROCKETNET_CHANNEL_TYPE_UNRELIABLE ) | ( 1u << ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED ) | ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE ) | ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED ) | ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED ) | ( 1u << ROCKETNET_CHANNEL_TYPE_UNRELIABLE_WITH_RECEIPT ) | ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT ) | ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT ) )

// a receipt type's base type: 5 travels as code 0, 6 as code 2, 7 as code 3

#define ROCKETNET_CHANNEL_RECEIPT_BASE( type )                  ( (type) < 5 ? (type) : ( (type) == 5 ? 0 : (type) - 4 ) )

// what a dispatch did with one record (docs/CHANNELS.md 7.2)

// The receive deferred its whole payload: a reliable record could not reserve
// its deliveries, so the payload ends part-way and its acknowledgement is
// withheld for retry (standard/MESSAGES.md m1.4 rule 2). Not a violation.

#define ROCKETNET_CHANNEL_RECEIVE_DEFERRED                    -1

#define ROCKETNET_CHANNEL_DISPATCH_NONE                         0   // nothing: refused or counted
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
    uint32_t payload_capacity;        // the payload bytes a frame holds: a message is small when its record fits one (8)
    int error;                        // sticky: ROCKETNET_DISCONNECT_REASON_NONE, or ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION after a wire violation (17.3 item 11)
    uint32_t pending;                 // the send descriptor between start and finish, or RN_DESCRIPTOR_NONE
    uint32_t pending_bytes;           // what the pending start reserved
    uint32_t pending_channel;
    uint32_t ready_head, ready_tail;  // cached ends of the flat ready sequence over channel_send, oldest turn first (9.1)
    uint32_t ready_ticket_next;       // next ready-sequence membership ticket, from 1; 0 is not a member
    uint32_t schedule_work;           // queue entries visited by compact and ticket rebase; one pass, not per record
};

// Binds the dispatch to one connection's region. Allocates nothing: the
// per-channel state is already carved. enabled_types 0 means the first
// version's mask (7.5). num_channels outside 1..region->num_channels is
// refused and the struct is left zeroed, so it dispatches nothing.

bool rn_channels_init( struct rn_channels_t * channels, struct rn_region_t * region, uint32_t num_channels, uint32_t enabled_types );

// The payload capacity a frame holds, 1 + the single-record room up to
// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES. A reliable ordered message whose record fits
// it is SMALL (packed, one record) and a larger one is a block of chunks (8).
// The default is ROCKETNET_MESSAGE_SINGLE_BYTES; the m2.2 one-record limits
// derive from it. Refused (false) below the room one chunk record needs.

bool rn_channels_set_payload_capacity( struct rn_channels_t * channels, uint32_t payload_capacity );

// The largest message that travels as one record: the capacity less the form
// byte and the longest code 3 header (flags, length, number, ring, ordering
// and a wide channel).

#define ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES      13

uint32_t rn_channels_small_limit( const struct rn_channels_t * channels );

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

// The same decision, counted nowhere: what rn_channels_send_check returns,
// for the room query (rn_endpoint_send_room), which changes no state.

int rn_channels_send_allowed( const struct rn_channels_t * channels, int type, uint32_t channel );

// RNChannel!Enqueue, the ordered half: ordering = ordered_write++, then
// sequenced_write = 0. The reset runs although no sequenced type is enabled,
// so enabling one changes no ordered trace (7.1).

uint32_t rn_channels_send_ordered( struct rn_channels_t * channels, uint32_t channel );

// RNChannel!Enqueue, the sequenced half: ordering = ordered_write,
// sequencing = sequenced_write++. Codes 1 and 4 share this transition.

uint32_t rn_channels_send_sequenced( struct rn_channels_t * channels, uint32_t channel, uint32_t * out_ordering );

// ---------------------------------------------------------------
//       the send API, like the transport's start_packet and finish_packet
// ---------------------------------------------------------------

// RNChannel!Enqueue, in two steps (owner rule R3). start reserves room for a
// message of `bytes` bytes on `channel` in the connection's region and returns
// the pointer to write it at, once, in place: a store chunk for a message up
// to one block stride, a contiguous span above it. The pointer is where
// every packet write copies the bytes from (THE COPIES (a)), so the
// application writes the payload exactly once. finish(bytes) enqueues the
// message in its channel's FIFO order and gives it its ordering index
// (RNChannel!Enqueue's ordering assignment, 7.1); `bytes` may be less than
// start reserved, never more.
//
// RELIABLE (type 2, received on channel 0), RELIABLE_ORDERED (type 3), and
// RELIABLE_SEQUENCED (type 4) travel this path.
// NULL (and no
// message) for a type that is not enabled (counted MESSAGE_SEND_TYPE_NOT_ENABLED),
// a bad channel or size, a start already pending, a region already exhausted,
// or no room: no room is region exhaustion (R5, tla/RNConnectionMemory.tla
// Exhaust), counted beside MESSAGE_SEND_STORE_FULL or MESSAGE_SEND_QUEUE_FULL,
// and the owner's next rn_connection_memory_decide kicks the connection.

uint8_t * rn_channels_start( struct rn_channels_t * channels, uint32_t channel, int type, uint32_t bytes );

bool rn_channels_finish( struct rn_channels_t * channels, uint32_t bytes );

// Gives back a start that will not be finished: its room returns to the region
// and nothing is enqueued, no ordering index taken. False when no start is
// pending.

bool rn_channels_cancel( struct rn_channels_t * channels );

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
// Code 2 reaches rn_channels_dispatch_reliable; code 3 reaches
// rn_channels_dispatch_ordered. The other codes reach their own file's function.

int rn_channels_dispatch( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// Code 3's dispatch body (7.2 and 8). The record is one layer 3 accepted
// (rn_window_receive_accept): its number is live in the receiver's descriptors.
//
//   ordering == the channel's read index: delivered now, a view into the batch,
//     then the drain runs.
//   ordering ahead (by under W): copied into the receive ring at the offset
//     the sender chose, indexed by (channel, ordering) in the held index, and
//     kept live; returns HELD.
//   ordering behind: a violation (MESSAGE_ORDERING_BEHIND).
//   a block fragment (CHANNELS.md 8): copied to ring + index * F, counted in the
//     block's state, found through the held index; the whole block is one
//     message once every fragment is in, delivered in place from the ring, never
//     before. A count or index that disagrees with the first fragment seen, a
//     count above ceil(max_message_bytes / F), or a span outside the ring is a
//     wire violation.
//
// Every delivery, this record's or one released from a hold, is appended to
// the region's delivery table in order; `out` is a copy of the first one this
// call appended. A wire violation sets the channels' sticky error, counts it,
// and the owner disconnects the other side of the connection.
//
// RNChannel actions: OpenPacket (the arrival gate and the hold), DeliverMessage
// (the coupling, once per delivery).

int rn_channels_dispatch_ordered( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// The drain after an ordered delivery on `channel` (7.2): while the held index
// has the channel's next ordering index complete, deliver it from the ring.
// RNChannel!DeliverMessage, repeated.

void rn_channels_drain( struct rn_channels_t * channels, uint32_t channel );

// A block's chunk, for codes 2 and 3 (CHANNELS.md 8): the first chunk seen
// anchors the block in the held index under (key_channel, key_index); every
// chunk after it must agree with the anchor on its count, first number and
// base, or it is MESSAGE_BAD_BLOCK. The chunk is copied to ring + index * F and
// counted in the anchor, so a block completes when the anchor's count does,
// with no rescan. Returns the anchor, or NULL with the violation counted or
// the region exhausted. tla/RNBlocks.tla Receive.

#define RN_HELD_RELIABLE_BLOCK      0xFFFFFFFFu     // code 2's key channel: above every channel index

struct rn_receive_descriptor_t * rn_channels_block_collect( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record,
                                                            uint32_t key_channel, uint32_t key_index, uint32_t slot,
                                                            uint32_t first_number, int64_t base_units, uint32_t * out_held );

// Every chunk is in: the first number's descriptor takes the block, and the
// anchor gives it up. keep leaves it in the held index, complete, for its turn
// (code 3); otherwise the held entry is erased (code 2 delivers it now).

void rn_channels_block_settle( struct rn_channels_t * channels, uint32_t held, struct rn_receive_descriptor_t * anchor,
                               uint32_t first_slot, uint32_t count, uint32_t total, uint32_t base_units, bool keep );

// Code 2's case (sdk/rocketnet_channel_reliable.cpp).

int rn_channels_dispatch_reliable( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// The dispatch half of m4 checks 7 and 8: a block's shape bounds (count at
// least two, inside W and max_message_bytes, index inside the count, every
// fragment but the last exactly F) and its base units, uniform across the three
// reliable codes. False on a breach; the caller counts BAD_BLOCK. The caller
// checks the block's span and the fragment's own ring span against the ring and
// counts its own counter. The layer 2 reader works on wire limits before a
// region exists, so it checks the same bounds apart.
bool rn_block_shape_valid( const struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, int64_t * out_base_units );

// The receiver's descriptor of a record's number: NULL when the slot is live
// under a different number (check 10, SLOT_COLLISION); fills a not-live slot
// from the record so dispatch stands alone.
struct rn_receive_descriptor_t * rn_channels_descriptor( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, uint32_t * out_slot );

#if ROCKETNET_DEV
// Present only in the unit-test library: the receive descriptors a code-2
// block's chunks visit on their way to one delivery, counted as work done.
struct rn_block_cost_t
{
    uint64_t chunks;
    uint64_t descriptor_visits;
};
void rn_channels_block_cost_reset();
rn_block_cost_t rn_channels_block_cost_read();
#endif // #if ROCKETNET_DEV

// Codes 1 and 4's case (sdk/rocketnet_channel_sequenced.cpp).

int rn_channels_dispatch_sequenced( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out );

// RNChannelTypes!Drain: release the newest held code-1 or code-4 record when
// its ordered epoch opens. Returns true if it appended a delivery.
bool rn_channels_drain_sequenced( struct rn_channels_t * channels, uint32_t channel );
void rn_channels_end_update_sequenced( struct rn_channels_t * channels );

// Codes 5, 6 and 7's case (sdk/rocketnet_channel_receipts.cpp).

// RNReceipts EffectiveKind: returns wire code 2 or 3 for public receipt
// types 6 or 7, and -1 for every other type. No wire receipt code is sent.
int rn_channels_reliable_receipt_base( int type );

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

// Ends the update (RNChannel!EndUpdate): the delivery table's views die, the
// receiver's descriptors of the messages delivered in this update (a block's
// every fragment) are released (6.3 step 1, a number is live while held or
// delivered in the current update), and the per-update counts start over. The
// application is finished with the deliveries before this.

void rn_channels_end_update( struct rn_channels_t * channels );

// ---------------------------------------------------------------
//        the ordered path of one connection, ends to end (CHANNELS 6.4 to 9.3)
// ---------------------------------------------------------------

#define ROCKETNET_ORDERED_MAX_ITEMS     96      // the items one write call plans

// What rn_ordered_receive returns for a violation that layer 4 found (an
// ordering index, a block or a ring span that breaks 7.2 or 8): the counters
// name it. Layer 2's and layer 3's violations return their own
// ROCKETNET_MESSAGE_VIOLATION_*.

#define ROCKETNET_CHANNEL_VIOLATION     ROCKETNET_MESSAGE_NUM_VIOLATIONS

// What a connection owns to send and receive RELIABLE and RELIABLE_ORDERED: the dispatch
// (layer 4), the sender's numbering and acks (layer 3), the receiver's window
// (layer 3), and the packet builder (layer 6). A plain struct the owner embeds
// in its connection state; it allocates nothing (R2), and all its memory is the
// connection's region.

struct rn_ordered_t
{
    struct rn_channels_t channels;
    struct rn_window_send_t send;
    struct rn_window_ack_t ack;
    struct rn_window_receive_t receive;
    struct rn_packet_builder_t builder;
    struct rocketnet_send_budget_t * budget;        // R6, or NULL
    struct rocketnet_message_limits_t limits;       // what layer 2's reader checks a payload against

    // CONFIG (docs/CHANNELS.md 10.3, 13.4; tla/RNEndpoint.tla)
    struct rocketnet_message_config_body_t config;  // this end's CONFIG body, set by rn_ordered_announce_config
    int config_announced;                           // this end's CONFIG is queued (RNEndpoint Reset)
    int config_received;                            // the remote endpoint's CONFIG was taken and equals this end's (RNEndpoint remoteCfg # NoCfg)
};

// Binds the whole path to one connection's carved region. `config` is the
// connection's message config (NULL: the defaults), `budget` the connection's
// send budget (NULL: unbudgeted), packets_per_update at most
// ROCKETNET_PACKET_BUILDER_MAX_PACKETS, payload_capacity the frame payload
// bytes. The region is carved already; nothing is allocated.

bool rn_ordered_init( struct rn_ordered_t * ordered, struct rn_region_t * region, const struct rocketnet_message_config_t * config,
                      struct rocketnet_send_budget_t * budget, int packets_per_update, int payload_capacity );

// RNChannel!WritePacket for one update. Resends first (the oldest due
// number, then each number numbered and never sent), then new messages, each
// numbered late and whole (a block takes consecutive numbers at once, 6.2, 8),
// packed several small records to a packet and one chunk to a packet, under the
// send budget (R6). Writes into `payloads` (each payload_capacity bytes) and
// returns the number of packets written. The caller seals each, and tells this
// the transport's packet id: rn_ordered_commit, or rn_ordered_packet_failed.

int rn_ordered_write( struct rn_ordered_t * ordered, double now, uint8_t ** payloads, int num_payloads );

void rn_ordered_commit( struct rn_ordered_t * ordered, int packet_index, uint64_t packet_id, double now );

void rn_ordered_packet_failed( struct rn_ordered_t * ordered, int packet_index, double now );

bool rn_ordered_settle( struct rn_ordered_t * ordered, int packet_index, int actual_wire_bytes );

// R1: the transport's acked packet ids, the only ack there is
// (rn_channels_feed_acks).

uint32_t rn_ordered_acks( struct rn_ordered_t * ordered, const uint64_t * packet_ids, uint32_t count, double now );

// RNChannel!OpenPacket: one received payload, whole. Layer 2's checks, then
// each record in wire order through layer 3's window and layer 4's dispatch.
// Returns ROCKETNET_MESSAGE_VIOLATION_NONE, or the violation that ended the
// payload (the connection is then to be disconnected: rn_ordered_decide).
// When a reliable record cannot reserve its deliveries the payload ends
// part-way and this returns ROCKETNET_CHANNEL_RECEIVE_DEFERRED; its
// acknowledgement is withheld for retry (standard/MESSAGES.md m1.4 rule 2),
// which is not a violation.
// Deliveries are appended to the region's delivery table.

int rn_ordered_receive( struct rn_ordered_t * ordered, const uint8_t * payload, int bytes );

// tla/RNEndpoint.tla Reset, the CONFIG half (CHANNELS.md 10.3): queues this
// end's one CONFIG control record, code 2 with CONTROL set and the 28-byte
// body of 13.4 written from `config` (NULL: the defaults) and the enabled
// mask, at the head of IMMEDIATE on channel 0, so it takes the connection's
// first reliable number and is resent until acked like any reliable record.
// Called once per connection, before anything else is queued. False when it
// was already queued, or the region has no room (the region is then
// exhausted and the owner kicks the connection).

bool rn_ordered_announce_config( struct rn_ordered_t * ordered, const struct rocketnet_message_config_t * config );

// tla/RNEndpoint.tla OnPayload with ConfigCompare (CHANNELS.md 10.3, 13.4):
// the remote endpoint's CONFIG record, already accepted by layer 3, read and compared
// field by field with this end's body. Equal: config_received is set. A
// different field, or a version this end does not know, logs one error line
// naming the first differing field and both values and sets the sticky error
// ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH (rn_ordered_decide). A body layer 2
// refuses (MESSAGE_BAD_CONTROL) is a wire violation. rn_ordered_receive calls
// it for each new control record; returns the error it set, or NONE.

int rn_ordered_take_config( struct rn_ordered_t * ordered, const struct rocketnet_message_record_t * record );

// The update's deliveries, in order: region->deliveries[0 .. count).

uint32_t rn_ordered_deliveries( const struct rn_ordered_t * ordered );

// RNChannel!EndUpdate for the receiver (rn_channels_end_update).

void rn_ordered_end_update( struct rn_ordered_t * ordered );

// What to end the connection with, or ROCKETNET_DISCONNECT_REASON_NONE: the region's
// exhaustion first (tla/RNConnectionMemory.tla Kick), then a wire violation,
// then an unacked reliable message past its timeout (6.4). The owner reads it
// every update.

int rn_ordered_decide( const struct rn_ordered_t * ordered );

#endif // ROCKETNET_SDK_ROCKETNET_CHANNEL_H
