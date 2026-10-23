/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server message object (docs/CHANNELS.md 11.1) and the object core it
// shares with the client: the client is the same object with one connection
// (11.3), so the core lives here and sdk/rocketnet_message_client.cpp adds the
// client's link and C API.
//
// The spec is tla/RNSlots.tla (the update, slot observation, admission, kick,
// reset and release) with tla/RNConnectionMemory.tla (exhaustion and reuse);
// each function names the action it implements (R7). The connection's own
// message state is layers 3 to 6 (sdk/rocketnet_channel.h, tla/RNChannel.tla).
//
// Memory (R2): rn_message_object_create makes one block -- or takes the
// caller's -- holding the object, its connections, their receive bookkeeping
// and the arena of regions. The only allocator calls in this file are that
// one allocation, in create, and destroy's matching free.
//
// Copies (R3): a fitting unreliable message is written in place in the packet
// buffer, with no copy between the message layer and the transport: start
// takes the transport frame and hands out a pointer inside it, finish writes
// the header and form byte around it and finishes that frame
// (docs/OUTGOING-BUILD.md, tla/RNOutgoingBuild.tla). Every other message is
// written once by the application, in place, into its region record (start,
// finish). THE COPIES (a): each packet write copies
// the record into the transport frame start_packet returned
// (rocketnet_message_writer_add, inside rn_ordered_write and
// rn_packet_builder_write), because a reliable record must outlive the frame
// for resends and the transport encrypts the frame in place. A message
// delivered in the update it arrives is a view into the transport's batch, and
// an ordered message held ahead of its turn is copied into the receive ring
// (THE COPIES (b), rn_channels_dispatch_ordered). rn_message_object_send is the
// one other copy: the convenience copies the caller's bytes into the record.

#include <string.h>

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_fragment.h"
#include "rocketnet_message.h"
#include "rocketnet_message_object.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_packet_builder.h"
#include "rocketnet_platform.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_slots.h"

// ---------------------------------------------------------------
//                          sizing
// ---------------------------------------------------------------

static inline uint64_t rn_message_align64( uint64_t bytes )
{
    return ( bytes + 63 ) & ~(uint64_t) 63;
}

struct rn_message_object_layout_t
{
    uint64_t hot_connections;       // compact slot records walked by update/send
    uint64_t connections;           // offset of the connection array
    uint64_t views;                 // offset of the first connection's views; each connection's next at + view_stride
    uint64_t view_stride;
    uint64_t cursors;
    uint64_t cursor_stride;
    uint64_t arena;
    uint64_t arena_bytes;
    uint64_t total;
};

static bool rn_message_object_layout( const rocketnet_message_config_t * config, int num_connections, rn_message_object_layout_t * layout )
{
    if ( num_connections < 1 || num_connections > ROCKETNET_MAX_CLIENTS )
        return false;
    if ( !rocketnet_message_validate_config( config ) )
        return false;

    const uint64_t arena_bytes = rn_arena_required_bytes( config, num_connections );
    if ( arena_bytes == 0 )
        return false;

    const uint64_t deliveries = (uint64_t) config->reliable_messages_in_flight + (uint64_t) config->unreliable_messages_per_update;
    const uint64_t n = (uint64_t) num_connections;

    memset( layout, 0, sizeof( *layout ) );
    uint64_t offset = rn_message_align64( sizeof( rn_message_object_t ) > sizeof( rocketnet_message_server_t ) ? sizeof( rn_message_object_t ) : sizeof( rocketnet_message_server_t ) );
    layout->hot_connections = offset;
    offset += rn_message_align64( sizeof( rn_message_connection_hot_t ) * n );
    layout->connections = offset;
    offset += rn_message_align64( sizeof( rn_message_connection_t ) * n );
    layout->views = offset;
    layout->view_stride = rn_message_align64( deliveries );
    offset += layout->view_stride * n;
    layout->cursors = offset;
    layout->cursor_stride = rn_message_align64( sizeof( uint32_t ) * (uint64_t) config->num_channels );
    offset += layout->cursor_stride * n;
    layout->arena = offset;
    layout->arena_bytes = arena_bytes;
    offset += rn_message_align64( arena_bytes );
    layout->total = offset;
    return true;
}

uint64_t rn_message_object_required_bytes( const struct rocketnet_message_config_t * config, int num_connections, int is_server )
{
    rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }
    // Only this direction's resolved send budget sizes its connection region.
    rocketnet_message_config_t sized = *config;
    const int kbps = is_server ? config->server_send_kbps : config->client_send_kbps;
    sized.client_send_kbps = kbps;
    sized.server_send_kbps = kbps;
    rn_message_object_layout_t layout;
    if ( !rn_message_object_layout( &sized, num_connections, &layout ) )
        return 0;
    return layout.total;
}

static void rn_message_count( rn_message_object_t * object, int counter, uint64_t amount = 1 )
{
    rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &object->counters.values[counter], amount );
}

// ---------------------------------------------------------------
//                     one connection's state
// ---------------------------------------------------------------

// RNSlots ResetFree, and RNConnectionMemory Reuse: the region carved again and
// every layer bound to it fresh, so nothing of the previous connection is
// reachable from the slot. Also the first bind at create.


struct rn_message_connection_t * rn_message_object_connection( struct rn_message_object_t * object, int index )
{
    if ( !object || index < 0 || index >= object->num_connections )
        return NULL;
    return &object->connections[index];
}

#if ROCKETNET_DEV
void rn_message_object_delivery_capture( struct rn_message_object_t * object, int index, struct rocketnet_delivery_capture_t * capture )
{
    if ( !object || !capture || index < 0 || index >= object->num_connections )
        return;
    if ( object->hot_connections[index].state != RN_MESSAGE_CONNECTION_CONNECTED )
        return;
    const struct rn_ordered_t * ordered = &object->connections[index].ordered;
    capture->reliable_unacked = ordered->send.next >= ordered->send.U ? ordered->send.next - ordered->send.U : 0;
    capture->reliable_resend_queued = ordered->ack.resend_count;
    capture->reliable_oldest_unacked_age_seconds = capture->reliable_unacked > 0 && ordered->ack.has_oldest ?
                                                   object->time - ordered->ack.oldest_first_send : -1.0;
    capture->srtt_seconds = ordered->ack.srtt;
    capture->resend_delay_seconds = ordered->ack.resend_delay;
    capture->window_stalled = ordered->send.stalled ? 1 : 0;
    capture->budget_reliable_waiting = ordered->budget ? ordered->budget->reliable_waiting : 0;
    capture->budget_tokens = ordered->budget ? ordered->budget->tokens : 0.0;
}

void rocketnet_message_server_delivery_capture( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( capture ) )
        return;
    rn_message_object_delivery_capture( &server->object, client_index, capture );
}
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------
//                     create and destroy
// ---------------------------------------------------------------

static void rn_message_link_abort_create( const struct rn_message_link_t * link )
{
    if ( !link || !link->destroy )
        return;
    if ( link->stop )
        link->stop( link->transport );
    while ( link->stopped && !link->stopped( link->transport ) )
    {
        if ( link->update )
            link->update( link->transport, 0.0 ); // stopping updates do not read game time
        if ( !link->stopped( link->transport ) )
            rocketnet_platform_sleep( 0.001 );
    }
    link->destroy( link->transport );
}

struct rn_message_object_t * rn_message_object_create( void * context, const struct rn_message_link_t * link, int is_server, int num_connections,
                                                       const struct rocketnet_message_config_t * config, void * memory, uint64_t memory_bytes,
                                                       int32_t default_kbps, double time, bool reserve_unlimited )
{
    rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }

    // R6 (docs/CHANNELS.md 11.5): a create argument of 0 takes the message
    // config's field; a positive value overrides it for this create; 0 or
    // lower in the config field itself disables that direction's budget.
    // The environment is not read here: the public creates resolve the
    // budget from ROCKETNET_CLIENT_SEND_KBPS or
    // ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS (sdk/rocketnet_api.cpp:135
    // and :173) and pass it as default_kbps.
    if ( default_kbps == 0 )
        default_kbps = is_server ? config->server_send_kbps : config->client_send_kbps;

    // Resolve the explicit create override before sizing. This copy is owned
    // by the object and does not alter the caller's configuration.
    rocketnet_message_config_t sized = *config;
    // Keep the public unlimited runtime ceiling separate from sizing.
    const int reserve_kbps = reserve_unlimited ? 0 : default_kbps;
    sized.client_send_kbps = reserve_kbps;
    sized.server_send_kbps = reserve_kbps;
    config = &sized;

    rn_message_object_layout_t layout = {};
    const bool laid_out = link && rn_message_object_layout( config, num_connections, &layout );
    const bool budget_valid = ( default_kbps <= 0 || rocketnet_send_budget_kbps_valid( (uint32_t) default_kbps ) ) && rocketnet_send_budget_time_valid( time );
    const bool memory_valid = !memory || ( laid_out && memory_bytes >= layout.total && ( (uintptr_t) memory % ROCKETNET_ALLOCATION_ALIGNMENT ) == 0 );

    if ( !laid_out || !budget_valid || !memory_valid )
    {
        if ( !laid_out )
            rocketnet_error( "message object create refused: a refused message config or connection count" );
        else if ( !budget_valid )
            rocketnet_error( "message object create refused: send budget %d kbps or time is outside its range", (int) default_kbps );
        else
            rocketnet_error( "message object create refused: the memory block is short or not %d-aligned", ROCKETNET_ALLOCATION_ALIGNMENT );
        rn_message_link_abort_create( link );
        return NULL;
    }

    // the one allocation, or the caller's block (R2)
    uint8_t * block = (uint8_t *) memory;
    const bool owns_block = block == NULL;
    if ( owns_block )
    {
        block = (uint8_t *) rocketnet_malloc( context, (size_t) layout.total );
        if ( !block )
        {
            rocketnet_error( "message object create failed: could not allocate %llu bytes", (unsigned long long) layout.total );
            rn_message_link_abort_create( link );
            return NULL;
        }
    }

    memset( block, 0, (size_t) layout.arena );

    rn_message_object_t * object = (rn_message_object_t *) block;
    object->context = context;
    object->link = *link;
    object->is_server = is_server;
    object->num_connections = num_connections;
    object->config = *config;
    object->default_kbps = default_kbps;
    object->unreliable_limit = ROCKETNET_MESSAGE_SINGLE_BYTES - 1 - ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
    object->time = time;
    object->pending_connection = -1;
    object->send_next_connection = 0;
    object->block = block;
    object->owns_block = owns_block ? 1 : 0;
    object->hot_connections = (rn_message_connection_hot_t *) ( block + layout.hot_connections );
    object->connections = (rn_message_connection_t *) ( block + layout.connections );
    rn_slots_init( &object->slots, num_connections );

    object->arena = rn_arena_create_in_block( block + layout.arena, layout.arena_bytes, config, num_connections, &object->counters );
    if ( !object->arena )
    {
        rocketnet_error( "message object create failed: the arena was refused" );
        rn_message_link_abort_create( link );
        if ( owns_block )
            rocketnet_free( context, block );
        return NULL;
    }

    rn_endpoint_config_t endpoint_config;
    memset( &endpoint_config, 0, sizeof( endpoint_config ) );
    endpoint_config.config = &object->config;
    endpoint_config.send_kbps = default_kbps;

    for ( int i = 0; i < num_connections; i++ )
    {
        rn_message_connection_t * connection = &object->connections[i];
        connection->hot = &object->hot_connections[i];
        rn_endpoint_init( connection, rn_arena_region( object->arena, i ), &endpoint_config );
        connection->hot->views = block + layout.views + layout.view_stride * (uint64_t) i;
        connection->hot->cursors = (uint32_t *) ( block + layout.cursors + layout.cursor_stride * (uint64_t) i );
        connection->disconnect_reason = ROCKETNET_DISCONNECT_REASON_NONE;
        if ( object->is_server )
            rocketnet_server_connection_reset( object, (int) ( connection - object->connections ) );
        else
            rocketnet_client_connection_reset( object );
    }

    // CHANNELS 11.3: the client's one connection is bound at create, because
    // the client transport hands it payloads from before CONNECTED is
    // reported, and it is never bound again. Its CONFIG control record is
    // announced here and waits in the queue for CONNECTED (RN-2 m11); an
    // application send before CONNECTED is dropped at start, never queued
    // (rn_message_object_start).
    if ( !is_server )
    {
        object->hot_connections[0].state = RN_MESSAGE_CONNECTION_CONNECTED;
        object->hot_connections[0].connection_id = 1;
        object->slots.connection_id[0] = 1;
        rn_endpoint_announce_config( &object->connections[0], time );     // RNEndpoint Reset: the client's connect (10.3)
    }

    return object;
}

static void rn_message_connection_cancel_pending( rn_message_object_t * object, int index );

void rn_message_object_destroy( struct rn_message_object_t * object )
{
    if ( !object )
        return;
    if ( object->link.destroy && object->link.stopped )
    {
        const bool stopped = object->link.stopped( object->link.transport );
        rocketnet_assert( stopped );
        if ( !stopped )
            return;
    }
    if ( object->in_place )
        rn_message_connection_cancel_pending( object, object->pending_connection );   // the open frame goes back first
    if ( object->link.destroy )
        object->link.destroy( object->link.transport );
    rn_arena_destroy( object->arena );
    if ( object->owns_block )
        rocketnet_free( object->context, object->block );
}

// ---------------------------------------------------------------
//                     kick, the one way a connection ends here
// ---------------------------------------------------------------

static void rn_message_connection_cancel_pending( rn_message_object_t * object, int index )
{
    rn_message_connection_t * connection = &object->connections[index];
    if ( object->pending_connection != index )
        return;
    if ( object->in_place )
    {
        // RNOutgoingBuild Abort: the frame goes back to the transport
        object->link.abort_packet( object->link.transport, &object->in_place_context );
        object->in_place = 0;
        object->in_place_payload = NULL;
    }
    rn_endpoint_abort( connection );
    object->pending_connection = -1;
}

// The transport forbids an update or a flush while a fill is open (sdk/rocketnet.h,
// the threading contract), so an in-place start still pending there is aborted:
// the caller's error, counted, and its later finish returns 0.

static void rn_message_object_close_in_place( rn_message_object_t * object )
{
    if ( !object->in_place )
        return;
    rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    rocketnet_api_misuse_record();
    rocketnet_error( "message start pending at update or send_packets: the in-place send is aborted" );
    rn_message_connection_cancel_pending( object, object->pending_connection );
}

// RNSlots ResetCancel: a queued-path start still pending on a connection its
// update resets is cancelled before that reset, as kick cancels it, so the
// recarved region holds none of it and later starts on every connection are
// not refused by it. Counted in MESSAGE_SEND_REFUSED; its finish then returns
// 0 (rn_message_object_finish: pending_connection is -1).

static void rn_message_connection_cancel_at_reset( rn_message_object_t * object, int index )
{
    if ( object->pending_connection != index )
        return;
    rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    rocketnet_info( "message start pending on connection %d at its reset: the start is cancelled", index );
    rn_message_connection_cancel_pending( object, index );
}

void rn_message_object_kick( struct rn_message_object_t * object, int index, int reason )
{
    // RNSlots Kick, RNConnectionMemory Kick: the transport ends the connection
    // now, with the reason recorded; nothing more is sent, delivered or taken
    // from its region. The region is reset at the next update (ResetFree),
    // before the transport can hand the slot to another client. Views the
    // application already holds stay valid until that update.
    if ( !object || index < 0 || index >= object->num_connections )
        return;
    rn_message_connection_t * connection = &object->connections[index];
    if ( connection->hot->state != RN_MESSAGE_CONNECTION_CONNECTED )
        return;
    rn_message_connection_cancel_pending( object, index );
    connection->hot->state = RN_MESSAGE_CONNECTION_KICKED;
    connection->disconnect_reason = reason;
    if ( object->link.disconnect )
        object->link.disconnect( object->link.transport, index );
    rocketnet_info( "message connection %d ended: reason %d", index, reason );
}

// ---------------------------------------------------------------
//                          update
// ---------------------------------------------------------------

// RNSlots Admit: an entry is applied only to the connection it was received
// for. The link tags each entry with the transport's id of that connection
// (payload_connection, acked_connection: P9b), and rn_slots_admit applies it
// only when that id is the slot's binding from this update's Observe, so an
// entry of a connection that ended inside the update is discarded and a slot
// bound in this update keeps its own first entries. A link with no tag
// function presents the slot's binding, or no tag (0, which no connection
// carries) when Observe bound or dropped the slot in this update, which
// discards it (CHANNELS 10.2, 11.5).

static rn_message_connection_t * rn_message_admit( rn_message_object_t * object, const int * client_index, uint64_t (*entry_connection)( void * transport, int k ), int k )
{
    const int index = client_index ? client_index[k] : 0;
    if ( index < 0 || index >= object->num_connections )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        return NULL;
    }
    rn_message_connection_t * connection = &object->connections[index];
    const uint64_t entry_connection_id = entry_connection ? entry_connection( object->link.transport, k )
                                                          : ( object->slots.changed[index] ? 0 : connection->hot->connection_id );
    if ( rn_slots_admit( &object->slots, index, entry_connection_id ) != RN_SLOTS_APPLY )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        return NULL;
    }
    return connection;
}

static void rn_message_object_release_batch( struct rn_message_object_t * object );

int rn_message_object_update( struct rn_message_object_t * object, double time )
{
    int result = ROCKETNET_UPDATE_OK;

    // RNSlots BeginUpdate, and RejectUnreleasedUpdate: every view handed out
    // in the last update must be released before this one. A held view is the
    // application's error: it asserts in a debug build and returns the named
    // error in release. The views die with the transport's batch either way,
    // so they are taken back here and memory never grows from a held view.
    rn_message_object_release_batch( object );            // a batch is released by the update that follows it
    uint32_t held = 0;
    for ( int i = 0; i < object->num_connections; i++ )
        held += object->hot_connections[i].held;
    if ( held > 0 )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED, held );
        rocketnet_error( "message update with %u received message(s) not released", held );
        rocketnet_assert( held == 0 && "every received message is released before the next update" );
        result = ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED;
    }

    // RNChannel EndUpdate, the misuse end (standard/MESSAGES.md m1.6): every
    // delivery of the last update is handed to the application before this
    // one. The transport acked each one already, so a delivery still waiting
    // here would be lost with the sender told it arrived. It is the
    // application's error, handled like a send with no room: counted, an
    // assert in a debug build, and that connection ends with
    // MESSAGE_NOT_RECEIVED. Nothing is dropped silently.
    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
            continue;                                   // a kicked connection's deliveries were never offered
        rn_message_connection_t * connection = &object->connections[i];
        const uint32_t deliveries = connection->ordered.channels.deliveries;
        uint32_t waiting = 0;
        for ( uint32_t k = 0; k < deliveries; k++ )
            waiting += connection->hot->views[k] == RN_MESSAGE_VIEW_WAITING ? 1u : 0u;
        if ( waiting == 0 )
            continue;
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_NOT_RECEIVED, waiting );
        rocketnet_error( "message update with %u delivered message(s) not received on connection %d: loop receive_messages until NULL before update", waiting, i );
        rocketnet_assert( waiting == 0 && "every delivered message is received before the next update" );
        rn_message_object_kick( object, i, ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED );
    }

    rn_message_object_close_in_place( object );

    bool time_clamped = false;
    time = rocketnet_clamp_update_time( object->time, time, &time_clamped );
    if ( time_clamped && !object->clock_started )
        return result;
    object->time = time;

    // rn_message_object_update leaves both clocks untouched on a rejected
    // first sample. The client transport starts its clock at the
    // first valid update, so connect timers and token lifetime start there; the
    // object's own clock (budgets, resends, timeouts) reads game time.
    if ( !object->clock_started && !time_clamped )
    {
        object->clock_started = 1;
        object->clock_origin = object->link.clock_from_first_update ? time : 0.0;
    }

    for ( int i = 0; i < object->num_connections; i++ )
    {
        rn_message_connection_t * connection = &object->connections[i];

        // RNChannel EndUpdate: the last update's deliveries expire and their
        // receive descriptors return to the region (RNSlots Release's charge)
        const uint32_t delivered = rn_endpoint_begin_update( connection );
        memset( connection->hot->views, 0, delivered < connection->region->delivery_entries ? delivered : connection->region->delivery_entries );
        memset( connection->hot->cursors, 0, sizeof( uint32_t ) * connection->region->num_channels );
        connection->hot->held = 0;

        // RNSlots ResetFree: a kicked connection's region is reset before the
        // transport's update can give the slot to another client, and the
        // slots machine sees the slot free until Observe reads the transport
        if ( object->hot_connections[i].state == RN_MESSAGE_CONNECTION_KICKED )
        {
            rn_message_connection_cancel_at_reset( object, i );     // RNSlots ResetCancel (kick cancelled it already)
            if ( object->is_server )
                rocketnet_server_connection_reset( object, i );
            else
                rocketnet_client_connection_reset( object );
            rn_slots_observe( &object->slots, i, 0 );
        }
    }

    object->receive_connection = 0;                     // the receive queue starts again at the first connection
    object->link.update( object->link.transport, time - object->clock_origin );

    // The client message connection is bound before the handshake so its
    // CONFIG record waits while its transport connects (application sends
    // before CONNECTED are dropped at start). Once that transport reaches a
    // terminal state, the binding has ended: release its channel/region state
    // before any ack or payload from the ended connection can be admitted.
    // Explicit message kicks were reset at the start of this update.
    if ( !object->is_server && object->link.state &&
         object->hot_connections[0].state == RN_MESSAGE_CONNECTION_CONNECTED &&
         object->link.state( object->link.transport ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
    {
        // the transport's end reason (SERVER_DISCONNECTED once it accepted
        // the server's disconnect) is recorded before the reset, which keeps it
        if ( object->link.end_reason )
            object->connections[0].disconnect_reason = object->link.end_reason( object->link.transport );
        rn_message_connection_cancel_at_reset( object, 0 );         // RNSlots ResetCancel
        rocketnet_client_connection_reset( object );
        rn_slots_observe( &object->slots, 0, 0 );
    }

    // RNSlots Observe: the server reads every slot's connection after the
    // transport's update; a changed connection resets the slot (Connect,
    // Disconnect)
    if ( object->is_server )
    {
        for ( int i = 0; i < object->num_connections; i++ )
        {
            rn_message_connection_t * connection = &object->connections[i];
            const uint64_t id = object->link.connection( object->link.transport, i );
            const int action = rn_slots_observe( &object->slots, i, id );
            if ( action == RN_SLOTS_NONE )
                continue;

            rn_message_connection_cancel_at_reset( object, i );     // RNSlots ResetCancel
            rocketnet_server_connection_reset( object, i );

            if ( action == RN_SLOTS_RESET )
            {
                connection->hot->state = RN_MESSAGE_CONNECTION_CONNECTED;
                connection->hot->connection_id = id;
                connection->disconnect_reason = ROCKETNET_DISCONNECT_REASON_NONE;
                rn_endpoint_announce_config( connection, time );   // RNEndpoint Reset: the server's connect (10.3)
            }
        }
    }

    // RNSlots TransportAck then Admit: the transport's acked packet ids are
    // the only ack (R1), RNChannel ReceiveAck
    {
        const int * client_index = NULL;
        const uint64_t * packet_ids = NULL;
        const int count = object->link.acked( object->link.transport, &client_index, &packet_ids );
        for ( int k = 0; k < count; k++ )
        {
            rn_message_connection_t * connection = rn_message_admit( object, client_index, object->link.acked_connection, k );
            if ( connection )
                rn_endpoint_on_acked( connection, packet_ids[k], time );   // RNEndpoint OnAcked
        }
    }

    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
            continue;
        rn_message_connection_t * connection = &object->connections[i];
        rn_endpoint_tick( connection, time );          // RNEndpoint Tick: RNReceipts SweepLoss
    }

    // RNSlots TransportOpen then Admit: each payload through layer 2's reader
    // into the dispatch (RNChannel OpenPacket, DeliverMessage)
    {
        const int * client_index = NULL;
        uint8_t * const * payload_data = NULL;
        const int * payload_bytes = NULL;
        const int count = object->link.payloads( object->link.transport, &client_index, &payload_data, &payload_bytes );
        for ( int k = 0; k < count; k++ )
        {
            rn_message_connection_t * connection = rn_message_admit( object, client_index, object->link.payload_connection, k );
            if ( connection )
                rn_endpoint_on_payload( connection, payload_data[k], payload_bytes[k], NULL, 0, NULL, time );    // RNEndpoint OnPayload
        }
    }

    // RNSlots Kick, RNConnectionMemory Kick: exhaustion first, then a wire
    // violation, then a reliable message past its timeout (rn_endpoint_error)
    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
            continue;
        rn_message_connection_t * connection = &object->connections[i];
        // RNEndpoint Tick, the CONFIG half: the wait for the peer's CONFIG
        // starts at the first update the connection is connected at (the
        // server's from its Observe reset, the client's from the first update
        // its transport reports CONNECTED, 11.3)
        const int transport_connected = object->is_server || object->link.connection( object->link.transport, i ) != 0;
        rn_endpoint_config_tick( connection, transport_connected, time );   // RNEndpoint Tick, the CONFIG half
        const int reason = rn_endpoint_error( connection );
        if ( reason != ROCKETNET_DISCONNECT_REASON_NONE )
        {
            // RNEndpoint OnPayload: a CONFIG mismatch was logged with the
            // field and both values when the CONFIG was compared
            rn_message_object_kick( object, i, reason );
        }
    }

    return result;
}

// ---------------------------------------------------------------
//                          send
// ---------------------------------------------------------------

uint8_t * rn_message_object_start( struct rn_message_object_t * object, int index, int channel, int reliability, int bytes )
{
    // RNSlots Request, RNChannel Enqueue's first half: room in the region for a
    // message of `bytes` bytes, and the pointer the application writes it at,
    // once, in place (R3)
    if ( index < 0 || index >= object->num_connections )
        return NULL;

    // the channel is the ordering channel: the types that are neither
    // ordered nor sequenced ignore it (they have no ordering state) and travel with none (codes 0 and 2), so any
    // value is accepted for them, never refused, and taken as 0
    if ( reliability == ROCKETNET_RELIABILITY_UNRELIABLE || reliability == ROCKETNET_RELIABILITY_RELIABLE ||
         reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT || reliability == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT )
        channel = 0;

    rn_message_connection_t * connection = &object->connections[index];

    // standard/MESSAGES.md m1.5 (tla/ClientSession.tla DropSend,
    // tla/ServerSession.tla ServerDropSend): an application message is sent
    // only on a CONNECTED connection. A client whose transport is not
    // CONNECTED, or a slot with no connection, drops the start: NULL, counted
    // once, nothing queued for later. A link with no connection query (a bare
    // object with no transport) has no transport state to wait on.
    const bool connected = connection->hot->state == RN_MESSAGE_CONNECTION_CONNECTED &&
                           ( object->is_server || !object->link.connection || object->link.connection( object->link.transport, 0 ) != 0 );
    if ( !connected )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );
        return NULL;
    }
    // one start pending per object: a second is refused until the first is
    // finished or aborted, or a reset of its connection cancels it
    // (rn_message_connection_cancel_at_reset, RNSlots ResetCancel)
    if ( object->pending_connection >= 0 )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        return NULL;
    }

    // the transport's payload cap, which the endpoint's large unreliable
    // check reads (a seam: a transport value passed in as an argument)
    const bool unreliable = reliability == ROCKETNET_RELIABILITY_UNRELIABLE || reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
                            reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
    const int transport_max = unreliable && object->link.max_payload_bytes ?
        object->link.max_payload_bytes( object->link.transport ) : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    // RNOutgoingBuild StartInPlace: a fitting unreliable message takes a
    // transport frame and is written in it, its header's room in front and
    // the form byte's after; header + bytes + 1, the declared maximum,
    // decides the frame
    if ( unreliable && object->link.start_packet )
    {
        int header = rn_endpoint_in_place_header( connection, channel, reliability, bytes, transport_max, object->time );
        // Reserve a complete round for the rotating pass, including this
        // connection's reliable work. In-place writes may use surplus slots,
        // but application call order cannot consume another client's turn.
        const int send_connections = object->link.send_connected_count ?
            object->link.send_connected_count( object->link.transport ) : object->num_connections;
        if ( header >= 0 && object->link.send_capacity_available &&
             object->link.send_capacity_available( object->link.transport ) <= send_connections )
            header = RN_ENDPOINT_IN_PLACE_QUEUED;
        if ( header >= 0 && !rn_endpoint_in_place_affordable( connection, header + bytes + 1, object->time ) )
            header = RN_ENDPOINT_IN_PLACE_QUEUED;               // the bucket cannot pay it now: the write pass decides
        if ( header >= 0 )
        {
            const int request = header + bytes + 1;
            rocketnet_packet_context_t context;
            uint8_t * frame = object->link.start_packet( object->link.transport, index, request, &context );
            if ( frame && context.payload_capacity < request )
            {
                object->link.abort_packet( object->link.transport, &context );
                frame = NULL;
            }
            if ( frame )
            {
                rn_endpoint_in_place_begin( connection, channel, reliability, bytes, header );
                object->in_place = 1;
                object->in_place_context = context;
                object->in_place_payload = frame + context.payload_index;
                object->pending_connection = index;
                return object->in_place_payload + header;
            }
        }
        // RNOutgoingBuild StartQueued with fellback: no frame, unreliable
        // messages wait ahead of it, the update's packet allowance is spent
        // (one packet of it kept while reliable work is due), or the budget
        // cannot pay it now (MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK)
        if ( header >= 0 || header == RN_ENDPOINT_IN_PLACE_QUEUED )
            rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );
    }

    // RNEndpoint Send's first half. The send room watch runs around it: before,
    // so acks that freed room rearm it, and after, so this start message's room
    // is seen (a debug-build warning, nothing in a release build)
    rn_region_send_room_note( connection->region, object->is_server, index );
    uint8_t * pointer = rn_endpoint_start( connection, channel, reliability, bytes, transport_max );
    if ( !pointer )
    {
        if ( connection->send_error != ROCKETNET_DISCONNECT_REASON_NONE )
        {
            if ( connection->send_error == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED )
                rn_region_send_room_exhausted( connection->region, object->is_server, index );
            rn_message_object_kick( object, index, connection->send_error );   // R5
        }
        return NULL;
    }
    rn_region_send_room_note( connection->region, object->is_server, index );

    object->pending_connection = index;
    return pointer;
}

// The functional tier's pacing reads the send room through these: 1 when a
// start message of `bytes` bytes would be given room now (rn_region_send_fits).
// Internal: declared by the test that reads them, never in rocketnet.h, never
// exported.
int rn_message_client_send_fits( struct rocketnet_message_client_t * client, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );
int rn_message_server_send_fits( struct rocketnet_message_server_t * server, int client_index, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );

static int rn_message_object_send_fits( struct rn_message_object_t * object, int index, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages )
{
    if ( !object || index < 0 || index >= object->num_connections )
        return 0;
    return rn_region_send_fits( object->connections[index].region, bytes, free_bytes, pool_bytes, free_messages );
}

int rn_message_client_send_fits( struct rocketnet_message_client_t * client, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages )
{
    return client ? rn_message_object_send_fits( &client->object, 0, bytes, free_bytes, pool_bytes, free_messages ) : 0;
}

int rn_message_server_send_fits( struct rocketnet_message_server_t * server, int client_index, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages )
{
    return server ? rn_message_object_send_fits( &server->object, client_index, bytes, free_bytes, pool_bytes, free_messages ) : 0;
}

uint32_t rn_message_object_finish( struct rn_message_object_t * object, int index, int bytes )
{
    // RNChannel Enqueue's second half: the message joins its channel's queue
    // in FIFO order and takes its receipt serial (7.4: nonzero for every send).
    // 0 when no start is pending on this connection, a start a reset of the
    // connection cancelled included (rn_message_connection_cancel_at_reset)
    if ( index < 0 || index >= object->num_connections || object->pending_connection != index )
        return 0;

    if ( object->in_place )
    {
        // RNOutgoingBuild FinishInPlace: the header and form byte written
        // around the bytes, and the same frame finished: sealed and staged by
        // the transport, the acks due riding in it
        rn_message_connection_t * connection = &object->connections[index];
        rn_message_link_t * link = &object->link;
        int payload_bytes = 0;
        const uint32_t serial = rn_endpoint_in_place_finish( connection, object->in_place_payload, bytes, object->time, &payload_bytes );
        if ( serial == 0 )
        {
            rn_message_connection_cancel_pending( object, index );     // a refused finish aborts its start: the object is never stuck
            return 0;
        }
        rocketnet_packet_context_t context = object->in_place_context;
        object->in_place = 0;
        object->in_place_payload = NULL;
        object->pending_connection = -1;
        if ( payload_bytes == 0 )
        {
            link->abort_packet( link->transport, &context );     // FinishInPlaceOverBudget: dropped and counted
            return serial;
        }
        const int wire_bytes = link->finish_packet( link->transport, &context, payload_bytes );
        if ( wire_bytes > 0 )
            rn_endpoint_in_place_sent( connection, context.packet_id, wire_bytes, object->time );
        else
            rn_endpoint_in_place_failed( connection );
        return serial;
    }

    const uint32_t serial = rn_endpoint_finish( &object->connections[index], bytes, 0 );   // RNEndpoint Send's second half
    if ( serial == 0 )
    {
        rn_message_connection_cancel_pending( object, index );         // a refused finish aborts its start (tla/RNSlots.tla FinishPending)
        return 0;
    }

    object->pending_connection = -1;
    return serial;
}

void rn_message_object_abort( struct rn_message_object_t * object, int index )
{
    // a start that will not be finished: its room returns to the region
    if ( index < 0 || index >= object->num_connections || object->pending_connection != index )
        return;
    rn_message_connection_cancel_pending( object, index );
}

uint32_t rn_message_object_send( struct rn_message_object_t * object, int index, int channel, int reliability, const uint8_t * data, int bytes )
{
    // the convenience over start and finish. It copies: the caller's bytes
    // into the region record, the one copy start and finish exist to avoid
    uint8_t * pointer = rn_message_object_start( object, index, channel, reliability, bytes );
    if ( !pointer )
        return 0;
    memcpy( pointer, data, (size_t) bytes );
    return rn_message_object_finish( object, index, bytes );
}

// RNChannel WritePacket with RNSendBudget Send, Wait and Drop, for one
// connection: the endpoint plans each packet (rn_endpoint_next_packet_bytes),
// the object starts one frame from the transport at a time, the endpoint
// writes it in place (THE COPIES (a)), and the object finishes it and tells the
// endpoint whether it left, under the transport's packet id (RNSlots Send).

static bool rn_message_connection_write( rn_message_object_t * object, int index )
{
    rn_message_connection_t * connection = &object->connections[index];
    rn_message_link_t * link = &object->link;
    const double now = object->time;

    for ( ;; )
    {
        const int request = rn_endpoint_next_packet_bytes( connection, now );
        if ( request == 0 )
            return false;

        rocketnet_packet_context_t context;
        uint8_t * frame = link->start_packet( link->transport, index, request, &context );
        if ( !frame )
        {
            rn_endpoint_packet_failed( connection, 0, now );    // no frame: nothing written
            continue;
        }

        const int payload_bytes = rn_endpoint_write_packet( connection, frame + context.payload_index, context.payload_capacity, context.packet_id, now );
        if ( payload_bytes == 0 )
        {
            link->abort_packet( link->transport, &context );
            continue;
        }

        const int wire_bytes = link->finish_packet( link->transport, &context, payload_bytes );
        if ( wire_bytes > 0 )
            rn_endpoint_packet_sent( connection, context.packet_id, wire_bytes, now );
        else
            rn_endpoint_packet_failed( connection, context.packet_id, now );
        return true;
    }
}

void rn_message_object_send_packets( struct rn_message_object_t * object )
{
    rn_message_object_close_in_place( object );

    // RNSlots Send, fair rotation: start at the first connection not yet
    // offered a frame, and stop when the transport's send capacity for this
    // update is spent. The cursor names the next unserved connection, so a
    // cap below the client count serves every client in turn rather than
    // forever serving the low indices. A connection that finds no capacity
    // leaves its queued sends for the next update; reliability keeps them
    // due, so nothing is lost.
    const int num_connections = object->num_connections;
    const int start = num_connections > 0 ? object->send_next_connection % num_connections : 0;
    bool progress;
    do
    {
        progress = false;
        for ( int offset = 0; offset < num_connections; offset++ )
        {
            const int i = ( start + offset ) % num_connections;
            if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
                continue;
            // The client's CONFIG waits unnumbered until the transport connects.
            if ( !object->is_server && object->link.connection( object->link.transport, 0 ) == 0 )
                continue;
            if ( object->link.send_capacity_available &&
                 object->link.send_capacity_available( object->link.transport ) <= 0 )
                goto flush;
            // One frame per connection per round, including when the per-client
            // allowance is larger than one. A busy client cannot take K turns.
            progress |= rn_message_connection_write( object, i );
            object->send_next_connection = ( i + 1 ) % num_connections;
        }
    } while ( progress );
flush:
    object->link.send_packets( object->link.transport );
}

// ---------------------------------------------------------------
//                          receive and release
// ---------------------------------------------------------------

// Receiving pops one queue across every connection. The
// object's queue is the update's deliveries, connection by connection in index
// order and each connection's in the order its channels delivered them
// (rn_delivery_t entries are appended in dispatch order, the drain of an
// ordered hold included), whatever channel they carry: a message is never
// filtered by channel, so an unordered type delivered on channel 0
// (standard/MESSAGES.md, code 0 and code 2 carry no channel) is seen like
// every other. Ordered and sequenced order holds within each channel of each
// connection, each connection's channels ordered on their own.

static const rn_delivery_t * rn_message_object_next( struct rn_message_object_t * object, int * out_index, uint32_t * out_entry )
{
    for ( int i = object->receive_connection; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state == RN_MESSAGE_CONNECTION_CONNECTED )
        {
            rn_message_connection_t * connection = &object->connections[i];
            const uint32_t deliveries = connection->ordered.channels.deliveries;
            const rn_delivery_t * table = connection->region->deliveries;
            for ( uint32_t k = connection->hot->cursors[0]; k < deliveries; k++ )
            {
                if ( connection->hot->views[k] != RN_MESSAGE_VIEW_WAITING )
                    continue;
                connection->hot->views[k] = RN_MESSAGE_VIEW_HELD;
                connection->hot->held++;
                connection->hot->cursors[0] = k + 1;
                object->receive_connection = i;
                *out_index = i;
                *out_entry = k;
                return &table[k];
            }
            connection->hot->cursors[0] = deliveries;
        }
        object->receive_connection = i + 1;
    }
    return NULL;
}

int rn_message_object_receive( struct rn_message_object_t * object, int * index, int * channel, const uint8_t ** data, int * bytes )
{
    // RNSlots EndUpdate: the update's deliveries go to the application, each a
    // view -- into the transport's batch, or into the receive ring for a
    // message held ahead of its turn -- valid until the next update and held
    // until released
    *data = NULL;
    *bytes = 0;
    if ( index )
        *index = 0;
    if ( channel )
        *channel = 0;

    int connection_index = 0;
    uint32_t entry = 0;
    const rn_delivery_t * delivery = rn_message_object_next( object, &connection_index, &entry );
    if ( !delivery )
        return 0;
    if ( index )
        *index = connection_index;
    if ( channel )
        *channel = delivery->channel;
    *data = delivery->data;
    *bytes = delivery->bytes;
    return 1;
}

int rn_message_object_receive_channel( struct rn_message_object_t * object, int index, int channel, const uint8_t ** data, int * bytes )
{
    // the per-channel cursor the unit suite's pre-batch cases still use; the
    // public one queue passes through rn_message_object_receive instead
    *data = NULL;
    *bytes = 0;
    if ( index < 0 || index >= object->num_connections || channel < 0 || channel >= object->config.num_channels )
        return 0;

    rn_message_connection_t * connection = &object->connections[index];
    if ( connection->hot->state != RN_MESSAGE_CONNECTION_CONNECTED )
        return 0;

    const uint32_t deliveries = connection->ordered.channels.deliveries;
    const rn_delivery_t * table = connection->region->deliveries;
    for ( uint32_t k = connection->hot->cursors[channel]; k < deliveries; k++ )
    {
        if ( table[k].channel != (uint16_t) channel || connection->hot->views[k] != RN_MESSAGE_VIEW_WAITING )
            continue;
        connection->hot->views[k] = RN_MESSAGE_VIEW_HELD;
        connection->hot->held++;
        connection->hot->cursors[channel] = k + 1;
        *data = table[k].data;
        *bytes = table[k].bytes;
        return 1;
    }
    connection->hot->cursors[channel] = deliveries;
    return 0;
}

static bool rn_message_connection_release_view( rn_message_connection_t * connection, const uint8_t * data )
{
    if ( connection->hot->held == 0 )
        return false;
    const uint32_t deliveries = connection->ordered.channels.deliveries;
    const rn_delivery_t * table = connection->region->deliveries;
    for ( uint32_t k = 0; k < deliveries; k++ )
    {
        if ( table[k].data == data && connection->hot->views[k] == RN_MESSAGE_VIEW_HELD )
        {
            connection->hot->views[k] = RN_MESSAGE_VIEW_RELEASED;
            connection->hot->held--;
            return true;
        }
    }
    return false;
}

void rn_message_object_release( struct rn_message_object_t * object, const uint8_t * data )
{
    // RNSlots Release: the view goes back, and with it the region's charge for
    // the message at the next update's EndUpdate. A release takes the
    // received message alone; the view is looked for in the connection the
    // queue is at first, then in every connection that holds one
    if ( !data )
        return;

    const int at = object->receive_connection < object->num_connections ? object->receive_connection : object->num_connections - 1;
    if ( object->hot_connections[at].held > 0 &&
         rn_message_connection_release_view( &object->connections[at], data ) )
        return;
    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( i != at && object->hot_connections[i].held > 0 &&
             rn_message_connection_release_view( &object->connections[i], data ) )
            return;
    }

    // not a held view of any connection: the caller's error, counted
    rocketnet_api_misuse_record();
}

// The batch's views go back together: each entry still HELD (a connection
// reset since the batch cleared its views already) is released.
static void rn_message_object_release_batch( struct rn_message_object_t * object )
{
    for ( int n = 0; n < object->batch_count; n++ )
    {
        rn_message_connection_t * connection = &object->connections[object->batch_connection[n]];
        const uint32_t k = object->batch_entry[n];
        if ( k < connection->region->delivery_entries && connection->hot->views[k] == RN_MESSAGE_VIEW_HELD )
        {
            connection->hot->views[k] = RN_MESSAGE_VIEW_RELEASED;
            connection->hot->held--;
        }
    }
    object->batch_count = 0;
}

int rn_message_object_receive_messages( struct rn_message_object_t * object, struct rocketnet_message_batch_t * out )
{
    // the pumped receive of the one queue (flow's batch handoff to the game):
    // the last call's entries are released, then out is filled with up to
    // ROCKETNET_RECEIVE_BATCH_MESSAGES next deliveries, in the order
    // rn_message_object_receive hands them out. The views point into the
    // transport's batch or the receive ring, never copied
    rn_message_object_release_batch( object );
    out->num_messages = 0;
    while ( out->num_messages < ROCKETNET_RECEIVE_BATCH_MESSAGES )
    {
        int connection_index = 0;
        uint32_t entry = 0;
        const rn_delivery_t * delivery = rn_message_object_next( object, &connection_index, &entry );
        if ( !delivery )
            break;
        const int n = out->num_messages++;
        object->batch_connection[n] = connection_index;
        object->batch_entry[n] = entry;
        object->batch_count = out->num_messages;
        out->client_index[n] = connection_index;
        out->channel[n] = delivery->channel;
        out->reliability[n] = (enum rocketnet_reliability_t) delivery->code;
        out->bytes[n] = delivery->bytes;
        out->data[n] = delivery->data;
    }
    return out->num_messages;
}

bool rn_message_object_set_send_kbps( struct rn_message_object_t * object, int index, int32_t kbps )
{
    if ( index < 0 || index >= object->num_connections )
        return false;
    return rn_endpoint_set_send_kbps( &object->connections[index], kbps, object->time );
}

int rn_message_object_disconnect_reason( const struct rn_message_object_t * object, int index )
{
    if ( index < 0 || index >= object->num_connections )
        return ROCKETNET_DISCONNECT_REASON_NONE;
    return object->connections[index].disconnect_reason;
}

int rn_message_object_connected( const struct rn_message_object_t * object, int index )
{
    if ( index < 0 || index >= object->num_connections )
        return 0;
    return object->hot_connections[index].state == RN_MESSAGE_CONNECTION_CONNECTED ? 1 : 0;
}

// ---------------------------------------------------------------
//                     the server transport's link
// ---------------------------------------------------------------

static void rn_server_link_update( void * transport, double time )
{
    rocketnet_server_transport_update( (rocketnet_server_transport_t *) transport, time );
}

static int rn_server_link_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    return rocketnet_server_transport_payloads( (rocketnet_server_transport_t *) transport, client_index, payload_data, payload_bytes );
}

static int rn_server_link_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    return rocketnet_server_transport_acked_packets( (rocketnet_server_transport_t *) transport, client_index, packet_ids );
}

static int rn_server_link_max_payload_bytes( void * transport )
{
    return rocketnet_server_transport_max_payload_bytes( (rocketnet_server_transport_t *) transport );
}

static uint8_t * rn_server_link_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    return rocketnet_server_transport_start_packet( (rocketnet_server_transport_t *) transport, client_index, request_bytes, context );
}

static int rn_server_link_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    return rocketnet_server_transport_finish_packet( (rocketnet_server_transport_t *) transport, context, payload_bytes );
}

static void rn_server_link_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    rocketnet_server_transport_abort_packet( (rocketnet_server_transport_t *) transport, context );
}

static void rn_server_link_send_packets( void * transport )
{
    rocketnet_server_transport_send_packets( (rocketnet_server_transport_t *) transport );
}

static int rn_server_link_send_connected_count( void * transport )
{
    return rocketnet_server_transport_num_connected_clients( (rocketnet_server_transport_t *) transport );
}

static int rn_server_link_send_capacity_available( void * transport )
{
    return rocketnet_server_transport_send_capacity_available( (const rocketnet_server_transport_t *) transport );
}

// P9a: the transport's id of the connection in the slot, 0 while it is free;
// a reconnect into the slot has a new id even with the same client id and
// inside one update, so Observe resets the slot for it
static uint64_t rn_server_link_connection( void * transport, int client_index )
{
    return rocketnet_server_transport_client_connection_id( (rocketnet_server_transport_t *) transport, client_index );
}

// P9b: the connection each batch entry was received for, parallel to the
// batch's client index array
static uint64_t rn_server_link_payload_connection( void * transport, int k )
{
    const uint64_t * connection_id = NULL;
    const int count = rocketnet_server_transport_payload_connection_ids( (rocketnet_server_transport_t *) transport, &connection_id );
    return connection_id && k >= 0 && k < count ? connection_id[k] : 0;
}

static uint64_t rn_server_link_acked_connection( void * transport, int k )
{
    const uint64_t * connection_id = NULL;
    const int count = rocketnet_server_transport_acked_connection_ids( (rocketnet_server_transport_t *) transport, &connection_id );
    return connection_id && k >= 0 && k < count ? connection_id[k] : 0;
}

static void rn_server_link_disconnect( void * transport, int client_index )
{
    rocketnet_server_transport_disconnect_client( (rocketnet_server_transport_t *) transport, client_index );
}

static int rn_server_link_state( void * transport )
{
    return rocketnet_server_transport_state( (rocketnet_server_transport_t *) transport );
}

static void rn_server_link_stop( void * transport )
{
    rocketnet_server_transport_stop( (rocketnet_server_transport_t *) transport );
}

static bool rn_server_link_stopped( const void * transport )
{
    return rocketnet_server_transport_stopped( (const rocketnet_server_transport_t *) transport );
}

static void rn_server_link_destroy( void * transport )
{
    rocketnet_server_transport_destroy( (rocketnet_server_transport_t *) transport );
}

void rn_message_server_link( struct rn_message_link_t * link, struct rocketnet_server_transport_t * transport )
{
    memset( link, 0, sizeof( *link ) );
    link->transport = transport;
    link->update = rn_server_link_update;
    link->payloads = rn_server_link_payloads;
    link->acked = rn_server_link_acked;
    link->payload_connection = rn_server_link_payload_connection;
    link->acked_connection = rn_server_link_acked_connection;
    link->max_payload_bytes = rn_server_link_max_payload_bytes;
    link->start_packet = rn_server_link_start_packet;
    link->finish_packet = rn_server_link_finish_packet;
    link->abort_packet = rn_server_link_abort_packet;
    link->send_packets = rn_server_link_send_packets;
    link->send_capacity_available = rn_server_link_send_capacity_available;
    link->send_connected_count = rn_server_link_send_connected_count;
    link->connection = rn_server_link_connection;
    link->disconnect = rn_server_link_disconnect;
    link->state = rn_server_link_state;
    link->stop = rn_server_link_stop;
    link->stopped = rn_server_link_stopped;
    link->destroy = rn_server_link_destroy;
}

// ---------------------------------------------------------------
//                     the server's C API
// ---------------------------------------------------------------

static int rn_message_server_max_clients( const struct rocketnet_server_transport_config_t * transport_config )
{
    struct rocketnet_server_transport_config_t defaults;
    if ( !transport_config )
    {
        rocketnet_server_transport_default_config( &defaults );
        transport_config = &defaults;
    }
    return transport_config->max_clients;
}

uint64_t rocketnet_message_server_required_memory( const struct rocketnet_server_transport_config_t * transport_config, const struct rocketnet_message_config_t * message_config )
{
    return rn_message_object_required_bytes( message_config, rn_message_server_max_clients( transport_config ), 1 );
}

struct rocketnet_message_server_t * rocketnet_message_server_create( void * context, const char * address,
                                                                     const struct rocketnet_server_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps )
{
    return rocketnet_message_server_create_ex( context, address, transport_config, message_config, memory, memory_bytes, per_client_send_kbps, NULL, 0 );
}

// resolved_memory_budget NULL: the transport create reads the environment
// (rocketnet_message_server_create_ex); set: transport_config is already
// resolved and start reads nothing (rocketnet_message_server_create_resolved)
static struct rocketnet_message_server_t * rn_message_server_create( void * context, const char * address,
                                                                     const struct rocketnet_server_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps, int * status, int reserve_unlimited,
                                                                     const uint64_t * resolved_memory_budget );

struct rocketnet_message_server_t * rocketnet_message_server_create_ex( void * context, const char * address,
                                                                        const struct rocketnet_server_transport_config_t * transport_config,
                                                                        const struct rocketnet_message_config_t * message_config,
                                                                        void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps, int * status, int reserve_unlimited )
{
    return rn_message_server_create( context, address, transport_config, message_config, memory, memory_bytes, per_client_send_kbps, status, reserve_unlimited, NULL );
}

struct rocketnet_message_server_t * rocketnet_message_server_create_resolved( void * context, const char * address,
                                                                              const struct rocketnet_server_transport_config_t * transport_config,
                                                                              const struct rocketnet_message_config_t * message_config,
                                                                              void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps, int * status, int reserve_unlimited,
                                                                              uint64_t memory_budget_bytes )
{
    return rn_message_server_create( context, address, transport_config, message_config, memory, memory_bytes, per_client_send_kbps, status, reserve_unlimited, &memory_budget_bytes );
}

static struct rocketnet_message_server_t * rn_message_server_create( void * context, const char * address,
                                                                     const struct rocketnet_server_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps, int * status, int reserve_unlimited,
                                                                     const uint64_t * resolved_memory_budget )
{
    int ignored = ROCKETNET_STATUS_OK;
    if ( !status )
        status = &ignored;
    *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    if ( !rocketnet_api_null_guard( address ) )
        return NULL;

    // R6 (docs/CHANNELS.md 11.5): 0 takes server_send_kbps from the message
    // config; a positive value overrides it for every slot this create makes.
    // The create argument is uint32_t, so refuse above the maximum before
    // narrowing: a value in [2^31, 2^32) must not wrap negative and become
    // uncapped.
    if ( per_client_send_kbps > ROCKETNET_SEND_BUDGET_MAX_KBPS )
    {
        rocketnet_error( "message server create refused: send budget above maximum" );
        *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }
    int32_t kbps = (int32_t) per_client_send_kbps;
    if ( kbps == 0 )
    {
        rocketnet_message_config_t defaults;
        const rocketnet_message_config_t * resolved = message_config;
        if ( !resolved )
        {
            rocketnet_message_default_config( &defaults );
            resolved = &defaults;
        }
        kbps = resolved->server_send_kbps;
    }
    const int max_clients = rn_message_server_max_clients( transport_config );

    // refuse before the transport opens a socket
    if ( rn_message_object_required_bytes( message_config, max_clients, 1 ) == 0 || !( kbps <= 0 || rocketnet_send_budget_kbps_valid( (uint32_t) kbps ) ) )
    {
        rocketnet_error( "message server create refused: message config, client count or send budget" );
        *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }

    rocketnet_server_transport_t * transport = ( resolved_memory_budget && transport_config ) ?
        rocketnet_server_transport_create_resolved( context, address, transport_config, *resolved_memory_budget, status ) :
        rocketnet_server_transport_create_current_ex( context, address, transport_config, status );
    if ( !transport )
        return NULL;

    rn_message_link_t link;
    rn_message_server_link( &link, transport );
    rocketnet_message_server_t * server = (rocketnet_message_server_t *) rn_message_object_create( context, &link, 1, max_clients, message_config, memory, memory_bytes, kbps, 0.0, reserve_unlimited != 0 );
    *status = server ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_OUT_OF_MEMORY;
    return server;
}

void rocketnet_message_server_destroy( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    rn_message_object_destroy( &server->object );
}

struct rocketnet_server_transport_t * rocketnet_message_server_transport( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    return server->object.link.destroy == rn_server_link_destroy ? (rocketnet_server_transport_t *) server->object.link.transport : NULL;
}

int rocketnet_message_server_update( struct rocketnet_message_server_t * server, double time )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_UPDATE_OK;
    return rn_message_object_update( &server->object, time );
}

void rocketnet_message_server_send_packets( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    rn_message_object_send_packets( &server->object );
}

uint8_t * rocketnet_message_server_start( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    return rn_message_object_start( &server->object, client_index, channel, reliability, bytes );
}

uint32_t rocketnet_message_server_finish( struct rocketnet_message_server_t * server, int client_index, int bytes_written )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rn_message_object_finish( &server->object, client_index, bytes_written );
}

void rocketnet_message_server_abort( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    rn_message_object_abort( &server->object, client_index );
}

uint32_t rocketnet_message_server_send( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, const uint8_t * data, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;
    return rn_message_object_send( &server->object, client_index, channel, reliability, data, bytes );
}

int rocketnet_message_server_broadcast( struct rocketnet_message_server_t * server, int channel, int reliability, const uint8_t * data, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;

    // CHANNELS 11.4 Broadcast: a loop over the slots, each send the same copying
    // convenience as rocketnet_message_server_send. The R3 copy is one per
    // connected connection, into that connection's region; there is no new
    // state and no allocation. A slot that refuses (free, kicked or exhausted)
    // is skipped by the send path, which counts the refusal, and never retried.
    int queued = 0;
    for ( int i = 0; i < server->object.num_connections; i++ )
    {
        if ( rn_message_object_send( &server->object, i, channel, reliability, data, bytes ) != 0 )
            queued++;
    }
    return queued;
}

int rocketnet_message_server_receive( struct rocketnet_message_server_t * server, int * client_index, int * channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;
    if ( !rocketnet_api_null_guard( bytes ) )
        return 0;
    return rn_message_object_receive( &server->object, client_index, channel, data, bytes );
}

int rocketnet_message_server_receive_channel( struct rocketnet_message_server_t * server, int client_index, int channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;
    if ( !rocketnet_api_null_guard( bytes ) )
        return 0;
    return rn_message_object_receive_channel( &server->object, client_index, channel, data, bytes );
}

int rocketnet_message_server_receive_messages( struct rocketnet_message_server_t * server, struct rocketnet_message_batch_t * messages )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( messages ) )
        return 0;
    return rn_message_object_receive_messages( &server->object, messages );
}

int rocketnet_message_server_receipts( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_message_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( server ) || capacity < 0 ||
         ( capacity > 0 && !rocketnet_api_null_guard( out ) ) )
        return 0;
    rn_message_connection_t * connection = rn_message_object_connection( &server->object, client_index );
    if ( !connection || connection->hot->state != RN_MESSAGE_CONNECTION_CONNECTED )
        return 0;
    return rn_endpoint_take_receipts( connection, out, capacity );
}

void rocketnet_message_server_release( struct rocketnet_message_server_t * server, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    if ( !rocketnet_api_null_guard( data ) )
        return;
    rn_message_object_release( &server->object, data );
}

int rocketnet_message_server_set_send_kbps( struct rocketnet_message_server_t * server, int client_index, int32_t kbps )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rn_message_object_set_send_kbps( &server->object, client_index, kbps ) ? 1 : 0;
}

int rocketnet_message_server_disconnect_reason( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_DISCONNECT_REASON_NONE;
    return rn_message_object_disconnect_reason( &server->object, client_index );
}

int rocketnet_message_server_client_connected( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rn_message_object_connected( &server->object, client_index );
}

const struct rocketnet_counters_t * rocketnet_message_server_counters( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    return &server->object.counters;
}
