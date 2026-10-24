/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_MESSAGE_OBJECT_H
#define ROCKETNET_MESSAGE_OBJECT_H

#include <stdint.h>

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_slots.h"

// Layers 7 and 8 of the message layer (docs/CHANNELS.md 10 and 11), condensed
// to what the C API in sdk/rocketnet.h needs: the client and the server message
// objects over the transports. tla/RNSlots.tla is the spec of the object's
// update, release and slot rules; tla/RNConnectionMemory.tla of exhaustion and
// reuse; each function in sdk/rocketnet_message_client.cpp and
// sdk/rocketnet_message_server.cpp names the action it implements (R7). Each
// connection's own message state is its endpoint (layer 7, CHANNELS.md 10.1,
// sdk/rocketnet_endpoint.h), which the object calls for everything per
// connection; the object keeps the transport link, the slots and the update
// order.
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

    // Publish ACK eligibility only after successful message processing.
    void (*accept_payload)( void * transport, int k );

    // the transport's acked packet ids of the last update (R1: the only ack).
    // The client's link leaves *client_index NULL.
    int (*acked)( void * transport, const int ** client_index, const uint64_t ** packet_ids );

    // the connection entry k of the update's payload batch (payload_connection)
    // or acked batch (acked_connection) was received for (P9b), which
    // rn_message_admit hands to rn_slots_admit. The server's link reads
    // rocketnet_server_transport_payload_connection_ids and
    // rocketnet_server_transport_acked_connection_ids; the client's link
    // answers 1, the id of its one connection, for every entry. NULL (the
    // unit suite's untagged fakes): rn_message_admit tags the entry itself.
    uint64_t (*payload_connection)( void * transport, int k );
    uint64_t (*acked_connection)( void * transport, int k );

    // The configured payload limit, after transport defaults. A large
    // unreliable record is refused before enqueue if it cannot fit an arena.
    int (*max_payload_bytes)( void * transport );
    uint8_t * (*start_packet)( void * transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context );
    int (*finish_packet)( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes );
    void (*abort_packet)( void * transport, struct rocketnet_packet_context_t * context );
    void (*send_packets)( void * transport );

    // How many payload frames the transport may still start this update.
    // NULL (the client's link, the unit suite's fakes): no transport-side
    // cap. The server's link reads it so a rotation advances only over
    // clients that had a chance to send.
    int (*send_capacity_available)( void * transport );
    // Connected clients whose next send round must remain available.
    int (*send_connected_count)( void * transport );

    // the connection in the slot now: a nonzero id, or 0 when none (RNSlots
    // Observe). The server's link answers
    // rocketnet_server_transport_client_connection_id, which differs for every
    // connection a slot holds (P9a). The client's link answers 1 while it is
    // CONNECTED.
    uint64_t (*connection)( void * transport, int client_index );

    void (*disconnect)( void * transport, int client_index );

    // the transport's state, ROCKETNET_CLIENT_TRANSPORT_STATE_* or
    // ROCKETNET_SERVER_TRANSPORT_*
    int (*state)( void * transport );

    // the reason the client's transport ended its connection with, a
    // ROCKETNET_DISCONNECT_REASON_*, read once the transport is terminal.
    // NULL (the server's link, the unit suite's): the transport names none.
    int (*end_reason)( void * transport );

    // NULL: the object does not own the transport
    void (*stop)( void * transport );
    bool (*stopped)( const void * transport );
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

// The small slot record is scanned on every server update and send pass. Keep
// it contiguous, apart from the endpoint's channel/builder/receipt payload.
#define RN_MESSAGE_CONNECTION_HOT_LAYOUT_FIELDS( X ) \
    X( connection_id, uint64_t connection_id ) \
    X( views, uint8_t * views ) \
    X( cursors, uint32_t * cursors ) \
    X( held, uint32_t held ) \
    X( state, int state )

struct rn_message_connection_hot_t
{
#define RN_MESSAGE_CONNECTION_HOT_DECLARE( name, declaration ) declaration;
    RN_MESSAGE_CONNECTION_HOT_LAYOUT_FIELDS( RN_MESSAGE_CONNECTION_HOT_DECLARE )
#undef RN_MESSAGE_CONNECTION_HOT_DECLARE
};

static_assert( sizeof( rn_message_connection_hot_t ) <= 32, "message hot slot must stay compact" );

// The cold connection owns the endpoint (CHANNELS.md 10.1), its channels,
// budget, queued sends and write pass. The pointer binds its hot slot for
// receive/release helpers that only have a connection address.

/*
    The declaration manifest: the struct is declared from this list, so a
    member exists only as an entry here. The RESET/KEEP inventory in
    rocketnet_connection_reset.h is compared with it by name, and a member with
    no decision is named in test_connection_reset_inventory_covers_every_field.
*/
// One open outgoing packet per connection. Only headers are written when a
// message finishes; its bytes already occupy their final position in the frame.
#define RN_MESSAGE_RECEIPT_SERIALS_MAX 16 // same bound as the endpoint's receipt_serials

struct rn_message_packet_build_t
{
    rocketnet_packet_context_t context;
    rocketnet_message_writer_t writer;
    uint32_t receipt_serials[RN_MESSAGE_RECEIPT_SERIALS_MAX]; // same bound as the endpoint's receipt_serials
    uint32_t receipt_count;
};

#define RN_MESSAGE_CONNECTION_LAYOUT_FIELDS( X ) \
    X( unreliable_build, rn_message_packet_build_t unreliable_build ) \
    X( hot, rn_message_connection_hot_t * hot ) /* stable parallel-array slot */                                         \
    X( disconnect_reason, int disconnect_reason ) /* ROCKETNET_DISCONNECT_REASON_* of the last connection that ended here */

struct rn_message_connection_t : rn_endpoint_t
{
#define RN_MESSAGE_CONNECTION_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    RN_MESSAGE_CONNECTION_LAYOUT_FIELDS( RN_MESSAGE_CONNECTION_LAYOUT_FIELDS_DECLARE )
#undef RN_MESSAGE_CONNECTION_LAYOUT_FIELDS_DECLARE
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
    rn_message_connection_t * connections;      // cold endpoints, indexed like hot_connections
    rn_message_connection_hot_t * hot_connections;
    struct rn_slots_t slots;                    // the slots machine (CHANNELS.md 10.2)
    int32_t default_kbps;                       // a new connection's send budget; 0 or lower uncapped
    uint32_t unreliable_limit;                  // the largest UNRELIABLE message: one record in one packet
    double time;                                // the last update's time
    double raw_time;                            // the caller's clock, last seen, beside the clamped one
    double time_offset;                         // the backwards offset, exactly 0.0 until the first step
    double clock_origin;                        // the game's time the transport's 0.0 stands for
    int clock_started;                          // 1 from the first update on
    int pending_connection;                     // the connection with a start pending, or -1

    // the one receive queue, pumped by each receive: the update's deliveries,
    // connection by connection in index order, each connection's in its
    // delivery order. receive_connection is the connection the queue is at;
    // its cursors[0] is the next delivery. The batch form hands out up to
    // ROCKETNET_RECEIVE_BATCH_MESSAGES of the same queue per call, released
    // together at the next batch call or update.
    int receive_connection;
    int batch_count;                            // the entries the last receive handed out
    int32_t batch_connection[ROCKETNET_RECEIVE_BATCH_MESSAGES];
    uint32_t batch_entry[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    // The send pass's rotation: the first connection to offer a frame next
    // update. A cap below the client count then serves every client in turn
    // instead of always serving the low indices.
    int send_next_connection;

    // the in-place unreliable send (docs/OUTGOING-BUILD.md, tla/RNOutgoingBuild.tla):
    // the pending start is written in this transport frame
    int in_place;                               // pending start: 0 region, 1 whole frame, 2 appended record
    struct rocketnet_packet_context_t in_place_context;
    uint8_t * in_place_payload;                 // the frame's payload root
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
                                                       int32_t default_kbps, double time, bool reserve_unlimited = false );

void rn_message_object_destroy( struct rn_message_object_t * object );

// The update, RNSlots BeginUpdate to EndUpdate. ROCKETNET_OK, or
// ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED when a received message was held.
int rn_message_object_update( struct rn_message_object_t * object, double time );

void rn_message_object_send_packets( struct rn_message_object_t * object );

uint8_t * rn_message_object_start( struct rn_message_object_t * object, int index, int channel, int reliability, int bytes );
uint32_t rn_message_object_finish( struct rn_message_object_t * object, int index, int bytes );

// The room query: true when rn_message_object_start with these arguments, now,
// would be given its room; false when it would be dropped or refused, or would
// find no room and end the connection (R5). Pure, O(1), allocates nothing.
bool rn_message_object_send_room( const struct rn_message_object_t * object, int index, int channel, int reliability, int bytes );
void rn_message_object_abort( struct rn_message_object_t * object, int index );
uint32_t rn_message_object_send( struct rn_message_object_t * object, int index, int channel, int reliability, const uint8_t * data, int bytes );

// The one receive queue, pumped by each receive: 1 with the next delivery of the
// update, its connection in *index and the channel it was delivered on in
// *channel (either NULL: not reported), 0 when the queue is empty. Release
// finds the held view in whichever connection holds it.
int rn_message_object_receive( struct rn_message_object_t * object, int * index, int * channel, const uint8_t ** data, int * bytes );
void rn_message_object_release( struct rn_message_object_t * object, const uint8_t * data );

// The batch form over the same queue: releases the last batch, then fills
// out with up to ROCKETNET_RECEIVE_BATCH_MESSAGES next deliveries in queue
// order and returns how many (out->num_messages too); 0 when none is pending.
int rn_message_object_receive_messages( struct rn_message_object_t * object, struct rocketnet_message_batch_t * out );

bool rn_message_object_set_send_kbps( struct rn_message_object_t * object, int index, int32_t kbps );
int rn_message_object_disconnect_reason( const struct rn_message_object_t * object, int index );
int rn_message_object_connected( const struct rn_message_object_t * object, int index );

// R5: ends a connection with `reason`. The transport disconnects it now; its
// region is reset at the start of the next update, before the slot can be
// reused (RNConnectionMemory Kick, then Reuse).
void rn_message_object_kick( struct rn_message_object_t * object, int index, int reason );

// The connection's state, for the unit suite.
struct rn_message_connection_t * rn_message_object_connection( struct rn_message_object_t * object, int index );

#if ROCKETNET_DEV
// The message half of the delivery capture (rocketnet_layers.h) for one
// connection: the ordered path's window, resend FIFOs, RTT estimate and send
// budget. A connection that is not CONNECTED leaves the capture untouched.
void rn_message_object_delivery_capture( struct rn_message_object_t * object, int index, struct rocketnet_delivery_capture_t * capture );
#endif // #if ROCKETNET_DEV

// The client and server links over the real transports.
void rn_message_client_link( struct rn_message_link_t * link, struct rocketnet_client_transport_t * transport );
void rn_message_server_link( struct rn_message_link_t * link, struct rocketnet_server_transport_t * transport );

#endif // #ifndef ROCKETNET_MESSAGE_OBJECT_H
