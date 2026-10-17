/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_MESSAGE_OBJECT_H
#define ROCKETNET_MESSAGE_OBJECT_H

#include <stdint.h>

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_slots.h"

// Layers 7 and 8 of the message layer (docs/CHANNELS.md 10 and 11), condensed
// to what the C API in sdk/rocketnet.h needs: the client and the server message
// objects over the transports. tla/RNSlots.tla is the spec of the object's
// update, release and slot rules; tla/RNConnectionMemory.tla of exhaustion and
// reuse; each function in sdk/rocketnet_message_client.cpp and
// sdk/rocketnet_message_server.cpp names the action it implements (R7).
//
// One object is one block made at create (owner rule R2): the object, its
// connections, each connection's receive bookkeeping, and the arena of regions
// (one per connection: the client's one, one per server slot). Nothing is
// allocated after create. A connection's whole message state -- the ordered
// path of layers 3 to 6 and the queued unreliable sends -- lives in its region.
//
// The transport is reached through one seam, rn_message_link_t: a table of the
// transport calls the object makes. The public create functions bind it to a
// rocketnet_client_transport_t or a rocketnet_server_transport_t. The unit
// suite binds it to an in-memory transport pair, so the same object code runs
// with no socket and no clock.

// ---------------------------------------------------------------
//                       the transport seam
// ---------------------------------------------------------------

struct rn_message_link_t
{
    void * transport;

    void (*update)( void * transport, double time );

    // the update's payload batch, valid until the next update. The client's
    // link leaves *client_index NULL.
    int (*payloads)( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes );

    // the transport's acked packet ids of the last update (R1: the only ack).
    // The client's link leaves *client_index NULL.
    int (*acked)( void * transport, const int ** client_index, const uint64_t ** packet_ids );

    // The configured payload limit, after transport defaults. A large
    // unreliable record is refused before enqueue if it cannot fit an arena.
    int (*max_payload_bytes)( void * transport );
    uint8_t * (*start_packet)( void * transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context );
    int (*finish_packet)( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes );
    void (*abort_packet)( void * transport, struct rocketnet_packet_context_t * context );
    void (*send_packets)( void * transport );

    // the connection in the slot now: a nonzero id, or 0 when none (RNSlots
    // Observe). The client's link answers 1 while it is CONNECTED.
    uint64_t (*connection)( void * transport, int client_index );

    void (*disconnect)( void * transport, int client_index );

    // the transport's state, ROCKETNET_CLIENT_TRANSPORT_STATE_* or
    // ROCKETNET_SERVER_TRANSPORT_*
    int (*state)( void * transport );

    // NULL: the object does not own the transport
    void (*destroy)( void * transport );

    // 1 for a transport whose clock was seeded at 0.0 at create (the client's,
    // whose create takes no time): its clock starts at the object's first
    // update, and each update hands it time - the first update's time. 0 for a
    // transport that reads the game's time as it is (the server's, the unit
    // suite's).
    int clock_from_first_update;
};

// ---------------------------------------------------------------
//                       one connection
// ---------------------------------------------------------------

#define RN_MESSAGE_CONNECTION_FREE          0   // no connection bound (RNSlots Free)
#define RN_MESSAGE_CONNECTION_CONNECTED     1   // bound to connection_id
#define RN_MESSAGE_CONNECTION_KICKED        2   // disconnected by the object, its region reset at the next update

// a delivery's state in the receive bookkeeping
#define RN_MESSAGE_VIEW_WAITING             0   // delivered, not yet handed to the application
#define RN_MESSAGE_VIEW_HELD                1   // handed out by receive, not yet released
#define RN_MESSAGE_VIEW_RELEASED            2   // released, or refused because the connection ended

struct rn_message_connection_t
{
    rn_region_t * region;
    rn_ordered_t ordered;                       // layers 3 to 6, RELIABLE_ORDERED, over the region
    rn_packet_builder_t unreliable_builder;     // layer 6 for UNRELIABLE records (no reliable numbers)
    rocketnet_send_budget_t budget;             // R6

    uint64_t connection_id;                     // RNSlots: the connection this slot is bound to, 0 when free
    int state;                                  // RN_MESSAGE_CONNECTION_*
    int disconnect_reason;                      // ROCKETNET_MESSAGE_ERROR_* of the last connection that ended here
    uint32_t serial;                            // the receipt serial of the last finished send (7.4)
    double config_wait_start;                   // when this end began waiting for the peer's CONFIG (10.3), or -1 before it is connected

    // the start between start and finish (one at a time, like the transport's start_packet)
    int pending_type;                           // -1 when no start is pending
    uint32_t pending_descriptor;                // UNRELIABLE: its send descriptor
    uint32_t pending_bytes;

    // the queued UNRELIABLE sends: send descriptors linked through their `next`
    uint32_t unreliable_head, unreliable_tail;
    uint32_t unreliable_count;

    // the receive bookkeeping (RNSlots Release): one state per delivery entry,
    // one cursor per channel, and the views held now
    uint8_t * views;                            // region->delivery_entries
    uint32_t * cursors;                         // region->num_channels
    uint32_t held;
};

// ---------------------------------------------------------------
//                       the object
// ---------------------------------------------------------------

struct rn_message_object_t
{
    void * context;
    struct rn_message_link_t link;
    int is_server;
    int num_connections;                        // 1 for the client, max_clients for the server
    struct rocketnet_message_config_t config;
    rocketnet_counters_t counters;              // the message counters of every region of this object
    rn_arena_t * arena;
    rn_message_connection_t * connections;
    struct rn_slots_t slots;                    // the slots machine (CHANNELS.md 10.2)
    uint32_t default_kbps;                      // a new connection's send budget
    uint32_t unreliable_limit;                  // the largest UNRELIABLE message: one record in one packet
    double time;                                // the last update's time
    double clock_origin;                        // the game's time the transport's 0.0 stands for
    int clock_started;                          // 1 from the first update on
    int pending_connection;                     // the connection with a start pending, or -1
    void * block;
    int owns_block;
};

struct rocketnet_message_client_t
{
    struct rn_message_object_t object;
};

struct rocketnet_message_server_t
{
    struct rn_message_object_t object;
};

// The block an object of num_connections connections needs at config: the
// object, its connections, their receive bookkeeping and the arena. 0 for a
// refused config or count.
uint64_t rn_message_object_required_bytes( const struct rocketnet_message_config_t * config, int num_connections, int is_server );

// Builds the object on a link, in `memory` (NULL: one rocketnet_malloc). The
// object owns the link's transport when link->destroy is set. NULL, with the
// link's transport destroyed, on a refused argument.
struct rn_message_object_t * rn_message_object_create( void * context, const struct rn_message_link_t * link, int is_server, int num_connections,
                                                       const struct rocketnet_message_config_t * config, void * memory, uint64_t memory_bytes,
                                                       uint32_t default_kbps, double time );

void rn_message_object_destroy( struct rn_message_object_t * object );

// The update, RNSlots BeginUpdate to EndUpdate. ROCKETNET_OK, or
// ROCKETNET_ERROR_MESSAGE_NOT_RELEASED when a received message was held.
int rn_message_object_update( struct rn_message_object_t * object, double time );

void rn_message_object_send_packets( struct rn_message_object_t * object );

uint8_t * rn_message_object_start( struct rn_message_object_t * object, int index, int channel, int reliability, int bytes );
uint32_t rn_message_object_finish( struct rn_message_object_t * object, int index, int bytes );
void rn_message_object_abort( struct rn_message_object_t * object, int index );
uint32_t rn_message_object_send( struct rn_message_object_t * object, int index, int channel, int reliability, const uint8_t * data, int bytes );

int rn_message_object_receive( struct rn_message_object_t * object, int index, int channel, const uint8_t ** data, int * bytes );
void rn_message_object_release( struct rn_message_object_t * object, int index, const uint8_t * data );

bool rn_message_object_set_send_kbps( struct rn_message_object_t * object, int index, uint32_t kbps );
int rn_message_object_disconnect_reason( const struct rn_message_object_t * object, int index );
int rn_message_object_connected( const struct rn_message_object_t * object, int index );

// R5: ends a connection with `reason`. The transport disconnects it now; its
// region is reset at the start of the next update, before the slot can be
// reused (RNConnectionMemory Kick, then Reuse).
void rn_message_object_kick( struct rn_message_object_t * object, int index, int reason );

// The connection's state, for the unit suite.
struct rn_message_connection_t * rn_message_object_connection( struct rn_message_object_t * object, int index );

// The client and server links over the real transports.
void rn_message_client_link( struct rn_message_link_t * link, struct rocketnet_client_transport_t * transport );
void rn_message_server_link( struct rn_message_link_t * link, struct rocketnet_server_transport_t * transport );

#endif // #ifndef ROCKETNET_MESSAGE_OBJECT_H
