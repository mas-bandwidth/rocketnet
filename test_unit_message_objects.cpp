/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client and server message objects and their C API (docs/CHANNELS.md 10
// to 12; tla/RNSlots.tla, tla/RNConnectionMemory.tla). The objects run over
// the in-memory transport pair below, bound through the object's transport
// seam (rn_message_link_t) exactly where the real transports bind: it hands
// out frames at start_packet with the transport's packet id, takes them at
// finish_packet, opens each packet once on the far side into a payload batch
// valid until the next update, and reports the opened ids back to the sender
// as its acked packet ids at its next update (R1: the only ack). Slots,
// connection ids and loss are the harness's to script. Time is only ever an
// argument: nothing sleeps, nothing opens a socket.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_counters.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_message_object.h"
#include "sdk/rocketnet_send_budget.h"
#include "sdk/rocketnet_wire.h"

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// the in-memory transport pair
// ---------------------------------------------------------------------------

#define MO_MAX_SLOTS        4
#define MO_PAYLOAD_INDEX    16
#define MO_MAX_FRAMES       64

struct mo_packet_t
{
    uint64_t id;
    int slot;
    int bytes;
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PAYLOAD_BYTES];
};

struct mo_net_t;

struct mo_endpoint_t
{
    mo_net_t * net;
    bool is_server;
    int slot;                                       // a client endpoint's slot on the server

    // send: frames handed out by start_packet, finished into `outbound`
    alignas( 16 ) uint8_t frames[MO_MAX_FRAMES][MO_PAYLOAD_INDEX + ROCKETNET_MAX_PAYLOAD_BYTES];
    bool frame_used[MO_MAX_FRAMES];
    uint64_t frame_id[MO_MAX_FRAMES];
    int frame_slot[MO_MAX_FRAMES];

    // receive: the packets on the wire to this endpoint, and the update's batch
    std::vector<mo_packet_t> inbound;
    std::vector<mo_packet_t> batch;
    std::vector<uint8_t *> batch_data;
    std::vector<int> batch_bytes;
    std::vector<int> batch_slot;

    // acks: ids this endpoint opened, reported to the sender at its next update
    std::vector<uint64_t> acks_inbox;               // ids the peer opened of this endpoint's sends
    std::vector<int> acks_inbox_slot;
    std::vector<uint64_t> acked;                    // this update's acked batch
    std::vector<int> acked_slot;

    // what this endpoint finished, for the pointer-identity checks
    std::vector<mo_packet_t> sent_log;

    bool connected;                                 // the client's transport state
    int disconnects;                                // times the object disconnected it
    int finishes;
    double time;
};

struct mo_net_t
{
    mo_endpoint_t * server;
    mo_endpoint_t * clients[MO_MAX_SLOTS];
    uint64_t slot_connection[MO_MAX_SLOTS];         // the server transport's slot owners
    uint64_t next_connection;
    uint64_t next_packet_id;
    bool drop_to_server;
    bool drop_to_client;
    int drop_next_to_server;                        // drop this many packets to the server, then deliver
    int fail_finish;                                // the next this many finish_packet calls fail (return 0, nothing sent)
    bool hold_acks;                                 // opened ids are not reported back while set
    bool keep_log;
};

static mo_net_t * mo_net_new()
{
    mo_net_t * net = new mo_net_t();
    memset( net->slot_connection, 0, sizeof( net->slot_connection ) );
    net->server = new mo_endpoint_t();
    net->server->net = net;
    net->server->is_server = true;
    net->server->slot = -1;
    for ( int i = 0; i < MO_MAX_SLOTS; i++ )
        net->clients[i] = NULL;
    net->next_connection = 100;
    net->next_packet_id = 1000;
    net->drop_to_server = false;
    net->drop_to_client = false;
    net->drop_next_to_server = 0;
    net->fail_finish = 0;
    net->hold_acks = false;
    net->keep_log = true;
    return net;
}

static mo_endpoint_t * mo_net_client( mo_net_t * net, int slot )
{
    mo_endpoint_t * client = new mo_endpoint_t();
    client->net = net;
    client->is_server = false;
    client->slot = slot;
    client->connected = false;
    net->clients[slot] = client;
    return client;
}

// the transport's handshake completes: the server's slot takes a new
// connection id and the client is CONNECTED
static void mo_net_connect( mo_net_t * net, int slot )
{
    net->slot_connection[slot] = net->next_connection++;
    if ( net->clients[slot] )
        net->clients[slot]->connected = true;
}

static void mo_net_drop_slot( mo_net_t * net, int slot )
{
    net->slot_connection[slot] = 0;
    if ( net->clients[slot] )
        net->clients[slot]->connected = false;
}

static void mo_net_free( mo_net_t * net )
{
    for ( int i = 0; i < MO_MAX_SLOTS; i++ )
        delete net->clients[i];
    delete net->server;
    delete net;
}

// --- the link functions ---

static void mo_update( void * transport, double time )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    mo_net_t * net = e->net;
    e->time = time;

    // the transport's acked packet ids: what the peer opened since the last update
    e->acked.clear();
    e->acked_slot.clear();
    if ( !net->hold_acks )
    {
        e->acked.swap( e->acks_inbox );
        e->acked_slot.swap( e->acks_inbox_slot );
    }

    // the batch: the last one is gone, the wire's packets are opened once each
    e->batch.clear();
    e->batch_data.clear();
    e->batch_bytes.clear();
    e->batch_slot.clear();
    e->batch.reserve( e->inbound.size() );
    for ( size_t i = 0; i < e->inbound.size(); i++ )
    {
        const mo_packet_t & p = e->inbound[i];
        if ( e->is_server && net->slot_connection[p.slot] == 0 )
            continue;                                           // no connection in the slot: the transport refuses it
        if ( !e->is_server && !e->connected )
            continue;
        e->batch.push_back( p );
        // the opened id goes back to the sender as an ack at its next update
        mo_endpoint_t * sender = e->is_server ? net->clients[p.slot] : net->server;
        if ( sender )
        {
            sender->acks_inbox.push_back( p.id );
            sender->acks_inbox_slot.push_back( p.slot );
        }
    }
    e->inbound.clear();
    for ( size_t i = 0; i < e->batch.size(); i++ )
    {
        e->batch_data.push_back( e->batch[i].data );
        e->batch_bytes.push_back( e->batch[i].bytes );
        e->batch_slot.push_back( e->batch[i].slot );
    }
}

static int mo_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    *client_index = e->is_server ? e->batch_slot.data() : NULL;
    *payload_data = e->batch_data.data();
    *payload_bytes = e->batch_bytes.data();
    return (int) e->batch.size();
}

static int mo_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    *client_index = e->is_server ? e->acked_slot.data() : NULL;
    *packet_ids = e->acked.data();
    return (int) e->acked.size();
}

static uint8_t * mo_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    memset( context, 0, sizeof( *context ) );
    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return NULL;
    if ( e->is_server ? e->net->slot_connection[client_index] == 0 : !e->connected )
        return NULL;
    for ( int f = 0; f < MO_MAX_FRAMES; f++ )
    {
        if ( e->frame_used[f] )
            continue;
        e->frame_used[f] = true;
        e->frame_id[f] = e->net->next_packet_id++;
        e->frame_slot[f] = e->is_server ? client_index : e->slot;
        context->frame = e->frames[f];
        context->payload_index = MO_PAYLOAD_INDEX;
        context->payload_capacity = ROCKETNET_MAX_PAYLOAD_BYTES;
        context->packet_id = e->frame_id[f];
        context->client_index = client_index;
        return e->frames[f];
    }
    return NULL;
}

static int mo_frame_of( mo_endpoint_t * e, const rocketnet_packet_context_t * context )
{
    for ( int f = 0; f < MO_MAX_FRAMES; f++ )
        if ( e->frame_used[f] && context->frame == e->frames[f] )
            return f;
    return -1;
}

static int mo_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    const int f = mo_frame_of( e, context );
    check( f >= 0 );
    check( payload_bytes >= 1 && payload_bytes <= ROCKETNET_MAX_PAYLOAD_BYTES );
    e->frame_used[f] = false;
    if ( e->net->fail_finish > 0 )
    {
        e->net->fail_finish--;                                  // the transport refused the packet: nothing goes on the wire
        return 0;
    }
    e->finishes++;

    mo_packet_t packet;
    packet.id = e->frame_id[f];
    packet.slot = e->frame_slot[f];
    packet.bytes = payload_bytes;
    memcpy( packet.data, e->frames[f] + MO_PAYLOAD_INDEX, (size_t) payload_bytes );         // the wire: the transport's own business
    if ( e->net->keep_log )
        e->sent_log.push_back( packet );

    mo_net_t * net = e->net;
    if ( e->is_server )
    {
        mo_endpoint_t * to = net->clients[packet.slot];
        if ( to && !net->drop_to_client )
            to->inbound.push_back( packet );
    }
    else
    {
        if ( net->drop_next_to_server > 0 )
            net->drop_next_to_server--;
        else if ( !net->drop_to_server )
            net->server->inbound.push_back( packet );
    }

    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_bytes );
}

static void mo_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    const int f = mo_frame_of( e, context );
    if ( f >= 0 )
        e->frame_used[f] = false;
}

static void mo_send_packets( void * transport )
{
    (void) transport;
}

static uint64_t mo_connection( void * transport, int client_index )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    if ( e->is_server )
        return e->net->slot_connection[client_index];
    return e->connected ? 1 : 0;
}

static void mo_disconnect( void * transport, int client_index )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    e->disconnects++;
    if ( e->is_server )
        mo_net_drop_slot( e->net, client_index );
    else
        e->connected = false;
}

static int mo_state( void * transport )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    if ( e->is_server )
        return ROCKETNET_SERVER_TRANSPORT_RUNNING;
    return e->connected ? ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED : ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
}

static rn_message_link_t mo_link( mo_endpoint_t * e )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = e;
    link.update = mo_update;
    link.payloads = mo_payloads;
    link.acked = mo_acked;
    link.start_packet = mo_start_packet;
    link.finish_packet = mo_finish_packet;
    link.abort_packet = mo_abort_packet;
    link.send_packets = mo_send_packets;
    link.connection = mo_connection;
    link.disconnect = mo_disconnect;
    link.state = mo_state;
    link.destroy = NULL;                                        // the harness owns the pair
    return link;
}

// ---------------------------------------------------------------------------
// objects on the pair
// ---------------------------------------------------------------------------

static void mo_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->num_channels = 4;
    config->window_messages = 64;
    config->receive_ring_bytes = 65536;
    config->max_message_bytes = 16384;
    config->send_buffer_bytes = 65536;
    config->send_queue_messages = 64;
    config->packets_in_flight = 64;
    config->max_packets_per_update = 8;
    config->unreliable_deliveries = 64;
    config->rto_initial_ms = 100;
    config->rto_min_ms = 20;
    config->rto_max_ms = 400;
    config->reliable_timeout_ms = 120000;
}

// the clock every case starts at: never 0.0, so a time field left at zero
// shows up as an immediate timeout instead of passing by accident

#define MO_T0 1000.0

struct mo_pair_t
{
    mo_net_t * net;
    rocketnet_message_server_t * server;
    rocketnet_message_client_t * clients[MO_MAX_SLOTS];
    int num_clients;
    double time;
};

static rocketnet_message_server_t * mo_server_create( mo_net_t * net, const rocketnet_message_config_t * config, int slots, uint32_t kbps, void * memory = NULL, uint64_t memory_bytes = 0 )
{
    rn_message_link_t link = mo_link( net->server );
    return (rocketnet_message_server_t *) rn_message_object_create( NULL, &link, 1, slots, config, memory, memory_bytes, kbps, MO_T0 );
}

static rocketnet_message_client_t * mo_client_create( mo_net_t * net, int slot, const rocketnet_message_config_t * config, uint32_t kbps, void * memory = NULL, uint64_t memory_bytes = 0 )
{
    mo_endpoint_t * e = mo_net_client( net, slot );
    rn_message_link_t link = mo_link( e );
    return (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, config, memory, memory_bytes, kbps, MO_T0 );
}

static void mo_pair_setup( mo_pair_t * pair, int num_clients, const rocketnet_message_config_t * config = NULL,
                           uint32_t client_kbps = ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, uint32_t server_kbps = ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS )
{
    rocketnet_message_config_t defaults;
    if ( !config )
    {
        mo_config( &defaults );
        config = &defaults;
    }
    memset( pair, 0, sizeof( *pair ) );
    pair->net = mo_net_new();
    pair->num_clients = num_clients;
    pair->server = mo_server_create( pair->net, config, MO_MAX_SLOTS, server_kbps );
    check( pair->server );
    for ( int i = 0; i < num_clients; i++ )
    {
        pair->clients[i] = mo_client_create( pair->net, i, config, client_kbps );
        check( pair->clients[i] );
        mo_net_connect( pair->net, i );
    }
    pair->time = MO_T0;
}

static void mo_pair_teardown( mo_pair_t * pair )
{
    for ( int i = 0; i < pair->num_clients; i++ )
        rocketnet_message_client_destroy( pair->clients[i] );
    rocketnet_message_server_destroy( pair->server );
    mo_net_free( pair->net );
}

static rn_message_connection_t * mo_server_slot( mo_pair_t * pair, int slot )
{
    return rn_message_object_connection( &pair->server->object, slot );
}

static rn_message_connection_t * mo_client_connection( rocketnet_message_client_t * client )
{
    return rn_message_object_connection( &client->object, 0 );
}

// every message the application received this update, released before the next

struct mo_received_t
{
    int slot;
    int channel;
    std::vector<uint8_t> bytes;
};

static void mo_drain_server( mo_pair_t * pair, std::vector<mo_received_t> * out )
{
    for ( int slot = 0; slot < MO_MAX_SLOTS; slot++ )
        for ( int channel = 0; channel < 4; channel++ )
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            while ( rocketnet_message_server_receive( pair->server, slot, channel, &data, &bytes ) )
            {
                mo_received_t r;
                r.slot = slot;
                r.channel = channel;
                r.bytes.assign( data, data + bytes );
                if ( out )
                    out->push_back( r );
                rocketnet_message_server_release( pair->server, slot, data );
            }
        }
}

static void mo_drain_client( rocketnet_message_client_t * client, std::vector<mo_received_t> * out )
{
    for ( int channel = 0; channel < 4; channel++ )
    {
        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_message_client_receive( client, channel, &data, &bytes ) )
        {
            mo_received_t r;
            r.slot = 0;
            r.channel = channel;
            r.bytes.assign( data, data + bytes );
            if ( out )
                out->push_back( r );
            rocketnet_message_client_release( client, data );
        }
    }
}

// one step of the whole pair: every object updates, the applications drain and
// release, every object sends

static void mo_step( mo_pair_t * pair, std::vector<mo_received_t> * server_got = NULL, std::vector<mo_received_t> * client_got = NULL, double dt = 0.01 )
{
    pair->time += dt;
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_MESSAGE_OK );
    for ( int i = 0; i < pair->num_clients; i++ )
        check( rocketnet_message_client_update( pair->clients[i], pair->time ) == ROCKETNET_MESSAGE_OK );
    mo_drain_server( pair, server_got );
    for ( int i = 0; i < pair->num_clients; i++ )
        mo_drain_client( pair->clients[i], client_got );
    rocketnet_message_server_send_packets( pair->server );
    for ( int i = 0; i < pair->num_clients; i++ )
        rocketnet_message_client_send_packets( pair->clients[i] );
}

static uint8_t mo_pattern( uint32_t id, uint32_t i )
{
    return (uint8_t) ( id * 131 + i * 7 + ( i >> 8 ) * 13 + 5 );
}

static void mo_fill( uint8_t * data, uint32_t id, uint32_t bytes )
{
    for ( uint32_t i = 0; i < bytes; i++ )
        data[i] = mo_pattern( id, i );
    if ( bytes >= 4 )
        memcpy( data, &id, 4 );
}

static bool mo_verify( const std::vector<uint8_t> & data, uint32_t * out_id )
{
    if ( data.size() < 4 )
        return false;
    uint32_t id;
    memcpy( &id, data.data(), 4 );
    for ( uint32_t i = 4; i < data.size(); i++ )
        if ( data[i] != mo_pattern( id, i ) )
            return false;
    *out_id = id;
    return true;
}

static uint64_t mo_counter( const rocketnet_counters_t * counters, int which )
{
    return rocketnet_counter_value( counters, which );
}

static void message_send_refused_counts_bad_arguments()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    const uint64_t before = mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    check( rocketnet_message_server_start( pair.server, 0, -1, ROCKETNET_RELIABLE_ORDERED, 1 ) == NULL );
    check( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 0 ) == NULL );
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == before + 2 );

    uint8_t * valid = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 1 );
    check( valid != NULL );
    valid[0] = 42;
    check( rocketnet_message_server_finish( pair.server, 0, 1 ) != 0 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_MESSAGE_ERROR_NONE );
    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R3: the pointer start returns is the region record the transport frame's
// bytes are copied from: THE COPIES (a), the one copy on the send path
// ---------------------------------------------------------------------------

static const uint8_t * mo_find( const uint8_t * haystack, int haystack_bytes, const uint8_t * needle, int needle_bytes )
{
    for ( int i = 0; i + needle_bytes <= haystack_bytes; i++ )
        if ( memcmp( haystack + i, needle, (size_t) needle_bytes ) == 0 )
            return haystack + i;
    return NULL;
}

static void start_finish_pointer_reaches_the_frame()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    rn_message_connection_t * connection = mo_client_connection( client );

    const int reliabilities[2] = { ROCKETNET_RELIABLE_ORDERED, ROCKETNET_UNRELIABLE };
    for ( int r = 0; r < 2; r++ )
    {
        const int bytes = 300;
        uint8_t * p = rocketnet_message_client_start( client, 1, reliabilities[r], bytes );
        check( p != NULL );
        check( rn_region_contains( connection->region, p, (uint64_t) bytes ) );        // in place, in the connection's region
        mo_fill( p, 7000 + (uint32_t) r, (uint32_t) bytes );
        check( rocketnet_message_client_finish( client, bytes ) != 0 );

        uint8_t expected[300];
        memcpy( expected, p, sizeof( expected ) );

        mo_endpoint_t * e = pair.net->clients[0];
        e->sent_log.clear();
        rocketnet_message_client_send_packets( client );
        check( e->sent_log.size() == 1 );

        // the frame carries the record's bytes, copied once from where start pointed
        const mo_packet_t & packet = e->sent_log[0];
        const uint8_t * in_frame = mo_find( packet.data, packet.bytes, p, bytes );
        check( in_frame != NULL );
        check( memcmp( in_frame, expected, sizeof( expected ) ) == 0 );
        check( in_frame != p );                                  // the one named copy: record to frame

        if ( reliabilities[r] == ROCKETNET_RELIABLE_ORDERED )
        {
            // the record outlives the frame until a transport ack: the bytes
            // are still at p, for a resend
            check( memcmp( p, expected, sizeof( expected ) ) == 0 );
            check( connection->region->descriptors_free < connection->region->queue );
        }

        std::vector<mo_received_t> got;
        mo_step( &pair, &got );
        check( got.size() == 1 );
        check( got[0].bytes.size() == (size_t) bytes );
        check( memcmp( got[0].bytes.data(), expected, sizeof( expected ) ) == 0 );
    }

    mo_step( &pair );
    mo_step( &pair );
    check( connection->region->descriptors_free == connection->region->queue );      // acked: the records' room is back

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R3: a message delivered in the update it arrives is a view into the
// transport's batch -- the record itself, never copied
// ---------------------------------------------------------------------------

static bool mo_in_batch( const mo_endpoint_t * e, const uint8_t * data, int bytes )
{
    for ( size_t i = 0; i < e->batch.size(); i++ )
    {
        const uint8_t * base = e->batch[i].data;
        if ( data >= base && data + bytes <= base + e->batch[i].bytes )
            return true;
    }
    return false;
}

static void receive_view_is_the_record()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    const int reliabilities[2] = { ROCKETNET_RELIABLE_ORDERED, ROCKETNET_UNRELIABLE };
    for ( int r = 0; r < 2; r++ )
    {
        uint8_t message[200];
        mo_fill( message, 42 + (uint32_t) r, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 2, reliabilities[r], message, (int) sizeof( message ) ) != 0 );
        rocketnet_message_client_send_packets( pair.clients[0] );

        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );

        const int channel = reliabilities[r] == ROCKETNET_UNRELIABLE ? 0 : 2;       // unreliable carries no channel: channel 0
        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, 0, channel, &data, &bytes ) == 1 );
        check( bytes == (int) sizeof( message ) );
        check( memcmp( data, message, sizeof( message ) ) == 0 );
        check( mo_in_batch( pair.net->server, data, bytes ) );                      // pointer identity: inside the transport's payload
        const uint8_t * again = NULL;
        int again_bytes = 0;
        check( rocketnet_message_server_receive( pair.server, 0, channel, &again, &again_bytes ) == 0 );
        check( again == NULL );

        rocketnet_message_server_release( pair.server, 0, data );
        mo_step( &pair );
    }

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// RNSlots Release and RejectUnreleasedUpdate: release returns the room;
// update with a held view asserts in a debug build and is the named error in
// release; memory never grows from held views
// ---------------------------------------------------------------------------

static int mo_asserts = 0;

static void mo_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
    mo_asserts++;
}

static void release_returns_room()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
    rn_region_t * region = slot->region;

    // release then update is fine, and the receive descriptor is free again
    {
        uint8_t message[64];
        mo_fill( message, 1, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 1, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        rocketnet_message_client_send_packets( pair.clients[0] );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, 0, 1, &data, &bytes ) == 1 );
        check( slot->held == 1 );
        uint32_t live = 0;
        for ( uint32_t i = 0; i < region->window; i++ )
            live += region->receive_descriptors[i].live;
        check( live == 1 );
        rocketnet_message_server_release( pair.server, 0, data );
        check( slot->held == 0 );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        live = 0;
        for ( uint32_t i = 0; i < region->window; i++ )
            live += region->receive_descriptors[i].live;
        check( live == 0 );
    }

    // a release of a pointer that is not a held view is a counted misuse
    {
        uint8_t stray[4] = { 0, 0, 0, 0 };
        const uint64_t before = rocketnet_api_misuse_count();
        rocketnet_message_server_release( pair.server, 0, stray );
        check( rocketnet_api_misuse_count() == before + 1 );
    }

    // update with a held view: the named error, the assert in a debug build,
    // and the views are taken back so nothing grows
    void (*saved_assert)( const char *, const char *, const char *, int ) = rocketnet_assert_function_pointer;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;

    // the server's CONFIG (CHANNELS.md 10.3) is acked first, so the room counted below is the application's alone
    mo_step( &pair );
    mo_step( &pair );
    const uint32_t descriptors = region->descriptors_free;
    for ( int round = 0; round < 3 * (int) region->window; round++ )
    {
        uint8_t message[64];
        mo_fill( message, 100 + (uint32_t) round, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 1, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        rocketnet_message_client_send_packets( pair.clients[0] );
        pair.time += 0.01;
        const int result = rocketnet_message_server_update( pair.server, pair.time );
        check( result == ( round == 0 ? ROCKETNET_MESSAGE_OK : ROCKETNET_ERROR_MESSAGE_NOT_RELEASED ) );
        rocketnet_message_server_send_packets( pair.server );
        check( rocketnet_message_client_update( pair.clients[0], pair.time ) == ROCKETNET_MESSAGE_OK );
        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, 0, 1, &data, &bytes ) == 1 );    // held, never released
        check( slot->held == 1 );
        uint32_t live = 0;
        for ( uint32_t i = 0; i < region->window; i++ )
            live += region->receive_descriptors[i].live;
        check( live <= 1 );                                                         // memory never grows from held views
        check( region->descriptors_free == descriptors );
        check( !rn_region_exhausted( region ) );
    }

#if ROCKETNET_ASSERTS
    check( mo_asserts == 3 * (int) region->window - 1 );
#else
    check( mo_asserts == 0 );
#endif
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED ) == (uint64_t) ( 3 * region->window - 1 ) );
    rocketnet_assert_function( saved_assert );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// both shipped types end to end over the pair, both directions, with loss:
// every reliable ordered message delivered once, in order, byte exact; the
// unreliable ones at most once
// ---------------------------------------------------------------------------

static void mo_end_to_end_up()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 2, NULL, 20000, 20000 );
    mo_step( &pair );

    const uint32_t sizes[] = { 1, 17, 200, 1200, 1411, 3000, 9000, 16384 };
    const int num_sizes = (int) ( sizeof( sizes ) / sizeof( sizes[0] ) );

    uint32_t next_id = 1;
    std::vector<uint32_t> sent_ordered[MO_MAX_SLOTS][4];         // client slot -> channel -> ids, to the server
    std::vector<uint32_t> got_ordered[MO_MAX_SLOTS][4];
    uint32_t unreliable_sent = 0, unreliable_got = 0;

    for ( int round = 0; round < 120; round++ )
    {
        // loss: every fifth round drops everything one way
        pair.net->drop_to_server = ( round % 5 ) == 2;
        pair.net->drop_to_client = ( round % 7 ) == 3;

        if ( round < 60 )
        {
            for ( int c = 0; c < 2; c++ )
            {
                const int channel = ( round + c ) % 4;
                const uint32_t bytes = sizes[( round + c ) % num_sizes];
                const uint32_t id = next_id++;

                // the client writes in place
                uint8_t * p = rocketnet_message_client_start( pair.clients[c], channel, ROCKETNET_RELIABLE_ORDERED, (int) bytes );
                check( p );
                if ( bytes >= 4 )
                    mo_fill( p, id, bytes );
                else
                    memset( p, 0xAB, bytes );
                check( rocketnet_message_client_finish( pair.clients[c], (int) bytes ) != 0 );
                if ( bytes >= 4 )
                    sent_ordered[c][channel].push_back( id );

                // the server sends down
                const uint32_t down_id = next_id++;
                uint8_t * q = rocketnet_message_server_start( pair.server, c, channel, ROCKETNET_RELIABLE_ORDERED, 600 );
                check( q );
                mo_fill( q, down_id, 600 );
                check( rocketnet_message_server_finish( pair.server, c, 600 ) != 0 );

                // and an unreliable one
                uint8_t u[100];
                mo_fill( u, 900000 + next_id++, sizeof( u ) );
                check( rocketnet_message_client_send( pair.clients[c], channel, ROCKETNET_UNRELIABLE, u, (int) sizeof( u ) ) != 0 );
                unreliable_sent++;
            }
        }

        std::vector<mo_received_t> server_got, client_got;
        mo_step( &pair, &server_got, &client_got, 0.02 );

        for ( size_t i = 0; i < server_got.size(); i++ )
        {
            const mo_received_t & r = server_got[i];
            if ( r.bytes.size() < 4 )
                continue;
            uint32_t id = 0;
            check( mo_verify( r.bytes, &id ) );
            if ( id >= 900000 )
                unreliable_got++;
            else
                got_ordered[r.slot][r.channel].push_back( id );
        }
        // the server's sends down arrive byte exact (their order per client is
        // mo_ordered_down_in_order's)
        for ( size_t i = 0; i < client_got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( client_got[i].bytes, &id ) );
            (void) id;
        }
    }

    // every reliable ordered message, per client and channel: once, in order
    for ( int c = 0; c < 2; c++ )
        for ( int channel = 0; channel < 4; channel++ )
            check( got_ordered[c][channel] == sent_ordered[c][channel] );

    check( unreliable_got <= unreliable_sent );
    check( unreliable_got > 0 );

    // every slot's and client's reliable records acked and their room back
    for ( int c = 0; c < 2; c++ )
    {
        check( mo_server_slot( &pair, c )->region->descriptors_free == mo_server_slot( &pair, c )->region->queue );
        check( mo_client_connection( pair.clients[c] )->region->descriptors_free == mo_client_connection( pair.clients[c] )->region->queue );
        check( rocketnet_message_server_client_connected( pair.server, c ) == 1 );
    }

    mo_pair_teardown( &pair );
}

// the server-to-client direction, per client, in order

static void mo_ordered_down_in_order()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 2, NULL, 20000, 20000 );
    mo_step( &pair );

    uint32_t next_id = 1;
    std::vector<uint32_t> sent[2][4], got[2][4];
    for ( int round = 0; round < 80; round++ )
    {
        pair.net->drop_to_client = ( round % 4 ) == 1;
        if ( round < 40 )
            for ( int c = 0; c < 2; c++ )
            {
                const int channel = round % 4;
                const uint32_t bytes = round % 3 == 0 ? 2500u : 300u;
                const uint32_t id = next_id++;
                uint8_t * q = rocketnet_message_server_start( pair.server, c, channel, ROCKETNET_RELIABLE_ORDERED, (int) bytes );
                check( q );
                mo_fill( q, id, bytes );
                check( rocketnet_message_server_finish( pair.server, c, (int) bytes ) != 0 );
                sent[c][channel].push_back( id );
            }
        pair.time += 0.02;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        mo_drain_server( &pair, NULL );
        for ( int c = 0; c < 2; c++ )
        {
            check( rocketnet_message_client_update( pair.clients[c], pair.time ) == ROCKETNET_MESSAGE_OK );
            std::vector<mo_received_t> g;
            mo_drain_client( pair.clients[c], &g );
            for ( size_t i = 0; i < g.size(); i++ )
            {
                uint32_t id = 0;
                check( mo_verify( g[i].bytes, &id ) );
                got[c][g[i].channel].push_back( id );
            }
        }
        rocketnet_message_server_send_packets( pair.server );
        for ( int c = 0; c < 2; c++ )
            rocketnet_message_client_send_packets( pair.clients[c] );
    }
    for ( int c = 0; c < 2; c++ )
        for ( int channel = 0; channel < 4; channel++ )
            check( got[c][channel] == sent[c][channel] );

    mo_pair_teardown( &pair );
}

static void ordered_and_unreliable_end_to_end_in_memory()
{
    mo_end_to_end_up();
    mo_ordered_down_in_order();
}

// ---------------------------------------------------------------------------
// R5: a server whose per-client memory is exhausted kicks that client with
// MESSAGE_MEMORY_EXHAUSTED; the other clients carry on untouched
// ---------------------------------------------------------------------------

static void mo_small_config( rocketnet_message_config_t * config )
{
    mo_config( config );
    config->max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config->send_buffer_bytes = 12288;
    config->send_queue_messages = 16;
}

static void server_kicks_exhausted_client_others_unaffected()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    mo_pair_t pair;
    mo_pair_setup( &pair, 2, &config );
    mo_step( &pair );

    // the server queues to slot 0 without sending until its region has no room
    int started = 0;
    for ( ;; )
    {
        uint8_t * q = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 100 );
        if ( !q )
            break;
        mo_fill( q, (uint32_t) started, 100 );
        check( rocketnet_message_server_finish( pair.server, 0, 100 ) != 0 );
        started++;
        check( started <= 1000 );
    }
    check( started > 0 );

    // kicked at once: the transport disconnected the slot, with the reason
    check( pair.net->server->disconnects == 1 );
    check( pair.net->slot_connection[0] == 0 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
    check( rocketnet_message_server_client_connected( pair.server, 0 ) == 0 );
    check( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 100 ) == NULL );
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 1 );

    // slot 1 is unaffected: a full exchange both ways
    std::vector<uint32_t> sent, got_up;
    uint32_t id = 50000;
    for ( int round = 0; round < 30; round++ )
    {
        if ( round < 10 )
        {
            uint8_t message[300];
            mo_fill( message, id, sizeof( message ) );
            check( rocketnet_message_client_send( pair.clients[1], 1, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
            check( rocketnet_message_server_send( pair.server, 1, 1, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
            sent.push_back( id++ );
        }
        std::vector<mo_received_t> server_got;
        mo_step( &pair, &server_got );
        for ( size_t i = 0; i < server_got.size(); i++ )
        {
            check( server_got[i].slot == 1 );
            uint32_t got_id = 0;
            check( mo_verify( server_got[i].bytes, &got_id ) );
            got_up.push_back( got_id );
        }
    }
    check( got_up == sent );
    check( rocketnet_message_server_client_connected( pair.server, 1 ) == 1 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 1 ) == ROCKETNET_MESSAGE_ERROR_NONE );
    check( pair.net->server->disconnects == 1 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R5: a client whose own memory is exhausted disconnects itself with the same
// reason; nothing else follows from exhaustion
// ---------------------------------------------------------------------------

static void client_disconnects_itself_on_exhaustion()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    int started = 0;
    for ( ;; )
    {
        uint8_t * p = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABLE_ORDERED, 900 );
        if ( !p )
            break;
        mo_fill( p, (uint32_t) started, 900 );
        check( rocketnet_message_client_finish( client, 900 ) != 0 );
        started++;
        check( started <= 1000 );
    }
    check( started > 0 );

    check( pair.net->clients[0]->disconnects == 1 );
    check( !pair.net->clients[0]->connected );
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
    check( rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( mo_counter( rocketnet_message_client_counters( client ), ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 1 );

    // nothing more: no start, no send, no partial packet
    check( rocketnet_message_client_start( client, 0, ROCKETNET_UNRELIABLE, 10 ) == NULL );
    const int finishes = pair.net->clients[0]->finishes;
    pair.time += 0.01;
    check( rocketnet_message_client_update( client, pair.time ) == ROCKETNET_MESSAGE_OK );
    rocketnet_message_client_send_packets( client );
    check( pair.net->clients[0]->finishes == finishes );
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );

    // the region was reset at that update: nothing of the connection is left
    rn_message_connection_t * connection = mo_client_connection( client );
    check( !rn_region_exhausted( connection->region ) );
    check( connection->region->descriptors_free == connection->region->queue );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// RNSlots ResetFree, RNConnectionMemory Reuse: a reused slot starts exactly as
// a fresh one
// ---------------------------------------------------------------------------

static void slot_reuse_starts_clean()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config );
    mo_step( &pair );

    rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
    const uint64_t carves = slot->region->carves;

    // traffic, a runtime budget, then exhaustion
    for ( int i = 0; i < 5; i++ )
    {
        uint8_t message[100];
        mo_fill( message, (uint32_t) i, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 0, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        mo_step( &pair );
    }
    mo_step( &pair );
    check( slot->region->channel_receive[0].ordered_read == 5 );
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, 500 ) == 1 );
    check( slot->budget.kbps == 500 );
    while ( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 100 ) )
        check( rocketnet_message_server_finish( pair.server, 0, 100 ) != 0 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );

    // the next update resets the region before the transport can reuse the slot
    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
    check( slot->state == RN_MESSAGE_CONNECTION_FREE );
    check( slot->region->carves == carves + 1 );
    check( !rn_region_exhausted( slot->region ) );

    // a new client takes the slot
    delete pair.net->clients[0];
    pair.net->clients[0] = NULL;
    rocketnet_message_client_destroy( pair.clients[0] );
    pair.clients[0] = mo_client_create( pair.net, 0, &config, ROCKETNET_DEFAULT_CLIENT_SEND_KBPS );
    check( pair.clients[0] );
    mo_net_connect( pair.net, 0 );
    mo_step( &pair );

    check( slot->state == RN_MESSAGE_CONNECTION_CONNECTED );
    check( slot->connection_id == pair.net->slot_connection[0] );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_MESSAGE_ERROR_NONE );
    check( slot->region->descriptors_free == slot->region->queue - 1 );          // the new connection's CONFIG, in flight (10.3)
    check( slot->region->channel_receive[0].ordered_read == 0 );
    check( slot->region->channel_send[0].ordered_write == 0 );
    check( slot->budget.kbps == ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS );
    check( slot->held == 0 && slot->unreliable_count == 0 && slot->serial == 0 );
    mo_step( &pair );
    mo_step( &pair );
    check( slot->region->descriptors_free == slot->region->queue );              // CONFIG acked: every descriptor is back (10.3)
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        check( rn_send_store_free_chunks( &slot->region->store, c ) == rn_send_store_total_chunks( &slot->region->store, c ) );

    // and the new connection's first message is ordering 0, delivered
    uint8_t message[100];
    mo_fill( message, 77, sizeof( message ) );
    check( rocketnet_message_client_send( pair.clients[0], 0, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
    std::vector<mo_received_t> got;
    mo_step( &pair );
    mo_step( &pair, &got );
    check( got.size() == 1 );
    uint32_t id = 0;
    check( mo_verify( got[0].bytes, &id ) && id == 77 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R1: only the transport's acked packet ids mark a message delivered
// ---------------------------------------------------------------------------

static void acks_only_from_transport_acked_packets()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rn_region_t * region = mo_client_connection( pair.clients[0] )->region;
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( pair.clients[0] );

    // the server receives every packet but the transport reports no ack
    pair.net->hold_acks = true;
    uint8_t message[500];
    mo_fill( message, 9, sizeof( message ) );
    check( rocketnet_message_client_send( pair.clients[0], 3, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );

    int delivered = 0;
    for ( int i = 0; i < 60; i++ )
    {
        std::vector<mo_received_t> got;
        mo_step( &pair, &got, NULL, 0.05 );
        delivered += (int) got.size();
    }
    check( delivered == 1 );                                                       // once, however often it arrives
    check( region->descriptors_free == region->queue - 2 );                        // still in flight: no ack, no release (this message and the connection's CONFIG, 10.3)
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );          // resent on its RTO
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) > 0 );

    // a delivered payload is not an ack, nor is any other event: the record
    // goes only when the transport reports a packet id that carried it
    pair.net->hold_acks = false;
    mo_step( &pair, NULL, NULL, 0.05 );
    mo_step( &pair, NULL, NULL, 0.05 );
    check( region->descriptors_free == region->queue );

    // an acked id this connection never sent changes nothing
    mo_endpoint_t * e = pair.net->clients[0];
    check( rocketnet_message_client_send( pair.clients[0], 3, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
    pair.net->drop_to_server = true;
    mo_step( &pair );
    check( region->descriptors_free == region->queue - 1 );
    e->acks_inbox.push_back( 0xDEADBEEFull );
    mo_step( &pair );
    check( region->descriptors_free == region->queue - 1 );
    pair.net->drop_to_server = false;
    for ( int i = 0; i < 20; i++ )
        mo_step( &pair, NULL, NULL, 0.05 );
    check( region->descriptors_free == region->queue );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R6: the send budget, in kbps, set at create and at runtime
// ---------------------------------------------------------------------------

static void budget_set_at_create_and_runtime()
{
    // at create: the given rate, and 0 is the default
    {
        mo_net_t * net = mo_net_new();
        rocketnet_message_config_t config;
        mo_config( &config );
        rocketnet_message_client_t * a = mo_client_create( net, 0, &config, 2500 );
        check( a && mo_client_connection( a )->budget.kbps == 2500 );
        rocketnet_message_server_t * s = mo_server_create( net, &config, 2, 40000 );
        check( s && rn_message_object_connection( &s->object, 1 )->budget.kbps == 40000 );
        rocketnet_message_client_destroy( a );
        rocketnet_message_server_destroy( s );
        // an invalid rate is refused at create
        rn_message_link_t link = mo_link( net->server );
        check( rn_message_object_create( NULL, &link, 1, 2, &config, NULL, 0, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1, MO_T0 ) == NULL );
        mo_net_free( net );
    }

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, NULL, ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS );
    check( mo_client_connection( pair.clients[0] )->budget.kbps == ROCKETNET_DEFAULT_CLIENT_SEND_KBPS );
    check( mo_server_slot( &pair, 0 )->budget.kbps == ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS );
    mo_step( &pair );

    // at runtime: a low rate. Reliable messages wait for budget and are all
    // delivered; unreliable ones over budget are dropped and counted
    rocketnet_message_client_t * client = pair.clients[0];
    check( rocketnet_message_client_set_send_kbps( client, 40 ) == 1 );            // 5000 bytes a second
    check( mo_client_connection( client )->budget.kbps == 40 );
    check( rocketnet_message_client_set_send_kbps( client, 0 ) == 0 );             // refused, unchanged
    check( mo_client_connection( client )->budget.kbps == 40 );

    std::vector<uint32_t> sent, got;
    const int rounds = 100;
    for ( int round = 0; round < rounds; round++ )
    {
        if ( round < 20 )
        {
            uint8_t message[1000];
            mo_fill( message, (uint32_t) round + 1, sizeof( message ) );
            check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
            sent.push_back( (uint32_t) round + 1 );
            uint8_t u[1000];
            mo_fill( u, 900000 + (uint32_t) round, sizeof( u ) );
            check( rocketnet_message_client_send( client, 1, ROCKETNET_UNRELIABLE, u, (int) sizeof( u ) ) );
        }
        std::vector<mo_received_t> server_got;
        mo_step( &pair, &server_got, NULL, 0.05 );
        for ( size_t i = 0; i < server_got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( server_got[i].bytes, &id ) );
            if ( id < 900000 )
                got.push_back( id );
        }
    }
    check( got == sent );                                                          // never dropped for budget
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) > 0 );

    // the wire stayed under the rate: 40 kbps over the run, plus the bucket's depth
    uint64_t wire = 0;
    const mo_endpoint_t * e = pair.net->clients[0];
    for ( size_t i = 0; i < e->sent_log.size(); i++ )
        wire += (uint64_t) rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, e->sent_log[i].bytes );
    const double seconds = rounds * 0.05 + 0.01;
    check( (double) wire <= 5000.0 * seconds + rocketnet_send_budget_depth_bytes( 1000 ) + rocketnet_send_budget_depth_bytes( 40 ) );

    // raised at runtime: the rate takes effect from now on
    check( rocketnet_message_client_set_send_kbps( client, 10000 ) == 1 );
    const uint64_t dropped = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
    for ( int round = 0; round < 10; round++ )
    {
        uint8_t u[1000];
        mo_fill( u, 950000 + (uint32_t) round, sizeof( u ) );
        check( rocketnet_message_client_send( client, 1, ROCKETNET_UNRELIABLE, u, (int) sizeof( u ) ) );
        mo_step( &pair, NULL, NULL, 0.05 );
    }
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == dropped );

    // and the server's per-client rate, at runtime
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, 777 ) == 1 );
    check( mo_server_slot( &pair, 0 )->budget.kbps == 777 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R2: a counting allocator sees create's one allocation and then nothing.
// Registered as message_objects_no_allocation_after_create: four other unit
// files already register a test named no_allocation_after_create, and the
// registry runs a name's first registration only.
// ---------------------------------------------------------------------------

static uint64_t mo_mallocs = 0;
static uint64_t mo_frees = 0;

static void * mo_malloc( void * context, size_t bytes )
{
    (void) context;
    mo_mallocs++;
    return malloc( bytes );
}

static void mo_free( void * context, void * p )
{
    (void) context;
    mo_frees++;
    free( p );
}

static void message_objects_no_allocation_after_create()
{
    rocketnet_term();
    check( rocketnet_allocator( mo_malloc, mo_free ) );
    check( rocketnet_init() );

    {
        rocketnet_message_config_t config;
        mo_config( &config );

        // NULL memory: one allocation per object at create
        const uint64_t before_create = mo_mallocs;
        mo_pair_t pair;
        mo_pair_setup( &pair, 2, &config );
        check( mo_mallocs == before_create + 3 );

        // caller memory: none at all
        mo_net_t * net = mo_net_new();
        const uint64_t bytes = rocketnet_message_client_required_memory( &config );
        check( bytes > 0 );
        const size_t alignment = ROCKETNET_ALLOCATION_ALIGNMENT;
        check( bytes <= SIZE_MAX - ( alignment - 1 ) );
        void * allocation = malloc( (size_t) bytes + alignment - 1 );
        check( allocation != NULL );
        void * block = (void *) ( ( (uintptr_t) allocation + alignment - 1 ) & ~(uintptr_t) ( alignment - 1 ) );
        const uint64_t before_block = mo_mallocs;
        rocketnet_message_client_t * in_block = mo_client_create( net, 0, &config, ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, block, bytes );
        check( in_block != NULL );
        check( (void *) in_block == block );
        check( mo_mallocs == before_block );
        check( mo_client_create( net, 1, &config, ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, block, bytes - 64 ) == NULL );        // short: refused
        rocketnet_message_client_destroy( in_block );

        // the hot path: sends both types, writes, resends under loss, acks,
        // receives, releases, ring holds and drains, a kick and a slot reset
        const uint64_t calls = mo_mallocs + mo_frees;
        mo_step( &pair );
        for ( int round = 0; round < 80; round++ )
        {
            pair.net->drop_to_server = ( round % 3 ) == 1;
            pair.net->drop_to_client = ( round % 4 ) == 2;
            if ( round < 40 )
                for ( int c = 0; c < 2; c++ )
                {
                    uint8_t * p = rocketnet_message_client_start( pair.clients[c], round % 4, ROCKETNET_RELIABLE_ORDERED, round % 2 ? 3000 : 200 );
                    check( p );
                    mo_fill( p, (uint32_t) round, round % 2 ? 3000 : 200 );
                    check( rocketnet_message_client_finish( pair.clients[c], round % 2 ? 3000 : 200 ) );
                    uint8_t u[64];
                    mo_fill( u, (uint32_t) round, sizeof( u ) );
                    check( rocketnet_message_client_send( pair.clients[c], 0, ROCKETNET_UNRELIABLE, u, (int) sizeof( u ) ) );
                    check( rocketnet_message_server_send( pair.server, c, 1, ROCKETNET_RELIABLE_ORDERED, u, (int) sizeof( u ) ) );
                    uint8_t * a = rocketnet_message_client_start( pair.clients[c], 2, ROCKETNET_RELIABLE_ORDERED, 50 );
                    check( a );
                    rocketnet_message_client_abort( pair.clients[c] );
                }
            mo_step( &pair, NULL, NULL, 0.03 );
        }
        // a kick and a reset, still allocation free
        while ( rocketnet_message_server_start( pair.server, 1, 0, ROCKETNET_RELIABLE_ORDERED, 1000 ) )
            check( rocketnet_message_server_finish( pair.server, 1, 1000 ) );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        check( mo_mallocs + mo_frees == calls );

        mo_pair_teardown( &pair );
        mo_net_free( net );
        free( allocation );
    }

    rocketnet_term();
    check( rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function ) );
    check( rocketnet_init() );
}

// ---------------------------------------------------------------------------
// tla/RNSlots.tla's five reversed witnesses, replayed against the objects:
// each mutation's counterexample, step by step, and the property it broke
// holding here
// ---------------------------------------------------------------------------

static void mo_send_up( mo_pair_t * pair, int c, int channel, uint32_t id, int bytes = 100 )
{
    uint8_t message[2000];
    mo_fill( message, id, (uint32_t) bytes );
    check( rocketnet_message_client_send( pair->clients[c], channel, ROCKETNET_RELIABLE_ORDERED, message, bytes ) );
    rocketnet_message_client_send_packets( pair->clients[c] );
}

static void rnslots_witness_replays()
{
    rocketnet_message_config_t config;
    mo_config( &config );

    // Apply by slot (NoCrossConnectionApply): connection A sends a record,
    // then A disconnects and B takes the slot inside one transport update.
    // Observe binds the slot to B; A's payload in that batch is discarded,
    // never applied to B.
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config );
        mo_step( &pair );
        const uint64_t a = pair.net->slot_connection[0];
        mo_send_up( &pair, 0, 0, 1 );
        mo_net_connect( pair.net, 0 );                                              // B, before the server's update
        check( pair.net->slot_connection[0] != a );
        pair.net->clients[0]->connected = true;
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, 0, 0, &data, &bytes ) == 0 );
        check( mo_server_slot( &pair, 0 )->connection_id == pair.net->slot_connection[0] );
        check( mo_server_slot( &pair, 0 )->region->channel_receive[0].ordered_read == 0 );
        check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) >= 1 );
        mo_pair_teardown( &pair );
    }

    // Omit reset (ReusedSlotStartsClean): A's record delivered, received and
    // released; A disconnects; the free slot holds none of A's receive state
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config );
        mo_step( &pair );
        mo_send_up( &pair, 0, 0, 1 );
        std::vector<mo_received_t> got;
        mo_step( &pair, &got );
        check( got.size() == 1 );
        rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
        check( slot->region->channel_receive[0].ordered_read == 1 );
        mo_net_drop_slot( pair.net, 0 );
        mo_step( &pair );
        check( slot->state == RN_MESSAGE_CONNECTION_FREE );
        check( slot->connection_id == 0 );
        check( slot->region->channel_receive[0].ordered_read == 0 );
        uint32_t live = 0;
        for ( uint32_t i = 0; i < slot->region->window; i++ )
            live += slot->region->receive_descriptors[i].live;
        check( live == 0 );
        mo_pair_teardown( &pair );
    }

    // Carry on after exhaustion (ExhaustionOnlyLeadsToDisconnect): records
    // arrive and are handed to the application in one update, then the
    // connection's region finds no room. The connection does not go on: the
    // next step is the disconnect with the named reason, nothing more is
    // taken from the region, the views already handed out are still released
    // before the next update, and the region is reset before the slot is reused.
    {
        rocketnet_message_config_t small;
        mo_small_config( &small );
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &small );
        mo_step( &pair );
        mo_send_up( &pair, 0, 0, 1 );
        mo_send_up( &pair, 0, 0, 2 );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        const uint8_t * first = NULL;
        const uint8_t * second = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, 0, 0, &first, &bytes ) == 1 );
        check( rocketnet_message_server_receive( pair.server, 0, 0, &second, &bytes ) == 1 );

        rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
        int started = 0;
        while ( rocketnet_message_server_start( pair.server, 0, 1, ROCKETNET_RELIABLE_ORDERED, 100 ) )
        {
            check( rocketnet_message_server_finish( pair.server, 0, 100 ) );
            started++;
        }
        check( started > 0 );
        check( slot->state == RN_MESSAGE_CONNECTION_KICKED );
        check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
        check( pair.net->slot_connection[0] == 0 );
        check( rocketnet_message_server_client_connected( pair.server, 0 ) == 0 );
        check( rocketnet_message_server_start( pair.server, 0, 1, ROCKETNET_UNRELIABLE, 10 ) == NULL );
        const int finishes = pair.net->server->finishes;
        rocketnet_message_server_send_packets( pair.server );
        check( pair.net->server->finishes == finishes );                            // nothing sent from an exhausted region

        rocketnet_message_server_release( pair.server, 0, first );
        rocketnet_message_server_release( pair.server, 0, second );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        check( slot->state == RN_MESSAGE_CONNECTION_FREE );
        check( !rn_region_exhausted( slot->region ) );
        check( slot->region->descriptors_free == slot->region->queue );
        check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
        mo_pair_teardown( &pair );
    }

    // Ignore release (EveryReliableDelivered): with release before every
    // update and the next message sent only after the last is released, the
    // region's room comes back each time, so many more messages than the
    // region holds are all delivered and nothing is exhausted
    {
        rocketnet_message_config_t small;
        mo_small_config( &small );
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &small );
        mo_step( &pair );
        rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
        const uint32_t total = 4 * slot->region->window;
        uint32_t delivered = 0;
        for ( uint32_t id = 1; id <= total; id++ )
        {
            mo_send_up( &pair, 0, id % 4, id, 1000 );
            std::vector<mo_received_t> got;
            for ( int i = 0; i < 4 && got.empty(); i++ )
                mo_step( &pair, &got );
            check( got.size() == 1 );
            uint32_t got_id = 0;
            check( mo_verify( got[0].bytes, &got_id ) && got_id == id );
            delivered++;
        }
        check( delivered == total );
        check( slot->state == RN_MESSAGE_CONNECTION_CONNECTED );
        check( !rn_region_exhausted( slot->region ) );
        check( !rn_region_exhausted( mo_client_connection( pair.clients[0] )->region ) );
        mo_pair_teardown( &pair );
    }

    // Update with held views (UpdateRequiresReleasedViews): a record delivered
    // and held; the next update is refused by name, never run as if released
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config );
        mo_step( &pair );
        mo_send_up( &pair, 0, 0, 1 );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, 0, 0, &data, &bytes ) == 1 );
        void (*saved_assert)( const char *, const char *, const char *, int ) = rocketnet_assert_function_pointer;
        rocketnet_assert_function( mo_assert_hook );
        mo_asserts = 0;
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_ERROR_MESSAGE_NOT_RELEASED );
#if ROCKETNET_ASSERTS
        check( mo_asserts == 1 );
#else
        check( mo_asserts == 0 );
#endif
        rocketnet_assert_function( saved_assert );
        check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED ) == 1 );
        // the following update, with nothing held, is fine
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_MESSAGE_OK );
        mo_pair_teardown( &pair );
    }
}

// The client's transport clock starts at the first update. The public client
// create takes no time and seeds its transport and its object at 0.0; the
// game's clock then starts at T=1000.0. The client's transport is handed
// time relative to the first update (0.0 at the first update, the elapsed
// seconds after), so its connect timers and token lifetime run from there and
// a seed of 0.0 never reads as 1000 seconds without a packet. The server's
// transport, and the object's own clock, read the game's time as it is.

static void message_objects_client_clock_starts_at_first_update()
{
    rocketnet_message_config_t config;
    mo_config( &config );
    mo_net_t * net = mo_net_new();

    // seeded at 0.0, the public create's seed
    rn_message_link_t server_link = mo_link( net->server );
    rocketnet_message_server_t * server = (rocketnet_message_server_t *) rn_message_object_create( NULL, &server_link, 1, 2, &config, NULL, 0, ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS, 0.0 );
    check( server );
    mo_endpoint_t * e = mo_net_client( net, 0 );
    rn_message_link_t client_link = mo_link( e );
    client_link.clock_from_first_update = 1;                    // as rn_message_client_link binds it
    rocketnet_message_client_t * client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &client_link, 0, 1, &config, NULL, 0, ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, 0.0 );
    check( client );
    mo_net_connect( net, 0 );

    // the public client binds the flag; the server does not
    {
        rn_message_link_t bound;
        rn_message_client_link( &bound, NULL );
        check( bound.clock_from_first_update == 1 );
        rn_message_server_link( &bound, NULL );
        check( bound.clock_from_first_update == 0 );
    }

    double time = MO_T0;
    check( rocketnet_message_server_update( server, time ) == ROCKETNET_MESSAGE_OK );
    check( rocketnet_message_client_update( client, time ) == ROCKETNET_MESSAGE_OK );
    check( e->time == 0.0 );                                    // the client transport's first update is its 0.0
    check( net->server->time == MO_T0 );                        // the server transport reads the game's time
    check( client->object.time == MO_T0 );                      // the object's own clock is the game's time
    check( server->object.time == MO_T0 );

    // a reliable message each way still flows, from T=1000.0 on
    uint8_t * up = rocketnet_message_client_start( client, 1, ROCKETNET_RELIABLE_ORDERED, 16 );
    check( up != NULL );
    mo_fill( up, 7, 16 );
    check( rocketnet_message_client_finish( client, 16 ) != 0 );
    uint8_t * down = rocketnet_message_server_start( server, 0, 1, ROCKETNET_RELIABLE_ORDERED, 16 );
    check( down != NULL );
    mo_fill( down, 9, 16 );
    check( rocketnet_message_server_finish( server, 0, 16 ) != 0 );
    rocketnet_message_server_send_packets( server );
    rocketnet_message_client_send_packets( client );

    bool server_got = false;
    bool client_got = false;
    for ( int step = 1; step <= 20 && !( server_got && client_got ); step++ )
    {
        time = MO_T0 + 0.25 * step;
        check( rocketnet_message_server_update( server, time ) == ROCKETNET_MESSAGE_OK );
        check( rocketnet_message_client_update( client, time ) == ROCKETNET_MESSAGE_OK );
        check( e->time == 0.25 * step );                        // elapsed since the first update
        check( net->server->time == time );

        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_message_server_receive( server, 0, 1, &data, &bytes ) )
        {
            std::vector<uint8_t> got( data, data + bytes );
            uint32_t id = 0;
            check( bytes == 16 && mo_verify( got, &id ) && id == 7 );
            server_got = true;
            rocketnet_message_server_release( server, 0, data );
        }
        while ( rocketnet_message_client_receive( client, 1, &data, &bytes ) )
        {
            std::vector<uint8_t> got( data, data + bytes );
            uint32_t id = 0;
            check( bytes == 16 && mo_verify( got, &id ) && id == 9 );
            client_got = true;
            rocketnet_message_client_release( client, data );
        }
        rocketnet_message_server_send_packets( server );
        rocketnet_message_client_send_packets( client );
    }
    check( server_got );
    check( client_got );
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_MESSAGE_ERROR_NONE );
    check( rocketnet_message_server_disconnect_reason( server, 0 ) == ROCKETNET_MESSAGE_ERROR_NONE );

    rocketnet_message_client_destroy( client );
    rocketnet_message_server_destroy( server );
    mo_net_free( net );
}

// ---------------------------------------------------------------------------
// The unreliable path on the shipped objects (docs/CHANNELS.md 5, 6.3, 6.4,
// 9.3): the cases of the retired standalone rn_unreliable_* module that
// describe behaviour the product has, run through the message objects.
// ---------------------------------------------------------------------------

// a send the store has no room for exhausts the connection and kicks it (R5):
// the shipped path never drops an unreliable send for a full store, it counts
// the refusal once and ends the connection
static void unreliable_store_full_exhausts_and_kicks()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    config.send_queue_messages = 1024;                          // the store runs out before the descriptors do
    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config );
    mo_step( &pair );

    int started = 0;
    for ( ;; )
    {
        uint8_t * p = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_UNRELIABLE, 16 );
        if ( !p )
            break;
        memset( p, 0xAB, 16 );
        check( rocketnet_message_server_finish( pair.server, 0, 16 ) != 0 );
        started++;
        check( started < 100000 );
    }
    check( started > 0 );

    const rocketnet_counters_t * counters = rocketnet_message_server_counters( pair.server );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 1 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 1 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED );
    check( rocketnet_message_server_client_connected( pair.server, 0 ) == 0 );
    check( pair.net->server->disconnects == 1 );

    // sticky: the kicked connection takes no further send
    check( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_UNRELIABLE, 16 ) == NULL );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 1 );

    mo_pair_teardown( &pair );
}

// an unreliable message past the one-packet limit is promoted to its reliable
// twin and counted (RakNet ReliabilityLayer); the one at the limit still goes
// as unreliable, and a valid message behind the promotion still arrives
static void unreliable_past_limit_promotes_counted_and_not_wedged()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );
    const int limit = (int) client->object.unreliable_limit;
    check( limit > 1000 && limit < ROCKETNET_MAX_PAYLOAD_BYTES );

    static uint8_t message[16384];
    mo_fill( message, 1, (uint32_t) limit );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, message, limit ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 0 );

    // past the limit, and the largest the config allows: both promoted
    mo_fill( message, 2, (uint32_t) limit + 1 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, message, limit + 1 ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 1 );
    mo_fill( message, 3, 16384 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, message, 16384 ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 2 );

    // a promotion is not a refusal or a store-full drop, and the connection stays up
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_MESSAGE_ERROR_NONE );

    // not wedged: a valid message behind the promoted ones is sent
    mo_fill( message, 4, 50 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, message, 50 ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 2 );

    std::vector<mo_received_t> got;
    for ( int i = 0; i < 128 && got.size() < 4; i++ )
        mo_step( &pair, &got );
    check( got.size() == 4 );
    const int expected_bytes[4] = { limit, limit + 1, 16384, 50 };
    for ( int id = 1; id <= 4; id++ )
    {
        int seen = 0;
        for ( size_t k = 0; k < got.size(); k++ )
        {
            uint32_t got_id = 0;
            if ( mo_verify( got[k].bytes, &got_id ) && got_id == (uint32_t) id )
            {
                seen++;
                check( (int) got[k].bytes.size() == expected_bytes[id - 1] );
            }
        }
        check( seen == 1 );
    }

    mo_pair_teardown( &pair );
}

// a packet the transport fails to finish drops the unreliable records it
// carried and counts each (6.4, Failed send); nothing reaches the wire, and the
// next send is unaffected
static void unreliable_failed_packet_drops_and_counts_each_record()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );

    for ( uint32_t i = 0; i < 3; i++ )
    {
        uint8_t message[16];
        mo_fill( message, 10 + i, sizeof( message ) );
        check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
    }

    pair.net->fail_finish = 1;
    std::vector<mo_received_t> got;
    mo_step( &pair, &got );
    mo_step( &pair, &got );
    check( pair.net->fail_finish == 0 );                        // the failure was taken by the one packet
    check( got.empty() );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 3 );

    // the dropped records are gone for good (unreliable): none arrives later
    for ( int i = 0; i < 4; i++ )
        mo_step( &pair, &got );
    check( got.empty() );

    uint8_t message[16];
    mo_fill( message, 99, sizeof( message ) );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
    for ( int i = 0; i < 3; i++ )
        mo_step( &pair, &got );
    check( got.size() == 1 );
    uint32_t id = 0;
    check( mo_verify( got[0].bytes, &id ) && id == 99 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 3 );

    mo_pair_teardown( &pair );
}

// the send store holds an unreliable message only until its record is written:
// the chunks are back once the packet is built (9.3 step 3, R5), across the
// store's classes
static void unreliable_chunks_freed_when_written()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    rn_send_store_t * store = &mo_client_connection( client )->region->store;

    uint8_t small[64], medium[400];
    mo_fill( small, 20, sizeof( small ) );
    mo_fill( medium, 21, sizeof( medium ) );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, small, (int) sizeof( small ) ) != 0 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, medium, (int) sizeof( medium ) ) != 0 );

    uint32_t used = 0;
    for ( int cl = 0; cl < RN_STORE_NUM_CLASSES; cl++ )
        used += rn_send_store_total_chunks( store, cl ) - rn_send_store_free_chunks( store, cl );
    check( used >= 2 );                                         // both messages hold store chunks while queued

    std::vector<mo_received_t> got;
    mo_step( &pair, &got );
    mo_step( &pair, &got );
    check( got.size() == 2 );
    for ( int cl = 0; cl < RN_STORE_NUM_CLASSES; cl++ )
        check( rn_send_store_free_chunks( store, cl ) == rn_send_store_total_chunks( store, cl ) );

    mo_pair_teardown( &pair );
}

// small unreliable messages over a link that loses packets: every message that
// arrives is byte-identical to one sent and arrives once, none is invented, and
// with no loss every message arrives (R8)
static void unreliable_over_a_lossy_link_arrives_intact_at_most_once()
{
    for ( int lossy = 0; lossy < 2; lossy++ )
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, NULL, 100000, 100000 );
        mo_step( &pair );

        rocketnet_message_client_t * client = pair.clients[0];
        const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );
        std::vector<mo_received_t> got;
        uint32_t next_id = 1000;
        uint32_t sent = 0;
        for ( int round = 0; round < 60; round++ )
        {
            for ( int n = 0; n < 4; n++ )
            {
                uint8_t message[100];
                mo_fill( message, next_id, sizeof( message ) );
                check( rocketnet_message_client_send( client, 0, ROCKETNET_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
                next_id++;
                sent++;
            }
            if ( lossy && round % 3 == 0 )
                pair.net->drop_next_to_server = 1;
            mo_step( &pair, &got );
        }
        for ( int i = 0; i < 4; i++ )
            mo_step( &pair, &got );

        std::vector<uint32_t> ids;
        for ( size_t i = 0; i < got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( got[i].bytes, &id ) );
            check( got[i].bytes.size() == 100 && id >= 1000 && id < next_id );
            for ( size_t k = 0; k < ids.size(); k++ )
                check( ids[k] != id );                          // at most once
            ids.push_back( id );
        }
        check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
        check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == 0 );
        if ( lossy )
            check( ids.size() > 0 && ids.size() < sent );
        else
            check( ids.size() == sent );

        mo_pair_teardown( &pair );
    }
}

REGISTER_UNIT_TEST( start_finish_pointer_reaches_the_frame );
REGISTER_UNIT_TEST( receive_view_is_the_record );
REGISTER_UNIT_TEST( release_returns_room );
REGISTER_UNIT_TEST( ordered_and_unreliable_end_to_end_in_memory );
REGISTER_UNIT_TEST( server_kicks_exhausted_client_others_unaffected );
REGISTER_UNIT_TEST( client_disconnects_itself_on_exhaustion );
REGISTER_UNIT_TEST( slot_reuse_starts_clean );
REGISTER_UNIT_TEST( acks_only_from_transport_acked_packets );
REGISTER_UNIT_TEST( budget_set_at_create_and_runtime );
REGISTER_UNIT_TEST( message_objects_no_allocation_after_create );
REGISTER_UNIT_TEST( rnslots_witness_replays );
REGISTER_UNIT_TEST( message_objects_client_clock_starts_at_first_update );
REGISTER_UNIT_TEST( message_send_refused_counts_bad_arguments );
REGISTER_UNIT_TEST( unreliable_store_full_exhausts_and_kicks );
REGISTER_UNIT_TEST( unreliable_past_limit_promotes_counted_and_not_wedged );
REGISTER_UNIT_TEST( unreliable_failed_packet_drops_and_counts_each_record );
REGISTER_UNIT_TEST( unreliable_chunks_freed_when_written );
REGISTER_UNIT_TEST( unreliable_over_a_lossy_link_arrives_intact_at_most_once );
