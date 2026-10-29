/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client and server message objects and their C API (docs/CHANNELS.md 10
// to 12; proofs/tla/RNSlots.tla, proofs/tla/RNConnectionMemory.tla). The objects run over
// the in-memory transport pair below, bound through the object's transport
// seam (rn_message_link_t) exactly where the real transports bind: it hands
// out frames at start_packet with the transport's packet id, takes them at
// finish_packet, opens each packet once on the far side into a payload batch
// valid until the next update, and reports the opened ids back to the sender
// as its acked packet ids at its next update (R1: the only ack). Slots,
// connection ids and loss are the harness's to script. Time is only ever an
// argument: nothing sleeps, nothing opens a socket.

#include "unit_shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <vector>

#include "test_unit_registry.h"
#if ROCKETNET_DEV
#include "test_env_source.h"
#endif // #if ROCKETNET_DEV
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_wire.h"
#include "loopback_hub.h"

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
    uint64_t connection;                            // the slot's connection when the packet was sent (P9b)
    int bytes;
    // the padding to data and after it is written out, so the compiler adds none (MSVC C4324)
    uint8_t head_pad[16 - sizeof( uint64_t ) - sizeof( int )];
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t tail_pad[( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 15 ) / 16 * 16 - ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
};

struct mo_net_t;

struct mo_endpoint_t
{
    mo_net_t * net;
    bool is_server;
    int slot;                                       // a client endpoint's slot on the server

    // send: frames handed out by start_packet, finished into `outbound`
    alignas( 16 ) uint8_t frames[MO_MAX_FRAMES][MO_PAYLOAD_INDEX + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    bool frame_used[MO_MAX_FRAMES];
    uint64_t frame_id[MO_MAX_FRAMES];
    int frame_slot[MO_MAX_FRAMES];

    // receive: the packets on the wire to this endpoint, and the update's batch
    std::vector<mo_packet_t> inbound;
    std::vector<mo_packet_t> batch;
    std::vector<uint8_t *> batch_data;
    std::vector<int> batch_bytes;
    std::vector<int> batch_slot;
    std::vector<uint64_t> batch_connection;

    // acks: ids this endpoint opened, reported to the sender at its next update
    std::vector<uint64_t> acks_inbox;               // ids the other end opened of this endpoint's sends
    std::vector<int> acks_inbox_slot;
    std::vector<uint64_t> acks_inbox_connection;
    std::vector<uint64_t> acked;                    // this update's acked batch
    std::vector<int> acked_slot;
    std::vector<uint64_t> acked_connection;

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
    bool refuse_start;                              // start_packet gives no frame while set (a full send ring)
    int start_limit_bytes;                          // above 0: start_packet gives no frame for a request above this many bytes
    bool hold_acks;                                 // opened ids are not reported back while set
    bool keep_log;
};

#if ROCKETNET_DEV
static mo_net_t * mo_net_new()
{
    mo_net_t * net = new mo_net_t();
    memset( net->slot_connection, 0, sizeof( net->slot_connection ) );
    net->server = new mo_endpoint_t();
    net->server->net = net;
    net->server->is_server = true;
    net->server->slot = -1;
    for ( int i = 0; i < MO_MAX_SLOTS; i++ )
    {
        net->clients[i] = NULL;
    }
    net->next_connection = 100;
    net->next_packet_id = 1000;
    net->drop_to_server = false;
    net->drop_to_client = false;
    net->drop_next_to_server = 0;
    net->fail_finish = 0;
    net->refuse_start = false;
    net->start_limit_bytes = 0;
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
    {
        net->clients[slot]->connected = true;
    }
}

static void mo_net_drop_slot( mo_net_t * net, int slot )
{
    net->slot_connection[slot] = 0;
    if ( net->clients[slot] )
    {
        net->clients[slot]->connected = false;
    }
}

static void mo_net_free( mo_net_t * net )
{
    for ( int i = 0; i < MO_MAX_SLOTS; i++ )
    {
        delete net->clients[i];
    }
    delete net->server;
    delete net;
}

// --- the link functions ---

static void mo_update( void * transport, double time )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    mo_net_t * net = e->net;
    e->time = time;

    // the transport's acked packet ids: what the other end opened since the last update

    e->acked.clear();
    e->acked_slot.clear();
    e->acked_connection.clear();

    if ( !net->hold_acks )
    {
        e->acked.swap( e->acks_inbox );
        e->acked_slot.swap( e->acks_inbox_slot );
        e->acked_connection.swap( e->acks_inbox_connection );
    }

    // the batch: the last one is gone, the wire's packets are opened once each

    e->batch.clear();
    e->batch_data.clear();
    e->batch_bytes.clear();
    e->batch_slot.clear();
    e->batch_connection.clear();
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
            sender->acks_inbox_connection.push_back( p.connection );
        }
    }

    e->inbound.clear();

    for ( size_t i = 0; i < e->batch.size(); i++ )
    {
        e->batch_data.push_back( e->batch[i].data );
        e->batch_bytes.push_back( e->batch[i].bytes );
        e->batch_slot.push_back( e->batch[i].slot );
        e->batch_connection.push_back( e->batch[i].connection );
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
    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES || e->net->refuse_start )
        return NULL;
    if ( e->net->start_limit_bytes > 0 && request_bytes > e->net->start_limit_bytes )
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
        context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
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
    check( payload_bytes >= 1 && payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
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
    packet.connection = e->net->slot_connection[packet.slot];
    packet.bytes = payload_bytes;
    memcpy( packet.data, e->frames[f] + MO_PAYLOAD_INDEX, (size_t) payload_bytes );         // the wire: the transport's own business
    if ( e->net->keep_log )
    {
        e->sent_log.push_back( packet );
    }

    mo_net_t * net = e->net;

    if ( e->is_server )
    {
        mo_endpoint_t * to = net->clients[packet.slot];
        if ( to && !net->drop_to_client )
        {
            to->inbound.push_back( packet );
        }
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
    {
        e->frame_used[f] = false;
    }
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

// the connection each batch entry was received for (P9b): the server's are
// the slot's connection when the packet was sent; the client holds one
// connection, which its object binds as 1

static uint64_t mo_payload_connection( void * transport, int k )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    return e->is_server ? e->batch_connection[(size_t) k] : 1;
}

static uint64_t mo_acked_connection( void * transport, int k )
{
    mo_endpoint_t * e = (mo_endpoint_t *) transport;
    return e->is_server ? e->acked_connection[(size_t) k] : 1;
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
    link.payload_connection = mo_payload_connection;
    link.acked_connection = mo_acked_connection;
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
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// objects on the pair
// ---------------------------------------------------------------------------

static void mo_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->num_channels = 4;
    config->reliable_messages_in_flight = 64;
    config->receive_message_buffer_bytes = 65536;
    config->max_message_bytes = 16384;
    config->send_message_buffer_bytes = 65536;
    config->send_queue_messages = 64;
    config->packets_in_flight = 64;
    config->packets_per_update = 8;
    config->unreliable_messages_per_update = 64;
    config->message_resend_delay_initial_ms = 100;
    config->message_resend_delay_min_ms = 20;
    config->message_resend_delay_max_ms = 400;
    config->reliable_message_timeout_ms = 120000;
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

#if ROCKETNET_DEV
static rocketnet_message_server_t * mo_server_create( mo_net_t * net, const rocketnet_message_config_t * config, int slots, int32_t kbps, void * memory = NULL, uint64_t memory_bytes = 0 )
{
    rn_message_link_t link = mo_link( net->server );
    return (rocketnet_message_server_t *) rn_message_object_create( NULL, &link, 1, slots, config, memory, memory_bytes, kbps, MO_T0 );
}

static rocketnet_message_client_t * mo_client_create( mo_net_t * net, int slot, const rocketnet_message_config_t * config, int32_t kbps, void * memory = NULL, uint64_t memory_bytes = 0 )
{
    mo_endpoint_t * e = mo_net_client( net, slot );
    rn_message_link_t link = mo_link( e );
    return (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, config, memory, memory_bytes, kbps, MO_T0 );
}

static void mo_pair_setup( mo_pair_t * pair, int num_clients, const rocketnet_message_config_t * config = NULL,
                           int32_t client_kbps = ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, int32_t server_kbps = ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS )
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
    {
        rocketnet_message_client_destroy( pair->clients[i] );
    }
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

static uint64_t mo_align64( uint64_t bytes )
{
    return ( bytes + 63 ) & ~(uint64_t) 63;
}
#endif // #if ROCKETNET_DEV

struct mo_old_message_connection_hot_t
{
    uint64_t connection_id;
    uint8_t * views;
    uint32_t * cursors;
    uint32_t held;
    int state;
};

#if ROCKETNET_DEV
static uint64_t mo_message_object_layout_formula( const rocketnet_message_config_t * config, int connections, bool is_server, bool old_cursor_array )
{
    rocketnet_message_config_t sized = *config;
    const int kbps = is_server ? config->server_send_kbps : config->client_send_kbps;
    sized.client_send_kbps = kbps;
    sized.server_send_kbps = kbps;

    const uint64_t n = (uint64_t) connections;
    const uint64_t deliveries = (uint64_t) sized.reliable_messages_in_flight + (uint64_t) sized.unreliable_messages_per_update;
    uint64_t offset = mo_align64( sizeof( rn_message_object_t ) > sizeof( rocketnet_message_server_t ) ? sizeof( rn_message_object_t ) : sizeof( rocketnet_message_server_t ) );
    offset += mo_align64( ( old_cursor_array ? sizeof( mo_old_message_connection_hot_t ) : sizeof( rn_message_connection_hot_t ) ) * n );
    offset += mo_align64( sizeof( rn_message_connection_t ) * n );
    offset += mo_align64( deliveries ) * n;
    if ( old_cursor_array )
    {
        offset += mo_align64( sizeof( uint32_t ) * (uint64_t) sized.num_channels ) * n;
    }
    offset += mo_align64( rn_arena_required_bytes( &sized, connections ) );
    return offset;
}

static void test_receive_cursor_scalar_layout_and_exact_blocks()
{
    static_assert( sizeof( mo_old_message_connection_hot_t ) == 32, "old cursor-pointer slot formula changed" );

    const int channels[] = { 1, 4, ROCKETNET_MAX_CHANNELS };
    const int connections[] = { 1, MO_MAX_SLOTS };

    for ( size_t c = 0; c < sizeof( channels ) / sizeof( channels[0] ); c++ )
    {
        rocketnet_message_config_t config;
        mo_config( &config );
        config.num_channels = channels[c];

        for ( size_t k = 0; k < sizeof( connections ) / sizeof( connections[0] ); k++ )
        {
            const int n = connections[k];
            const bool is_server = n > 1;
            const uint64_t actual = rn_message_object_required_bytes( &config, n, is_server );
            const uint64_t current_formula = mo_message_object_layout_formula( &config, n, is_server, false );
            const uint64_t old_formula = mo_message_object_layout_formula( &config, n, is_server, true );
            const uint64_t removed_hot_bytes = mo_align64( sizeof( mo_old_message_connection_hot_t ) * (uint64_t) n ) -
                                               mo_align64( sizeof( rn_message_connection_hot_t ) * (uint64_t) n );
            const uint64_t removed_cursor_bytes = mo_align64( sizeof( uint32_t ) * (uint64_t) channels[c] ) * (uint64_t) n;
            check( actual == current_formula );
            check( old_formula == actual + removed_hot_bytes + removed_cursor_bytes );
        }

        mo_net_t * net = mo_net_new();
        rn_message_link_t client_link = mo_link( mo_net_client( net, 0 ) );
        const uint64_t bytes = rn_message_object_required_bytes( &config, 1, 0 );
        const uint64_t allocation_bytes = bytes + ROCKETNET_ALLOCATION_ALIGNMENT - 1;
        check( allocation_bytes <= SIZE_MAX );

        void * allocation = malloc( (size_t) allocation_bytes );
        check( allocation != NULL );

        void * block = (void *) ( ( (uintptr_t) allocation + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) &
                                  ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
        check( rn_message_object_create( NULL, &client_link, 0, 1, &config, block, bytes - 1,
                                         ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, MO_T0 ) == NULL );

        rn_message_object_t * object = rn_message_object_create( NULL, &client_link, 0, 1, &config, block, bytes,
                                                                 ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, MO_T0 );
        check( object != NULL );
        check( object->hot_connections[0].receive_cursor == 0 );
        rn_message_object_destroy( object );
        free( allocation );
        mo_net_free( net );
    }
}
#endif // #if ROCKETNET_DEV

// every message the application received this update, released before the next

struct mo_received_t
{
    int slot;
    int channel;
    std::vector<uint8_t> bytes;
};

#if ROCKETNET_DEV
static void mo_drain_server( mo_pair_t * pair, std::vector<mo_received_t> * out )
{
    // the server's one receive queue across every slot, in batches: each
    // entry names its sender's slot and the channel it was delivered on

    for ( ;; )
    {
        static rocketnet_message_batch_t batch;     // 1.5 MB: never on the stack (Windows gives 1 MB)
        const int count = rocketnet_message_server_receive_messages( pair->server, &batch );
        if ( count == 0 )
            break;

        for ( int i = 0; i < batch.num_messages; i++ )
        {
            check( batch.client_index[i] >= 0 && batch.client_index[i] < MO_MAX_SLOTS );

            mo_received_t r;
            r.slot = (int) batch.client_index[i];
            r.channel = batch.channel[i];
            r.bytes.assign( batch.data[i], batch.data[i] + batch.bytes[i] );
            if ( out )
            {
                out->push_back( r );
            }
        }
    }
}

static void mo_drain_client( rocketnet_message_client_t * client, std::vector<mo_received_t> * out )
{
    for ( ;; )
    {
        static rocketnet_message_batch_t batch;     // 1.5 MB: never on the stack (Windows gives 1 MB)
        const int count = rocketnet_message_client_receive_messages( client, &batch );
        if ( count == 0 )
            break;

        for ( int i = 0; i < batch.num_messages; i++ )
        {
            check( batch.client_index[i] == 0 );

            mo_received_t r;
            r.slot = 0;
            r.channel = batch.channel[i];
            r.bytes.assign( batch.data[i], batch.data[i] + batch.bytes[i] );
            if ( out )
            {
                out->push_back( r );
            }
        }
    }
}

// one step of the whole pair: every object updates, the applications drain and
// release, every object sends

static void mo_step( mo_pair_t * pair, std::vector<mo_received_t> * server_got = NULL, std::vector<mo_received_t> * client_got = NULL, double dt = 0.01 )
{
    pair->time += dt;
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        check( rocketnet_message_client_update( pair->clients[i], pair->time ) == ROCKETNET_UPDATE_OK );
    }
    mo_drain_server( pair, server_got );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        mo_drain_client( pair->clients[i], client_got );
    }
    rocketnet_message_server_send_packets( pair->server );
    for ( int i = 0; i < pair->num_clients; i++ )
    {
        rocketnet_message_client_send_packets( pair->clients[i] );
    }
}

static uint8_t mo_pattern( uint32_t id, uint32_t i )
{
    return (uint8_t) ( id * 131 + i * 7 + ( i >> 8 ) * 13 + 5 );
}

static void mo_fill( uint8_t * data, uint32_t id, uint32_t bytes )
{
    for ( uint32_t i = 0; i < bytes; i++ )
    {
        data[i] = mo_pattern( id, i );
    }
    if ( bytes >= 4 )
    {
        memcpy( data, &id, 4 );
    }
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

static void test_message_send_refused_counts_bad_arguments()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    const uint64_t before = mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    check( rocketnet_message_server_start( pair.server, 0, -1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 ) == NULL );
    check( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0 ) == NULL );
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == before + 2 );

    uint8_t * valid = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
    check( valid != NULL );
    valid[0] = 42;
    check( rocketnet_message_server_finish( pair.server, 0, 1 ) != 0 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R3: on the queued path -- a reliable message, or an unreliable one the
// transport gave no frame at start -- the pointer start returns is the region
// record the transport frame's bytes are copied from: THE COPIES (a), the one
// copy on the send path. A fitting unreliable message written in place is
// test_unit_unreliable_in_place.cpp's.
// ---------------------------------------------------------------------------

// A client start before its transport is CONNECTED is dropped and counted
// once in MESSAGE_SEND_NOT_CONNECTED (standard/MESSAGES.md m1.5,
// proofs/tla/ClientSession.tla DropSend): NULL, never queued, never a fallback, and
// nothing of it arrives once the transport connects.
static int mo_asserts = 0;

static void mo_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    mo_asserts++;
}

static void test_message_client_start_dropped_before_transport_connected()
{
    rocketnet_message_config_t config;
    mo_config( &config );

    mo_pair_t pair;
    memset( &pair, 0, sizeof( pair ) );
    pair.net = mo_net_new();
    pair.num_clients = 1;
    pair.server = mo_server_create( pair.net, &config, MO_MAX_SLOTS, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
    pair.clients[0] = mo_client_create( pair.net, 0, &config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( pair.server && pair.clients[0] );
    pair.time = MO_T0;

    const rocketnet_counters_t * counters = rocketnet_message_client_counters( pair.clients[0] );

    const int modes[] = { ROCKETNET_RELIABILITY_RELIABLE_ORDERED, ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_RELIABLE };
    const int num_modes = (int) ( sizeof( modes ) / sizeof( modes[0] ) );
    for ( int i = 0; i < num_modes; i++ )
    {
        const uint64_t before = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );
        uint8_t * p = rocketnet_message_client_start( pair.clients[0], 0, modes[i], 100 );
        check( p == NULL );
        if ( p )
        {
            mo_fill( p, 900 + (uint32_t) i, 100 );
        }
        rocketnet_assert_function( mo_assert_hook );
        mo_asserts = 0;
        check( rocketnet_message_client_finish( pair.clients[0], 100 ) == 0 );
        rocketnet_assert_function( NULL );
        check( mo_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
        check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) == before + 1 );
    }

    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) == (uint64_t) num_modes );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == 0 );
    check( pair.net->clients[0]->finishes == 0 );

    // the transport connects: nothing of the dropped sends arrives, and the
    // first send after CONNECTED does

    mo_net_connect( pair.net, 0 );

    std::vector<mo_received_t> server_got;
    for ( int i = 0; i < 20; i++ )
    {
        mo_step( &pair, &server_got, NULL );
    }
    check( server_got.empty() );

    uint8_t * p = rocketnet_message_client_start( pair.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 );
    check( p != NULL );

    if ( p )
    {
        mo_fill( p, 7, 100 );
        check( rocketnet_message_client_finish( pair.clients[0], 100 ) != 0 );
    }

    for ( int i = 0; i < 20; i++ )
    {
        mo_step( &pair, &server_got, NULL );
    }
    check( server_got.size() == 1 );

    uint32_t id = 0;
    check( server_got.size() == 1 && mo_verify( server_got[0].bytes, &id ) && id == 7 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) == (uint64_t) num_modes );

    // a server slot with no connection: the same drop, on the server's counters

    const rocketnet_counters_t * server_counters = rocketnet_message_server_counters( pair.server );
    const uint64_t server_before = mo_counter( server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );
    const uint64_t refused_before = mo_counter( server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    check( rocketnet_message_server_start( pair.server, 1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 10 ) == NULL );
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;
    check( rocketnet_message_server_finish( pair.server, 1, 10 ) == 0 );
    rocketnet_assert_function( NULL );
    check( mo_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    check( mo_counter( server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) == server_before + 1 );
    check( mo_counter( server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused_before );
    mo_pair_teardown( &pair );
}
REGISTER_UNIT_TEST( test_message_client_start_dropped_before_transport_connected );

// A finish whose bytes_written is outside 1 .. the reservation is refused and
// aborts its start; a second start while one is pending is refused too. Each
// refusal logs exactly one ERROR line naming the refused size and the reserved
// size (or the pending start). Control flow, return values, counters and the
// abort are unchanged: this pins the log lines.
static int g_mo_error_lines = 0;
static char g_mo_last_line[512];

static void mo_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
        return;

    const size_t n = length < sizeof( g_mo_last_line ) - 1 ? length : sizeof( g_mo_last_line ) - 1;
    memcpy( g_mo_last_line, message, n );
    g_mo_last_line[n] = '\0';
    g_mo_error_lines++;
}

static void test_message_refused_finish_and_start_log_one_error()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rn_message_object_t * object = &pair.server->object;
    const rocketnet_counters_t * counters = rocketnet_message_server_counters( pair.server );

    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    g_mo_error_lines = 0;
    rocketnet_log_function( mo_log_capture, NULL );

    const int channel = 0;

    // both finish appusals, on a fitting-unreliable in-place start (the
    // in-place build or whole-frame refusal) and a reliable region start: each
    // logs one error naming the bytes and the reservation, returns 0, and
    // leaves no start pending

    for ( int mode = 0; mode < 2; mode++ )
    {
        const int reliability = mode == 0 ? ROCKETNET_RELIABILITY_UNRELIABLE : ROCKETNET_RELIABILITY_RELIABLE_ORDERED;
        uint8_t * p = rn_message_object_start( object, 0, channel, reliability, 10 );
        check( p != NULL );
        if ( !p )
            break;

        const uint64_t line_before = (uint64_t) g_mo_error_lines;
        check( rn_message_object_finish( object, 0, 0 ) == 0 );     // bytes_written 0
        check( (uint64_t) g_mo_error_lines == line_before + 1 );
        check( strstr( g_mo_last_line, "message finish refused" ) != NULL );

        p = rn_message_object_start( object, 0, channel, reliability, 10 );
        check( p != NULL );
        if ( !p )
            break;
        check( rn_message_object_finish( object, 0, 11 ) == 0 );    // past the reservation
        check( (uint64_t) g_mo_error_lines == line_before + 2 );
        check( strstr( g_mo_last_line, "message finish refused" ) != NULL );
    }

    // a second start while one is pending: NULL, counted REFUSED, one error line

    const uint64_t refused_before = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    const uint64_t line_before = (uint64_t) g_mo_error_lines;
    uint8_t * first = rn_message_object_start( object, 0, channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 10 );
    check( first != NULL );
    check( rn_message_object_start( object, 0, channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 10 ) == NULL );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused_before + 1 );
    check( (uint64_t) g_mo_error_lines == line_before + 1 );
    check( strstr( g_mo_last_line, "start is already pending" ) != NULL );
    check( rn_message_object_finish( object, 0, 10 ) != 0 );         // the first start still finishes

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    mo_pair_teardown( &pair );
}
REGISTER_UNIT_TEST( test_message_refused_finish_and_start_log_one_error );

static const uint8_t * mo_find( const uint8_t * haystack, int haystack_bytes, const uint8_t * needle, int needle_bytes )
{
    for ( int i = 0; i + needle_bytes <= haystack_bytes; i++ )
        if ( memcmp( haystack + i, needle, (size_t) needle_bytes ) == 0 )
            return haystack + i;
    return NULL;
}

static void test_start_finish_pointer_reaches_the_frame()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    rn_message_connection_t * connection = mo_client_connection( client );

    const int reliabilities[2] = { ROCKETNET_RELIABILITY_RELIABLE_ORDERED, ROCKETNET_RELIABILITY_UNRELIABLE };

    for ( int r = 0; r < 2; r++ )
    {
        const int bytes = 300;
        pair.net->refuse_start = reliabilities[r] == ROCKETNET_RELIABILITY_UNRELIABLE;           // no frame at start: the queued path

        uint8_t * p = rocketnet_message_client_start( client, 1, reliabilities[r], bytes );
        pair.net->refuse_start = false;
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

        if ( reliabilities[r] == ROCKETNET_RELIABILITY_RELIABLE_ORDERED )
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

static void test_receive_view_is_the_record()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    const int reliabilities[2] = { ROCKETNET_RELIABILITY_RELIABLE_ORDERED, ROCKETNET_RELIABILITY_UNRELIABLE };

    for ( int r = 0; r < 2; r++ )
    {
        uint8_t message[200];
        mo_fill( message, 42 + (uint32_t) r, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 2, reliabilities[r], message, (int) sizeof( message ) ) != 0 );
        rocketnet_message_client_send_packets( pair.clients[0] );

        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

        const int channel = reliabilities[r] == ROCKETNET_RELIABILITY_UNRELIABLE ? 0 : 2;       // unreliable carries no channel: channel 0
        const uint8_t * data = NULL;
        int bytes = 0;
        int from = -1;
        int delivered_channel = -1;
        check( rocketnet_message_server_receive( pair.server, &from, &delivered_channel, &data, &bytes ) == 1 );
        check( from == 0 && delivered_channel == channel );
        check( bytes == (int) sizeof( message ) );
        check( memcmp( data, message, sizeof( message ) ) == 0 );
        check( mo_in_batch( pair.net->server, data, bytes ) );                      // pointer identity: inside the transport's payload

        const uint8_t * again = NULL;
        int again_bytes = 0;
        check( rocketnet_message_server_receive( pair.server, NULL, NULL, &again, &again_bytes ) == 0 );
        check( again == NULL );

        rocketnet_message_server_release( pair.server, data );
        mo_step( &pair );
    }

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// RNSlots Release and RejectUnreleasedUpdate: release returns the room;
// update with a held view asserts in a debug build and is the named error in
// release; memory never grows from held views
// ---------------------------------------------------------------------------

static void test_release_returns_room()
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
        check( rocketnet_message_client_send( pair.clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        rocketnet_message_client_send_packets( pair.clients[0] );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

        const uint8_t * data = NULL;
        int bytes = 0;
        int delivered_channel = -1;
        check( rocketnet_message_server_receive( pair.server, NULL, &delivered_channel, &data, &bytes ) == 1 );
        check( delivered_channel == 1 );
        check( slot->hot->held == 1 );

        uint32_t live = 0;
        for ( uint32_t i = 0; i < region->window; i++ )
        {
            live += region->receive_descriptors[i].live;
        }
        check( live == 1 );
        rocketnet_message_server_release( pair.server, data );
        check( slot->hot->held == 0 );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
        live = 0;
        for ( uint32_t i = 0; i < region->window; i++ )
        {
            live += region->receive_descriptors[i].live;
        }
        check( live == 0 );
    }

    // a release of a pointer that is not a held view is a counted misuse
    {
        uint8_t stray[4] = { 0, 0, 0, 0 };
        const uint64_t before = rocketnet_api_misuse_count();
        rocketnet_message_server_release( pair.server, stray );
        check( rocketnet_api_misuse_count() == before + 1 );
    }

    // update with a held view: the named error, the assert in a debug build,
    // and the views are taken back so nothing grows

    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
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
        check( rocketnet_message_client_send( pair.clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        rocketnet_message_client_send_packets( pair.clients[0] );
        pair.time += 0.01;

        const int result = rocketnet_message_server_update( pair.server, pair.time );
        check( result == ( round == 0 ? ROCKETNET_UPDATE_OK : ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED ) );
        rocketnet_message_server_send_packets( pair.server );
        check( rocketnet_message_client_update( pair.clients[0], pair.time ) == ROCKETNET_UPDATE_OK );

        const uint8_t * data = NULL;
        int bytes = 0;
        int delivered_channel = -1;
        check( rocketnet_message_server_receive( pair.server, NULL, &delivered_channel, &data, &bytes ) == 1 );    // held, never released
        check( delivered_channel == 1 );
        check( slot->hot->held == 1 );

        uint32_t live = 0;
        for ( uint32_t i = 0; i < region->window; i++ )
        {
            live += region->receive_descriptors[i].live;
        }
        check( live <= 1 );                                                         // memory never grows from held views
        check( region->descriptors_free == descriptors );
        check( !rn_region_exhausted( region ) );
    }

#if ROCKETNET_ASSERTS
    check( mo_asserts == 3 * (int) region->window - 1 );
#else
    check( mo_asserts == 0 );
#endif // #if ROCKETNET_ASSERTS
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED ) == (uint64_t) ( 3 * region->window - 1 ) );
    rocketnet_assert_function( saved_assert );

    mo_pair_teardown( &pair );
}

// A code 1 in-place start reserves the record shape's header (finding 8), and
// release finds every held view after receive, out of order and across the
// scan's wrap (finding 12).

static void test_message_release_after_receive_and_in_place_header()
{
    rocketnet_message_config_t config;
    mo_config( &config );
    config.num_channels = 4;
    config.reliable_messages_in_flight = 64;
    config.unreliable_messages_per_update = 64;
    config.packets_per_update = 64;

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config, 20000, 20000 );
    mo_step( &pair );
    mo_step( &pair );

    // ROCKETNET_MAX_CHANNELS is 32, so the start is reached with the object's
    // channel count and the send table widened for the start alone; the start
    // is then aborted before the channel counters are read

    rn_message_connection_t * connection = mo_client_connection( pair.clients[0] );
    std::vector<rn_channel_send_t> wide_send( 512 );

    for ( size_t i = 0; i < wide_send.size(); i++ )
    {
        memset( &wide_send[i], 0, sizeof( wide_send[i] ) );
        wide_send[i].queue_head = RN_DESCRIPTOR_NONE;
        wide_send[i].queue_tail = RN_DESCRIPTOR_NONE;
    }

    rn_channel_send_t * saved_send = connection->region->channel_send;
    connection->region->channel_send = wide_send.data();

    const int channels[2] = { 255, 256 };

    for ( int c = 0; c < 2; c++ )
    {
        struct rocketnet_message_record_t shape = {};
        shape.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
        shape.channel = (uint32_t) channels[c];

        const int header = rocketnet_message_record_header_bytes( &shape );
        check( header == ( channels[c] >= 256 ? 11 : 10 ) );

        pair.clients[0]->object.config.num_channels = channels[c] + 1;

        uint8_t * p = rocketnet_message_client_start( pair.clients[0], channels[c], ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 128 );
        pair.clients[0]->object.config.num_channels = 4;
        check( p != NULL );
        check( connection->pending_header == header );
        check( connection->pending_channel == channels[c] );
        if ( pair.clients[0]->object.in_place == 2 )
            check( p == connection->unreliable_build.writer.payload + connection->unreliable_build.writer.bytes + header );
        else
            check( pair.clients[0]->object.in_place == 1 && p == pair.clients[0]->object.in_place_payload + header );
        check( rocketnet_message_client_finish( pair.clients[0], 0 ) == 0 );
    }

    connection->region->channel_send = saved_send;

    const int messages = 64;
    for ( int m = 0; m < messages; m++ )
    {
        uint8_t * q = rocketnet_message_client_start( pair.clients[0], 1, ROCKETNET_RELIABILITY_UNRELIABLE, 32 );
        check( q != NULL );
        mo_fill( q, (uint32_t) m, 32 );
        check( rocketnet_message_client_finish( pair.clients[0], 32 ) != 0 );
    }

    rocketnet_message_client_send_packets( pair.clients[0] );

    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

    std::vector<const uint8_t *> views;

    for ( ;; )
    {
        const uint8_t * data = NULL;
        int bytes = 0;
        if ( !rn_message_object_receive( &pair.server->object, NULL, NULL, &data, &bytes ) )
            break;
        check( data != NULL && bytes == 32 );
        views.push_back( data );
    }

    check( (int) views.size() == messages );

    rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
    check( slot->hot->held == (uint32_t) messages );

    const uint64_t misuse_before = rocketnet_api_misuse_count();

    // first, then last behind a forced cursor (the wrap), then middle

    rocketnet_message_server_release( pair.server, views[0] );
    check( slot->hot->held == (uint32_t) messages - 1 );

    const uint32_t cursor = slot->hot->receive_cursor;
    slot->hot->receive_cursor = (uint32_t) ( messages / 2 );
    rocketnet_message_server_release( pair.server, views[(size_t) ( messages - 1 )] );
    check( slot->hot->held == (uint32_t) messages - 2 );
    rocketnet_message_server_release( pair.server, views[(size_t) ( messages / 2 )] );
    check( slot->hot->held == (uint32_t) messages - 3 );
    slot->hot->receive_cursor = cursor;

    for ( int m = messages - 2; m > 0; m-- )
    {
        if ( m == messages / 2 )
            continue;

        const uint64_t before = rocketnet_api_misuse_count();
        rocketnet_message_server_release( pair.server, views[(size_t) m] );
        check( rocketnet_api_misuse_count() == before );
    }

    check( slot->hot->held == 0 );
    check( rocketnet_api_misuse_count() == misuse_before );

    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
    mo_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_message_release_after_receive_and_in_place_header );

static void test_receive_cursor_single_then_batch_across_connections()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 2 );
    mo_step( &pair );
    mo_step( &pair );

    for ( int slot = 0; slot < 2; slot++ )
    {
        for ( int message = 0; message < 3; message++ )
        {
            uint8_t bytes[32];
            mo_fill( bytes, (uint32_t) ( 100 * ( slot + 1 ) + message ), sizeof( bytes ) );
            check( rocketnet_message_client_send( pair.clients[slot], message & 1,
                                                  ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                                  bytes, (int) sizeof( bytes ) ) != 0 );
        }

        rocketnet_message_client_send_packets( pair.clients[slot] );
    }

    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

    const uint8_t * first = NULL;
    int first_slot = -1, first_channel = -1, first_bytes = 0;
    check( rn_message_object_receive( &pair.server->object, &first_slot, &first_channel, &first, &first_bytes ) == 1 );
    check( first_slot == 0 && first_bytes == 32 );
    std::vector<uint8_t> first_copy( first, first + first_bytes );

    uint32_t first_id = 0;
    check( mo_verify( first_copy, &first_id ) );
    check( first_id >= 100 && first_id <= 102 );
    check( first_channel == (int) ( first_id & 1 ) );
    check( mo_server_slot( &pair, 0 )->hot->receive_cursor == 1 );

    static rocketnet_message_batch_t batch;
    check( rocketnet_message_server_receive_messages( pair.server, &batch ) == 5 );

    bool seen[2][3] = {};
    seen[0][first_id - 100] = true;

    for ( int i = 0; i < batch.num_messages; i++ )
    {
        check( batch.client_index[i] == ( i < 2 ? 0 : 1 ) );
        std::vector<uint8_t> received( batch.data[i], batch.data[i] + batch.bytes[i] );

        uint32_t id = 0;
        check( mo_verify( received, &id ) );

        const int slot = batch.client_index[i];
        const uint32_t base = (uint32_t) ( 100 * ( slot + 1 ) );
        check( id >= base && id < base + 3 );
        check( batch.channel[i] == (int) ( id & 1 ) );
        check( !seen[slot][id - base] );
        seen[slot][id - base] = true;
    }

    for ( int slot = 0; slot < 2; slot++ )
        for ( int message = 0; message < 3; message++ )
            check( seen[slot][message] );
    check( mo_server_slot( &pair, 0 )->hot->receive_cursor == 3 );
    check( mo_server_slot( &pair, 1 )->hot->receive_cursor == 3 );

    rocketnet_message_server_release( pair.server, first );
    check( rocketnet_message_server_receive_messages( pair.server, &batch ) == 0 );
    check( mo_server_slot( &pair, 0 )->hot->held == 0 );
    check( mo_server_slot( &pair, 1 )->hot->held == 0 );

    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
    check( mo_server_slot( &pair, 0 )->hot->receive_cursor == 0 );
    check( mo_server_slot( &pair, 1 )->hot->receive_cursor == 0 );
    mo_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_receive_cursor_scalar_layout_and_exact_blocks );
REGISTER_UNIT_TEST( test_receive_cursor_single_then_batch_across_connections );

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

    uint32_t loss_random = 0x12345678u; // fixed seed: reproducible 1/7 random loss

    for ( int round = 0; round < 120; round++ )
    {
        // loss: every fifth round drops everything one way
        pair.net->drop_to_server = ( round % 5 ) == 2;

        // randomize the 1/7 loss phase so it cannot lock to a fixed resend delay

        loss_random ^= loss_random << 13;
        loss_random ^= loss_random >> 17;
        loss_random ^= loss_random << 5;
        pair.net->drop_to_client = ( loss_random % 7 ) == 0;

        if ( round < 60 )
        {
            for ( int c = 0; c < 2; c++ )
            {
                const int channel = ( round + c ) % 4;
                const uint32_t bytes = sizes[( round + c ) % num_sizes];
                const uint32_t id = next_id++;

                // the client writes in place

                uint8_t * p = rocketnet_message_client_start( pair.clients[c], channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) bytes );
                check( p );
                if ( bytes >= 4 )
                    mo_fill( p, id, bytes );
                else
                    memset( p, 0xAB, bytes );
                check( rocketnet_message_client_finish( pair.clients[c], (int) bytes ) != 0 );
                if ( bytes >= 4 )
                {
                    sent_ordered[c][channel].push_back( id );
                }

                // the server sends down

                const uint32_t down_id = next_id++;
                uint8_t * q = rocketnet_message_server_start( pair.server, c, channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 600 );
                check( q );
                mo_fill( q, down_id, 600 );
                check( rocketnet_message_server_finish( pair.server, c, 600 ) != 0 );

                // and an unreliable one

                uint8_t u[100];
                mo_fill( u, 900000 + next_id++, sizeof( u ) );
                check( rocketnet_message_client_send( pair.clients[c], channel, ROCKETNET_RELIABILITY_UNRELIABLE, u, (int) sizeof( u ) ) != 0 );
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
                uint8_t * q = rocketnet_message_server_start( pair.server, c, channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) bytes );
                check( q );
                mo_fill( q, id, bytes );
                check( rocketnet_message_server_finish( pair.server, c, (int) bytes ) != 0 );
                sent[c][channel].push_back( id );
            }
        pair.time += 0.02;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
        mo_drain_server( &pair, NULL );

        for ( int c = 0; c < 2; c++ )
        {
            check( rocketnet_message_client_update( pair.clients[c], pair.time ) == ROCKETNET_UPDATE_OK );

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
        {
            rocketnet_message_client_send_packets( pair.clients[c] );
        }
    }

    for ( int c = 0; c < 2; c++ )
        for ( int channel = 0; channel < 4; channel++ )
            check( got[c][channel] == sent[c][channel] );

    mo_pair_teardown( &pair );
}

static void test_ordered_and_unreliable_end_to_end_in_memory()
{
    mo_end_to_end_up();
    mo_ordered_down_in_order();
}
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// R5: a server whose per-client memory is exhausted kicks that client with
// MESSAGE_MEMORY_EXHAUSTED; the other clients carry on untouched
// ---------------------------------------------------------------------------

static void mo_small_config( rocketnet_message_config_t * config )
{
    mo_config( config );
    config->max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config->send_message_buffer_bytes = 12288;
    config->send_queue_messages = 16;
}

#if ROCKETNET_DEV
static void test_server_kicks_exhausted_client_others_unaffected()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // deliberate fixed-pool exhaustion

    mo_pair_t pair;
    mo_pair_setup( &pair, 2, &config, 0, 0 );
    mo_step( &pair );

    // the server queues to slot 0 without sending until its region has no room

    int started = 0;

    for ( ;; )
    {
        uint8_t * q = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 );
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
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( rocketnet_message_server_client_connected( pair.server, 0 ) == 0 );
    check( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 ) == NULL );
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
            check( rocketnet_message_client_send( pair.clients[1], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
            check( rocketnet_message_server_send( pair.server, 1, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
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
    check( rocketnet_message_server_disconnect_reason( pair.server, 1 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( pair.net->server->disconnects == 1 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R5: a client whose own memory is exhausted disconnects itself with the same
// reason; nothing else follows from exhaustion
// ---------------------------------------------------------------------------

static void test_client_disconnects_itself_on_exhaustion()
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
        uint8_t * p = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 900 );
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
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( mo_counter( rocketnet_message_client_counters( client ), ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 1 );

    // nothing more: no start, no send, no partial packet

    check( rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 10 ) == NULL );

    const int finishes = pair.net->clients[0]->finishes;
    pair.time += 0.01;
    check( rocketnet_message_client_update( client, pair.time ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_client_send_packets( client );
    check( pair.net->clients[0]->finishes == finishes );
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );

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

static void test_slot_reuse_starts_clean()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    config.server_send_kbps = 7000;                                   // R6: a reused slot starts at the config field

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0 );
    mo_step( &pair );

    rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
    const uint64_t carves = slot->region->carves;
    check( slot->budget.kbps == 7000 );

    // traffic, a runtime budget, then exhaustion

    for ( int i = 0; i < 5; i++ )
    {
        uint8_t message[100];
        mo_fill( message, (uint32_t) i, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        mo_step( &pair );
    }

    mo_step( &pair );
    check( slot->region->channel_receive[0].ordered_read == 5 );
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, 500 ) == 1 );
    check( slot->budget.kbps == 500 );
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, -1 ) == 1 );
    check( slot->send_budget_enabled == 0 );
    while ( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 ) )
    {
        check( rocketnet_message_server_finish( pair.server, 0, 100 ) != 0 );
    }
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );

    // the next update frees the slot in O(1); the region is carved lazily,
    // when the slot is bound to its next connection (RNSlots ResetFree,
    // Observe), so the free slot's region is not carved yet

    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
    check( slot->hot->state == RN_MESSAGE_CONNECTION_FREE );
    check( slot->region->carves == carves );

    // a new client takes the slot

    delete pair.net->clients[0];
    pair.net->clients[0] = NULL;
    rocketnet_message_client_destroy( pair.clients[0] );
    pair.clients[0] = mo_client_create( pair.net, 0, &config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( pair.clients[0] );
    mo_net_connect( pair.net, 0 );
    mo_step( &pair );

    check( slot->hot->state == RN_MESSAGE_CONNECTION_CONNECTED );
    check( slot->region->carves == carves + 1 );                        // carved once, at the reuse
    check( !rn_region_exhausted( slot->region ) );
    check( slot->hot->connection_id == pair.net->slot_connection[0] );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( slot->region->descriptors_free == slot->region->queue - 1 );          // the new connection's CONFIG, in flight (10.3)
    check( slot->region->channel_receive[0].ordered_read == 0 );
    check( slot->region->channel_send[0].ordered_write == 0 );
    check( slot->budget.kbps == 7000 );                                // not the setter's -1 nor the default
    check( slot->send_budget_enabled == 1 );
    check( slot->hot->held == 0 && slot->unreliable_count == 0 && slot->serial == 0 );
    mo_step( &pair );
    mo_step( &pair );
    check( slot->region->descriptors_free == slot->region->queue );              // CONFIG acked: every descriptor is back (10.3)
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        check( rn_send_store_free_chunks( &slot->region->store, c ) == rn_send_store_total_chunks( &slot->region->store, c ) );
    }

    // and the new connection's first message is ordering 0, delivered

    uint8_t message[100];
    mo_fill( message, 77, sizeof( message ) );
    check( rocketnet_message_client_send( pair.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );

    std::vector<mo_received_t> got;
    mo_step( &pair );
    mo_step( &pair, &got );
    check( got.size() == 1 );

    uint32_t id = 0;
    check( mo_verify( got[0].bytes, &id ) && id == 77 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// RNSlots ResetFree and Observe, the lazy reset: the update a server slot's
// connection ends in frees it in O(1) and walks none of its region; the
// region is carved when the slot is bound to its next connection, and from
// that update on the reused slot's region is byte for byte the region of a
// slot that never had a connection. Nothing of the previous connection
// crosses the reuse.
// ---------------------------------------------------------------------------

static bool mo_region_bookkeeping_equal( const rn_region_t * a, const rn_region_t * b )
{
    if ( a->bytes != b->bytes || a->num_channels != b->num_channels || a->window != b->window || a->queue != b->queue )
        return false;
    if ( a->exhausted != b->exhausted || a->exhausted_reason != b->exhausted_reason || a->send_room_armed != b->send_room_armed )
        return false;
    if ( a->descriptors_free_top != b->descriptors_free_top || a->descriptors_free != b->descriptors_free || a->descriptors_fresh != b->descriptors_fresh )
        return false;
    if ( a->span_head != b->span_head || a->span_tail != b->span_tail || a->span_first != b->span_first || a->span_last != b->span_last )
        return false;
    if ( memcmp( a->channel_send, b->channel_send, sizeof( rn_channel_send_t ) * a->num_channels ) != 0 )
        return false;
    if ( memcmp( a->channel_receive, b->channel_receive, sizeof( rn_channel_receive_t ) * a->num_channels ) != 0 )
        return false;
    if ( memcmp( a->send_numbers, b->send_numbers, sizeof( rn_send_number_t ) * a->window ) != 0 )
        return false;
    if ( memcmp( a->receive_descriptors, b->receive_descriptors, sizeof( rn_receive_descriptor_t ) * a->window ) != 0 )
        return false;
    if ( memcmp( a->received_bits, b->received_bits, a->window / 8 ) != 0 )
        return false;
    if ( memcmp( a->held_index, b->held_index, sizeof( rn_held_entry_t ) * a->held_entries ) != 0 )
        return false;
    if ( memcmp( a->send_descriptors, b->send_descriptors, sizeof( rn_send_descriptor_t ) * a->descriptors_fresh ) != 0 )
        return false;
    if ( memcmp( a->sent_packets, b->sent_packets, sizeof( rn_sent_packet_t ) * a->packets ) != 0 )
        return false;
    if ( memcmp( a->sent_index, b->sent_index, sizeof( rn_sent_index_t ) * a->sent_index_entries ) != 0 )
        return false;
    if ( memcmp( a->sent_refs, b->sent_refs, sizeof( uint32_t ) * a->sent_ref_entries ) != 0 )
        return false;
    if ( memcmp( a->deliveries, b->deliveries, sizeof( rn_delivery_t ) * a->delivery_entries ) != 0 )
        return false;
    if ( memcmp( a->receipts, b->receipts, sizeof( rn_receipt_t ) * a->receipt_entries ) != 0 )
        return false;
    if ( memcmp( a->resend_fifo, b->resend_fifo, sizeof( rn_resend_link_t ) * a->window ) != 0 )
        return false;

    for ( uint32_t h = 0; h < a->holds; h++ )
    {
        const rn_sequenced_hold_t * x = &a->sequenced_messages_held[h];
        const rn_sequenced_hold_t * y = &b->sequenced_messages_held[h];
        if ( x->bytes != y->bytes || x->ordering != y->ordering || x->sequencing != y->sequencing || x->channel != y->channel || x->used != y->used || x->code != y->code )
            return false;
    }

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const rn_store_class_t * x = &a->store.classes[c];
        const rn_store_class_t * y = &b->store.classes[c];
        if ( x->num_chunks != y->num_chunks || x->free_head != y->free_head || x->free_count != y->free_count || x->fresh != y->fresh )
            return false;
    }

    return true;
}

static void test_lazy_slot_reset_nothing_crosses_reuse()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );
    mo_step( &pair );

    // the previous connection on slot 0: messages both ways, and the
    // server's last sends left unacked

    for ( int i = 0; i < 5; i++ )
    {
        uint8_t message[100];
        mo_fill( message, (uint32_t) i, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        check( rocketnet_message_server_send( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        mo_step( &pair );
    }

    pair.net->hold_acks = true;

    for ( int i = 0; i < 3; i++ )
    {
        uint8_t message[200];
        mo_fill( message, 50 + (uint32_t) i, sizeof( message ) );
        check( rocketnet_message_server_send( pair.server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
    }

    mo_step( &pair );
    pair.net->hold_acks = false;

    rn_message_connection_t * reused = mo_server_slot( &pair, 0 );
    rn_message_connection_t * fresh = mo_server_slot( &pair, 1 );         // never had a connection
    check( reused->region->channel_receive[0].ordered_read == 5 );
    check( reused->region->descriptors_free < reused->region->queue );
    check( !mo_region_bookkeeping_equal( reused->region, fresh->region ) );

    // the connection ends: the update frees the slot and carves nothing

    const uint64_t carves = reused->region->carves;
    mo_net_drop_slot( pair.net, 0 );
    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_server_send_packets( pair.server );
    check( reused->hot->state == RN_MESSAGE_CONNECTION_FREE );
    check( reused->hot->connection_id == 0 && reused->hot->held == 0 );
    check( reused->region->carves == carves );
    check( reused->region->channel_receive[0].ordered_read == 5 );     // not walked: carved at reuse
    check( !rocketnet_message_server_client_connected( pair.server, 0 ) );
    check( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 10 ) == NULL );

    // a new client on slot 0 and a first client on slot 1, bound in one
    // update: after it the reused region equals the region that never had a
    // connection, the CONFIG of each new connection queued alike

    rocketnet_message_client_destroy( pair.clients[0] );
    delete pair.net->clients[0];
    pair.net->clients[0] = NULL;
    pair.clients[0] = mo_client_create( pair.net, 0, &pair.server->object.config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    pair.clients[1] = mo_client_create( pair.net, 1, &pair.server->object.config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( pair.clients[0] && pair.clients[1] );
    pair.num_clients = 2;
    mo_net_connect( pair.net, 0 );
    mo_net_connect( pair.net, 1 );
    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
    check( reused->hot->state == RN_MESSAGE_CONNECTION_CONNECTED );
    check( fresh->hot->state == RN_MESSAGE_CONNECTION_CONNECTED );
    check( reused->region->carves == carves + 1 );                     // carved once, at the reuse
    check( mo_region_bookkeeping_equal( reused->region, fresh->region ) );
    check( reused->hot->held == fresh->hot->held );
    check( reused->serial == fresh->serial );
    check( reused->send_error == fresh->send_error );
    check( reused->budget.kbps == fresh->budget.kbps );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    rocketnet_message_server_send_packets( pair.server );

    // and the new connection's first message is ordering 0 on both slots

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t message[100];
        mo_fill( message, 90 + (uint32_t) i, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
    }

    std::vector<mo_received_t> got;
    mo_step( &pair );
    mo_step( &pair, &got );
    mo_step( &pair, &got );
    check( got.size() == 2 );

    for ( size_t k = 0; k < got.size(); k++ )
    {
        uint32_t id = 0;
        check( mo_verify( got[k].bytes, &id ) && id == 90 + (uint32_t) got[k].slot );
    }

    check( reused->region->channel_receive[0].ordered_read == 1 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R1: only the transport's acked packet ids mark a message delivered
// ---------------------------------------------------------------------------

static void test_acks_only_from_transport_acked_packets()
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
    check( rocketnet_message_client_send( pair.clients[0], 3, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );

    int delivered = 0;

    for ( int i = 0; i < 60; i++ )
    {
        std::vector<mo_received_t> got;
        mo_step( &pair, &got, NULL, 0.05 );
        delivered += (int) got.size();
    }

    check( delivered == 1 );                                                       // once, however often it arrives
    check( region->descriptors_free == region->queue - 2 );                        // still in flight: no ack, no release (this message and the connection's CONFIG, 10.3)
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );          // resent on its resend delay
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) > 0 );

    // a delivered payload is not an ack, nor is any other event: the record
    // goes only when the transport reports a packet id that carried it

    pair.net->hold_acks = false;
    mo_step( &pair, NULL, NULL, 0.05 );
    mo_step( &pair, NULL, NULL, 0.05 );
    check( region->descriptors_free == region->queue );

    // an acked id this connection never sent changes nothing

    mo_endpoint_t * e = pair.net->clients[0];
    check( rocketnet_message_client_send( pair.clients[0], 3, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
    pair.net->drop_to_server = true;
    mo_step( &pair );
    check( region->descriptors_free == region->queue - 1 );
    e->acks_inbox.push_back( 0xDEADBEEFull );
    mo_step( &pair );
    check( region->descriptors_free == region->queue - 1 );
    pair.net->drop_to_server = false;
    for ( int i = 0; i < 20; i++ )
    {
        mo_step( &pair, NULL, NULL, 0.05 );
    }
    check( region->descriptors_free == region->queue );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R6: the send budget, in kbps, set at create and at runtime
// ---------------------------------------------------------------------------

static void test_budget_set_at_create_and_runtime()
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
    mo_pair_setup( &pair, 1, NULL, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
    check( mo_client_connection( pair.clients[0] )->budget.kbps == ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( mo_server_slot( &pair, 0 )->budget.kbps == ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
    mo_step( &pair );

    // at runtime: a low rate. Reliable messages wait for budget and are all
    // delivered; unreliable ones over budget are dropped and counted

    rocketnet_message_client_t * client = pair.clients[0];
    check( rocketnet_message_client_set_send_kbps( client, 40 ) == 1 );            // 5000 bytes a second
    check( mo_client_connection( client )->budget.kbps == 40 );
    check( rocketnet_message_client_set_send_kbps( client, 0 ) == 1 );             // 0 uncaps
    check( mo_client_connection( client )->send_budget_enabled == 0 );
    check( rocketnet_message_client_set_send_kbps( client, 40 ) == 1 );            // cap again for this case
    check( mo_client_connection( client )->budget.kbps == 40 );

    std::vector<uint32_t> sent, got;
    const int rounds = 100;
    for ( int round = 0; round < rounds; round++ )
    {
        if ( round < 20 )
        {
            uint8_t message[1000];
            mo_fill( message, (uint32_t) round + 1, sizeof( message ) );
            check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
            sent.push_back( (uint32_t) round + 1 );

            uint8_t u[1000];
            mo_fill( u, 900000 + (uint32_t) round, sizeof( u ) );
            check( rocketnet_message_client_send( client, 1, ROCKETNET_RELIABILITY_UNRELIABLE, u, (int) sizeof( u ) ) );
        }

        std::vector<mo_received_t> server_got;
        mo_step( &pair, &server_got, NULL, 0.05 );

        for ( size_t i = 0; i < server_got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( server_got[i].bytes, &id ) );
            if ( id < 900000 )
            {
                got.push_back( id );
            }
        }
    }

    check( got == sent );                                                          // never dropped for budget

    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) > 0 );

    // the wire stayed under the rate: 40 kbps over the run, plus the bucket's depth

    uint64_t wire = 0;
    const mo_endpoint_t * e = pair.net->clients[0];
    for ( size_t i = 0; i < e->sent_log.size(); i++ )
    {
        wire += (uint64_t) rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, e->sent_log[i].bytes );
    }

    const double seconds = rounds * 0.05 + 0.01;
    check( (double) wire <= 5000.0 * seconds + rocketnet_send_budget_depth_bytes( 1000 ) + rocketnet_send_budget_depth_bytes( 40 ) );

    // raised at runtime: the rate takes effect from now on

    check( rocketnet_message_client_set_send_kbps( client, 10000 ) == 1 );

    const uint64_t dropped = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );

    for ( int round = 0; round < 10; round++ )
    {
        uint8_t u[1000];
        mo_fill( u, 950000 + (uint32_t) round, sizeof( u ) );
        check( rocketnet_message_client_send( client, 1, ROCKETNET_RELIABILITY_UNRELIABLE, u, (int) sizeof( u ) ) );
        mo_step( &pair, NULL, NULL, 0.05 );
    }

    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == dropped );

    // and the server's per-client rate, at runtime

    check( rocketnet_message_server_set_send_kbps( pair.server, 0, 777 ) == 1 );
    check( mo_server_slot( &pair, 0 )->budget.kbps == 777 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R6: the send budget is a configured default, and 0 or lower disables it
// (docs/CHANNELS.md 11.5; proofs/tla/RNSendBudget.tla)
// ---------------------------------------------------------------------------

static void test_send_budget_defaults_from_config()
{
    // a NULL config and a default config both give the owner's defaults: the
    // client 1000 kbps up, every server slot 10000 kbps down. The create
    // argument 0 takes the config field.
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, NULL, 0, 0 );
        check( mo_client_connection( pair.clients[0] )->budget.kbps == 1000 );
        check( mo_client_connection( pair.clients[0] )->send_budget_enabled == 1 );
        check( mo_server_slot( &pair, 0 )->budget.kbps == 10000 );
        check( mo_server_slot( &pair, 0 )->send_budget_enabled == 1 );
        mo_step( &pair );
        check( mo_server_slot( &pair, 0 )->budget.kbps == 10000 );
        mo_pair_teardown( &pair );
    }

    {
        rocketnet_message_config_t config;
        rocketnet_message_default_config( &config );
        check( config.client_send_kbps == 1000 );
        check( config.server_send_kbps == 10000 );

        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config, 0, 0 );
        check( mo_client_connection( pair.clients[0] )->budget.kbps == 1000 );
        check( mo_server_slot( &pair, 0 )->budget.kbps == 10000 );
        check( mo_server_slot( &pair, 1 )->budget.kbps == 10000 );
        mo_pair_teardown( &pair );
    }
}

static void test_send_budget_configured_value_honoured()
{
    rocketnet_message_config_t config;
    mo_config( &config );
    config.client_send_kbps = 250;
    config.server_send_kbps = 4000;

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config, 0, 0 );
    check( mo_client_connection( pair.clients[0] )->budget.kbps == 250 );
    check( mo_server_slot( &pair, 0 )->budget.kbps == 4000 );
    mo_step( &pair );

    // a burst at 250 kbps (31250 bytes a second, the bucket depth one packet)
    // over budget: unreliable sends are dropped and counted, reliable ones
    // wait and are never dropped

    rocketnet_message_client_t * client = pair.clients[0];
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );
    const uint64_t drops_before = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );

    std::vector<uint32_t> reliable_sent;

    for ( int i = 0; i < 5; i++ )
    {
        uint8_t message[1000];
        mo_fill( message, (uint32_t) i + 1, sizeof( message ) );
        check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        reliable_sent.push_back( (uint32_t) i + 1 );
    }

    for ( int i = 0; i < 8; i++ )
    {
        uint8_t u[1000];
        mo_fill( u, 900000 + (uint32_t) i, sizeof( u ) );
        check( rocketnet_message_client_send( client, 1, ROCKETNET_RELIABILITY_UNRELIABLE, u, (int) sizeof( u ) ) );
    }

    std::vector<mo_received_t> got;
    mo_step( &pair, &got, NULL, 0.01 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) > drops_before );

    int reliable_received = 0;

    for ( size_t i = 0; i < got.size(); i++ )
    {
        uint32_t id = 0;
        check( mo_verify( got[i].bytes, &id ) );
        if ( id < 900000 )
        {
            reliable_received++;
        }
    }

    check( reliable_received < (int) reliable_sent.size() );           // some reliable send waits for budget

    for ( int round = 0; round < 400 && reliable_received < (int) reliable_sent.size(); round++ )
    {
        got.clear();
        mo_step( &pair, &got, NULL, 0.01 );

        for ( size_t i = 0; i < got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( got[i].bytes, &id ) );
            if ( id < 900000 )
            {
                reliable_received++;
            }
        }
    }

    check( reliable_received == (int) reliable_sent.size() );          // never dropped for budget

    mo_pair_teardown( &pair );
}

static void mo_uncapped_case( int client_kbps, int server_kbps )
{
    rocketnet_message_config_t config;
    mo_config( &config );
    config.client_send_kbps = client_kbps;
    config.server_send_kbps = server_kbps;

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config, 0, 0 );
    check( mo_client_connection( pair.clients[0] )->send_budget_enabled == 0 );
    check( mo_server_slot( &pair, 0 )->send_budget_enabled == 0 );
    check( rocketnet_message_client_counters( pair.clients[0] ) != NULL );
    mo_step( &pair );                                                  // bind the server's slot

    // simulated time starts at 1000.0; one update per millisecond, and eight
    // packets per update above the 10 Mbit/s equivalent in both directions

    const int rounds = 200;
    std::vector<uint32_t> sent;
    int reliable_received = 0;
    int reliable_received_by_client = 0;

    for ( int round = 0; round < rounds; round++ )
    {
        uint8_t message[1000];
        mo_fill( message, (uint32_t) round + 1, sizeof( message ) );
        check( rocketnet_message_client_send( pair.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        sent.push_back( (uint32_t) round + 1 );
        check( rocketnet_message_server_send( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );

        for ( int i = 0; i < 7; i++ )
        {
            uint8_t u[1000];
            mo_fill( u, 900000 + (uint32_t) ( round * 10 + i ), sizeof( u ) );
            check( rocketnet_message_client_send( pair.clients[0], 1, ROCKETNET_RELIABILITY_UNRELIABLE, u, (int) sizeof( u ) ) );
            check( rocketnet_message_server_send( pair.server, 0, 1, ROCKETNET_RELIABILITY_UNRELIABLE, u, (int) sizeof( u ) ) );
        }

        std::vector<mo_received_t> server_got, client_got;
        mo_step( &pair, &server_got, &client_got, 0.001 );

        for ( size_t i = 0; i < server_got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( server_got[i].bytes, &id ) );
            if ( id < 800000 )
            {
                reliable_received++;
            }
        }

        for ( size_t i = 0; i < client_got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( client_got[i].bytes, &id ) );
            if ( id < 800000 )
            {
                reliable_received_by_client++;
            }
        }
    }

    // drain the last reliable sends of the run

    for ( int extra = 0; extra < 5; extra++ )
    {
        std::vector<mo_received_t> server_got, client_got;
        mo_step( &pair, &server_got, &client_got, 0.001 );

        for ( size_t i = 0; i < server_got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( server_got[i].bytes, &id ) );
            if ( id < 800000 )
            {
                reliable_received++;
            }
        }

        for ( size_t i = 0; i < client_got.size(); i++ )
        {
            uint32_t id = 0;
            check( mo_verify( client_got[i].bytes, &id ) );
            if ( id < 800000 )
            {
                reliable_received_by_client++;
            }
        }
    }

    // no unreliable drop and no reliable wait by the budget in either direction

    check( mo_counter( rocketnet_message_client_counters( pair.clients[0] ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == 0 );
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == 0 );
    check( reliable_received == rounds );                              // every reliable client message arrived
    check( reliable_received_by_client == rounds );                    // every reliable server message arrived
    check( mo_client_connection( pair.clients[0] )->budget.reliable_waiting == 0 );
    check( mo_server_slot( &pair, 0 )->budget.reliable_waiting == 0 );
    check( mo_client_connection( pair.clients[0] )->budget.refusals == 0 );   // the bucket is never touched
    check( mo_server_slot( &pair, 0 )->budget.refusals == 0 );
    check( mo_client_connection( pair.clients[0] )->budget.bytes_spent == 0 );
    check( mo_server_slot( &pair, 0 )->budget.bytes_spent == 0 );

    // well over 10 Mbit/s of wire bytes in the simulated second, each direction

    uint64_t wire = 0;
    for ( size_t i = 0; i < pair.net->clients[0]->sent_log.size(); i++ )
    {
        wire += (uint64_t) rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, pair.net->clients[0]->sent_log[i].bytes );
    }
    check( (double) wire > 10.0e6 / 8.0 * ( rounds * 0.001 ) );

    uint64_t server_wire = 0;
    for ( size_t i = 0; i < pair.net->server->sent_log.size(); i++ )
    {
        server_wire += (uint64_t) rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, pair.net->server->sent_log[i].bytes );
    }
    check( (double) server_wire > 10.0e6 / 8.0 * ( rounds * 0.001 ) );

    mo_pair_teardown( &pair );
}

static void test_send_budget_zero_or_negative_is_uncapped()
{
    mo_uncapped_case( 0, 0 );
    mo_uncapped_case( -1, -1 );
}

static void test_set_send_kbps_zero_or_negative_uncaps()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    rn_message_connection_t * connection = mo_client_connection( client );

    check( rocketnet_message_client_set_send_kbps( client, 0 ) == 1 );
    check( connection->send_budget_enabled == 0 );
    check( rocketnet_message_client_set_send_kbps( client, -1 ) == 1 );
    check( connection->send_budget_enabled == 0 );
    check( rocketnet_message_client_set_send_kbps( client, 500 ) == 1 );
    check( connection->send_budget_enabled == 1 && connection->budget.kbps == 500 );
    check( rocketnet_message_client_set_send_kbps( client, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == 0 );
    check( connection->budget.kbps == 500 );

    rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, 0 ) == 1 );
    check( slot->send_budget_enabled == 0 );
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, -1 ) == 1 );
    check( slot->send_budget_enabled == 0 );
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, 777 ) == 1 );
    check( slot->send_budget_enabled == 1 && slot->budget.kbps == 777 );
    check( rocketnet_message_server_set_send_kbps( pair.server, 0, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == 0 );
    check( slot->budget.kbps == 777 );

    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// R2: a counting allocator sees create's one allocation and then nothing.
// Registered as test_message_objects_no_allocation_after_create: four other unit
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

static void test_message_objects_no_allocation_after_create()
{
    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, mo_malloc, mo_free ) == ROCKETNET_STATUS_OK );

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
        rocketnet_message_client_t * in_block = mo_client_create( net, 0, &config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, block, bytes );
        check( in_block != NULL );
        check( (void *) in_block == block );
        check( mo_mallocs == before_block );
        check( mo_client_create( net, 1, &config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, block, bytes - 64 ) == NULL );        // short: refused
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
                    uint8_t * p = rocketnet_message_client_start( pair.clients[c], round % 4, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, round % 2 ? 3000 : 200 );
                    check( p );
                    mo_fill( p, (uint32_t) round, round % 2 ? 3000 : 200 );
                    check( rocketnet_message_client_finish( pair.clients[c], round % 2 ? 3000 : 200 ) );
                    uint8_t u[64];
                    mo_fill( u, (uint32_t) round, sizeof( u ) );
                    check( rocketnet_message_client_send( pair.clients[c], 0, ROCKETNET_RELIABILITY_UNRELIABLE, u, (int) sizeof( u ) ) );
                    check( rocketnet_message_server_send( pair.server, c, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, u, (int) sizeof( u ) ) );
                    uint8_t * a = rocketnet_message_client_start( pair.clients[c], 2, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 50 );
                    check( a );
                    rocketnet_message_client_abort( pair.clients[c] );
                }
            mo_step( &pair, NULL, NULL, 0.03 );
        }

        // a kick and a reset, still allocation free

        while ( rocketnet_message_server_start( pair.server, 1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000 ) )
        {
            check( rocketnet_message_server_finish( pair.server, 1, 1000 ) );
        }
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
        check( mo_mallocs + mo_frees == calls );

        mo_pair_teardown( &pair );
        mo_net_free( net );
        free( allocation );
    }

    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// proofs/tla/RNSlots.tla's five reversed witnesses, replayed against the objects:
// each mutation's counterexample, step by step, and the property it broke
// holding here
// ---------------------------------------------------------------------------

static void mo_send_up( mo_pair_t * pair, int c, int channel, uint32_t id, int bytes = 100 )
{
    uint8_t message[2000];
    mo_fill( message, id, (uint32_t) bytes );
    check( rocketnet_message_client_send( pair->clients[c], channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, bytes ) );
    rocketnet_message_client_send_packets( pair->clients[c] );
}

static void test_rnslots_witness_replays()
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
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, NULL, NULL, &data, &bytes ) == 0 );
        check( mo_server_slot( &pair, 0 )->hot->connection_id == pair.net->slot_connection[0] );
        check( mo_server_slot( &pair, 0 )->region->channel_receive[0].ordered_read == 0 );
        check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) >= 1 );
        mo_pair_teardown( &pair );
    }

    // omit reset (ReusedSlotStartsClean): A's record delivered, received and
    // released; A disconnects and the slot is freed in O(1), bound to no
    // connection (the lazy reset); B takes the slot, and the Observe that
    // binds it carves the region: B's slot holds none of A's receive state
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
        check( slot->hot->state == RN_MESSAGE_CONNECTION_FREE );
        check( slot->hot->connection_id == 0 );
        check( pair.server->object.slots.connection_id[0] == 0 );
        mo_net_connect( pair.net, 0 );                                              // B
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
        check( slot->hot->state == RN_MESSAGE_CONNECTION_CONNECTED );
        check( slot->hot->connection_id == pair.net->slot_connection[0] );
        check( slot->region->channel_receive[0].ordered_read == 0 );

        uint32_t live = 0;
        for ( uint32_t i = 0; i < slot->region->window; i++ )
        {
            live += slot->region->receive_descriptors[i].live;
        }
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
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

        const uint8_t * first = NULL;
        const uint8_t * second = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, NULL, NULL, &first, &bytes ) == 1 );
        check( rocketnet_message_server_receive( pair.server, NULL, NULL, &second, &bytes ) == 1 );

        rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
        int started = 0;

        while ( rocketnet_message_server_start( pair.server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 ) )
        {
            check( rocketnet_message_server_finish( pair.server, 0, 100 ) );
            started++;
        }

        check( started > 0 );
        check( slot->hot->state == RN_MESSAGE_CONNECTION_KICKED );
        check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
        check( pair.net->slot_connection[0] == 0 );
        check( rocketnet_message_server_client_connected( pair.server, 0 ) == 0 );
        check( rocketnet_message_server_start( pair.server, 0, 1, ROCKETNET_RELIABILITY_UNRELIABLE, 10 ) == NULL );

        const int finishes = pair.net->server->finishes;
        rocketnet_message_server_send_packets( pair.server );
        check( pair.net->server->finishes == finishes );                            // nothing sent from an exhausted region

        rocketnet_message_server_release( pair.server, first );
        rocketnet_message_server_release( pair.server, second );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
        check( slot->hot->state == RN_MESSAGE_CONNECTION_FREE );
        check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );

        // the slot is freed in O(1) and carved when it is bound again (the
        // lazy reset): the next connection starts unexhausted, every
        // descriptor back but its own CONFIG's

        const uint64_t carves = slot->region->carves;
        mo_net_connect( pair.net, 0 );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
        check( slot->hot->state == RN_MESSAGE_CONNECTION_CONNECTED );
        check( slot->region->carves == carves + 1 );
        check( !rn_region_exhausted( slot->region ) );
        check( slot->region->descriptors_free == slot->region->queue - 1 );
        check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
        mo_pair_teardown( &pair );
    }

    // ignore release (EveryReliableDelivered): with release before every
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
            {
                mo_step( &pair, &got );
            }
            check( got.size() == 1 );

            uint32_t got_id = 0;
            check( mo_verify( got[0].bytes, &got_id ) && got_id == id );
            delivered++;
        }

        check( delivered == total );
        check( slot->hot->state == RN_MESSAGE_CONNECTION_CONNECTED );
        check( !rn_region_exhausted( slot->region ) );
        check( !rn_region_exhausted( mo_client_connection( pair.clients[0] )->region ) );
        mo_pair_teardown( &pair );
    }

    // update with held views (UpdateRequiresReleasedViews): a record delivered
    // and held; the next update is refused by name, never run as if released
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config );
        mo_step( &pair );
        mo_send_up( &pair, 0, 0, 1 );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, NULL, NULL, &data, &bytes ) == 1 );
        void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
        rocketnet_assert_function( mo_assert_hook );
        mo_asserts = 0;
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED );
#if ROCKETNET_ASSERTS
        check( mo_asserts == 1 );
#else
        check( mo_asserts == 0 );
#endif // #if ROCKETNET_ASSERTS
        rocketnet_assert_function( saved_assert );
        check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED ) == 1 );

        // the following update, with nothing held, is fine

        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
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

static void test_message_objects_client_clock_starts_at_first_update()
{
    rocketnet_message_config_t config;
    mo_config( &config );

    mo_net_t * net = mo_net_new();

    // seeded at 0.0, the public create's seed

    rn_message_link_t server_link = mo_link( net->server );
    rocketnet_message_server_t * server = (rocketnet_message_server_t *) rn_message_object_create( NULL, &server_link, 1, 2, &config, NULL, 0, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, 0.0 );
    check( server );

    mo_endpoint_t * e = mo_net_client( net, 0 );
    rn_message_link_t client_link = mo_link( e );
    client_link.clock_from_first_update = 1;                    // as rn_message_client_link binds it

    rocketnet_message_client_t * client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &client_link, 0, 1, &config, NULL, 0, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    check( client );
    mo_net_connect( net, 0 );

    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;

    const uint64_t misuse_before = rocketnet_api_misuse_count();
    check( rocketnet_message_client_update( client, (double) NAN ) == ROCKETNET_UPDATE_OK );
    check( !client->object.clock_started );
    check( rocketnet_api_misuse_count() == misuse_before + 1 );
    check( client->object.time == 0.0 );
    check( e->time == 0.0 );
    check( rocketnet_message_client_update( client, (double) INFINITY ) == ROCKETNET_UPDATE_OK );
    check( !client->object.clock_started );
    check( rocketnet_api_misuse_count() == misuse_before + 2 );
    check( client->object.time == 0.0 );
    check( e->time == 0.0 );

    // the public client binds the flag; the server does not
    {
        rn_message_link_t bound;
        rn_message_client_link( &bound, NULL );
        check( bound.clock_from_first_update == 1 );
        rn_message_server_link( &bound, NULL );
        check( bound.clock_from_first_update == 0 );
    }

    double time = MO_T0;
    check( rocketnet_message_server_update( server, time ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_client_update( client, time ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_api_misuse_count() == misuse_before + 2 );
    check( client->object.clock_started );
    check( client->object.clock_origin == MO_T0 );
    check( e->time == 0.0 );                                    // the client transport's first update is its 0.0
    check( net->server->time == MO_T0 );                        // the server transport reads the game's time
    check( client->object.time == MO_T0 );                      // the object's own clock is the game's time
    check( server->object.time == MO_T0 );

    // a reliable message each way still flows, from T=1000.0 on

    uint8_t * up = rocketnet_message_client_start( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 );
    check( up != NULL );
    mo_fill( up, 7, 16 );
    check( rocketnet_message_client_finish( client, 16 ) != 0 );

    uint8_t * down = rocketnet_message_server_start( server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 );
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
        check( rocketnet_message_server_update( server, time ) == ROCKETNET_UPDATE_OK );
        check( rocketnet_message_client_update( client, time ) == ROCKETNET_UPDATE_OK );
        check( e->time == 0.25 * step );                        // elapsed since the first update
        check( net->server->time == time );

        const uint8_t * data = NULL;
        int bytes = 0;
        int delivered_channel = -1;
        while ( rocketnet_message_server_receive( server, NULL, &delivered_channel, &data, &bytes ) )
        {
            check( delivered_channel == 1 );
            std::vector<uint8_t> got( data, data + bytes );

            uint32_t id = 0;
            check( bytes == 16 && mo_verify( got, &id ) && id == 7 );
            server_got = true;
            rocketnet_message_server_release( server, data );
        }

        while ( rocketnet_message_client_receive( client, &delivered_channel, &data, &bytes ) )
        {
            check( delivered_channel == 1 );
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
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_message_server_disconnect_reason( server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );

    rocketnet_assert_function( saved_assert );
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

static void test_unreliable_store_full_exhausts_and_kicks()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    config.send_queue_messages = 1024;                          // the store runs out before the descriptors do

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config );
    mo_step( &pair );

    int started = 0;
    pair.net->refuse_start = true;                              // no frame at start: every send takes the queued path

    for ( ;; )
    {
        uint8_t * p = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 );
        if ( !p )
            break;
        memset( p, 0xAB, 16 );
        check( rocketnet_message_server_finish( pair.server, 0, 16 ) != 0 );
        started++;
        check( started < 100000 );
    }

    pair.net->refuse_start = false;
    check( started > 0 );

    const rocketnet_counters_t * counters = rocketnet_message_server_counters( pair.server );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 1 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 1 );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( rocketnet_message_server_client_connected( pair.server, 0 ) == 0 );
    check( pair.net->server->disconnects == 1 );

    // sticky: the kicked connection takes no further send

    check( rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 ) == NULL );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 1 );

    mo_pair_teardown( &pair );
}

// an unreliable message past the one-packet limit is promoted to its reliable
// twin and counted (the promotion rule); the one at the limit still goes
// as unreliable, and a valid message behind the promotion still arrives

static void test_unreliable_past_limit_promotes_counted_and_not_wedged()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1, NULL, 100000, 100000 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );
    const int limit = (int) client->object.unreliable_limit;
    check( limit > 1000 && limit < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

    static uint8_t message[16384];
    mo_fill( message, 1, (uint32_t) limit );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, limit ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 0 );

    // past the limit, and the largest the config allows: both promoted

    mo_fill( message, 2, (uint32_t) limit + 1 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, limit + 1 ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 1 );
    mo_fill( message, 3, 16384 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, 16384 ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 2 );

    // a promotion is not a refusal or a store-full drop, and the connection stays up

    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // not wedged: a valid message behind the promoted ones is sent

    mo_fill( message, 4, 50 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, 50 ) != 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 2 );

    std::vector<mo_received_t> got;
    for ( int i = 0; i < 512 && got.size() < 4; i++ )
    {
        mo_step( &pair, &got );
    }
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

static void test_unreliable_failed_packet_drops_and_counts_each_record()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( client );

    pair.net->refuse_start = true;                              // no frame at start: the three are queued, one packet

    for ( uint32_t i = 0; i < 3; i++ )
    {
        uint8_t message[16];
        mo_fill( message, 10 + i, sizeof( message ) );
        check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
    }

    pair.net->refuse_start = false;

    pair.net->fail_finish = 1;

    std::vector<mo_received_t> got;
    mo_step( &pair, &got );
    mo_step( &pair, &got );
    check( pair.net->fail_finish == 0 );                        // the failure was taken by the one packet
    check( got.empty() );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 3 );

    // the dropped records are gone for good (unreliable): none arrives later

    for ( int i = 0; i < 4; i++ )
    {
        mo_step( &pair, &got );
    }
    check( got.empty() );

    uint8_t message[16];
    mo_fill( message, 99, sizeof( message ) );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
    for ( int i = 0; i < 3; i++ )
    {
        mo_step( &pair, &got );
    }
    check( got.size() == 1 );

    uint32_t id = 0;
    check( mo_verify( got[0].bytes, &id ) && id == 99 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 3 );

    mo_pair_teardown( &pair );
}

// the send store holds an unreliable message only until its record is written:
// the chunks are back once the packet is built (9.3 step 3, R5), across the
// store's classes

static void test_unreliable_chunks_freed_when_written()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );
    mo_step( &pair );

    rocketnet_message_client_t * client = pair.clients[0];
    rn_send_store_t * store = &mo_client_connection( client )->region->store;

    uint8_t small[64], medium[400];
    mo_fill( small, 20, sizeof( small ) );
    mo_fill( medium, 21, sizeof( medium ) );
    pair.net->refuse_start = true;                              // no frame at start: the queued path holds store chunks
    check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, small, (int) sizeof( small ) ) != 0 );
    check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, medium, (int) sizeof( medium ) ) != 0 );
    pair.net->refuse_start = false;

    uint32_t used = 0;
    for ( int cl = 0; cl < RN_STORE_NUM_CLASSES; cl++ )
    {
        used += rn_send_store_total_chunks( store, cl ) - rn_send_store_free_chunks( store, cl );
    }
    check( used >= 2 );                                         // both messages hold store chunks while queued

    std::vector<mo_received_t> got;
    mo_step( &pair, &got );
    mo_step( &pair, &got );
    check( got.size() == 2 );
    for ( int cl = 0; cl < RN_STORE_NUM_CLASSES; cl++ )
    {
        check( rn_send_store_free_chunks( store, cl ) == rn_send_store_total_chunks( store, cl ) );
    }

    mo_pair_teardown( &pair );
}

// small unreliable messages over a link that loses packets: every message that
// arrives is byte-identical to one sent and arrives once, none is invented, and
// with no loss every message arrives (R8)

static void test_unreliable_over_a_lossy_link_arrives_intact_at_most_once()
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
                check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
                next_id++;
                sent++;
            }

            if ( lossy && round % 3 == 0 )
            {
                pair.net->drop_next_to_server = 1;
            }
            mo_step( &pair, &got );
        }

        for ( int i = 0; i < 4; i++ )
        {
            mo_step( &pair, &got );
        }

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
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// the send room watch: a debug build warns once when the fullest send pool
// crosses 75%, again only after it fell below 50%, and when the region is
// exhausted, just before the kick; a release build says nothing
// ---------------------------------------------------------------------------

// the decision itself, on one region: 16 descriptors are the fullest pool
// for 100-byte sends (the small class holds 31), so the 12th send crosses

static void test_send_room_watch_crosses_once_and_rearms_below_half()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit descriptor headroom

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );
    check( region->queue == 16 );

    uint32_t taken[16];
    int crossings = 0;
    int crossed_at = -1;
    check( rn_region_send_room_watch( region ) == RN_SEND_ROOM_NONE );

    for ( int i = 0; i < 15; i++ )
    {
        uint32_t free_messages = 0;
        check( rn_region_send_fits( region, 100, NULL, NULL, &free_messages ) == 1 );
        check( free_messages == (uint32_t) ( 16 - i ) );
        taken[i] = rn_region_send_alloc( region, 100 );
        check( taken[i] != RN_DESCRIPTOR_NONE );

        if ( rn_region_send_room_watch( region ) == RN_SEND_ROOM_CROSSED )
        {
            crossings++;
            crossed_at = i + 1;
        }
    }

    check( crossings == 1 );                                    // a connection sitting above 75% crosses once
    check( crossed_at == 12 );                                  // 12 of 16 is 75%

    rn_send_room_t room;
    rn_region_send_room( region, &room );
    check( room.used_messages == 15 && room.messages == 16 );
    check( room.permille == 937 );
    check( room.store_used_bytes == 15 * RN_STORE_SMALL_BYTES );

    // down to 9 of 16 (56%): not below half, so no rearm, and back up is quiet

    for ( int i = 14; i >= 9; i-- )
    {
        rn_region_send_free( region, taken[i] );
        check( rn_region_send_room_watch( region ) == RN_SEND_ROOM_NONE );
    }

    for ( int i = 9; i < 13; i++ )
    {
        taken[i] = rn_region_send_alloc( region, 100 );
        check( rn_region_send_room_watch( region ) == RN_SEND_ROOM_NONE );
    }

    // down to 7 of 16 (43%) rearms; back up to 12 crosses once more

    for ( int i = 12; i >= 7; i-- )
    {
        rn_region_send_free( region, taken[i] );
    }
    check( rn_region_send_room_watch( region ) == RN_SEND_ROOM_NONE );
    crossings = 0;

    for ( int i = 7; i < 13; i++ )
    {
        taken[i] = rn_region_send_alloc( region, 100 );
        crossings += rn_region_send_room_watch( region ) == RN_SEND_ROOM_CROSSED;
    }

    check( crossings == 1 );

    // the last descriptor: fits; then none, and the send that finds none exhausts

    for ( int i = 13; i < 16; i++ )
    {
        taken[i] = rn_region_send_alloc( region, 100 );
    }
    check( rn_region_send_fits( region, 100, NULL, NULL, NULL ) == 0 );
    check( rn_region_send_alloc( region, 100 ) == RN_DESCRIPTOR_NONE );
    check( rn_region_exhausted( region ) );

    // a reset carve rearms

    rn_region_reset( region );
    check( region->send_room_armed == 1 );
    check( rn_region_send_fits( region, 100, NULL, NULL, NULL ) == 1 );

    rn_arena_destroy( arena );
}

// ---------------------------------------------------------------------------
// the room query (rn_message_object_send_room, under
// rocketnet_client_can_send_message and rocketnet_server_can_send_message):
// it says no exactly when the start would find no room, a start after a no
// still ends the connection, and asking changes nothing
// ---------------------------------------------------------------------------

struct mo_room_case_t
{
    const char * name;
    int reliability;
    int bytes;
    int receive_message_buffer_bytes;          // the send spans' size; 0 keeps the small config's
    int counter;                               // the pool that runs out first
};

#if ROCKETNET_DEV
// every reliability and size the query is asked for at each step: each store
// class, the spans, a promotion, and the refusals
static const int mo_room_ask_reliabilities[] = {
    ROCKETNET_RELIABILITY_UNRELIABLE,
    ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED,
    ROCKETNET_RELIABILITY_RELIABLE,
    ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
    ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED,
    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
    ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT,
    ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT,
    -1,
    99,
};
static const int mo_room_ask_bytes[] = { -1, 0, 1, 100, 300, 1000, 1152, 1153, 2000, 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES, 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1 };
static const int mo_room_ask_channels[] = { -1, 0, 3, 4 };

// asks every combination on one object; the whole object block (the object,
// its connections, receive bookkeeping and every region) and its counters are
// the same bytes after as before

static void mo_room_ask_everything( rn_message_object_t * object, int index )
{
    check( object->block != NULL );

    const uint64_t block_bytes = rn_message_object_required_bytes( &object->config, object->num_connections, object->is_server );
    check( block_bytes > 0 );
    std::vector<uint8_t> before( (const uint8_t *) object->block, (const uint8_t *) object->block + block_bytes );
    for ( size_t r = 0; r < sizeof( mo_room_ask_reliabilities ) / sizeof( mo_room_ask_reliabilities[0] ); r++ )
        for ( size_t b = 0; b < sizeof( mo_room_ask_bytes ) / sizeof( mo_room_ask_bytes[0] ); b++ )
            for ( size_t c = 0; c < sizeof( mo_room_ask_channels ) / sizeof( mo_room_ask_channels[0] ); c++ )
                for ( int i = index - 1; i <= index + 1; i++ )
                    (void) rn_message_object_send_room( object, i, mo_room_ask_channels[c], mo_room_ask_reliabilities[r], mo_room_ask_bytes[b] );
    check( memcmp( before.data(), object->block, block_bytes ) == 0 );
}

// one object queues without sending until a start finds no room. Before each
// start the query is asked (every combination, changing nothing), and its
// answer for the start is the start's: yes, a pointer; no, NULL with the
// connection's send error set, and the kick at the next update

static void mo_room_run_case( const mo_room_case_t * room_case, bool server_side )
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0;     // deliberate fixed-pool exhaustion
    if ( room_case->receive_message_buffer_bytes )
    {
        config.receive_message_buffer_bytes = room_case->receive_message_buffer_bytes;
    }

    // both objects live in caller blocks that start zeroed: the ask compares
    // the whole block, unused bytes included, and bytes nothing wrote would
    // read as uninitialised under MemorySanitizer (tools/msan_known.txt)

    const size_t alignment = ROCKETNET_ALLOCATION_ALIGNMENT;
    const uint64_t server_bytes = rn_message_object_required_bytes( &config, MO_MAX_SLOTS, 1 );
    const uint64_t client_bytes = rn_message_object_required_bytes( &config, 1, 0 );
    check( server_bytes > 0 && client_bytes > 0 );

    void * server_allocation = calloc( 1, (size_t) server_bytes + alignment - 1 );
    void * client_allocation = calloc( 1, (size_t) client_bytes + alignment - 1 );
    check( server_allocation != NULL && client_allocation != NULL );

    void * server_block = (void *) ( ( (uintptr_t) server_allocation + alignment - 1 ) & ~(uintptr_t) ( alignment - 1 ) );
    void * client_block = (void *) ( ( (uintptr_t) client_allocation + alignment - 1 ) & ~(uintptr_t) ( alignment - 1 ) );

    mo_pair_t pair;
    memset( &pair, 0, sizeof( pair ) );
    pair.net = mo_net_new();
    pair.num_clients = 1;
    pair.server = mo_server_create( pair.net, &config, MO_MAX_SLOTS, 0, server_block, server_bytes );
    pair.clients[0] = mo_client_create( pair.net, 0, &config, 0, client_block, client_bytes );
    check( pair.server && pair.clients[0] );
    mo_net_connect( pair.net, 0 );
    pair.time = MO_T0;
    mo_step( &pair );

    rn_message_object_t * object = server_side ? &pair.server->object : &pair.clients[0]->object;
    rn_message_connection_t * connection = rn_message_object_connection( object, 0 );
    const rocketnet_counters_t * counters = server_side ? rocketnet_message_server_counters( pair.server ) : rocketnet_message_client_counters( pair.clients[0] );
    const int channel = 1;

    int started = 0;
    bool said_no = false;

    for ( int step = 0; step < 1000; step++ )
    {
        mo_room_ask_everything( object, 0 );

        const bool room = rn_message_object_send_room( object, 0, channel, room_case->reliability, room_case->bytes );
        check( room == rn_message_object_send_room( object, 0, channel, room_case->reliability, room_case->bytes ) );
        check( !rn_region_exhausted( connection->region ) );

        uint8_t * p = rn_message_object_start( object, 0, channel, room_case->reliability, room_case->bytes );

        if ( !room )
        {
            // a no: the start is still the fatal one, unchanged
            check( p == NULL );
            check( rn_region_exhausted( connection->region ) );
            check( connection->send_error == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
            check( !rn_message_object_send_room( object, 0, channel, room_case->reliability, room_case->bytes ) );
            said_no = true;
            break;
        }

        check( p != NULL );

        // while a start is pending the object takes no second one: no room

        check( !rn_message_object_send_room( object, 0, channel, room_case->reliability, room_case->bytes ) );
        mo_fill( p, (uint32_t) started, (uint32_t) room_case->bytes );
        check( rn_message_object_finish( object, 0, room_case->bytes ) != 0 );
        started++;
    }

    check( said_no );
    check( started > 0 );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 1 );
    check( mo_counter( counters, room_case->counter ) == 1 );

    // the kick: the connection ends with the reason, and a gone connection has no room

    if ( server_side )
    {
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
        check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
        check( rocketnet_message_server_client_connected( pair.server, 0 ) == 0 );
    }
    else
    {
        pair.time += 0.01;
        (void) rocketnet_message_client_update( pair.clients[0], pair.time );
        check( rocketnet_message_client_disconnect_reason( pair.clients[0] ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    }

    check( !rn_message_object_send_room( object, 0, channel, room_case->reliability, room_case->bytes ) );
    mo_room_ask_everything( object, 0 );

    mo_pair_teardown( &pair );
    free( client_allocation );
    free( server_allocation );
}

static void test_send_room_query_says_no_exactly_when_a_start_exhausts()
{
    // the small config: 16 descriptors, a store of 12288 bytes in three
    // classes, and the spans sized by receive_message_buffer_bytes

    const mo_room_case_t cases[] = {
        { "small reliable ordered, the descriptors run out", ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100, 0, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL },
        { "large-class reliable, the store runs out", ROCKETNET_RELIABILITY_RELIABLE, 1000, 0, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL },
        { "span reliable with receipt, the spans run out", ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 2000, 8192, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL },
        { "promoted unreliable, the spans run out", ROCKETNET_RELIABILITY_UNRELIABLE, 2000, 8192, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL },
    };
    for ( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
    {
        mo_room_run_case( &cases[i], false );
        mo_room_run_case( &cases[i], true );
    }
}

static int mo_room_lines = 0;
static int mo_room_crossings = 0;
static int mo_room_exhausted = 0;
static int mo_room_named = 0;
static const char * mo_room_name = "client";

static void mo_room_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;

    char line[1024];
    const size_t n = length < sizeof( line ) - 1 ? length : sizeof( line ) - 1;
    memcpy( line, message, n );
    line[n] = '\0';
    if ( strstr( line, "send room" ) == NULL )
        return;
    check( level == ROCKETNET_LOG_LEVEL_WARN );
    mo_room_lines++;
    if ( strstr( line, "over 75% full" ) )
    {
        mo_room_crossings++;
    }
    if ( strstr( line, "exhausted" ) )
    {
        mo_room_exhausted++;
    }
    if ( strstr( line, mo_room_name ) && strstr( line, "send_message_buffer_bytes" ) && strstr( line, "send_queue_messages" ) && strstr( line, " of " ) )
    {
        mo_room_named++;
    }
}

static uint32_t mo_room_permille( rn_message_connection_t * connection )
{
    rn_send_room_t room;
    rn_region_send_room( connection->region, &room );
    return room.permille;
}

// through the client and server objects, with the log captured: one line at
// the crossing, none while the connection sits above it, one more after acks
// took it below half and it crossed again, one at exhaustion; none at all in
// a release build

static void test_send_room_warns_in_debug_builds_only()
{
    rocketnet_message_config_t config;
    mo_small_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // deliberate fixed-pool exhaustion

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config, 0, 0 );
    mo_step( &pair );
    mo_step( &pair );

    check( rn_send_room_warnings == ( ROCKETNET_ASSERTS ? 1 : 0 ) );     // compiled in exactly when the debug macro is on
    mo_room_lines = mo_room_crossings = mo_room_exhausted = mo_room_named = 0;
    mo_room_name = "client";
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( mo_room_capture, NULL );

    rocketnet_message_client_t * client = pair.clients[0];
    rn_message_connection_t * connection = mo_client_connection( client );
    uint8_t message[100];
    mo_fill( message, 1, sizeof( message ) );

    // up past 75%, and on to 87%: one crossing

    int sends = 0;

    while ( mo_room_permille( connection ) < 870 )
    {
        check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        check( ++sends < 100 );
    }

    check( mo_room_crossings == ( rn_send_room_warnings ? 1 : 0 ) );

    // the acks take it below half; the next sends rearm and cross again

    for ( int step = 0; step < 50 && mo_room_permille( connection ) >= 500; step++ )
    {
        mo_step( &pair );
    }
    check( mo_room_permille( connection ) < 500 );

    while ( mo_room_permille( connection ) < 750 )
    {
        check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) );
        check( ++sends < 200 );
    }

    check( mo_room_crossings == ( rn_send_room_warnings ? 2 : 0 ) );
    check( mo_room_exhausted == 0 );

    // on to exhaustion: one line just before the kick, naming the client

    while ( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) )
    {
        check( ++sends < 300 );
    }
    check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( mo_room_crossings == ( rn_send_room_warnings ? 2 : 0 ) );
    check( mo_room_exhausted == ( rn_send_room_warnings ? 1 : 0 ) );
    check( mo_room_lines == ( rn_send_room_warnings ? 3 : 0 ) );
    check( mo_room_named == mo_room_lines );

    // a server names the slot

    mo_pair_teardown( &pair );
    mo_pair_setup( &pair, 1, &config, 0, 0 );
    mo_step( &pair );
    mo_step( &pair );
    check( rocketnet_message_server_client_connected( pair.server, 0 ) == 1 );
    mo_room_lines = mo_room_crossings = mo_room_exhausted = mo_room_named = 0;
    mo_room_name = "server slot 0";
    while ( rocketnet_message_server_send( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) )
    {
        check( ++sends < 400 );
    }
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( mo_room_crossings == ( rn_send_room_warnings ? 1 : 0 ) );
    check( mo_room_exhausted == ( rn_send_room_warnings ? 1 : 0 ) );
    check( mo_room_named == mo_room_lines );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    mo_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// A queued-path (region) start left pending across an update whose reset hits
// its slot is cancelled by that reset (sdk/rocketnet_message_server.cpp,
// rn_message_object_update: rn_message_connection_cancel_pending before each
// connection reset; proofs/tla/RNSlots.tla NoPendingAcrossReset): the finish after
// it is a no-op that returns 0, the cancelled start is counted in
// MESSAGE_SEND_REFUSED, nothing lands in the recarved region, and a later
// start on every slot is accepted.
// ---------------------------------------------------------------------------

static void test_pending_start_cancelled_by_slot_reset()
{
    mo_pair_t pair;
    mo_pair_setup( &pair, MO_MAX_SLOTS );
    mo_step( &pair );
    mo_step( &pair );
    mo_step( &pair );

    // a region-path start on slot 0, written by the application, not finished

    uint8_t * pointer = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 );
    check( pointer != NULL );
    mo_fill( pointer, 7, 100 );

    const uint64_t refused = mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );

    // the transport gives slot 0 to a new connection: Observe resets the slot

    rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
    const uint64_t carves = slot->region->carves;
    mo_net_drop_slot( pair.net, 0 );
    mo_net_connect( pair.net, 0 );
    pair.time += 0.01;
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
    check( slot->region->carves == carves + 1 );
    check( slot->hot->state == RN_MESSAGE_CONNECTION_CONNECTED );
    check( pair.server->object.pending_connection == -1 );
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused + 1 );

    // the finish after the reset asserts in Debug and returns 0 in Release: nothing joins the new connection's queue

    const uint32_t descriptors_free = slot->region->descriptors_free;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;
    check( rocketnet_message_server_finish( pair.server, 0, 100 ) == 0 );
    rocketnet_assert_function( NULL );
    check( mo_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    check( slot->region->descriptors_free == descriptors_free );
    check( slot->region->channel_send[0].ordered_write == 0 );

    // and every slot takes a later start

    for ( int i = 0; i < MO_MAX_SLOTS; i++ )
    {
        uint8_t * next = rocketnet_message_server_start( pair.server, i, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 );
        check( next != NULL );
        mo_fill( next, (uint32_t) ( 100 + i ), 100 );
        check( rocketnet_message_server_finish( pair.server, i, 100 ) != 0 );
    }

    // the client: a start pending across the update that ends its transport is
    // cancelled by that update's reset, and its finish asserts in Debug and returns 0 in Release

    uint8_t * up = rocketnet_message_client_start( pair.clients[1], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 );
    check( up != NULL );
    mo_fill( up, 9, 100 );
    mo_net_drop_slot( pair.net, 1 );
    check( rocketnet_message_client_update( pair.clients[1], pair.time + 0.01 ) == ROCKETNET_UPDATE_OK );
    check( mo_client_connection( pair.clients[1] )->hot->state == RN_MESSAGE_CONNECTION_FREE );
    check( pair.clients[1]->object.pending_connection == -1 );
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;
    check( rocketnet_message_client_finish( pair.clients[1], 100 ) == 0 );
    rocketnet_assert_function( NULL );
    check( mo_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    mo_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_start_finish_pointer_reaches_the_frame );
REGISTER_UNIT_TEST( test_receive_view_is_the_record );
REGISTER_UNIT_TEST( test_release_returns_room );

// the message object reads no environment (sdk/rocketnet_message_server.cpp:146):
// the public budget variables are
// ROCKETNET_CLIENT_SEND_KBPS and ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS, read by
// the public creates (sdk/rocketnet_api.cpp:135 and :173).
// A ROCKETNET_MESSAGE_* budget variable changes nothing and refuses nothing.
// The helpers set and unset the unit environment table on every platform.

static void mo_setenv( const char * name, const char * value ) { rocketnet_test_env_setenv( name, value, 1 ); }
static void mo_unsetenv( const char * name ) { rocketnet_test_env_unsetenv( name ); }

static void test_send_budget_message_env_is_not_read()
{
    rocketnet_message_config_t config;
    mo_config( &config );
    config.client_send_kbps = 250;
    config.server_send_kbps = 4000;

    mo_setenv( "ROCKETNET_MESSAGE_CLIENT_SEND_KBPS", "fast" );
    mo_setenv( "ROCKETNET_MESSAGE_SERVER_SEND_KBPS", "-1" );

    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config, 0, 0 );
        check( mo_client_connection( pair.clients[0] )->budget.kbps == 250 );
        check( mo_server_slot( &pair, 0 )->budget.kbps == 4000 );
        check( mo_server_slot( &pair, 0 )->send_budget_enabled == 1 );
        mo_pair_teardown( &pair );
    }

    mo_unsetenv( "ROCKETNET_MESSAGE_CLIENT_SEND_KBPS" );
    mo_unsetenv( "ROCKETNET_MESSAGE_SERVER_SEND_KBPS" );
}

// ---------------------------------------------------------------------------
// the object's update clamps a bad time through rocketnet_clamp_update_time
// once: a backwards, NaN or infinite update is counted once in
// rocketnet_api_misuse_count, never stored in object->time, never made the
// clock origin, so +INFINITY cannot wedge the clock or the connection
// ---------------------------------------------------------------------------

static bool mo_send_and_verify_delivery( mo_pair_t * pair, bool from_client, uint8_t * msg, int bytes, uint32_t tag )
{
    // warm up the handshake and CONFIG exchange so the connection is CONNECTED

    double t = pair->time + 0.02;

    for ( int s = 0; s < 3; s++ )
    {
        check( rocketnet_message_server_update( pair->server, t ) == ROCKETNET_UPDATE_OK );
        check( rocketnet_message_client_update( pair->clients[0], t ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( pair->server );
        rocketnet_message_client_send_packets( pair->clients[0] );
        t += 0.02;
    }

    pair->time = t;

    mo_fill( msg, tag, (uint32_t) bytes );
    if ( from_client )
        check( rocketnet_message_client_send( pair->clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, msg, bytes ) != 0 );
    else
        check( rocketnet_message_server_send( pair->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, msg, bytes ) != 0 );

    rocketnet_message_client_send_packets( pair->clients[0] );
    rocketnet_message_server_send_packets( pair->server );

    for ( int step = 0; step < 40; step++ )
    {
        std::vector<mo_received_t> got;
        check( rocketnet_message_server_update( pair->server, t ) == ROCKETNET_UPDATE_OK );
        check( rocketnet_message_client_update( pair->clients[0], t ) == ROCKETNET_UPDATE_OK );
        if ( from_client )
            mo_drain_server( pair, &got );
        else
            mo_drain_client( pair->clients[0], &got );
        rocketnet_message_server_send_packets( pair->server );
        rocketnet_message_client_send_packets( pair->clients[0] );
        for ( size_t i = 0; i < got.size(); i++ )
            if ( (int) got[i].bytes.size() == bytes && memcmp( got[i].bytes.data(), msg, (size_t) bytes ) == 0 )
                return true;
        t += 0.02;
    }

    return false;
}

static void test_update_time_bad_counted_once_never_stored_or_wedged()
{
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;

    // the client object, then the server object, on the pair

    for ( int side = 0; side < 2; side++ )
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1 );                       // objects seeded at MO_T0

        const uint64_t base = rocketnet_api_misuse_count();

        if ( side == 0 )
        {
            rocketnet_message_client_t * client = pair.clients[0];
            check( rocketnet_message_client_update( client, MO_T0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base );

            check( rocketnet_message_client_update( client, MO_T0 - 1.0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 1 );
            check( client->object.time == MO_T0 );

            check( rocketnet_message_client_update( client, (double) NAN ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 2 );
            check( client->object.time == MO_T0 );

            check( rocketnet_message_client_update( client, (double) INFINITY ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 3 );
            check( client->object.time == MO_T0 );

            check( rocketnet_message_client_update( client, -(double) INFINITY ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 4 );
            check( client->object.time == MO_T0 );

            check( rocketnet_message_client_update( client, MO_T0 + 1.0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 4 );
            check( client->object.time == MO_T0 + 1.0 );
            pair.time = MO_T0 + 1.0;
        }
        else
        {
            rocketnet_message_server_t * server = pair.server;
            check( rocketnet_message_server_update( server, MO_T0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base );

            check( rocketnet_message_server_update( server, MO_T0 - 1.0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 1 );
            check( server->object.time == MO_T0 );

            check( rocketnet_message_server_update( server, (double) NAN ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 2 );
            check( server->object.time == MO_T0 );

            check( rocketnet_message_server_update( server, (double) INFINITY ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 3 );
            check( server->object.time == MO_T0 );

            check( rocketnet_message_server_update( server, -(double) INFINITY ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 4 );
            check( server->object.time == MO_T0 );

            check( rocketnet_message_server_update( server, MO_T0 + 1.0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 4 );
            check( server->object.time == MO_T0 + 1.0 );
            pair.time = MO_T0 + 1.0;
        }

        // the connection still completes: a message after the bad times arrives

        uint8_t msg[32];
        check( mo_send_and_verify_delivery( &pair, side == 0, msg, 32, side == 0 ? 777 : 888 ) );

        mo_pair_teardown( &pair );
    }

    rocketnet_assert_function( saved_assert );
}

static void test_update_time_first_update_not_finite_refused()
{
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;

    for ( int side = 0; side < 2; side++ )
    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1 );                       // objects seeded at MO_T0, never updated

        const uint64_t base = rocketnet_api_misuse_count();

        if ( side == 0 )
        {
            rocketnet_message_client_t * client = pair.clients[0];
            check( rocketnet_message_client_update( client, (double) NAN ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 1 );
            check( client->object.time == MO_T0 );

            check( rocketnet_message_client_update( client, (double) INFINITY ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 2 );
            check( client->object.time == MO_T0 );

            check( rocketnet_message_client_update( client, MO_T0 + 1.0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 2 );
            check( client->object.time == MO_T0 + 1.0 );
            pair.time = MO_T0 + 1.0;
        }
        else
        {
            rocketnet_message_server_t * server = pair.server;
            check( rocketnet_message_server_update( server, (double) NAN ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 1 );
            check( server->object.time == MO_T0 );

            check( rocketnet_message_server_update( server, (double) INFINITY ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 2 );
            check( server->object.time == MO_T0 );

            check( rocketnet_message_server_update( server, MO_T0 + 1.0 ) == ROCKETNET_UPDATE_OK );
            check( rocketnet_api_misuse_count() == base + 2 );
            check( server->object.time == MO_T0 + 1.0 );
            pair.time = MO_T0 + 1.0;
        }

        uint8_t msg[32];
        check( mo_send_and_verify_delivery( &pair, side == 0, msg, 32, side == 0 ? 901 : 902 ) );

        mo_pair_teardown( &pair );
    }

    rocketnet_assert_function( saved_assert );
}

// slots are keyed on the transport's connection id (P9a), and each batch entry
// is admitted by the connection it was received for (P9b): proofs/tla/RNSlots.tla
// NoCrossConnectionApply, NoOwnEntryDropped, ReusedSlotStartsClean, and the
// reversed witness proofs/tla/MCRNSlotsBrokenClientIdAsConnection.cfg
// ---------------------------------------------------------------------------

// A slot bound in this update keeps its own first entries (NoOwnEntryDropped):
// the client's first packet is on the wire before the server's update that
// observes its connection, and the record in it is delivered in that update.

static void test_slot_bound_this_update_keeps_its_first_payload()
{
    rocketnet_message_config_t config;
    mo_config( &config );

    mo_pair_t pair;
    mo_pair_setup( &pair, 1, &config );                                         // the slot's connection is up; the server has not updated
    pair.time += 0.01;
    check( rocketnet_message_client_update( pair.clients[0], pair.time ) == ROCKETNET_UPDATE_OK );
    mo_send_up( &pair, 0, 0, 7 );
    check( pair.net->server->inbound.size() >= 1 );

    const uint64_t stale = mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
    check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );
    check( mo_server_slot( &pair, 0 )->hot->connection_id == pair.net->slot_connection[0] );

    const uint8_t * data = NULL;
    int bytes = 0;
    int from = -1;
    check( rocketnet_message_server_receive( pair.server, &from, NULL, &data, &bytes ) == 1 );
    check( from == 0 );

    uint32_t id = 0;
    check( data && mo_verify( std::vector<uint8_t>( data, data + bytes ), &id ) && id == 7 );
    rocketnet_message_server_release( pair.server, data );
    check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) == stale );
    mo_pair_teardown( &pair );
}

// An entry stamped with a previous connection's id is dropped and counted
// (NoCrossConnectionApply), in an update after the one that observed the new
// connection: connection A's payload, and the transport's ack of a packet the
// server sent to A, each reach the batch one update after B took the slot.

static void test_previous_connection_payload_dropped_after_reconnect()
{
    rocketnet_message_config_t config;
    mo_config( &config );

    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config );
        mo_step( &pair );
        mo_step( &pair );

        const uint64_t a = pair.net->slot_connection[0];
        mo_send_up( &pair, 0, 0, 1 );                                           // A's record, on the wire

        std::vector<mo_packet_t> late;
        late.swap( pair.net->server->inbound );                                 // held past B's connect
        check( late.size() >= 1 && late[0].connection == a );
        mo_net_connect( pair.net, 0 );                                          // B takes the slot
        check( pair.net->slot_connection[0] != a );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );     // Observe binds B
        check( mo_server_slot( &pair, 0 )->hot->connection_id == pair.net->slot_connection[0] );

        const uint64_t stale = mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        pair.net->server->inbound.swap( late );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );

        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, NULL, NULL, &data, &bytes ) == 0 );    // never applied to B
        check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) == stale + 1 );
        mo_pair_teardown( &pair );
    }
}

static void test_previous_connection_ack_dropped_after_reconnect()
{
    rocketnet_message_config_t config;
    mo_config( &config );

    {
        mo_pair_t pair;
        mo_pair_setup( &pair, 1, &config );
        mo_step( &pair );
        mo_step( &pair );

        const uint64_t a = pair.net->slot_connection[0];
        uint8_t message[100];
        mo_fill( message, 9, sizeof( message ) );

        const uint32_t a_server_serial = rocketnet_message_server_send(
            pair.server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, message, (int) sizeof( message ) );
        check( a_server_serial != 0 );
        rocketnet_message_server_send_packets( pair.server );
        pair.net->hold_acks = true;                                             // A's ack is deferred past B's connect
        pair.time += 0.01;
        check( rocketnet_message_client_update( pair.clients[0], pair.time ) == ROCKETNET_UPDATE_OK );     // A opens it

        // drain A's received message before A disconnects so m1.6 is satisfied
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            while ( rocketnet_message_client_receive( pair.clients[0], NULL, &data, &bytes ) )
            {
                rocketnet_message_client_release( pair.clients[0], data );
            }
        }

        const size_t held = pair.net->server->acks_inbox.size();               // this packet's ack, and any of A's earlier ones
        check( held >= 1 );
        for ( size_t i = 0; i < held; i++ )
        {
            check( pair.net->server->acks_inbox_connection[i] == a );
        }

        // also queue an unacked reliable-with-receipt message and an in-flight
        // resend candidate from A on the client side before B takes the slot

        mo_fill( message, 10, sizeof( message ) );

        const uint32_t a_client_serial = rocketnet_message_client_send(
            pair.clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, message, (int) sizeof( message ) );
        check( a_client_serial != 0 );
        rocketnet_message_client_send_packets( pair.clients[0] );

        std::vector<mo_packet_t> a_late_c2s;
        a_late_c2s.swap( pair.net->server->inbound );
        check( a_late_c2s.size() >= 1 && a_late_c2s[0].connection == a );

        rocketnet_message_client_destroy( pair.clients[0] );
        delete pair.net->clients[0];
        pair.net->clients[0] = NULL;
        pair.clients[0] = mo_client_create( pair.net, 0, &config, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
        check( pair.clients[0] );
        mo_net_connect( pair.net, 0 );                                          // B takes the slot with a new connection_id
        check( pair.net->slot_connection[0] != a );
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );     // Observe binds B
        check( rocketnet_message_client_update( pair.clients[0], pair.time ) == ROCKETNET_UPDATE_OK ); // Client binds B
        rocketnet_message_server_send_packets( pair.server );
        rocketnet_message_client_send_packets( pair.clients[0] );
        check( mo_server_slot( &pair, 0 )->hot->connection_id == pair.net->slot_connection[0] );

        const uint64_t stale = mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY );
        pair.net->hold_acks = false;
        pair.net->server->inbound.insert( pair.net->server->inbound.begin(), a_late_c2s.begin(), a_late_c2s.end() );
        pair.time += 0.01;
        check( rocketnet_message_server_update( pair.server, pair.time ) == ROCKETNET_UPDATE_OK );     // A's acks and late payload in the batch
        check( mo_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) == stale + held + 1 );

        // neither A's late payload nor A's late ACK produces a message or receipt for B

        const uint8_t * data = NULL;
        int bytes = 0;
        check( rocketnet_message_server_receive( pair.server, NULL, NULL, &data, &bytes ) == 0 );

        rocketnet_message_receipt_t receipts[8];
        check( rocketnet_message_server_receipts( pair.server, 0, receipts, 8 ) == 0 );
        check( rocketnet_message_client_receipts( pair.clients[0], receipts, 8 ) == 0 );

        // settle B's CONFIG and verify B's new-generation messages and ACK receipts complete both ways

        mo_step( &pair );
        mo_step( &pair );
        mo_fill( message, 21, sizeof( message ) );

        const uint32_t b_c2s_serial = rocketnet_message_client_send(
            pair.clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, message, (int) sizeof( message ) );
        mo_fill( message, 22, sizeof( message ) );

        const uint32_t b_s2c_serial = rocketnet_message_server_send(
            pair.server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, message, (int) sizeof( message ) );
        check( b_c2s_serial != 0 && b_s2c_serial != 0 );

        std::vector<mo_received_t> srv_got, cli_got;
        int got_c2s_receipt = 0, got_s2c_receipt = 0;

        for ( int step = 0; step < 10; step++ )
        {
            mo_step( &pair, &srv_got, &cli_got );

            const int cn = rocketnet_message_client_receipts( pair.clients[0], receipts, 8 );
            for ( int r = 0; r < cn; r++ )
            {
                check( receipts[r].serial == b_c2s_serial );
                if ( receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED )
                {
                    got_c2s_receipt = 1;
                }
            }

            const int sn = rocketnet_message_server_receipts( pair.server, 0, receipts, 8 );
            for ( int r = 0; r < sn; r++ )
            {
                check( receipts[r].serial == b_s2c_serial );
                if ( receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED )
                {
                    got_s2c_receipt = 1;
                }
            }
        }

        uint32_t id = 0;
        check( srv_got.size() == 1 && mo_verify( srv_got[0].bytes, &id ) && id == 21 );
        check( cli_got.size() == 1 && mo_verify( cli_got[0].bytes, &id ) && id == 22 );
        check( got_c2s_receipt == 1 && got_s2c_receipt == 1 );
        mo_pair_teardown( &pair );
    }
}

// A client disconnects and a new connection with the same client id takes the
// same slot inside one server update (ReusedSlotStartsClean): the public
// server and clients over the in-memory loopback transport, step driven, time
// from 1000.0. The server's slot is reset for the new connection and announces
// its CONFIG to it, so the two exchange messages from a fresh start.

#define RECONNECT_PROTOCOL 0x7A11C1C1E5EED0A1ULL
#define RECONNECT_CLIENT_ID 0x5107ULL

static double reconnect_now = 1000.0;
static double reconnect_clock( void * ) { return reconnect_now; }

struct reconnect_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_server_t * server;
    rocketnet_client_t * clients[2];
    double time;
};

static void reconnect_pump( reconnect_world_t * world, double dt = 0.01 )
{
    reconnect_now = world->time;

    for ( int i = 0; i < 2; i++ )
    {
        rocketnet_client_update( world->clients[i], world->time );
        rocketnet_client_send_packets( world->clients[i] );
    }

    rocketnet_server_update( world->server, world->time );
    rocketnet_server_send_packets( world->server );
    world->time += dt;
}

static void reconnect_connect( reconnect_world_t * world, int client )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );

    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( world->backend_secret, world->server_public,
                                                     RECONNECT_PROTOCOL, addresses, 1, RECONNECT_CLIENT_ID, 1000, 1ULL << 40, 300, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, sizeof( token_string ) ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    check( rocketnet_client_connect( world->clients[client], token_string ) == ROCKETNET_STATUS_OK );
}

// Both directions on channel 0, values counted from 0 in order: a slot that
// kept the last connection's sequence state, or a client that never received
// the server's CONFIG, does not deliver them.

static int reconnect_exchange( reconnect_world_t * world, int client, uint8_t tag, int want )
{
    rocketnet_client_t * c = world->clients[client];

    for ( int i = 0; i < want; i++ )
    {
        uint8_t * up = rocketnet_client_start_message( c, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
        check( up != NULL );

        if ( up )
        {
            up[0] = tag;
            up[1] = (uint8_t) i;
            check( rocketnet_client_finish_message( c, 2 ) != 0 );
        }

        uint8_t * down = rocketnet_server_start_message( world->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
        check( down != NULL );

        if ( down )
        {
            down[0] = tag;
            down[1] = (uint8_t) i;
            check( rocketnet_server_finish_message( world->server, 2 ) != 0 );
        }
    }

    int at_server = 0, at_client = 0;
    for ( int step = 0; step < 300 && ( at_server < want || at_client < want ); step++ )
    {
        reconnect_pump( world );

        const uint8_t * data = NULL;
        int bytes = 0;
        uint64_t sender = ~0ULL;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
                sender = (uint64_t) batch->client_index[i];
                check( sender == 0 );
                check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_server );

                at_server++;
            }
        }

        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( c ) ) != NULL )
        {
            for ( int i = 0; i < incoming->num_messages; i++ )
            {
                data = incoming->data[i];
                bytes = incoming->bytes[i];
                check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_client );

                at_client++;
            }
        }
    }

    return at_server == want && at_client == want;
}

static void test_same_client_id_reconnect_in_one_update_resets_the_slot()
{
    reconnect_world_t world;
    memset( &world, 0, sizeof( world ) );
    rocketnet_quiet( 1 );
    world.time = 1000.0;
    reconnect_now = world.time;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, world.backend_public, world.backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( world.server_public, world.server_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = RECONNECT_PROTOCOL;
    server_config.max_clients = 1;                                              // the new connection can only take slot 0
    memcpy( server_config.backend_public_key, world.backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world.server != NULL );
    rocketnet_server_public_key( world.server, world.server_public );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 2; i++ )
    {
        rocketnet_client_config_t client_config;
        rocketnet_client_default_config( &client_config );
        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = true;
        client_config.time_callback = reconnect_clock;
        world.clients[i] = rocketnet_client_create( NULL, &client_config, NULL );
        check( world.clients[i] != NULL );
    }

    // A and B hold tokens for one client id. Both are challenged; A's
    // response connects first, and B's responses are ignored while A holds
    // that id, so B waits in its response state.

    reconnect_connect( &world, 0 );
    reconnect_connect( &world, 1 );
    for ( int i = 0; i < 400 && rocketnet_client_state( world.clients[0] ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        reconnect_pump( &world );
    }
    check( rocketnet_client_state( world.clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( world.clients[0] ) == 0 );
    check( rocketnet_server_client_id( world.server, 0 ) == RECONNECT_CLIENT_ID );
    check( reconnect_exchange( &world, 0, 0xA1, 3 ) );
    check( rocketnet_client_state( world.clients[1] ) == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE );
    check( rn_server_stats_of( world.server ).connections_accepted == 1 );

    // A disconnects; in the next server update its disconnect is processed
    // and then B's response, which takes slot 0 with the same client id. B's
    // response is due (its resend interval has passed) in that same pump.

    world.time += 0.25;
    rocketnet_client_disconnect( world.clients[0] );
    rn_test_client_wait_disconnected( world.clients[0] );
    reconnect_pump( &world );
    check( rn_server_stats_of( world.server ).connections_accepted == 2 );
    check( rocketnet_server_client_connected( world.server, 0 ) != 0 );
    check( rocketnet_server_client_id( world.server, 0 ) == RECONNECT_CLIENT_ID );

    for ( int i = 0; i < 400 && rocketnet_client_state( world.clients[1] ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        reconnect_pump( &world );
    }
    check( rocketnet_client_state( world.clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( world.clients[1] ) == 0 );

    // the slot started clean and B received the server's CONFIG: the exchange
    // runs from 0 in order, and B is still connected after the CONFIG wait

    check( reconnect_exchange( &world, 1, 0xB2, 3 ) );

    const double until = world.time + (double) server_config.reliable_message_timeout_ms / 1000.0 + 1.0;
    while ( world.time < until && rocketnet_client_state( world.clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        reconnect_pump( &world, 0.05 );
    }
    check( rocketnet_client_state( world.clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    for ( int i = 0; i < 2; i++ )
    {
        rn_test_client_stop_and_destroy( world.clients[i] );
    }
    rn_test_server_stop_and_destroy( world.server );
}
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// standard/MESSAGES.md m1.6, proofs/tla/RNChannel.tla EndUpdate: every delivery is
// received before the next update. A delivery still in the receive queue at
// update is API misuse: counted MESSAGE_NOT_RECEIVED, an assert in a debug
// build, and that connection ends with MESSAGE_NOT_RECEIVED. Nothing is
// dropped silently. A full drain every update delivers every message once,
// in order.
// ---------------------------------------------------------------------------

#define MO_NOT_RECEIVED_MESSAGES        8
#define MO_NOT_RECEIVED_TAKEN           3

// queues the 8 reliable ordered messages ids 100.. on channel 1, from the
// server to slot 0 (to_client) or from client 0 to the server

#if ROCKETNET_DEV
static void mo_not_received_send( mo_pair_t * pair, bool to_client )
{
    for ( int m = 0; m < MO_NOT_RECEIVED_MESSAGES; m++ )
    {
        uint8_t * q = to_client ? rocketnet_message_server_start( pair->server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 64 ) : rocketnet_message_client_start( pair->clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 64 );
        check( q );
        mo_fill( q, (uint32_t) ( 100 + m ), 64 );
        check( to_client ? rocketnet_message_server_finish( pair->server, 0, 64 ) != 0 : rocketnet_message_client_finish( pair->clients[0], 64 ) != 0 );
    }

    if ( to_client )
        rocketnet_message_server_send_packets( pair->server );
    else
        rocketnet_message_client_send_packets( pair->clients[0] );
}

static void mo_not_received_case( bool to_client )
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 2, NULL, 20000, 20000 );
    mo_step( &pair );
    mo_step( &pair );

    rn_message_object_t * receiver = to_client ? &pair.clients[0]->object : &pair.server->object;
    const rocketnet_counters_t * counters = to_client ? rocketnet_message_client_counters( pair.clients[0] ) : rocketnet_message_server_counters( pair.server );

    mo_not_received_send( &pair, to_client );
    pair.time += 0.01;
    check( rn_message_object_update( receiver, pair.time ) == ROCKETNET_UPDATE_OK );

    // the application takes 3 of the 8 delivered, one at a time, and updates

    std::vector<uint32_t> got;

    for ( int k = 0; k < MO_NOT_RECEIVED_TAKEN; k++ )
    {
        const uint8_t * data = NULL;
        int bytes = 0;
        int index = -1;
        check( rn_message_object_receive( receiver, &index, NULL, &data, &bytes ) == 1 );
        check( index == 0 );
        std::vector<uint8_t> v( data, data + bytes );

        uint32_t id = 0;
        check( mo_verify( v, &id ) );
        got.push_back( id );
        rn_message_object_release( receiver, data );
    }

    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_NOT_RECEIVED ) == 0 );
    pair.time += 0.01;
    check( rn_message_object_update( receiver, pair.time ) == ROCKETNET_UPDATE_OK );
#if ROCKETNET_ASSERTS
    check( mo_asserts == 1 );
#else
    check( mo_asserts == 0 );
#endif // #if ROCKETNET_ASSERTS
    rocketnet_assert_function( saved_assert );

    // the 5 never received are counted, and the connection ended for them

    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_NOT_RECEIVED ) == (uint64_t) ( MO_NOT_RECEIVED_MESSAGES - MO_NOT_RECEIVED_TAKEN ) );
    check( rn_message_object_disconnect_reason( receiver, 0 ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED );
    check( !rn_message_object_connected( receiver, 0 ) );
    check( to_client ? pair.net->clients[0]->disconnects == 1 : pair.net->server->disconnects == 1 );
    for ( size_t m = 0; m < got.size(); m++ )
    {
        check( got[m] == (uint32_t) ( 100 + m ) );
    }

    // the other client of the server is untouched

    if ( !to_client )
    {
        check( rn_message_object_connected( receiver, 1 ) );
    }

    mo_pair_teardown( &pair );
}

static void test_message_not_received_ends_the_connection()
{
    mo_not_received_case( true );
    mo_not_received_case( false );
}

static void mo_full_drain_case( bool to_client )
{
    mo_pair_t pair;
    mo_pair_setup( &pair, 1, NULL, 20000, 20000 );
    mo_step( &pair );
    mo_step( &pair );

    mo_not_received_send( &pair, to_client );

    std::vector<uint32_t> got;

    for ( int i = 0; i < 10; i++ )
    {
        std::vector<mo_received_t> g;
        mo_step( &pair, to_client ? NULL : &g, to_client ? &g : NULL );

        for ( size_t j = 0; j < g.size(); j++ )
        {
            uint32_t id = 0;
            check( mo_verify( g[j].bytes, &id ) );
            check( g[j].channel == 1 );
            got.push_back( id );
        }
    }

    check( got.size() == MO_NOT_RECEIVED_MESSAGES );
    for ( size_t m = 0; m < got.size(); m++ )
    {
        check( got[m] == (uint32_t) ( 100 + m ) );
    }

    const rocketnet_counters_t * counters = to_client ? rocketnet_message_client_counters( pair.clients[0] ) : rocketnet_message_server_counters( pair.server );
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_NOT_RECEIVED ) == 0 );
    check( rn_message_object_connected( &pair.server->object, 0 ) );
    check( rn_message_object_connected( &pair.clients[0]->object, 0 ) );

    mo_pair_teardown( &pair );
}

static void test_message_full_drain_delivers_every_message_once_in_order()
{
    mo_full_drain_case( true );
    mo_full_drain_case( false );
}
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// RNSendBudget SendFailed (R6): a packet the transport refuses at finish is
// charged its maximum wire bytes when written and refunded whole when the
// finish fails, so the bucket and bytes_spent are as they were before it. The
// server's slot is capped at a low rate, where one maximum packet is a visible
// share of the bucket, and every check runs at one update time T, where a
// second refill adds nothing.
// ---------------------------------------------------------------------------

#define MO_REFUND_KBPS 400                          // 50000 bytes a second, a 5000-byte bucket

struct mo_refund_mark_t
{
    double tokens;
    uint64_t bytes_spent;
    uint64_t refusals;
};

#if ROCKETNET_DEV
static void mo_refund_setup( mo_pair_t * pair )
{
    mo_pair_setup( pair, 1, NULL, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, MO_REFUND_KBPS );
    for ( int i = 0; i < 3; i++ )
    {
        mo_step( pair );
    }
    check( mo_server_slot( pair, 0 )->send_budget_enabled == 1 );
}

// one update at the next time T, with nothing to send: the bucket refilled at
// T, then read

static mo_refund_mark_t mo_refund_mark( mo_pair_t * pair )
{
    pair->time += 0.01;
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_server_send_packets( pair->server );

    const rn_message_connection_t * slot = mo_server_slot( pair, 0 );
    mo_refund_mark_t mark;
    mark.tokens = slot->budget.tokens;
    mark.bytes_spent = slot->budget.bytes_spent;
    mark.refusals = slot->budget.refusals;
    return mark;
}

// the update again at the same T: the failed packet's charge is back

static void mo_refund_update_at_mark( mo_pair_t * pair )
{
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_server_send_packets( pair->server );
}

static void mo_refund_check( mo_pair_t * pair, const mo_refund_mark_t & mark )
{
    const rn_message_connection_t * slot = mo_server_slot( pair, 0 );
    check( pair->net->fail_finish == 0 );                                  // the one failure was taken
    check( fabs( slot->budget.tokens - mark.tokens ) < 1e-6 );             // the bucket is not left short
    check( slot->budget.bytes_spent == mark.bytes_spent );
    check( slot->budget.refusals == mark.refusals );                       // the settle was accepted
}

static void test_message_budget_refunded_on_failed_large_sequenced_finish()
{
    mo_pair_t pair;
    mo_refund_setup( &pair );

    const rocketnet_counters_t * counters = rocketnet_message_server_counters( pair.server );
    const uint64_t failed = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
    const mo_refund_mark_t mark = mo_refund_mark( &pair );

    // sequenced state that fills a whole packet, queued (no frame at start):
    // the write pass takes it as the large sequenced packet, charged its
    // maximum wire bytes at write, and the transport refuses the finish.
    // State larger than one packet is promoted to the reliable twin at start,
    // so this is the one size that reaches the large sequenced packet.

    uint8_t message[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 - 10];          // the record's 10-byte header on channel 1, and the form byte
    mo_fill( message, 41, sizeof( message ) );
    pair.net->refuse_start = true;
    check( rocketnet_message_server_send( pair.server, 0, 1, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, message, (int) sizeof( message ) ) != 0 );
    pair.net->refuse_start = false;
    check( mo_server_slot( &pair, 0 )->large_admitted == 0 );
    pair.net->fail_finish = 1;
    mo_refund_update_at_mark( &pair );

    check( mo_server_slot( &pair, 0 )->large_admitted > 0 );               // planned as the large sequenced packet
    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed + 1 );
    mo_refund_check( &pair, mark );
    mo_pair_teardown( &pair );
}

static void test_message_budget_refunded_on_failed_in_place_finish()
{
    mo_pair_t pair;
    mo_refund_setup( &pair );

    const rocketnet_counters_t * counters = rocketnet_message_server_counters( pair.server );
    const uint64_t failed = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
    const mo_refund_mark_t mark = mo_refund_mark( &pair );

    // no frame for a whole packet, so the message cannot be packed: it takes
    // a frame of its own, written in place, admitted at finish, and the
    // transport refuses the finish

    pair.net->start_limit_bytes = 1000;

    uint8_t * p = rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 900 );
    check( p != NULL );
    check( pair.server->object.in_place == 1 );
    if ( p )
    {
        mo_fill( p, 42, 900 );
    }
    pair.net->fail_finish = 1;
    check( rocketnet_message_server_finish( pair.server, 0, 900 ) != 0 );
    pair.net->start_limit_bytes = 0;
    mo_refund_update_at_mark( &pair );

    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed + 1 );
    check( mo_server_slot( &pair, 0 )->in_place_admitted == 0 );
    mo_refund_check( &pair, mark );
    mo_pair_teardown( &pair );
}

static void test_message_budget_refunded_on_failed_write_pass_finish()
{
    mo_pair_t pair;
    mo_refund_setup( &pair );

    const mo_refund_mark_t mark = mo_refund_mark( &pair );

    // a reliable message on the write pass: the ordered packet is charged,
    // the finish is refused, and the charge comes back

    uint8_t message[1000];
    mo_fill( message, 43, sizeof( message ) );
    check( rocketnet_message_server_send( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) != 0 );
    pair.net->fail_finish = 1;

    const size_t logged = pair.net->server->sent_log.size();
    mo_refund_update_at_mark( &pair );
    check( pair.net->server->sent_log.size() == logged );                  // nothing left
    mo_refund_check( &pair, mark );

    // its records are due again and resent at the next update: the resend
    // is charged once, at the wire bytes it took

    std::vector<mo_received_t> client_got;
    mo_step( &pair, NULL, &client_got );

    const rn_message_connection_t * slot = mo_server_slot( &pair, 0 );
    check( pair.net->server->sent_log.size() == logged + 1 );

    uint64_t wire = 0;
    for ( size_t i = logged; i < pair.net->server->sent_log.size(); i++ )
    {
        wire += (uint64_t) rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, pair.net->server->sent_log[i].bytes );
    }
    check( slot->budget.bytes_spent == mark.bytes_spent + wire );
    for ( int i = 0; i < 3; i++ )
    {
        mo_step( &pair, NULL, &client_got );
    }
    check( client_got.size() == 1 );

    uint32_t id = 0;
    check( client_got.size() == 1 && mo_verify( client_got[0].bytes, &id ) && id == 43 );

    // an unreliable message queued for the write pass (no frame at start):
    // its packed packet's charge comes back the same way

    const mo_refund_mark_t unreliable_mark = mo_refund_mark( &pair );
    pair.net->refuse_start = true;
    check( rocketnet_message_server_send( pair.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
    pair.net->refuse_start = false;
    pair.net->fail_finish = 1;
    mo_refund_update_at_mark( &pair );
    mo_refund_check( &pair, unreliable_mark );
    mo_pair_teardown( &pair );
}

static void test_message_budget_refunded_on_failed_unreliable_flush()
{
    mo_pair_t pair;
    mo_refund_setup( &pair );

    const rocketnet_counters_t * counters = rocketnet_message_server_counters( pair.server );
    const uint64_t failed = mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
    const mo_refund_mark_t mark = mo_refund_mark( &pair );

    // a small unreliable message packed into the open build, admitted when the
    // build is sealed at the flush, and the transport refuses the finish

    uint8_t message[100];
    mo_fill( message, 44, sizeof( message ) );
    check( rocketnet_message_server_send( pair.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
    pair.net->fail_finish = 1;
    mo_refund_update_at_mark( &pair );

    check( mo_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed + 1 );
    mo_refund_check( &pair, mark );
    mo_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_message_budget_refunded_on_failed_large_sequenced_finish );
REGISTER_UNIT_TEST( test_message_budget_refunded_on_failed_in_place_finish );
REGISTER_UNIT_TEST( test_message_budget_refunded_on_failed_write_pass_finish );
REGISTER_UNIT_TEST( test_message_budget_refunded_on_failed_unreliable_flush );
REGISTER_UNIT_TEST( test_message_not_received_ends_the_connection );
REGISTER_UNIT_TEST( test_message_full_drain_delivers_every_message_once_in_order );

REGISTER_UNIT_TEST( test_ordered_and_unreliable_end_to_end_in_memory );
REGISTER_UNIT_TEST( test_server_kicks_exhausted_client_others_unaffected );
REGISTER_UNIT_TEST( test_client_disconnects_itself_on_exhaustion );
REGISTER_UNIT_TEST( test_slot_reuse_starts_clean );
REGISTER_UNIT_TEST( test_lazy_slot_reset_nothing_crosses_reuse );
REGISTER_UNIT_TEST( test_acks_only_from_transport_acked_packets );
REGISTER_UNIT_TEST( test_budget_set_at_create_and_runtime );
REGISTER_UNIT_TEST( test_send_budget_defaults_from_config );
REGISTER_UNIT_TEST( test_send_budget_configured_value_honoured );
REGISTER_UNIT_TEST( test_send_budget_zero_or_negative_is_uncapped );
REGISTER_UNIT_TEST( test_set_send_kbps_zero_or_negative_uncaps );
REGISTER_UNIT_TEST( test_send_budget_message_env_is_not_read );
REGISTER_UNIT_TEST( test_message_objects_no_allocation_after_create );
REGISTER_UNIT_TEST( test_rnslots_witness_replays );
REGISTER_UNIT_TEST( test_message_objects_client_clock_starts_at_first_update );
REGISTER_UNIT_TEST( test_message_send_refused_counts_bad_arguments );
REGISTER_UNIT_TEST( test_unreliable_store_full_exhausts_and_kicks );
REGISTER_UNIT_TEST( test_unreliable_past_limit_promotes_counted_and_not_wedged );
REGISTER_UNIT_TEST( test_unreliable_failed_packet_drops_and_counts_each_record );
REGISTER_UNIT_TEST( test_unreliable_chunks_freed_when_written );
REGISTER_UNIT_TEST( test_unreliable_over_a_lossy_link_arrives_intact_at_most_once );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_send_room_watch_crosses_once_and_rearms_below_half );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_send_room_query_says_no_exactly_when_a_start_exhausts );
REGISTER_UNIT_TEST( test_send_room_warns_in_debug_builds_only );
REGISTER_UNIT_TEST( test_update_time_bad_counted_once_never_stored_or_wedged );

// ---------------------------------------------------------------------------
// the object's clock resumes after a backwards step: the caller's clock
// stepped back, the clamped time stands for that step, and the next forward
// raw step of d advances the clamped time by exactly d, so the object and
// the transport it drives move together again instead of standing still
// until the caller's clock passes the last good time once more
// ---------------------------------------------------------------------------

static void test_message_object_clock_resumes_after_backwards_step()
{
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;

    mo_pair_t pair;
    mo_pair_setup( &pair, 1 );                       // objects seeded at MO_T0

    const uint64_t base = rocketnet_api_misuse_count();

    rocketnet_message_server_t * server = pair.server;

    // the first two updates are the caller's clock moving forward: the
    // object's time is the caller's time, bit for bit

    check( rocketnet_message_server_update( server, MO_T0 ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_api_misuse_count() == base );
    check( server->object.time == MO_T0 );

    check( rocketnet_message_server_update( server, MO_T0 + 1.0 ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_api_misuse_count() == base );
    check( server->object.time == MO_T0 + 1.0 );

    // the caller's clock steps back: the clamped time stands, one misuse is
    // counted, and the raw time is the new one

    check( rocketnet_message_server_update( server, MO_T0 - 10.0 ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_api_misuse_count() == base + 1 );
    check( server->object.time == MO_T0 + 1.0 );

    // the next forward step of half a second advances the clamped time by
    // exactly half a second: the clock resumes with the caller, and the
    // misuse count moved by exactly one

    check( rocketnet_message_server_update( server, MO_T0 - 9.5 ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_api_misuse_count() == base + 1 );
    check( server->object.time == MO_T0 + 1.5 );

    mo_pair_teardown( &pair );

    rocketnet_assert_function( saved_assert );
}

REGISTER_UNIT_TEST( test_update_time_first_update_not_finite_refused );
REGISTER_UNIT_TEST( test_message_object_clock_resumes_after_backwards_step );
REGISTER_UNIT_TEST( test_pending_start_cancelled_by_slot_reset );
REGISTER_UNIT_TEST( test_slot_bound_this_update_keeps_its_first_payload );
REGISTER_UNIT_TEST( test_previous_connection_payload_dropped_after_reconnect );
REGISTER_UNIT_TEST( test_previous_connection_ack_dropped_after_reconnect );
REGISTER_UNIT_TEST( test_same_client_id_reconnect_in_one_update_resets_the_slot );
#endif // #if ROCKETNET_DEV

// A reset increments the carve counter once and clears session metadata.
// That counter counts invocations. It is not a measure of bytes visited.
// Poisoned receive-ring and send-store bytes are still there afterwards.

static uint64_t mo_count_byte( const uint8_t * bytes, uint64_t n, uint8_t value )
{
    uint64_t count = 0;
    for ( uint64_t i = 0; i < n; i++ )
    {
        count += bytes[i] == value;
    }
    return count;
}

static void mo_payload_capacity_config( rocketnet_message_config_t * config, int receive_bytes, int send_bytes )
{
    mo_small_config( config );
    config->reliable_messages_in_flight = 64;
    config->packets_in_flight = 64;
    config->send_queue_messages = 16;
    config->receive_message_buffer_bytes = receive_bytes;
    config->max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config->send_message_buffer_bytes = send_bytes;
}

static int mo_carve_leaves_payload_and_counts_one( const rocketnet_message_config_t * config, uint32_t * ring_bytes, uint64_t * store_bytes, uint32_t * window, uint32_t * packets )
{
    check( rocketnet_message_validate_config( config ) == 1 );

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rn_arena_t * arena = rn_arena_create( NULL, config, 1, &counters );
    check( arena );

    rn_region_t * region = rn_arena_region( arena, 0 );
    check( region && region->receive_ring && region->store_memory );
    *ring_bytes = region->ring_bytes;
    *store_bytes = region->store_bytes;
    *window = region->window;
    *packets = region->packets;

    memset( region->receive_ring, 0xA5, region->ring_bytes );
    memset( region->store_memory, 0x5A, (size_t) region->store_bytes );
    region->channel_receive[0].ordered_read = 3;
    region->sent_packets[0].packet_id = 9;

    const uint64_t carves = region->carves;
    rn_region_reset( region );

    const int one_carve = region->carves == carves + 1;
    const int metadata_reset = region->channel_receive[0].ordered_read == 0 && region->sent_packets[0].packet_id == 0;
    const int ring_kept = mo_count_byte( region->receive_ring, region->ring_bytes, 0xA5 ) == region->ring_bytes;
    const int store_kept = mo_count_byte( region->store_memory, region->store_bytes, 0x5A ) == region->store_bytes;
    rn_arena_destroy( arena );
    check( one_carve );
    check( metadata_reset );
    check( ring_kept );
    check( store_kept );

    return one_carve && metadata_reset && ring_kept && store_kept;
}

static void test_region_reset_keeps_payload_bytes()
{
    rocketnet_message_config_t small_config;
    rocketnet_message_config_t large_config;
    mo_payload_capacity_config( &small_config, 4096, 12288 );
    mo_payload_capacity_config( &large_config, 65536, 65536 );

    uint32_t small_ring = 0, large_ring = 0, small_window = 0, large_window = 0, small_packets = 0, large_packets = 0;
    uint64_t small_store = 0, large_store = 0;
    check( mo_carve_leaves_payload_and_counts_one( &small_config, &small_ring, &small_store, &small_window, &small_packets ) );
    check( mo_carve_leaves_payload_and_counts_one( &large_config, &large_ring, &large_store, &large_window, &large_packets ) );

    check( small_window == large_window );
    check( small_packets == large_packets );
    check( small_ring == 4096 && large_ring == 65536 );
    check( small_store < large_store );
    check( (uint64_t) small_ring + small_store < (uint64_t) large_ring + large_store );
}
REGISTER_UNIT_TEST( test_region_reset_keeps_payload_bytes );
