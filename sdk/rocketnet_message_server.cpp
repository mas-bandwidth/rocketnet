/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// the server message object and the object core it
// shares with the client: the client is the same object with one connection
// so the core lives here and sdk/rocketnet_message_client.cpp adds the
// client's link and C API.
//
// The spec is proofs/tla/RNSlots.tla (the update, slot observation, admission, kick,
// reset and release) with proofs/tla/RNConnectionMemory.tla (exhaustion and reuse);
// each function names the action it implements (R7). The connection's own
// message state is layers 3 to 6 (sdk/rocketnet_channel.h, proofs/tla/RNChannel.tla).
//
// Memory (R2): rn_message_object_create makes one block -- or takes the
// caller's -- holding the object, its connections, their receive bookkeeping
// and the arena of regions. The only allocator calls in this file are that
// one allocation, in create, and destroy's matching free.
//
// Copies (R3): a fitting unreliable message is written in place in the packet
// buffer, with no copy between the message layer and the transport: start
// reserves room at the committed end of the connection's open transport
// frame and hands out a pointer there, finish writes the record header in
// front and commits it, and later records append behind it until the frame
// is full or a flush seals it (docs/OUTGOING-BUILD.md, proofs/tla/RNOutgoingBuild.tla
// StartInPlace, FinishInPlace, SealBuild). Every other message is
// written once by the application, in place, into its region record (start,
// finish). THE COPIES (a): each packet write copies
// the record into the transport frame start_packet returned
// (rocketnet_message_writer_add, inside rn_ordered_write and
// rn_packet_builder_write), because a reliable record must outlive the frame
// for resends and the transport encrypts the frame in place. A message
// delivered in the update it arrives is a view into the transport's batch, and
// an ordered message held ahead of its turn is copied into the receive ring
// (THE COPIES (b), rn_channels_dispatch_ordered). rn_message_object_send is
// one other copy: the convenience copies the caller's bytes into the record.
// The last: while a connection's send budget holds less than one full-size
// frame, its payload is written into a stack buffer and copied into the
// frame, so that a payload the budget refuses takes no frame
// (rn_message_connection_write).

#include <string.h>

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_fill_phase.h"
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
    uint64_t slot_connection_ids;   // the slots machine's arrays (rn_slots_t), one entry per connection
    uint64_t slot_changed;
    uint64_t send_writers;          // the send pass's writer list, two int32_t per connection (none for one connection)
    uint64_t views;                 // offset of the first connection's views; each connection's next at + view_stride
    uint64_t view_stride;
    uint64_t arena;
    uint64_t arena_bytes;
    uint64_t total;
};

static bool rn_message_object_layout( const rocketnet_message_config_t * config, int num_connections, rn_message_object_layout_t * layout )
{
    if ( num_connections < 1 || num_connections > ROCKETNET_MAX_CLIENTS )
    {
        return false;
    }
    if ( !rocketnet_message_validate_config( config ) )
    {
        return false;
    }

    const uint64_t arena_bytes = rn_arena_required_bytes( config, num_connections );
    if ( arena_bytes == 0 )
    {
        return false;
    }

    const uint64_t deliveries = (uint64_t) config->reliable_messages_in_flight + (uint64_t) config->unreliable_messages_per_update;
    const uint64_t n = (uint64_t) num_connections;

    memset( layout, 0, sizeof( *layout ) );

    uint64_t offset = rn_message_align64( sizeof( rn_message_object_t ) > sizeof( rocketnet_message_server_t ) ? sizeof( rn_message_object_t ) : sizeof( rocketnet_message_server_t ) );
    layout->hot_connections = offset;
    offset += rn_message_align64( sizeof( rn_message_connection_hot_t ) * n );
    layout->connections = offset;
    offset += rn_message_align64( sizeof( rn_message_connection_t ) * n );
    layout->slot_connection_ids = offset;
    offset += rn_message_align64( sizeof( uint64_t ) * n );
    layout->slot_changed = offset;
    offset += rn_message_align64( sizeof( uint8_t ) * n );
    layout->send_writers = offset;
    offset += n > 1 ? rn_message_align64( sizeof( int32_t ) * 2 * n ) : 0;    // one connection: two entries on the send pass's stack
    layout->views = offset;
    layout->view_stride = rn_message_align64( deliveries );
    offset += layout->view_stride * n;
    layout->arena = offset;
    layout->arena_bytes = arena_bytes;
    offset += rn_message_align64( arena_bytes );
    layout->total = offset;

    return true;
}

// The config a connection region is sized with: only this direction's send
// budget, resolved as create resolves it (R6, standard/MESSAGES.md m9.4): a
// kbps of 0 takes the config's field, a positive value overrides it, and the
// public unlimited runtime ceiling reserves nothing. Returns the resolved
// budget. required_memory and create both size through here, so a block of
// required_memory bytes for a kbps is the block create needs for it.
static int32_t rn_message_object_sized_config( const struct rocketnet_message_config_t * config, int is_server, int32_t kbps,
                                               bool reserve_unlimited, rocketnet_message_config_t * sized )
{
    if ( kbps == 0 )
    {
        kbps = is_server ? config->server_send_kbps : config->client_send_kbps;
    }

    const int reserve_kbps = reserve_unlimited ? 0 : kbps;
    *sized = *config;
    sized->client_send_kbps = reserve_kbps;
    sized->server_send_kbps = reserve_kbps;
    return kbps;
}

uint64_t rn_message_object_required_bytes( const struct rocketnet_message_config_t * config, int num_connections, int is_server,
                                           int32_t kbps, bool reserve_unlimited )
{
    rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }

    rocketnet_message_config_t sized;
    rn_message_object_sized_config( config, is_server, kbps, reserve_unlimited, &sized );

    rn_message_object_layout_t layout;
    if ( !rn_message_object_layout( &sized, num_connections, &layout ) )
    {
        return 0;
    }

    return layout.total;
}

static void rn_message_count( rn_message_object_t * object, int counter, uint64_t amount = 1 )
{
    rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &object->counters.values[counter], amount );
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
    {
        return NULL;
    }
    return &object->connections[index];
}

bool rn_message_object_conditions( const struct rn_message_object_t * object, int index, struct rocketnet_conditions_t * conditions )
{
    if ( !object || index < 0 || index >= object->num_connections || object->hot_connections[index].state != RN_MESSAGE_CONNECTION_CONNECTED )
    {
        return false;
    }
    const struct rn_endpoint_t * endpoint = &object->connections[index];
    const struct rn_ordered_t * ordered = &endpoint->ordered;
    conditions->reliable_in_flight = ordered->send.next >= ordered->send.U ? ordered->send.next - ordered->send.U : 0;
    conditions->send_refused = endpoint->send_refused;
    if ( conditions->reliable_in_flight > 0 && ordered->ack.has_oldest )
    {
        const double from = ordered->ack.last_progress > ordered->ack.oldest_first_send ? ordered->ack.last_progress : ordered->ack.oldest_first_send;
        conditions->seconds_since_progress = object->time - from;
    }
    else
    {
        conditions->seconds_since_progress = -1.0;
    }
    return true;
}

bool rocketnet_message_server_conditions( const struct rocketnet_message_server_t * server, int client_index, struct rocketnet_conditions_t * conditions )
{
    return rn_message_object_conditions( &server->object, client_index, conditions );
}

#if ROCKETNET_DEV
void rn_message_object_delivery_capture( struct rn_message_object_t * object, int index, struct rocketnet_delivery_capture_t * capture )
{
    if ( !object || !capture || index < 0 || index >= object->num_connections )
    {
        return;
    }
    if ( object->hot_connections[index].state != RN_MESSAGE_CONNECTION_CONNECTED )
    {
        return;
    }

    const struct rn_ordered_t * ordered = &object->connections[index].ordered;
    capture->reliable_unacked = ordered->send.next >= ordered->send.U ? ordered->send.next - ordered->send.U : 0;
    capture->reliable_resend_queued = ordered->ack.resend_count;
    capture->reliable_oldest_unacked_age_seconds = capture->reliable_unacked > 0 && ordered->ack.has_oldest ? object->time - ordered->ack.oldest_first_send : -1.0;
    capture->srtt_seconds = ordered->ack.srtt;
    capture->resend_delay_seconds = ordered->ack.resend_delay;
    capture->window_stalled = ordered->send.stalled ? 1 : 0;
    capture->budget_reliable_waiting = ordered->budget ? ordered->budget->reliable_waiting : 0;
    capture->budget_tokens = ordered->budget ? ordered->budget->tokens : 0.0;
}

void rocketnet_message_server_delivery_capture( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( capture ) )
    {
        return;
    }
    rn_message_object_delivery_capture( &server->object, client_index, capture );
}
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------
//                     create and destroy
// ---------------------------------------------------------------

#if ROCKETNET_DEV
// A unit test holds the worker not-stopped here and counts polls. NULL
// leaves the transport's own stopped() in charge. The test fails at its
// ceiling, so a wait with no deadline cannot hang the suite, and this
// path does not sleep. Per process, null by default.
static rocketnet_dev_link_stop_wait_hold_function_t * rn_link_stop_wait_hold;
static void * rn_link_stop_wait_hold_context;

void rocketnet_dev_set_link_stop_wait_hold_hook( rocketnet_dev_link_stop_wait_hold_function_t * function, void * context )
{
    rn_link_stop_wait_hold = function;
    rn_link_stop_wait_hold_context = context;
}
#endif // #if ROCKETNET_DEV

// rocketnet_client_destroy polls a worker for RN_DESTROY_STOP_SECONDS
// (sdk/rocketnet_api.cpp, 1.0). That constant is private to the api file,
// so the same budget is repeated here.
static const double RN_MESSAGE_LINK_STOP_WAIT_SECONDS = 1.0;

// The wait's clock, sleep and stopped query. A build reads the platform clock
// and sleeps; with the DEV hold set, the clock is a counter the sleep
// advances, and the hold answers for the worker, so a unit test runs this
// same loop with no real time.
struct rn_link_stop_wait_t
{
    const struct rn_message_link_t * link;
    double now;                                     // the counter clock, held only
    bool held;
};

static double rn_link_stop_wait_time( struct rn_link_stop_wait_t * wait )
{
    return wait->held ? wait->now : rocketnet_platform_time();
}

static void rn_link_stop_wait_sleep( struct rn_link_stop_wait_t * wait, double seconds )
{
    if ( wait->held )
    {
        wait->now += seconds;
    }
    else
    {
        rocketnet_platform_sleep( seconds );
    }
}

static bool rn_link_stop_wait_stopped( const struct rn_link_stop_wait_t * wait )
{
#if ROCKETNET_DEV
    if ( wait->held )
    {
        return !rn_link_stop_wait_hold( rn_link_stop_wait_hold_context );
    }
#endif // #if ROCKETNET_DEV
    return !wait->link->stopped || wait->link->stopped( wait->link->transport );
}

// stops an owned transport and steps it until it reports stopped, or the
// budget above runs out. The wait used to sleep 1 ms with no deadline. On the
// deadline it logs once and returns false: the worker still owns the
// transport, so the caller keeps it rather than destroy it.
static bool rn_message_link_stop_and_wait( const struct rn_message_link_t * link )
{
    if ( link->stop )
    {
        link->stop( link->transport );
    }

    struct rn_link_stop_wait_t wait = { link, 0.0, false };
#if ROCKETNET_DEV
    wait.held = rn_link_stop_wait_hold != NULL;
#endif // #if ROCKETNET_DEV

    const double deadline = rn_link_stop_wait_time( &wait ) + RN_MESSAGE_LINK_STOP_WAIT_SECONDS;
    while ( !rn_link_stop_wait_stopped( &wait ) )
    {
        if ( link->update )
        {
            link->update( link->transport, 0.0 ); // stopping updates do not read game time
        }
        if ( !rn_link_stop_wait_stopped( &wait ) )
        {
            if ( rn_link_stop_wait_time( &wait ) >= deadline )
            {
                rocketnet_error( "message transport stop: the worker has not exited after %.1f seconds; the transport is kept, not freed",
                                 RN_MESSAGE_LINK_STOP_WAIT_SECONDS );
                return false;
            }
            rn_link_stop_wait_sleep( &wait, 0.001 );
        }
    }

    return true;
}

static void rn_message_link_abort_create( const struct rn_message_link_t * link )
{
    if ( !link || !link->destroy )
    {
        return;
    }

    if ( rn_message_link_stop_and_wait( link ) )
    {
        link->destroy( link->transport );
    }
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

    // R6 (standard/MESSAGES.md m9.4): a create argument of 0 takes the message
    // config's field; a positive value overrides it for this create; 0 or
    // lower in the config field itself disables that direction's budget.
    // The environment is not read here: the public creates resolve the
    // budget from ROCKETNET_CLIENT_SEND_KBPS or
    // ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS (rn_client_split and
    // rn_server_split) and pass it as default_kbps.

    // resolve the explicit create override before sizing, as required_memory
    // does. This copy is owned by the object and does not alter the caller's
    // configuration.

    rocketnet_message_config_t sized;
    default_kbps = rn_message_object_sized_config( config, is_server, default_kbps, reserve_unlimited, &sized );
    config = &sized;

    rn_message_object_layout_t layout = {};
    const bool laid_out = link && rn_message_object_layout( config, num_connections, &layout );
    const bool budget_valid = ( default_kbps <= 0 || rocketnet_send_budget_kbps_valid( (uint32_t) default_kbps ) ) && rocketnet_send_budget_time_valid( time );
    const bool memory_valid = !memory || ( laid_out && memory_bytes >= layout.total && ( (uintptr_t) memory % ROCKETNET_ALLOCATION_ALIGNMENT ) == 0 );

    if ( !laid_out || !budget_valid || !memory_valid )
    {
        if ( !laid_out )
        {
            rocketnet_error( "message object create refused: a refused message config or connection count" );
        }
        else if ( !budget_valid )
        {
            rocketnet_error( "message object create refused: send budget %d kbps or time is outside its range", (int) default_kbps );
        }
        else
        {
            rocketnet_error( "message object create refused: the memory block is short or not %d-aligned", ROCKETNET_ALLOCATION_ALIGNMENT );
        }

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

    // everything before the arena is state but the receive batch's two
    // arrays (512 KB), which are written before they are read, below
    // batch_count: they are never zeroed, so their pages are touched only by
    // a batch that uses them

    const size_t batch_begin = offsetof( rn_message_object_t, batch_connection );
    const size_t batch_end = offsetof( rn_message_object_t, send_next_connection );

    static_assert( offsetof( rn_message_object_t, batch_entry ) > offsetof( rn_message_object_t, batch_connection ) &&
                       offsetof( rn_message_object_t, batch_entry ) < offsetof( rn_message_object_t, send_next_connection ),
                   "the batch arrays are one span of the object" );

    memset( block, 0, batch_begin );
    memset( block + batch_end, 0, (size_t) layout.arena - batch_end );

    rn_message_object_t * object = (rn_message_object_t *) block;
    object->context = context;
    object->link = *link;
    object->is_server = is_server;
    object->num_connections = num_connections;
    object->config = *config;
    object->default_kbps = default_kbps;
    object->time = time;
    object->raw_time = time;                    // the seed is the clock's origin, not a step
    object->time_offset = 0.0;
    memset( object->open_lines, 0, sizeof( object->open_lines ) );
    object->fill_phase = NULL;
    object->send_next_connection = 0;
    object->block = block;
    object->owns_block = owns_block ? 1 : 0;
    object->hot_connections = (rn_message_connection_hot_t *) ( block + layout.hot_connections );
    object->connections = (rn_message_connection_t *) ( block + layout.connections );
    object->send_writers = num_connections > 1 ? (int32_t *) ( block + layout.send_writers ) : NULL;
    rn_slots_init( &object->slots, num_connections, (uint64_t *) ( block + layout.slot_connection_ids ), block + layout.slot_changed );

    object->arena = rn_arena_create_in_block( block + layout.arena, layout.arena_bytes, config, num_connections, &object->counters );
    if ( !object->arena )
    {
        rocketnet_error( "message object create failed: the arena was refused" );
        rn_message_link_abort_create( link );
        if ( owns_block )
        {
            rocketnet_free( context, block );
        }
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
        connection->disconnect_reason = ROCKETNET_DISCONNECT_REASON_NONE;

        if ( object->is_server )
        {
            rocketnet_server_connection_reset( object, (int) ( connection - object->connections ) );
        }
        else
        {
            rocketnet_client_connection_reset( object );
        }
    }

    // The client's one connection is bound at create, because
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

// proofs/tla/RNMessageOpen.tla Open and Close: a connection's one open start,
// and the object's count of them (fill threads open and close on different
// connections at once, so the count and the last index are atomics)
static rn_message_open_line_t * rn_message_open_line( rn_message_object_t * object )
{
    return &object->open_lines[rocketnet_fill_phase_line_of( rocketnet_platform_thread_id() )];
}

static void rn_message_open_set( rn_message_object_t * object, int index )
{
    rn_message_open_t * open = &object->connections[index].open;
    rocketnet_assert( !open->pending );
    open->pending = 1;
    rn_message_open_line_t * line = rn_message_open_line( object );
    rocketnet_platform_atomic_add_u64( (uint64_t *) &line->opens, 1 );
    rocketnet_platform_atomic_store_u64( &line->last_open, (uint64_t) index );
}

static void rn_message_open_close( rn_message_object_t * object, rn_message_open_t * open )
{
    if ( open->pending )
    {
        open->pending = 0;
        rocketnet_platform_atomic_add_u64( (uint64_t *) &rn_message_open_line( object )->opens, ~0ULL );
    }
}

int64_t rn_message_open_count( const rn_message_object_t * object )
{
    int64_t opens = 0;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        opens += (int64_t) rocketnet_platform_atomic_load_u64( (uint64_t *) const_cast<int64_t *>( &object->open_lines[i].opens ) );
    }
    return opens;
}

int rn_message_open_last( const rn_message_object_t * object )
{
    const rn_message_open_line_t * line = &object->open_lines[rocketnet_fill_phase_line_of( rocketnet_platform_thread_id() )];
    return (int) rocketnet_platform_atomic_load_u64( const_cast<uint64_t *>( &line->last_open ) );
}

static void rn_message_flush_unreliable( rn_message_object_t * object, int index, bool discard )
{
    // seal the connection's completed records once. No message payload is copied.
    // RNOutgoingBuild Seal, from SealBuild (a start that does not fit or falls
    // back), BeginFlush (send_packets) and BeginUpdate (update); the over-budget
    // drop is its drop branch, and the allowance is charged either way.

    rn_message_connection_t * connection = &object->connections[index];
    rn_message_packet_build_t * build = &connection->unreliable_build;
    if ( !build->writer.payload )
    {
        return;
    }
    const int records = build->writer.records;
    int admitted = 0;
    int failed_counter = ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED;
    const int bytes = rocketnet_message_writer_finish( &build->writer );

    if ( !discard && records > 0 && connection->send_budget_enabled )
    {
        admitted = rocketnet_send_budget_max_wire_bytes( bytes );
        if ( !rn_endpoint_in_place_affordable( connection, bytes, object->time ) ||
             rocketnet_send_budget_admit( &connection->budget, ROCKETNET_SEND_CLASS_UNRELIABLE, admitted ) != ROCKETNET_SEND_BUDGET_SEND )
        {
            discard = true;
            failed_counter = ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET;
            admitted = 0;
        }
    }

    int wire_bytes = 0;
    if ( discard || records == 0 )
    {
        object->link.abort_packet( object->link.transport, &build->context );
    }
    else
    {
        wire_bytes = object->link.finish_packet( object->link.transport, &build->context, bytes );
    }
    if ( wire_bytes > 0 )
    {
        if ( build->receipt_count )
        {
            rn_window_ack_commit_receipt_packet( &connection->ordered.ack, build->context.packet_id,
                                                 build->receipt_serials, build->receipt_count, object->time );
        }

        if ( admitted )
        {
            rocketnet_send_budget_settle( &connection->budget, admitted, wire_bytes );
        }
    }
    else if ( records > 0 )
    {
        if ( admitted )
        {
            rocketnet_send_budget_settle( &connection->budget, admitted, 0 );    // proofs/tla/RNSendBudget.tla SendFailed: refund the whole charge
        }

        for ( int i = 0; i < records; ++i )
        {
            rn_message_count( object, failed_counter );
        }

        for ( uint32_t i = 0; i < build->receipt_count; ++i )
        {
            rn_window_ack_receipt_emit_loss( &connection->ordered.ack, build->receipt_serials[i] );
        }
    }

    if ( records > 0 )
    {
        connection->written++;
    }

    memset( build, 0, sizeof( *build ) );
}

void rn_message_object_destroy( struct rn_message_object_t * object )
{
    if ( !object )
    {
        return;
    }

    // destroy before the owned transport has stopped is a misuse: it is
    // logged and counted, and destroy finishes the shutdown itself, so the
    // transport, its threads and the block are released all the same

    bool transport_stopped = true;
    if ( object->link.destroy && object->link.stopped && !object->link.stopped( object->link.transport ) )
    {
        rocketnet_error( "message %s destroy: the transport is not stopped; stop it and poll stopped before destroy. Stopping it now",
                         object->is_server ? "server" : "client" );
        rocketnet_api_misuse_record();
        transport_stopped = rn_message_link_stop_and_wait( &object->link );
    }

    for ( int i = 0; i < object->num_connections; ++i )
    {
        if ( object->connections[i].open.in_place > 0 )
        {
            rn_message_connection_cancel_pending( object, i );     // the open frame goes back first
        }
    }

    for ( int i = 0; i < object->num_connections; ++i )
    {
        rn_message_flush_unreliable( object, i, true );
    }

    if ( object->link.destroy && transport_stopped )
    {
        object->link.destroy( object->link.transport );
    }

    rn_arena_destroy( object->arena );

    if ( object->owns_block )
    {
        rocketnet_free( object->context, object->block );
    }
}

// ---------------------------------------------------------------
//                     kick, the one way a connection ends here
// ---------------------------------------------------------------

static void rn_message_connection_cancel_pending( rn_message_object_t * object, int index )
{
    rn_message_connection_t * connection = &object->connections[index];
    rn_message_open_t * open = &connection->open;

    if ( !open->pending )
    {
        return;
    }

    if ( open->in_place > 0 )
    {
        // RNOutgoingBuild Abort: the reservation is returned; a frame with no
        // committed record goes back to the transport

        if ( open->in_place == 1 )
        {
            object->link.abort_packet( object->link.transport, &open->in_place_context );
        }
        else if ( connection->unreliable_build.writer.records == 0 )
        {
            rn_message_flush_unreliable( object, index, true );
        }

        open->in_place = 0;
        open->in_place_payload = NULL;
    }

    rn_endpoint_abort( connection );
    rn_message_open_close( object, open );
}

static void rn_message_object_close_in_place( rn_message_object_t * object )
{
    // a start and its finish are fill calls of one fill phase (sdk/rocketnet.h,
    // the threading contract), so a start still pending at an update or a
    // flush is aborted, in place or not: the caller's error, counted, and its
    // later finish asserts in a debug build and returns 0 in a release build.
    // A finish or abort the fill-phase check refused leaves its start here.

    if ( rn_message_open_count( object ) == 0 )
    {
        return;
    }

    for ( int index = 0; index < object->num_connections; ++index )
    {
        rn_message_open_t * open = &object->connections[index].open;
        if ( !open->pending )
        {
            continue;
        }

        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        rocketnet_api_misuse_record();

        rn_message_connection_cancel_pending( object, index );
        open->in_place = -1;

        rocketnet_error( "message start pending on connection %d at update or send_packets: the send is aborted", index );
    }
}

static void rn_message_connection_cancel_at_reset( rn_message_object_t * object, int index )
{
    // RNSlots ResetCancel: a queued-path start still pending on a connection its
    // update resets is cancelled before that reset, as kick cancels it, so the
    // recarved region holds none of it and later starts on every connection are
    // not refused by it. Counted in MESSAGE_SEND_REFUSED; its finish then asserts
    // in a debug build and returns 0 in a release build (rn_message_object_finish:
    // pending_connection is -1).

    rn_message_flush_unreliable( object, index, true );

    rn_message_open_t * open = &object->connections[index].open;
    if ( open->in_place < 0 )
    {
        open->in_place = 0;
    }

    if ( !open->pending )
    {
        return;
    }

    rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    rocketnet_info( "message start pending on connection %d at its reset: the start is cancelled", index );
    rn_message_connection_cancel_pending( object, index );
}

static void rn_message_connection_release( rn_message_object_t * object, int index )
{
    // RNSlots ResetFree, the lazy reset (proofs/tla/RNSlots.tla ResetFree, Observe;
    // proofs/tla/RNConnectionMemory.tla Reuse, whose one step this splits by a stutter):
    // a server slot whose connection ended is freed here in O(1) -- only its
    // binding cleared, its open unreliable build already discarded by
    // rn_message_connection_cancel_at_reset -- and its region is carved only when
    // Observe binds the slot to its next connection (RN_SLOTS_RESET calls
    // rocketnet_server_connection_reset). Its views, receive cursor and held
    // count need no clearing here: both call sites run in an update whose
    // BeginUpdate pass already returned the last update's views to WAITING,
    // reset the cursor and held count, and expired the deliveries
    // (rn_endpoint_begin_update), so a clear here would cover an empty range.
    // A free slot is never offered, written
    // or admitted to (every path checks CONNECTED, rn_slots_admit discards for a
    // free slot), so nothing of the ended connection is reachable, and a reused
    // slot still starts exactly as a fresh one (ReusedSlotStartsClean). The
    // region holds message bytes only: the session keys are wiped by the
    // transport when the slot ends (rocketnet_server_connection_reset in
    // sdk/rocketnet_connection_reset.cpp, the encryption map's remove), never here.
    // The region is carved once, at the next connect, not here as well.

    rocketnet_assert( object->is_server );
    rn_message_connection_t * connection = &object->connections[index];
    connection->hot->connection_id = 0;
    connection->hot->state = RN_MESSAGE_CONNECTION_FREE;
}

void rn_message_object_kick( struct rn_message_object_t * object, int index, int reason )
{
    rocketnet_fill_phase_main_scope_t phase( object ? object->fill_phase : NULL );
    if ( !phase.entered )
    {
        return;
    }

    // proofs/tla/RNSlots.tla Kick: ends the connection, cancels its pending start, and records the reason.
    // Nothing more is sent or delivered from its region. Views the application
    // already holds stay valid until the next update.

    if ( !object || index < 0 || index >= object->num_connections )
    {
        return;
    }

    rn_message_connection_t * connection = &object->connections[index];
    if ( connection->hot->state != RN_MESSAGE_CONNECTION_CONNECTED )
    {
        return;
    }

    if ( connection->open.in_place < 0 )
    {
        connection->open.in_place = 0;
    }
    rn_message_connection_cancel_pending( object, index );
    rn_message_flush_unreliable( object, index, true );

    connection->hot->state = RN_MESSAGE_CONNECTION_KICKED;
    connection->disconnect_reason = reason;

    if ( object->link.disconnect )
    {
        object->link.disconnect( object->link.transport, index );
    }

    rocketnet_info( "message connection %d ended: reason %d", index, reason );
}

// ---------------------------------------------------------------
//                          update
// ---------------------------------------------------------------

// the connection id entry k is tagged with, as rn_message_admit compares it;
// 0 for a slot out of range, which admit discards before reading it

static uint64_t rn_message_entry_connection( rn_message_object_t * object, const int * client_index, uint64_t ( *entry_connection )( void * transport, int k ), int k )
{
    const int index = client_index ? client_index[k] : 0;
    if ( index < 0 || index >= object->num_connections )
    {
        return 0;
    }

    return entry_connection ? entry_connection( object->link.transport, k )
                            : ( object->slots.changed[index] ? 0 : object->connections[index].hot->connection_id );
}

// rn_message_admit for an in-range slot whose entry connection id is already read

static rn_message_connection_t * rn_message_admit_tagged( rn_message_object_t * object, int index, uint64_t entry_connection_id )
{
    rocketnet_assert( index >= 0 && index < object->num_connections );

    if ( rn_slots_admit( &object->slots, index, entry_connection_id ) != RN_SLOTS_APPLY )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        return NULL;
    }

    return &object->connections[index];
}

static rn_message_connection_t * rn_message_admit( rn_message_object_t * object, const int * client_index, uint64_t ( *entry_connection )( void * transport, int k ), int k )
{
    // RNSlots Admit: an entry is applied only to the connection it was received
    // for. The link tags each entry with the transport's id of that connection
    // (payload_connection, acked_connection: P9b), and rn_slots_admit applies it
    // only when that id is the slot's binding from this update's Observe, so an
    // entry of a connection that ended inside the update is discarded and a slot
    // bound in this update keeps its own first entries. A link with no tag
    // function presents the slot's binding, or no tag (0, which no connection
    // carries) when Observe bound or dropped the slot in this update, which
    // discards it (proofs/tla/RNSlots.tla).

    const int index = client_index ? client_index[k] : 0;
    if ( index < 0 || index >= object->num_connections )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        return NULL;
    }

    return rn_message_admit_tagged( object, index, rn_message_entry_connection( object, client_index, entry_connection, k ) );
}

static void rn_message_object_release_batch( struct rn_message_object_t * object );

// proofs/tla/RNMessageViews.tla Update: the batch released, the last update's views cleared by its delivery count, cursor and held reset
int rn_message_object_update( struct rn_message_object_t * object, double time )
{
    rn_message_object_update_begin( object, time );
    return rn_message_object_update_end( object );
}

// the link splits update only when update is the server transport's own: a
// test that wraps update keeps its wrapper whole
static bool rn_message_link_splits_update( const struct rn_message_object_t * object );

void rn_message_object_update_begin( struct rn_message_object_t * object, double time )
{
    // measure-only merge: the wide fill's main-thread phase scope covers the
    // begin of the split update (the end is the main thread's too)
    rocketnet_fill_phase_main_scope_t phase( object ? object->fill_phase : NULL );
    if ( !phase.entered )
    {
        return;
    }
    if ( object->update_stage != RN_MESSAGE_UPDATE_CLOSED )
    {
        rocketnet_api_misuse_record();
        rocketnet_assert( object->update_stage == RN_MESSAGE_UPDATE_CLOSED && "end the update before the next begins" );
        (void) rn_message_object_update_end( object );
    }

    int result = ROCKETNET_UPDATE_OK;
    object->update_stage = RN_MESSAGE_UPDATE_DONE;

    // proofs/tla/RNSlots.tla BeginUpdate: enters the transport phase once every view from the last update is released.
    // proofs/tla/RNSlots.tla RejectUnreleasedUpdate: a view still held sets the not-released result this update returns.
    // A held view is the application's error: it asserts in a debug build and returns the named
    // error. The views die with the transport's batch either way,
    // so they are taken back here and memory never grows from a held view.

    rn_message_object_release_batch( object );            // a batch is released by the update that follows it

    uint32_t held = 0;
    for ( int i = 0; i < object->num_connections; i++ )
    {
        held += object->hot_connections[i].held;
    }
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
    // application's error, handled like a send with no room: counted, API
    // misuse (once per connection), an assert in a debug build, and that
    // connection ends with MESSAGE_NOT_RECEIVED. Nothing is dropped silently.

    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
        {
            continue;                                   // a kicked connection's deliveries were never offered
        }

        rn_message_connection_t * connection = &object->connections[i];
        const uint32_t deliveries = connection->ordered.channels.deliveries;
        uint32_t waiting = 0;
        for ( uint32_t k = 0; k < deliveries; k++ )
        {
            waiting += connection->hot->views[k] == RN_MESSAGE_VIEW_WAITING ? 1u : 0u;
        }
        if ( waiting == 0 )
        {
            continue;
        }

        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_NOT_RECEIVED, waiting );
        rocketnet_api_misuse_record();
        // proofs/tla/ServerSession.tla MessageEnd: the update kick ends a connected slot of a running server.
        rn_message_object_kick( object, i, ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED );
        rocketnet_error( "message update with %u delivered message(s) not received on connection %d: loop receive_messages until NULL before update", waiting, i );
        rocketnet_assert( waiting == 0 && "every delivered message is received before the next update" );
    }

    rn_message_object_close_in_place( object );       // RNOutgoingBuild BeginUpdate: the open builds sealed
    for ( int i = 0; i < object->num_connections; ++i )
    {
        rn_message_flush_unreliable( object, i, false );
    }

    bool time_clamped = false;
    time = rocketnet_update_clock_advance( object->time, &object->raw_time, &object->time_offset, time, &time_clamped );
    if ( time_clamped && !object->clock_started )
    {
        object->update_result = result;
        return;
    }

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
        connection->hot->receive_cursor = 0;
        connection->hot->held = 0;

        // proofs/tla/RNSlots.tla ResetFree: a kicked connection's region is cleared before the transport update can give the slot to another client

        if ( object->hot_connections[i].state == RN_MESSAGE_CONNECTION_KICKED )
        {
            rn_message_connection_cancel_at_reset( object, i );     // RNSlots ResetCancel (kick cancelled it already)

            if ( object->is_server )
            {
                rn_message_connection_release( object, i );         // RNSlots ResetFree: lazy, carved at reuse
            }
            else
            {
                rocketnet_client_connection_reset( object );
            }

            rn_slots_observe( &object->slots, i, 0 );
        }
    }

    object->receive_connection = 0;                     // the receive queue starts again at the first connection

    object->update_result = result;
    if ( rn_message_link_splits_update( object ) )
    {
        object->link.update_receive( object->link.transport, time - object->clock_origin );
        object->update_stage = RN_MESSAGE_UPDATE_SPLIT;
        return;
    }

    object->link.update( object->link.transport, time - object->clock_origin );
    object->update_stage = RN_MESSAGE_UPDATE_WHOLE;
}

int rn_message_object_update_end( struct rn_message_object_t * object )
{
    const int stage = object->update_stage;
    object->update_stage = RN_MESSAGE_UPDATE_CLOSED;
    const int result = object->update_result;
    object->update_result = ROCKETNET_UPDATE_OK;

    if ( stage == RN_MESSAGE_UPDATE_CLOSED )
    {
        // an end with no begin: nothing to finish
        rocketnet_api_misuse_record();
        rocketnet_assert( stage != RN_MESSAGE_UPDATE_CLOSED && "begin the update before ending it" );
        return ROCKETNET_UPDATE_OK;
    }

    if ( stage == RN_MESSAGE_UPDATE_DONE )
    {
        return result;
    }

    if ( stage == RN_MESSAGE_UPDATE_SPLIT )
    {
        object->link.update_finish( object->link.transport );
    }

    const double time = object->time;

    // the client message connection is bound before the handshake so its
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
        {
            object->connections[0].disconnect_reason = object->link.end_reason( object->link.transport, 0 );
        }

        rn_message_connection_cancel_at_reset( object, 0 );         // RNSlots ResetCancel
        rocketnet_client_connection_reset( object );
        rn_slots_observe( &object->slots, 0, 0 );
    }

    // proofs/tla/RNSlots.tla Observe: reads each slot's connection after the transport update and resets a slot whose binding changed

    if ( object->is_server )
    {
        for ( int i = 0; i < object->num_connections; i++ )
        {
            rn_message_connection_t * connection = &object->connections[i];
            const uint64_t id = object->link.connection( object->link.transport, i );
            const int action = rn_slots_observe( &object->slots, i, id );
            if ( action == RN_SLOTS_NONE )
            {
                continue;
            }

            rn_message_connection_cancel_at_reset( object, i );     // RNSlots ResetCancel

            // RNSlots Observe: a slot going free is released in O(1); the
            // region is carved when the slot is bound again (the lazy reset).
            // The transport ended its connection: the reason is the one the
            // transport's ending path named (TIMED_OUT, CLIENT_DISCONNECTED,
            // SERVER_DISCONNECTED), kept until the slot's next connection.
            // A kick never reaches here: its slot was observed free at reset.

            if ( action == RN_SLOTS_DROP )
            {
                if ( object->link.end_reason )
                {
                    connection->disconnect_reason = object->link.end_reason( object->link.transport, i );
                }
                rn_message_connection_release( object, i );
                continue;
            }

            rocketnet_server_connection_reset( object, i );

            connection->hot->state = RN_MESSAGE_CONNECTION_CONNECTED;
            connection->hot->connection_id = id;
            connection->disconnect_reason = ROCKETNET_DISCONNECT_REASON_NONE;
            rn_endpoint_announce_config( connection, time );       // RNEndpoint Reset: the server's connect (10.3)
        }
    }

    // proofs/tla/RNSlots.tla TransportAck: feeds the transport's acked packet ids to the slot each one was opened on
    {
        const int * client_index = NULL;
        const uint64_t * packet_ids = NULL;
        const int count = object->link.acked( object->link.transport, &client_index, &packet_ids );

        // the transport appends each received block's ids as one run of one
        // slot and one connection id, so the entries are taken a run at a
        // time: a run is the longest stretch of entries with the same slot
        // and the same entry connection id, so every entry of it gets the
        // admit verdict of its first (rn_slots_admit reads only the slot's
        // binding, which nothing in this loop changes). An admitted run's ids
        // go to the window in one call, in entry order, which is the same as
        // one call per id (rn_window_ack_on_acks); a discarded run counts
        // one stale entry per id, as one admit per id did.

        // the batch's connection ids read once when the link offers them
        // (acked_connection_ids): ids[k] is what acked_connection( k )
        // answers, and 0 past the count

        const uint64_t * acked_ids = NULL;
        int acked_ids_count = 0;
        if ( object->link.acked_connection_ids )
        {
            acked_ids_count = object->link.acked_connection_ids( object->link.transport, &acked_ids );
            if ( !acked_ids )
            {
                acked_ids_count = 0;
            }
        }

        auto entry_id = [&]( int e ) -> uint64_t
        {
            if ( !object->link.acked_connection_ids )
            {
                return rn_message_entry_connection( object, client_index, object->link.acked_connection, e );
            }
            const int slot = client_index ? client_index[e] : 0;
            if ( slot < 0 || slot >= object->num_connections )
            {
                return 0;
            }
            return e < acked_ids_count ? acked_ids[e] : 0;
        };

        int k = 0;
        while ( k < count )
        {
            const int index = client_index ? client_index[k] : 0;
            const uint64_t entry_connection_id = entry_id( k );

            int end = k + 1;
            while ( end < count && ( client_index ? client_index[end] : 0 ) == index && entry_id( end ) == entry_connection_id )
            {
                end++;
            }

            rn_message_connection_t * connection = NULL;
            if ( index < 0 || index >= object->num_connections )
            {
                rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
            }
            else
            {
                connection = rn_message_admit_tagged( object, index, entry_connection_id );
            }

            if ( connection )
            {
                rn_endpoint_on_acked_ids( connection, &packet_ids[k], (uint32_t) ( end - k ), time );   // RNEndpoint OnAcked
            }
            else
            {
                for ( int stale = k + 1; stale < end; stale++ )
                {
                    rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
                }
            }

            k = end;
        }
    }

    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
        {
            continue;
        }

        rn_message_connection_t * connection = &object->connections[i];

        // proofs/tla/RNResendTiming.tla Seed (standard/MESSAGES.md m9.2): until
        // the connection's own first RTT sample, the transport's measured round
        // trip is taken as that sample, after this update's acks, so the
        // resend delay follows the path from the first round trip the
        // transport measures rather than waiting message_resend_delay_initial_ms;
        // rn_window_ack_seed_rtt does nothing once the connection has a sample,
        // so the transport's figure (a cold line per connection) is read only
        // before it: link.rtt is a read with no effects

        if ( object->link.rtt && !connection->ordered.ack.has_rtt )
        {
            rn_window_ack_seed_rtt( &connection->ordered.ack, object->link.rtt( object->link.transport, i ) );
        }

        rn_endpoint_tick( connection, time );          // RNEndpoint Tick: RNReceipts SweepLoss, the reliable timeout
    }

    // proofs/tla/RNSlots.tla TransportOpen: feeds each opened payload to the slot it arrived on (RNChannel OpenPacket, DeliverMessage)
    {
        const int * client_index = NULL;
        uint8_t * const * payload_data = NULL;
        const int * payload_bytes = NULL;
        const int count = object->link.payloads( object->link.transport, &client_index, &payload_data, &payload_bytes );
        for ( int k = 0; k < count; k++ )
        {
            rn_message_connection_t * connection = rn_message_admit( object, client_index, object->link.payload_connection, k );
            if ( connection && rn_endpoint_on_payload( connection, payload_data[k], payload_bytes[k], NULL, 0, NULL, time ) &&
                 object->link.accept_payload )
            {
                object->link.accept_payload( object->link.transport, k );
            }
        }
    }

    // RNSlots Kick, RNConnectionMemory Kick, proofs/tla/ServerSession.tla
    // MessageEnd: exhaustion first, then a wire violation, then a reliable
    // message past its timeout (rn_endpoint_error)

    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
        {
            continue;
        }

        rn_message_connection_t * connection = &object->connections[i];

        // RNEndpoint Tick, the CONFIG half: the wait for the remote endpoint's CONFIG
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

    // proofs/tla/RNSlots.tla EndUpdate: the update returns, and the application then sees this update's views

    return result;
}

// ---------------------------------------------------------------
//                          send
// ---------------------------------------------------------------

static int rn_message_send_channel( int channel, int reliability )
{
    // the channel is the ordering channel: the types that are neither
    // ordered nor sequenced ignore it (they have no ordering state) and travel with none (codes 0 and 2), so any
    // value is accepted for them, never refused, and taken as 0

    if ( reliability == ROCKETNET_RELIABILITY_UNRELIABLE || reliability == ROCKETNET_RELIABILITY_RELIABLE ||
         reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT || reliability == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT )
    {
        return 0;
    }
    return channel;
}

static bool rn_message_send_connected( const struct rn_message_object_t * object, const rn_message_connection_t * connection )
{
    // standard/MESSAGES.md m1.5 (proofs/tla/ClientSession.tla DropSend,
    // proofs/tla/ServerSession.tla ServerDropSend): an application message is sent
    // only on a CONNECTED connection. A link with no connection query (a bare
    // object that only starts and finishes, never updating or sending) has no
    // transport state to wait on.

    return connection->hot->state == RN_MESSAGE_CONNECTION_CONNECTED &&
           ( object->is_server || !object->link.connection || object->link.connection( object->link.transport, 0 ) != 0 );
}

static int rn_message_send_transport_max( const struct rn_message_object_t * object, int reliability )
{
    // the transport's payload cap, which the endpoint's large unreliable
    // check reads (a seam: a transport value passed in as an argument)

    const bool unreliable = reliability == ROCKETNET_RELIABILITY_UNRELIABLE || reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
                            reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
    return unreliable && object->link.max_payload_bytes ? object->link.max_payload_bytes( object->link.transport ) : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

uint8_t * rn_message_object_start( struct rn_message_object_t * object, int index, int channel, int reliability, int bytes )
{
    // proofs/tla/RNSlots.tla Request: reserves room for one message and returns the pointer the application writes.
    // proofs/tla/RNSlots.tla StartPending: one start stays pending on this slot until finish or abort.

    if ( !object )
    {
        return NULL;
    }

    if ( index < 0 || index >= object->num_connections )
    {
        return NULL;
    }

    // a fill call: one filler per connection (sdk/rocketnet_fill_phase.h)

    rocketnet_fill_phase_fill_scope_t phase( object->fill_phase, index );
    if ( !phase.entered )
    {
        return NULL;
    }

    channel = rn_message_send_channel( channel, reliability );

    rn_message_connection_t * connection = &object->connections[index];
    rn_message_open_t * open = &connection->open;

    if ( open->in_place < 0 )
    {
        open->in_place = 0;
    }

    // a client whose transport is not CONNECTED, or a slot with no
    // connection, drops the start: NULL, counted once, nothing queued for
    // later.

    if ( !rn_message_send_connected( object, connection ) )
    {
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );
        return NULL;
    }

    // one start pending per connection (proofs/tla/RNMessageOpen.tla): a
    // second on the same connection is refused until the first is finished or
    // aborted, or a reset of its connection cancels it
    // (rn_message_connection_cancel_at_reset, RNSlots ResetCancel). Starts on
    // other connections are independent, so fill threads each hold one.

    if ( open->pending )
    {
        rocketnet_error( "message start refused: a start is already pending on connection %d; finish or abort it first", index );
        rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
        return NULL;
    }

    const bool unreliable = reliability == ROCKETNET_RELIABILITY_UNRELIABLE || reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
                            reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
    const int transport_max = rn_message_send_transport_max( object, reliability );
    bool frame_refused = false;

    // RNOutgoingBuild StartInPlace: a fitting unreliable message reserves its
    // declared maximum at the open build's committed end, or opens a frame;
    // SealBuild first when it does not fit the rest of the frame

    if ( unreliable && object->link.start_packet )
    {
        const int eligible = rn_endpoint_in_place_header( connection, channel, reliability, bytes, transport_max, object->time );
        int header = ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
        if ( reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED )
        {
            // the code 1 header is the record shape's, the same call rn_channels_finish makes

            struct rocketnet_message_record_t sequenced_shape = {};
            sequenced_shape.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
            sequenced_shape.channel = (uint32_t) channel;
            header = rocketnet_message_record_header_bytes( &sequenced_shape );
        }

        rn_message_packet_build_t * build = &connection->unreliable_build;
        const int room = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1;
        if ( eligible >= 0 && bytes <= room - header )
        {
            const bool receipt = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
            if ( build->writer.payload &&
                 ( bytes + header > rocketnet_message_writer_space( &build->writer ) ||
                   ( receipt && build->receipt_count == RN_MESSAGE_RECEIPT_SERIALS_MAX ) ||
                   !rn_endpoint_in_place_affordable( connection, build->writer.bytes + header + bytes + 1, object->time ) ) )
            {
                rn_message_flush_unreliable( object, index, false );
            }

            if ( !build->writer.payload )
            {
                // keep a fair rotating send round available to the other clients

                const int reserve = object->link.send_connected_count ? object->link.send_connected_count( object->link.transport ) : object->num_connections;
                const bool frame_available = !object->link.send_capacity_available || object->link.send_capacity_available( object->link.transport ) > reserve;
                if ( frame_available && rn_endpoint_in_place_header( connection, channel, reliability, bytes, transport_max, object->time ) >= 0 &&
                     rn_endpoint_in_place_affordable( connection, header + bytes + 1, object->time ) )
                {
                    uint8_t * frame = object->link.start_packet( object->link.transport, index, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &build->context );
                    frame_refused = frame == NULL && object->link.max_payload_bytes && transport_max >= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
                    if ( frame && build->context.payload_capacity >= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
                    {
                        rocketnet_message_writer_begin( &build->writer, frame + build->context.payload_index, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
                    }
                    else if ( frame )
                    {
                        object->link.abort_packet( object->link.transport, &build->context );
                    }
                }
            }

            if ( build->writer.payload )
            {
                rn_endpoint_in_place_begin( connection, channel, reliability, bytes, header );
                open->in_place = 2;
                open->in_place_payload = build->writer.payload + build->writer.bytes;
                rn_message_open_set( object, index );
                return open->in_place_payload + header;
            }
        }

        // a queued or whole-packet send cannot overtake completed records
        // (RNOutgoingBuild SealBuild before StartQueued). A start that will be
        // refused (no whole frame for it and no room or a refused route in
        // the region, the room query's own decision) seals nothing: a
        // refusal leaves the connection as it was (standard/MESSAGES.md m1.7)

        if ( rn_endpoint_in_place_header( connection, channel, reliability, bytes, transport_max, object->time ) >= 0 ||
             rn_endpoint_send_room( connection, channel, reliability, bytes, transport_max ) )
        {
            rn_message_flush_unreliable( object, index, false );
        }
    }

    // RNOutgoingBuild StartInPlace, the whole-frame case: a fitting message
    // that cannot be packed takes a transport frame of its own and is written
    // in it, its header's room in front and the form byte's after;
    // header + bytes + 1, the declared maximum, decides the frame

    if ( unreliable && object->link.start_packet )
    {
        int header = rn_endpoint_in_place_header( connection, channel, reliability, bytes, transport_max, object->time );

        // reserve a complete round for the rotating pass, including this
        // connection's reliable work. In-place writes may use surplus slots,
        // but application call order cannot consume another client's turn.

        const int send_connections = object->link.send_connected_count ? object->link.send_connected_count( object->link.transport ) : object->num_connections;
        if ( header >= 0 && object->link.send_capacity_available &&
             object->link.send_capacity_available( object->link.transport ) <= send_connections )
        {
            header = RN_ENDPOINT_IN_PLACE_QUEUED;
        }
        if ( header >= 0 && !rn_endpoint_in_place_affordable( connection, header + bytes + 1, object->time ) )
        {
            header = RN_ENDPOINT_IN_PLACE_QUEUED;               // the bucket cannot pay it now: the write pass decides
        }

        if ( header >= 0 && !frame_refused )
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
                open->in_place = 1;
                open->in_place_context = context;
                open->in_place_payload = frame + context.payload_index;
                rn_message_open_set( object, index );
                return open->in_place_payload + header;
            }
        }

        // RNOutgoingBuild StartQueued with fellback: no frame, unreliable
        // messages wait ahead of it, the update's packet allowance is spent
        // (one packet of it kept while reliable work is due), or the budget
        // cannot pay it now (MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK)

        if ( header >= 0 || header == RN_ENDPOINT_IN_PLACE_QUEUED )
        {
            rn_message_count( object, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );
        }
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
            {
                rn_region_send_room_exhausted( connection->region, object->is_server, index );
            }

            // proofs/tla/ServerSession.tla MessageEnd: a start on a region the receive side already
            // exhausted ends the slot here, before the update's kick would; a start with no room is only refused.
            // On the server a start is a fill call, and a kick reaches the
            // transport's slots, which other fillers read: the next update's
            // kick ends it instead (rn_endpoint_error reads send_error).
            if ( !object->fill_phase )
            {
                rn_message_object_kick( object, index, connection->send_error );   // R5
            }
        }

        return NULL;
    }

    rn_region_send_room_note( connection->region, object->is_server, index );

    rn_message_open_set( object, index );

    return pointer;
}

bool rn_message_object_send_room( const struct rn_message_object_t * object, int index, int channel, int reliability, int bytes )
{
    // the room query (rocketnet_client_can_send_message,
    // rocketnet_server_can_send_message): rn_message_object_start's checks
    // ahead of the endpoint, then the endpoint's own decision
    // (rn_endpoint_send_room), which shares the start's route and the
    // region's allocation decision. Pure: no count, no state, no allocation.
    // The in-place unreliable path (a transport frame taken at start) is not
    // asked: it takes no region room, and when it finds no frame the start
    // falls back to the region, which this answers for.

    if ( !object || index < 0 || index >= object->num_connections )
    {
        return false;
    }

    const rn_message_connection_t * connection = &object->connections[index];
    if ( !rn_message_send_connected( object, connection ) || connection->open.pending )
    {
        return false;
    }

    return rn_endpoint_send_room( connection, rn_message_send_channel( channel, reliability ), reliability, bytes,
                                  rn_message_send_transport_max( object, reliability ) );
}

// the functional tier's pacing reads the send room through these: 1 when a
// start message of `bytes` bytes would be given room now (rn_region_send_fits).
// Internal: declared by the test that reads them, never in rocketnet.h, never
// exported.
int rn_message_client_send_fits( struct rocketnet_message_client_t * client, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );
int rn_message_server_send_fits( struct rocketnet_message_server_t * server, int client_index, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );

static int rn_message_object_send_fits( struct rn_message_object_t * object, int index, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages )
{
    if ( !object || index < 0 || index >= object->num_connections )
    {
        return 0;
    }
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
    // Asserts in a debug build and returns 0 in a release build when no start
    // is pending on this connection, a start a reset of the connection
    // cancelled included (rn_message_connection_cancel_at_reset)

    if ( !object )
    {
        rocketnet_assert( object != NULL );
        return 0;
    }

    if ( index < 0 || index >= object->num_connections )
    {
        rocketnet_assert( index >= 0 && index < object->num_connections );
        return 0;
    }

    rocketnet_fill_phase_fill_scope_t phase( object->fill_phase, index );
    if ( !phase.entered )
    {
        return 0;                                       // the start stays open for the next update to close
    }

    rn_message_open_t * open = &object->connections[index].open;
    if ( !open->pending )
    {
        if ( open->in_place < 0 )
        {
            open->in_place = 0;
        }

        rocketnet_assert( open->pending );
        return 0;
    }

    // RNOutgoingBuild FinishInPlace: the record committed at its reservation
    // and the committed end advanced by the bytes written, so a short finish
    // returns the rest; no committed record moves (PriorRecordsUnmoved)

    if ( open->in_place == 2 )
    {
        rn_message_connection_t * connection = &object->connections[index];
        rn_message_packet_build_t * build = &connection->unreliable_build;

        if ( bytes < 1 || (uint32_t) bytes > connection->pending_bytes )
        {
            rocketnet_error( "message finish refused: bytes_written %d outside 1..reserved %u; the start is aborted", bytes, (unsigned) connection->pending_bytes );
            rn_message_connection_cancel_pending( object, index );
            return 0;
        }

        rocketnet_message_record_t record = {};
        const int type = connection->pending_type;
        if ( type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED )
        {
            uint32_t ordering = 0;
            record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
            record.channel = (uint32_t) connection->pending_channel;
            record.sequencing = rn_channels_send_sequenced( &connection->ordered.channels, record.channel, &ordering ) & ROCKETNET_MESSAGE_U24_MASK;
            record.ordering = ordering & ROCKETNET_MESSAGE_U24_MASK;
        }
        else
        {
            record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        }

        record.data = open->in_place_payload + connection->pending_header;
        record.bytes = bytes;
        if ( !rocketnet_message_writer_add_in_place( &build->writer, &record ) )
        {
            rn_message_connection_cancel_pending( object, index );
            return 0;
        }

        uint32_t serial = ++connection->serial;
        if ( !serial )
        {
            serial = ++connection->serial;
        }

        if ( type == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT )
        {
            build->receipt_serials[build->receipt_count++] = serial;
        }

        rn_endpoint_abort( connection ); // clear the reservation; committed bytes stay in the frame
        open->in_place = 0;
        open->in_place_payload = NULL;
        rn_message_open_close( object, open );

        if ( rocketnet_message_writer_space( &build->writer ) < ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1 )
        {
            rn_message_flush_unreliable( object, index, false );
        }

        return serial;
    }

    if ( open->in_place )
    {
        // RNOutgoingBuild FinishInPlace, the whole-frame case: the header and
        // form byte written around the bytes, and the frame, full, sealed at
        // once (SealBuild) and staged by the transport, the acks due riding in it

        rn_message_connection_t * connection = &object->connections[index];
        rn_message_link_t * link = &object->link;
        int payload_bytes = 0;
        const uint32_t reserved = connection->pending_bytes;
        const uint32_t serial = rn_endpoint_in_place_finish( connection, open->in_place_payload, bytes, object->time, &payload_bytes );
        if ( serial == 0 )
        {
            if ( bytes < 1 || (uint32_t) bytes > reserved )
            {
                rocketnet_error( "message finish refused: bytes_written %d outside 1..reserved %u; the start is aborted", bytes, (unsigned) reserved );
            }

            rn_message_connection_cancel_pending( object, index );     // a refused finish aborts its start: the object is never stuck
            return 0;
        }

        rocketnet_packet_context_t context = open->in_place_context;
        open->in_place = 0;
        open->in_place_payload = NULL;
        rn_message_open_close( object, open );

        if ( payload_bytes == 0 )
        {
            link->abort_packet( link->transport, &context );     // a seal's over-budget drop: dropped and counted
            return serial;
        }

        const int wire_bytes = link->finish_packet( link->transport, &context, payload_bytes );
        if ( wire_bytes > 0 )
        {
            rn_endpoint_in_place_sent( connection, context.packet_id, wire_bytes, object->time );
        }
        else
        {
            rn_endpoint_in_place_failed( connection );
        }

        return serial;
    }

    const uint32_t reserved = object->connections[index].pending_bytes;
    const uint32_t serial = rn_endpoint_finish( &object->connections[index], bytes, 0 );   // RNEndpoint Send's second half
    if ( serial == 0 )
    {
        if ( bytes < 1 || (uint32_t) bytes > reserved )
        {
            rocketnet_error( "message finish refused: bytes_written %d outside 1..reserved %u; the start is aborted", bytes, (unsigned) reserved );
        }

        rn_message_connection_cancel_pending( object, index );         // a refused finish aborts its start (proofs/tla/RNSlots.tla FinishPending)
        return 0;
    }

    rn_message_open_close( object, open );

    return serial;
}

void rn_message_object_abort( struct rn_message_object_t * object, int index )
{
    // a start that will not be finished: its room returns to the region

    if ( !object )
    {
        return;
    }

    if ( index < 0 || index >= object->num_connections )
    {
        return;
    }

    rocketnet_fill_phase_fill_scope_t phase( object->fill_phase, index );
    if ( !phase.entered )
    {
        return;
    }

    rn_message_open_t * open = &object->connections[index].open;
    if ( open->in_place < 0 )
    {
        open->in_place = 0;
    }

    if ( !open->pending )
    {
        return;
    }

    rn_message_connection_cancel_pending( object, index );
}

uint32_t rn_message_object_send( struct rn_message_object_t * object, int index, int channel, int reliability, const uint8_t * data, int bytes )
{
    // the convenience over start and finish. It copies: the caller's bytes
    // into the region record, the one copy start and finish exist to avoid

    uint8_t * pointer = rn_message_object_start( object, index, channel, reliability, bytes );
    if ( !pointer )
    {
        return 0;
    }

    memcpy( pointer, data, (size_t) bytes );

    return rn_message_object_finish( object, index, bytes );
}

#if ROCKETNET_DEV
// Unit-library instrumentation only: the send pass's slot visits and aborted
// frames on this thread (rn_send_pass_cost_t). No production state.
static thread_local rn_send_pass_cost_t rn_send_pass_cost;
void rn_send_pass_cost_reset() { memset( &rn_send_pass_cost, 0, sizeof( rn_send_pass_cost ) ); }
rn_send_pass_cost_t rn_send_pass_cost_read() { return rn_send_pass_cost; }
#define RN_SEND_PASS_COST( field ) ( rn_send_pass_cost.field++ )
#else
#define RN_SEND_PASS_COST( field ) ( (void) 0 )
#endif // #if ROCKETNET_DEV

// a started frame given up with nothing finished in it: the transport's slot
// of this update's send capacity is not returned (an abort never gives back a
// reservation, on any transport)
static void rn_message_connection_abort_frame( rn_message_link_t * link, rocketnet_packet_context_t * context )
{
    RN_SEND_PASS_COST( frames_aborted );
    link->abort_packet( link->transport, context );
}

static bool rn_message_connection_write( rn_message_object_t * object, int index )
{
    // proofs/tla/RNSlots.tla Send: writes one requested record into a transport frame under the transport's packet id (RNSlots Send).
    // RNChannel WritePacket with the send budget, for one connection: the endpoint plans the frame
    // (rn_endpoint_next_packet_bytes), the object starts one frame from the transport at a time,
    // the endpoint writes it in place, and the object finishes it.
    //
    // proofs/tla/RNSendRotation.tla ConnWrite: a frame is started only for a
    // payload that will be written (standard/MESSAGES.md m1.8, m9.4). Every
    // frame start takes a slot of the update's send capacity, which every
    // connection shares, and an abort never returns it. The window's refusal
    // is decided before a frame (rn_endpoint_next_packet_bytes asks
    // rn_window_send_would_number). The budget's cannot be: a payload's charge
    // depends on its bytes, known only once it is packed. So while the
    // connection's bucket holds less than one full-size frame
    // (rn_endpoint_write_may_be_refused) the payload is written into a stack
    // buffer first and a frame is started only when bytes came out: one copy
    // of at most one payload, paid only while the bucket is short.

    rn_message_connection_t * connection = &object->connections[index];
    rn_message_link_t * link = &object->link;
    const double now = object->time;

    alignas( 16 ) uint8_t scratch[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_taken_t taken;                                  // unreliable records written first, still queued
    taken.count = 0;

    for ( ;; )
    {
        const int request = rn_endpoint_next_packet_bytes( connection, now );
        if ( request == 0 )
        {
            return false;
        }

        const bool write_first = rn_endpoint_write_may_be_refused( connection );
        int payload_bytes = 0;
        if ( write_first )
        {
            payload_bytes = rn_endpoint_write_packet_first( connection, scratch, request, now, &taken );
            if ( payload_bytes == 0 )
            {
                continue;                                       // refused before any frame was taken
            }
        }

        rocketnet_packet_context_t context;
        uint8_t * frame = link->start_packet( link->transport, index, request, &context );
        if ( !frame )
        {
            // no frame: reliable messages written first are due again,
            // unreliable records stay queued and end the pass, as with
            // nothing written; either charge is refunded
            rn_endpoint_packet_failed( connection, 0, now );
            continue;
        }

        if ( context.payload_capacity < request )
        {
            // the frame is smaller than requested: abort it and fail the send
            // (unreachable: every transport's payload capacity is
            // ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, the most a plan requests)
            rn_message_connection_abort_frame( link, &context );
            rn_endpoint_packet_failed( connection, 0, now );
            continue;
        }

        if ( write_first )
        {
            rn_endpoint_packet_started( connection, &taken );
            memcpy( frame + context.payload_index, scratch, (size_t) payload_bytes );
        }
        else
        {
            // the bucket holds at least one full-size frame, or the connection
            // is uncapped: the budget admits it, and the endpoint writes in place
            payload_bytes = rn_endpoint_write_packet( connection, frame + context.payload_index, context.payload_capacity, context.packet_id, now );
            if ( payload_bytes == 0 )
            {
                // only an ended connection gets here: exhausted, a channel
                // error or a reliable message past its timeout, once
                rn_message_connection_abort_frame( link, &context );
                continue;
            }
        }

        const int wire_bytes = link->finish_packet( link->transport, &context, payload_bytes );
        if ( wire_bytes > 0 )
        {
            rn_endpoint_packet_sent( connection, context.packet_id, wire_bytes, now );
        }
        else
        {
            rn_endpoint_packet_failed( connection, context.packet_id, now );
        }

        return true;
    }
}

// proofs/tla/RNSendRotation.tla BeginFlush and Visit: the first round over every connection, later rounds over the last round's writers, the capacity test, the cursor at i + 1.
// proofs/tla/RNServerSendFair.tla BeginFlush, FirstVisit and LaterVisit: the same rounds at this loop's index arithmetic (send_writers, count, kept, previous, the stored cursors).
void rn_message_object_send_packets( struct rn_message_object_t * object )
{
    rocketnet_fill_phase_main_scope_t phase( object ? object->fill_phase : NULL );
    if ( !phase.entered )
    {
        return;
    }

    // RNOutgoingBuild BeginFlush: a pending in-place start aborted, then every
    // open build sealed before the write pass

    rn_message_object_close_in_place( object );
    for ( int i = 0; i < object->num_connections; ++i )
    {
        rn_message_flush_unreliable( object, i, false );
        rn_endpoint_begin_flush( &object->connections[i] );
    }

    // RNSlots Send, fair rotation: start at the first connection not yet
    // offered a frame, and stop when the transport's send capacity for this
    // update is spent. The cursor names the next unserved connection, so a
    // cap below the client count serves every client in turn rather than
    // forever serving the low indices. A connection that finds no capacity
    // leaves its queued sends for the next update; reliability keeps them
    // due, so nothing is lost.
    //
    // Rounds go on while any connection wrote, one frame per connection per
    // round, so a busy client cannot take K turns. A connection that writes
    // nothing in a round has ended its pass until the next flush
    // (rn_endpoint_next_packet_bytes), so only the connections that wrote in
    // a round can write in the next: later rounds walk the writer list, in
    // the same cyclic order, and a flush costs num_connections visits plus
    // one per frame written, however busy one client is. The cursor is set
    // as a walk over every connection would set it: past each connection
    // visited, at a capacity cut to the cursor held before the cut
    // connection's visit, and at the end past the last connected connection.

    const int num_connections = object->num_connections;
    const int start = num_connections > 0 ? object->send_next_connection % num_connections : 0;
    int32_t one_connection[2] = { 0, 0 };
    int32_t * writers = object->send_writers ? object->send_writers : one_connection;
    int count = 0;
    int previous = -1;                                          // the connected connection visited last in the first round

    for ( int offset = 0; offset < num_connections; offset++ )
    {
        const int i = ( start + offset ) % num_connections;
        RN_SEND_PASS_COST( slot_visits );
        if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED )
        {
            continue;
        }

        // the client's CONFIG waits unnumbered until the transport connects

        if ( !object->is_server && object->link.connection( object->link.transport, 0 ) == 0 )
        {
            continue;
        }
        if ( object->link.send_capacity_available &&
             object->link.send_capacity_available( object->link.transport ) <= 0 )
        {
            goto flush;
        }

        if ( rn_message_connection_write( object, i ) )
        {
            writers[2 * (size_t) count] = i;
            writers[2 * (size_t) count + 1] = previous < 0 ? -1 : ( previous + 1 ) % num_connections;
            count++;
        }

        object->send_next_connection = ( i + 1 ) % num_connections;
        previous = i;
    }

    while ( count > 0 )
    {
        int kept = 0;
        for ( int k = 0; k < count; k++ )
        {
            const int i = writers[2 * (size_t) k];
            const int cursor = writers[2 * (size_t) k + 1] < 0 ? ( previous + 1 ) % num_connections : writers[2 * (size_t) k + 1];
            RN_SEND_PASS_COST( slot_visits );
            if ( object->hot_connections[i].state != RN_MESSAGE_CONNECTION_CONNECTED ||
                 ( !object->is_server && object->link.connection( object->link.transport, 0 ) == 0 ) )
            {
                continue;
            }
            if ( object->link.send_capacity_available &&
                 object->link.send_capacity_available( object->link.transport ) <= 0 )
            {
                object->send_next_connection = cursor;
                goto flush;
            }

            if ( rn_message_connection_write( object, i ) )
            {
                writers[2 * (size_t) kept + 1] = writers[2 * (size_t) k + 1];
                writers[2 * (size_t) kept] = i;
                kept++;
            }

            object->send_next_connection = ( i + 1 ) % num_connections;
        }

        count = kept;
    }

    if ( previous >= 0 )
    {
        object->send_next_connection = ( previous + 1 ) % num_connections;
    }
flush:
    object->link.send_packets( object->link.transport );
}

// ---------------------------------------------------------------
//                          receive and release
// ---------------------------------------------------------------

// receiving pops one queue across every connection. The
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
    // proofs/tla/RNChannel.tla Take: hands the application the next delivery and advances the queue

    for ( int i = object->receive_connection; i < object->num_connections; i++ )
    {
        if ( object->hot_connections[i].state == RN_MESSAGE_CONNECTION_CONNECTED )
        {
            rn_message_connection_t * connection = &object->connections[i];
            const uint32_t deliveries = connection->ordered.channels.deliveries;
            const rn_delivery_t * table = connection->region->deliveries;
            for ( uint32_t k = connection->hot->receive_cursor; k < deliveries; k++ )
            {
                if ( connection->hot->views[k] != RN_MESSAGE_VIEW_WAITING )
                {
                    continue;
                }

                connection->hot->views[k] = RN_MESSAGE_VIEW_HELD;
                connection->hot->held++;
                connection->hot->receive_cursor = k + 1;

                object->receive_connection = i;
                *out_index = i;
                *out_entry = k;

                return &table[k];
            }

            connection->hot->receive_cursor = deliveries;
        }

        object->receive_connection = i + 1;
    }

    return NULL;
}

// proofs/tla/RNMessageViews.tla Receive: the first WAITING entry at or past the cursor is held
int rn_message_object_receive( struct rn_message_object_t * object, int * index, int * channel, const uint8_t ** data, int * bytes )
{
    rocketnet_fill_phase_main_scope_t phase( object ? object->fill_phase : NULL );
    if ( !phase.entered )
    {
        return 0;
    }

    // one delivery from the queue, valid until the next update and held until the caller lets it go

    *data = NULL;
    *bytes = 0;
    if ( index )
    {
        *index = 0;
    }
    if ( channel )
    {
        *channel = 0;
    }

    int connection_index = 0;
    uint32_t entry = 0;
    const rn_delivery_t * delivery = rn_message_object_next( object, &connection_index, &entry );
    if ( !delivery )
    {
        return 0;
    }

    if ( index )
    {
        *index = connection_index;
    }
    if ( channel )
    {
        *channel = delivery->channel;
    }
    *data = delivery->data;
    *bytes = delivery->bytes;

    return 1;
}

static bool rn_message_connection_release_view( rn_message_connection_t * connection, const uint8_t * data )
{
    if ( connection->hot->held == 0 )
    {
        return false;
    }

    const uint32_t deliveries = connection->ordered.channels.deliveries;
    const rn_delivery_t * table = connection->region->deliveries;
    uint32_t cursor = connection->hot->receive_cursor;
    if ( cursor > deliveries )
    {
        cursor = deliveries;
    }

    uint32_t k = cursor == 0 ? deliveries - 1 : cursor - 1;
    for ( uint32_t n = 0; n < deliveries; n++ )
    {
        if ( table[k].data == data && connection->hot->views[k] == RN_MESSAGE_VIEW_HELD )
        {
            connection->hot->views[k] = RN_MESSAGE_VIEW_RELEASED;
            connection->hot->held--;
            return true;
        }

        k = k == 0 ? deliveries - 1 : k - 1;
    }

    return false;
}

// proofs/tla/RNMessageViews.tla Release: the backward search from cursor - 1 for the held view of that pointer
void rn_message_object_release( struct rn_message_object_t * object, const uint8_t * data )
{
    rocketnet_fill_phase_main_scope_t phase( object ? object->fill_phase : NULL );
    if ( !phase.entered )
    {
        return;
    }

    // proofs/tla/RNSlots.tla Release: a held view returns, and the region's charge for that message returns with it.
    // A release takes the received message alone; the view is looked for in the connection the
    // queue is at first, then in every connection that holds one.

    if ( !data )
    {
        return;
    }

    const int at = object->receive_connection < object->num_connections ? object->receive_connection : object->num_connections - 1;
    if ( object->hot_connections[at].held > 0 &&
         rn_message_connection_release_view( &object->connections[at], data ) )
    {
        return;
    }

    for ( int i = 0; i < object->num_connections; i++ )
    {
        if ( i != at && object->hot_connections[i].held > 0 &&
             rn_message_connection_release_view( &object->connections[i], data ) )
        {
            return;
        }
    }

    // not a held view of any connection: the caller's error, counted

    rocketnet_api_misuse_record();
}

static void rn_message_object_release_batch( struct rn_message_object_t * object )
{
    // the batch's views go back together: each entry still HELD (a connection
    // reset since the batch cleared its views already) is released.

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

// proofs/tla/RNMessageViews.tla ReceiveBatch: the last batch released, then up to the batch size received
int rn_message_object_receive_messages( struct rn_message_object_t * object, struct rocketnet_message_batch_t * out )
{
    rocketnet_fill_phase_main_scope_t phase( object ? object->fill_phase : NULL );
    if ( !phase.entered )
    {
        return 0;
    }

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
        {
            break;
        }

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
    rocketnet_fill_phase_main_scope_t phase( object ? object->fill_phase : NULL );
    if ( !phase.entered )
    {
        return false;
    }

    if ( index < 0 || index >= object->num_connections )
    {
        return false;
    }
    return rn_endpoint_set_send_kbps( &object->connections[index], kbps, object->time );
}

int rn_message_object_disconnect_reason( const struct rn_message_object_t * object, int index )
{
    if ( index < 0 || index >= object->num_connections )
    {
        return ROCKETNET_DISCONNECT_REASON_NONE;
    }
    return object->connections[index].disconnect_reason;
}

int rn_message_object_connected( const struct rn_message_object_t * object, int index )
{
    if ( index < 0 || index >= object->num_connections )
    {
        return 0;
    }
    return object->hot_connections[index].state == RN_MESSAGE_CONNECTION_CONNECTED ? 1 : 0;
}

// ---------------------------------------------------------------
//                     the server transport's link
// ---------------------------------------------------------------

static uint64_t rn_server_link_payload_connection( void * transport, int k )
{
    // P9b: the connection each batch entry was received for, parallel to the
    // batch's client index array

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

// the link hands the layer the frame (rocketnet_message_object.h, start_packet)
static uint8_t * rn_server_link_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    return rocketnet_server_transport_start_packet( (rocketnet_server_transport_t *) transport, client_index, request_bytes, context ) ? context->frame : NULL;
}

static bool rn_message_link_splits_update( const struct rn_message_object_t * object )
{
    return object->link.update_receive && object->link.update_client_acks && object->link.update_finish &&
           object->link.update == RN_LINK_FORWARD( rocketnet_server_transport_update )::call;
}

void rn_message_server_link( struct rn_message_link_t * link, struct rocketnet_server_transport_t * transport )
{
    memset( link, 0, sizeof( *link ) );
    link->transport = transport;
    link->update = RN_LINK_FORWARD( rocketnet_server_transport_update )::call;
    link->update_receive = RN_LINK_FORWARD( rocketnet_server_transport_update_receive )::call;
    link->update_client_acks = RN_LINK_FORWARD( rocketnet_server_transport_update_client_acks )::call;
    link->update_finish = RN_LINK_FORWARD( rocketnet_server_transport_update_finish )::call;
    link->payloads = RN_LINK_FORWARD( rocketnet_server_transport_payloads )::call;
    link->accept_payload = RN_LINK_FORWARD( rocketnet_server_transport_accept_payload )::call;

    if ( transport )
    {
        rocketnet_server_transport_defer_payload_acks( transport );
    }

    link->acked = RN_LINK_FORWARD( rocketnet_server_transport_acked_packets )::call;
    link->payload_connection = rn_server_link_payload_connection;
    link->acked_connection = rn_server_link_acked_connection;
    link->acked_connection_ids = RN_LINK_FORWARD( rocketnet_server_transport_acked_connection_ids )::call;
    link->max_payload_bytes = RN_LINK_FORWARD( rocketnet_server_transport_max_payload_bytes )::call;
    link->start_packet = rn_server_link_start_packet;
    link->finish_packet = RN_LINK_FORWARD( rocketnet_server_transport_finish_packet )::call;
    link->abort_packet = RN_LINK_FORWARD( rocketnet_server_transport_abort_packet )::call;
    link->send_packets = RN_LINK_FORWARD( rocketnet_server_transport_send_packets )::call;
    link->send_capacity_available = RN_LINK_FORWARD( rocketnet_server_transport_send_capacity_available )::call;
    link->send_connected_count = RN_LINK_FORWARD( rocketnet_server_transport_num_connected_clients )::call;
    // P9a: the transport's id of the connection in the slot, 0 while it is
    // free; a reconnect into the slot has a new id even with the same client id
    // and inside one update, so Observe resets the slot for it
    link->connection = RN_LINK_FORWARD( rocketnet_server_transport_client_connection_id )::call;
    link->disconnect = RN_LINK_FORWARD( rocketnet_server_transport_disconnect_client )::call;
    link->state = RN_LINK_FORWARD( rocketnet_server_transport_state )::call;
    link->end_reason = RN_LINK_FORWARD( rocketnet_server_transport_client_end_reason )::call;
    link->rtt = RN_LINK_FORWARD( rocketnet_server_transport_client_rtt )::call;
    link->stop = RN_LINK_FORWARD( rocketnet_server_transport_stop )::call;
    link->stopped = RN_LINK_FORWARD( rocketnet_server_transport_stopped )::call_const;
    link->destroy = RN_LINK_FORWARD( rocketnet_server_transport_destroy )::call;
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

uint64_t rocketnet_message_server_required_memory( const struct rocketnet_server_transport_config_t * transport_config, const struct rocketnet_message_config_t * message_config,
                                                   uint32_t per_client_send_kbps )
{
    if ( per_client_send_kbps > ROCKETNET_SEND_BUDGET_MAX_KBPS )
    {
        return 0;
    }
    return rn_message_object_required_bytes( message_config, rn_message_server_max_clients( transport_config ), 1, (int32_t) per_client_send_kbps );
}

struct rocketnet_message_server_t * rocketnet_message_server_create( void * context, const char * address,
                                                                     const struct rocketnet_server_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps )
{
    return rocketnet_message_server_create_internal( context, address, transport_config, message_config, memory, memory_bytes, per_client_send_kbps, NULL, 0 );
}

// resolved_memory_budget NULL: the transport create reads the environment
// (rocketnet_message_server_create_internal); set: transport_config is already
// resolved and start reads nothing (rocketnet_message_server_create_resolved)
static struct rocketnet_message_server_t * rn_message_server_create( void * context, const char * address,
                                                                     const struct rocketnet_server_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps, int * status, int reserve_unlimited,
                                                                     const uint64_t * resolved_memory_budget );

struct rocketnet_message_server_t * rocketnet_message_server_create_internal( void * context, const char * address,
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
    {
        status = &ignored;
    }
    *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    if ( !rocketnet_api_null_guard( address ) )
    {
        return NULL;
    }

    // R6 (standard/MESSAGES.md m9.4): 0 takes server_send_kbps from the message
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

    // refuse before the transport opens a socket: the config, the budget, and
    // a caller block too short for the layout create sizes at this budget

    const uint64_t required_bytes = rn_message_object_required_bytes( message_config, max_clients, 1, kbps, reserve_unlimited != 0 );
    if ( required_bytes == 0 || !( kbps <= 0 || rocketnet_send_budget_kbps_valid( (uint32_t) kbps ) ) )
    {
        rocketnet_error( "message server create refused: message config, client count or send budget" );
        *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }

    if ( memory && ( memory_bytes < required_bytes || ( (uintptr_t) memory % ROCKETNET_ALLOCATION_ALIGNMENT ) != 0 ) )
    {
        rocketnet_error( "message server create refused: the memory block is %" PRIu64 " bytes or not %d-aligned; this config and send budget need %" PRIu64,
                         memory_bytes, ROCKETNET_ALLOCATION_ALIGNMENT, required_bytes );
        *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        return NULL;
    }

    rocketnet_server_transport_t * transport = ( resolved_memory_budget && transport_config ) ? rocketnet_server_transport_create_resolved( context, address, transport_config, *resolved_memory_budget, status ) : rocketnet_server_transport_create_current_internal( context, address, transport_config, 0.0, status );
    if ( !transport )
    {
        return NULL;
    }

    rn_message_link_t link;
    rn_message_server_link( &link, transport );

    rocketnet_message_server_t * server = (rocketnet_message_server_t *) rn_message_object_create( context, &link, 1, max_clients, message_config, memory, memory_bytes, kbps, 0.0, reserve_unlimited != 0 );
    if ( server )
    {
        server->object.fill_phase = rocketnet_server_transport_fill_phase( transport );
    }
    *status = server ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_OUT_OF_MEMORY;

    return server;
}

void rocketnet_message_server_destroy( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return;
    }
    rn_message_object_destroy( &server->object );
}

struct rocketnet_server_transport_t * rocketnet_message_server_transport( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return NULL;
    }
    return server->object.link.destroy == RN_LINK_FORWARD( rocketnet_server_transport_destroy )::call ? (rocketnet_server_transport_t *) server->object.link.transport : NULL;
}

int rocketnet_message_server_update( struct rocketnet_message_server_t * server, double time )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return ROCKETNET_UPDATE_OK;
    }
    if ( server->object.link.destroy == RN_LINK_FORWARD( rocketnet_server_transport_destroy )::call )
    {
        rocketnet_server_transport_begin_message_update( (rocketnet_server_transport_t *) server->object.link.transport );
    }
    return rn_message_object_update( &server->object, time );
}

void rocketnet_message_server_update_receive( struct rocketnet_message_server_t * server, double time )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return;
    }
    if ( server->object.link.destroy == RN_LINK_FORWARD( rocketnet_server_transport_destroy )::call )
    {
        rocketnet_server_transport_begin_message_update( (rocketnet_server_transport_t *) server->object.link.transport );
    }
    rn_message_object_update_begin( &server->object, time );
}

int rocketnet_message_server_update_client_acks( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return 0;
    }

    // only a split update leaves acks to apply; any other stage has none
    // (the begin returned early, or the link updated whole), and that is
    // not the caller's error

    if ( server->object.update_stage != RN_MESSAGE_UPDATE_SPLIT )
    {
        if ( server->object.update_stage == RN_MESSAGE_UPDATE_CLOSED )
        {
            rocketnet_api_misuse_record();
            rocketnet_assert( server->object.update_stage != RN_MESSAGE_UPDATE_CLOSED && "update_client_acks runs between update_receive and update_finish" );
        }
        return 0;
    }
    return server->object.link.update_client_acks( server->object.link.transport, client_index );
}

int rocketnet_message_server_update_finish( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return ROCKETNET_UPDATE_OK;
    }
    return rn_message_object_update_end( &server->object );
}

void rocketnet_message_server_send_packets( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return;
    }
    rn_message_object_send_packets( &server->object );
}

uint8_t * rocketnet_message_server_start( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return NULL;
    }
    return rn_message_object_start( &server->object, client_index, channel, reliability, bytes );
}

uint32_t rocketnet_message_server_finish( struct rocketnet_message_server_t * server, int client_index, int bytes_written )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return 0;
    }
    return rn_message_object_finish( &server->object, client_index, bytes_written );
}

void rocketnet_message_server_abort( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return;
    }
    rn_message_object_abort( &server->object, client_index );
}

uint32_t rocketnet_message_server_send( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, const uint8_t * data, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( data ) )
    {
        return 0;
    }
    return rn_message_object_send( &server->object, client_index, channel, reliability, data, bytes );
}

int rocketnet_message_server_receive( struct rocketnet_message_server_t * server, int * client_index, int * channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( data ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( bytes ) )
    {
        return 0;
    }
    return rn_message_object_receive( &server->object, client_index, channel, data, bytes );
}

int rocketnet_message_server_receive_messages( struct rocketnet_message_server_t * server, struct rocketnet_message_batch_t * messages )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( messages ) )
    {
        return 0;
    }
    return rn_message_object_receive_messages( &server->object, messages );
}

int rocketnet_message_server_receipts( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_message_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( server ) || capacity < 0 ||
         ( capacity > 0 && !rocketnet_api_null_guard( out ) ) )
    {
        return 0;
    }

    // a main-thread operation: taking receipts moves the connection's ring,
    // which its filler's finish appends to (sdk/rocketnet_fill_phase.h)

    rocketnet_fill_phase_main_scope_t phase( server->object.fill_phase );
    if ( !phase.entered )
    {
        return 0;
    }

    rn_message_connection_t * connection = rn_message_object_connection( &server->object, client_index );

    // RNReceipts TakeReceipt after Disconnect: a kicked slot's recorded
    // outcomes stay readable until its next begin_update expires them;
    // a free slot's ring is never read, and a reused slot's ring is its
    // new connection's, carved fresh at Observe.

    if ( !connection || ( connection->hot->state != RN_MESSAGE_CONNECTION_CONNECTED && connection->hot->state != RN_MESSAGE_CONNECTION_KICKED ) )
    {
        return 0;
    }
    return rn_endpoint_take_receipts( connection, out, capacity );
}

void rocketnet_message_server_release( struct rocketnet_message_server_t * server, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return;
    }
    if ( !rocketnet_api_null_guard( data ) )
    {
        return;
    }
    rn_message_object_release( &server->object, data );
}

int rocketnet_message_server_set_send_kbps( struct rocketnet_message_server_t * server, int client_index, int32_t kbps )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return 0;
    }
    return rn_message_object_set_send_kbps( &server->object, client_index, kbps ) ? 1 : 0;
}

int rocketnet_message_server_disconnect_reason( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return ROCKETNET_DISCONNECT_REASON_NONE;
    }
    return rn_message_object_disconnect_reason( &server->object, client_index );
}

int rocketnet_message_server_client_connected( struct rocketnet_message_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return 0;
    }
    return rn_message_object_connected( &server->object, client_index );
}

const struct rocketnet_counters_t * rocketnet_message_server_counters( struct rocketnet_message_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
    {
        return NULL;
    }
    return &server->object.counters;
}
