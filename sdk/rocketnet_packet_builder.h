/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_PACKET_BUILDER_H
#define ROCKETNET_SDK_ROCKETNET_PACKET_BUILDER_H

#include <stdint.h>

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_window_ack.h"

// Layer 6's packet builder (standard/MESSAGES.md m2): per connection per update,
// write packets through layer 2's writer into frames the caller provides,
// packing several small records per packet, taking items in the budget's class
// order, and committing each packet's carried reliable numbers to layer 3's
// sent map under the packet id the transport assigned. The spec is
// proofs/tla/RNChannel.tla (WritePacket) with proofs/tla/RNSendBudget.tla (Send, Drop, Wait);
// each function names the action it implements.
//
// The builder owns no memory: its state is a fixed struct the owner embeds in
// its connection state (R2), and every byte it writes lands in a caller frame
// or in layer 3's region tables.
//
// The one copy on this path is THE COPIES (a): rocketnet_message_writer_add
// copies a record's bytes from its region record into the frame, because a
// reliable record must outlive the frame for resends and the transport encrypts
// the frame in place.

#define ROCKETNET_PACKET_BUILDER_MAX_PACKETS            16      // the packets ONE builder call tracks; the endpoint asks for one packet per call and counts the update's packets against the config's packets_per_update (1 .. 1024), so this is not the per-update limit
#define ROCKETNET_PACKET_BUILDER_MAX_NUMBERS            16      // the reliable numbers one packet carries (layer 3's reference ring takes 16 per packet)
#define ROCKETNET_PACKET_BUILDER_MAX_RECORDS            16      // the records one packet carries: a packet's records are tracked to commit or to give back
#define ROCKETNET_PACKET_BUILDER_MAX_ITEMS              96      // the bounded ordered plan one call accepts (the producer has the same 96-item ceiling)

#define ROCKETNET_PACKET_BUILDER_LEVELS                 4       // the four priorities, ROCKETNET_PRIORITY_IMMEDIATE (0) to ROCKETNET_PRIORITY_LOW (3)

#define ROCKETNET_PACKET_NUMBER_NONE                    0xFFFFFFFFFFFFFFFFull   // an item that takes no reliable number (I3)

// One item the builder may write: a record's bytes as they sit in the region,
// with the fields its wire code carries.

struct rn_packet_item_t
{
    const uint8_t * data;           // the region record's bytes, never copied by this struct
    uint32_t bytes;                 // 1 to 65535
    uint64_t number;                // the reliable number, or ROCKETNET_PACKET_NUMBER_NONE
    uint32_t ordering;              // codes 1, 3, 4
    uint32_t sequencing;            // codes 1, 4
    uint32_t descriptor;            // the send descriptor, or RN_DESCRIPTOR_NONE
    uint32_t ring;                  // codes 3, 4 and a block: the receive ring offset in 16-byte units
    uint32_t fragment;              // a block: this fragment's index
    uint32_t fragments;             // a block: the fragment count - 1 (the wire field)
    uint16_t channel;
    uint8_t code;                   // the wire code the record carries
    uint8_t send_class;             // ROCKETNET_SEND_CLASS_*: the budget's class order
    uint8_t taken;                  // the builder sets this on an item it wrote
    uint8_t priority;               // ROCKETNET_PRIORITY_*: 0 IMMEDIATE to 3 LOW; boosts the item's whole channel (9.2)
    uint8_t block;                  // nonzero: one fragment of a block (standard/MESSAGES.md m8)
    uint8_t alone;                  // nonzero: the record travels in a packet by itself (a chunk: proofs/tla/RNChannel.tla, a chunk travels alone)
    uint8_t resend;                 // nonzero: a resend; the count is taken when the packet carrying it is sent (rn_packet_builder_commit)
    uint8_t control;                // nonzero: a layer control record, code 2 with CONTROL set (standard/MESSAGES.md m11)
};

// One call's flat sequence entry, in a plan built afresh on each call's stack.

struct rn_packet_builder_sequence_t
{
    uint32_t item_index;
    uint16_t group_index;
    uint8_t suffix_level;
    uint8_t reserved;
};

// One contiguous channel/class range in sequence[]. cursor is the first
// untaken entry. Groups are sorted by class then the full 16-bit channel.

struct rn_packet_builder_group_t
{
    uint16_t channel;
    uint8_t begin;
    uint8_t end;
    uint8_t cursor;
    uint8_t send_class;
};

// One write call's bounded scratch. It lives on that call's stack, not in
// each connection: the maximum is 96 items, so the flat plan stays small
// without making every client permanently pay for transient scheduler state.

struct rn_packet_builder_plan_t
{
    struct rn_packet_builder_sequence_t sequence[ROCKETNET_PACKET_BUILDER_MAX_ITEMS];
    struct rn_packet_builder_group_t groups[ROCKETNET_PACKET_BUILDER_MAX_ITEMS];
    uint8_t sort_indices[ROCKETNET_PACKET_BUILDER_MAX_ITEMS];
    uint8_t group_count;
    uint8_t class_group_begin[ROCKETNET_SEND_CLASS_UNRELIABLE + 1];
    uint8_t class_group_end[ROCKETNET_SEND_CLASS_UNRELIABLE + 1];

#if ROCKETNET_DEV
    uint64_t plan_operations;                   // bounded grouping/tag/suffix operations in this call
    uint64_t selection_operations;              // current group heads inspected in this call
#endif // #if ROCKETNET_DEV
};

struct rn_packet_builder_t
{
    rn_region_t * region;                       // the connection's region (its counters take the drops)
    rn_window_ack_t * ack;                      // layer 3, or NULL when nothing here is reliable
    rocketnet_send_budget_t * budget;           // R6's budget, or NULL when the caller budgets above

    int packets_per_update;                 // 1 to ROCKETNET_PACKET_BUILDER_MAX_PACKETS
    int payload_capacity;                       // the bytes a frame's payload area holds

    uint32_t num_channels;                      // the round robin's modulus (17.3 item 4)
    uint32_t round_robin;                       // the channel the last item was taken from
    uint32_t packets;                           // packets written by the last call
    uint32_t unreliable_dropped;                // unreliable items dropped over budget by the last call

    uint64_t level_clock[ROCKETNET_PACKET_BUILDER_LEVELS];     // RNScheduler's clock per priority level (the level weights)
    uint64_t clock_base;                        // the clock of the level taken last: a clock behind it is pulled forward to it

    struct
    {
        uint64_t numbers[ROCKETNET_PACKET_BUILDER_MAX_NUMBERS];     // the reliable numbers this packet carried
        uint32_t count;
        uint32_t records;                       // the records this packet carried
        uint32_t payload_bytes;                 // the sealed payload's length L
        int admitted_wire_bytes;                // what the budget charged for it, to settle after sealing
    } packet[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
};

#if ROCKETNET_DEV
// The calling thread's last completed flat plan, for operation-bound and tag
// checks. This development-only record is not part of a connection and is
// absent from ROCKETNET_DEV=0 builds.
const struct rn_packet_builder_plan_t * rn_packet_builder_test_last_plan( void );
#endif // #if ROCKETNET_DEV

// Binds a builder to one connection. Allocates nothing. packets_per_update
// is clamped to ROCKETNET_PACKET_BUILDER_MAX_PACKETS; payload_capacity outside
// 1..ROCKETNET_MESSAGE_SINGLE_BYTES, or a NULL region, is refused and the
// struct is left zeroed, so it writes no packet.

bool rn_packet_builder_init( struct rn_packet_builder_t * builder, struct rn_region_t * region, struct rn_window_ack_t * ack, struct rocketnet_send_budget_t * budget, int packets_per_update, int payload_capacity );

// RNChannel!WritePacket with RNSendBudget!Send, !Wait and !Drop, taking items
// in RNScheduler!Write's order.
//
// Writes at most packets_per_update packets into the caller's payload
// areas (each payload_capacity bytes; the caller owns the frames and passes
// frame + payload_index). Items are taken in three nested orders
// (proofs/tla/RNScheduler.tla, standard/MESSAGES.md m10):
//
// 1. the budget's class order, the outer rule -- retransmits, ordered
//    progress, new reliable, unreliable -- whatever the priorities;
// 2. within a class, the four priority levels: the level with the
//    smallest clock goes next and its clock advances by its stride (1, 5, 14,
//    35 for IMMEDIATE to LOW), so under a backlog no level starves;
// 3. within a level, round robin across channels, one item each (17.3 item
//    4).
//
// Within a channel the caller's order is kept: only a channel's first item of
// a class is offered, at the best priority among that channel's items of the
// class, so a priority boosts the whole channel and never reorders it (17.3
// item 3). The caller lists each channel's items in the order the
// application queued them. An item whose priority is not 0 to 3 is never
// taken. Taking items is linear in the plan: one stable radix plan pass makes
// contiguous channel/class ranges and their suffix priority minima, and each
// take looks at one current range head. Several small records pack per packet (9.3). A record that does not
// fit ends the packet; it is never split. When every item is taken the clocks
// reset to their seeds.
//
// Each finished packet is admitted against the budget at its own class, at
// rocketnet_send_budget_max_wire_bytes of its payload. A packet the budget
// cannot pay is not written: a reliable one waits for the next update and is
// never dropped (R6), an unreliable one is dropped and counted
// (MESSAGE_UNRELIABLE_OVER_BUDGET). Either way the update stops there: nothing
// after a waiting reliable item spends the bucket.
//
// Returns the number of packets written, 0 when the plan is empty (an empty
// plan writes nothing, 9.3). Items taken are marked in item->taken.

int rn_packet_builder_write( struct rn_packet_builder_t * builder, struct rn_packet_item_t * items, int num_items, uint8_t ** payloads, int num_payloads );

// Commits one written packet's carried reliable numbers to layer 3's sent map
// under `packet_id`: the id the transport assigned when it finished the packet
// (rocketnet_client_transport_finish_packet, rocketnet_server_transport_
// finish_packet), never an invented counter, so the keys of the sent map are
// the ids rocketnet_*_transport_acked_packets returns and a resend's ack
// matches the send that produced it. RNChannel!WritePacket's smap' entry.

void rn_packet_builder_commit( struct rn_packet_builder_t * builder, int packet_index, uint64_t packet_id, double now );

// Recovers a packet the transport refused to finish (standard/MESSAGES.md m9.1): its numbers keep their assignment and go to the head of the
// resend FIFO, due now, so the next update resends them first.

void rn_packet_builder_packet_failed( struct rn_packet_builder_t * builder, int packet_index, double now );

// After sealing: refunds what the packet did not use, when the transport's
// ack block came in under the bound admitted at write time
// (rocketnet_send_budget_settle). Returns false for a packet index outside the
// last call, or an actual above the admitted, and changes nothing.

bool rn_packet_builder_settle( struct rn_packet_builder_t * builder, int packet_index, int actual_wire_bytes );

// The reliable numbers one written packet carried, and their count. NULL for
// an index outside the last call.

#if ROCKETNET_DEV
const uint64_t * rn_packet_builder_numbers( const struct rn_packet_builder_t * builder, int packet_index, uint32_t * out_count );
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_PACKET_BUILDER_H
