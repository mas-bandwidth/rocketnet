/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message object's server paths the mutation campaigns found unwatched
// (sdk/rocketnet_message_server.cpp): the bounds of every per-slot call and
// loop, the receive outputs on an empty queue, the slot admission of entries
// that arrive in the update a slot is bound or a client's transport ends, a
// free slot that must stay quiet, the transport's payload cap per type, the
// frames the in-place starts and the write pass take and give back, the
// whole-frame and packed finishes at their edges, the in-place markers an
// update leaves, the NULL guards of the C API, and the create refusals with
// their log lines.
//
// A message object runs over a fake link: no socket, no thread, no real time
// (time is an argument from 1000.0). Some cases plant a CONNECTED-looking hot
// slot in the padding just past (or before) the hot slot array, inside the
// object's one allocation where ASan cannot see a read: every call and loop
// bounded by num_connections must leave it unread. Each failing check prints
// the fault first.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "loopback_hub.h"

#define check rocketnet_unit_check

// a failed expectation prints the fault, then the check stops the run
#define r2ms_expect( condition, ... )                                                                             \
do                                                                                                                \
{                                                                                                                 \
    if ( !( condition ) )                                                                                         \
    {                                                                                                             \
        printf( "fault: " );                                                                                      \
        printf( __VA_ARGS__ );                                                                                    \
        printf( "\n" );                                                                                           \
        fflush( stdout );                                                                                         \
    }                                                                                                             \
    check( condition );                                                                                           \
} while ( 0 )

// ---- asserts counted, not stopping -----------------------------------------------------

static int g_r2ms_asserts = 0;

static void r2ms_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    g_r2ms_asserts++;
}

struct r2ms_assert_scope_t
{
    rocketnet_assert_function_t saved;

    r2ms_assert_scope_t()
    {
        saved = rn_assert_function;
        g_r2ms_asserts = 0;
        rocketnet_assert_function( r2ms_assert_hook );
    }

    ~r2ms_assert_scope_t()
    {
        rocketnet_assert_function( saved );
    }
};

// ---- the error log, captured ------------------------------------------------------------

#define R2MS_LOG_LINES 16

static int g_r2ms_log_count = 0;
static char g_r2ms_log[R2MS_LOG_LINES][512];

static void r2ms_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    if ( level != ROCKETNET_LOG_LEVEL_ERROR || g_r2ms_log_count >= R2MS_LOG_LINES )
    {
        return;
    }
    const size_t n = length < sizeof( g_r2ms_log[0] ) - 1 ? length : sizeof( g_r2ms_log[0] ) - 1;
    memcpy( g_r2ms_log[g_r2ms_log_count], message, n );
    g_r2ms_log[g_r2ms_log_count][n] = '\0';
    g_r2ms_log_count++;
}

struct r2ms_log_scope_t
{
    enum rocketnet_log_level_t saved_level;

    r2ms_log_scope_t()
    {
        saved_level = rocketnet_log_level_current();
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
        rocketnet_quiet( 0 );
        g_r2ms_log_count = 0;
        rocketnet_log_function( r2ms_log_capture, NULL );
    }

    ~r2ms_log_scope_t()
    {
        rocketnet_log_function( NULL, NULL );
        rocketnet_log_level( saved_level );
        rocketnet_quiet( 1 );
    }
};

// how many captured error lines contain `text`
static int r2ms_logged( const char * text )
{
    int found = 0;
    for ( int i = 0; i < g_r2ms_log_count; i++ )
    {
        found += strstr( g_r2ms_log[i], text ) != NULL ? 1 : 0;
    }
    return found;
}

// ---- the fake link -----------------------------------------------------------------------

#define R2MS_SLOTS          8
#define R2MS_PAYLOAD_INDEX  32
#define R2MS_T0             1000.0
#define R2MS_DT             ( 1.0 / 60.0 )

struct r2ms_transport_t
{
    uint64_t next_id;
    uint64_t connection_id[R2MS_SLOTS];     // the connection in each slot, 0 when free (the client's is slot 0)
    int state;                              // state(): the client transport's state, or the server's RUNNING
    int max_payload;                        // max_payload_bytes() answers this
    int start_limit;                        // above 0: no frame for a request above this many bytes
    int short_by;                           // frames are given this many payload bytes short of the request
    int null_with_capacity;                 // 1: start_packet returns NULL with payload_capacity set to the maximum
    int fail_finish;                        // 1: finish_packet refuses every frame
    int ack_finished;                       // 1: a flush's frames are acknowledged at the next update
    uint64_t tag;                           // a tagged link's payload_connection and acked_connection answer this
    int starts;
    int finishes;
    int disconnects;
    std::vector<uint64_t> started_ids;
    std::vector<uint64_t> aborted_ids;
    std::vector<int> inject_acked_slot;     // acks the test hands the next update
    std::vector<uint64_t> inject_acked;
    std::vector<int> acked_slot;            // this update's acks: the injected, then the last flush's frames
    std::vector<uint64_t> acked;
    std::vector<int> pending_slot;
    std::vector<uint64_t> pending;
    std::vector<int> inbound_slot;          // the payloads the next update presents
    std::vector<uint8_t *> inbound_data;
    std::vector<int> inbound_bytes;
};

alignas( 16 ) static uint8_t r2ms_frame[R2MS_PAYLOAD_INDEX + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
static uint8_t r2ms_junk_payload[8] = { 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE };

static void r2ms_update( void * transport, double time )
{
    r2ms_transport_t * t = (r2ms_transport_t *) transport;
    (void) time;
    t->acked.swap( t->inject_acked );       // acks the test hands this update, then the last flush's frames
    t->acked_slot.swap( t->inject_acked_slot );
    t->inject_acked.clear();
    t->inject_acked_slot.clear();
    if ( t->ack_finished )
    {
        t->acked.insert( t->acked.end(), t->pending.begin(), t->pending.end() );
        t->acked_slot.insert( t->acked_slot.end(), t->pending_slot.begin(), t->pending_slot.end() );
    }
    t->pending.clear();
    t->pending_slot.clear();
}

static int r2ms_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    r2ms_transport_t * t = (r2ms_transport_t *) transport;
    *client_index = t->inbound_slot.data();
    *payload_data = t->inbound_data.data();
    *payload_bytes = t->inbound_bytes.data();
    return (int) t->inbound_data.size();
}

static int r2ms_client_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    const int count = r2ms_payloads( transport, client_index, payload_data, payload_bytes );
    *client_index = NULL;                   // the client's link leaves the index NULL
    return count;
}

static int r2ms_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    r2ms_transport_t * t = (r2ms_transport_t *) transport;
    *client_index = t->acked_slot.data();
    *packet_ids = t->acked.data();
    return (int) t->acked.size();
}

static int r2ms_client_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    const int count = r2ms_acked( transport, client_index, packet_ids );
    *client_index = NULL;
    return count;
}

static uint64_t r2ms_tag( void * transport, int k )
{
    (void) k;
    return ( (r2ms_transport_t *) transport )->tag;
}

static int r2ms_max_payload_bytes( void * transport )
{
    return ( (r2ms_transport_t *) transport )->max_payload;
}

static uint8_t * r2ms_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    r2ms_transport_t * t = (r2ms_transport_t *) transport;
    memset( context, 0, sizeof( *context ) );
    check( request_bytes >= 1 && request_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    if ( t->starts >= 10000 )
    {
        return NULL;                        // a pass that never ends stops here, and its checks name it
    }
    if ( t->null_with_capacity )
    {
        context->payload_index = R2MS_PAYLOAD_INDEX;
        context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        return NULL;
    }
    if ( ( t->start_limit > 0 && request_bytes > t->start_limit ) || request_bytes > t->max_payload )
    {
        return NULL;                        // a transport gives no frame above its payload cap
    }
    context->frame = r2ms_frame;
    context->payload_index = R2MS_PAYLOAD_INDEX;
    context->payload_capacity = request_bytes - t->short_by;
    context->packet_id = ++t->next_id;
    context->client_index = client_index;
    t->starts++;
    t->started_ids.push_back( context->packet_id );
    return r2ms_frame;
}

static int r2ms_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    r2ms_transport_t * t = (r2ms_transport_t *) transport;
    if ( t->fail_finish )
    {
        return 0;
    }
    t->finishes++;
    t->pending.push_back( context->packet_id );
    t->pending_slot.push_back( context->client_index );
    return ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + payload_bytes;
}

static void r2ms_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    r2ms_transport_t * t = (r2ms_transport_t *) transport;
    t->aborted_ids.push_back( context->packet_id );
}

static void r2ms_send_packets( void * transport )
{
    (void) transport;
}

static uint64_t r2ms_connection( void * transport, int client_index )
{
    r2ms_transport_t * t = (r2ms_transport_t *) transport;
    check( client_index >= 0 && client_index < R2MS_SLOTS );
    return t->connection_id[client_index];
}

static void r2ms_disconnect( void * transport, int client_index )
{
    (void) client_index;
    ( (r2ms_transport_t *) transport )->disconnects++;
}

static int r2ms_state( void * transport )
{
    return ( (r2ms_transport_t *) transport )->state;
}

// ---- a message object over it -------------------------------------------------------------

struct r2ms_world_t
{
    r2ms_transport_t * t;
    rn_message_object_t * object;
    rocketnet_message_server_t * server;    // the object, for the server's C API (servers only)
    double time;
};

static void r2ms_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->num_channels = 1;
    config->reliable_messages_in_flight = 128;
    config->packets_in_flight = 128;
    config->receive_message_buffer_bytes = 4096;
    config->max_message_bytes = 4096;
    config->send_message_buffer_bytes = 131072;
    config->send_queue_messages = 128;
    config->packets_per_update = 64;
    config->unreliable_messages_per_update = 16;
    config->max_message_receipts = 16;
    config->sequenced_messages_held = 0;
    config->server_send_kbps = 0;
    config->client_send_kbps = 0;
    check( rocketnet_message_validate_config( config ) );
}

static void r2ms_link( rn_message_link_t * link, r2ms_transport_t * t, int is_server, bool tagged, bool max_payload )
{
    memset( link, 0, sizeof( *link ) );
    link->transport = t;
    link->update = r2ms_update;
    link->payloads = is_server ? r2ms_payloads : r2ms_client_payloads;
    link->acked = is_server ? r2ms_acked : r2ms_client_acked;
    link->payload_connection = tagged ? r2ms_tag : NULL;
    link->acked_connection = tagged ? r2ms_tag : NULL;
    link->max_payload_bytes = max_payload ? r2ms_max_payload_bytes : NULL;
    link->start_packet = r2ms_start_packet;
    link->finish_packet = r2ms_finish_packet;
    link->abort_packet = r2ms_abort_packet;
    link->send_packets = r2ms_send_packets;
    link->connection = r2ms_connection;
    link->disconnect = r2ms_disconnect;
    link->state = r2ms_state;
}

// slots connections; bound[i] nonzero binds slot i at the first update (a
// client binds its one connection at create). Warm: update, flush, update,
// so each CONFIG went out and was acknowledged.
static void r2ms_create( r2ms_world_t * w, int is_server, int slots, const int * bound, int32_t kbps, bool tagged = false, bool max_payload = true )
{
    check( slots >= 1 && slots <= R2MS_SLOTS );
    rocketnet_message_config_t config;
    r2ms_config( &config );

    w->t = new r2ms_transport_t();
    w->t->next_id = 9000;
    w->t->state = is_server ? ROCKETNET_SERVER_TRANSPORT_RUNNING : ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    w->t->max_payload = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    w->t->ack_finished = 1;
    w->t->tag = 1;
    for ( int i = 0; i < slots; i++ )
    {
        w->t->connection_id[i] = bound[i] ? (uint64_t) i + 1 : 0;
    }

    rn_message_link_t link;
    r2ms_link( &link, w->t, is_server, tagged, max_payload );
    w->object = rn_message_object_create( NULL, &link, is_server, slots, &config, NULL, 0, kbps, R2MS_T0 );
    check( w->object != NULL );
    w->server = (rocketnet_message_server_t *) w->object;
    w->time = R2MS_T0;
}

static void r2ms_step( r2ms_world_t * w )
{
    check( rn_message_object_update( w->object, w->time ) == ROCKETNET_UPDATE_OK );
    w->t->inbound_slot.clear();
    w->t->inbound_data.clear();
    w->t->inbound_bytes.clear();
    w->time += R2MS_DT;
}

static void r2ms_warm( r2ms_world_t * w )
{
    r2ms_step( w );
    rn_message_object_send_packets( w->object );
    r2ms_step( w );
}

static void r2ms_destroy( r2ms_world_t * w )
{
    rn_message_object_destroy( w->object );
    delete w->t;
    w->t = NULL;
    w->object = NULL;
    w->server = NULL;
}

static uint64_t r2ms_counter( const r2ms_world_t * w, int counter )
{
    return w->object->counters.values[counter];
}

// ---- the planted hot slots ----------------------------------------------------------------
//
// The hot slot array is padded to 64 bytes: with 3 slots of 32 bytes, slot
// [3] is padding nobody writes. The slot before the array is padding when the
// object struct ends 32 or more bytes short of its 64-byte boundary.

static bool r2ms_padding_past( const rn_message_object_t * object )
{
    const uint8_t * past = (const uint8_t *) &object->hot_connections[object->num_connections];
    return past + sizeof( rn_message_connection_hot_t ) <= (const uint8_t *) object->connections;
}

static bool r2ms_padding_before( const rn_message_object_t * object )
{
    const uint8_t * before = (const uint8_t *) &object->hot_connections[-1];
    return before >= (const uint8_t *) object + sizeof( rn_message_object_t );
}

static void r2ms_plant( rn_message_connection_hot_t * slot, uint32_t held )
{
    memset( slot, 0, sizeof( *slot ) );
    slot->connection_id = 99;
    slot->held = held;
    slot->state = RN_MESSAGE_CONNECTION_CONNECTED;
}

static bool r2ms_planted_intact( const rn_message_connection_hot_t * slot, uint32_t held )
{
    rn_message_connection_hot_t expected;
    r2ms_plant( &expected, held );
    return memcmp( slot, &expected, sizeof( expected ) ) == 0;
}

// =====================================================================================
// receive on an empty queue clears every output, and the queue's walk stops at
// the last connection
// mutation gap: hunt 1/1767, 1/1769, 1/1772, 1/1775 (rn_message_object_receive's output clears)
// mutation gap: hunt 1/1748 (rn_message_object_next, i <= num_connections)

static void test_gaps_r2ms_receive_on_an_empty_queue_clears_every_output()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_warm( &w );

    int index = 7;
    int channel = 7;
    const uint8_t * data = (const uint8_t *) &index;
    int bytes = 7;
    const int got = rocketnet_message_server_receive( w.server, &index, &channel, &data, &bytes );
    r2ms_expect( got == 0, "mutation gap hunt 1/1767..1775: receive on an empty queue returned %d", got );
    r2ms_expect( data == NULL, "mutation gap hunt 1/1767: receive on an empty queue left the caller's data pointer %p, not NULL", (const void *) data );
    r2ms_expect( bytes == 0, "mutation gap hunt 1/1769: receive on an empty queue left the caller's bytes %d, not 0", bytes );
    r2ms_expect( index == 0, "mutation gap hunt 1/1772: receive on an empty queue left the caller's client index %d, not 0", index );
    r2ms_expect( channel == 0, "mutation gap hunt 1/1775: receive on an empty queue left the caller's channel %d, not 0", channel );

    // the walk ends at the last connection: the cursor is num_connections, never past

    r2ms_expect( w.object->receive_connection == w.object->num_connections,
                 "mutation gap hunt 1/1748: the receive queue's walk left its cursor at %d with %d connections: it read the hot slot past the last",
                 w.object->receive_connection, w.object->num_connections );

    rocketnet_message_batch_t batch;
    check( rocketnet_message_server_receive_messages( w.server, &batch ) == 0 );
    r2ms_expect( w.object->receive_connection == w.object->num_connections,
                 "mutation gap hunt 1/1748: the batch receive left the queue's cursor at %d with %d connections", w.object->receive_connection,
                 w.object->num_connections );
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_receive_on_an_empty_queue_clears_every_output );

// =====================================================================================
// every per-slot call refuses the index one past the slots, and every update
// loop stops at the last slot, with a CONNECTED-looking hot slot planted in
// the padding just past the array (and just before it when that is padding)
// mutation gap: hunt 1/855 (rn_message_object_conditions, index > num_connections)
// mutation gap: hunt 1/1878 (rn_message_object_connected, the early return deleted)
// mutation gap: hunt 1/876, 1/879, 1/882 (rn_message_object_delivery_capture's guards, DEV)
// mutation gap: hunt 1/1277 (the update's tick loop, i <= num_connections)

static void test_gaps_r2ms_slot_calls_never_read_a_planted_slot()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 0 };       // slot 2 stays free
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_warm( &w );

    rn_message_object_t * object = w.object;
    r2ms_expect( r2ms_padding_past( object ),
                 "the hot slot array of 3 slots has no padding slot past it: this case's planted slot would overwrite live state" );
    const bool before = r2ms_padding_before( object );

    rn_message_connection_hot_t * past = &object->hot_connections[3];
    rn_message_connection_hot_t * prior = &object->hot_connections[-1];
    r2ms_plant( past, 0 );
    if ( before )
    {
        r2ms_plant( prior, 0 );
    }

    r2ms_assert_scope_t asserts;
    const int indices[2] = { 3, -1 };
    for ( int n = 0; n < ( before ? 2 : 1 ); n++ )
    {
        const int index = indices[n];

        rocketnet_conditions_t conditions;
        memset( &conditions, 0x5A, sizeof( conditions ) );
        const bool answered = rocketnet_message_server_conditions( w.server, index, &conditions );
        r2ms_expect( !answered, "mutation gap hunt 1/855: conditions answered for client index %d of 3 slots: it read a slot outside the array", index );

        const int connected = rocketnet_message_server_client_connected( w.server, index );
        r2ms_expect( connected == 0, "mutation gap hunt 1/1878: client_connected answered %d for client index %d of 3 slots: it read a slot outside the array",
                     connected, index );

#if ROCKETNET_DEV
        rocketnet_delivery_capture_t capture;
        rocketnet_delivery_capture_t sentinel;
        memset( &capture, 0x5A, sizeof( capture ) );
        memset( &sentinel, 0x5A, sizeof( sentinel ) );
        rocketnet_message_server_delivery_capture( w.server, index, &capture );
        r2ms_expect( memcmp( &capture, &sentinel, sizeof( capture ) ) == 0,
                     "mutation gap hunt 1/876,879: delivery_capture filled the capture for client index %d of 3 slots: it read a slot outside the array", index );
#endif // #if ROCKETNET_DEV
    }

#if ROCKETNET_DEV
    {
        // a free slot is not captured

        rocketnet_delivery_capture_t capture;
        rocketnet_delivery_capture_t sentinel;
        memset( &capture, 0x5A, sizeof( capture ) );
        memset( &sentinel, 0x5A, sizeof( sentinel ) );
        rocketnet_message_server_delivery_capture( w.server, 2, &capture );
        r2ms_expect( memcmp( &capture, &sentinel, sizeof( capture ) ) == 0, "mutation gap hunt 1/882: delivery_capture filled the capture for the free slot 2" );
    }
#endif // #if ROCKETNET_DEV

    // the update and flush walk the slots and stop at the last; a loop that
    // reads the planted slot past the array ticks a connection past the
    // connection array (a crash, or the planted words or counters changed)

    rocketnet_counters_t counters_before = object->counters;
    for ( int u = 0; u < 3; u++ )
    {
        r2ms_step( &w );
        rn_message_object_send_packets( object );
    }
    r2ms_expect( r2ms_planted_intact( past, 0 ), "mutation gap hunt 1/1277: an update changed the planted hot slot past the array" );
    r2ms_expect( memcmp( &counters_before, &object->counters, sizeof( counters_before ) ) == 0,
                 "mutation gap hunt 1/1277: three quiet updates changed a message counter: an update loop walked past the last slot" );
    check( g_r2ms_asserts == 0 );

    memset( past, 0, sizeof( *past ) );
    if ( before )
    {
        memset( prior, 0, sizeof( *prior ) );
    }
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_slot_calls_never_read_a_planted_slot );

// =====================================================================================
// a release past the end of the queue on a one-connection object looks in its
// one connection, never at the slot before the array
// mutation gap: hunt 1/1820 (rn_message_object_release, num_connections - 2)

static void test_gaps_r2ms_release_at_the_queue_end_reads_no_slot_before_the_array()
{
    rocketnet_quiet( 1 );
    const int bound[1] = { 1 };
    r2ms_world_t w;
    r2ms_create( &w, 0, 1, bound, 0 );
    r2ms_warm( &w );

    rn_message_object_t * object = w.object;

    // the queue drained: its cursor is past the one connection

    int index = 0;
    int channel = 0;
    const uint8_t * data = NULL;
    int bytes = 0;
    check( rn_message_object_receive( object, &index, &channel, &data, &bytes ) == 0 );
    check( object->receive_connection == 1 );

    // a release of a pointer no connection holds: the caller's error, counted
    // once, and nothing of the object changed. Slot -1's held count lies in
    // the object struct's tail padding, past its last member, on the 64-bit
    // layouts (planted only where it is padding): a release that starts its
    // search at slot -1 reads it as a held count and walks a connection
    // before the connection array.

    rn_message_connection_hot_t * prior = &object->hot_connections[-1];
    const uint8_t * held_at = (const uint8_t *) &prior->held;
    const bool plant = held_at >= (const uint8_t *) &object->owns_block + sizeof( object->owns_block ) &&
                       held_at + sizeof( prior->held ) <= (const uint8_t *) object->hot_connections;
    if ( plant )
    {
        prior->held = 1;
    }

    rn_message_object_t copy;
    memcpy( (void *) &copy, (const void *) object, sizeof( copy ) );
    const uint64_t misuse_before = rocketnet_api_misuse_count();
    rn_message_object_release( object, r2ms_junk_payload );
    const uint64_t misuse_after = rocketnet_api_misuse_count();
    r2ms_expect( memcmp( (const void *) &copy, (const void *) object, sizeof( copy ) ) == 0,
                 "mutation gap hunt 1/1820: a release past the queue's end changed the object struct before the hot array" );
    r2ms_expect( misuse_after == misuse_before + 1, "mutation gap hunt 1/1820: a release of a pointer no connection holds counted %llu misuse, not 1",
                 (unsigned long long) ( misuse_after - misuse_before ) );
    if ( plant )
    {
        prior->held = 0;
    }
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_release_at_the_queue_end_reads_no_slot_before_the_array );

#if ROCKETNET_DEV

// =====================================================================================
// the delivery capture's budget fields are the connection's budget, and 0 for
// an uncapped connection
// mutation gap: hunt 1/897, 1/898, 1/899 (rn_message_object_delivery_capture, DEV)

static void test_gaps_r2ms_delivery_capture_reads_the_budget()
{
    rocketnet_quiet( 1 );
    const int bound[2] = { 1, 1 };
    for ( int capped = 0; capped < 2; capped++ )
    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 2, bound, capped ? 256 : 0 );
        r2ms_warm( &w );

        const rn_ordered_t * ordered = &w.object->connections[0].ordered;
        check( ( ordered->budget != NULL ) == ( capped != 0 ) );
        if ( capped )
        {
            w.object->connections[0].budget.reliable_waiting = 1;   // a reliable item of the last pass waiting on tokens
        }

        rocketnet_delivery_capture_t capture;
        memset( &capture, 0x5A, sizeof( capture ) );
        rocketnet_message_server_delivery_capture( w.server, 0, &capture );
        const int waiting = capped ? (int) ordered->budget->reliable_waiting : 0;
        const double tokens = capped ? ordered->budget->tokens : 0.0;
        r2ms_expect( capture.budget_reliable_waiting == waiting, "mutation gap hunt 1/897,898: a %s connection's capture reports budget_reliable_waiting %d, not %d",
                     capped ? "capped" : "uncapped", capture.budget_reliable_waiting, waiting );
        r2ms_expect( capture.budget_tokens == tokens, "mutation gap hunt 1/899: a %s connection's capture reports budget_tokens %g, not %g", capped ? "capped" : "uncapped",
                     capture.budget_tokens, tokens );
        r2ms_destroy( &w );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2ms_delivery_capture_reads_the_budget );

#endif // #if ROCKETNET_DEV

// =====================================================================================
// an untagged entry of a slot bound in this update is discarded: the slot's
// first entries are its new connection's only when the link tags them
// mutation gap: hunt 1/1169 (rn_message_admit, changed ? 1 : id)

static void test_gaps_r2ms_untagged_entry_of_a_slot_bound_this_update_is_discarded()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 0, 0, 0 };
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_step( &w );

    // slot 0 is bound to connection 1 in this update, and a payload for it
    // arrives in the same update on a link with no tags

    w.t->connection_id[0] = 1;
    w.t->inbound_slot.push_back( 0 );
    w.t->inbound_data.push_back( r2ms_junk_payload );
    w.t->inbound_bytes.push_back( (int) sizeof( r2ms_junk_payload ) );
    const uint64_t stale = r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
    r2ms_step( &w );

    const uint64_t counted = r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) - stale;
    r2ms_expect( counted == 1, "mutation gap hunt 1/1169: an untagged payload in the update slot 0 was bound counted %llu stale entries, not 1: it was applied to the new connection",
                 (unsigned long long) counted );
    r2ms_expect( rocketnet_message_server_client_connected( w.server, 0 ) == 1, "mutation gap hunt 1/1169: slot 0 is not connected after its binding update" );
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_untagged_entry_of_a_slot_bound_this_update_is_discarded );

// =====================================================================================
// a client whose transport ended this update discards that update's payload
// and ack, even when the link tags them with the old connection
// mutation gap: hunt 1/1253, 1/1254 (the client's reset Observe, rn_slots_observe( slots, 0, 0 ))

static void test_gaps_r2ms_client_ended_this_update_discards_its_entries()
{
    rocketnet_quiet( 1 );
    const int bound[1] = { 1 };
    r2ms_world_t w;
    r2ms_create( &w, 0, 1, bound, 0, true );
    r2ms_warm( &w );
    check( rn_message_object_connected( w.object, 0 ) == 1 );

    // the transport ends and still hands over a payload and an ack of the old
    // connection in the same update (tagged 1, the client's connection)

    w.t->state = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
    w.t->connection_id[0] = 0;
    w.t->ack_finished = 0;
    w.t->inject_acked.push_back( 4242 );
    w.t->inject_acked_slot.push_back( 0 );
    w.t->inbound_slot.push_back( 0 );
    w.t->inbound_data.push_back( r2ms_junk_payload );
    w.t->inbound_bytes.push_back( (int) sizeof( r2ms_junk_payload ) );

    const uint64_t stale = r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
    check( rn_message_object_update( w.object, w.time ) == ROCKETNET_UPDATE_OK );
    const uint64_t counted = r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) - stale;
    r2ms_expect( counted == 2, "mutation gap hunt 1/1253,1254: the update the client's transport ended counted %llu stale entries, not 2 (its ack and payload): the reset left the slot bound",
                 (unsigned long long) counted );
    r2ms_expect( w.object->slots.connection_id[0] == 0, "mutation gap hunt 1/1253,1254: the client's slot is still bound to %llu after its transport ended",
                 (unsigned long long) w.object->slots.connection_id[0] );
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_client_ended_this_update_discards_its_entries );

// =====================================================================================
// a slot freed with reliable messages and receipts in flight, and its CONFIG
// never received, stays quiet: no counter, no disconnect and no reason change
// over updates past every timeout
// mutation gap: hunt 1/1281 (the tick loop's continue for a slot not connected)
// mutation gap: hunt 1/1295 (the CONFIG and kick loop's continue for a slot not connected)

static void test_gaps_r2ms_freed_slot_stays_quiet_past_every_timeout()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 0, 0 };
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_warm( &w );

    // in flight on slot 0, never acknowledged

    w.t->ack_finished = 0;
    uint8_t message[64];
    memset( message, 0x33, sizeof( message ) );
    check( rn_message_object_send( w.object, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
    check( rn_message_object_send( w.object, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, message, (int) sizeof( message ) ) != 0 );
    check( rn_message_object_send( w.object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, message, (int) sizeof( message ) ) != 0 );
    rn_message_object_send_packets( w.object );

    // the transport frees slot 0

    w.t->connection_id[0] = 0;
    r2ms_step( &w );
    check( rn_message_object_connected( w.object, 0 ) == 0 );
    check( w.object->hot_connections[0].state == RN_MESSAGE_CONNECTION_FREE );

    const rocketnet_counters_t counters_before = w.object->counters;
    const int reason_before = rn_message_object_disconnect_reason( w.object, 0 );
    const int disconnects_before = w.t->disconnects;

    // no other slot is connected: every counter that moves from here on is
    // the free slot's. Past every reliable timeout and the CONFIG wait.

    for ( int u = 0; u < 400; u++ )
    {
        w.time += 0.5;
        check( rn_message_object_update( w.object, w.time ) == ROCKETNET_UPDATE_OK );
        rn_message_object_send_packets( w.object );
    }

    int changed = -1;
    for ( int c = 0; c < ROCKETNET_NUM_COUNTERS; c++ )
    {
        if ( w.object->counters.values[c] != counters_before.values[c] && changed < 0 )
        {
            changed = c;
        }
    }
    r2ms_expect( changed < 0, "mutation gap hunt 1/1281,1295: counter %d went from %llu to %llu while only the free slot 0 had work: a loop worked a slot that is not connected",
                 changed, changed < 0 ? 0ULL : (unsigned long long) counters_before.values[changed],
                 changed < 0 ? 0ULL : (unsigned long long) w.object->counters.values[changed] );
    const int reason_after = rn_message_object_disconnect_reason( w.object, 0 );
    r2ms_expect( reason_after == reason_before, "mutation gap hunt 1/1281,1295: the free slot 0's disconnect reason went from %d to %d", reason_before, reason_after );
    r2ms_expect( w.t->disconnects == disconnects_before, "mutation gap hunt 1/1281,1295: the link was asked to disconnect %d times after slot 0 was freed",
                 w.t->disconnects - disconnects_before );
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_freed_slot_stays_quiet_past_every_timeout );

// =====================================================================================
// the transport's payload cap applies to each of the three unreliable types: a
// message the cap cannot carry in one packet never goes in place
// mutation gap: hunt 1/1318, 1/1320, 1/1322 (rn_message_send_transport_max, == to !=)

static void test_gaps_r2ms_payload_cap_applies_to_each_unreliable_type()
{
    rocketnet_quiet( 1 );
    const int types[3] = { ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT };
    const char * names[3] = { "UNRELIABLE", "UNRELIABLE_SEQUENCED", "UNRELIABLE_WITH_ACK_RECEIPT" };
    const char * ids[3] = { "1/1318", "1/1320", "1/1322" };
    const int bound[3] = { 1, 1, 1 };
    for ( int t = 0; t < 3; t++ )
    {
        for ( int capped = 0; capped < 2; capped++ )
        {
            r2ms_world_t w;
            r2ms_create( &w, 1, 3, bound, 0 );
            w.t->max_payload = capped ? 600 : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
            r2ms_warm( &w );

            uint8_t * p = rn_message_object_start( w.object, 0, 0, types[t], 700 );
            r2ms_expect( p != NULL, "a %s start of 700 bytes was refused (transport cap %d)", names[t], w.t->max_payload );
            if ( capped )
            {
                // no frame above the cap: the start goes to the region, which
                // keeps the cap for the record (a record past it is promoted
                // to its reliable twin)

                const int kept = w.object->connections[0].max_payload_bytes;
                r2ms_expect( kept == 600, "mutation gap hunt %s: a %s start of 700 bytes over a 600-byte transport cap reserved under a cap of %d: the transport's cap was not applied to its type",
                             ids[t], names[t], kept );
                check( w.object->in_place == 0 );
            }
            else
            {
                check( w.object->in_place == 2 );   // the control: uncapped, it packs in place
            }
            rn_message_object_abort( w.object, 0 );
            check( w.object->pending_connection == -1 && w.object->in_place == 0 );
            r2ms_destroy( &w );
        }
    }
}

REGISTER_UNIT_TEST( test_gaps_r2ms_payload_cap_applies_to_each_unreliable_type );

// =====================================================================================
// the in-place starts give back every frame they cannot use: a NULL frame is
// never written into, and a frame short of the request is aborted, packed and
// whole-frame alike, and the start falls back to the region
// mutation gap: hunt 1/1388 (frame && capacity, && to ||)
// mutation gap: hunt 1/1393 (the packed start's short frame not aborted)
// mutation gap: hunt 1/1426, 1/1427 (the whole-frame start's short frame not aborted, not dropped)

static void test_gaps_r2ms_in_place_start_gives_back_frames_it_cannot_use()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };

    // a NULL frame that came with a capacity

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 0 );
        r2ms_warm( &w );
        w.t->null_with_capacity = 1;
        const uint64_t fallback = r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );
        uint8_t * p = rn_message_object_start( w.object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 );
        r2ms_expect( w.object->in_place == 0, "mutation gap hunt 1/1388: a start given a NULL frame went in place (%d), writing at %p", w.object->in_place, (void *) p );
        r2ms_expect( p != NULL, "a start given no frame was refused instead of falling back to the region" );
        check( r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == fallback + 1 );
        rn_message_object_abort( w.object, 0 );
        w.t->null_with_capacity = 0;
        r2ms_destroy( &w );
    }

    // frames one byte short of each request

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 0 );
        r2ms_warm( &w );
        w.t->short_by = 1;
        w.t->started_ids.clear();
        w.t->aborted_ids.clear();
        const uint64_t fallback = r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );
        uint8_t * p = rn_message_object_start( w.object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 );
        r2ms_expect( w.object->in_place == 0, "mutation gap hunt 1/1427: a start given a frame short of its request went in place (%d)", w.object->in_place );
        r2ms_expect( w.t->started_ids.size() == 2, "a start given short frames asked for %d frames, not 2 (packed, then whole)", (int) w.t->started_ids.size() );
        const bool packed_back = w.t->aborted_ids.size() >= 1 && w.t->aborted_ids[0] == w.t->started_ids[0];
        r2ms_expect( packed_back, "mutation gap hunt 1/1393: the packed start's short frame %llu was not given back (%d aborted)",
                     (unsigned long long) w.t->started_ids[0], (int) w.t->aborted_ids.size() );
        const bool whole_back = w.t->aborted_ids.size() == 2 && w.t->aborted_ids[1] == w.t->started_ids[1];
        r2ms_expect( whole_back, "mutation gap hunt 1/1426: the whole-frame start's short frame %llu was not given back (%d aborted)",
                     (unsigned long long) w.t->started_ids[1], (int) w.t->aborted_ids.size() );
        r2ms_expect( p != NULL, "a start given short frames was refused instead of falling back to the region" );
        check( r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == fallback + 1 );
        rn_message_object_abort( w.object, 0 );
        w.t->short_by = 0;
        r2ms_destroy( &w );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2ms_in_place_start_gives_back_frames_it_cannot_use );

// =====================================================================================
// a link with no start_packet takes no in-place path: an unreliable start goes
// to the region, counted as no fallback, and finishes
// mutation gap: hunt 1/1402 (the whole-frame start's unreliable && start_packet, && to ||)

static void test_gaps_r2ms_link_without_start_packet_starts_in_the_region()
{
    rocketnet_quiet( 1 );
    rocketnet_message_config_t config;
    r2ms_config( &config );
    r2ms_transport_t * t = new r2ms_transport_t();
    t->next_id = 1;
    t->state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    t->max_payload = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    t->connection_id[0] = 1;

    rn_message_link_t link;
    r2ms_link( &link, t, 0, false, true );
    link.start_packet = NULL;                   // a bare link: the object only starts and finishes
    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1, &config, NULL, 0, 0, R2MS_T0 );
    check( object != NULL );

    const int types[3] = { ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT };
    for ( int k = 0; k < 3; k++ )
    {
        const uint64_t fallback = object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK];
        uint8_t * p = rn_message_object_start( object, 0, 0, types[k], 100 );
        r2ms_expect( p != NULL && object->in_place == 0, "mutation gap hunt 1/1402: an unreliable start (type %d) on a link with no start_packet gave %p, in_place %d",
                     types[k], (void *) p, object->in_place );
        const uint64_t counted = object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK] - fallback;
        r2ms_expect( counted == 0, "mutation gap hunt 1/1402: an unreliable start on a link with no start_packet counted %llu in-place fallbacks: it took the in-place path",
                     (unsigned long long) counted );
        memset( p, 0x31, 100 );
        check( rn_message_object_finish( object, 0, 100 ) != 0 );
    }
    rn_message_object_destroy( object );
    delete t;
}

REGISTER_UNIT_TEST( test_gaps_r2ms_link_without_start_packet_starts_in_the_region );

// =====================================================================================
// the write pass gives back a frame smaller than its plan, and the message
// goes in a later flush
// mutation gap: hunt 1/1627 (rn_message_connection_write, the short frame not aborted)

static void test_gaps_r2ms_write_pass_gives_back_a_short_frame()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_warm( &w );

    uint8_t message[100];
    memset( message, 0x44, sizeof( message ) );
    check( rn_message_object_send( w.object, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );

    w.t->short_by = 1;
    w.t->started_ids.clear();
    w.t->aborted_ids.clear();
    const int finishes = w.t->finishes;
    rn_message_object_send_packets( w.object );
    r2ms_expect( !w.t->started_ids.empty(), "the write pass asked for no frame for a due reliable message" );
    r2ms_expect( w.t->aborted_ids == w.t->started_ids, "mutation gap hunt 1/1627: the write pass started %d short frames and gave back %d",
                 (int) w.t->started_ids.size(), (int) w.t->aborted_ids.size() );
    r2ms_expect( w.t->finishes == finishes, "the write pass finished a frame shorter than its plan" );

    w.t->short_by = 0;
    r2ms_step( &w );
    rn_message_object_send_packets( w.object );
    r2ms_expect( w.t->finishes > finishes, "the reliable message was not sent in the flush after the short frames" );
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_write_pass_gives_back_a_short_frame );

// =====================================================================================
// the whole-frame start: an abort, a destroy and an over-budget finish each give
// its frame back; a finish of no bytes logs the refusal line
// mutation gap: hunt 1/1097 (rn_message_connection_cancel_pending, the whole frame not aborted)
// mutation gap: hunt 1/1077 (destroy, the pending start not cancelled)
// mutation gap: hunt 1/1562 (the whole-frame finish's over-budget drop not aborted)
// mutation gap: hunt 1/1545, 1/1548, 1/1549, 1/1550 (the whole-frame finish's refusal line)

// a whole-frame start on slot 0: no frame above 1000 bytes, so the packed
// start finds none and the message takes a frame of its own
static uint8_t * r2ms_whole_frame_start( r2ms_world_t * w, int bytes )
{
    w->t->start_limit = 1000;
    uint8_t * p = rn_message_object_start( w->object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
    w->t->start_limit = 0;
    check( p != NULL );
    check( w->object->in_place == 1 );
    return p;
}

static void test_gaps_r2ms_whole_frame_start_gives_its_frame_back()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };

    // abort

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 0, false, false );
        r2ms_warm( &w );
        w.t->aborted_ids.clear();
        r2ms_whole_frame_start( &w, 900 );
        const uint64_t id = w.t->started_ids.back();
        rn_message_object_abort( w.object, 0 );
        r2ms_expect( w.t->aborted_ids.size() == 1 && w.t->aborted_ids[0] == id, "mutation gap hunt 1/1097: an aborted whole-frame start did not give its frame %llu back (%d aborted)",
                     (unsigned long long) id, (int) w.t->aborted_ids.size() );
        r2ms_destroy( &w );
    }

    // destroy with the start pending

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 0, false, false );
        r2ms_warm( &w );
        w.t->aborted_ids.clear();
        r2ms_whole_frame_start( &w, 900 );
        const uint64_t id = w.t->started_ids.back();
        r2ms_transport_t * t = w.t;
        rn_message_object_destroy( w.object );
        r2ms_expect( t->aborted_ids.size() == 1 && t->aborted_ids[0] == id, "mutation gap hunt 1/1077: destroy with a whole-frame start pending did not give its frame %llu back (%d aborted)",
                     (unsigned long long) id, (int) t->aborted_ids.size() );
        delete t;
    }

    // a finish of no bytes: refused, logged, the frame back

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 0, false, false );
        r2ms_warm( &w );
        r2ms_whole_frame_start( &w, 900 );
        r2ms_log_scope_t log;
        const uint32_t serial = rn_message_object_finish( w.object, 0, 0 );
        check( serial == 0 );
        r2ms_expect( r2ms_logged( "bytes_written 0 outside 1..reserved 900" ) == 1,
                     "mutation gap hunt 1/1545,1548,1549,1550: a whole-frame finish of 0 bytes logged %d refusal lines naming it (%d error lines)",
                     r2ms_logged( "bytes_written 0 outside 1..reserved 900" ), g_r2ms_log_count );
        r2ms_destroy( &w );
    }

    // over budget at finish: the bucket emptied between start and finish

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 256, false, false );
        r2ms_warm( &w );
        w.t->aborted_ids.clear();
        r2ms_whole_frame_start( &w, 900 );
        const uint64_t id = w.t->started_ids.back();
        const int finishes = w.t->finishes;
        const uint64_t over = r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        w.object->connections[0].budget.tokens = 0.0;
        check( rn_message_object_finish( w.object, 0, 900 ) != 0 );
        check( w.t->finishes == finishes );
        check( r2ms_counter( &w, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == over + 1 );
        r2ms_expect( w.t->aborted_ids.size() == 1 && w.t->aborted_ids[0] == id, "mutation gap hunt 1/1562: a whole-frame finish dropped over budget did not give its frame %llu back (%d aborted)",
                     (unsigned long long) id, (int) w.t->aborted_ids.size() );
        r2ms_destroy( &w );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2ms_whole_frame_start_gives_its_frame_back );

// =====================================================================================
// the packed finish at its edges: the serial wraps past 0 to 1, and a record
// the build cannot add refuses the finish and cancels the start
// mutation gap: hunt 1/1523 (the serial's wrap past 0)
// mutation gap: hunt 1/1520, 1/1521 (the packed finish's add refused)

static void test_gaps_r2ms_packed_finish_edges()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_warm( &w );

    uint8_t * p = rn_message_object_start( w.object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 );
    check( p != NULL && w.object->in_place == 2 );
    memset( p, 0x21, 100 );
    w.object->connections[0].serial = 0xFFFFFFFFu;
    const uint32_t serial = rn_message_object_finish( w.object, 0, 100 );
    r2ms_expect( serial == 1, "mutation gap hunt 1/1523: the packed finish after serial 0xFFFFFFFF returned serial %u, not 1 (0 is never a serial)", serial );

    // the build cannot take the record: its capacity cut to what it holds

    p = rn_message_object_start( w.object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 );
    check( p != NULL && w.object->in_place == 2 );
    memset( p, 0x22, 100 );
    rn_message_packet_build_t * build = &w.object->connections[0].unreliable_build;
    const int capacity = build->writer.capacity;
    build->writer.capacity = build->writer.bytes;
    uint32_t refused = 0;
    {
        r2ms_assert_scope_t asserts;
        refused = rn_message_object_finish( w.object, 0, 100 );
    }
    if ( build->writer.payload )
    {
        build->writer.capacity = capacity;
    }
    r2ms_expect( refused == 0, "mutation gap hunt 1/1521: a packed finish whose record the build could not add returned serial %u, not 0", refused );
    r2ms_expect( w.object->pending_connection == -1 && w.object->in_place == 0,
                 "mutation gap hunt 1/1520: a refused packed finish left its start pending (connection %d, in_place %d)", w.object->pending_connection, w.object->in_place );

    // the next start goes as before

    check( rn_message_object_send( w.object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, r2ms_junk_payload, (int) sizeof( r2ms_junk_payload ) ) != 0 );
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_packed_finish_edges );

// =====================================================================================
// an in-place start still pending at an update is aborted there and leaves a
// marker; the marker is cleared by the slot's reset and by an abort, never
// turned into a whole-frame start
// mutation gap: hunt 1/1128 (rn_message_connection_cancel_at_reset, in_place = 1)
// mutation gap: hunt 1/1589 (rn_message_object_abort, in_place = 1)

static void test_gaps_r2ms_in_place_marker_is_cleared()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };

    // the slot dropped at Observe in the update that aborted its start

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 0 );
        r2ms_warm( &w );
        r2ms_assert_scope_t asserts;
        check( rn_message_object_start( w.object, 1, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 ) != NULL );
        check( w.object->in_place == 2 );
        w.t->connection_id[1] = 0;
        r2ms_step( &w );
        r2ms_expect( w.object->in_place == 0, "mutation gap hunt 1/1128: the slot whose start an update aborted was dropped, and in_place is %d, not 0", w.object->in_place );
        check( rn_message_object_send( w.object, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, r2ms_junk_payload, (int) sizeof( r2ms_junk_payload ) ) != 0 );
        r2ms_destroy( &w );
    }

    // an abort after the update that aborted the start

    {
        r2ms_world_t w;
        r2ms_create( &w, 1, 3, bound, 0 );
        r2ms_warm( &w );
        r2ms_assert_scope_t asserts;
        check( rn_message_object_start( w.object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 ) != NULL );
        r2ms_step( &w );
        check( w.object->in_place == -1 );
        rn_message_object_abort( w.object, 0 );
        r2ms_expect( w.object->in_place == 0, "mutation gap hunt 1/1589: an abort after the update aborted the start left in_place %d, not 0", w.object->in_place );
        if ( w.object->in_place == 0 )
        {
            r2ms_destroy( &w );
        }
    }
}

REGISTER_UNIT_TEST( test_gaps_r2ms_in_place_marker_is_cleared );

// =====================================================================================
// the server's C API refuses a NULL data or bytes address: 0, one misuse, and
// nothing written through it
// mutation gap: hunt 1/2002, 1/2003 (rocketnet_message_server_send, data NULL)
// mutation gap: hunt 1/2008, 1/2009 (rocketnet_message_server_receive, data NULL)
// mutation gap: hunt 1/2011, 1/2012 (rocketnet_message_server_receive, bytes NULL)

static void test_gaps_r2ms_server_api_refuses_null_addresses()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_warm( &w );

    r2ms_assert_scope_t asserts;
    uint64_t before = rocketnet_api_misuse_count();
    const uint32_t sent = rocketnet_message_server_send( w.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, NULL, 8 );
    r2ms_expect( sent == 0, "mutation gap hunt 1/2002,2003: send with NULL data returned serial %u, not 0", sent );
    r2ms_expect( w.object->pending_connection == -1, "mutation gap hunt 1/2003: send with NULL data left a start pending on %d", w.object->pending_connection );
    r2ms_expect( rocketnet_api_misuse_count() == before + 1, "send with NULL data counted %llu misuse, not 1",
                 (unsigned long long) ( rocketnet_api_misuse_count() - before ) );

    int index = 0;
    int channel = 0;
    const uint8_t * data = NULL;
    int bytes = 0;
    before = rocketnet_api_misuse_count();
    const int got_null_data = rocketnet_message_server_receive( w.server, &index, &channel, NULL, &bytes );
    r2ms_expect( got_null_data == 0, "mutation gap hunt 1/2008,2009: receive with NULL data returned %d, not 0", got_null_data );
    const int got_null_bytes = rocketnet_message_server_receive( w.server, &index, &channel, &data, NULL );
    r2ms_expect( got_null_bytes == 0, "mutation gap hunt 1/2011,2012: receive with NULL bytes returned %d, not 0", got_null_bytes );
    r2ms_expect( rocketnet_api_misuse_count() == before + 2, "the two NULL receives counted %llu misuse, not 2",
                 (unsigned long long) ( rocketnet_api_misuse_count() - before ) );
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_server_api_refuses_null_addresses );

// =====================================================================================
// conditions report no progress age while nothing of the reliable work has
// been sent, and none once everything is acknowledged
// mutation gap: hunt 1/866 (rn_message_object_conditions, && to ||)

static void test_gaps_r2ms_conditions_age_needs_work_in_flight_and_a_first_send()
{
    rocketnet_quiet( 1 );
    const int bound[3] = { 1, 1, 1 };
    r2ms_world_t w;
    r2ms_create( &w, 1, 3, bound, 0 );
    r2ms_warm( &w );

    // every reliable message acknowledged: nothing in flight

    rocketnet_conditions_t conditions;
    check( rocketnet_message_server_conditions( w.server, 0, &conditions ) );
    r2ms_expect( conditions.reliable_in_flight == 0 && conditions.seconds_since_progress == -1.0,
                 "mutation gap hunt 1/866: with nothing in flight the conditions report %llu in flight and %g seconds since progress, not -1",
                 (unsigned long long) conditions.reliable_in_flight, conditions.seconds_since_progress );

    // a reliable message numbered, its frame refused by the transport

    w.t->fail_finish = 1;
    check( rn_message_object_send( w.object, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, r2ms_junk_payload, (int) sizeof( r2ms_junk_payload ) ) != 0 );
    rn_message_object_send_packets( w.object );
    r2ms_step( &w );
    check( rocketnet_message_server_conditions( w.server, 0, &conditions ) );
    const bool has_oldest = w.object->connections[0].ordered.ack.has_oldest;
    r2ms_expect( has_oldest || conditions.seconds_since_progress == -1.0,
                 "mutation gap hunt 1/866: %llu in flight with no first send, and the conditions report %g seconds since progress, not -1",
                 (unsigned long long) conditions.reliable_in_flight, conditions.seconds_since_progress );
    w.t->fail_finish = 0;
    r2ms_destroy( &w );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_conditions_age_needs_work_in_flight_and_a_first_send );

// =====================================================================================
// the object create refusals: each refused argument returns NULL and logs the
// line that names it
// mutation gap: hunt 1/936 (budget_valid, && to ||)
// mutation gap: hunt 1/947, 1/949, 1/950 (the refusal lines)
// mutation gap: hunt 1/957 (the refused allocation's line)

static void test_gaps_r2ms_object_create_refusals_name_their_argument()
{
    rocketnet_quiet( 1 );
    rocketnet_message_config_t config;
    r2ms_config( &config );
    r2ms_transport_t t;
    t.next_id = 1;
    rn_message_link_t link;
    r2ms_link( &link, &t, 1, false, true );

    r2ms_log_scope_t log;

    // a refused config

    rocketnet_message_config_t bad = config;
    bad.num_channels = 0;
    g_r2ms_log_count = 0;
    check( rn_message_object_create( NULL, &link, 1, 2, &bad, NULL, 0, 0, R2MS_T0 ) == NULL );
    r2ms_expect( r2ms_logged( "a refused message config or connection count" ) == 1, "mutation gap hunt 1/947: a refused config logged %d lines naming the config (%d error lines: %s)",
                 r2ms_logged( "a refused message config or connection count" ), g_r2ms_log_count, g_r2ms_log_count ? g_r2ms_log[0] : "" );

    // a valid budget at a time outside its range, and a budget past its maximum at a valid time

    g_r2ms_log_count = 0;
    rn_message_object_t * object = rn_message_object_create( NULL, &link, 1, 2, &config, NULL, 0, 64, nan( "" ) );
    r2ms_expect( object == NULL, "mutation gap hunt 1/936: a create at time NaN with a 64 kbps budget was not refused" );
    r2ms_expect( r2ms_logged( "send budget 64 kbps or time is outside its range" ) == 1, "mutation gap hunt 1/949,950: a create at time NaN logged %d budget lines (%d error lines: %s)",
                 r2ms_logged( "send budget 64 kbps or time is outside its range" ), g_r2ms_log_count, g_r2ms_log_count ? g_r2ms_log[0] : "" );
    if ( object )
    {
        rn_message_object_destroy( object );
    }

    object = rn_message_object_create( NULL, &link, 1, 2, &config, NULL, 0, (int32_t) 0x7fffffff, R2MS_T0 );
    r2ms_expect( object == NULL, "mutation gap hunt 1/936: a create with a budget past ROCKETNET_SEND_BUDGET_MAX_KBPS at a valid time was not refused" );
    if ( object )
    {
        rn_message_object_destroy( object );
    }

    // a caller block not 64-aligned

    const uint64_t required = rn_message_object_required_bytes( &config, 2, 1 );
    check( required > 0 );
    uint8_t * block = (uint8_t *) malloc( (size_t) required + 128 );
    check( block != NULL );
    uint8_t * unaligned = block + ( 64 - ( (uintptr_t) block % 64 ) ) + 8;
    g_r2ms_log_count = 0;
    check( rn_message_object_create( NULL, &link, 1, 2, &config, unaligned, required, 0, R2MS_T0 ) == NULL );
    r2ms_expect( r2ms_logged( "the memory block is short or not 64-aligned" ) == 1, "mutation gap hunt 1/949: an unaligned block logged %d memory lines (%d error lines: %s)",
                 r2ms_logged( "the memory block is short or not 64-aligned" ), g_r2ms_log_count, g_r2ms_log_count ? g_r2ms_log[0] : "" );
    free( block );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_object_create_refusals_name_their_argument );

static size_t g_r2ms_refuse_bytes = 0;

static void * r2ms_refusing_malloc( void * context, size_t bytes )
{
    (void) context;
    return bytes >= g_r2ms_refuse_bytes ? NULL : malloc( bytes );
}

static void r2ms_refusing_free( void * context, void * p )
{
    (void) context;
    free( p );
}

static void test_gaps_r2ms_object_create_logs_a_refused_allocation()
{
    rocketnet_quiet( 1 );
    rocketnet_message_config_t config;
    r2ms_config( &config );
    r2ms_transport_t t;
    t.next_id = 1;
    rn_message_link_t link;
    r2ms_link( &link, &t, 1, false, true );

    const uint64_t required = rn_message_object_required_bytes( &config, 2, 1 );
    check( required > 0 );
    g_r2ms_refuse_bytes = (size_t) required;

    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, r2ms_refusing_malloc, r2ms_refusing_free ) == ROCKETNET_STATUS_OK );
    {
        r2ms_log_scope_t log;
        check( rn_message_object_create( NULL, &link, 1, 2, &config, NULL, 0, 0, R2MS_T0 ) == NULL );
        r2ms_expect( r2ms_logged( "could not allocate" ) == 1, "mutation gap hunt 1/957: a refused allocation of the object's block logged %d lines naming it (%d error lines)",
                     r2ms_logged( "could not allocate" ), g_r2ms_log_count );
    }
    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_gaps_r2ms_object_create_logs_a_refused_allocation );

// =====================================================================================
// the server create: a per-client budget of 1 is 1, not the config's field, and
// a refused config logs its line
// mutation gap: hunt 1/1941 (kbps == 0 to kbps == 1)
// mutation gap: hunt 1/1958 (the server create's refusal line)

static void r2ms_server_transport_config( rocketnet_server_transport_config_t * server_config )
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    rocketnet_server_transport_default_config_current( server_config );
    server_config->protocol_id = 0x72326d73ULL;
    server_config->max_clients = 2;
    memcpy( server_config->backend_public_key, backend_public_key, sizeof( server_config->backend_public_key ) );
    memcpy( server_config->server_public_key, server_public_key, sizeof( server_config->server_public_key ) );
    memcpy( server_config->server_secret_key, server_secret_key, sizeof( server_config->server_secret_key ) );
    snprintf( server_config->transport, sizeof( server_config->transport ), "%s", "loopback" );
    server_config->loopback_hub = rocketnet_test_loopback_hub();
    memset( server_config->packet_filter, 0x5A, sizeof( server_config->packet_filter ) );
}

static void test_gaps_r2ms_server_create_budget_of_one_and_refusal_line()
{
    rocketnet_quiet( 1 );
    rocketnet_server_transport_config_t server_config;
    r2ms_server_transport_config( &server_config );
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.server_send_kbps = 256;

    int status = ROCKETNET_TEST_STATUS_UNSET;
    rocketnet_message_server_t * server = rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, NULL, 0, 1, &status, 0 );
    check( server != NULL && status == ROCKETNET_STATUS_OK );
    if ( server )
    {
        r2ms_expect( server->object.default_kbps == 1, "mutation gap hunt 1/1941: a server created with a per-client budget of 1 kbps has %d kbps (the config's field is 256)",
                     (int) server->object.default_kbps );
        rocketnet_message_server_destroy( server );
    }

    rocketnet_message_config_t bad = config;
    bad.num_channels = 0;
    {
        r2ms_log_scope_t log;
        status = ROCKETNET_TEST_STATUS_UNSET;
        check( rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &bad, NULL, 0, 0, &status, 0 ) == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
        r2ms_expect( r2ms_logged( "message server create refused: message config, client count or send budget" ) == 1,
                     "mutation gap hunt 1/1958: a server create refused for its message config logged %d lines naming it (%d error lines)",
                     r2ms_logged( "message server create refused: message config, client count or send budget" ), g_r2ms_log_count );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2ms_server_create_budget_of_one_and_refusal_line );
