/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's send pass costs the same whatever one client queues: a flush
// visits each connection slot once, plus one visit per frame written
// (rn_message_object_send_packets, proofs/tla/RNSendRotation.tla Visit).
// One busy client with K frames due must not make every flush walk all
// max_clients slots K + 1 times, which would let one client's condition cost
// every other client's update (standard/MESSAGES.md m1.8).
//
// The case runs a server message object of 1000 connections over a fake
// transport: every slot holds a connection from the first update, a frame
// start always succeeds, every frame finished in a flush is acknowledged at
// the next update, and nothing is received. The visits are counted through
// the ROCKETNET_DEV send-pass seam (rn_send_pass_cost_read). Time is an
// argument from 1000.0; nothing sleeps and nothing opens a socket.

#if ROCKETNET_DEV

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"

#define check rocketnet_unit_check

#define SP_CONNECTIONS      1000
#define SP_PAYLOAD_INDEX    32
#define SP_MESSAGE_BYTES    1000
#define SP_T0               1000.0
#define SP_DT               ( 1.0 / 60.0 )

struct sp_transport_t
{
    uint64_t next_id;
    int frames;                             // frames finished this flush
    int aborts;                             // frames aborted this flush
    int capacity;                           // frame starts allowed per flush (send_capacity_available)
    int started;                            // frames started this flush
    std::vector<int> acked_slot;            // the last flush's frames, acknowledged at this update
    std::vector<uint64_t> acked;
    std::vector<int> pending_slot;          // this flush's frames
    std::vector<uint64_t> pending;
    alignas( 16 ) uint8_t frame[SP_PAYLOAD_INDEX + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
};

static void sp_update( void * transport, double time )
{
    sp_transport_t * t = (sp_transport_t *) transport;
    (void) time;
    t->acked.swap( t->pending );
    t->acked_slot.swap( t->pending_slot );
    t->pending.clear();
    t->pending_slot.clear();
}

static int sp_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    (void) transport;
    *client_index = NULL;
    *payload_data = NULL;
    *payload_bytes = NULL;
    return 0;
}

static int sp_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    sp_transport_t * t = (sp_transport_t *) transport;
    *client_index = t->acked_slot.data();
    *packet_ids = t->acked.data();
    return (int) t->acked.size();
}

static int sp_max_payload_bytes( void * transport )
{
    (void) transport;
    return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

static uint8_t * sp_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    sp_transport_t * t = (sp_transport_t *) transport;
    memset( context, 0, sizeof( *context ) );
    check( request_bytes >= 1 && request_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    context->frame = t->frame;
    context->payload_index = SP_PAYLOAD_INDEX;
    context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    context->packet_id = ++t->next_id;
    t->started++;
    context->client_index = client_index;
    return t->frame;
}

static int sp_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    sp_transport_t * t = (sp_transport_t *) transport;
    t->frames++;
    t->pending.push_back( context->packet_id );
    t->pending_slot.push_back( context->client_index );
    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_bytes );
}

static void sp_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    sp_transport_t * t = (sp_transport_t *) transport;
    (void) context;
    t->aborts++;
}

static void sp_send_packets( void * transport )
{
    sp_transport_t * t = (sp_transport_t *) transport;
    t->started = 0;                                 // the ring's count resets when the transport sends
}

static int sp_send_capacity_available( void * transport )
{
    sp_transport_t * t = (sp_transport_t *) transport;
    return t->capacity - t->started;
}

static uint64_t sp_connection( void * transport, int client_index )
{
    (void) transport;
    return (uint64_t) client_index + 1;
}

static void sp_disconnect( void * transport, int client_index )
{
    (void) transport;
    (void) client_index;
}

static int sp_state( void * transport )
{
    (void) transport;
    return ROCKETNET_SERVER_TRANSPORT_RUNNING;
}

struct sp_cost_t
{
    uint64_t visits;
    uint64_t frames;
    uint64_t aborted;
};

// K messages of SP_MESSAGE_BYTES for connection 0 every update, one frame
// each, the other connections idle: the visits and frames of each measured
// flush, summed
static sp_cost_t sp_run( int K, int flushes )
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.reliable_messages_in_flight = 128;
    config.packets_in_flight = 128;
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = 4096;
    config.send_message_buffer_bytes = 131072;
    config.send_queue_messages = 128;
    config.packets_per_update = 64;
    config.unreliable_messages_per_update = 16;
    config.max_message_receipts = 16;
    config.sequenced_messages_held = 0;
    config.server_send_kbps = 0;                    // uncapped: the budget is not what this measures
    check( rocketnet_message_validate_config( &config ) );

    sp_transport_t * t = new sp_transport_t();
    t->next_id = 5000;

    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = t;
    link.update = sp_update;
    link.payloads = sp_payloads;
    link.acked = sp_acked;
    link.max_payload_bytes = sp_max_payload_bytes;
    link.start_packet = sp_start_packet;
    link.finish_packet = sp_finish_packet;
    link.abort_packet = sp_abort_packet;
    link.send_packets = sp_send_packets;
    link.connection = sp_connection;
    link.disconnect = sp_disconnect;
    link.state = sp_state;

    rocketnet_message_server_t * server = (rocketnet_message_server_t *) rn_message_object_create( NULL, &link, 1, SP_CONNECTIONS, &config, NULL, 0, 0, SP_T0 );
    check( server != NULL );

    sp_cost_t cost = {};
    if ( !server )
    {
        delete t;
        return cost;
    }

    // two warm flushes: every connection's CONFIG goes out, then is acknowledged

    double time = SP_T0;
    uint8_t message[SP_MESSAGE_BYTES];
    memset( message, 0x5A, sizeof( message ) );
    for ( int f = 0; f < flushes + 2; f++ )
    {
        check( rocketnet_message_server_update( server, time ) == ROCKETNET_UPDATE_OK );
        for ( int k = 0; f >= 2 && k < K; k++ )
        {
            check( rocketnet_message_server_send( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
        }

        t->frames = 0;
        t->aborts = 0;
        rn_send_pass_cost_reset();
        rocketnet_message_server_send_packets( server );
        const rn_send_pass_cost_t pass = rn_send_pass_cost_read();

        if ( f >= 2 )
        {
            cost.visits += pass.slot_visits;
            cost.frames += (uint64_t) t->frames;
            cost.aborted += pass.frames_aborted;
            check( t->aborts == 0 );

            // the bound, flush by flush: every slot once, then one visit per frame written

            check( pass.slot_visits <= (uint64_t) SP_CONNECTIONS + (uint64_t) t->frames );
        }

        time += SP_DT;
    }

    rocketnet_message_server_destroy( server );
    delete t;
    return cost;
}

static void test_server_send_pass_visits_bounded_by_slots_plus_frames()
{
    const int flushes = 10;
    const int Ks[] = { 0, 1, 8, 32 };
    for ( int i = 0; i < (int) ( sizeof( Ks ) / sizeof( Ks[0] ) ); i++ )
    {
        const int K = Ks[i];
        const sp_cost_t cost = sp_run( K, flushes );
        printf( "    [K=%d] %llu slot visits per flush, %llu frames written per flush, %d connections\n", K,
                (unsigned long long) ( cost.visits / flushes ), (unsigned long long) ( cost.frames / flushes ), SP_CONNECTIONS );

        // the busy connection wrote its K frames every flush, and no frame was aborted

        check( cost.frames == (uint64_t) K * (uint64_t) flushes );
        check( cost.aborted == 0 );
        check( cost.visits <= (uint64_t) flushes * SP_CONNECTIONS + cost.frames );
    }
}

REGISTER_UNIT_TEST( test_server_send_pass_visits_bounded_by_slots_plus_frames );

// ---- the cursor at a capacity cut in a later round ------------------------------------
//
// A cut in a later round leaves the cursor where a walk over every connection
// would hold it: the position the cut connection had in the first round,
// before its visit (sdk/rocketnet_message_server.cpp, the later-round
// capacity test; proofs/tla/RNServerSendFair.tla CursorAsFullWalk). Four
// connections, the cursor at 0. Flush A, capacity 3: connections 0 and 2 have
// two frames due each, 1 and 3 nothing. Round one serves 0 and 2 (1 and 3 are
// visited and write nothing); round two serves 0's second frame, and the
// capacity is spent before 2: the cursor is 2's first-round position, 2. The
// last connection visited would leave it at 1. Flush B, capacity 1:
// connection 1 has a frame due now, and 2 still has its second. 2 was cut and
// 1 was offered its turn in flush A, so 2 is served.

static void test_server_send_cursor_at_later_round_cut()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.reliable_messages_in_flight = 128;
    config.packets_in_flight = 128;
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = 4096;
    config.send_message_buffer_bytes = 131072;
    config.send_queue_messages = 128;
    config.packets_per_update = 64;
    config.unreliable_messages_per_update = 16;
    config.max_message_receipts = 16;
    config.sequenced_messages_held = 0;
    config.server_send_kbps = 0;
    check( rocketnet_message_validate_config( &config ) );

    sp_transport_t * t = new sp_transport_t();
    t->next_id = 5000;
    t->capacity = 1000;

    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = t;
    link.update = sp_update;
    link.payloads = sp_payloads;
    link.acked = sp_acked;
    link.max_payload_bytes = sp_max_payload_bytes;
    link.start_packet = sp_start_packet;
    link.finish_packet = sp_finish_packet;
    link.abort_packet = sp_abort_packet;
    link.send_packets = sp_send_packets;
    link.send_capacity_available = sp_send_capacity_available;
    link.connection = sp_connection;
    link.disconnect = sp_disconnect;
    link.state = sp_state;

    rn_message_object_t * object = rn_message_object_create( NULL, &link, 1, 4, &config, NULL, 0, 0, SP_T0 );
    check( object != NULL );
    if ( !object )
    {
        delete t;
        return;
    }
    rocketnet_message_server_t * server = (rocketnet_message_server_t *) object;

    // two warm flushes: every connection's CONFIG goes out, then is acknowledged

    double time = SP_T0;
    for ( int f = 0; f < 2; f++ )
    {
        check( rocketnet_message_server_update( server, time ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( server );
        time += SP_DT;
    }

    uint8_t message[SP_MESSAGE_BYTES];
    memset( message, 0x5A, sizeof( message ) );

    // flush A: capacity 3, two frames due on 0 and on 2

    check( rocketnet_message_server_update( server, time ) == ROCKETNET_UPDATE_OK );
    for ( int k = 0; k < 2; k++ )
    {
        check( rocketnet_message_server_send( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
        check( rocketnet_message_server_send( server, 2, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
    }
    object->send_next_connection = 0;
    t->capacity = 3;
    rocketnet_message_server_send_packets( server );
    check( t->pending_slot.size() == 3 );
    check( t->pending_slot.size() == 3 && t->pending_slot[0] == 0 && t->pending_slot[1] == 2 && t->pending_slot[2] == 0 );
    printf( "    flush A served %d %d %d, cursor %d\n", t->pending_slot[0], t->pending_slot[1], t->pending_slot[2], object->send_next_connection );
    check( object->send_next_connection == 2 );
    time += SP_DT;

    // flush B: capacity 1, a frame due on 1 now; 2's second frame waits since flush A

    check( rocketnet_message_server_update( server, time ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_server_send( server, 1, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
    t->capacity = 1;
    rocketnet_message_server_send_packets( server );
    check( t->pending_slot.size() == 1 );
    printf( "    flush B served %d\n", t->pending_slot.empty() ? -1 : t->pending_slot[0] );
    check( t->pending_slot.size() == 1 && t->pending_slot[0] == 2 );
    check( t->aborts == 0 );

    rocketnet_message_server_destroy( server );
    delete t;
}

REGISTER_UNIT_TEST( test_server_send_cursor_at_later_round_cut );

#endif // #if ROCKETNET_DEV
