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
// Copies (R3): the payload is written once by the application, in place, into
// its region record (start, finish). THE COPIES (a): each packet write copies
// the record into the transport frame start_packet returned
// (rocketnet_message_writer_add, inside rn_ordered_write and
// rn_packet_builder_write), because a reliable record must outlive the frame
// for resends and the transport encrypts the frame in place. A message
// delivered in the update it arrives is a view into the transport's batch, and
// an ordered message held ahead of its turn is copied into the receive ring
// (THE COPIES (b), rn_channels_dispatch_ordered). rn_message_object_send is the
// one other copy: the convenience copies the caller's bytes into the record.

#include <string.h>

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_fragment.h"
#include "rocketnet_message.h"
#include "rocketnet_message_object.h"
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

    const uint64_t deliveries = (uint64_t) config->window_messages + (uint64_t) config->unreliable_deliveries;
    const uint64_t n = (uint64_t) num_connections;

    memset( layout, 0, sizeof( *layout ) );
    uint64_t offset = rn_message_align64( sizeof( rn_message_object_t ) > sizeof( rocketnet_message_server_t ) ? sizeof( rn_message_object_t ) : sizeof( rocketnet_message_server_t ) );
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
    (void) is_server;
    rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }
    rn_message_object_layout_t layout;
    if ( !rn_message_object_layout( config, num_connections, &layout ) )
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

static void rn_message_connection_reset( rn_message_object_t * object, rn_message_connection_t * connection )
{
    rn_region_reset( connection->region );

    rocketnet_send_budget_init( &connection->budget, object->default_kbps, object->time );

    const int max_packets = object->config.max_packets_per_update < ROCKETNET_PACKET_BUILDER_MAX_PACKETS ? object->config.max_packets_per_update : ROCKETNET_PACKET_BUILDER_MAX_PACKETS;
    rn_ordered_init( &connection->ordered, connection->region, &object->config, &connection->budget, max_packets, ROCKETNET_MESSAGE_SINGLE_BYTES );
    rn_packet_builder_init( &connection->unreliable_builder, connection->region, NULL, &connection->budget, 1, ROCKETNET_MESSAGE_SINGLE_BYTES );

    connection->connection_id = 0;
    connection->state = RN_MESSAGE_CONNECTION_FREE;
    connection->serial = 0;
    connection->pending_type = -1;
    connection->pending_descriptor = RN_DESCRIPTOR_NONE;
    connection->pending_bytes = 0;
    connection->unreliable_head = RN_DESCRIPTOR_NONE;
    connection->unreliable_tail = RN_DESCRIPTOR_NONE;
    connection->unreliable_count = 0;
    memset( connection->views, 0, connection->region->delivery_entries );
    memset( connection->cursors, 0, sizeof( uint32_t ) * connection->region->num_channels );
    connection->held = 0;
}

struct rn_message_connection_t * rn_message_object_connection( struct rn_message_object_t * object, int index )
{
    if ( !object || index < 0 || index >= object->num_connections )
        return NULL;
    return &object->connections[index];
}

// ---------------------------------------------------------------
//                     create and destroy
// ---------------------------------------------------------------

struct rn_message_object_t * rn_message_object_create( void * context, const struct rn_message_link_t * link, int is_server, int num_connections,
                                                       const struct rocketnet_message_config_t * config, void * memory, uint64_t memory_bytes,
                                                       uint32_t default_kbps, double time )
{
    rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }

    rn_message_object_layout_t layout;
    const bool laid_out = link && rn_message_object_layout( config, num_connections, &layout );
    const bool budget_valid = rocketnet_send_budget_kbps_valid( default_kbps ) && rocketnet_send_budget_time_valid( time );
    const bool memory_valid = !memory || ( laid_out && memory_bytes >= layout.total && ( (uintptr_t) memory % ROCKETNET_ALLOCATION_ALIGNMENT ) == 0 );

    if ( !laid_out || !budget_valid || !memory_valid )
    {
        if ( !laid_out )
            rocketnet_error( "message object create refused: a refused message config or connection count" );
        else if ( !budget_valid )
            rocketnet_error( "message object create refused: send budget %u kbps or time is outside its range", default_kbps );
        else
            rocketnet_error( "message object create refused: the memory block is short or not %d-aligned", ROCKETNET_ALLOCATION_ALIGNMENT );
        if ( link && link->destroy )
            link->destroy( link->transport );
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
            if ( link->destroy )
                link->destroy( link->transport );
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
    object->block = block;
    object->owns_block = owns_block ? 1 : 0;
    object->connections = (rn_message_connection_t *) ( block + layout.connections );
    rn_slots_init( &object->slots, num_connections );

    object->arena = rn_arena_create_in_block( block + layout.arena, layout.arena_bytes, config, num_connections, &object->counters );
    if ( !object->arena )
    {
        rocketnet_error( "message object create failed: the arena was refused" );
        if ( link->destroy )
            link->destroy( link->transport );
        if ( owns_block )
            rocketnet_free( context, block );
        return NULL;
    }

    for ( int i = 0; i < num_connections; i++ )
    {
        rn_message_connection_t * connection = &object->connections[i];
        connection->region = rn_arena_region( object->arena, i );
        connection->views = block + layout.views + layout.view_stride * (uint64_t) i;
        connection->cursors = (uint32_t *) ( block + layout.cursors + layout.cursor_stride * (uint64_t) i );
        connection->disconnect_reason = ROCKETNET_MESSAGE_ERROR_NONE;
        rn_message_connection_reset( object, connection );
    }

    // CHANNELS 11.3: the client's one connection is bound at create, because
    // the client transport hands it payloads from before CONNECTED is
    // reported, and it is never bound again.
    if ( !is_server )
    {
        object->connections[0].state = RN_MESSAGE_CONNECTION_CONNECTED;
        object->connections[0].connection_id = 1;
        object->slots.connection_id[0] = 1;
    }

    return object;
}

void rn_message_object_destroy( struct rn_message_object_t * object )
{
    if ( !object )
        return;
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
    if ( connection->pending_type == ROCKETNET_RELIABLE || connection->pending_type == ROCKETNET_RELIABLE_ORDERED ||
         connection->pending_type == ROCKETNET_RELIABLE_SEQUENCED ||
         rn_channels_reliable_receipt_base( connection->pending_type ) >= 0 ||
         ( connection->pending_type == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT && connection->ordered.channels.pending != RN_DESCRIPTOR_NONE ) )
        rn_channels_cancel( &connection->ordered.channels );
    else if ( ( connection->pending_type == ROCKETNET_UNRELIABLE || connection->pending_type == ROCKETNET_UNRELIABLE_SEQUENCED ||
                connection->pending_type == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT ) &&
              connection->pending_descriptor != RN_DESCRIPTOR_NONE )
        rn_region_send_free( connection->region, connection->pending_descriptor );
    connection->pending_type = -1;
    connection->pending_descriptor = RN_DESCRIPTOR_NONE;
    connection->pending_bytes = 0;
    object->pending_connection = -1;
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
    if ( connection->state != RN_MESSAGE_CONNECTION_CONNECTED )
        return;
    rn_message_connection_cancel_pending( object, index );
    connection->state = RN_MESSAGE_CONNECTION_KICKED;
    connection->disconnect_reason = reason;
    if ( object->link.disconnect )
        object->link.disconnect( object->link.transport, index );
    rocketnet_info( "message connection %d ended: reason %d", index, reason );
}

// ---------------------------------------------------------------
//                          update
// ---------------------------------------------------------------

// RNSlots Admit: an entry is applied only to the connection it was received
// for. The transport tags no batch entry with its connection (P9b is a later
// card), so the entry id is the slot's client id as the transport reports it.
// A slot bound or dropped by this update cannot identify its entries: they
// are presented with no tag (0, which no connection carries) and discarded
// (CHANNELS 10.2, 11.5).

static rn_message_connection_t * rn_message_admit( rn_message_object_t * object, const int * client_index, int k )
{
    const int index = client_index ? client_index[k] : 0;
    if ( index < 0 || index >= object->num_connections )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        return NULL;
    }
    rn_message_connection_t * connection = &object->connections[index];
    const uint64_t entry_connection_id = object->slots.changed[index] ? 0 : connection->connection_id;
    if ( rn_slots_admit( &object->slots, index, entry_connection_id ) != RN_SLOTS_APPLY )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        return NULL;
    }
    return connection;
}

int rn_message_object_update( struct rn_message_object_t * object, double time )
{
    int result = ROCKETNET_MESSAGE_OK;

    // RNSlots BeginUpdate, and RejectUnreleasedUpdate: every view handed out
    // in the last update must be released before this one. A held view is the
    // application's error: it asserts in a debug build and returns the named
    // error in release. The views die with the transport's batch either way,
    // so they are taken back here and memory never grows from a held view.
    uint32_t held = 0;
    for ( int i = 0; i < object->num_connections; i++ )
        held += object->connections[i].held;
    if ( held > 0 )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED, held );
        rocketnet_error( "message update with %u received message(s) not released", held );
        rocketnet_assert( held == 0 && "every received message is released before the next update" );
        result = ROCKETNET_ERROR_MESSAGE_NOT_RELEASED;
    }

    if ( time != time || time < object->time )
        time = object->time;
    object->time = time;

    // the transport's clock: a client transport seeded at 0.0 at create starts
    // counting at this first update, so its connect timers and token lifetime
    // run from the first update the game makes, whatever its clock reads then.
    // The object's own clock (budgets, resends, timeouts) is the game's time.
    if ( !object->clock_started )
    {
        object->clock_started = 1;
        object->clock_origin = object->link.clock_from_first_update ? time : 0.0;
    }

    for ( int i = 0; i < object->num_connections; i++ )
    {
        rn_message_connection_t * connection = &object->connections[i];

        // RNChannel EndUpdate: the last update's deliveries expire and their
        // receive descriptors return to the region (RNSlots Release's charge)
        const uint32_t delivered = connection->ordered.channels.deliveries;
        rn_ordered_end_update( &connection->ordered );
        rn_window_ack_receipt_begin_update( &connection->ordered.ack );
        memset( connection->views, 0, delivered < connection->region->delivery_entries ? delivered : connection->region->delivery_entries );
        memset( connection->cursors, 0, sizeof( uint32_t ) * connection->region->num_channels );
        connection->held = 0;

        // RNSlots ResetFree: a kicked connection's region is reset before the
        // transport's update can give the slot to another client, and the
        // slots machine sees the slot free until Observe reads the transport
        if ( connection->state == RN_MESSAGE_CONNECTION_KICKED )
        {
            const int reason = connection->disconnect_reason;
            rn_message_connection_reset( object, connection );
            rn_slots_observe( &object->slots, i, 0 );
            connection->disconnect_reason = reason;
        }
    }

    object->link.update( object->link.transport, time - object->clock_origin );

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

            const int reason = connection->disconnect_reason;
            rn_message_connection_reset( object, connection );
            connection->disconnect_reason = reason;

            if ( action == RN_SLOTS_RESET )
            {
                connection->state = RN_MESSAGE_CONNECTION_CONNECTED;
                connection->connection_id = id;
                connection->disconnect_reason = ROCKETNET_MESSAGE_ERROR_NONE;
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
            rn_message_connection_t * connection = rn_message_admit( object, client_index, k );
            if ( connection )
                rn_ordered_acks( &connection->ordered, &packet_ids[k], 1, time );
        }
    }

    for ( int i = 0; i < object->num_connections; i++ )
    {
        rn_message_connection_t * connection = &object->connections[i];
        if ( connection->state != RN_MESSAGE_CONNECTION_CONNECTED )
            continue;
        rn_window_ack_receipt_sweep( &connection->ordered.ack, time ); // RNReceipts SweepLoss
        rn_window_ack_receipt_end_update( &connection->ordered.ack );
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
            rn_message_connection_t * connection = rn_message_admit( object, client_index, k );
            if ( connection )
                rn_ordered_receive( &connection->ordered, payload_data[k], payload_bytes[k] );
        }
    }

    // RNSlots Kick, RNConnectionMemory Kick: exhaustion first, then a wire
    // violation, then a reliable message past its timeout (rn_ordered_decide)
    for ( int i = 0; i < object->num_connections; i++ )
    {
        rn_message_connection_t * connection = &object->connections[i];
        if ( connection->state != RN_MESSAGE_CONNECTION_CONNECTED )
            continue;
        const int reason = rn_ordered_decide( &connection->ordered );
        if ( reason != ROCKETNET_MESSAGE_ERROR_NONE )
            rn_message_object_kick( object, i, reason );
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

    rn_message_connection_t * connection = &object->connections[index];
    if ( connection->state != RN_MESSAGE_CONNECTION_CONNECTED || object->pending_connection >= 0 ||
         channel < 0 || channel >= object->config.num_channels || bytes < 1 )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        return NULL;
    }

    rn_region_t * region = connection->region;
    uint8_t * pointer = NULL;

    if ( reliability == ROCKETNET_RELIABLE || reliability == ROCKETNET_RELIABLE_ORDERED ||
         reliability == ROCKETNET_RELIABLE_SEQUENCED ||
         rn_channels_reliable_receipt_base( reliability ) >= 0 )
    {
        pointer = rn_channels_start( &connection->ordered.channels, (uint32_t) channel, reliability, (uint32_t) bytes );
        if ( !pointer )
        {
            if ( rn_region_exhausted( region ) )
                rn_message_object_kick( object, index, ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );     // R5
            else
                rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
            return NULL;
        }
        connection->pending_descriptor = connection->ordered.channels.pending;
    }
    else if ( reliability == ROCKETNET_UNRELIABLE || reliability == ROCKETNET_UNRELIABLE_SEQUENCED ||
              reliability == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT )
    {
        if ( rn_channels_send_check( &connection->ordered.channels, reliability, (uint32_t) channel ) != ROCKETNET_CHANNEL_SEND_OK )
            return NULL;
        // Code 1 may occupy one large RECORDS payload. Its u16 length and
        // the transport's configured payload cap are permanent refusal
        // boundaries; checking here keeps an impossible send off the FIFO.
        const int header = 3 + 3 + 3 + ( channel >= 256 ? 2 : 1 );
        const int transport_max = object->link.max_payload_bytes ?
            object->link.max_payload_bytes( object->link.transport ) : ROCKETNET_MAX_PAYLOAD_BYTES;
        const bool too_large = reliability == ROCKETNET_UNRELIABLE_SEQUENCED ?
            bytes > ROCKETNET_MESSAGE_MAX_RECORD_BYTES || bytes > object->config.max_message_bytes ||
            bytes + header + 1 > transport_max : (uint32_t) bytes > object->unreliable_limit;
        if ( too_large )
        {
            if ( reliability != ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT )
            {
                // Non-receipt unreliable sends cannot exceed their wire limit.
                rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
                return NULL;
            }
            // RNReceipts EffectiveKind: a large public type 5 reserves through
            // the internal reliable code-2 block path, preserving code 5's
            // receipt serial and reliable-only completion.
            pointer = rn_channels_start( &connection->ordered.channels, (uint32_t) channel, ROCKETNET_RELIABLE, (uint32_t) bytes );
            if ( !pointer )
            {
                if ( rn_region_exhausted( region ) )
                    rn_message_object_kick( object, index, ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
                else
                    rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
                return NULL;
            }
            connection->pending_descriptor = connection->ordered.channels.pending;
        }
        else if ( rn_region_exhausted( region ) )
        {
            rn_message_object_kick( object, index, ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
            return NULL;
        }
        else
        {
            const uint32_t d = bytes >= RN_SPAN_MIN_BYTES ? rn_region_send_alloc_span( region, bytes ) : rn_region_send_alloc( region, bytes );
            if ( d == RN_DESCRIPTOR_NONE )
            {
                // RNConnectionMemory Exhaust then Kick (R5): the region has no room
                // for a send it must not drop
                rn_message_object_kick( object, index, ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
                return NULL;
            }
            rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
            descriptor->channel = (uint16_t) channel;
            descriptor->reliability = (uint8_t) reliability;
            descriptor->priority = ROCKETNET_PRIORITY_MEDIUM;
            descriptor->fragments = 1;
            descriptor->next = RN_DESCRIPTOR_NONE;
            connection->pending_descriptor = d;
            pointer = rn_region_send_write_pointer( region, d );
        }
    }
    else
    {
        // the remaining types are refused until their cards enable them (7.5)
        if ( reliability >= 0 && reliability < ROCKETNET_CHANNEL_NUM_TYPES )
            rn_channels_send_check( &connection->ordered.channels, reliability, (uint32_t) channel );
        else
            rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        return NULL;
    }

    connection->pending_type = reliability;
    connection->pending_bytes = (uint32_t) bytes;
    object->pending_connection = index;
    return pointer;
}

uint32_t rn_message_object_finish( struct rn_message_object_t * object, int index, int bytes )
{
    // RNChannel Enqueue's second half: the message joins its channel's queue
    // in FIFO order and takes its receipt serial (7.4: nonzero for every send)
    if ( index < 0 || index >= object->num_connections || object->pending_connection != index )
        return 0;

    rn_message_connection_t * connection = &object->connections[index];
    if ( bytes < 1 || (uint32_t) bytes > connection->pending_bytes )
        return 0;                                               // the start stays pending

    uint32_t serial = connection->serial + 1;
    if ( serial == 0 )
        serial = 1;

    if ( connection->pending_type == ROCKETNET_RELIABLE || connection->pending_type == ROCKETNET_RELIABLE_ORDERED ||
         connection->pending_type == ROCKETNET_RELIABLE_SEQUENCED ||
         rn_channels_reliable_receipt_base( connection->pending_type ) >= 0 ||
         ( connection->pending_type == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT && connection->ordered.channels.pending != RN_DESCRIPTOR_NONE &&
           (uint32_t) bytes > object->unreliable_limit ) )
    {
        const uint32_t d = connection->ordered.channels.pending;
        if ( !rn_channels_finish( &connection->ordered.channels, (uint32_t) bytes ) )
            return 0;
        rn_send_descriptor_t * descriptor = &connection->region->send_descriptors[d];
        descriptor->serial = serial;
        if ( connection->pending_type == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT )
            descriptor->flags |= RN_SEND_FLAG_RECEIPT_PROMOTED; // RNReceipts EffectiveKind: reliable completion, no LOSS
    }
    else
    {
        rn_region_t * region = connection->region;
        const uint32_t d = connection->pending_descriptor;
        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        if ( connection->ordered.channels.pending == d )
        {
            // A caller may reserve a block and finish fewer bytes. Move the
            // same region descriptor to the single-packet route without a copy.
            rocketnet_assert( connection->pending_type == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT );
            connection->ordered.channels.pending = RN_DESCRIPTOR_NONE;
            descriptor->reliability = ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT;
            descriptor->priority = ROCKETNET_PRIORITY_MEDIUM;
            descriptor->fragments = 1;
        }
        descriptor->bytes = (uint32_t) bytes;
        descriptor->serial = serial;
        descriptor->next = RN_DESCRIPTOR_NONE;
        if ( connection->pending_type == ROCKETNET_UNRELIABLE_SEQUENCED )
        {
            // RNChannelTypes!Enqueue: a sequenced send stays in the unreliable
            // queue and takes no reliable number or resend entry.
            descriptor->sequencing = rn_channels_send_sequenced( &connection->ordered.channels, descriptor->channel, &descriptor->ordering );
        }
        if ( connection->unreliable_tail == RN_DESCRIPTOR_NONE )
            connection->unreliable_head = d;
        else
            region->send_descriptors[connection->unreliable_tail].next = d;
        connection->unreliable_tail = d;
        connection->unreliable_count++;
    }

    connection->serial = serial;
    connection->pending_type = -1;
    connection->pending_descriptor = RN_DESCRIPTOR_NONE;
    connection->pending_bytes = 0;
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

// Whether the ordered path has a packet to write now: a message queued for
// its first write, or a number at the head of either resend FIFO that is
// unsent or past its class's delay, min( rto, resend_interval ) for a small
// record and the RTO for a chunk of a block (tla/RNResendTiming.tla Due). It
// keeps a frame from being started for nothing.

static int rn_message_ordered_fifo_due( const rn_region_t * region, uint32_t cursor, double delay, double now )
{
    for ( int steps = 0; cursor != RN_RESEND_NONE; steps++ )
    {
        if ( steps >= 64 )
            return 1;                                           // a long acked prefix: let the write walk it
        const rn_send_number_t * slot = &region->send_numbers[cursor];
        if ( !slot->acked )
            return ( slot->last_send <= 0.0 || now >= slot->last_send + delay ) ? 1 : 0;
        cursor = region->resend_fifo[cursor].next;
    }
    return 0;
}

static bool rn_message_ordered_due( const rn_message_connection_t * connection, double now )
{
    const rn_ordered_t * ordered = &connection->ordered;
    if ( ordered->channels.ready_head != RN_DESCRIPTOR_NONE )
        return true;

    const rn_window_ack_t * ack = &ordered->ack;
    const rn_region_t * region = connection->region;
    return rn_message_ordered_fifo_due( region, ack->resend_head, rn_window_ack_resend_delay( ack ), now ) ||
           rn_message_ordered_fifo_due( region, ack->chunk_head, rn_window_ack_chunk_resend_delay( ack ), now );
}

static void rn_message_unreliable_unlink( rn_message_connection_t * connection, uint32_t d, uint32_t previous )
{
    rn_region_t * region = connection->region;
    const uint32_t next = region->send_descriptors[d].next;
    if ( previous == RN_DESCRIPTOR_NONE )
        connection->unreliable_head = next;
    else
        region->send_descriptors[previous].next = next;
    if ( connection->unreliable_tail == d )
        connection->unreliable_tail = previous;
    connection->unreliable_count--;
    rn_region_send_free( region, d );
}

static void rn_message_unreliable_drop_all( rn_message_object_t * object, rn_message_connection_t * connection, int counter, uint32_t already_counted )
{
    const uint32_t count = connection->unreliable_count;
    while ( connection->unreliable_head != RN_DESCRIPTOR_NONE )
    {
        const rn_send_descriptor_t * descriptor = &connection->region->send_descriptors[connection->unreliable_head];
        if ( descriptor->reliability == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT )
            rn_window_ack_receipt_emit_loss( &connection->ordered.ack, descriptor->serial ); // local budget drop before wire send
        rn_message_unreliable_unlink( connection, connection->unreliable_head, RN_DESCRIPTOR_NONE );
    }
    if ( count > already_counted )
        rn_message_count( object, counter, count - already_counted );
}

static int rn_message_sequenced_payload_bytes( const rn_send_descriptor_t * descriptor )
{
    const int header = 3 + 3 + 3 + ( descriptor->channel >= 256 ? 2 : 1 );
    return (int) descriptor->bytes + header + 1; // one RECORDS record and form byte
}

// RNChannelTypes!WritePacket: a large code-1 state is one unreliable RECORDS
// payload. The existing transport splits it into type-9 fragments; no layer-6
// block or reliable number is created. Charge the whole fragment plan before
// writing, so an over-budget state cannot send only a prefix of its fragments.
static bool rn_message_write_large_sequenced( rn_message_object_t * object, int index, rn_message_connection_t * connection )
{
    rn_region_t * region = connection->region;
    const uint32_t d = connection->unreliable_head;
    const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    const int payload_bytes = rn_message_sequenced_payload_bytes( descriptor );
    int fragment_count, stride;
    if ( !rocketnet_fragment_plan( payload_bytes, &fragment_count, &stride ) )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
        rn_message_unreliable_unlink( connection, d, RN_DESCRIPTOR_NONE );
        return true;
    }

    int admitted_wire_bytes = 0;
    for ( int i = 0; i < fragment_count; i++ )
    {
        const int fragment_bytes = i + 1 < fragment_count ? stride : payload_bytes - stride * ( fragment_count - 1 );
        const int maximum = rocketnet_send_budget_max_wire_bytes( fragment_bytes + ROCKETNET_FRAGMENT_HEADER_BYTES );
        if ( maximum == 0 )
            return false;
        admitted_wire_bytes += maximum;
    }

    rocketnet_send_budget_t * budget = &connection->budget;
    if ( budget->rank_reached < rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_UNRELIABLE ) )
        budget->rank_reached = rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_UNRELIABLE );
    if ( budget->reliable_waiting || (double) admitted_wire_bytes > budget->tokens )
    {
        budget->unreliable_dropped++;
        budget->unreliable_dropped_bytes += (uint64_t) admitted_wire_bytes;
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        rn_message_unreliable_unlink( connection, d, RN_DESCRIPTOR_NONE );
        return true;
    }

    rocketnet_packet_context_t context;
    uint8_t * frame = object->link.start_packet( object->link.transport, index, payload_bytes, &context );
    if ( !frame )
        return false; // transient transport room shortage; keep the queue head

    rocketnet_message_record_t record = {};
    record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
    record.channel = descriptor->channel;
    record.ordering = descriptor->ordering & ROCKETNET_MESSAGE_U24_MASK;
    record.sequencing = descriptor->sequencing & ROCKETNET_MESSAGE_U24_MASK;
    record.data = rn_region_send_write_pointer( region, d );
    record.bytes = (int) descriptor->bytes;
    rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, frame + context.payload_index, payload_bytes );
    if ( !rocketnet_message_writer_add( &writer, &record ) || rocketnet_message_writer_finish( &writer ) != payload_bytes )
    {
        object->link.abort_packet( object->link.transport, &context );
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
        rn_message_unreliable_unlink( connection, d, RN_DESCRIPTOR_NONE );
        return true;
    }

    // The plan's maximum wire charge is conservative; settle with the sum
    // returned by the transport after it stages every sealed fragment.
    budget->tokens -= (double) admitted_wire_bytes;
    budget->bytes_spent += (uint64_t) admitted_wire_bytes;
    const int actual_wire_bytes = object->link.finish_packet( object->link.transport, &context, payload_bytes );
    if ( actual_wire_bytes > 0 )
        rocketnet_send_budget_settle( budget, admitted_wire_bytes, actual_wire_bytes );
    else
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
    rn_message_unreliable_unlink( connection, d, RN_DESCRIPTOR_NONE );
    return true;
}

// RNChannel WritePacket with RNSendBudget Send, Wait and Drop, for one
// connection: one frame from the transport at a time, written by layer 6 in
// place (THE COPIES (a)), finished, and its packet id committed to the sent
// map (RNSlots Send). Ordered progress and retransmits first, then the
// unreliable records, under the connection's budget (R6).

static void rn_message_connection_write( rn_message_object_t * object, int index )
{
    rn_message_connection_t * connection = &object->connections[index];
    rn_message_link_t * link = &object->link;
    const double now = object->time;
    const int max_packets = object->config.max_packets_per_update;

    rocketnet_send_budget_begin_update( &connection->budget, now );

    int written = 0;

    // the reliable path, one packet per call so a frame is started only for a
    // packet that is written; every resend waits the same delay, with no
    // back-off (tla/RNResendTiming.tla)
    rn_window_ack_t * ack = &connection->ordered.ack;
    rn_window_ack_gaps_update( ack, now );                  // the resend gap record (development builds only)

    while ( written < max_packets && rn_message_ordered_due( connection, now ) )
    {
        rocketnet_packet_context_t context;
        uint8_t * frame = link->start_packet( link->transport, index, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        if ( !frame )
            break;
        uint8_t * payload = frame + context.payload_index;

        const int packets = rn_ordered_write( &connection->ordered, now, &payload, 1 );
        if ( packets == 0 )
        {
            link->abort_packet( link->transport, &context );
            break;
        }

        const int wire_bytes = link->finish_packet( link->transport, &context, (int) connection->ordered.builder.packet[0].payload_bytes );
        if ( wire_bytes > 0 )
        {
            rn_ordered_commit( &connection->ordered, 0, context.packet_id, now );
            rn_ordered_settle( &connection->ordered, 0, wire_bytes );
        }
        else
        {
            rn_ordered_packet_failed( &connection->ordered, 0, now );     // 6.4 Failed send: resent first next update
            written++;
            break;
        }
        written++;
    }

    // a reliable packet the budget could not pay waits; nothing after it
    // spends the bucket this update (RNSendBudget Wait), so this update's
    // unreliable sends are over budget: dropped and counted (Drop, R6)
    if ( connection->budget.reliable_waiting )
    {
        rn_message_unreliable_drop_all( object, connection, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET, 0 );
        return;
    }

    // the unreliable records, packed several to a packet
    rn_region_t * region = connection->region;
    while ( written < max_packets && connection->unreliable_count > 0 )
    {
        const rn_send_descriptor_t * first = &region->send_descriptors[connection->unreliable_head];
        if ( first->reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED &&
             rn_message_sequenced_payload_bytes( first ) > ROCKETNET_MAX_PAYLOAD_BYTES )
        {
            if ( !rn_message_write_large_sequenced( object, index, connection ) )
                break;
            written++;
            continue;
        }
        rn_packet_item_t items[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
        uint32_t descriptors[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
        int count = 0;
        for ( uint32_t d = connection->unreliable_head; d != RN_DESCRIPTOR_NONE && count < ROCKETNET_PACKET_BUILDER_MAX_RECORDS; d = region->send_descriptors[d].next )
        {
            const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
            if ( descriptor->reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED &&
                 rn_message_sequenced_payload_bytes( descriptor ) > ROCKETNET_MAX_PAYLOAD_BYTES )
                break; // send the preceding small records first
            rn_packet_item_t * item = &items[count];
            memset( item, 0, sizeof( *item ) );
            item->data = rn_region_send_write_pointer( region, d );
            item->bytes = descriptor->bytes;
            item->number = ROCKETNET_PACKET_NUMBER_NONE;
            item->descriptor = d;
            item->channel = descriptor->channel;
            item->code = descriptor->reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED ?
                         ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED : ROCKETNET_MESSAGE_CODE_UNRELIABLE;
            item->ordering = descriptor->ordering & ROCKETNET_MESSAGE_U24_MASK;
            item->sequencing = descriptor->sequencing & ROCKETNET_MESSAGE_U24_MASK;
            item->send_class = ROCKETNET_SEND_CLASS_UNRELIABLE;
            descriptors[count] = d;
            count++;
        }

        rocketnet_packet_context_t context;
        uint8_t * frame = link->start_packet( link->transport, index, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        if ( !frame )
            break;
        uint8_t * payload = frame + context.payload_index;

        const int packets = rn_packet_builder_write( &connection->unreliable_builder, items, count, &payload, 1 );
        if ( packets == 0 )
        {
            link->abort_packet( link->transport, &context );
            // RNSendBudget Drop: unreliable sends over budget are dropped and
            // counted, all of this update's (R6)
            if ( connection->unreliable_builder.unreliable_dropped > 0 )
                rn_message_unreliable_drop_all( object, connection, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET, connection->unreliable_builder.unreliable_dropped );
            break;
        }

        const int wire_bytes = link->finish_packet( link->transport, &context, (int) connection->unreliable_builder.packet[0].payload_bytes );
        if ( wire_bytes > 0 )
        {
            uint32_t serials[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
            uint32_t receipts = 0;
            for ( int i = 0; i < count; i++ )
            {
                if ( !items[i].taken )
                    continue;
                const rn_send_descriptor_t * descriptor = &region->send_descriptors[descriptors[i]];
                if ( descriptor->reliability == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT )
                    serials[receipts++] = descriptor->serial;
            }
            if ( receipts )
                rn_window_ack_commit_receipt_packet( ack, context.packet_id, serials, receipts, now ); // RNReceipts WritePacket
            rn_packet_builder_settle( &connection->unreliable_builder, 0, wire_bytes );
        }
        else
        {
            rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED, connection->unreliable_builder.packet[0].records );
            for ( int i = 0; i < count; i++ )
                if ( items[i].taken && region->send_descriptors[descriptors[i]].reliability == ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT )
                    rn_window_ack_receipt_emit_loss( ack, region->send_descriptors[descriptors[i]].serial );
        }

        // the written records' room returns to the region (9.3 step 3)
        for ( int i = 0; i < count; i++ )
        {
            if ( !items[i].taken )
                continue;
            uint32_t previous = RN_DESCRIPTOR_NONE;
            for ( uint32_t d = connection->unreliable_head; d != RN_DESCRIPTOR_NONE; previous = d, d = region->send_descriptors[d].next )
            {
                if ( d == descriptors[i] )
                {
                    rn_message_unreliable_unlink( connection, d, previous );
                    break;
                }
            }
        }
        written++;
    }
}

void rn_message_object_send_packets( struct rn_message_object_t * object )
{
    for ( int i = 0; i < object->num_connections; i++ )
    {
        rn_message_connection_t * connection = &object->connections[i];
        if ( connection->state != RN_MESSAGE_CONNECTION_CONNECTED )
            continue;
        // the client transport accepts sends only while CONNECTED (11.3):
        // until then the queue waits, unnumbered
        if ( !object->is_server && object->link.connection( object->link.transport, 0 ) == 0 )
            continue;
        rn_message_connection_write( object, i );
    }
    object->link.send_packets( object->link.transport );
}

// ---------------------------------------------------------------
//                          receive and release
// ---------------------------------------------------------------

int rn_message_object_receive( struct rn_message_object_t * object, int index, int channel, const uint8_t ** data, int * bytes )
{
    // RNSlots EndUpdate: the update's deliveries go to the application, each a
    // view -- into the transport's batch, or into the receive ring for a
    // message held ahead of its turn -- valid until the next update and held
    // until released
    *data = NULL;
    *bytes = 0;
    if ( index < 0 || index >= object->num_connections || channel < 0 || channel >= object->config.num_channels )
        return 0;

    rn_message_connection_t * connection = &object->connections[index];
    if ( connection->state != RN_MESSAGE_CONNECTION_CONNECTED )
        return 0;

    const uint32_t deliveries = connection->ordered.channels.deliveries;
    const rn_delivery_t * table = connection->region->deliveries;
    for ( uint32_t k = connection->cursors[channel]; k < deliveries; k++ )
    {
        if ( table[k].channel != (uint16_t) channel || connection->views[k] != RN_MESSAGE_VIEW_WAITING )
            continue;
        connection->views[k] = RN_MESSAGE_VIEW_HELD;
        connection->held++;
        connection->cursors[channel] = k + 1;
        *data = table[k].data;
        *bytes = table[k].bytes;
        return 1;
    }
    connection->cursors[channel] = deliveries;
    return 0;
}

void rn_message_object_release( struct rn_message_object_t * object, int index, const uint8_t * data )
{
    // RNSlots Release: the view goes back, and with it the region's charge for
    // the message at the next update's EndUpdate
    if ( index < 0 || index >= object->num_connections || !data )
        return;

    rn_message_connection_t * connection = &object->connections[index];
    const uint32_t deliveries = connection->ordered.channels.deliveries;
    const rn_delivery_t * table = connection->region->deliveries;
    for ( uint32_t k = 0; k < deliveries; k++ )
    {
        if ( table[k].data == data && connection->views[k] == RN_MESSAGE_VIEW_HELD )
        {
            connection->views[k] = RN_MESSAGE_VIEW_RELEASED;
            connection->held--;
            return;
        }
    }

    // not a held view of this connection: the caller's error, counted
    rocketnet_api_misuse_record();
}

bool rn_message_object_set_send_kbps( struct rn_message_object_t * object, int index, uint32_t kbps )
{
    if ( index < 0 || index >= object->num_connections )
        return false;
    return rocketnet_send_budget_set_kbps( &object->connections[index].budget, kbps, object->time );
}

int rn_message_object_disconnect_reason( const struct rn_message_object_t * object, int index )
{
    if ( index < 0 || index >= object->num_connections )
        return ROCKETNET_MESSAGE_ERROR_NONE;
    return object->connections[index].disconnect_reason;
}

int rn_message_object_connected( const struct rn_message_object_t * object, int index )
{
    if ( index < 0 || index >= object->num_connections )
        return 0;
    return object->connections[index].state == RN_MESSAGE_CONNECTION_CONNECTED ? 1 : 0;
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

static uint64_t rn_server_link_connection( void * transport, int client_index )
{
    rocketnet_server_transport_t * server = (rocketnet_server_transport_t *) transport;
    if ( !rocketnet_server_transport_is_client_connected( server, client_index ) )
        return 0;
    const uint64_t id = rocketnet_server_transport_client_id( server, client_index );
    return id != 0 ? id : 1;
}

static void rn_server_link_disconnect( void * transport, int client_index )
{
    rocketnet_server_transport_disconnect_client( (rocketnet_server_transport_t *) transport, client_index );
}

static int rn_server_link_state( void * transport )
{
    return rocketnet_server_transport_state( (rocketnet_server_transport_t *) transport );
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
    link->max_payload_bytes = rn_server_link_max_payload_bytes;
    link->start_packet = rn_server_link_start_packet;
    link->finish_packet = rn_server_link_finish_packet;
    link->abort_packet = rn_server_link_abort_packet;
    link->send_packets = rn_server_link_send_packets;
    link->connection = rn_server_link_connection;
    link->disconnect = rn_server_link_disconnect;
    link->state = rn_server_link_state;
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
    if ( !rocketnet_api_null_guard( address ) )
        return NULL;

    const uint32_t kbps = per_client_send_kbps != 0 ? per_client_send_kbps : ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS;
    const int max_clients = rn_message_server_max_clients( transport_config );

    // refuse before the transport opens a socket
    if ( rn_message_object_required_bytes( message_config, max_clients, 1 ) == 0 || !rocketnet_send_budget_kbps_valid( kbps ) )
    {
        rocketnet_error( "message server create refused: message config, client count or send budget" );
        return NULL;
    }

    rocketnet_server_transport_t * transport = rocketnet_server_transport_create( context, address, transport_config );
    if ( !transport )
        return NULL;

    rn_message_link_t link;
    rn_message_server_link( &link, transport );
    return (rocketnet_message_server_t *) rn_message_object_create( context, &link, 1, max_clients, message_config, memory, memory_bytes, kbps, 0.0 );
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
        return ROCKETNET_MESSAGE_OK;
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

int rocketnet_message_server_receive( struct rocketnet_message_server_t * server, int client_index, int channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !rocketnet_api_null_guard( data ) )
        return 0;
    if ( !rocketnet_api_null_guard( bytes ) )
        return 0;
    return rn_message_object_receive( &server->object, client_index, channel, data, bytes );
}

int rocketnet_message_server_receipts( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_message_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( out ) || capacity < 1 )
        return 0;
    rn_message_connection_t * connection = rn_message_object_connection( &server->object, client_index );
    if ( !connection || connection->state != RN_MESSAGE_CONNECTION_CONNECTED )
        return 0;
    return rn_window_ack_receipts( &connection->ordered.ack, out, capacity );
}

void rocketnet_message_server_release( struct rocketnet_message_server_t * server, int client_index, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    if ( !rocketnet_api_null_guard( data ) )
        return;
    rn_message_object_release( &server->object, client_index, data );
}

int rocketnet_message_server_set_send_kbps( struct rocketnet_message_server_t * server, int client_index, uint32_t kbps )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rn_message_object_set_send_kbps( &server->object, client_index, kbps ) ? 1 : 0;
}

int rocketnet_message_server_disconnect_reason( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_MESSAGE_ERROR_NONE;
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
