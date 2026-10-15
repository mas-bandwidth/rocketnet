/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_PACKET_BUILDER_H
#define ROCKETNET_PACKET_BUILDER_H

#include <stdint.h>

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_window_ack.h"

// Layer 6's packet builder (docs/CHANNELS.md 9.3): per connection per update,
// write packets through layer 2's writer into frames the caller provides,
// packing several small records per packet, taking items in the budget's class
// order, and committing each packet's carried reliable numbers to layer 3's
// sent map under the packet id the transport assigned. The spec is
// tla/RNChannel.tla (WritePacket) with tla/RNSendBudget.tla (Send, Drop, Wait);
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

#define ROCKETNET_PACKET_BUILDER_MAX_PACKETS            16      // the packets one builder call tracks, config max_packets_per_update at most this
#define ROCKETNET_PACKET_BUILDER_MAX_NUMBERS            16      // the reliable numbers one packet carries (layer 3's reference ring takes 16 per packet)

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
    uint16_t channel;
    uint8_t code;                   // the wire code the record carries
    uint8_t send_class;             // ROCKETNET_SEND_CLASS_*: the budget's class order
    uint8_t taken;                  // the builder sets this on an item it wrote
    uint8_t pad[3];
};

struct rn_packet_builder_t
{
    rn_region_t * region;                       // the connection's region (its counters take the drops)
    rn_window_ack_t * ack;                      // layer 3, or NULL when nothing here is reliable
    rocketnet_send_budget_t * budget;           // R6's budget, or NULL when the caller budgets above

    int max_packets_per_update;                 // 1 to ROCKETNET_PACKET_BUILDER_MAX_PACKETS
    int payload_capacity;                       // the bytes a frame's payload area holds

    uint32_t num_channels;                      // the round robin's modulus (17.3 item 4)
    uint32_t round_robin;                       // the channel the last packet took from
    uint32_t packets;                           // packets written by the last call
    uint32_t unreliable_dropped;                // unreliable items dropped over budget by the last call

    struct
    {
        uint64_t numbers[ROCKETNET_PACKET_BUILDER_MAX_NUMBERS];     // the reliable numbers this packet carried
        uint32_t count;
        uint32_t records;                       // the records this packet carried
        uint32_t payload_bytes;                 // the sealed payload's length L
        int admitted_wire_bytes;                // what the budget charged for it, to settle after sealing
    } packet[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
};

// Binds a builder to one connection. Allocates nothing. max_packets_per_update
// is clamped to ROCKETNET_PACKET_BUILDER_MAX_PACKETS; payload_capacity outside
// 1..ROCKETNET_MESSAGE_SINGLE_BYTES, or a NULL region, is refused and the
// struct is left zeroed, so it writes no packet.

bool rn_packet_builder_init( struct rn_packet_builder_t * builder, struct rn_region_t * region, struct rn_window_ack_t * ack, struct rocketnet_send_budget_t * budget, int max_packets_per_update, int payload_capacity );

// RNChannel!WritePacket with RNSendBudget!Send, !Wait and !Drop.
//
// Writes at most max_packets_per_update packets into the caller's payload
// areas (each payload_capacity bytes; the caller owns the frames and passes
// frame + payload_index). Items are taken in the budget's class order --
// retransmits and ordered progress, then new reliable, then unreliable --
// round robin across channels within a class (17.3 item 4), packing several
// small records per packet (9.3). A record that does not fit ends the packet;
// it is never split.
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

// Recovers a packet the transport refused to finish (docs/CHANNELS.md 6.4
// "Failed send"): its numbers keep their assignment and go to the head of the
// resend FIFO, due now, so the next update resends them first.

void rn_packet_builder_packet_failed( struct rn_packet_builder_t * builder, int packet_index, double now );

// After sealing: refunds what the packet did not use, when the transport's
// ack block came in under the bound admitted at write time
// (rocketnet_send_budget_settle). Returns false for a packet index outside the
// last call, or an actual above the admitted, and changes nothing.

bool rn_packet_builder_settle( struct rn_packet_builder_t * builder, int packet_index, int actual_wire_bytes );

// The reliable numbers one written packet carried, and their count. NULL for
// an index outside the last call.

const uint64_t * rn_packet_builder_numbers( const struct rn_packet_builder_t * builder, int packet_index, uint32_t * out_count );

#endif // #ifndef ROCKETNET_PACKET_BUILDER_H
