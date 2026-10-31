/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_ENDPOINT_H
#define ROCKETNET_SDK_ROCKETNET_ENDPOINT_H

#include <stdint.h>

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_packet_builder.h"
#include "rocketnet_send_budget.h"

// Layer 7 of the message layer: the endpoint, one connection as a pure machine
// (proofs/tla/RNEndpoint.tla). It is the composition of layers 3 to 6 for
// one connection -- the window, the channels, the blocks, the scheduler and the
// packet builder (rn_ordered_t), the unreliable packet builder and the send
// budget -- behind one struct that points into the connection's region. Events
// come in (send, acked, payload, tick), actions go out (bytes into a frame the
// caller supplies, deliveries, receipts, an error, a deadline). The endpoint
// never reads a clock (time is the `now` argument), never calls the transport
// and never allocates. proofs/tla/RNEndpoint.tla is its spec; each function names the
// action it implements.
//
// The message objects (sdk/rocketnet_message_object.h) hold one endpoint per
// connection: rn_message_connection_t is an rn_endpoint_t that points to the
// object's compact per-slot state (the slot binding and receive views). The objects keep the
// transport link, the slots machine and the update order.

#define RN_ENDPOINT_WRITE_IDLE              0   // no write pass open
#define RN_ENDPOINT_WRITE_RELIABLE          1   // the reliable path: retransmits and ordered progress
#define RN_ENDPOINT_WRITE_UNRELIABLE        2   // the queued unreliable records
#define RN_ENDPOINT_WRITE_DONE              3   // the pass is over; next_packet_bytes answers 0 until the next flush or update

#define RN_ENDPOINT_PACKET_NONE             0
#define RN_ENDPOINT_PACKET_ORDERED          1   // rn_ordered_write's packet
#define RN_ENDPOINT_PACKET_UNRELIABLE       2   // small unreliable records packed by the unreliable builder
#define RN_ENDPOINT_PACKET_SEQUENCED_LARGE  3   // one code-1 record larger than a packet, fragmented by the transport

struct rn_endpoint_config_t
{
    const struct rocketnet_message_config_t * config;   // the connection's message config; outlives the endpoint
    int32_t send_kbps;                                  // the send budget a reset starts with (R6); 0 or lower uncapped
    int max_payload_bytes;                              // the transport's payload cap, for rn_endpoint_send's large unreliable check (0: ROCKETNET_MAX_PACKET_PAYLOAD_BYTES)
};

/*
    The declaration manifest: the struct is declared from this list, so a
    member exists only as an entry here. The RESET/KEEP inventory in
    rocketnet_connection_reset.h is compared with it by name, and a member with
    no decision is named in test_connection_reset_inventory_covers_every_field.
*/
#define RN_ENDPOINT_LAYOUT_FIELDS( X ) \
    X( region, rn_region_t * region )                                                                                                                 \
    X( ordered, rn_ordered_t ordered ) /* layers 3 to 6 over the region */                                                                            \
    X( unreliable_builder, rn_packet_builder_t unreliable_builder ) /* layer 6 for UNRELIABLE records (no reliable numbers) */                        \
    X( budget, rocketnet_send_budget_t budget ) /* R6 */                                                                                              \
    X( config, const struct rocketnet_message_config_t * config )                                                                                     \
    X( default_kbps, int32_t default_kbps ) /* the budget a reset starts with; 0 or lower uncapped */                                                 \
    X( send_budget_enabled, int send_budget_enabled ) /* 0: this connection is uncapped (R6, standard/MESSAGES.md m9.4) */                                \
    X( unreliable_limit, uint32_t unreliable_limit ) /* the largest UNRELIABLE message: one record in one packet */                                   \
    X( max_payload_bytes, int max_payload_bytes ) /* the transport's payload cap (rn_endpoint_send) */                                                \
    X( send_error, int send_error ) /* ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED once a send found no room (R5), else NONE */ /* CONFIG (10.3): the wait for the remote endpoint's CONFIG */                                                                                               \
    X( config_wait_start, double config_wait_start ) /* when this end began waiting for the remote endpoint's CONFIG, or -1 before its transport is connected */ \
    X( config_error, int config_error ) /* ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT once the remote endpoint's CONFIG is past reliable_timeout, else NONE */     \
    X( serial, uint32_t serial ) /* the receipt serial of the last finished send (7.4) */ /* the start between start and finish (one at a time) */                                                                                          \
    X( pending_type, int pending_type ) /* -1 when no start is pending */                                                                             \
    X( pending_descriptor, uint32_t pending_descriptor ) /* UNRELIABLE: its send descriptor */                                                        \
    X( pending_bytes, uint32_t pending_bytes ) /* the in-place unreliable send (docs/OUTGOING-BUILD.md, proofs/tla/RNOutgoingBuild.tla): */ /* a fitting unreliable message written in the transport frame start took */                                                                      \
    X( pending_in_place, int pending_in_place ) /* 1 while the pending start is written in place */                                                   \
    X( pending_channel, int pending_channel ) /* its channel */                                                                                       \
    X( pending_header, int pending_header ) /* its header bytes in front of the data (0: the SINGLE form) */                                          \
    X( in_place_serial, uint32_t in_place_serial ) /* the finished in-place packet: its serial */                                                     \
    X( in_place_receipt, int in_place_receipt ) /* 1 when it carries a receipt (UNRELIABLE_WITH_ACK_RECEIPT) */                                       \
    X( in_place_admitted, int in_place_admitted ) /* the wire bytes admitted to the budget (0: uncapped) */ /* the queued UNRELIABLE sends: a ring of descriptor entries in push order (region->unreliable_ring) */                                                                   \
    X( unreliable_head, uint32_t unreliable_head )                                                                                                    \
    X( unreliable_ring_head, uint32_t unreliable_ring_head ) /* the ring position of the oldest entry */                                              \
    X( unreliable_ring_used, uint32_t unreliable_ring_used ) /* entries from there, tombstones included */                                            \
    X( unreliable_count, uint32_t unreliable_count ) /* the write pass (9.3): next_packet_bytes plans a packet, write_packet */ /* writes it, packet_sent or packet_failed ends it */                                                                                             \
    X( write_phase, int write_phase ) /* RN_ENDPOINT_WRITE_* */                                                                                       \
    X( written, int written ) /* packets this update, in place and queued, against packets_per_update */                                          \
    X( packet_kind, int packet_kind ) /* RN_ENDPOINT_PACKET_* of the planned packet */                                                                \
    X( packet_written, int packet_written ) /* 1 between write_packet and packet_sent or packet_failed */                                             \
    X( large_bytes, int large_bytes ) /* SEQUENCED_LARGE: the payload bytes */                                                                        \
    X( large_admitted, int large_admitted ) /* SEQUENCED_LARGE: the wire bytes charged to the budget */                                               \
    X( large_descriptor, uint32_t large_descriptor ) /* SEQUENCED_LARGE: its send descriptor */                                                       \
    X( packet_records, uint32_t packet_records ) /* UNRELIABLE: the records the written packet carries */                                             \
    X( receipt_count, uint32_t receipt_count ) /* UNRELIABLE: the code-5 serials the written packet carries */                                        \
    X( receipt_serials, uint32_t receipt_serials[ROCKETNET_PACKET_BUILDER_MAX_RECORDS] )

struct rn_endpoint_t
{
#define RN_ENDPOINT_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    RN_ENDPOINT_LAYOUT_FIELDS( RN_ENDPOINT_LAYOUT_FIELDS_DECLARE )
#undef RN_ENDPOINT_LAYOUT_FIELDS_DECLARE
};

// Binds the endpoint to its region (carved already) and its configuration.
// Nothing is allocated. rn_endpoint_reset starts the first connection.
void rn_endpoint_init( struct rn_endpoint_t * endpoint, rn_region_t * region, const struct rn_endpoint_config_t * config );

// A new connection on this endpoint: the region carved again and every layer
// bound to it fresh, so nothing of the previous connection is reachable.
void rn_endpoint_reset( struct rn_endpoint_t * endpoint, double now );

// proofs/tla/RNEndpoint.tla Reset, the CONFIG half (10.3): called when the connection
// this endpoint was reset for is connected, before anything else is queued.
// This end's CONFIG control record takes the connection's first reliable
// number and is resent until acked. No room is region exhaustion, which
// rn_endpoint_error reports.
void rn_endpoint_announce_config( struct rn_endpoint_t * endpoint, double now );

// ---------------------------------------------------------------
//                          events in
// ---------------------------------------------------------------

// proofs/tla/RNSendBudget.tla SetKbps (R6): the runtime override. kbps 0 or lower
// uncaps this connection; a positive value up to
// ROCKETNET_SEND_BUDGET_MAX_KBPS caps it, initialising the bucket at now when
// the connection was uncapped. Above the maximum is refused, unchanged.
bool rn_endpoint_set_send_kbps( struct rn_endpoint_t * endpoint, int32_t kbps, double now );

// The send, as one call: the bytes copied into the region record. serial 0
// takes the endpoint's next serial. 0 when refused; then rn_endpoint_error
// says whether the refusal ended the connection.
uint32_t rn_endpoint_send( struct rn_endpoint_t * endpoint, const uint8_t * data, int bytes,
                           int reliability, int priority, int channel, uint32_t serial );

// The send in two halves, for a caller that writes the bytes in place (R3):
// start reserves the record and returns where to write it, finish queues it
// and returns its serial (serial 0: the next; asserts in a debug build and
// returns 0 in a release build when no queued start is pending), abort returns
// the room. transport_max_payload_bytes is the transport's payload cap now.
uint8_t * rn_endpoint_start( struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int transport_max_payload_bytes );

// The room query: true when rn_endpoint_start with these arguments, now,
// would be given its room in the region; false when it would be refused or
// would find no room (and so end the connection, R5). It shares the start's
// route and the region's allocation decision (rn_region_send_fit), so the two
// never disagree. Pure: changes nothing, counts nothing, allocates nothing.
bool rn_endpoint_send_room( const struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int transport_max_payload_bytes );
uint32_t rn_endpoint_finish( struct rn_endpoint_t * endpoint, int bytes, uint32_t serial );
void rn_endpoint_abort( struct rn_endpoint_t * endpoint );

// The in-place send of a fitting unreliable message (docs/OUTGOING-BUILD.md,
// proofs/tla/RNOutgoingBuild.tla StartInPlace and FinishInPlace): the owner starts a
// transport frame and the message is written in it, with no copy between the
// message layer and the transport. in_place_header answers the header bytes
// in front of the data (0 for UNRELIABLE, the SINGLE form; 3 for the receipt
// twin's code-0 record; 10 or 11 for a code-1 record) when the message takes
// this path, RN_ENDPOINT_IN_PLACE_QUEUED when it fits but unreliable messages
// wait in the queue ahead of it (it queues behind them, so no mode is
// reordered; for UNRELIABLE_SEQUENCED also its channel's queue or a pending
// block chunk) or the update's packet allowance is spent, and
// RN_ENDPOINT_IN_PLACE_NO otherwise (the region path decides and refuses).
// In-place packets and the write pass's packets share packets_per_update;
// while reliable work is due at now, an in-place packet never takes the last
// of it, so the write pass's reliable phase has a packet for that work in the
// same update (RNOutgoingBuild ReliableSentWithinOneUpdate; a full window or a
// waiting budget can still hold the work, and the packet then goes to the
// unreliable phase). It changes nothing and counts nothing. The frame the owner asks for holds header + bytes + 1 payload bytes
// (the declared maximum, the header and the form byte).
#define RN_ENDPOINT_IN_PLACE_NO             -1
#define RN_ENDPOINT_IN_PLACE_QUEUED         -2

int rn_endpoint_in_place_header( const struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int transport_max_payload_bytes, double now );

// Whether the send budget can pay an in-place packet of payload_bytes now
// (R6, RNSendBudget Fits at the unreliable class): the bucket refilled to now,
// no reliable item of the last write pass waiting, and, while the reliable path
// has work due, one maximum packet left over for it. A message the bucket
// cannot pay now takes the queued path, where the write pass sends it or drops
// it and counts it (MESSAGE_UNRELIABLE_OVER_BUDGET) by the rule it always had.
bool rn_endpoint_in_place_affordable( struct rn_endpoint_t * endpoint, int payload_bytes, double now );

// The start is pending in place: the owner holds the frame and the game
// writes at payload root + header.
void rn_endpoint_in_place_begin( struct rn_endpoint_t * endpoint, int channel, int reliability, int bytes, int header );

// The finish of an in-place start: the header and the form byte are written
// around the bytes in place at payload (the frame's payload root), the
// sequencing index and the serial are taken, and the send budget admits the
// packet. Asserts in a debug build and returns 0 in a release build when no
// in-place start is pending or payload is NULL. Returns the serial, or 0 when
// bytes is outside 1 to the bytes started (the start stays pending).
// *payload_bytes is the payload length to finish the frame with, or 0 when the
// budget dropped the message (counted MESSAGE_UNRELIABLE_OVER_BUDGET; a
// receipt takes its LOSS): the owner then aborts the frame.
uint32_t rn_endpoint_in_place_finish( struct rn_endpoint_t * endpoint, uint8_t * payload, int bytes, double now, int * payload_bytes );

// The finished in-place frame left (finish_packet returned wire_bytes), or
// did not (counted MESSAGE_UNRELIABLE_SEND_FAILED; a receipt takes its LOSS).
void rn_endpoint_in_place_sent( struct rn_endpoint_t * endpoint, uint64_t packet_id, int wire_bytes, double now );
void rn_endpoint_in_place_failed( struct rn_endpoint_t * endpoint );

// The transport acked packet_id (R1: the only ack).
void rn_endpoint_on_acked( struct rn_endpoint_t * endpoint, uint64_t packet_id, double now );

// One payload the transport opened. Its deliveries are appended to the
// region's delivery table; out, when not NULL, also gets a copy of up to
// out_capacity of them, and *out_count their number.
bool rn_endpoint_on_payload( struct rn_endpoint_t * endpoint, const uint8_t * payload, int bytes,
                             rn_delivery_t * out, int out_capacity, int * out_count, double now );

// The receipts' LOSS sweep, the end of the update's receipt list, and the
// reliable timeout, which rn_endpoint_error then reports. Called once per
// update for every connected endpoint, whether or not its write pass runs.
void rn_endpoint_tick( struct rn_endpoint_t * endpoint, double now );

// proofs/tla/RNEndpoint.tla Tick, the CONFIG half (10.3): called once per update,
// after the update's payloads. The first call with transport_connected set
// starts the wait for the remote endpoint's CONFIG; from then on, a call at
// reliable_timeout or more after it, with the remote endpoint's CONFIG still not taken,
// sets RELIABLE_TIMEOUT, which rn_endpoint_error reports. Whether the
// transport is connected is the caller's to say (a seam: the endpoint never
// calls the transport).
void rn_endpoint_config_tick( struct rn_endpoint_t * endpoint, int transport_connected, double now );

// The start of an update on the receive side: the last update's deliveries
// expire and their room returns to the region. Returns how many there were.
uint32_t rn_endpoint_begin_update( struct rn_endpoint_t * endpoint );

// ---------------------------------------------------------------
//                          actions out
// ---------------------------------------------------------------

// Opens the write pass for this flush (send_packets): a pass the last flush
// ended opens again at its reliable phase.
void rn_endpoint_begin_flush( struct rn_endpoint_t * endpoint );

// The size of the frame payload to request for the next packet of this write
// pass, or 0 when the pass is over. An ended pass answers 0 until the next
// flush or update.
int rn_endpoint_next_packet_bytes( struct rn_endpoint_t * endpoint, double now );

// Writes the planned packet into payload (at least next_packet_bytes bytes)
// for the transport packet packet_id. Returns the payload bytes written, or 0
// when nothing was written (the caller aborts its frame and asks again).
int rn_endpoint_write_packet( struct rn_endpoint_t * endpoint, uint8_t * payload, int capacity, uint64_t packet_id, double now );

// The written packet left: wire_bytes is the sealed size the transport
// reported. Commits the packet to the sent map and settles the budget.
void rn_endpoint_packet_sent( struct rn_endpoint_t * endpoint, uint64_t packet_id, int wire_bytes, double now );

// The planned packet did not leave: either no frame was started for it
// (nothing written), or the transport refused the written packet.
void rn_endpoint_packet_failed( struct rn_endpoint_t * endpoint, uint64_t packet_id, double now );

int rn_endpoint_take_receipts( const struct rn_endpoint_t * endpoint, struct rocketnet_message_receipt_t * out, int capacity );

// 0, or the ROCKETNET_DISCONNECT_REASON_* reason the connection ends with (12).
int rn_endpoint_error( const struct rn_endpoint_t * endpoint );

#endif // ROCKETNET_SDK_ROCKETNET_ENDPOINT_H
