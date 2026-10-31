/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#include <stdio.h>
#include <string.h>
#include <vector>
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_wire.h"
#define check rocketnet_unit_check
#define BOUNDARY_MAX_FRAMES 64
#define BOUNDARY_PAYLOAD_INDEX 16
#define BOUNDARY_FRAME_CAPACITY (ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 256)
// the padding to data and after it is written out, so the compiler adds none (MSVC C4324)
struct boundary_packet_t
{
    uint64_t id;
    int slot;
    int bytes;
    uint8_t head_pad[32 - sizeof( uint64_t ) - 2 * sizeof( int )];
    alignas( 16 ) uint8_t data[BOUNDARY_FRAME_CAPACITY];
    uint8_t tail_pad[( BOUNDARY_FRAME_CAPACITY + 15 ) / 16 * 16 - BOUNDARY_FRAME_CAPACITY];
};
struct boundary_net_t;
struct boundary_endpoint_t
{
    boundary_net_t * net;
    bool is_server;
    int slot;
    alignas( 16 ) uint8_t frames[BOUNDARY_MAX_FRAMES][BOUNDARY_PAYLOAD_INDEX + BOUNDARY_FRAME_CAPACITY];
    bool frame_used[BOUNDARY_MAX_FRAMES];
    uint64_t frame_id[BOUNDARY_MAX_FRAMES];
    int frame_slot[BOUNDARY_MAX_FRAMES];
    std::vector<boundary_packet_t> inbound, batch;
    std::vector<uint8_t *> batch_data;
    std::vector<int> batch_bytes, batch_slot;
    std::vector<uint64_t> acks_inbox, acked;
    std::vector<int> acks_inbox_slot, acked_slot;
    bool connected;
    int disconnects;
    double time;
};
struct boundary_net_t
{
    boundary_endpoint_t * server;
    boundary_endpoint_t * client;
    uint64_t slot_connection;
    uint64_t next_packet_id;
};
static void boundary_update( void * transport, double time )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    e->time = time;
    e->acked.clear();
    e->acked_slot.clear();
    e->acked.swap( e->acks_inbox );
    e->acked_slot.swap( e->acks_inbox_slot );
    e->batch.clear();
    e->batch_data.clear();
    e->batch_bytes.clear();
    e->batch_slot.clear();
    e->batch.reserve( e->inbound.size() );

    for ( size_t i = 0; i < e->inbound.size(); ++i )
    {
        const boundary_packet_t & p = e->inbound[i];
        if ( ( e->is_server && e->net->slot_connection == 0 ) || ( !e->is_server && !e->connected ) )
            continue;

        boundary_endpoint_t * sender = e->is_server ? e->net->client : e->net->server;
        e->batch.push_back( p );
        sender->acks_inbox.push_back( p.id );
        sender->acks_inbox_slot.push_back( p.slot );
    }

    e->inbound.clear();

    for ( size_t i = 0; i < e->batch.size(); ++i )
    {
        e->batch_data.push_back( e->batch[i].data );
        e->batch_bytes.push_back( e->batch[i].bytes );
        e->batch_slot.push_back( e->batch[i].slot );
    }
}
static int boundary_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    *client_index = e->is_server ? e->batch_slot.data() : NULL;
    *payload_data = e->batch_data.data();
    *payload_bytes = e->batch_bytes.data();

    return (int) e->batch.size();
}
static int boundary_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    *client_index = e->is_server ? e->acked_slot.data() : NULL;
    *packet_ids = e->acked.data();
    return (int) e->acked.size();
}
static int boundary_max_payload( void * transport )
{
    (void) transport;
    return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}
static uint8_t * boundary_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    memset( context, 0, sizeof( *context ) );
    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return NULL;
    if ( e->is_server ? e->net->slot_connection == 0 : !e->connected )
        return NULL;
    for ( int f = 0; f < BOUNDARY_MAX_FRAMES; ++f )
        if ( !e->frame_used[f] )
        {
            e->frame_used[f] = true;
            e->frame_id[f] = e->net->next_packet_id++;
            e->frame_slot[f] = e->is_server ? client_index : e->slot;
            context->frame = e->frames[f];
            context->payload_index = BOUNDARY_PAYLOAD_INDEX;
            context->payload_capacity = request_bytes;
            context->packet_id = e->frame_id[f];
            context->client_index = client_index;
            return e->frames[f];
        }

    return NULL;
}
static int boundary_frame_of( boundary_endpoint_t * e, const rocketnet_packet_context_t * context )
{
    for ( int f = 0; f < BOUNDARY_MAX_FRAMES; ++f )
        if ( e->frame_used[f] && context->frame == e->frames[f] )
            return f;
    return -1;
}
static int boundary_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    const int f = boundary_frame_of( e, context );
    check( f >= 0 );
    check( payload_bytes >= 1 && payload_bytes <= context->payload_capacity );
    e->frame_used[f] = false;

    boundary_endpoint_t * receiver = e->is_server ? e->net->client : e->net->server;
    const uint8_t * payload = e->frames[f] + BOUNDARY_PAYLOAD_INDEX;

    boundary_packet_t p = {};
    p.id = e->frame_id[f];
    p.slot = e->frame_slot[f];
    p.bytes = payload_bytes;
    memcpy( p.data, payload, (size_t) payload_bytes );
    receiver->inbound.push_back( p );
    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_bytes );
}
static void boundary_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    const int f = boundary_frame_of( e, context );
    if ( f >= 0 )
    {
        e->frame_used[f] = false;
    }
}
static void boundary_send_packets( void * transport ) { (void) transport; }
static uint64_t boundary_connection( void * transport, int client_index )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    (void) client_index;
    return e->is_server ? e->net->slot_connection : e->connected ? 1
                                                                 : 0;
}
static void boundary_disconnect( void * transport, int client_index )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    (void) client_index;
    e->disconnects++;
    if ( e->is_server )
        e->net->slot_connection = 0;
    else
        e->connected = false;
}
static int boundary_state( void * transport )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    return e->is_server ? ROCKETNET_SERVER_TRANSPORT_RUNNING : e->connected ? ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED
                                                                            : ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
}
static rn_message_link_t boundary_link( boundary_endpoint_t * e )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = e;
    link.update = boundary_update;
    link.payloads = boundary_payloads;
    link.acked = boundary_acked;
    link.max_payload_bytes = boundary_max_payload;
    link.start_packet = boundary_start_packet;
    link.finish_packet = boundary_finish_packet;
    link.abort_packet = boundary_abort_packet;
    link.send_packets = boundary_send_packets;
    link.connection = boundary_connection;
    link.disconnect = boundary_disconnect;
    link.state = boundary_state;
    return link;
}
struct boundary_pair_t
{
    boundary_net_t * net;
    rocketnet_message_server_t * server;
    rocketnet_message_client_t * client;
    double time;
};
struct boundary_received_t
{
    int channel;
    std::vector<uint8_t> data;
};
static void boundary_config( rocketnet_message_config_t * c, int max_message_bytes )
{
    rocketnet_message_default_config( c );
    c->num_channels = 4;
    c->reliable_messages_in_flight = 64;
    c->receive_message_buffer_bytes = 65536;
    c->max_message_bytes = max_message_bytes;
    c->send_message_buffer_bytes = 65536;
    c->send_queue_messages = 64;
    c->packets_in_flight = 64;
    c->packets_per_update = 8;
    c->unreliable_messages_per_update = 64;
    c->message_resend_delay_initial_ms = 100;
    c->message_resend_delay_min_ms = 20;
    c->message_resend_delay_max_ms = 400;
    c->reliable_message_timeout_ms = 120000;
}
static void boundary_setup_max( boundary_pair_t * pair, int max_message_bytes )
{
    rocketnet_message_config_t config;
    boundary_config( &config, max_message_bytes );
    pair->net = new boundary_net_t();
    pair->net->slot_connection = 100;
    pair->net->next_packet_id = 1000;
    pair->net->server = new boundary_endpoint_t();
    pair->net->client = new boundary_endpoint_t();
    memset( pair->net->server->frame_used, 0, sizeof( pair->net->server->frame_used ) );
    memset( pair->net->client->frame_used, 0, sizeof( pair->net->client->frame_used ) );
    pair->net->server->net = pair->net;
    pair->net->server->is_server = true;
    pair->net->server->slot = -1;
    pair->net->server->connected = false;
    pair->net->server->disconnects = 0;
    pair->net->client->net = pair->net;
    pair->net->client->is_server = false;
    pair->net->client->slot = 0;
    pair->net->client->connected = true;
    pair->net->client->disconnects = 0;
    pair->net->server->time = 1000.0;
    pair->net->client->time = 1000.0;

    rn_message_link_t sl = boundary_link( pair->net->server ), cl = boundary_link( pair->net->client );
    pair->server = (rocketnet_message_server_t *) rn_message_object_create( NULL, &sl, 1, 1, &config, NULL, 0, 100000, 1000.0 );
    pair->client = (rocketnet_message_client_t *) rn_message_object_create( NULL, &cl, 0, 1, &config, NULL, 0, 100000, 1000.0 );
    check( pair->server );
    check( pair->client );
    pair->time = 1000.0;
}
static void boundary_setup( boundary_pair_t * pair ) { boundary_setup_max( pair, 16384 ); }
static void boundary_teardown( boundary_pair_t * pair )
{
    rocketnet_message_client_destroy( pair->client );
    rocketnet_message_server_destroy( pair->server );
    delete pair->net->server;
    delete pair->net->client;
    delete pair->net;
}
static void boundary_drain( boundary_pair_t * pair, std::vector<boundary_received_t> * got )
{
    // the one receive queue, in batches: each entry names the channel it was delivered on

    for ( ;; )
    {
        static rocketnet_message_batch_t batch;     // 1.5 MB: never on the stack (Windows gives 1 MB)
        const int count = rocketnet_message_server_receive_messages( pair->server, &batch );
        if ( count == 0 )
            break;

        for ( int i = 0; i < batch.num_messages; ++i )
        {
            check( batch.client_index[i] == 0 );

            boundary_received_t r;
            r.channel = batch.channel[i];
            r.data.assign( batch.data[i], batch.data[i] + batch.bytes[i] );
            got->push_back( r );
        }
    }
}
static void boundary_step( boundary_pair_t * pair, std::vector<boundary_received_t> * got )
{
    pair->time += 0.01;
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_client_update( pair->client, pair->time ) == ROCKETNET_UPDATE_OK );
    boundary_drain( pair, got );
    rocketnet_message_server_send_packets( pair->server );
    rocketnet_message_client_send_packets( pair->client );
}
static std::vector<uint8_t> boundary_data( uint32_t sequence, int bytes )
{
    std::vector<uint8_t> data( (size_t) bytes );
    for ( int i = 0; i < bytes; ++i )
    {
        data[(size_t) i] = (uint8_t) ( (uint32_t) i + sequence );
    }
    return data;
}
static void boundary_check_counter( const rocketnet_counters_t * counters, int index ) { check( rocketnet_counter_value( counters, index ) == 0 ); }
static void test_message_boundary_sizes_every_mode_every_channel()
{
    const int sizes[] = { 1404, 16, 1405, 16, 1406, 16 };

    for ( int reliability = 0; reliability < ROCKETNET_CHANNEL_NUM_TYPES; ++reliability )
    {
        // only channel-bearing wire codes route across the configured channel set
        const int channels = reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED ||
                                     reliability == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED || reliability == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED ||
                                     reliability == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT
                                 ? 4
                                 : 1;
        for ( int channel = 0; channel < channels; ++channel )
        {
            boundary_pair_t pair;
            boundary_setup( &pair );

            std::vector<boundary_received_t> got;
            boundary_step( &pair, &got );
            std::vector<std::vector<uint8_t>> expected;

            for ( int i = 0; i < 6; ++i )
            {
                const uint32_t sequence = (uint32_t) ( 1 + reliability * 32 + channel * 8 + i );
                expected.push_back( boundary_data( sequence, sizes[i] ) );

                const uint32_t serial = rocketnet_message_client_send( pair.client, channel, reliability, expected.back().data(), (int) expected.back().size() );
                if ( serial == 0 )
                {
                    fprintf( stderr, "boundary send refused mode=%d channel=%d bytes=%d\n", reliability, channel, sizes[i] );
                }
                check( serial != 0 );
            // let each send land before the next, so a promoted twin never
            // overtakes an older unreliable sequenced message in one batch
                for ( int flush = 0; flush < 8; ++flush )
                {
                    boundary_step( &pair, &got );
                }
            }

            for ( int step = 0; step < 512 && got.size() < expected.size(); ++step )
            {
                boundary_step( &pair, &got );
            }
            for ( int step = 0; step < 16; ++step )
            {
                boundary_step( &pair, &got );
            }

            if ( got.size() != expected.size() )
            {
                fprintf( stderr, "boundary count mode=%d channel=%d got=%zu expected=%zu\n", reliability, channel, got.size(), expected.size() );

                for ( int j = 0; j < 6; ++j )
                {
                    bool seen = false;
                    for ( size_t k = 0; k < got.size(); ++k )
                        if ( got[k].data == expected[(size_t) j] )
                            seen = true;
                    if ( !seen )
                    {
                        fprintf( stderr, "boundary missing mode=%d channel=%d bytes=%d\n", reliability, channel, sizes[j] );
                    }
                }
            }

            check( got.size() == expected.size() );

            const bool ordered = reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED || reliability == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED ||
                                 reliability == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED || reliability == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT;
            bool matched[6] = { false, false, false, false, false, false };

            for ( size_t i = 0; i < got.size(); ++i )
            {
                if ( got[i].channel != channel )
                {
                    fprintf( stderr, "boundary channel mode=%d expected=%d got=%d bytes=%zu first=%d\n", reliability, channel, got[i].channel, got[i].data.size(), got[i].data.empty() ? -1 : got[i].data[0] );
                }
                check( got[i].channel == channel );

                int found = -1;
                for ( int j = 0; j < 6; ++j )
                    if ( !matched[j] && got[i].data == expected[(size_t) j] )
                    {
                        found = j;
                        break;
                    }
                if ( found < 0 )
                {
                    fprintf( stderr, "boundary data mode=%d channel=%d received=%zu\n", reliability, channel, got[i].data.size() );
                }
                check( found >= 0 );
                matched[found] = true;
                if ( ordered && found >= 0 )
                {
                    check( (int) i == found );
                }
            }

            for ( int i = 0; i < 6; ++i )
            {
                if ( !matched[i] )
                {
                    fprintf( stderr, "boundary missing mode=%d channel=%d bytes=%d\n", reliability, channel, sizes[i] );
                }
                check( matched[i] );
            }

            check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
            check( rocketnet_message_server_client_connected( pair.server, 0 ) == 1 );
            check( pair.net->client->disconnects == 0 && pair.net->server->disconnects == 0 );

            const rocketnet_counters_t * cc = rocketnet_message_client_counters( pair.client );
            const rocketnet_counters_t * sc = rocketnet_message_server_counters( pair.server );
            boundary_check_counter( cc, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
            boundary_check_counter( cc, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL );
            boundary_check_counter( cc, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED );
            boundary_check_counter( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
            boundary_check_counter( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
            boundary_check_counter( cc, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
            boundary_check_counter( cc, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
            boundary_check_counter( sc, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED );
            boundary_check_counter( sc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
            boundary_check_counter( sc, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
            boundary_check_counter( sc, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
        // Code 1 promotes 1406, the first size past one packet's record.

            const uint32_t promoted = (uint32_t) rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED );
            check( promoted == ( reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED ? 1u : 0u ) );
            check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE ) == 0 );
            check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT ) == 0 );
            boundary_teardown( &pair );
        }
    }
}
REGISTER_UNIT_TEST( test_message_boundary_sizes_every_mode_every_channel );

static void test_message_boundary_oversize_unreliable_counts_failed_not_refused()
{
    boundary_pair_t pair;
    boundary_setup( &pair );

    std::vector<boundary_received_t> got;
    boundary_step( &pair, &got );

    const rocketnet_counters_t * cc = rocketnet_message_client_counters( pair.client );
    const uint64_t failed_before = rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
    const uint64_t refused_before = rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    const int oversize = 16384 + 1;
    const uint32_t serial = rocketnet_message_client_send( pair.client, 0, ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED,
                                                           boundary_data( 1, oversize ).data(), oversize );
    check( serial == 0 );
    check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed_before + 1 );
    check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused_before );
    boundary_teardown( &pair );
}
REGISTER_UNIT_TEST( test_message_boundary_oversize_unreliable_counts_failed_not_refused );

// max_message_bytes holds on the send side in every mode: with a 1416-byte
// limit, 1417 and 2000 bytes are refused and
// never reach the server, and 1416 bytes is delivered whole.

static void test_message_boundary_every_mode_refuses_over_max_message_bytes()
{
    const int limit = 1416;

    for ( int reliability = 0; reliability < ROCKETNET_CHANNEL_NUM_TYPES; ++reliability )
    {
        boundary_pair_t pair;
        boundary_setup_max( &pair, limit );

        std::vector<boundary_received_t> got;
        boundary_step( &pair, &got );

        const int over[2] = { limit + 1, 2000 };

        for ( int i = 0; i < 2; ++i )
        {
            const std::vector<uint8_t> data = boundary_data( (uint32_t) ( reliability * 4 + i ), over[i] );
            check( rocketnet_message_client_send( pair.client, 0, reliability, data.data(), over[i] ) == 0 );
        }

        const std::vector<uint8_t> at_limit = boundary_data( (uint32_t) ( reliability * 4 + 3 ), limit );
        check( rocketnet_message_client_send( pair.client, 0, reliability, at_limit.data(), limit ) != 0 );
        for ( int step = 0; step < 64 && got.empty(); ++step )
        {
            boundary_step( &pair, &got );
        }
        for ( int step = 0; step < 8; ++step )
        {
            boundary_step( &pair, &got );
        }
        check( got.size() == 1 );
        if ( !got.empty() )
        {
            check( got[0].data == at_limit );
        }
        check( rocketnet_counter_value( rocketnet_message_server_counters( pair.server ), ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) == 0 );
        check( pair.net->client->disconnects == 0 && pair.net->server->disconnects == 0 );
        boundary_teardown( &pair );
    }
}
REGISTER_UNIT_TEST( test_message_boundary_every_mode_refuses_over_max_message_bytes );
