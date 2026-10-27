/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <vector>
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_fragment.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_wire.h"
#define check rocketnet_unit_check
#if ROCKETNET_MAX_CHANNELS > 300
#define BOUNDARY_TEST_NUM_CHANNELS 301
#define BOUNDARY_TEST_CHANNEL_COUNT 2
#define BOUNDARY_TEST_CHANNELS { 0, 300 }
#elif ROCKETNET_MAX_CHANNELS > 31
#define BOUNDARY_TEST_NUM_CHANNELS 32
#define BOUNDARY_TEST_CHANNEL_COUNT 2
#define BOUNDARY_TEST_CHANNELS { 0, 31 }
#else
#define BOUNDARY_TEST_NUM_CHANNELS ROCKETNET_MAX_CHANNELS
#define BOUNDARY_TEST_CHANNEL_COUNT 1
#define BOUNDARY_TEST_CHANNELS { 0 }
#endif // #if ROCKETNET_MAX_CHANNELS > 300
#define BOUNDARY_MAX_FRAMES 64
#define BOUNDARY_PAYLOAD_INDEX 16
#define BOUNDARY_FRAME_CAPACITY (ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 256)
// the padding to data and after it is written out, so the compiler adds none (MSVC C4324)
struct boundary_packet_t
{
    uint64_t id;
    int slot;
    int bytes;
    int fragment;
    uint8_t head_pad[32 - sizeof( uint64_t ) - 3 * sizeof( int )];
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
    uint32_t next_payload_id;
    rocketnet_reassembly_set_t * reassembly;
    uint64_t fragment_counters[ROCKETNET_NUM_COUNTERS];
};
static const rocketnet_reassembly_counters_t boundary_fragment_counter_table = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID, ROCKETNET_COUNTER_FRAGMENT_STALE, ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE, ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH, ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE, ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX, ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED, ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED, ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL
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
    if ( e->is_server )
    {
        rocketnet_reassembly_set_begin_update( e->net->reassembly );
    }

    for ( size_t i = 0; i < e->inbound.size(); ++i )
    {
        const boundary_packet_t & p = e->inbound[i];
        if ( ( e->is_server && e->net->slot_connection == 0 ) || ( !e->is_server && !e->connected ) )
            continue;
        boundary_endpoint_t * sender = e->is_server ? e->net->client : e->net->server;
        if ( p.fragment && e->is_server )
        {
            check( rocketnet_reassembly_process_fragment( e->net->reassembly, 0, p.data, p.bytes, e->net->fragment_counters, &boundary_fragment_counter_table ) );
            if ( e->net->reassembly->completed_payload )
            {
                boundary_packet_t complete = {};
                complete.slot = p.slot;
                complete.bytes = e->net->reassembly->completed_bytes;
                memcpy( complete.data, e->net->reassembly->completed_payload, (size_t) complete.bytes );
                e->batch.push_back( complete );
            }
        }
        else
        {
            e->batch.push_back( p );
            sender->acks_inbox.push_back( p.id );
            sender->acks_inbox_slot.push_back( p.slot );
        }
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
    return 16384;
}

static uint8_t * boundary_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    boundary_endpoint_t * e = (boundary_endpoint_t *) transport;
    memset( context, 0, sizeof( *context ) );
    if ( request_bytes < 1 || request_bytes > BOUNDARY_FRAME_CAPACITY )
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
    if ( payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
    {
        boundary_packet_t p = {};
        p.id = e->frame_id[f];
        p.slot = e->frame_slot[f];
        p.bytes = payload_bytes;
        memcpy( p.data, payload, (size_t) payload_bytes );
        receiver->inbound.push_back( p );
        return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_bytes );
    }

    int fragments = 0, stride = 0;
    check( rocketnet_fragment_plan( payload_bytes, &fragments, &stride ) );

    const uint32_t payload_id = e->net->next_payload_id++;
    int total_wire_bytes = 0;
    for ( int i = 0; i < fragments; ++i )
    {
        const int bytes = i + 1 < fragments ? stride : payload_bytes - stride * ( fragments - 1 );
        boundary_packet_t fragment = {};
        fragment.id = e->net->next_packet_id++;
        fragment.slot = e->frame_slot[f];
        fragment.fragment = 1;
        rocketnet_write_fragment_header( fragment.data, payload_id, i, fragments );
        memcpy( fragment.data + ROCKETNET_FRAGMENT_HEADER_BYTES, payload + stride * i, (size_t) bytes );
        fragment.bytes = ROCKETNET_FRAGMENT_HEADER_BYTES + bytes;
        receiver->inbound.push_back( fragment );
        const int wire_bytes = rocketnet_send_budget_max_wire_bytes( fragment.bytes );
        check( wire_bytes > 0 );

        total_wire_bytes += wire_bytes;
    }

    return total_wire_bytes;
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
static void boundary_config( rocketnet_message_config_t * c )
{
    rocketnet_message_default_config( c );
    c->num_channels = BOUNDARY_TEST_NUM_CHANNELS;
    c->reliable_messages_in_flight = 64;
    c->receive_message_buffer_bytes = 65536;
    c->max_message_bytes = 16384;
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

// The default configuration's maximum message is above the u16 record length (65535),
// so a record-length refusal cannot hide below the configured maximum.
static void boundary_default_max_config( rocketnet_message_config_t * c )
{
    rocketnet_message_default_config( c );
    c->num_channels = BOUNDARY_TEST_NUM_CHANNELS;
    c->receive_message_buffer_bytes = c->max_message_bytes;
    c->reliable_messages_in_flight = 1024;
}

static void boundary_setup( boundary_pair_t * pair, bool default_max = false )
{
    rocketnet_message_config_t config;
    if ( default_max )
        boundary_default_max_config( &config );
    else
        boundary_config( &config );
    pair->net = new boundary_net_t();
    pair->net->slot_connection = 100;
    pair->net->next_packet_id = 1000;
    pair->net->next_payload_id = 1;
    memset( pair->net->fragment_counters, 0, sizeof( pair->net->fragment_counters ) );
    pair->net->reassembly = rocketnet_reassembly_set_create( NULL, 1, 8, 16384, 64 );
    check( pair->net->reassembly );

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

static void boundary_teardown( boundary_pair_t * pair )
{
    rocketnet_message_client_destroy( pair->client );
    rocketnet_message_server_destroy( pair->server );
    rocketnet_reassembly_set_destroy( pair->net->reassembly );
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

// ---------------------------------------------------------------------------
// Oversize unreliable sends are promoted to their reliable twin
// (the promotion rule): code 0 to 2, code 1 to 4, code 5 to its reliable
// receipt twin (wire code 2, receipt kept). The threshold is the mode's own
// one-packet record edge; max_message_bytes is accepted and max+1 is refused.
// ---------------------------------------------------------------------------

static void test_unreliable_promotion_every_mode()
{
    const int modes[3] = { ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT };

    for ( int m = 0; m < 3; ++m )
    {
        const int reliability = modes[m];
        static const int channels[] = BOUNDARY_TEST_CHANNELS;
        for ( int c = 0; c < BOUNDARY_TEST_CHANNEL_COUNT; ++c )
        {
            const int channel = channels[c];
            boundary_pair_t pair;
            boundary_setup( &pair );

            std::vector<boundary_received_t> got;
            boundary_step( &pair, &got );

            // the fixture transport exceeds the packet cap, so the packet cap bounds each record

            const int record_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
            const int header = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? 3 + 3 + 3 + ( channel >= 256 ? 2 : 1 ) : ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
            const int one_packet_capacity = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? record_capacity : ( record_capacity < ROCKETNET_MESSAGE_SINGLE_BYTES ? record_capacity : ROCKETNET_MESSAGE_SINGLE_BYTES );
            const int edge = one_packet_capacity - header - 1;
            int sizes[10] = { 1404, 1405, 1406, edge - 1, edge, edge + 1, edge * 2, edge * 4, 16384, 16385 };
            int size_count = 10;
            for ( int a = 0; a < size_count; ++a )
                for ( int b = a + 1; b < size_count; ++b )
                    if ( sizes[b] == sizes[a] )
                    {
                        for ( int k = b; k + 1 < size_count; ++k )
                        {
                            sizes[k] = sizes[k + 1];
                        }

                        --size_count;
                        --b;
                    }

            int expected_promotions = 0;
            int expected_refusals = 0;
            static const int promotion_counters[3] = { ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE,
                                                       ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT };
            const rocketnet_counters_t * cc = rocketnet_message_client_counters( pair.client );
            const int expected_channel = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? channel : 0;
            for ( int i = 0; i < size_count; ++i )
            {
                const int bytes = sizes[i];
                const bool promote = bytes > edge;
                const bool over_max = bytes > 16384;
                std::vector<uint8_t> message = boundary_data( (uint32_t) ( 100 + m * 64 + c * 8 + i ), bytes );
                const uint64_t before = rocketnet_counter_value( cc, promotion_counters[m] );
                const uint64_t failed_before = rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
                const uint32_t serial = rocketnet_message_client_send( pair.client, channel, reliability, message.data(), bytes );
                if ( over_max )
                {
                    check( serial == 0 );
                    check( rocketnet_counter_value( cc, promotion_counters[m] ) == before );

                    expected_refusals++;

                    check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed_before + 1 );
                }
                else
                {
                    check( serial != 0 );
                    check( rocketnet_counter_value( cc, promotion_counters[m] ) == before + ( promote ? 1u : 0u ) );
                    if ( promote )
                    {
                        expected_promotions++;
                    }

                    // drain the promoted message before a same-channel sequenced follow, preserving order

                    got.clear();

                    int found_message = 0;
                    for ( int step = 0; step < 256 && found_message == 0; ++step )
                    {
                        boundary_step( &pair, &got );
                        for ( size_t k = 0; k < got.size(); ++k )
                            if ( got[k].data == message )
                                found_message++;
                    }

                    check( found_message == 1 && got.size() == 1 && got[0].data == message );
                    check( got[0].channel == expected_channel );
                }

                std::vector<uint8_t> follow = boundary_data( (uint32_t) ( 200 + m * 64 + c * 8 + i ), 16 );
                const int follow_reliability = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED : ROCKETNET_RELIABILITY_UNRELIABLE;
                check( rocketnet_message_client_send( pair.client, channel, follow_reliability, follow.data(), 16 ) != 0 );

                got.clear();

                int found_follow = 0;
                for ( int step = 0; step < 256 && found_follow == 0; ++step )
                {
                    boundary_step( &pair, &got );
                    for ( size_t k = 0; k < got.size(); ++k )
                        if ( got[k].data == follow )
                            found_follow++;
                }

                check( found_follow == 1 && got.size() == 1 && got[0].data == follow );
                check( got[0].channel == ( follow_reliability == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED ? expected_channel : 0 ) );
            }

            // A large start may finish short; promotion follows the bytes
            // committed at finish, not the reservation capacity.

            const int reserved_bytes = edge + 1;
            std::vector<uint8_t> short_message = boundary_data( (uint32_t) ( 300 + m * 64 + c * 8 ), 16 );
            const uint64_t before_short = rocketnet_counter_value( cc, promotion_counters[m] );
            uint8_t * short_pointer = rocketnet_message_client_start( pair.client, channel, reliability, reserved_bytes );
            check( short_pointer );

            memcpy( short_pointer, short_message.data(), short_message.size() );

            check( rocketnet_message_client_finish( pair.client, 16 ) != 0 );
            check( rocketnet_counter_value( cc, promotion_counters[m] ) == before_short );

            got.clear();

            int found_short = 0;
            for ( int step = 0; step < 256 && found_short == 0; ++step )
            {
                boundary_step( &pair, &got );
                for ( size_t k = 0; k < got.size(); ++k )
                    if ( got[k].data == short_message )
                        found_short++;
            }

            check( found_short == 1 && got.size() == 1 && got[0].data == short_message );
            check( got[0].channel == expected_channel );

            check( rocketnet_counter_value( cc, promotion_counters[m] ) == (uint32_t) expected_promotions );
            for ( int other = 0; other < 3; ++other )
                if ( other != m )
                    check( rocketnet_counter_value( cc, promotion_counters[other] ) == 0 );
            check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == (uint32_t) expected_refusals );
            check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == 0 );
            check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 0 );
            check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == 0 );
            check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

            boundary_teardown( &pair );
        }
    }

    // The configured maximum refuses every unreliable source mode on both
    // start paths before promotion; none can consume a promotion count.

    for ( int m = 0; m < 3; ++m )
    {
        boundary_pair_t pair;
        boundary_setup( &pair );

        std::vector<boundary_received_t> got;
        boundary_step( &pair, &got );

        std::vector<uint8_t> too_big = boundary_data( (uint32_t) ( 7 + m ), 16385 );
        const int reliability = modes[m];
        const int promotion_counter = reliability == ROCKETNET_RELIABILITY_UNRELIABLE ? ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE : reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED
                                                                                                                                                                                                  : ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT;
        const rocketnet_counters_t * client_counters = rocketnet_message_client_counters( pair.client );
        const rocketnet_counters_t * server_counters = rocketnet_message_server_counters( pair.server );
        check( rocketnet_message_client_send( pair.client, 0, reliability, too_big.data(), 16385 ) == 0 );
        check( rocketnet_counter_value( client_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 1 );
        check( rocketnet_counter_value( client_counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == 0 );
        check( rocketnet_counter_value( client_counters, promotion_counter ) == 0 );
        check( rocketnet_message_server_start( pair.server, 0, 0, reliability, 16385 ) == NULL );
        check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 1 );
        check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == 0 );
        check( rocketnet_counter_value( server_counters, promotion_counter ) == 0 );

        boundary_teardown( &pair );
    }

    // One size rule above the u16 record length: with the default maximum
    // (above 65535) every unreliable mode, narrow and WIDE, promotes 65535,
    // 65536 and the maximum, and refuses only the maximum plus one.
    {
        rocketnet_message_config_t max_config;
        boundary_default_max_config( &max_config );
        const int max_bytes = max_config.max_message_bytes;
        check( max_bytes > 65536 );

        static const int promotion_counters[3] = { ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE,
                                                   ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT };
        static const int channels[] = BOUNDARY_TEST_CHANNELS;
        for ( int m = 0; m < 3; ++m )
            for ( int c = 0; c < BOUNDARY_TEST_CHANNEL_COUNT; ++c )
            {
                const int reliability = modes[m];
                const int channel = channels[c];
                const int expected_channel = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? channel : 0;
                boundary_pair_t pair;
                boundary_setup( &pair, true );

                std::vector<boundary_received_t> got;
                boundary_step( &pair, &got );
                const rocketnet_counters_t * cc = rocketnet_message_client_counters( pair.client );
                const int sizes[4] = { 65535, 65536, max_bytes, max_bytes + 1 };
                uint32_t promotions = 0;
                for ( int i = 0; i < 4; ++i )
                {
                    const int bytes = sizes[i];
                    std::vector<uint8_t> message = boundary_data( (uint32_t) ( 400 + m * 64 + c * 8 + i ), bytes );
                    const uint64_t failed_before = rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
                    const uint32_t serial = rocketnet_message_client_send( pair.client, channel, reliability, message.data(), bytes );
                    if ( bytes > max_bytes )
                    {
                        check( serial == 0 );
                        check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed_before + 1 );
                        check( rocketnet_counter_value( cc, promotion_counters[m] ) == promotions );
                        continue;
                    }

                    check( serial != 0 );

                    promotions++;
                    check( rocketnet_counter_value( cc, promotion_counters[m] ) == promotions );
                    check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed_before );

                    got.clear();

                    int found = 0;
                    for ( int step = 0; step < 20000 && found == 0; ++step )
                    {
                        boundary_step( &pair, &got );
                        for ( size_t k = 0; k < got.size(); ++k )
                            if ( got[k].data == message )
                                found++;
                    }

                    check( found == 1 && got.size() == 1 && got[0].channel == expected_channel );
                }

                check( rocketnet_counter_value( cc, promotion_counters[m] ) == 3 );
                for ( int other = 0; other < 3; ++other )
                    if ( other != m )
                        check( rocketnet_counter_value( cc, promotion_counters[other] ) == 0 );
                check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 1 );
                check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == 0 );
                check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) == 0 );
                check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == 0 );
                check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

                boundary_teardown( &pair );
            }
    }

    // A huge code-1 size is refused before record-overhead arithmetic can overflow

    boundary_pair_t pair;
    boundary_setup( &pair );

    std::vector<boundary_received_t> got;
    boundary_step( &pair, &got );
    const rocketnet_counters_t * cc = rocketnet_message_client_counters( pair.client );
    check( rocketnet_message_client_start( pair.client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, INT_MAX ) == NULL );
    check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 1 );
    check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED ) == 0 );
    check( rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == 0 );

    boundary_teardown( &pair );
}

REGISTER_UNIT_TEST( test_unreliable_promotion_every_mode );
