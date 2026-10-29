/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SEND_BUDGET_H
#define ROCKETNET_SDK_ROCKETNET_SEND_BUDGET_H

// The per-connection send budget: each connection (the client's one, each
// server client slot) may send at most so many kilobits per second, counted
// in the transport's wire bytes. The semantics are proofs/tla/RNSendBudget.tla;
// each function below names the action it implements.
//
// The unit. 1 kbps = 1000 bit/s, the unit of the transport's own
// bandwidth_sent_kbps (sdk/rocketnet_ack.cpp, update_stats), so the budget
// and the transport's measured rate agree. A budget of K kbps is K * 125
// bytes per second.
//
// The bytes counted. A packet's wire bytes as the transport records them in
// rocketnet_ack_endpoint_record_sent: the sealed payload packet, prefilter
// header, sequence, ack block, payload and tag included, IP and UDP headers
// excluded. rocketnet_write_sealed_payload_in_place (sdk/rocketnet_protocol.cpp)
// returns ( p - wire_start ) + sealed_bytes, which is
//
//     ROCKETNET_PACKET_OVERHEAD_BYTES + ack_block_bytes + payload_bytes
//
// (the overhead is the 6-byte prefilter header, the 4-byte sequence and the
// 16-byte tag, sdk/rocketnet_protocol.h), and the client and server seal paths
// pass that return value to record_sent as wire_bytes.
// rocketnet_send_budget_wire_bytes is that formula, from the same macros.
//
// The rate. A token bucket in bytes, refilled with rate x elapsed time at
// each update, time passed as an argument, so the rule does not depend on
// the update rate. The bucket's depth is the larger of one maximum packet
// (ROCKETNET_SEND_BUDGET wire bytes, the most any packet carries) and 100 ms
// of budget: at least one packet, so every packet fits an idle bucket and a
// reliable item never waits forever at a low rate; at most 100 ms, so an
// update that stalls releases no more than a tenth of a second of budget at
// once. Bytes sent in any window of one second are at most the budget plus
// the depth (RNSendBudget!NeverOverspend).
//
// The class order. The packet writer offers items in this order within an
// update: retransmits and ordered progress (a chunk or small message that
// advances the oldest unacked ordered number), then new reliable items, then
// unreliable items. A reliable item that does not fit waits for a later
// update and is never dropped; once one waits, nothing after it in this
// update spends the bucket. An unreliable item that does not fit the bytes
// left to its class is dropped and counted.
//
// The uncapped case. A value of 0 or lower at the message layer means that
// connection's budget does not apply: the endpoint passes no budget to its
// packet builders (a NULL budget) and skips the bucket on its large
// sequenced path, so nothing is dropped or delayed by the budget. Uncapping
// is NOT RECOMMENDED (RFC 2119): without a budget there is no defined
// behaviour when a sender outruns its path or its receiver. The struct below and
// its functions keep their rules for a capped connection: a budget of 1 kbps
// to ROCKETNET_SEND_BUDGET_MAX_KBPS, and 0 is not a valid capped rate.
//
// Header only and allocation free: a plain struct the owner embeds in its
// connection state (owner rule R2).

#include <stdint.h>
#include <string.h>

#include "rocketnet_protocol.h"     // ROCKETNET_PACKET_OVERHEAD_BYTES
#include "rocketnet_wire.h"         // ROCKETNET_SEND_BUDGET, ROCKETNET_ACK_BLOCK_MIN_BYTES, ROCKETNET_ACK_BLOCK_MAX_BYTES

// the defaults of the message config fields client_send_kbps and
// server_send_kbps (sdk/rocketnet.h), not limits: only the config table reads them
#ifndef ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS
#define ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS                  1000        // client to server
#endif // #ifndef ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS
#ifndef ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS
#define ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS       10000       // server to each client slot
#endif // #ifndef ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS

#define ROCKETNET_SEND_BUDGET_MIN_KBPS                      1
// the largest capped budget: every int below ROCKETNET_SEND_KBPS_UNLIMITED,
// which the public layer passes down as 0, uncapped
#define ROCKETNET_SEND_BUDGET_MAX_KBPS                      ( 0x7fffffff - 1 )
#define ROCKETNET_SEND_BUDGET_DEPTH_SECONDS                 0.1         // the bucket holds 100 ms of budget...
#define ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES         ROCKETNET_SEND_BUDGET   // ...and never less than one maximum packet

// the send classes, in the order the writer offers them; the first two share
// the first rank

#define ROCKETNET_SEND_CLASS_RETRANSMIT                     0
#define ROCKETNET_SEND_CLASS_ORDERED_PROGRESS               1
#define ROCKETNET_SEND_CLASS_NEW_RELIABLE                   2
#define ROCKETNET_SEND_CLASS_UNRELIABLE                     3

// what rocketnet_send_budget_admit decides

#define ROCKETNET_SEND_BUDGET_SEND                          0           // spent: write it
#define ROCKETNET_SEND_BUDGET_WAIT                          1           // reliable, does not fit: keep it for a later update
#define ROCKETNET_SEND_BUDGET_DROP                          2           // unreliable, does not fit: dropped and counted
#define ROCKETNET_SEND_BUDGET_REFUSED                       3           // a bad argument: nothing spent, counted

struct rocketnet_send_budget_t
{
    uint32_t kbps;                          // the budget, kilobits per second
    double bytes_per_second;                // kbps * 125
    double depth;                           // the bucket's capacity, bytes
    double tokens;                          // bytes that may be sent now, 0 <= tokens <= depth
    double last_time;                       // the time of the last refill
    int rank_reached;                       // this update: the lowest-priority rank offered so far
    int reliable_waiting;                   // this update: a reliable item did not fit and waits

    uint64_t bytes_spent;                   // wire bytes admitted, net of settled refunds
    uint64_t unreliable_dropped;            // unreliable items dropped over budget
    uint64_t unreliable_dropped_bytes;      // their wire bytes
    uint64_t order_refusals;                // items offered after a lower-priority class in one update
    uint64_t refusals;                      // bad arguments
};

// a capped budget is valid from 1 kbps to ROCKETNET_SEND_BUDGET_MAX_KBPS; 0 is
// refused here and means uncapped at the message layer, which holds no bucket

// proofs/tla/RNSendBudget.tla Init
inline bool rocketnet_send_budget_kbps_valid( uint32_t kbps )
{
    return kbps >= ROCKETNET_SEND_BUDGET_MIN_KBPS && kbps <= ROCKETNET_SEND_BUDGET_MAX_KBPS;
}

inline double rocketnet_send_budget_bytes_per_second( uint32_t kbps )
{
    return (double) kbps * 1000.0 / 8.0;
}

inline double rocketnet_send_budget_depth_bytes( uint32_t kbps )
{
    const double window = rocketnet_send_budget_bytes_per_second( kbps ) * ROCKETNET_SEND_BUDGET_DEPTH_SECONDS;
    const double packet = (double) ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES;
    return window > packet ? window : packet;
}

// the wire bytes the transport records for one sealed payload packet: the
// formula rocketnet_write_sealed_payload_in_place returns, from the macros it
// uses. 0 for a block or payload length the seal path refuses.

#if ROCKETNET_DEV
inline int rocketnet_send_budget_wire_bytes( int ack_block_bytes, int payload_bytes )
{
    if ( ack_block_bytes < ROCKETNET_ACK_BLOCK_MIN_BYTES || ack_block_bytes > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        return 0;
    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ack_block_bytes )
        return 0;
    return ROCKETNET_PACKET_OVERHEAD_BYTES + ack_block_bytes + payload_bytes;
}
#endif // #if ROCKETNET_DEV

// the most wire bytes a packet of this payload can take once the transport
// adds its ack block: the seal paths cap the block at the room the send budget
// leaves (rocketnet_client_protocol_seal_payload_frame and the server's), so
// this is the bound to admit before the block is known; settle refunds the
// difference after sealing. 0 for a payload the seal path refuses.

inline int rocketnet_send_budget_max_wire_bytes( int payload_bytes )
{
    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES )
        return 0;
    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - payload_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;
    return ROCKETNET_PACKET_OVERHEAD_BYTES + block_capacity + payload_bytes;
}

// the rank a class is offered at: 0 retransmit and ordered progress, 1 new
// reliable, 2 unreliable; -1 for an unknown class

inline int rocketnet_send_budget_class_rank( int send_class )
{
    switch ( send_class )
    {
        case ROCKETNET_SEND_CLASS_RETRANSMIT:
        case ROCKETNET_SEND_CLASS_ORDERED_PROGRESS:
            return 0;
        case ROCKETNET_SEND_CLASS_NEW_RELIABLE:
            return 1;
        case ROCKETNET_SEND_CLASS_UNRELIABLE:
            return 2;
        default:
            return -1;
    }
}

inline bool rocketnet_send_budget_time_valid( double now )
{
    return now == now && now > -1.0e300 && now < 1.0e300;      // finite, without <math.h>
}

// RNSendBudget!Init: a full bucket. Refuses 0, anything above the maximum and
// a non-finite time, leaving the budget zeroed (it then admits nothing).

// proofs/tla/RNSendBudget.tla Init
inline bool rocketnet_send_budget_init( struct rocketnet_send_budget_t * budget, uint32_t kbps, double now )
{
    if ( !budget )
        return false;
    memset( budget, 0, sizeof( *budget ) );
    if ( !rocketnet_send_budget_kbps_valid( kbps ) || !rocketnet_send_budget_time_valid( now ) )
        return false;
    budget->kbps = kbps;
    budget->bytes_per_second = rocketnet_send_budget_bytes_per_second( kbps );
    budget->depth = rocketnet_send_budget_depth_bytes( kbps );
    budget->tokens = budget->depth;
    budget->last_time = now;

    return true;
}

// RNSendBudget!Tick, the refill: tokens += rate x ( now - last ), capped at the
// depth. A time at or before the last refill adds nothing and does not move
// the clock back.

// proofs/tla/RNSendBudget.tla Tick
inline void rocketnet_send_budget_refill( struct rocketnet_send_budget_t * budget, double now )
{
    if ( !budget || !rocketnet_send_budget_time_valid( now ) || now <= budget->last_time )
        return;
    const double elapsed = now - budget->last_time;
    budget->last_time = now;

    double tokens = budget->tokens + budget->bytes_per_second * elapsed;
    if ( tokens > budget->depth )
        tokens = budget->depth;
    budget->tokens = tokens;
}

// RNSendBudget!Tick with run: the refill, then a new send phase: the class
// order and the reliable wait start over.

// proofs/tla/RNSendBudget.tla Tick
inline void rocketnet_send_budget_begin_update( struct rocketnet_send_budget_t * budget, double now )
{
    if ( !budget )
        return;
    rocketnet_send_budget_refill( budget, now );
    budget->rank_reached = 0;
    budget->reliable_waiting = 0;
}

// The runtime setter: refill at the old rate up to now, then the new rate and
// depth hold from now on, so the change takes effect at the next refill. The
// bucket keeps its bytes, capped at the new depth. Refuses an invalid budget
// or time and changes nothing.

inline bool rocketnet_send_budget_set_kbps( struct rocketnet_send_budget_t * budget, uint32_t kbps, double now )
{
    if ( !budget || budget->kbps == 0 || !rocketnet_send_budget_kbps_valid( kbps ) || !rocketnet_send_budget_time_valid( now ) )
    {
        if ( budget )
            budget->refusals++;
        return false;
    }

    rocketnet_send_budget_refill( budget, now );
    budget->kbps = kbps;
    budget->bytes_per_second = rocketnet_send_budget_bytes_per_second( kbps );
    budget->depth = rocketnet_send_budget_depth_bytes( kbps );
    if ( budget->tokens > budget->depth )
        budget->tokens = budget->depth;
    return true;
}

// RNSendBudget!Fits: the guard every spend passes

// proofs/tla/RNSendBudget.tla Fits
inline bool rocketnet_send_budget_can_send( const struct rocketnet_send_budget_t * budget, int wire_bytes )
{
    return budget && wire_bytes >= 1 && wire_bytes <= ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES &&
           (double) wire_bytes <= budget->tokens;
}

// RNSendBudget!Send's tokens' = tokens - s: spends only what fits (the check is
// the rule; spending without it is RNSendBudget's BrokenOverspend witness).

// proofs/tla/RNSendBudget.tla Send
inline bool rocketnet_send_budget_spend( struct rocketnet_send_budget_t * budget, int wire_bytes )
{
    if ( !rocketnet_send_budget_can_send( budget, wire_bytes ) )
        return false;
    budget->tokens -= (double) wire_bytes;
    budget->bytes_spent += (uint64_t) wire_bytes;

    return true;
}

// The writer's question for each item, in class order within one update:
// RNSendBudget!Send when it fits; for a reliable item that does not fit,
// RNSendBudget!EndUpdate's guard (it waits, and so does every later item this
// update); for an unreliable item that does not fit the bytes left to its
// class (none while a reliable item waits), RNSendBudget!Drop. An item
// offered after a lower-priority class in the same update breaks the order:
// it is counted, and a reliable one waits rather than spend the bytes the
// earlier class was promised.

// proofs/tla/RNSendBudget.tla Send, Drop and EndUpdate
inline int rocketnet_send_budget_admit( struct rocketnet_send_budget_t * budget, int send_class, int wire_bytes )
{
    const int rank = rocketnet_send_budget_class_rank( send_class );
    if ( !budget || budget->kbps == 0 || rank < 0 || wire_bytes < 1 || wire_bytes > ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES )
    {
        if ( budget )
            budget->refusals++;
        return ROCKETNET_SEND_BUDGET_REFUSED;
    }

    if ( rank < budget->rank_reached )
    {
        budget->order_refusals++;
        return ROCKETNET_SEND_BUDGET_WAIT;                 // only the reliable ranks sit above another
    }

    budget->rank_reached = rank;

    if ( send_class != ROCKETNET_SEND_CLASS_UNRELIABLE )
    {
        if ( budget->reliable_waiting || !rocketnet_send_budget_spend( budget, wire_bytes ) )
        {
            budget->reliable_waiting = 1;
            return ROCKETNET_SEND_BUDGET_WAIT;
        }

        return ROCKETNET_SEND_BUDGET_SEND;
    }

    if ( budget->reliable_waiting || !rocketnet_send_budget_spend( budget, wire_bytes ) )
    {
        budget->unreliable_dropped++;
        budget->unreliable_dropped_bytes += (uint64_t) wire_bytes;
        return ROCKETNET_SEND_BUDGET_DROP;
    }

    return ROCKETNET_SEND_BUDGET_SEND;
}

// After sealing: an item admitted at rocketnet_send_budget_max_wire_bytes gets
// back what the packet did not use. actual above admitted is refused (the
// bucket is never charged after the fact) and counted.

inline bool rocketnet_send_budget_settle( struct rocketnet_send_budget_t * budget, int admitted_wire_bytes, int actual_wire_bytes )
{
    if ( !budget || actual_wire_bytes < 0 || actual_wire_bytes > admitted_wire_bytes ||
         (uint64_t) admitted_wire_bytes > budget->bytes_spent )
    {
        if ( budget )
            budget->refusals++;
        return false;
    }

    const int refund = admitted_wire_bytes - actual_wire_bytes;
    double tokens = budget->tokens + (double) refund;
    if ( tokens > budget->depth )
        tokens = budget->depth;
    budget->tokens = tokens;
    budget->bytes_spent -= (uint64_t) refund;

    return true;
}

#endif // ROCKETNET_SDK_ROCKETNET_SEND_BUDGET_H
