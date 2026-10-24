/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Worst-case (all slots connected) per-slot call counts on the server
// update and send paths: Observe must not poll every slot's connection
// when no binding changed, and send_packets must not ask the transport
// for its remaining capacity once per slot per round. Driven through a
// fake link: no sockets, no real time (time is an argument).

#include "test_unit_registry.h"

#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_message.h"

#include <string.h>

#define check rocketnet_unit_check

// ---------------------------------------------------------------
// client slots binding epoch
// ---------------------------------------------------------------

static void test_client_slots_binding_epoch_changes_only_on_binding()
{
    rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 4 );
    check( slots != NULL );
    const uint64_t e0 = rocketnet_client_slots_binding_epoch( slots );
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0, sizeof( user ) );
    const int a = rocketnet_client_slots_assign( slots, 111, 0, user );
    check( a >= 0 );
    const uint64_t e1 = rocketnet_client_slots_binding_epoch( slots );
    check( e1 != e0 );
    rocketnet_client_slots_confirm( slots, a );
    check( rocketnet_client_slots_binding_epoch( slots ) == e1 );   // confirm is not a binding change
    check( rocketnet_client_slots_find( slots, 111 ) == a );
    check( rocketnet_client_slots_binding_epoch( slots ) == e1 );   // lookup is not a binding change
    rocketnet_client_slots_release( slots, a );
    check( rocketnet_client_slots_binding_epoch( slots ) != e1 );
    rocketnet_client_slots_destroy( slots );
}

// ---------------------------------------------------------------
// message object over a counting fake link
// ---------------------------------------------------------------

#define WC_SLOTS 8

struct wc_transport_t
{
    alignas( 16 ) uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 64];
    uint64_t connection[WC_SLOTS];
    uint64_t epoch;
    long connection_calls;
    long capacity_calls;
    int capacity;                       // frames the transport may still start
    int refuse_start;                   // start_packet gives no frame while set
    int finished_frames;
    int finished_slot[64];
};

static void wc_update( void *, double ) {}

static int wc_payloads( void *, const int ** client_index, uint8_t * const **, const int ** )
{
    *client_index = NULL;
    return 0;
}

static int wc_acked( void *, const int ** client_index, const uint64_t ** )
{
    *client_index = NULL;
    return 0;
}

static uint64_t wc_connection( void * transport, int slot )
{
    wc_transport_t * t = (wc_transport_t *) transport;
    t->connection_calls++;
    return t->connection[slot];
}

static uint64_t wc_binding_epoch( void * transport )
{
    return ((wc_transport_t *) transport)->epoch;
}

static int wc_capacity( void * transport )
{
    wc_transport_t * t = (wc_transport_t *) transport;
    t->capacity_calls++;
    return t->capacity;
}

static uint8_t * wc_start( void * transport, int slot, int, rocketnet_packet_context_t * context )
{
    wc_transport_t * t = (wc_transport_t *) transport;
    if ( t->refuse_start || t->capacity <= 0 )
        return NULL;
    memset( context, 0, sizeof( *context ) );
    context->frame = t->frame;
    context->payload_index = 16;
    context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    context->packet_id = 42;
    context->client_index = slot;
    return t->frame;
}

static int wc_finish( void * transport, rocketnet_packet_context_t * context, int bytes )
{
    wc_transport_t * t = (wc_transport_t *) transport;
    check( bytes > 0 );
    t->capacity--;
    t->finished_slot[t->finished_frames++] = context->client_index;
    return bytes + 16;
}

static void wc_abort( void *, rocketnet_packet_context_t * ) {}
static void wc_send( void * ) {}
static void wc_disconnect( void *, int ) {}

static rn_message_object_t * wc_server( wc_transport_t * t )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = t;
    link.update = wc_update;
    link.payloads = wc_payloads;
    link.acked = wc_acked;
    link.connection = wc_connection;
    link.binding_epoch = wc_binding_epoch;
    link.send_capacity_available = wc_capacity;
    link.start_packet = wc_start;
    link.finish_packet = wc_finish;
    link.abort_packet = wc_abort;
    link.send_packets = wc_send;
    link.disconnect = wc_disconnect;
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    return rn_message_object_create( NULL, &link, 1, WC_SLOTS, &config, NULL, 0, 0, 1000.0 );
}

// Observe polls every slot only when the binding epoch moved; a steady
// update makes no per-slot connection calls at all.
static void test_message_observe_skips_unchanged_bindings()
{
    wc_transport_t t;
    memset( &t, 0, sizeof( t ) );
    for ( int i = 0; i < WC_SLOTS; i++ )
        t.connection[i] = (uint64_t) ( i + 1 );
    t.epoch = 7;
    rn_message_object_t * object = wc_server( &t );
    check( object != NULL );

    check( rn_message_object_update( object, 1000.0 ) == ROCKETNET_UPDATE_OK );
    check( t.connection_calls == WC_SLOTS );                    // first Observe reads every slot
    for ( int i = 0; i < WC_SLOTS; i++ )
        check( rn_message_object_connected( object, i ) == 1 );

    t.connection_calls = 0;
    check( rn_message_object_update( object, 1000.01 ) == ROCKETNET_UPDATE_OK );
    check( t.connection_calls == 0 );                           // steady state: no polling
    check( rn_message_object_update( object, 1000.02 ) == ROCKETNET_UPDATE_OK );
    check( t.connection_calls == 0 );

    // a binding change is observed on the next update
    t.connection[3] = 0;
    t.epoch++;
    check( rn_message_object_update( object, 1000.03 ) == ROCKETNET_UPDATE_OK );
    check( t.connection_calls == WC_SLOTS );
    check( rn_message_object_connected( object, 3 ) == 0 );
    check( rn_message_object_connected( object, 4 ) == 1 );

    rn_message_object_destroy( object );
}

// send_packets asks for capacity once, then counts its own frames down;
// a spent capacity stops the round and the rotation resumes fairly.
static void test_message_send_capacity_queried_once_per_send()
{
    wc_transport_t t;
    memset( &t, 0, sizeof( t ) );
    for ( int i = 0; i < WC_SLOTS; i++ )
        t.connection[i] = (uint64_t) ( i + 1 );
    t.epoch = 1;
    t.capacity = 1000;
    rn_message_object_t * object = wc_server( &t );
    check( object != NULL );
    check( rn_message_object_update( object, 1000.0 ) == ROCKETNET_UPDATE_OK );

    // the connect CONFIG each slot announced at Observe goes out first;
    // drain it, then an idle send starts nothing
    t.capacity = 1000;
    rn_message_object_send_packets( object );
    t.finished_frames = 0;
    t.capacity_calls = 0;
    t.capacity = 1000;
    rn_message_object_send_packets( object );                   // idle: nothing queued
    check( t.capacity_calls <= 1 );
    check( t.finished_frames == 0 );

    // queue one unreliable message per slot (no frame at start: queued)
    t.refuse_start = 1;
    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for ( int i = 0; i < 4; i++ )
        check( rn_message_object_send( object, i, 0, ROCKETNET_RELIABILITY_UNRELIABLE, data, (int) sizeof( data ) ) != 0 );
    t.refuse_start = 0;

    t.capacity = 2;                                             // only two frames this send
    t.capacity_calls = 0;
    rn_message_object_send_packets( object );
    check( t.capacity_calls <= 1 );
    check( t.finished_frames == 2 );

    t.capacity = 2;                                             // the rest go next send, in rotation
    rn_message_object_send_packets( object );
    check( t.finished_frames == 4 );
    check( t.finished_slot[0] != t.finished_slot[1] );
    check( t.finished_slot[2] != t.finished_slot[3] );

    rn_message_object_destroy( object );
}

REGISTER_UNIT_TEST( test_client_slots_binding_epoch_changes_only_on_binding );
REGISTER_UNIT_TEST( test_message_observe_skips_unchanged_bindings );
REGISTER_UNIT_TEST( test_message_send_capacity_queried_once_per_send );
