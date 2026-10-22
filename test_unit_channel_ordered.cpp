/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RELIABLE_ORDERED, code 3, end to end (docs/CHANNELS.md 6 to 9;
// tla/RNChannel.tla): small messages packed and resent until a transport packet
// ack covers them, large messages cut into chunks that travel one to a packet
// and are placed in the receiver's ring by index, one ordering sequence for
// both. Two connections' message state exchange packets over
// rocketnet_network_simulator (loss, duplication, reorder) or over a scripted
// wire. Time is only ever an argument: nothing sleeps, nothing opens a socket.
// The transport is played by the harness: it assigns each written packet its
// id, "opens" each received packet once (replay suppression), and acks the
// opened ids after the update's deliveries are drained (R1, P2, P6).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <vector>

#include "test_unit_registry.h"
#include "test/plan_rss.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_address.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_counters.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_network_simulator.h"
#include "sdk/rocketnet_packet_builder.h"
#include "sdk/rocketnet_send_budget.h"

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// messages: an id in the first 8 bytes, the rest a function of (id, length), so
// every delivery is verified byte for byte without keeping the sent bytes
// ---------------------------------------------------------------------------

static uint8_t pattern_byte( uint64_t id, uint32_t i )
{
    return (uint8_t) ( id * 131 + i * 7 + ( i >> 8 ) * 13 + 5 );
}

static void pattern_fill( uint8_t * data, uint64_t id, uint32_t bytes )
{
    for ( uint32_t i = 0; i < bytes; i++ )
        data[i] = pattern_byte( id, i );
    if ( bytes >= 8 )
        memcpy( data, &id, 8 );
}

static bool pattern_verify( const uint8_t * data, uint32_t bytes, uint64_t * out_id )
{
    if ( bytes < 8 )
        return false;
    uint64_t id;
    memcpy( &id, data, 8 );
    for ( uint32_t i = 8; i < bytes; i++ )
        if ( data[i] != pattern_byte( id, i ) )
            return false;
    *out_id = id;
    return true;
}

// ---------------------------------------------------------------------------
// one connection's message state, and the transport played around it
// ---------------------------------------------------------------------------

struct wire_packet_t
{
    uint64_t id;
    int bytes;
    uint8_t bytes_pad[16 - sizeof( uint64_t ) - sizeof( int )];                 // explicit: data starts 16-aligned
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t tail_pad[16 - ROCKETNET_MAX_PACKET_PAYLOAD_BYTES % 16];                     // explicit: the size rounds up to 16
};
static_assert( sizeof( wire_packet_t ) == 16 + ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 15 ) / 16 * 16, "no implicit padding" );
static_assert( alignof( wire_packet_t ) == 16, "data is 16-aligned" );

struct delivered_t
{
    uint32_t channel;
    uint64_t id;
    uint32_t bytes;
    const uint8_t * view;           // where the application was handed the bytes
};

struct node_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    rocketnet_send_budget_t budget;
    uint64_t next_packet_id;                    // the transport's sequence
    uint64_t next_message_id;
    std::set<uint64_t> opened;                  // replay suppression: a packet id opens once
    std::vector<uint64_t> pending_acks;         // opened since the last ack went out
    std::vector<delivered_t> delivered;         // every delivery, in order
    std::vector<uint64_t> sent[ROCKETNET_MAX_CHANNELS];
    std::vector<uint64_t> got[ROCKETNET_MAX_CHANNELS];
    int packets_written;
    int violations;
};

static void node_config( rocketnet_message_config_t * config, int window, int ring, int max_message, int store )
{
    rocketnet_message_default_config( config );
    config->reliable_messages_in_flight = window;
    config->receive_message_buffer_bytes = ring;
    config->max_message_bytes = max_message;
    config->send_message_buffer_bytes = store;
    config->message_resend_delay_min_ms = 20;
    config->message_resend_delay_initial_ms = 100;
    config->message_resend_delay_max_ms = 400;
    config->reliable_message_timeout_ms = 120000;
}

static void node_setup( node_t * node, const rocketnet_message_config_t * config, int max_packets, bool budgeted = false, uint32_t kbps = 10000 )
{
    node->arena = rn_arena_create( NULL, config, 1, &node->counters );
    check( node->arena != NULL );
    node->region = rn_arena_region( node->arena, 0 );
    if ( budgeted )
        check( rocketnet_send_budget_init( &node->budget, kbps, 0.0 ) );
    check( rn_ordered_init( &node->ordered, node->region, config, budgeted ? &node->budget : NULL, max_packets, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    node->next_packet_id = 1;
    node->next_message_id = 1;
}

static void node_teardown( node_t * node )
{
    rn_arena_destroy( node->arena );
}

static node_t * node_new( const rocketnet_message_config_t * config, int max_packets, bool budgeted = false, uint32_t kbps = 10000 )
{
    node_t * node = new node_t();
    memset( &node->counters, 0, sizeof( node->counters ) );
    node->packets_written = 0;
    node->violations = 0;
    node_setup( node, config, max_packets, budgeted, kbps );
    return node;
}

static void node_free( node_t * node )
{
    node_teardown( node );
    delete node;
}

static uint64_t counter( const node_t * node, int which )
{
    return node->counters.values[which];
}

// the application: start, write once in place, finish. Returns the message id,
// or 0 when start gave no room.

static uint64_t send_message( node_t * node, uint32_t channel, uint32_t bytes, uint8_t ** out_pointer = NULL )
{
    uint8_t * p = rn_channels_start( &node->ordered.channels, channel, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, bytes );
    if ( !p )
        return 0;
    const uint64_t id = node->next_message_id++;
    pattern_fill( p, id, bytes );
    check( rn_channels_finish( &node->ordered.channels, bytes ) );
    node->sent[channel].push_back( id );
    if ( out_pointer )
        *out_pointer = p;
    return id;
}

// one update's packets: written, given the transport's ids, committed

static int write_packets( node_t * node, double now, wire_packet_t * out, int max_out )
{
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    const int limit = max_out < ROCKETNET_PACKET_BUILDER_MAX_PACKETS ? max_out : ROCKETNET_PACKET_BUILDER_MAX_PACKETS;
    for ( int i = 0; i < limit; i++ )
        pointers[i] = frames[i];

    const int n = rn_ordered_write( &node->ordered, now, pointers, limit );
    for ( int p = 0; p < n; p++ )
    {
        out[p].id = node->next_packet_id++;
        out[p].bytes = (int) node->ordered.builder.packet[p].payload_bytes;
        memcpy( out[p].data, frames[p], (size_t) out[p].bytes );
        rn_ordered_commit( &node->ordered, p, out[p].id, now );
        // a sealed packet's wire bytes are at most what was admitted; settle the difference
        if ( node->ordered.budget )
            check( rn_ordered_settle( &node->ordered, p, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, out[p].bytes ) ) );
        node->packets_written++;
    }
    return n;
}

// the transport opens a packet (once per id), the message layer takes the
// payload, the update's deliveries are drained and verified, the update ends.
// Returns false for a packet the transport suppressed as a replay.

static bool open_packet( node_t * node, const wire_packet_t * packet )
{
    if ( !node->opened.insert( packet->id ).second )
        return false;
    node->pending_acks.push_back( packet->id );

    const int violation = rn_ordered_receive( &node->ordered, packet->data, packet->bytes );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        node->violations++;

    const uint32_t n = rn_ordered_deliveries( &node->ordered );
    for ( uint32_t i = 0; i < n; i++ )
    {
        const rn_delivery_t & delivery = node->region->deliveries[i];
        delivered_t d;
        d.channel = delivery.channel;
        d.bytes = (uint32_t) delivery.bytes;
        d.view = delivery.data;
        check( delivery.code == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED );
        check( pattern_verify( delivery.data, d.bytes, &d.id ) );
        node->delivered.push_back( d );
        node->got[d.channel].push_back( d.id );
    }
    rn_ordered_end_update( &node->ordered );
    return true;
}

static void take_acks( node_t * node, std::vector<uint64_t> * out )
{
    out->insert( out->end(), node->pending_acks.begin(), node->pending_acks.end() );
    node->pending_acks.clear();
}

static void feed_acks( node_t * node, const std::vector<uint64_t> & ids, double now )
{
    if ( !ids.empty() )
        rn_ordered_acks( &node->ordered, ids.data(), (uint32_t) ids.size(), now );
}

// ---------------------------------------------------------------------------
// the simulated wire: data packets and ack lists through the network simulator
// ---------------------------------------------------------------------------

struct sim_t
{
    rocketnet_network_simulator_t * wire;
    rocketnet_address_t address[2];
    double now;
};

static void sim_setup( sim_t * sim, float loss, float duplicate, uint64_t seed )
{
    sim->wire = rocketnet_network_simulator_create( NULL, 16384, seed );
    check( sim->wire != NULL );
    check( rocketnet_address_parse( &sim->address[0], "127.0.0.1:40000" ) );
    check( rocketnet_address_parse( &sim->address[1], "127.0.0.1:40001" ) );
    rocketnet_network_simulator_config( sim->wire, 0.015, 0.03, loss, duplicate );
    sim->now = 1.0;
}

static void sim_teardown( sim_t * sim )
{
    rocketnet_network_simulator_destroy( sim->wire );
}

// kind 0: data, then the id and the payload. kind 1: acks, a count, then ids.

static void sim_send_data( sim_t * sim, int from, const wire_packet_t * packet )
{
    alignas( 16 ) uint8_t bytes[16 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    bytes[0] = 0;
    memcpy( bytes + 1, &packet->id, 8 );
    memcpy( bytes + 9, packet->data, (size_t) packet->bytes );
    rocketnet_network_simulator_send( sim->wire, &sim->address[from], &sim->address[1 - from], bytes, 9 + packet->bytes, sim->now );
}

static void sim_send_acks( sim_t * sim, int from, const std::vector<uint64_t> & ids )
{
    size_t at = 0;
    while ( at < ids.size() )
    {
        const size_t count = ids.size() - at < 64 ? ids.size() - at : 64;
        uint8_t bytes[1 + 4 + 64 * 8];
        bytes[0] = 1;
        const uint32_t n = (uint32_t) count;
        memcpy( bytes + 1, &n, 4 );
        memcpy( bytes + 5, ids.data() + at, count * 8 );
        rocketnet_network_simulator_send( sim->wire, &sim->address[from], &sim->address[1 - from], bytes, (int) ( 5 + count * 8 ), sim->now );
        at += count;
    }
}

// one tick of the whole system: both connections write, the wire delivers what
// is due, each side opens, drains, and acks

static void sim_tick( sim_t * sim, node_t * nodes[2], double dt )
{
    sim->now += dt;

    for ( int side = 0; side < 2; side++ )
    {
        wire_packet_t out[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
        const int n = write_packets( nodes[side], sim->now, out, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
        for ( int p = 0; p < n; p++ )
            sim_send_data( sim, side, &out[p] );
    }

    for ( int side = 0; side < 2; side++ )
    {
        alignas( 16 ) static uint8_t bytes[16 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        rocketnet_address_t from;
        std::vector<uint64_t> acks_in;
        for ( int got = rocketnet_network_simulator_receive( sim->wire, &sim->address[side], sim->now, &from, bytes );
              got > 0;
              got = rocketnet_network_simulator_receive( sim->wire, &sim->address[side], sim->now, &from, bytes ) )
        {
            if ( bytes[0] == 0 )
            {
                wire_packet_t packet;
                memcpy( &packet.id, bytes + 1, 8 );
                packet.bytes = got - 9;
                memcpy( packet.data, bytes + 9, (size_t) packet.bytes );
                open_packet( nodes[side], &packet );
            }
            else
            {
                uint32_t n;
                memcpy( &n, bytes + 1, 4 );
                for ( uint32_t i = 0; i < n; i++ )
                {
                    uint64_t id;
                    memcpy( &id, bytes + 5 + i * 8, 8 );
                    acks_in.push_back( id );
                }
            }
        }
        feed_acks( nodes[side], acks_in, sim->now );

        std::vector<uint64_t> acks_out;
        take_acks( nodes[side], &acks_out );
        sim_send_acks( sim, side, acks_out );
    }
}

static bool everything_delivered( node_t * sender, node_t * receiver )
{
    for ( int c = 0; c < ROCKETNET_MAX_CHANNELS; c++ )
        if ( receiver->got[c].size() != sender->sent[c].size() )
            return false;
    return true;
}

// exactly once, in order, per channel: the receiver's sequence is a prefix of
// the sender's accepted sequence, whole when full is true

static void check_ordered_exactly_once( node_t * sender, node_t * receiver, bool full )
{
    for ( int c = 0; c < ROCKETNET_MAX_CHANNELS; c++ )
    {
        const std::vector<uint64_t> & sent = sender->sent[c];
        const std::vector<uint64_t> & got = receiver->got[c];
        check( got.size() <= sent.size() );
        for ( size_t i = 0; i < got.size(); i++ )
            check( got[i] == sent[i] );
        if ( full )
            check( got.size() == sent.size() );
    }
}

// run until the sender's messages are all delivered and acked, and the wire is quiet

static void sim_drain( sim_t * sim, node_t * nodes[2], double limit_seconds )
{
    const double end = sim->now + limit_seconds;
    while ( sim->now < end )
    {
        sim_tick( sim, nodes, 1.0 / 60.0 );
        if ( everything_delivered( nodes[0], nodes[1] ) && everything_delivered( nodes[1], nodes[0] ) &&
             nodes[0]->region->descriptors_free == nodes[0]->region->queue && nodes[1]->region->descriptors_free == nodes[1]->region->queue )
            break;
    }
}

// ---------------------------------------------------------------------------
// 1. small messages, in order, exactly once, under loss, duplication, reorder
// ---------------------------------------------------------------------------

static void small_leg( float loss, float duplicate, uint64_t seed )
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 4 );
    node_t * b = node_new( &config, 4 );
    node_t * nodes[2] = { a, b };

    sim_t sim;
    sim_setup( &sim, loss, duplicate, seed );

    uint64_t random = seed * 2862933555777941757ULL + 3037000493ULL;
    int sent = 0;
    for ( int tick = 0; tick < 200; tick++ )
    {
        for ( int i = 0; i < 4; i++ )
        {
            random = random * 6364136223846793005ULL + 1442695040888963407ULL;
            const uint32_t channel = (uint32_t) ( ( random >> 33 ) % 4 );
            const uint32_t bytes = 8 + (uint32_t) ( ( random >> 40 ) % 300 );
            if ( send_message( a, channel, bytes ) )
                sent++;
            // and traffic the other way, so acks ride real packets too
            if ( i == 0 && send_message( b, channel, bytes ) )
                sent++;
        }
        sim_tick( &sim, nodes, 1.0 / 60.0 );
        // while sending: nothing is ever delivered out of order or twice
        check_ordered_exactly_once( a, b, false );
        check_ordered_exactly_once( b, a, false );
    }
    check( sent == 1000 );

    sim_drain( &sim, nodes, 600.0 );

    check_ordered_exactly_once( a, b, true );
    check_ordered_exactly_once( b, a, true );

    // nothing was a violation, and nothing is left in flight or unfreed
    check( a->violations == 0 && b->violations == 0 );
    check( rn_ordered_decide( &a->ordered ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rn_ordered_decide( &b->ordered ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( a->region->descriptors_free == a->region->queue );
    check( b->region->descriptors_free == b->region->queue );
    check( counter( b, ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND ) == 0 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 0 );

    if ( loss > 0.0f )
    {
        check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );        // lost packets were resent
        check( counter( b, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) > 0 || counter( a, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) > 0 );
    }

    sim_teardown( &sim );
    node_free( a );
    node_free( b );
}

static void test_small_in_order_exactly_once_under_loss_and_reorder()
{
    small_leg( 0.0f, 0.0f, 1 );
    small_leg( 20.0f, 20.0f, 2 );
    small_leg( 40.0f, 25.0f, 3 );
}

// ---------------------------------------------------------------------------
// 2. several small messages in one packet, resent until a packet ack covers them
// ---------------------------------------------------------------------------

static void test_small_packed_and_resent_until_packet_acked()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 4 );
    node_t * b = node_new( &config, 4 );

    double now = 1.0;
    const uint32_t sizes[4] = { 10, 20, 30, 40 };
    const uint32_t channels[4] = { 0, 0, 0, 1 };
    for ( int i = 0; i < 4; i++ )
        check( send_message( a, channels[i], sizes[i] ) != 0 );

    // nothing is numbered at start/finish (6.2): late numbering
    check( a->ordered.send.next == 0 );

    // one update: the four messages are packed in ONE packet
    wire_packet_t first[4];
    check( write_packets( a, now, first, 4 ) == 1 );
    check( a->ordered.builder.packet[0].records == 4 );
    check( a->ordered.builder.packet[0].count == 4 );
    check( a->ordered.send.next == 4 );

    // and read back it is four code 3 records, numbers 0 to 3
    rocketnet_message_reader_t reader;
    check( rocketnet_message_reader_begin( &reader, first[0].data, first[0].bytes, &a->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rocketnet_message_record_t record;
    int n = 0;
    bool number_seen[4] = { false, false, false, false };
    bool message_seen[4] = { false, false, false, false };
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
        check( record.number < 4 && !number_seen[record.number] );
        number_seen[record.number] = true;
        check( !record.block );
        int which = -1;                                         // channels take turns, so numbers are not in send order
        for ( int i = 0; i < 4; i++ )
            if ( !message_seen[i] && (uint32_t) record.bytes == sizes[i] && record.channel == channels[i] )
                which = i;
        check( which >= 0 );
        message_seen[which] = true;
        n++;
    }
    check( n == 4 );

    // the packet is lost. Before the resend delay, nothing is resent
    now += 0.05;
    wire_packet_t none[4];
    check( write_packets( a, now, none, 4 ) == 0 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 0 );

    // after the resend delay passes without a transport ack, the same four numbers go again, packed again
    now += 0.07;
    wire_packet_t second[4];
    check( write_packets( a, now, second, 4 ) == 1 );
    check( a->ordered.builder.packet[0].count == 4 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 4 );
    check( second[0].id != first[0].id );
    check( rn_window_ack_resend_delay( &a->ordered.ack ) == 0.1 );   // the same delay next time: no back-off

    check( rocketnet_message_reader_begin( &reader, second[0].data, second[0].bytes, &a->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    n = 0;
    bool resent_seen[4] = { false, false, false, false };
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        check( record.number < 4 && !resent_seen[record.number] );        // the same four numbers
        resent_seen[record.number] = true;
        n++;
    }
    check( n == 4 );

    // the resend arrives; the peer delivers each channel in order, and its transport acks the packet id
    check( open_packet( b, &second[0] ) );
    check( b->got[0].size() == 3 && b->got[1].size() == 1 );
    check( b->got[0][0] == a->sent[0][0] && b->got[0][1] == a->sent[0][1] && b->got[0][2] == a->sent[0][2] );
    check( b->got[1][0] == a->sent[1][0] );

    // the ack of the RESEND's id (not the lost first send's) covers all four numbers: freed, never resent
    check( a->ordered.send.U == 0 );
    std::vector<uint64_t> acks;
    take_acks( b, &acks );
    check( acks.size() == 1 && acks[0] == second[0].id );
    now += 0.03;
    feed_acks( a, acks, now );
    check( a->ordered.send.U == 4 );
    check( a->region->descriptors_free == a->region->queue );
    check( a->ordered.ack.has_rtt == 1 && rn_window_ack_resend_delay( &a->ordered.ack ) == a->ordered.ack.resend_delay );

    now += 5.0;
    wire_packet_t after[4];
    check( write_packets( a, now, after, 4 ) == 0 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 4 );

    // a late ack of the first packet's id changes nothing
    const uint64_t late = first[0].id;
    check( rn_ordered_acks( &a->ordered, &late, 1, now ) <= 1 );
    check( a->ordered.send.U == 4 );

    // a message that goes unacked past the reliable timeout ends the connection (6.4 "Giving up")
    {
        rocketnet_message_config_t timed;
        node_config( &timed, 1024, 262144, 262144, 800000 );
        timed.reliable_message_timeout_ms = 1000;
        node_t * c = node_new( &timed, 4 );
        check( send_message( c, 0, 64 ) != 0 );
        wire_packet_t sent[4];
        check( write_packets( c, 1.0, sent, 4 ) == 1 );
        check( rn_ordered_decide( &c->ordered ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( write_packets( c, 1.6, sent, 4 ) >= 0 );                  // resent, still inside the timeout
        check( rn_ordered_decide( &c->ordered ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( write_packets( c, 2.5, sent, 4 ) == 0 );                  // past it: nothing more is sent
        check( rn_ordered_decide( &c->ordered ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
        node_free( c );
    }

    // and the lost first send, arriving late, is a duplicate the window drops, not a second delivery
    const uint64_t duplicates = counter( b, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
    check( open_packet( b, &first[0] ) );
    check( counter( b, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) == duplicates + 4 );
    check( b->got[0].size() == 3 && b->got[1].size() == 1 );

    node_free( a );
    node_free( b );
}

// ---------------------------------------------------------------------------
// 3. a large message: one chunk to a packet, paced by the budget, delivered whole
// ---------------------------------------------------------------------------

static void exchange_perfect( node_t * a, node_t * b, double * now, int updates, int * out_packets, int * out_records_max )
{
    for ( int u = 0; u < updates; u++ )
    {
        *now += 0.01;
        wire_packet_t out[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
        const int n = write_packets( a, *now, out, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
        for ( int p = 0; p < n; p++ )
        {
            const uint32_t records = a->ordered.builder.packet[p].records;
            if ( out_records_max && (int) records > *out_records_max )
                *out_records_max = (int) records;
            (*out_packets)++;
            check( open_packet( b, &out[p] ) );
        }
        std::vector<uint64_t> acks;
        take_acks( b, &acks );
        feed_acks( a, acks, *now + 0.001 );
    }
}

static void test_large_chunked_one_per_packet_and_delivered_whole()
{
    // the defaults: the largest message fits (max_message_bytes 262144, 228 chunks)
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    check( config.max_message_bytes >= 262144 );
    node_t * a = node_new( &config, 16 );
    node_t * b = node_new( &config, 16 );

    const uint32_t bytes = (uint32_t) config.max_message_bytes;
    uint8_t * source = NULL;
    const uint64_t id = send_message( a, 0, bytes, &source );
    check( id != 0 && source != NULL );

    // the span is contiguous, and inside the region
    check( rn_region_contains( a->region, source, bytes ) );

    double now = 1.0;
    int packets = 0;
    int records_max = 0;
    exchange_perfect( a, b, &now, 40, &packets, &records_max );

    // 228 chunks, each in its own packet: one record each, nothing resent
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( packets == (int) ( ( bytes + F - 1 ) / F ) );
    check( records_max == 1 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 0 );

    // delivered once, whole, byte for byte, in place in the ring: no reassembly buffer
    check( b->delivered.size() == 1 );
    check( b->delivered[0].bytes == bytes );
    check( b->delivered[0].id == id );
    check( b->delivered[0].view == b->region->receive_ring );
    check( memcmp( b->delivered[0].view, source, bytes ) == 0 );

    // acked: the span and the descriptor are free again
    check( a->region->descriptors_free == a->region->queue );
    check( a->region->span_first == RN_DESCRIPTOR_NONE );

    node_free( a );
    node_free( b );

    // the send budget paces the chunks: 1000 kbps is 125,000 bytes a second, so
    // 100 KiB takes most of a second on the wire and never exceeds the budget
    // plus the bucket's depth in any window (R6, replacing a fixed chunk rate)
    node_config( &config, 1024, 262144, 262144, 800000 );
    a = node_new( &config, 16, true, 1000 );
    b = node_new( &config, 16 );
    const uint32_t small_bytes = 100 * 1024;
    source = NULL;
    const uint64_t id2 = send_message( a, 0, small_bytes, &source );
    check( id2 != 0 );

    now = 1.0;
    int budget_packets = 0;
    int max_per_update = 0;
    uint64_t wire_bytes = 0;
    int updates = 0;
    while ( b->delivered.empty() && updates < 2000 )
    {
        now += 0.01;
        updates++;
        wire_packet_t out[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
        const int n = write_packets( a, now, out, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
        if ( n > max_per_update )
            max_per_update = n;
        for ( int p = 0; p < n; p++ )
        {
            check( a->ordered.builder.packet[p].records == 1 );
            wire_bytes += (uint64_t) rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, out[p].bytes );
            budget_packets++;
            check( open_packet( b, &out[p] ) );
        }
        std::vector<uint64_t> acks;
        take_acks( b, &acks );
        feed_acks( a, acks, now + 0.001 );
    }
    check( b->delivered.size() == 1 && b->delivered[0].bytes == small_bytes );
    check( memcmp( b->delivered[0].view, source, small_bytes ) == 0 );
    check( budget_packets == (int) ( ( small_bytes + F - 1 ) / F ) );
    // paced: not every chunk in the first update, and the elapsed time bounds the bytes
    check( max_per_update < budget_packets );
    const double elapsed = now - 1.0;
    check( (double) wire_bytes <= 125000.0 * elapsed + a->budget.depth + 1.0 );
    check( elapsed > 0.4 );

    node_free( a );
    node_free( b );
}

// ---------------------------------------------------------------------------
// 4. a large message between small ones takes its place in the one order
// ---------------------------------------------------------------------------

// the application sizes its sends to the connection's memory (R5): a start with
// no room ends the connection. The room a run of spans can use contiguously is
// what the ring leaves behind the tail (tla/RNRing.tla, PlaceSingle).

static uint64_t span_room( const node_t * node )
{
    const rn_region_t * region = node->region;
    if ( region->span_first == RN_DESCRIPTOR_NONE )
        return region->span_bytes;
    if ( region->span_tail > region->span_head )
        return ( region->span_bytes - region->span_tail > region->span_head ) ? region->span_bytes - region->span_tail : region->span_head;
    return region->span_head - region->span_tail;
}

static bool has_room_for( node_t * node, uint32_t bytes )
{
    return span_room( node ) >= bytes + 64 && node->region->descriptors_free > 16;
}

static void large_between_small_leg( float loss, float duplicate, uint64_t seed )
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 4 );
    node_t * b = node_new( &config, 4 );
    node_t * nodes[2] = { a, b };
    sim_t sim;
    sim_setup( &sim, loss, duplicate, seed );

    uint64_t random = seed * 2862933555777941757ULL + 3037000493ULL;
    int rounds = 0;
    int ticks = 0;
    while ( rounds < 30 && ticks < 100000 )
    {
        random = random * 6364136223846793005ULL + 1442695040888963407ULL;
        // 1,300 bytes: one record yet above one chunk; and 3,000 to 24,000: chunks
        const uint32_t bytes = ( rounds % 3 == 0 ) ? 1300 : 3000 + (uint32_t) ( ( random >> 33 ) % 21000 );
        if ( has_room_for( a, 3 * ( bytes + 16 ) ) )
        {
            for ( uint32_t channel = 0; channel < 3; channel++ )
            {
                check( send_message( a, channel, 8 + (uint32_t) ( ( random >> 33 ) % 100 ) ) != 0 );
                check( send_message( a, channel, bytes ) != 0 );
                check( send_message( a, channel, 8 + (uint32_t) ( ( random >> 45 ) % 100 ) ) != 0 );
            }
            rounds++;
        }
        sim_tick( &sim, nodes, 1.0 / 60.0 );
        ticks++;
        check_ordered_exactly_once( a, b, false );
    }
    check( rounds == 30 );

    sim_drain( &sim, nodes, 900.0 );
    check_ordered_exactly_once( a, b, true );
    check( a->violations == 0 && b->violations == 0 );
    check( rn_ordered_decide( &b->ordered ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( a->region->descriptors_free == a->region->queue );
    check( a->region->span_first == RN_DESCRIPTOR_NONE );
    // nothing held is left
    for ( uint32_t i = 0; i < b->region->held_entries; i++ )
        check( b->region->held_index[i].used == 0 );

    sim_teardown( &sim );
    node_free( a );
    node_free( b );
}

// the edges between the three storages and the two modes: a store chunk (up to
// 128, 512, 1152 bytes), a span that is still one record (1153 to the packet's
// room), and a block of chunks (above it)

static void boundary_sizes_leg()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 8 );
    node_t * b = node_new( &config, 8 );
    const uint32_t small_limit = rn_channels_small_limit( &a->ordered.channels );
    check( small_limit == ROCKETNET_MESSAGE_SINGLE_BYTES - 1 - ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES );
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t sizes[] = { 8, 127, 128, 129, 511, 512, 513, 1151, F, F + 1, 1300, small_limit - 1, small_limit, small_limit + 1,
                               2 * F - 1, 2 * F, 2 * F + 1, 3 * F, 3 * F + 1, 16 * F + 5 };
    uint32_t channel = 0;
    for ( size_t i = 0; i < sizeof( sizes ) / sizeof( sizes[0] ); i++ )
    {
        check( send_message( a, channel, sizes[i] ) != 0 );
        channel = ( channel + 1 ) % 3;
    }
    double now = 1.0;
    int packets = 0;
    exchange_perfect( a, b, &now, 80, &packets, NULL );
    check_ordered_exactly_once( a, b, true );
    check( b->violations == 0 );

    // a block's chunks are one packet each; a message that is one record is one number
    uint32_t chunks = 0;
    for ( size_t i = 0; i < sizeof( sizes ) / sizeof( sizes[0] ); i++ )
        chunks += sizes[i] > small_limit ? ( sizes[i] + F - 1 ) / F : 1;
    check( a->ordered.send.next == chunks );
    check( a->region->descriptors_free == a->region->queue );
    check( a->region->span_first == RN_DESCRIPTOR_NONE );
    node_free( a );
    node_free( b );
}

// the ordering index is 24 bits on the wire and 32 in the channel's counters:
// both ends start just short of the wrap, and a mix of small and chunked
// messages crosses 2^24 and 2^32 in order

static void ordering_wrap_leg( uint32_t start )
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 8 );
    node_t * b = node_new( &config, 8 );
    a->region->channel_send[0].ordered_write = start;
    b->region->channel_receive[0].ordered_read = start;
    for ( int i = 0; i < 12; i++ )
        check( send_message( a, 0, i % 4 == 3 ? 3000 : 24 + (uint32_t) i ) != 0 );
    double now = 1.0;
    int packets = 0;
    // lose every other packet on the way, so held and drained records cross the wrap too
    for ( int u = 0; u < 400 && b->got[0].size() < 12; u++ )
    {
        now += 0.02;
        wire_packet_t out[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
        const int n = write_packets( a, now, out, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
        for ( int p = 0; p < n; p++ )
            if ( ( ++packets % 2 ) == 0 )
                open_packet( b, &out[p] );
        std::vector<uint64_t> acks;
        take_acks( b, &acks );
        feed_acks( a, acks, now + 0.005 );
    }
    check_ordered_exactly_once( a, b, true );
    check( b->violations == 0 );
    check( b->region->channel_receive[0].ordered_read == start + 12 );
    node_free( a );
    node_free( b );
}

static void test_large_between_small_keeps_order()
{
    boundary_sizes_leg();
    ordering_wrap_leg( 0x00FFFFFA );
    ordering_wrap_leg( 0xFFFFFFFA );

    large_between_small_leg( 0.0f, 0.0f, 11 );
    large_between_small_leg( 15.0f, 15.0f, 12 );
    large_between_small_leg( 35.0f, 20.0f, 13 );
}

// ---------------------------------------------------------------------------
// 5. chunks are placed by index, not by arrival
// ---------------------------------------------------------------------------

static void test_chunks_placed_by_index_not_arrival()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 8 );
    node_t * b = node_new( &config, 8 );

    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t bytes = 4 * F + 500;                         // five chunks, the last short
    uint8_t * source = NULL;
    const uint64_t id = send_message( a, 3, bytes, &source );
    check( id != 0 );

    wire_packet_t out[8];
    check( write_packets( a, 1.0, out, 8 ) == 5 );
    for ( int p = 0; p < 5; p++ )
        check( a->ordered.builder.packet[p].records == 1 );

    // which chunk is in which packet: the block fragment index of each record
    int index_of[5];
    for ( int p = 0; p < 5; p++ )
    {
        rocketnet_message_reader_t reader;
        check( rocketnet_message_reader_begin( &reader, out[p].data, out[p].bytes, &a->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        rocketnet_message_record_t record;
        check( rocketnet_message_reader_next( &reader, &record ) );
        check( record.block && record.fragments == 4 && record.channel == 3 && record.ordering == 0 );
        check( record.bytes == ( record.fragment == 4 ? 500 : (int) F ) );
        index_of[p] = (int) record.fragment;
    }
    for ( int i = 0; i < 5; i++ )
        check( index_of[i] == i );

    // arrival order 3, 0, 4, 2, 1: after each, the ring holds exactly the chunks that came, each at index * F
    const int arrival[5] = { 3, 0, 4, 2, 1 };
    bool present[5] = { false, false, false, false, false };
    for ( int step = 0; step < 5; step++ )
    {
        const int chunk = arrival[step];
        check( open_packet( b, &out[chunk] ) );
        present[chunk] = true;
        for ( int c = 0; c < 5; c++ )
        {
            if ( !present[c] )
                continue;
            const uint32_t length = c == 4 ? 500 : F;
            check( memcmp( b->region->receive_ring + (uint64_t) c * F, source + (uint64_t) c * F, length ) == 0 );
        }
        // never a partial delivery: the message appears only with its last chunk
        check( b->delivered.size() == ( step == 4 ? 1u : 0u ) );
    }

    check( b->delivered[0].bytes == bytes );
    check( b->delivered[0].view == b->region->receive_ring );
    check( memcmp( b->delivered[0].view, source, bytes ) == 0 );
    check( b->violations == 0 );

    node_free( a );
    node_free( b );
}

// ---------------------------------------------------------------------------
// 6. no partial delivery: the last index arriving is not the message arriving
// ---------------------------------------------------------------------------

static void test_no_partial_delivery()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 8 );
    node_t * b = node_new( &config, 8 );

    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    uint8_t * big = NULL;
    const uint64_t first = send_message( a, 1, 8 + 40 );
    const uint64_t large = send_message( a, 1, 3 * F, &big );
    const uint64_t last = send_message( a, 1, 8 + 90 );
    check( first && large && last );

    // numbers: small 0, chunks 1 2 3, small 4. Written in one update: five packets, the small ones packed alone
    wire_packet_t out[8];
    const int n = write_packets( a, 1.0, out, 8 );
    check( n >= 4 );

    // find each packet's record kind
    int packet_of_chunk[3] = { -1, -1, -1 };
    int packet_small_first = -1, packet_small_last = -1;
    for ( int p = 0; p < n; p++ )
    {
        rocketnet_message_reader_t reader;
        check( rocketnet_message_reader_begin( &reader, out[p].data, out[p].bytes, &a->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        rocketnet_message_record_t record;
        while ( rocketnet_message_reader_next( &reader, &record ) )
        {
            if ( record.block )
                packet_of_chunk[record.fragment] = p;
            else if ( record.ordering == 0 )
                packet_small_first = p;
            else
                packet_small_last = p;
        }
    }
    check( packet_of_chunk[0] >= 0 && packet_of_chunk[1] >= 0 && packet_of_chunk[2] >= 0 );
    check( packet_small_first >= 0 && packet_small_last >= 0 );

    // the first small message is lost. The LAST chunk and the final small message arrive:
    // nothing is delivered (the small one is ahead; the chunk is alone)
    check( open_packet( b, &out[packet_of_chunk[2]] ) );
    check( open_packet( b, &out[packet_small_last] ) );
    check( b->delivered.empty() );

    // the first chunk arrives: still short a chunk, still nothing
    check( open_packet( b, &out[packet_of_chunk[0]] ) );
    check( b->delivered.empty() );

    // the first small message arrives: it alone is delivered; the block is incomplete and stops the line
    check( open_packet( b, &out[packet_small_first] ) );
    check( b->delivered.size() == 1 && b->delivered[0].id == first );

    // the missing middle chunk completes the block: the block, then the final small one, drained in order
    check( open_packet( b, &out[packet_of_chunk[1]] ) );
    check( b->delivered.size() == 3 );
    check( b->delivered[1].id == large && b->delivered[1].bytes == 3 * F );
    check( memcmp( b->delivered[1].view, big, 3 * F ) == 0 );
    check( b->delivered[2].id == last );
    check( b->violations == 0 );

    // the held message was delivered from the ring, not from the batch
    check( b->delivered[2].view >= b->region->receive_ring && b->delivered[2].view < b->region->receive_ring + b->region->ring_bytes );

    node_free( a );
    node_free( b );
}

// ---------------------------------------------------------------------------
// 7. hostile chunks kick, and the kick frees the slot
// ---------------------------------------------------------------------------

static void chunk_record( rocketnet_message_record_t * record, uint32_t number, uint32_t ordering, uint32_t index, uint32_t fragments, uint32_t ring_units, const uint8_t * data, int bytes )
{
    memset( record, 0, sizeof( *record ) );
    record->code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record->block = 1;
    record->number = number;
    record->ordering = ordering;
    record->channel = 0;
    record->fragment = index;
    record->fragments = fragments;
    record->ring = ring_units;
    record->data = data;
    record->bytes = bytes;
}

static void expect_kick( node_t * node, int counter_id, uint64_t before )
{
    check( node->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( rn_ordered_decide( &node->ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( counter( node, counter_id ) == before + 1 );
    check( counter( node, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) >= 1 );
    check( node->ordered.channels.deliveries == 0 );
}

static void test_hostile_chunk_count_or_index_kicks()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    static uint8_t chunk[1152];
    memset( chunk, 0x77, sizeof( chunk ) );
    rn_delivery_t out;
    rocketnet_message_record_t record;

    // a count that disagrees with the first count seen for the block
    {
        node_t * b = node_new( &config, 4 );
        chunk_record( &record, 0, 0, 0, 3, 0, chunk, (int) F );                 // 4 chunks
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        check( b->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 1, 0, 1, 4, 72, chunk, (int) F );                // claims 5
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // an index at or past the count
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 0, 0, 4, 3, 288, chunk, (int) F );
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // a count above ceil( max_message_bytes / F ): through the dispatch, and through the wire
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 0, 0, 0, 228, 0, chunk, (int) F );               // 229 chunks > 228
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );

        // the same record as a payload: the standard's check 7 refuses it whole, before any dispatch
        b = node_new( &config, 4 );
        rocketnet_message_writer_t writer;
        alignas( 16 ) uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        rocketnet_message_writer_begin( &writer, payload, ROCKETNET_MESSAGE_SINGLE_BYTES );
        chunk_record( &record, 0, 0, 0, 228, 0, chunk, (int) F );
        check( rocketnet_message_writer_add( &writer, &record ) );
        const int payload_bytes = rocketnet_message_writer_finish( &writer );
        check( rn_ordered_receive( &b->ordered, payload, payload_bytes ) == ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
        check( rn_ordered_decide( &b->ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        check( b->ordered.channels.deliveries == 0 );
        node_free( b );
    }

    // a span outside the ring: the block would end past B
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        const uint32_t units = (uint32_t) ( b->region->ring_bytes / 16 ) - 72;     // chunk 0 of a 4-chunk block at the ring's last stride
        chunk_record( &record, 0, 0, 0, 3, units, chunk, (int) F );
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_RING, before );
        node_free( b );
    }

    // a chunk whose base disagrees with the block's first chunk seen
    {
        node_t * b = node_new( &config, 4 );
        chunk_record( &record, 0, 0, 0, 1, 0, chunk, (int) F );                 // chunk 0 at ring 0
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 1, 0, 1, 1, 200, chunk, 100 );                   // chunk 1 claims another base
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // a length that is not F on a chunk that is not the last
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 0, 0, 0, 3, 0, chunk, 100 );
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // an ordering index a window ahead, and a bad channel
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD );
        memset( &record, 0, sizeof( record ) );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.data = chunk;
        record.bytes = 16;
        record.ordering = 5000;
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD, before );
        node_free( b );

        b = node_new( &config, 4 );
        const uint64_t bad_before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );
        memset( &record, 0, sizeof( record ) );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.data = chunk;
        record.bytes = 16;
        record.channel = b->region->num_channels;
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL, bad_before );
        node_free( b );
    }

    // the kick frees the slot: the region is reset and the next client starts clean (R5)
    {
        node_t * b = node_new( &config, 4 );
        chunk_record( &record, 0, 0, 0, 3, 0, chunk, (int) F );
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        chunk_record( &record, 1, 0, 1, 9, 72, chunk, (int) F );
        rn_channels_dispatch( &b->ordered.channels, &record, &out );
        check( rn_ordered_decide( &b->ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

        rn_region_reset( b->region );
        check( rn_ordered_init( &b->ordered, b->region, &config, NULL, 4, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
        check( rn_ordered_decide( &b->ordered ) == ROCKETNET_DISCONNECT_REASON_NONE );
        for ( uint32_t i = 0; i < b->region->held_entries; i++ )
            check( b->region->held_index[i].used == 0 );
        for ( uint32_t i = 0; i < b->region->window; i++ )
            check( !b->region->receive_descriptors[i].live );
        node_free( b );
    }
}

// hostile reliable chunks (code 2) are refused and counted: each refusal in
// rn_channels_dispatch_reliable_chunk (sdk/rocketnet_channel_reliable.cpp)
// fires BAD_BLOCK for the shape, the dead number and a part that disagrees,
// and BAD_RING for a span outside the ring.
static void test_hostile_reliable_chunks_are_refused()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    static uint8_t chunk[1152];
    memset( chunk, 0x77, sizeof( chunk ) );
    rn_delivery_t out;
    rocketnet_message_record_t record;

    // (a) fragments = 0: a block of one is not a block
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 0, 0, 0, 0, 0, chunk, (int) F );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // (b) an index at the count
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 0, 0, 2, 1, 0, chunk, (int) F );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // (c) a non-last chunk shorter than F
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 0, 0, 0, 1, 0, chunk, 100 );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // (d) a number no peer ever accepted
    {
        node_t * b = node_new( &config, 4 );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 5, 0, 0, 1, 0, chunk, (int) F );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }

    // (e) a span outside the ring: accepted, then refused as BAD_RING
    {
        node_t * b = node_new( &config, 4 );
        chunk_record( &record, 0, 0, 0, 1, (uint32_t) ( b->region->ring_bytes / 16 ), chunk, (int) F );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        uint32_t slot;
        check( rn_window_receive_accept( &b->ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_RING, before );
        node_free( b );
    }

    // (f) a chunk whose base disagrees with the block's first chunk seen
    {
        node_t * b = node_new( &config, 4 );
        chunk_record( &record, 0, 0, 0, 1, 0, chunk, (int) F );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        uint32_t slot;
        check( rn_window_receive_accept( &b->ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        chunk_record( &record, 1, 0, 1, 1, 200, chunk, 100 );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        check( rn_window_receive_accept( &b->ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        expect_kick( b, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK, before );
        node_free( b );
    }
}

static void test_hostile_small_at_held_ordering_is_refused()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    static uint8_t chunk[1152];
    memset( chunk, 0x77, sizeof( chunk ) );
    rn_delivery_t out;
    rocketnet_message_record_t record;

    node_t * b = node_new( &config, 4 );

    // chunk 0 of a 4-chunk block at ordering 0 is held, still collecting chunks
    chunk_record( &record, 0, 0, 0, 3, 0, chunk, (int) F );
    check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( b->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );

    uint32_t held_before = 0;
    for ( uint32_t i = 0; i < b->region->held_entries; i++ )
        held_before += b->region->held_index[i].used ? 1 : 0;
    check( held_before == 1 );

    // a small record at the same ordering index: refused, not delivered
    const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.number = 1;
    record.ordering = 0;
    record.channel = 0;
    record.data = chunk;
    record.bytes = 16;
    check( rn_channels_dispatch( &b->ordered.channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( b->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( rn_ordered_decide( &b->ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( counter( b, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) == before + 1 );
    check( b->ordered.channels.deliveries == 0 );

    // no held entry was leaked: only the blocks own entry remains
    uint32_t held_after = 0;
    for ( uint32_t i = 0; i < b->region->held_entries; i++ )
        held_after += b->region->held_index[i].used ? 1 : 0;
    check( held_after == 1 );

    node_free( b );
}

// the same conflict over the simulated receive path: two connections on
// rocketnet_network_simulator, time only an argument starting at 1000.0, the
// hostile small record injected as a packet after the block's chunk is held

static void test_hostile_small_at_held_ordering_is_refused_sim()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    static uint8_t chunk[1152];
    memset( chunk, 0x77, sizeof( chunk ) );

    node_t * a = node_new( &config, 4 );
    node_t * b = node_new( &config, 4 );
    node_t * nodes[2] = { a, b };

    sim_t sim;
    sim_setup( &sim, 0.0f, 0.0f, 9 );
    sim.now = 1000.0;

    alignas( 16 ) static uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rocketnet_message_writer_t writer;
    rocketnet_message_record_t record;
    wire_packet_t wp;

    // packet 1: chunk 0 of a 4-chunk block at ordering 0, held while collecting
    chunk_record( &record, 0, 0, 0, 3, 0, chunk, (int) F );
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );
    check( rocketnet_message_writer_add( &writer, &record ) == 1 );
    int len = rocketnet_message_writer_finish( &writer );
    check( len > 0 );
    wp.id = a->next_packet_id++;
    wp.bytes = len;
    memcpy( wp.data, payload, (size_t) len );
    sim_send_data( &sim, 0, &wp );

    int ticks = 0;
    uint32_t held_before = 0;
    while ( ticks++ < 200 )
    {
        sim_tick( &sim, nodes, 1.0 / 60.0 );
        held_before = 0;
        for ( uint32_t i = 0; i < b->region->held_entries; i++ )
            held_before += b->region->held_index[i].used ? 1 : 0;
        if ( held_before == 1 )
            break;
    }
    check( held_before == 1 );

    // packet 2: a small record at the same ordering index: refused, not delivered
    const uint64_t before = counter( b, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.number = 1;
    record.ordering = 0;
    record.channel = 0;
    record.data = chunk;
    record.bytes = 16;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );
    check( rocketnet_message_writer_add( &writer, &record ) == 1 );
    len = rocketnet_message_writer_finish( &writer );
    check( len > 0 );
    wp.id = a->next_packet_id++;
    wp.bytes = len;
    memcpy( wp.data, payload, (size_t) len );
    sim_send_data( &sim, 0, &wp );

    ticks = 0;
    uint64_t after = before;
    while ( ticks++ < 200 && after == before )
    {
        sim_tick( &sim, nodes, 1.0 / 60.0 );
        after = counter( b, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
    }
    check( counter( b, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) == before + 1 );
    check( b->ordered.channels.deliveries == 0 );
    check( b->delivered.size() == 0 );
    check( b->got[0].size() == 0 );
    check( rn_ordered_decide( &b->ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    // no held entry was leaked: only the block's own entry remains
    uint32_t held_after = 0;
    for ( uint32_t i = 0; i < b->region->held_entries; i++ )
        held_after += b->region->held_index[i].used ? 1 : 0;
    check( held_after == 1 );

    sim_teardown( &sim );
    node_free( a );
    node_free( b );
}

// ---------------------------------------------------------------------------
// 8. zero copy on the reliable path: the start pointer is where the frame is
//    copied from (a reliable record outlives the frame for its resends), and
//    delivery is a view. A fitting unreliable message is written in the frame
//    itself, with no copy (test_unit_unreliable_in_place.cpp)
// ---------------------------------------------------------------------------

// every record of a packet, in wire order

static int packet_records( node_t * node, const wire_packet_t * packet, rocketnet_message_record_t * records, int max_records )
{
    rocketnet_message_reader_t reader;
    check( rocketnet_message_reader_begin( &reader, packet->data, packet->bytes, &node->ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    int n = 0;
    while ( n < max_records && rocketnet_message_reader_next( &reader, &records[n] ) )
        n++;
    return n;
}

static void test_start_pointer_is_the_frame_source()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 8 );
    node_t * b = node_new( &config, 8 );
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    double now = 1.0;
    rocketnet_message_record_t records[8];
    uint32_t length = 0;

    // a start that is not finished gives its room back, and takes no ordering index
    check( rn_channels_start( &a->ordered.channels, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 100 ) != NULL );
    check( rn_channels_start( &a->ordered.channels, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 100 ) == NULL );      // one start at a time
    check( rn_channels_cancel( &a->ordered.channels ) );
    check( rn_channels_start( &a->ordered.channels, 1, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 5000 ) != NULL );
    check( a->region->span_first != RN_DESCRIPTOR_NONE );
    check( rn_channels_cancel( &a->ordered.channels ) );
    check( !rn_channels_cancel( &a->ordered.channels ) );
    check( a->region->span_first == RN_DESCRIPTOR_NONE && a->region->descriptors_free == a->region->queue );
    check( a->region->channel_send[0].ordered_write == 0 && a->region->channel_send[1].ordered_write == 0 );
    // a finish larger than the start, or an unstarted finish, enqueues nothing
    check( !rn_channels_finish( &a->ordered.channels, 10 ) );
    check( rn_channels_start( &a->ordered.channels, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 100 ) != NULL );
    check( !rn_channels_finish( &a->ordered.channels, 101 ) );
    check( rn_channels_cancel( &a->ordered.channels ) );
    // Exercise refusal against a negotiated mask that excludes this type.
    const uint32_t enabled_types = a->ordered.channels.enabled_types;
    a->ordered.channels.enabled_types &= ~( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT );
    check( rn_channels_start( &a->ordered.channels, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT, 100 ) == NULL );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED ) == 1 );
    a->ordered.channels.enabled_types = enabled_types;
    check( rn_channels_start( &a->ordered.channels, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED, 100 ) != NULL );
    check( rn_channels_cancel( &a->ordered.channels ) );

    // SMALL: a store chunk of the region. The planner reads the message from exactly this pointer.
    uint8_t * small = NULL;
    check( send_message( a, 0, 100, &small ) != 0 );
    check( rn_region_contains( a->region, small, 100 ) );
    const uint32_t small_descriptor = a->region->channel_send[0].queue_head;
    check( small_descriptor != RN_DESCRIPTOR_NONE );
    check( rn_region_send_data( a->region, small_descriptor, 0, &length ) == small && length == 100 );
    check( rn_region_send_write_pointer( a->region, small_descriptor ) == small );

    // and the frame carries those bytes, copied once from there
    wire_packet_t out[8];
    check( write_packets( a, now, out, 8 ) == 1 );
    check( packet_records( a, &out[0], records, 8 ) == 1 );
    check( records[0].bytes == 100 && memcmp( records[0].data, small, 100 ) == 0 );
    check( records[0].data != small );                                  // the frame is the one copy (THE COPIES (a))

    // the region record outlives the frame: change it in place, and the resend carries the change.
    // That is only possible if the packet write reads the start pointer, not a copy of its own.
    small[50] ^= 0xFF;
    now += 0.5;
    check( write_packets( a, now, out, 8 ) == 1 );
    check( packet_records( a, &out[0], records, 8 ) == 1 );
    check( records[0].data[50] == small[50] );
    small[50] ^= 0xFF;                                                  // restored: the receiver verifies the bytes below

    // LARGE: one contiguous span; fragment i is read at the pointer + i * F
    uint8_t * large = NULL;
    const uint32_t large_bytes = 3 * F + 300;
    check( send_message( a, 1, large_bytes, &large ) != 0 );
    const uint32_t large_descriptor = a->region->channel_send[1].queue_head;
    const rn_send_descriptor_t * descriptor = &a->region->send_descriptors[large_descriptor];
    check( ( descriptor->flags & RN_SEND_FLAG_SPAN ) != 0 );
    check( large == a->region->send_spans + descriptor->span_start );
    check( rn_region_contains( a->region, large, large_bytes ) );
    check( large == rn_region_send_write_pointer( a->region, large_descriptor ) );
    for ( uint32_t f = 0; f < 4; f++ )
        check( rn_region_send_data( a->region, large_descriptor, f, &length ) == large + (uint64_t) f * F && length == ( f == 3 ? 300u : F ) );

    now += 0.5;
    const int n = write_packets( a, now, out, 8 );
    check( n >= 4 );
    int chunk_packet[4] = { -1, -1, -1, -1 };
    for ( int p = 0; p < n; p++ )
    {
        const int count = packet_records( a, &out[p], records, 8 );
        for ( int r = 0; r < count; r++ )
            if ( records[r].block )
            {
                check( count == 1 );
                chunk_packet[records[r].fragment] = p;
                check( memcmp( records[r].data, large + (uint64_t) records[r].fragment * F, (size_t) records[r].bytes ) == 0 );
            }
    }
    for ( int f = 0; f < 4; f++ )
        check( chunk_packet[f] >= 0 );

    // a chunk is resent from the span too: change chunk 2 in place; only that chunk's resend shows it
    large[2 * F + 9] ^= 0x55;
    now += 3.0;
    const int resent = write_packets( a, now, out, 8 );
    check( resent >= 4 );
    int seen = 0;
    for ( int p = 0; p < resent; p++ )
    {
        const int count = packet_records( a, &out[p], records, 8 );
        for ( int r = 0; r < count; r++ )
            if ( records[r].block )
            {
                check( memcmp( records[r].data, large + (uint64_t) records[r].fragment * F, (size_t) records[r].bytes ) == 0 );
                seen++;
            }
    }
    check( seen == 4 );
    large[2 * F + 9] ^= 0x55;

    node_free( a );
    node_free( b );

    // RECEIVE: a record delivered in its turn is a view into the transport's batch, never a copy;
    // a record held ahead is placed in the ring once and delivered from there
    a = node_new( &config, 8 );
    b = node_new( &config, 8 );
    check( send_message( a, 0, 80 ) != 0 );
    check( send_message( a, 0, 90 ) != 0 );
    wire_packet_t first[8];
    check( write_packets( a, 1.0, first, 8 ) == 1 );                    // both packed in one packet, ordering 0 and 1
    // split them: two updates, so the second can arrive first
    node_free( a );
    a = node_new( &config, 8 );
    check( send_message( a, 0, 80 ) != 0 );
    wire_packet_t p0[8], p1[8];
    check( write_packets( a, 1.0, p0, 8 ) == 1 );
    check( send_message( a, 0, 90 ) != 0 );
    check( write_packets( a, 1.01, p1, 8 ) == 1 );

    // the later one arrives first: held, copied to the ring at the offset it names, nothing delivered
    rocketnet_message_record_t held_record;
    check( packet_records( a, &p1[0], &held_record, 1 ) == 1 );
    check( rn_ordered_receive( &b->ordered, p1[0].data, p1[0].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &b->ordered ) == 0 );
    check( memcmp( b->region->receive_ring + (uint64_t) held_record.ring * 16, held_record.data, (size_t) held_record.bytes ) == 0 );
    check( b->region->receive_ring + (uint64_t) held_record.ring * 16 != held_record.data );

    // the earlier one arrives: delivered at once as a view INTO THE BATCH (the payload the transport handed over)...
    rocketnet_message_record_t turn_record;
    check( packet_records( a, &p0[0], &turn_record, 1 ) == 1 );
    check( rn_ordered_receive( &b->ordered, p0[0].data, p0[0].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &b->ordered ) == 2 );
    check( b->region->deliveries[0].data == turn_record.data );
    check( b->region->deliveries[0].data >= p0[0].data && b->region->deliveries[0].data < p0[0].data + p0[0].bytes );
    check( b->region->deliveries[0].bytes == 80 );

    // ...and the drain delivers the held one from the ring, where it lies
    check( b->region->deliveries[1].data == b->region->receive_ring + (uint64_t) held_record.ring * 16 );
    check( b->region->deliveries[1].bytes == 90 );
    rn_ordered_end_update( &b->ordered );

    node_free( a );
    node_free( b );
}

// ---------------------------------------------------------------------------
// 9. nothing is allocated after create (R2), the defaults are admissible, and
//    exhaustion kicks (R5)
// ---------------------------------------------------------------------------

static uint64_t cm_mallocs = 0;
static uint64_t cm_frees = 0;

static void * cm_malloc( void * context, size_t bytes )
{
    (void) context;
    cm_mallocs++;
    return malloc( bytes );
}

static void cm_free( void * context, void * p )
{
    (void) context;
    cm_frees++;
    free( p );
}

static void cm_install()
{
    rocketnet_term();
    check( rocketnet_init( NULL, plan_rss_chain( cm_malloc, cm_free ), plan_rss_free ) == ROCKETNET_STATUS_OK );
}

static void cm_restore()
{
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static uint64_t cm_calls()
{
    return cm_mallocs + cm_frees;
}

// Registered as test_ordered_no_allocation_after_create: unit test names are unique
// across the suite, and the registrar aborts on a duplicate
// (test_unit_registry.h, rocketnet_unit_file_registrar_t).

static void test_ordered_no_allocation_after_create()
{
    // the defaults are admissible, and a connection's region at them is this many bytes (CHANNELS.md 14)
    rocketnet_message_config_t defaults;
    rocketnet_message_default_config( &defaults );
    check( defaults.max_message_bytes >= 262144 );
    check( defaults.receive_message_buffer_bytes >= defaults.max_message_bytes );
    check( rocketnet_message_validate_config( NULL ) == 1 );
    rn_region_layout_t layout;
    check( rn_region_layout( NULL, &layout ) );
    check( layout.formula_bytes > 1588480 );
    check( layout.region_bytes >= layout.formula_bytes );
    check( rocketnet_message_connection_bytes( NULL ) == layout.region_bytes );
    check( layout.store_chunks[RN_STORE_LARGE] >= ( (uint32_t) defaults.max_message_bytes + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES );
    check( layout.bytes[RN_PART_SEND_SPANS] >= (uint64_t) defaults.max_message_bytes );

    cm_install();
    cm_mallocs = 0;
    cm_frees = 0;

    // create: the one allocation per connection (a server slot's, a client's)
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    config.send_queue_messages = 64;
    node_t * a = node_new( &config, 8 );
    node_t * b = node_new( &config, 8 );
    check( cm_mallocs >= 2 );
    const uint64_t after_create = cm_calls();

    // the hot path: send (start, finish), write, receive, hold, drain, ack, resend, free, end update.
    // The largest message first, alone, then a mix; every third packet is lost.
    double now = 1.0;
    int dropped = 0;
    uint64_t random = 12345;
    check( send_message( a, 0, (uint32_t) config.max_message_bytes ) != 0 );
    int rounds_sent = 0;
    for ( int round = 0; round < 6000; round++ )
    {
        if ( rounds_sent >= 150 && everything_delivered( a, b ) && a->region->descriptors_free == a->region->queue )
            break;

        // the application paces itself to the room the connection has (the maximal message holds all of it at first)
        random = random * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint32_t pick = (uint32_t) ( random >> 33 ) % 3;
        const uint32_t bytes = pick == 0 ? 8 + (uint32_t) ( ( random >> 40 ) % 200 ) : ( pick == 1 ? 1300 : 5000 );
        if ( rounds_sent < 150 && has_room_for( a, bytes + 16 ) && send_message( a, (uint32_t) ( ( random >> 50 ) % 4 ), bytes ) )
            rounds_sent++;

        now += 0.02;
        wire_packet_t out[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
        const int n = write_packets( a, now, out, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
        for ( int p = 0; p < n; p++ )
        {
            if ( out[p].id % 3 == 0 )
            {
                dropped++;
                continue;
            }
            open_packet( b, &out[p] );
        }
        std::vector<uint64_t> acks;
        take_acks( b, &acks );
        feed_acks( a, acks, now + 0.005 );
    }
    check( dropped > 0 );
    check_ordered_exactly_once( a, b, true );
    check( a->region->descriptors_free == a->region->queue );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );
    check( b->delivered.size() == a->sent[0].size() + a->sent[1].size() + a->sent[2].size() + a->sent[3].size() );

    // exhaustion: no room for a start is the end of the connection, not a refused send (R5)
    node_t * c = node_new( &config, 8 );
    int accepted = 0;
    while ( send_message( c, 0, 100000 ) )
        accepted++;
    check( accepted == (int) ( c->region->span_bytes / 100000 ) );          // exact capacity includes the create-time reserve
    check( counter( c, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 1 );
    check( counter( c, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) >= 1 );
    check( rn_ordered_decide( &c->ordered ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( send_message( c, 0, 16 ) == 0 );                             // nothing more is taken
    wire_packet_t none[4];
    check( write_packets( c, now, none, 4 ) == 0 );                     // and nothing is sent: it is kicked
    // the slot is freed and its region reset for the next client
    rn_region_reset( c->region );
    check( rn_ordered_init( &c->ordered, c->region, &config, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    check( rn_ordered_decide( &c->ordered ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( send_message( c, 0, 100000 ) != 0 );

    // the queue bound (Q) is the same rule
    node_t * d = node_new( &config, 8 );
    int queued = 0;
    while ( send_message( d, 1, 16 ) )
        queued++;
    check( queued == (int) d->region->queue );
    check( counter( d, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) == 1 );
    check( rn_ordered_decide( &d->ordered ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );

    // and not one allocator call came from any of it
    const uint64_t creates = 2;                                         // node c and node d: their arenas, made after the mark
    check( cm_mallocs == 2 + creates );
    check( cm_frees == 0 );
    check( cm_calls() == after_create + creates );                      // every call since the mark is one of those two creates

    node_free( a );
    node_free( b );
    node_free( c );
    node_free( d );
    cm_restore();
}

// ---------------------------------------------------------------------------
// 10. the RNChannel witnesses, replayed on the code
// ---------------------------------------------------------------------------

// the model's instance (tla/README-RNChannel.md): channel 1 carries message 0
// (small), message 2 (two chunks) and message 3 (small); channel 2 carries
// message 1 (small). Reliable numbers {0}, {2,3}, {4}, {1}. Here channel 0 is
// the model's channel 1 and channel 1 is its channel 2. Each message goes in
// an update of its own, so each travels in a packet of its own.

struct model_t
{
    node_t * a;
    node_t * b;
    wire_packet_t packet[8];            // message 0, message 1, chunk 0 and 1 of message 2, message 3
    uint64_t id[4];                     // messages 0 to 3
    int count;
    double now;
};

static const uint32_t model_chunk_bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES + 600;      // above one packet's record room: two chunks

static void model_setup( model_t * m, const rocketnet_message_config_t * config )
{
    m->a = node_new( config, 8 );
    m->b = node_new( config, 8 );
    m->now = 1.0;
    m->count = 0;
    // update 1 to 4, in the model's number order
    m->id[0] = send_message( m->a, 0, 40 );
    m->count += write_packets( m->a, m->now, m->packet + m->count, 8 - m->count );
    m->now += 0.001;
    m->id[1] = send_message( m->a, 1, 50 );
    m->count += write_packets( m->a, m->now, m->packet + m->count, 8 - m->count );
    m->now += 0.001;
    m->id[2] = send_message( m->a, 0, model_chunk_bytes );
    m->count += write_packets( m->a, m->now, m->packet + m->count, 8 - m->count );
    m->now += 0.001;
    m->id[3] = send_message( m->a, 0, 60 );
    m->count += write_packets( m->a, m->now, m->packet + m->count, 8 - m->count );
    check( m->id[0] && m->id[1] && m->id[2] && m->id[3] );
    check( m->count == 5 );
    check( m->a->ordered.send.next == 5 );                       // numbers 0, 1, 2 3, 4 as the model has them
}

// the packet that carries a reliable number

static const wire_packet_t * packet_carrying( model_t * m, uint32_t number )
{
    for ( int p = 0; p < m->count; p++ )
    {
        rocketnet_message_record_t records[8];
        const int n = packet_records( m->a, &m->packet[p], records, 8 );
        for ( int r = 0; r < n; r++ )
            if ( records[r].number == number )
                return &m->packet[p];
    }
    check( false );
    return NULL;
}

static void model_ack( model_t * m, double now )
{
    std::vector<uint64_t> acks;
    take_acks( m->b, &acks );
    feed_acks( m->a, acks, now );
}

static void test_rnchannel_witness_replays_ordered()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );

    // BrokenOutOfOrder: message 0's packet is lost. Message 1 reaches its channel; both chunks of
    // message 2 and message 3 arrive. Channel 0 delivers nothing: no message past a missing predecessor.
    {
        model_t m;
        model_setup( &m, &config );
        check( open_packet( m.b, packet_carrying( &m, 1 ) ) );
        check( open_packet( m.b, packet_carrying( &m, 3 ) ) );
        check( open_packet( m.b, packet_carrying( &m, 2 ) ) );
        check( open_packet( m.b, packet_carrying( &m, 4 ) ) );
        check( m.b->got[0].empty() );
        check( m.b->got[1].size() == 1 && m.b->got[1][0] == m.id[1] );
        check( m.b->violations == 0 );

        // message 0 arrives later (its resend): 0, 2, 3 are delivered, in that order, exactly once
        model_ack( &m, m.now + 0.01 );
        m.now += 0.2;
        wire_packet_t resend[4];
        check( write_packets( m.a, m.now, resend, 4 ) == 1 );
        check( open_packet( m.b, &resend[0] ) );
        check( m.b->got[0].size() == 3 );
        check( m.b->got[0][0] == m.id[0] && m.b->got[0][1] == m.id[2] && m.b->got[0][2] == m.id[3] );
        check( counter( m.b, ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND ) == 0 );
        node_free( m.a );
        node_free( m.b );
    }

    // BrokenTwice: the same reliable number written again under another packet id is delivered once
    {
        model_t m;
        model_setup( &m, &config );
        check( open_packet( m.b, packet_carrying( &m, 0 ) ) );
        check( m.b->got[0].size() == 1 );
        m.now += 0.2;
        wire_packet_t again[4];
        const int count = write_packets( m.a, m.now, again, 4 );       // nothing was acked: every number resends
        check( count >= 1 );
        const uint64_t duplicates = counter( m.b, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
        for ( int p = 0; p < count; p++ )
            if ( again[p].bytes > 0 && again[p].id >= 6 )
                open_packet( m.b, &again[p] );
        check( counter( m.b, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) > duplicates );
        // message 0 appears exactly once in channel 0's sequence
        int zero = 0;
        for ( size_t i = 0; i < m.b->got[0].size(); i++ )
            if ( m.b->got[0][i] == m.id[0] )
                zero++;
        check( zero == 1 );
        check( m.b->violations == 0 );
        node_free( m.a );
        node_free( m.b );
    }

    // BrokenDropOnRetransmit: the packet carrying number 0 is lost, and lost again. The number
    // stays unacked, stays in the resend set, and is resent until it gets through.
    {
        model_t m;
        model_setup( &m, &config );
        for ( int p = 1; p < m.count; p++ )
            open_packet( m.b, &m.packet[p] );
        model_ack( &m, m.now + 0.01 );
        check( m.b->got[0].empty() );
        int lost = 0;
        bool delivered = false;
        for ( int step = 0; step < 400 && !delivered; step++ )
        {
            m.now += 0.05;
            wire_packet_t out[4];
            const int n = write_packets( m.a, m.now, out, 4 );
            for ( int p = 0; p < n; p++ )
            {
                if ( lost < 3 )
                {
                    lost++;                                            // the first three transmissions of number 0 are lost
                    continue;
                }
                open_packet( m.b, &out[p] );
            }
            model_ack( &m, m.now + 0.01 );
            delivered = m.b->got[0].size() == 3;
            check( m.a->ordered.ack.resend_count == (uint32_t) ( delivered ? 0 : 1 ) );
        }
        check( delivered );
        check( lost == 3 );
        check( counter( m.a, ROCKETNET_COUNTER_MESSAGE_RESENT ) >= 3 );
        check( m.b->got[0][0] == m.id[0] && m.b->got[0][1] == m.id[2] && m.b->got[0][2] == m.id[3] );
        node_free( m.a );
        node_free( m.b );
    }

    // BrokenPartialChunk: chunk 1 of message 2 arrives (its LAST index) while chunk 0 is missing.
    // The message is not delivered, and neither is message 3 behind it.
    {
        model_t m;
        model_setup( &m, &config );
        check( open_packet( m.b, packet_carrying( &m, 0 ) ) );
        check( open_packet( m.b, packet_carrying( &m, 1 ) ) );
        check( open_packet( m.b, packet_carrying( &m, 3 ) ) );           // chunk 1 (number 3)
        check( open_packet( m.b, packet_carrying( &m, 4 ) ) );           // message 3
        check( m.b->got[0].size() == 1 && m.b->got[0][0] == m.id[0] );
        check( m.b->delivered.size() == 2 );                              // message 0 and message 1 only
        check( open_packet( m.b, packet_carrying( &m, 2 ) ) );           // chunk 0
        check( m.b->got[0].size() == 3 && m.b->got[0][1] == m.id[2] && m.b->got[0][2] == m.id[3] );
        node_free( m.a );
        node_free( m.b );
    }

    // BrokenChunkBlocksSmall (NoHeadOfLineDeadlock): a large message queues while the window has no room
    // for it. Nothing behind it overtakes it, and when acks return room it is numbered, then the small
    // message after it, and both are delivered. Neither mode deadlocks the other.
    {
        rocketnet_message_config_t narrow;
        node_config( &narrow, 64, 262144, 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES, 800000 );
        node_t * a = node_new( &narrow, 16 );
        node_t * b = node_new( &narrow, 16 );
        double now = 1.0;

        // 63 small messages fill the window except one number
        for ( int i = 0; i < 63; i++ )
            check( send_message( a, 1, 16 ) != 0 );
        wire_packet_t out[16];
        int written = 0;
        wire_packet_t held_back[16];
        while ( written < 4 )
        {
            now += 0.001;
            const int n = write_packets( a, now, out, 16 );
            check( n > 0 );
            for ( int p = 0; p < n; p++ )
                held_back[written++] = out[p];
        }
        check( a->ordered.send.next == 63 );

        // the large message (4 chunks) and a small one behind it, on the same channel, and a third on another channel
        const uint64_t large = send_message( a, 0, 4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES );
        const uint64_t after = send_message( a, 0, 24 );
        const uint64_t other = send_message( a, 2, 24 );
        check( large && after && other );
        now += 0.001;
        const uint64_t stalls = counter( a, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL );
        check( write_packets( a, now, out, 16 ) == 0 );                   // no room: 63 + 4 is past U' + W
        check( counter( a, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) == stalls + 1 );
        check( a->ordered.send.next == 63 );                              // the small ones behind it did not overtake

        // the first four packets arrive and are acked: the window reopens
        for ( int p = 0; p < written; p++ )
            check( open_packet( b, &held_back[p] ) );
        std::vector<uint64_t> acks;
        take_acks( b, &acks );
        now += 0.01;
        feed_acks( a, acks, now );
        check( a->ordered.send.U == 63 );

        // the block is numbered whole, then the small ones; all go out and are delivered in order
        for ( int step = 0; step < 20; step++ )
        {
            now += 0.01;
            const int n = write_packets( a, now, out, 16 );
            for ( int p = 0; p < n; p++ )
                open_packet( b, &out[p] );
            acks.clear();
            take_acks( b, &acks );
            feed_acks( a, acks, now + 0.001 );
        }
        check( b->got[0].size() == 2 && b->got[0][0] == large && b->got[0][1] == after );
        check( b->got[2].size() == 1 && b->got[2][0] == other );
        check( b->got[1].size() == 63 );
        check( a->region->descriptors_free == a->region->queue );
        node_free( a );
        node_free( b );
    }

    // BrokenNoSequencedReset: an ordered transition leaves no sequenced epoch running, on send and on delivery
    {
        model_t m;
        model_setup( &m, &config );
        m.a->region->channel_send[1].sequenced_write = 7;
        check( send_message( m.a, 1, 20 ) != 0 );
        check( m.a->region->channel_send[1].sequenced_write == 0 );
        check( m.a->region->channel_send[1].ordered_write == 2 );        // message 1, then this one

        m.b->region->channel_receive[1].sequenced_watermark = 9;
        check( open_packet( m.b, packet_carrying( &m, 1 ) ) );
        check( m.b->region->channel_receive[1].sequenced_watermark == 0 );
        node_free( m.a );
        node_free( m.b );
    }
}

// A record first sent at time 0.0 waits for its resend like any other: 0.0
// is a real send time, not "never sent" (RNResendTiming!Due reads lastSend as
// a real tick, 0 included).

static void test_ordered_record_sent_at_zero_waits_for_its_resend()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 4 );

    check( send_message( a, 0, 16 ) != 0 );

    // the game clock starts at 0.0; the first write sends the record once
    wire_packet_t first[4];
    check( write_packets( a, 0.0, first, 4 ) == 1 );
    const double delay = rn_window_ack_class_delay( &a->ordered.ack, false );

    // until the resend delay passes, nothing is written again: the record was
    // sent at 0.0, it is not unsent
    for ( int k = 1; k / 60.0 < delay; k++ )
        check( write_packets( a, k / 60.0, first, 4 ) == 0 );

    // the first update at or past the delay resends it exactly once
    wire_packet_t resend[4];
    check( write_packets( a, delay, resend, 4 ) == 1 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 1 );

    // and not again the next update
    check( write_packets( a, delay + 1.0 / 60.0, first, 4 ) == 0 );

    // the first send is recorded once, at 0.0, and no reliable timeout
    check( rn_window_ack_check_timeout( &a->ordered.ack, delay ) == 0 );
    check( a->ordered.ack.oldest_first_send == 0.0 );

    node_free( a );
}

static void test_ordered_record_sent_at_1000_waits_for_its_resend()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 4 );

    check( send_message( a, 0, 16 ) != 0 );

    wire_packet_t first[4];
    check( write_packets( a, 1000.0, first, 4 ) == 1 );
    const double delay = rn_window_ack_class_delay( &a->ordered.ack, false );

    for ( int k = 1; 1000.0 + k / 60.0 < 1000.0 + delay; k++ )
        check( write_packets( a, 1000.0 + k / 60.0, first, 4 ) == 0 );

    wire_packet_t resend[4];
    check( write_packets( a, 1000.0 + delay, resend, 4 ) == 1 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 1 );

    check( write_packets( a, 1000.0 + delay + 1.0 / 60.0, first, 4 ) == 0 );

    check( rn_window_ack_check_timeout( &a->ordered.ack, 1000.0 + delay ) == 0 );
    check( a->ordered.ack.oldest_first_send == 1000.0 );

    node_free( a );
}

static void test_ordered_failed_send_at_zero_is_due_next_update()
{
    rocketnet_message_config_t config;
    node_config( &config, 1024, 262144, 262144, 800000 );
    node_t * a = node_new( &config, 4 );

    check( send_message( a, 0, 16 ) != 0 );

    // write at 0.0, then fail the packet in place of committing
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    pointers[0] = frames[0];
    check( rn_ordered_write( &a->ordered, 0.0, pointers, 1 ) == 1 );
    rn_ordered_packet_failed( &a->ordered, 0, 0.0 );

    // the failed-send retry is due at the next update, as a first write (resend = 0)
    wire_packet_t out[4];
    check( write_packets( a, 1.0 / 60.0, out, 4 ) == 1 );
    check( counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 0 );

    // and not again immediately after
    check( write_packets( a, 2.0 / 60.0, out, 4 ) == 0 );

    node_free( a );
}

REGISTER_UNIT_TEST( test_ordered_record_sent_at_zero_waits_for_its_resend );
REGISTER_UNIT_TEST( test_ordered_record_sent_at_1000_waits_for_its_resend );
REGISTER_UNIT_TEST( test_ordered_failed_send_at_zero_is_due_next_update );
REGISTER_UNIT_TEST( test_small_in_order_exactly_once_under_loss_and_reorder );
REGISTER_UNIT_TEST( test_small_packed_and_resent_until_packet_acked );
REGISTER_UNIT_TEST( test_large_chunked_one_per_packet_and_delivered_whole );
REGISTER_UNIT_TEST( test_large_between_small_keeps_order );
REGISTER_UNIT_TEST( test_chunks_placed_by_index_not_arrival );
REGISTER_UNIT_TEST( test_no_partial_delivery );
REGISTER_UNIT_TEST( test_hostile_chunk_count_or_index_kicks );
REGISTER_UNIT_TEST( test_hostile_reliable_chunks_are_refused );
REGISTER_UNIT_TEST( test_hostile_small_at_held_ordering_is_refused );
REGISTER_UNIT_TEST( test_hostile_small_at_held_ordering_is_refused_sim );
REGISTER_UNIT_TEST( test_start_pointer_is_the_frame_source );
REGISTER_UNIT_TEST( test_ordered_no_allocation_after_create );
REGISTER_UNIT_TEST( test_rnchannel_witness_replays_ordered );
