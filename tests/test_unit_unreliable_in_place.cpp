/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A fitting unreliable message is written in place in the packet buffer, with
// no copy between the message layer and the transport (docs/OUTGOING-BUILD.md,
// tla/RNOutgoingBuild.tla, STANDARD.md _Payload forms_). The pointer start
// returns lies inside the transport frame start_packet handed out, and the
// bytes the game wrote there are the bytes finish_packet is given, at the same
// address: buffer identity, not equal contents.
//
// The cases run the message objects over a fake transport that records every
// frame it hands out (address and capacity) and every frame it is asked to
// finish (address and byte range), and whose packets carry the acks due at
// their finish, as the transport's ack block does. One case runs the same
// checks over the functional tier's in-memory transport (the client and server
// protocols sealing frames in place over the network simulator). Time is an
// argument from 1000.0; nothing sleeps and nothing opens a socket.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_wire.h"

// copies count bytes into a vector; a zero count copies nothing, so a NULL
// source with no bytes is never handed to memmove (gcc 15 -Wnonnull sees
// vector::assign's memmove with a possibly-NULL source)
static void ip_copy_bytes( std::vector<uint8_t> & to, const uint8_t * from, size_t count )
{
    to.resize( count );
    if ( count > 0 )
        memcpy( to.data(), from, count );
}

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// the recording transport pair
// ---------------------------------------------------------------------------

#define IP_FRAMES           16
#define IP_PAYLOAD_INDEX    32
#define IP_T0               1000.0
#define IP_ARENA_BYTES      16384                   // the transport's configured maximum payload: a request above one frame takes its arena

struct ip_buffer_t
{
    const uint8_t * address;            // the payload root handed out, or finished
    int bytes;                          // its capacity when handed out, its payload bytes when finished
    uint64_t id;
};

struct ip_packet_t
{
    uint64_t id;
    int slot;
    std::vector<uint8_t> data;
    std::vector<uint64_t> acks;         // the acks this packet carried (its ack block)
};

struct ip_net_t;

// The bytes of ip_end_t after frames, used through refuse_start: no member
// there needs padding before it, so a tail_pad of the rest to 16 leaves the
// compiler none to add (MSVC C4324).
#define IP_END_TAIL_BYTES ( sizeof( bool ) * IP_FRAMES + sizeof( uint64_t ) * IP_FRAMES + 13 * sizeof( std::vector<int> ) + 2 * sizeof( bool ) )

struct ip_end_t
{
    ip_net_t * net;
    bool is_server;
    uint8_t head_pad[16 - sizeof( ip_net_t * ) - sizeof( bool )];      // frames starts at 16
    alignas( 16 ) uint8_t frames[IP_FRAMES][IP_PAYLOAD_INDEX + IP_ARENA_BYTES];
    bool used[IP_FRAMES];
    uint64_t frame_id[IP_FRAMES];

    std::vector<ip_buffer_t> handed;    // every frame start_packet handed out
    std::vector<ip_buffer_t> finished;  // every frame finish_packet was given
    std::vector<ip_packet_t> sent;      // the wire, as finish_packet staged it

    std::vector<ip_packet_t> inbound;   // the wire to this end
    std::vector<ip_packet_t> batch;
    std::vector<uint8_t *> batch_data;
    std::vector<int> batch_bytes;
    std::vector<int> batch_slot;

    std::vector<uint64_t> acks_due;     // ids this end opened, waiting for a packet to carry them
    std::vector<uint64_t> acks_inbox;   // ids the other end acked in the packets it sent
    std::vector<uint64_t> acked;        // this update's acked batch
    std::vector<int> acked_slot;
    std::vector<uint64_t> acked_all;    // every id the other end acked

    bool connected;
    bool refuse_start;                  // start_packet gives no frame while set (a full ring)
    uint8_t tail_pad[( IP_END_TAIL_BYTES + 15 ) / 16 * 16 - IP_END_TAIL_BYTES];
};

struct ip_net_t
{
    ip_end_t server;
    ip_end_t client;
    uint64_t next_id;
};

// a standalone ack carries acks and no payload: its acks are taken, and it
// is not a payload of the batch
static ip_end_t * ip_taking = NULL;

static bool ip_ack_only_take( const ip_packet_t & packet )
{
    if ( !packet.data.empty() )
        return false;
    for ( size_t k = 0; k < packet.acks.size(); k++ )
        ip_taking->acks_inbox.push_back( packet.acks[k] );
    return true;
}

static void ip_update( void * transport, double time )
{
    (void) time;
    ip_end_t * e = (ip_end_t *) transport;
    ip_end_t * remote = e->is_server ? &e->net->client : &e->net->server;
    e->acked.clear();
    e->acked_slot.clear();
    e->batch.clear();
    e->batch_data.clear();
    e->batch_bytes.clear();
    e->batch_slot.clear();
    e->batch.swap( e->inbound );
    ip_taking = e;
    e->batch.erase( std::remove_if( e->batch.begin(), e->batch.end(), ip_ack_only_take ), e->batch.end() );
    for ( size_t i = 0; i < e->batch.size(); i++ )
    {
        // the packet opens once: the acks it carries are taken, and its id is due an ack
        for ( size_t k = 0; k < e->batch[i].acks.size(); k++ )
            e->acks_inbox.push_back( e->batch[i].acks[k] );
        e->acks_due.push_back( e->batch[i].id );
        e->batch_data.push_back( e->batch[i].data.data() );
        e->batch_bytes.push_back( (int) e->batch[i].data.size() );
        e->batch_slot.push_back( 0 );
    }
    for ( size_t k = 0; k < e->acks_inbox.size(); k++ )
    {
        e->acked.push_back( e->acks_inbox[k] );
        e->acked_slot.push_back( 0 );
        e->acked_all.push_back( e->acks_inbox[k] );
    }
    e->acks_inbox.clear();
    (void) remote;
}

static int ip_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    ip_end_t * e = (ip_end_t *) transport;
    *client_index = e->is_server ? e->batch_slot.data() : NULL;
    *payload_data = e->batch_data.data();
    *payload_bytes = e->batch_bytes.data();
    return (int) e->batch.size();
}

static int ip_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    ip_end_t * e = (ip_end_t *) transport;
    *client_index = e->is_server ? e->acked_slot.data() : NULL;
    *packet_ids = e->acked.data();
    return (int) e->acked.size();
}

static uint8_t * ip_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    ip_end_t * e = (ip_end_t *) transport;
    memset( context, 0, sizeof( *context ) );
    if ( request_bytes < 1 || request_bytes > IP_ARENA_BYTES || e->refuse_start || !e->connected )
        return NULL;
    for ( int f = 0; f < IP_FRAMES; f++ )
    {
        if ( e->used[f] )
            continue;
        e->used[f] = true;
        e->frame_id[f] = ++e->net->next_id;
        context->frame = e->frames[f];
        context->payload_index = IP_PAYLOAD_INDEX;
        const int capacity = request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? IP_ARENA_BYTES : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        context->payload_capacity = capacity;
        context->packet_id = e->frame_id[f];
        context->client_index = client_index;
        ip_buffer_t handed = { e->frames[f] + IP_PAYLOAD_INDEX, capacity, e->frame_id[f] };
        e->handed.push_back( handed );
        return e->frames[f];
    }
    return NULL;
}

static int ip_frame_of( ip_end_t * e, const rocketnet_packet_context_t * context )
{
    for ( int f = 0; f < IP_FRAMES; f++ )
        if ( e->used[f] && context->frame == e->frames[f] )
            return f;
    return -1;
}

static int ip_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    ip_end_t * e = (ip_end_t *) transport;
    const int f = ip_frame_of( e, context );
    check( f >= 0 );
    check( payload_bytes >= 1 && payload_bytes <= IP_ARENA_BYTES );
    e->used[f] = false;

    const uint8_t * payload = e->frames[f] + IP_PAYLOAD_INDEX;
    ip_buffer_t finished = { payload, payload_bytes, e->frame_id[f] };
    e->finished.push_back( finished );

    // the seal: the payload goes on the wire with the acks due now (the ack block)
    ip_packet_t packet;
    packet.id = e->frame_id[f];
    packet.slot = 0;
    ip_copy_bytes( packet.data, payload, (size_t) payload_bytes );
    packet.acks.swap( e->acks_due );
    e->sent.push_back( packet );
    ip_end_t * remote = e->is_server ? &e->net->client : &e->net->server;
    remote->inbound.push_back( packet );
    if ( payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return payload_bytes + 64 * ( payload_bytes / ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 );   // the fragments' wire bytes
    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_bytes );
}

static int ip_max_payload_bytes( void * transport )
{
    (void) transport;
    return IP_ARENA_BYTES;
}

static void ip_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    ip_end_t * e = (ip_end_t *) transport;
    const int f = ip_frame_of( e, context );
    if ( f >= 0 )
        e->used[f] = false;
}

// the flush: acks still due go in a standalone ack, as the transport sends them
static void ip_send_packets( void * transport )
{
    ip_end_t * e = (ip_end_t *) transport;
    if ( e->acks_due.empty() )
        return;
    ip_packet_t packet;
    packet.id = ++e->net->next_id;
    packet.slot = 0;
    packet.acks.swap( e->acks_due );
    ip_end_t * remote = e->is_server ? &e->net->client : &e->net->server;
    remote->inbound.push_back( packet );
}

static uint64_t ip_connection( void * transport, int client_index )
{
    ip_end_t * e = (ip_end_t *) transport;
    (void) client_index;
    return e->connected ? ( e->is_server ? 77 : 1 ) : 0;
}

static void ip_disconnect( void * transport, int client_index )
{
    (void) client_index;
    ( (ip_end_t *) transport )->connected = false;
}

static int ip_state( void * transport )
{
    ip_end_t * e = (ip_end_t *) transport;
    if ( e->is_server )
        return ROCKETNET_SERVER_TRANSPORT_RUNNING;
    return e->connected ? ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED : ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
}

static rn_message_link_t ip_link( ip_end_t * e )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = e;
    link.update = ip_update;
    link.payloads = ip_payloads;
    link.acked = ip_acked;
    link.max_payload_bytes = ip_max_payload_bytes;
    link.start_packet = ip_start_packet;
    link.finish_packet = ip_finish_packet;
    link.abort_packet = ip_abort_packet;
    link.send_packets = ip_send_packets;
    link.connection = ip_connection;
    link.disconnect = ip_disconnect;
    link.state = ip_state;
    return link;
}

struct ip_pair_t
{
    ip_net_t * net;
    rocketnet_message_server_t * server;
    rocketnet_message_client_t * client;
    double time;
};

static void ip_pair_setup( ip_pair_t * pair, int packets_per_update = 8 )
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 4;
    config.packets_per_update = packets_per_update;
    config.message_resend_delay_initial_ms = 100;
    pair->net = new ip_net_t();
    pair->net->server.net = pair->net;
    pair->net->server.is_server = true;
    pair->net->client.net = pair->net;
    pair->net->client.is_server = false;
    pair->net->server.connected = true;
    pair->net->client.connected = true;
    pair->net->next_id = 5000;
    rn_message_link_t server_link = ip_link( &pair->net->server );
    rn_message_link_t client_link = ip_link( &pair->net->client );
    pair->server = (rocketnet_message_server_t *) rn_message_object_create( NULL, &server_link, 1, 1, &config, NULL, 0, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, IP_T0 );
    pair->client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &client_link, 0, 1, &config, NULL, 0, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, IP_T0 );
    check( pair->server && pair->client );
    pair->time = IP_T0;
}

static void ip_pair_teardown( ip_pair_t * pair )
{
    // A teardown-ownership case may already have destroyed either endpoint.
    if ( pair->client ) rocketnet_message_client_destroy( pair->client );
    if ( pair->server ) rocketnet_message_server_destroy( pair->server );
    delete pair->net;
}

struct ip_received_t
{
    int channel;
    std::vector<uint8_t> bytes;
};

// one update of both ends: update, receive every channel, send_packets
static void ip_step( ip_pair_t * pair, std::vector<ip_received_t> * server_got, std::vector<ip_received_t> * client_got, double dt = 0.01 )
{
    pair->time += dt;
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_client_update( pair->client, pair->time ) == ROCKETNET_UPDATE_OK );
    // each end's one receive queue, in batches: each entry names the channel
    // it was delivered on
    for ( ;; )
    {
        static rocketnet_message_batch_t batch;     // 1.5 MB: never on the stack (Windows gives 1 MB)
        const int count = rocketnet_message_server_receive_messages( pair->server, &batch );
        if ( count == 0 )
            break;
        for ( int i = 0; i < batch.num_messages && server_got; i++ )
        {
            ip_received_t r;
            r.channel = batch.channel[i];
            ip_copy_bytes( r.bytes, batch.data[i], (size_t) batch.bytes[i] );
            server_got->push_back( r );
        }
    }
    for ( ;; )
    {
        static rocketnet_message_batch_t batch;     // 1.5 MB: never on the stack (Windows gives 1 MB)
        const int count = rocketnet_message_client_receive_messages( pair->client, &batch );
        if ( count == 0 )
            break;
        for ( int i = 0; i < batch.num_messages && client_got; i++ )
        {
            ip_received_t r;
            r.channel = batch.channel[i];
            ip_copy_bytes( r.bytes, batch.data[i], (size_t) batch.bytes[i] );
            client_got->push_back( r );
        }
    }
    rocketnet_message_server_send_packets( pair->server );
    rocketnet_message_client_send_packets( pair->client );
}

static void ip_fill( uint8_t * data, uint32_t id, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( id * 37 + (uint32_t) i * 11 + 3 );
}

static bool ip_inside( const uint8_t * p, int bytes, const ip_buffer_t & buffer )
{
    return p >= buffer.address && p + bytes <= buffer.address + buffer.bytes;
}

static bool ip_inside_any( const uint8_t * p, int bytes, const std::vector<ip_buffer_t> & buffers )
{
    for ( size_t i = 0; i < buffers.size(); i++ )
        if ( ip_inside( p, bytes, buffers[i] ) )
            return true;
    return false;
}

static uint64_t ip_counter( const rocketnet_counters_t * counters, int counter )
{
    return counters->values[counter];
}

static const int ip_modes[3] = { ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT };

// start, write the pattern, finish on one end; check the start pointer lies in
// the frame handed out at start and in the frame finished, with the pattern at
// that address in what went on the wire
static void ip_send_in_place( ip_pair_t * pair, bool from_server, int mode, int channel, int bytes, uint32_t id )
{
    ip_end_t * e = from_server ? &pair->net->server : &pair->net->client;
    const size_t handed_before = e->handed.size();
    uint8_t * p = from_server ? rocketnet_message_server_start( pair->server, 0, channel, mode, bytes )
                              : rocketnet_message_client_start( pair->client, channel, mode, bytes );
    check( p != NULL );
    check( e->handed.size() == handed_before || e->handed.size() == handed_before + 1 );
    check( !e->handed.empty() && ip_inside( p, bytes, e->handed.back() ) );
    ip_fill( p, id, bytes );
    const uint32_t serial = from_server ? rocketnet_message_server_finish( pair->server, 0, bytes )
                                        : rocketnet_message_client_finish( pair->client, bytes );
    check( serial != 0 );
    // finish commits the record in the same frame; sealing waits for full/flush.
    uint8_t expected[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    ip_fill( expected, id, bytes );
    check( memcmp( p, expected, (size_t) bytes ) == 0 );

}

static bool ip_got( const std::vector<ip_received_t> & got, int channel, uint32_t id, int bytes )
{
    std::vector<uint8_t> expected( (size_t) bytes );
    ip_fill( expected.data(), id, bytes );
    for ( size_t i = 0; i < got.size(); i++ )
        if ( got[i].channel == channel && (int) got[i].bytes.size() == bytes && memcmp( got[i].bytes.data(), expected.data(), (size_t) bytes ) == 0 )
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// the cases
// ---------------------------------------------------------------------------

static void test_unreliable_fit_is_written_in_the_packet()
{
    ip_pair_t pair;
    ip_pair_setup( &pair );
    ip_step( &pair, NULL, NULL );

    for ( int side = 0; side < 2; side++ )
    {
        const bool from_server = side == 1;
        std::vector<ip_received_t> server_got, client_got;
        for ( int m = 0; m < 3; m++ )
        {
            const int bytes = m == 1 ? 1400 : 200 + m;                     // a sequenced one at the frame's edge
            ip_send_in_place( &pair, from_server, ip_modes[m], 2, bytes, (uint32_t) ( 10 * side + m ) );
        }
        if ( from_server ) rocketnet_message_server_send_packets( pair.server );
        else rocketnet_message_client_send_packets( pair.client );
        ip_step( &pair, &server_got, &client_got );
        const std::vector<ip_received_t> & got = from_server ? client_got : server_got;
        check( got.size() == 3 );
        // UNRELIABLE and its receipt twin are received on channel 0, a sequenced one on its own
        check( ip_got( got, 0, (uint32_t) ( 10 * side + 0 ), 200 ) );
        check( ip_got( got, 2, (uint32_t) ( 10 * side + 1 ), 1400 ) );
        check( ip_got( got, 0, (uint32_t) ( 10 * side + 2 ), 202 ) );
    }

    // the in-place path takes no fallback and no region room
    check( ip_counter( rocketnet_message_client_counters( pair.client ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == 0 );
    check( ip_counter( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == 0 );
    ip_pair_teardown( &pair );
}

static void test_unreliable_fit_has_no_message_copy()
{
    ip_pair_t pair;
    ip_pair_setup( &pair );
    ip_step( &pair, NULL, NULL );

    // fitting unreliable sends, written in place, and the send_packets after
    // them: no message-to-packet copy
    const uint64_t before = rn_message_payload_copy_count();
    for ( int m = 0; m < 3; m++ )
    {
        ip_send_in_place( &pair, false, ip_modes[m], 1, 300, 40 + (uint32_t) m );
        ip_send_in_place( &pair, true, ip_modes[m], 1, 300, 50 + (uint32_t) m );
    }
    rocketnet_message_client_send_packets( pair.client );
    rocketnet_message_server_send_packets( pair.server );
    check( rn_message_payload_copy_count() == before );

    // a reliable send keeps its record copy into the frame (resends need the record)
    const uint64_t reliable_before = rn_message_payload_copy_count();
    uint8_t message[300];
    ip_fill( message, 60, (int) sizeof( message ) );
    check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) != 0 );
    rocketnet_message_client_send_packets( pair.client );
    check( rn_message_payload_copy_count() >= reliable_before + 1 );

    ip_pair_teardown( &pair );
}

static void test_unreliable_over_fit_takes_the_message_buffer()
{
    ip_pair_t pair;
    ip_pair_setup( &pair );
    ip_step( &pair, NULL, NULL );
    ip_end_t * e = &pair.net->client;
    rn_message_connection_t * connection = rn_message_object_connection( &pair.client->object, 0 );
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( pair.client );

    // a reliable message, an oversize sequenced one (the large single-record
    // path) and an oversize receipt-bearing one (promoted to the reliable
    // block) take the region record, never a frame
    struct { int mode; int bytes; } over[3] = { { ROCKETNET_RELIABILITY_RELIABLE, 300 }, { ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 3000 }, { ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 2000 } };
    for ( int i = 0; i < 3; i++ )
    {
        const size_t handed_before = e->handed.size();
        uint8_t * p = rocketnet_message_client_start( pair.client, 1, over[i].mode, over[i].bytes );
        check( p != NULL );
        check( e->handed.size() == handed_before );                        // no frame at start
        check( rn_region_contains( connection->region, p, (uint64_t) over[i].bytes ) );
        ip_fill( p, 70 + (uint32_t) i, over[i].bytes );
        check( rocketnet_message_client_finish( pair.client, over[i].bytes ) != 0 );
        rocketnet_message_client_send_packets( pair.client );
        check( !ip_inside_any( p, over[i].bytes, e->finished ) );         // the frame got a copy
    }

    // a fitting one the transport gives no frame falls back, counted, and the
    // next fitting one queues behind it so no mode is reordered
    e->refuse_start = true;
    uint8_t * p = rocketnet_message_client_start( pair.client, 2, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 100 );
    e->refuse_start = false;
    check( p != NULL && rn_region_contains( connection->region, p, 100 ) );
    ip_fill( p, 80, 100 );
    check( rocketnet_message_client_finish( pair.client, 100 ) != 0 );
    check( ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == 1 );
    p = rocketnet_message_client_start( pair.client, 2, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 100 );
    check( p != NULL && rn_region_contains( connection->region, p, 100 ) );
    ip_fill( p, 81, 100 );
    check( rocketnet_message_client_finish( pair.client, 100 ) != 0 );
    check( ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == 2 );

    std::vector<ip_received_t> server_got;
    for ( int i = 0; i < 4; i++ )
        ip_step( &pair, &server_got, NULL );
    check( ip_got( server_got, 2, 80, 100 ) && ip_got( server_got, 2, 81, 100 ) );
    check( ip_got( server_got, 0, 70, 300 ) );                             // RELIABLE (code 2) is received on channel 0
    check( ip_got( server_got, 1, 71, 3000 ) );
    check( ip_got( server_got, 0, 72, 2000 ) );

    // with the queue empty again, the next fitting one is in place
    ip_send_in_place( &pair, false, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 2, 100, 82 );
    ip_pair_teardown( &pair );
}

static void test_mixed_in_place_and_queued_respect_packet_cap()
{
    const int cap = 2;
    ip_pair_t pair;
    ip_pair_setup( &pair, cap );
    ip_step( &pair, NULL, NULL );
    ip_step( &pair, NULL, NULL );                         // settle the initial CONFIG exchange

    ip_end_t * client = &pair.net->client;
    const size_t finished_before = client->finished.size();

    // 1 in-place unreliable packet + 2 queued reliable packets in one update:
    // at most packets_per_update (2) packets reach the fake transport.
    ip_send_in_place( &pair, false, ROCKETNET_RELIABILITY_UNRELIABLE, 1, 100, 90 );
    check( client->finished.size() == finished_before ); // completed record awaits packet flush

    uint8_t reliable_a[1400], reliable_b[1400];
    ip_fill( reliable_a, 91, (int) sizeof( reliable_a ) );
    ip_fill( reliable_b, 92, (int) sizeof( reliable_b ) );
    check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, reliable_a, (int) sizeof( reliable_a ) ) != 0 );
    check( rocketnet_message_client_send( pair.client, 2, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, reliable_b, (int) sizeof( reliable_b ) ) != 0 );

    rocketnet_message_client_send_packets( pair.client );
    check( client->finished.size() - finished_before <= (size_t) cap );
    check( client->finished.size() == finished_before + (size_t) cap );

    // another fitting unreliable send in the same update is also capped and queued
    rn_message_connection_t * connection = rn_message_object_connection( &pair.client->object, 0 );
    uint8_t queued_unr[100];
    ip_fill( queued_unr, 93, (int) sizeof( queued_unr ) );
    check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_UNRELIABLE, queued_unr, (int) sizeof( queued_unr ) ) != 0 );
    check( connection->unreliable_count == 1 );
    rocketnet_message_client_send_packets( pair.client );
    check( client->finished.size() - finished_before <= (size_t) cap );

    // the next update resets the allowance and flushes the remaining queued packets
    std::vector<ip_received_t> server_got;
    ip_step( &pair, &server_got, NULL );
    ip_step( &pair, &server_got, NULL );
    check( ip_got( server_got, 0, 90, 100 ) );
    check( ip_got( server_got, 1, 91, 1400 ) );
    check( ip_got( server_got, 2, 92, 1400 ) );
    check( ip_got( server_got, 0, 93, 100 ) );
    ip_pair_teardown( &pair );
}

static void test_repeated_fill_flush_respects_packet_cap()
{
    const int cap = 2;
    ip_pair_t pair;
    ip_pair_setup( &pair, cap );
    ip_step( &pair, NULL, NULL );
    ip_step( &pair, NULL, NULL );                         // settle the initial CONFIG exchange

    ip_end_t * client = &pair.net->client;
    const size_t finished_before = client->finished.size();

    // Repeatedly filling and calling send_packets within one update (without
    // calling update) must not exceed packets_per_update packets.
    for ( int cycle = 0; cycle < 4; cycle++ )
    {
        uint8_t msg[1400];
        ip_fill( msg, (uint32_t) ( 190 + cycle ), (int) sizeof( msg ) );
        check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, msg, (int) sizeof( msg ) ) != 0 );
        rocketnet_message_client_send_packets( pair.client );
        check( client->finished.size() - finished_before <= (size_t) cap );
    }
    check( client->finished.size() == finished_before + (size_t) cap );

    // In-place start/finish after the cap is exhausted in the same update must
    // also fall back to the queue instead of finishing another packet.
    uint8_t unr[100];
    ip_fill( unr, 199, (int) sizeof( unr ) );
    check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_UNRELIABLE, unr, (int) sizeof( unr ) ) != 0 );
    rocketnet_message_client_send_packets( pair.client );
    check( client->finished.size() - finished_before <= (size_t) cap );

    // After subsequent updates, all queued messages drain under the per-update cap.
    std::vector<ip_received_t> server_got;
    for ( int step = 0; step < 4; step++ )
        ip_step( &pair, &server_got, NULL );
    for ( int cycle = 0; cycle < 4; cycle++ )
        check( ip_got( server_got, 1, (uint32_t) ( 190 + cycle ), 1400 ) );
    check( ip_got( server_got, 0, 199, 100 ) );
    ip_pair_teardown( &pair );
}

// whether the bytes of needle appear anywhere in a packet's payload
static bool ip_carries( const ip_packet_t & packet, const uint8_t * needle, int bytes )
{
    if ( (int) packet.data.size() < bytes )
        return false;
    for ( size_t i = 0; i + (size_t) bytes <= packet.data.size(); i++ )
        if ( memcmp( packet.data.data() + i, needle, (size_t) bytes ) == 0 )
            return true;
    return false;
}

static void test_reliable_not_starved_by_in_place()
{
    // RNOutgoingBuild ReliableSentWithinOneUpdate: with reliable work due, an
    // in-place packet never takes the last of packets_per_update, so the
    // write pass sends that work in the same update
    const int cap = 2;
    ip_pair_t pair;
    ip_pair_setup( &pair, cap );
    ip_step( &pair, NULL, NULL );
    ip_step( &pair, NULL, NULL );                         // settle the initial CONFIG exchange

    ip_end_t * client = &pair.net->client;
    const size_t sent_before = client->sent.size();
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( pair.client );
    const uint64_t fallback_before = ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );

    uint8_t reliable[200];
    ip_fill( reliable, 120, (int) sizeof( reliable ) );
    check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, reliable, (int) sizeof( reliable ) ) != 0 );

    for ( int i = 0; i < cap; i++ )
    {
        uint8_t * p = rocketnet_message_client_start( pair.client, 2, ROCKETNET_RELIABILITY_UNRELIABLE, 64 );
        check( p != NULL );
        ip_fill( p, 121 + (uint32_t) i, 64 );
        check( rocketnet_message_client_finish( pair.client, 64 ) != 0 );
    }
    rocketnet_message_client_send_packets( pair.client );

    check( client->sent.size() - sent_before <= (size_t) cap );
    bool reliable_out = false;
    for ( size_t i = sent_before; i < client->sent.size(); i++ )
        if ( ip_carries( client->sent[i], reliable, (int) sizeof( reliable ) ) )
            reliable_out = true;
    check( reliable_out );
    // Both small records share one frame, leaving the last packet for reliable work.
    check( ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == fallback_before );

    // nothing is lost: the queued unreliable goes in the next update (an
    // UNRELIABLE message is delivered on channel 0)
    std::vector<ip_received_t> server_got;
    ip_step( &pair, &server_got, NULL );
    ip_step( &pair, &server_got, NULL );
    check( ip_got( server_got, 1, 120, (int) sizeof( reliable ) ) );
    check( ip_got( server_got, 0, 121, 64 ) );
    check( ip_got( server_got, 0, 122, 64 ) );
    ip_pair_teardown( &pair );
}

static int ip_asserts = 0;
static void ip_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    ip_asserts++;
}

static void test_in_place_unreliable_keeps_acks_and_order()
{
    ip_pair_t pair;
    ip_pair_setup( &pair );
    ip_step( &pair, NULL, NULL );
    ip_step( &pair, NULL, NULL );
    ip_end_t * client = &pair.net->client;
    ip_end_t * server = &pair.net->server;

    // the server's reliable packets reach the client: acks are due there
    uint8_t message[200];
    for ( uint32_t i = 0; i < 3; i++ )
    {
        ip_fill( message, 100 + i, (int) sizeof( message ) );
        check( rocketnet_message_server_send( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) != 0 );
        rocketnet_message_server_send_packets( pair.server );
    }
    pair.time += 0.01;
    check( rocketnet_message_client_update( pair.client, pair.time ) == ROCKETNET_UPDATE_OK );
    int client_got_early = 0;
    {
        const uint8_t * data;
        int bytes;
        while ( rocketnet_message_client_receive( pair.client, NULL, &data, &bytes ) )
        {
            client_got_early++;
            rocketnet_message_client_release( pair.client, data );
        }
    }
    check( client_got_early == 3 );
    std::vector<uint64_t> due = client->acks_due;
    check( due.size() >= 3 );

    // the client: reliable messages queued, then in-place unreliable sends of
    // every mode, interleaved, then more reliable ones
    for ( uint32_t i = 0; i < 2; i++ )
    {
        ip_fill( message, 200 + i, (int) sizeof( message ) );
        check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) != 0 );
    }
    ip_send_in_place( &pair, false, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 2, 120, 300 );
    rocketnet_message_client_send_packets( pair.client );
    check( client->acks_due.empty() );                                     // every ack due rode a sealed packet
    for ( size_t i = 0; i < due.size(); i++ )
    {
        bool found = false;
        for ( const ip_packet_t & packet : client->sent )
            found |= std::find( packet.acks.begin(), packet.acks.end(), due[i] ) != packet.acks.end();
        check( found );
    }
    ip_send_in_place( &pair, false, ROCKETNET_RELIABILITY_UNRELIABLE, 3, 120, 301 );
    ip_send_in_place( &pair, false, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 2, 120, 302 );
    ip_send_in_place( &pair, false, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 3, 120, 303 );
    for ( uint32_t i = 2; i < 4; i++ )
    {
        ip_fill( message, 200 + i, (int) sizeof( message ) );
        check( rocketnet_message_client_send( pair.client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, (int) sizeof( message ) ) != 0 );
    }

    std::vector<ip_received_t> server_got, client_got;
    rocketnet_message_receipt_t receipts[8];
    int receipt_count = 0;
    for ( int i = 0; i < 6; i++ )
    {
        ip_step( &pair, &server_got, &client_got );
        const int available = rocketnet_message_client_receipts( pair.client, receipts + receipt_count, 8 - receipt_count );
        check( available >= 0 && available <= 8 - receipt_count );
        receipt_count += available;
    }

    // the server took the acks: every reliable packet it sent is acked
    for ( size_t i = 0; i < due.size(); i++ )
        check( std::find( server->acked_all.begin(), server->acked_all.end(), due[i] ) != server->acked_all.end() );

    // reliable order on channel 1, sequenced order on channel 2, every unreliable message once
    std::vector<uint32_t> reliable, sequenced;
    int unreliable = 0;
    for ( size_t i = 0; i < server_got.size(); i++ )
    {
        uint32_t first = 0;
        for ( uint32_t id = 200; id < 304; id++ )
        {
            const int bytes = (int) server_got[i].bytes.size();
            std::vector<uint8_t> expected( (size_t) bytes );
            ip_fill( expected.data(), id, bytes );
            if ( memcmp( server_got[i].bytes.data(), expected.data(), (size_t) bytes ) == 0 )
            {
                first = id;
                break;
            }
        }
        if ( server_got[i].channel == 1 )
            reliable.push_back( first );
        else if ( server_got[i].channel == 2 )
            sequenced.push_back( first );
        else
            unreliable++;
    }
    check( reliable.size() == 4 && reliable[0] == 200 && reliable[1] == 201 && reliable[2] == 202 && reliable[3] == 203 );
    check( sequenced.size() == 2 && sequenced[0] == 300 && sequenced[1] == 302 );
    check( unreliable == 2 );
    check( receipt_count == 1 && receipts[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );

    // an in-place start left pending at send_packets is aborted there: the
    // transport forbids a flush with a fill open, and its later finish asserts
    // in Debug and returns 0 in Release
    const uint64_t refused = ip_counter( rocketnet_message_client_counters( pair.client ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    uint8_t * p = rocketnet_message_client_start( pair.client, 2, ROCKETNET_RELIABILITY_UNRELIABLE, 50 );
    check( p != NULL );
    rocketnet_message_client_send_packets( pair.client );
    check( ip_counter( rocketnet_message_client_counters( pair.client ), ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused + 1 );
    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    ip_asserts = 0;
    rocketnet_assert_function( ip_assert_hook );
    check( rocketnet_message_client_finish( pair.client, 50 ) == 0 );
    rocketnet_assert_function( saved_assert );
    check( ip_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    // and abort gives the frame back
    p = rocketnet_message_client_start( pair.client, 2, ROCKETNET_RELIABILITY_UNRELIABLE, 50 );
    check( p != NULL );
    rocketnet_message_client_abort( pair.client );
    for ( int f = 0; f < IP_FRAMES; f++ )
        check( !client->used[f] );

    ip_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// the functional tier's in-memory transport: the client and server protocols
// seal each frame in place over the network simulator. Its harness mints the
// connect token with the development key writer, which the release library
// the unit suite links does not carry; the same token is written here with the
// test vectors' keys, which are the development keys.
// ---------------------------------------------------------------------------

static enum rocketnet_status_t ip_write_connect_token( const char * server_address_string, uint64_t client_id, uint64_t current_timestamp, uint8_t * connect_token, size_t connect_token_bytes );
#define rocketnet_write_test_connect_token ip_write_connect_token
#include "../functional/channel_harness.h"
#undef rocketnet_write_test_connect_token

static enum rocketnet_status_t ip_write_connect_token( const char * server_address_string, uint64_t client_id, uint64_t current_timestamp, uint8_t * connect_token, size_t connect_token_bytes )
{
    if ( connect_token_bytes < ROCKETNET_CONNECT_TOKEN_BYTES )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    struct rocketnet_address_t server_address;
    if ( !rocketnet_address_parse( &server_address, server_address_string ) || server_address.type != ROCKETNET_ADDRESS_IPV4 )
        return ROCKETNET_STATUS_INVALID_ADDRESS;
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = current_timestamp;
    signed_fields.expire_timestamp = current_timestamp + ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS;
    signed_fields.timeout_seconds = ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS;
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    uint8_t * p = signed_fields.ipv4_address;
    rocketnet_write_bytes( &p, server_address.data.ipv4, 4 );
    rocketnet_write_uint16( &p, server_address.port );
    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = client_id;
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        private_fields.client_to_server_key[i] = (uint8_t) ( 0x31 + i );
        private_fields.server_to_client_key[i] = (uint8_t) ( 0x75 + i );
    }
    return rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, connect_token ) ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_CRYPTO_INIT_FAILED;
}

static void test_unreliable_fit_in_place_every_transport()
{
    rocketnet_channel_harness_t h;
    check( rocketnet_channel_harness_create( &h ) );
    check( rocketnet_channel_harness_connect( &h, 600 ) );
    for ( int i = 0; i < 4; i++ )
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );                 // the CONFIG exchange settles

    for ( int side = 0; side < 2; side++ )
    {
        const bool from_server = side == 1;
        rocketnet_channel_endpoint_t * e = from_server ? &h.server_endpoint : &h.client_endpoint;
        for ( int m = 0; m < 3; m++ )
        {
            const int bytes = 64 + m;
            uint8_t * p = from_server ? rocketnet_message_server_start( h.message_server, h.client_slot, 1, ip_modes[m], bytes )
                                      : rocketnet_message_client_start( h.message_client, 1, ip_modes[m], bytes );
            check( p != NULL );
            const int slot = e->pending_frame;
            check( slot >= 0 );                                            // start holds a transport frame
            const uint8_t * root = e->frame[slot] + ROCKETNET_FRAME_PAYLOAD_OFFSET;
            check( p >= root && p + bytes <= root + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            ip_fill( p, 500 + (uint32_t) ( 10 * side + m ), bytes );
            const int staged = e->outgoing_count;
            check( ( from_server ? rocketnet_message_server_finish( h.message_server, h.client_slot, bytes )
                                 : rocketnet_message_client_finish( h.message_client, bytes ) ) != 0 );
            check( e->pending_frame == slot && e->outgoing_count == staged ); // committed records retain the open frame
        }

        if ( from_server ) rocketnet_message_server_send_packets( h.message_server );
        else rocketnet_message_client_send_packets( h.message_client );
        check( e->pending_frame == -1 );
        uint8_t expected[128];
        int found = 0;
        for ( int i = 0; i < 60 && found < 3; i++ )
        {
            rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
            rocketnet_channel_delivery_t d;
            for ( int channel = 0; channel < 2; channel++ )
                while ( from_server ? rocketnet_channel_harness_client_collect( &h, channel, &d ) : rocketnet_channel_harness_server_collect( &h, channel, &d ) )
                {
                    for ( int m = 0; m < 3; m++ )
                    {
                        ip_fill( expected, 500 + (uint32_t) ( 10 * side + m ), 64 + m );
                        if ( d.bytes == 64 + m && memcmp( d.data, expected, (size_t) d.bytes ) == 0 )
                            found++;
                    }
                }
        }
        check( found == 3 );
    }
    rocketnet_channel_harness_destroy( &h );
}

// A burst of start/finish sends as fast as the caller can make them, many
// thousands across several updates, with the reliable bytes in flight well
// under the send store (send_message_buffer_bytes, send_queue_messages and
// reliable_messages_in_flight at their defaults): the connection stays up, no exhaustion
// or refusal counter moves, and every message is delivered, in order, in a
// later update. The client's budget is uncapped so the packet allowance and
// the 16-record packet bound are the only pacing.
static void test_message_burst_start_finish_delivers_every_message()
{
    const int updates = 60;
    const int per_update = 100;                         // under packets_per_update (8) x 16 records
    const int bytes = 32;
    ip_pair_t pair;
    ip_pair_setup( &pair );
    ip_step( &pair, NULL, NULL );
    ip_step( &pair, NULL, NULL );                         // settle the initial CONFIG exchange
    check( rocketnet_message_client_set_send_kbps( pair.client, 0 ) == 1 );

    const rocketnet_counters_t * counters = rocketnet_message_client_counters( pair.client );
    const int watched[5] = { ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED,
                             ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET };
    uint64_t before[5];
    for ( int i = 0; i < 5; i++ )
        before[i] = ip_counter( counters, watched[i] );

    std::vector<ip_received_t> server_got;
    uint32_t id = 0;
    for ( int u = 0; u < updates; u++ )
    {
        for ( int i = 0; i < per_update; i++ )
        {
            uint8_t * p = rocketnet_message_client_start( pair.client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
            check( p != NULL );
            ip_fill( p, id, bytes );
            check( rocketnet_message_client_finish( pair.client, bytes ) != 0 );
            id++;
        }
        rocketnet_message_client_send_packets( pair.client );
        ip_step( &pair, &server_got, NULL );
        check( rocketnet_message_client_disconnect_reason( pair.client ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    }
    for ( int u = 0; u < 20 && server_got.size() < (size_t) id; u++ )
        ip_step( &pair, &server_got, NULL );

    check( server_got.size() == (size_t) id );
    std::vector<uint8_t> expected( (size_t) bytes );
    for ( uint32_t k = 0; k < id && k < server_got.size(); k++ )
    {
        ip_fill( expected.data(), k, bytes );
        check( server_got[k].channel == 1 );
        check( server_got[k].bytes.size() == (size_t) bytes && memcmp( server_got[k].bytes.data(), expected.data(), (size_t) bytes ) == 0 );
    }
    for ( int i = 0; i < 5; i++ )
        check( ip_counter( counters, watched[i] ) == before[i] );
    check( rocketnet_message_client_disconnect_reason( pair.client ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_message_server_disconnect_reason( pair.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( pair.net->client.connected && pair.net->server.connected );
    ip_pair_teardown( &pair );
}

// Past the update's packet allowance a fitting unreliable start is not
// refused: it falls back to the region (rocketnet_endpoint.h
// RN_ENDPOINT_IN_PLACE_QUEUED, "or the update's packet allowance is spent";
// counters.h MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK), finish queues it with a
// serial, no further packet goes out in that update, and the next update's
// write pass sends it.
static void test_start_past_packet_allowance_falls_back_to_the_region()
{
    const int cap = 2;
    const int extra = 10;
    ip_pair_t pair;
    ip_pair_setup( &pair, cap );
    ip_step( &pair, NULL, NULL );
    ip_step( &pair, NULL, NULL );                         // settle the initial CONFIG exchange

    ip_end_t * client = &pair.net->client;
    rn_message_connection_t * connection = rn_message_object_connection( &pair.client->object, 0 );
    const rocketnet_counters_t * counters = rocketnet_message_client_counters( pair.client );
    const uint64_t fallback_before = ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );
    const uint64_t refused_before = ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );

    // the allowance: cap in-place packets, nothing reliable due
    for ( int i = 0; i < cap; i++ )
    {
        ip_send_in_place( &pair, false, ROCKETNET_RELIABILITY_UNRELIABLE, 1, 64, 300 + (uint32_t) i );
        rocketnet_message_client_send_packets( pair.client ); // spend a packet, not merely append a record
    }
    const size_t finished_spent = client->finished.size();
    check( ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == fallback_before );

    // past it: start returns room in the region, never a frame and never NULL
    for ( int i = 0; i < extra; i++ )
    {
        const size_t handed_before = client->handed.size();
        uint8_t * p = rocketnet_message_client_start( pair.client, 1, ROCKETNET_RELIABILITY_UNRELIABLE, 64 );
        check( p != NULL );
        check( client->handed.size() == handed_before );
        check( p != NULL && rn_region_contains( connection->region, p, 64 ) );
        if ( p )
            ip_fill( p, 310 + (uint32_t) i, 64 );
        check( rocketnet_message_client_finish( pair.client, 64 ) != 0 );
    }
    check( ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) == fallback_before + (uint64_t) extra );
    check( ip_counter( counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused_before );
    rocketnet_message_client_send_packets( pair.client );
    check( client->finished.size() == finished_spent );  // the allowance stays spent in this update

    std::vector<ip_received_t> server_got;
    ip_step( &pair, &server_got, NULL );
    ip_step( &pair, &server_got, NULL );
    for ( int i = 0; i < cap; i++ )
        check( ip_got( server_got, 0, 300 + (uint32_t) i, 64 ) );
    for ( int i = 0; i < extra; i++ )
        check( ip_got( server_got, 0, 310 + (uint32_t) i, 64 ) );
    check( rocketnet_message_client_disconnect_reason( pair.client ) == ROCKETNET_DISCONNECT_REASON_NONE );
    ip_pair_teardown( &pair );
}


// Abort and short finish must reuse the uncommitted suffix without moving the
// committed prefix. Verify the final frame identity as well as delivery.
static void test_unreliable_packing_abort_and_short_finish()
{
    for ( int side = 0; side < 2; ++side )
    {
        ip_pair_t pair;
        ip_pair_setup( &pair );
        ip_step( &pair, NULL, NULL );
        ip_step( &pair, NULL, NULL );
        const bool server = side != 0;
        ip_end_t * end = server ? &pair.net->server : &pair.net->client;
        auto start = [&]( int n ) { return server ? rocketnet_message_server_start( pair.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, n ) : rocketnet_message_client_start( pair.client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, n ); };
        auto finish = [&]( int n ) { return server ? rocketnet_message_server_finish( pair.server, 0, n ) : rocketnet_message_client_finish( pair.client, n ); };
        auto abort = [&]() { if ( server ) rocketnet_message_server_abort( pair.server, 0 ); else rocketnet_message_client_abort( pair.client ); };
        const uint64_t copies = rn_message_payload_copy_count();
        const size_t frames = end->finished.size();
        uint8_t * first = start( 128 );
        check( first ); ip_fill( first, 1, 7 ); check( finish( 7 ) );
        uint8_t * suffix = start( 200 );
        check( suffix == first + 7 + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES );
        memset( suffix, 0xee, 200 ); abort();
        uint8_t * second = start( 100 );
        check( second == suffix ); ip_fill( second, 2, 11 ); check( finish( 11 ) );
        uint8_t * bad = start( 8 );
        check( bad ); memset( bad, 0xdd, 8 ); check( finish( 9 ) == 0 );
        uint8_t * third = start( 8 );
        check( third == bad ); ip_fill( third, 3, 8 ); check( finish( 8 ) );
        if ( server ) rocketnet_message_server_send_packets( pair.server );
        else rocketnet_message_client_send_packets( pair.client );
        check( end->finished.size() == frames + 1 );
        check( ip_inside( first, 7, end->finished.back() ) );
        check( ip_inside( second, 11, end->finished.back() ) );
        check( ip_inside( third, 8, end->finished.back() ) );
        check( end->finished.back().bytes == 7 + 11 + 8 + 3 * ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1 );
        check( rn_message_payload_copy_count() == copies );
        std::vector<ip_received_t> got_server, got_client;
        for ( int i = 0; i < 3; ++i ) ip_step( &pair, &got_server, &got_client );
        const auto & got = server ? got_client : got_server;
        check( got.size() == 3 );
        check( ip_got( got, 0, 1, 7 ) && ip_got( got, 0, 2, 11 ) && ip_got( got, 0, 3, 8 ) );
        // An empty reservation and a committed frame both release on teardown.
        check( start( 32 ) ); abort();
        for ( bool used : end->used ) check( !used );
        ip_send_in_place( &pair, server, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 32, 4 );
        if ( server ) { rocketnet_message_server_destroy( pair.server ); pair.server = NULL; }
        else { rocketnet_message_client_destroy( pair.client ); pair.client = NULL; }
        for ( bool used : end->used ) check( !used );
        ip_pair_teardown( &pair );
    }
}

static void test_unreliable_packing_receipt_boundary()
{
    ip_pair_t pair;
    ip_pair_setup( &pair );
    ip_step( &pair, NULL, NULL );
    ip_step( &pair, NULL, NULL );
    const size_t frames = pair.net->client.finished.size();
    uint32_t serials[33];
    const uint64_t copies = rn_message_payload_copy_count();
    for ( int i = 0; i < 33; ++i )
    {
        uint8_t * p = rocketnet_message_client_start( pair.client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 4 );
        check( p ); ip_fill( p, (uint32_t) i, 4 );
        serials[i] = rocketnet_message_client_finish( pair.client, 4 ); check( serials[i] );
    }
    rocketnet_message_client_send_packets( pair.client );
    check( pair.net->client.finished.size() == frames + 3 ); // bounded receipt map: 16 + 16 + 1
    check( rn_message_payload_copy_count() == copies );
    std::vector<ip_received_t> got;
    bool seen[33] = {};
    int count = 0;
    for ( int step = 0; step < 10; ++step )
    {
        ip_step( &pair, &got, NULL );
        rocketnet_message_receipt_t receipts[64];
        const int n = rocketnet_message_client_receipts( pair.client, receipts, 64 );
        for ( int j = 0; j < n; ++j )
        {
            int match = -1;
            for ( int i = 0; i < 33; ++i ) if ( receipts[j].serial == serials[i] ) match = i;
            check( match >= 0 && !seen[match] );
            check( receipts[j].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            seen[match] = true; ++count;
        }
    }
    check( got.size() == 33 && count == 33 );
    for ( int i = 0; i < 33; ++i ) check( ip_got( got, 0, (uint32_t) i, 4 ) );
    ip_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_unreliable_packing_abort_and_short_finish );
REGISTER_UNIT_TEST( test_unreliable_packing_receipt_boundary );

REGISTER_UNIT_TEST( test_unreliable_fit_is_written_in_the_packet );
REGISTER_UNIT_TEST( test_unreliable_fit_in_place_every_transport );
REGISTER_UNIT_TEST( test_unreliable_fit_has_no_message_copy );
REGISTER_UNIT_TEST( test_unreliable_over_fit_takes_the_message_buffer );
REGISTER_UNIT_TEST( test_in_place_unreliable_keeps_acks_and_order );
REGISTER_UNIT_TEST( test_mixed_in_place_and_queued_respect_packet_cap );
REGISTER_UNIT_TEST( test_repeated_fill_flush_respects_packet_cap );
REGISTER_UNIT_TEST( test_reliable_not_starved_by_in_place );
REGISTER_UNIT_TEST( test_message_burst_start_finish_delivers_every_message );
REGISTER_UNIT_TEST( test_start_past_packet_allowance_falls_back_to_the_region );
