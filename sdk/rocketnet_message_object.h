/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message objects (docs/CHANNELS.md 10 to 12) and their C API. This header
// is the shared implementation core of the client and server objects: one
// connection's message state (the ordered channel over its region and budget)
// plus the small amount of per-connection bookkeeping the transport and the
// receive/release contract need. The objects wrap the transports; the
// in-memory unit suite drives a pair of these directly.

#pragma once

#ifndef ROCKETNET_MESSAGE_OBJECT_H
#define ROCKETNET_MESSAGE_OBJECT_H

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_unreliable.h"

#include <stdint.h>
#include <string.h>

// One frame the builder wrote, to hand to the peer: the payload the transport
// frame carries, plus the transport's packet id.
struct rn_message_frame_t
{
    uint64_t id;
    int bytes;
    uint8_t data[ROCKETNET_MAX_PAYLOAD_BYTES];
};

// The receive-hand contract: a delivered message, a view valid until the next
// update, released by the application before then.
struct rn_message_delivery_t
{
    const uint8_t * data;
    int32_t bytes;
    int channel;
    uint8_t released;       // the application gave it back
    uint8_t consumed;       // handed to the application by receive()
};

// One connection's message state: the ordered channel over its region and
// budget, plus the receive/release bookkeeping and the transport's packet id.
struct rn_message_node_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    rocketnet_send_budget_t budget;
    int budgeted;
    uint64_t next_packet_id;
    int pending_unreliable;             // a start() took an unreliable store chunk, not a region descriptor
    uint32_t upending_chunk;
    uint32_t upending_bytes;
    int upending_channel;
    double time;
    int disconnect_reason;              // 0 = connected, else MESSAGE_MEMORY_EXHAUSTED etc.

    rn_message_delivery_t deliveries[ROCKETNET_MAX_CHANNELS * 2 + 64];
    uint32_t deliveries_count;
    uint32_t deliveries_read;

    // The frames the connection wrote this update, in the connection's own
    // memory: a peer's delivery view points into them and stays valid until
    // the next update (11.1).
    rn_message_frame_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
};

static inline void rn_message_node_init_deliveries( struct rn_message_node_t * n )
{
    for ( uint32_t i = 0; i < n->deliveries_count; i++ )
        n->deliveries[i].released = n->deliveries[i].consumed = 0;
    n->deliveries_count = 0;
    n->deliveries_read = 0;
}

static inline void rn_message_node_release( struct rn_message_node_t * n, const uint8_t * data )
{
    if ( data == NULL )
        return;
    for ( uint32_t i = 0; i < n->deliveries_count; i++ )
        if ( n->deliveries[i].data == data && !n->deliveries[i].released )
        {
            n->deliveries[i].released = 1;
            return;
        }
}

static inline int rn_message_node_any_unreleased( const struct rn_message_node_t * n )
{
    for ( uint32_t i = 0; i < n->deliveries_count; i++ )
        if ( n->deliveries[i].consumed && !n->deliveries[i].released )
            return 1;
    return 0;
}

// Setup the ordered channel over the connection's region with the given
// message config (NULL: defaults) and optional budget.
static inline bool rn_message_node_ordered_setup( struct rn_message_node_t * n,
                                                  const struct rocketnet_message_config_t * config,
                                                  int max_packets,
                                                  int budgeted, uint32_t kbps, double now )
{
    if ( budgeted && !rocketnet_send_budget_init( &n->budget, kbps, now ) )
        return false;
    return rn_ordered_init( &n->ordered, n->region, config, budgeted ? &n->budget : NULL,
                            max_packets, ROCKETNET_MESSAGE_SINGLE_BYTES );
}

// Write one update's packets into frames, hand them the transport's ids and
// commit. Returns the number of packets written.
static inline int rn_message_node_write_packets( struct rn_message_node_t * n, double now, int max_out, int settled )
{
    uint8_t * pointers[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    const int limit = max_out < ROCKETNET_PACKET_BUILDER_MAX_PACKETS ? max_out : ROCKETNET_PACKET_BUILDER_MAX_PACKETS;
    uint8_t raw[ROCKETNET_PACKET_BUILDER_MAX_PACKETS * ROCKETNET_MAX_PAYLOAD_BYTES];
    for ( int i = 0; i < limit; i++ )
        pointers[i] = raw + (size_t) i * ROCKETNET_MAX_PAYLOAD_BYTES;
    const int count = rn_ordered_write( &n->ordered, now, pointers, limit );
    for ( int p = 0; p < count; p++ )
    {
        n->frames[p].id = n->next_packet_id++;
        n->frames[p].bytes = (int) n->ordered.builder.packet[p].payload_bytes;
        memcpy( n->frames[p].data, pointers[p], (size_t) n->frames[p].bytes );
        rn_ordered_commit( &n->ordered, p, n->frames[p].id, now );
        if ( settled && n->ordered.budget )
            rn_ordered_settle( &n->ordered, p, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, n->frames[p].bytes ) );
    }
    return count;
}

// Open one received frame (the transport hands it once), drain its deliveries
// into the node's receive list. Returns 0 on a replay.
static inline int rn_message_node_open_frame( struct rn_message_node_t * n,
                                              const struct rn_message_frame_t * frame )
{
    const int violation = rn_ordered_receive( &n->ordered, frame->data, frame->bytes );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return -1;
    const uint32_t count = rn_ordered_deliveries( &n->ordered );
    for ( uint32_t i = 0; i < count && n->deliveries_count < (uint32_t) ( sizeof( n->deliveries ) / sizeof( n->deliveries[0] ) ); i++ )
    {
        const rn_delivery_t & d = n->region->deliveries[i];
        rn_message_delivery_t & got = n->deliveries[n->deliveries_count++];
        got.data = d.data;
        got.bytes = d.bytes;
        got.channel = d.channel;
        got.released = 0;
    }
    rn_ordered_end_update( &n->ordered );
    return 0;
}

// The connection's decision each update: exhaustion first (R5 kicks), then a
// wire violation.
static inline void rn_message_node_decide( struct rn_message_node_t * n )
{
    if ( n->disconnect_reason != 0 )
        return;
    const int decision = rn_ordered_decide( &n->ordered );
    if ( decision != ROCKETNET_MESSAGE_ERROR_NONE )
    {
        if ( n->region->exhausted )
            n->disconnect_reason = MESSAGE_MEMORY_EXHAUSTED;
        else
            n->disconnect_reason = decision;
    }
}

// The client and server message objects. In the in-memory unit suite the
// object pair is wired to each other directly (the transport's sends land on
// the peer's receive), so each object holds a peer pointer. On a real
// transport the send path hands frames to the transport instead.

struct rocketnet_message_client_t
{
    rn_message_node_t node;
    struct rocketnet_message_server_t * peer;
    int connected;
};

// The server's slot count for the in-memory unit pair. A real server sizes
// slots from its config's max_clients; the unit suite uses one client.
#ifndef RN_MESSAGE_SERVER_MAX_SLOTS
#define RN_MESSAGE_SERVER_MAX_SLOTS 16
#endif

struct rocketnet_message_server_t
{
    void * context;
    rn_message_node_t slots[RN_MESSAGE_SERVER_MAX_SLOTS];
    int used[RN_MESSAGE_SERVER_MAX_SLOTS];
    struct rocketnet_message_client_t * peer[RN_MESSAGE_SERVER_MAX_SLOTS];
    int connected_count;
    struct rocketnet_message_config_t cfg;     // the slot config, for lazy slot bring-up
    uint32_t kbps;                            // the per-client send budget
};

#endif // #ifndef ROCKETNET_MESSAGE_OBJECT_H
