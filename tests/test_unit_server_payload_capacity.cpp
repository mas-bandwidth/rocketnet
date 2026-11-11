/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server transport's in-place receive entry refuses a payload that does
// not fit the caller's cursor whole, before publishing any payload metadata.
// A fitting sealed payload is delivered byte-exact from the open buffer with
// its live slot and connection generation, and its deferred delivery ACK is
// accepted only when the caller explicitly accepts it. The fitting payload is
// the full receive payload bound, so a payload slot holds the largest payload.
//
// A created server's payload batch holds every packet one update drains: the
// receive budget (rocketnet_server_receive_packets_per_update) sizes both, so
// no payload whose sequence the open spent is discarded. A loopback client's
// payloads are held back from the server, then one update is handed exactly
// that budget of them, and every one is delivered in that update.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_message_object.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"
#include "rocketnet_driver.h"
#include "unit_shutdown.h"
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_counter_delta( const struct rocketnet_counters_t * before,
                                 const struct rocketnet_counters_t * after,
                                 int first, int second, int third )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const uint64_t expected = ( i == first || i == second || i == third ) ? 1 : 0;
        rocketnet_unit_check( after->values[i] - before->values[i] == expected );
    }
}

static int seal_payload( uint8_t * packet, uint64_t sequence, const uint8_t * data, int data_bytes )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    rocketnet_unit_check( data_bytes <= ROCKETNET_RECEIVE_PAYLOAD_BYTES );

    memset( plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    memcpy( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, data, (size_t) data_bytes );
    return rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                          rocketnet_test_filter_value, sequence, NULL,
                                          plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + data_bytes,
                                          ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                          rocketnet_test_session_c2s_key );
}

static void test_server_payload_capacity_uses_live_session_path()
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    memset( &server, 0, sizeof( server ) );
    memset( &counters, 0, sizeof( counters ) );

    uint64_t connection_ids[1] = { 41 };
    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    server.encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server.client_slots = rocketnet_client_slots_create( NULL, 1 );
    server.acks = rocketnet_server_acks_create( NULL, 1 );
    server.counters = &counters;
    server.connection_ids = connection_ids;
    server.defer_payload_acks = 1;
    rocketnet_unit_check( server.encryption_map && server.client_slots && server.acks );

    struct rocketnet_address_t from;
    rocketnet_unit_check( rocketnet_address_parse( &from, "198.51.100.7:56789" ) );

    const int mapping_index = rocketnet_encryption_map_add( server.encryption_map, &from, 1005.0 );
    rocketnet_unit_check( mapping_index >= 0 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server.encryption_map, mapping_index );
    memcpy( mapping->keys.session_client_to_server, rocketnet_test_session_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( mapping->connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    const int client_index = rocketnet_client_slots_assign( server.client_slots, 0x1400, mapping_index, user_data );
    rocketnet_unit_check( client_index == 0 );

    mapping->control->client_index = client_index;
    mapping->control->established = 1;

    uint8_t expected[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    for ( int i = 0; i < ROCKETNET_RECEIVE_PAYLOAD_BYTES; i++ )
    {
        expected[i] = (uint8_t) ( i * 37 + 11 );
    }
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 777;
    const uint8_t * payload = response;
    int payload_bytes = 777;
    int payload_client_index = 777;

    int packet_bytes = seal_payload( packet, 1, expected, (int) sizeof( expected ) );
    rocketnet_unit_check( packet_bytes > 0 );

    const struct rocketnet_counters_t before_small = counters;
    rocketnet_unit_check( rocketnet_server_protocol_process_received( &server, &from, mapping_index,
                                                                      packet, packet_bytes,
                                                                      ROCKETNET_TEST_CREATE_TS, 100.0,
                                                                      response, &response_bytes,
                                                                      opened, (int) sizeof( expected ) - 1,
                                                                      &payload, &payload_bytes,
                                                                      &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( response_bytes == 0 );
    rocketnet_unit_check( payload == NULL );
    rocketnet_unit_check( payload_bytes == 0 );
    rocketnet_unit_check( payload_client_index == -1 );
    rocketnet_unit_check( server.payload_sequence == 0 );
    rocketnet_unit_check( server.payload_connection_id == 0 );
    rocketnet_unit_check( mapping->replay_session.highest_opened == 1 );
    rocketnet_unit_check( mapping->accepted_session.highest_opened == 0 );
    rocketnet_unit_check( server.acks->endpoints[client_index].ack_debt == 0 );

    check_counter_delta( &before_small, &counters,
                         ROCKETNET_COUNTER_PACKETS_RECEIVED,
                         ROCKETNET_COUNTER_PAYLOAD_RECEIVED,
                         ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL );

    packet_bytes = seal_payload( packet, 2, expected, (int) sizeof( expected ) );
    rocketnet_unit_check( packet_bytes > 0 );

    const struct rocketnet_counters_t before_fit = counters;
    rocketnet_unit_check( rocketnet_server_protocol_process_received( &server, &from, mapping_index,
                                                                      packet, packet_bytes,
                                                                      ROCKETNET_TEST_CREATE_TS, 100.1,
                                                                      response, &response_bytes,
                                                                      opened, (int) sizeof( expected ),
                                                                      &payload, &payload_bytes,
                                                                      &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_unit_check( payload >= opened && payload + payload_bytes <= opened + sizeof( opened ) );
    rocketnet_unit_check( payload_bytes == (int) sizeof( expected ) );
    rocketnet_unit_check( memcmp( payload, expected, sizeof( expected ) ) == 0 );
    rocketnet_unit_check( payload_client_index == client_index );
    rocketnet_unit_check( server.payload_sequence == 2 );
    rocketnet_unit_check( server.payload_connection_id == connection_ids[client_index] );
    rocketnet_unit_check( mapping->replay_session.highest_opened == 2 );
    rocketnet_unit_check( mapping->accepted_session.highest_opened == 0 );
    rocketnet_unit_check( server.acks->endpoints[client_index].ack_debt == 0 );

    check_counter_delta( &before_fit, &counters,
                         ROCKETNET_COUNTER_PACKETS_RECEIVED,
                         ROCKETNET_COUNTER_PAYLOAD_RECEIVED,
                         ROCKETNET_COUNTER_PAYLOAD_DELIVERED );

    rocketnet_server_protocol_accept_payload( &server, payload_client_index,
                                              server.payload_connection_id,
                                              server.payload_sequence );

    rocketnet_unit_check( mapping->accepted_session.highest_opened == 2 );
    rocketnet_unit_check( server.acks->endpoints[client_index].ack_debt == 1 );

    rocketnet_server_acks_destroy( server.acks );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_encryption_map_destroy( server.encryption_map );
}

// ---------------------------------------------------------------------------
// the send write pass honours the frame's reported payload capacity
// ---------------------------------------------------------------------------

#define PC_PAYLOAD_INDEX 32
#define PC_CAPACITY      64
#define PC_GUARD         0xA5
#define PC_FRAME_BYTES   ( PC_PAYLOAD_INDEX + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + PC_CAPACITY )
#define PC_MESSAGE_BYTES 1000

struct pc_endpoint_t
{
    uint8_t frame[PC_FRAME_BYTES];
    bool used;
    uint64_t next_id;
    int starts;
    int aborts;
    int finishes;
};

static int pc_max_payload_bytes( void * transport )
{
    (void) transport;
    return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

static uint8_t * pc_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    pc_endpoint_t * end = (pc_endpoint_t *) transport;
    (void) client_index;
    memset( context, 0, sizeof( *context ) );
    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES || end->used )
    {
        return NULL;
    }

    end->used = true;
    memset( end->frame, PC_GUARD, sizeof( end->frame ) );
    context->frame = end->frame;
    context->payload_index = PC_PAYLOAD_INDEX;
    context->payload_capacity = PC_CAPACITY;
    context->packet_id = ++end->next_id;
    end->starts++;

    return end->frame;
}

static int pc_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    pc_endpoint_t * end = (pc_endpoint_t *) transport;
    (void) context;
    if ( !end->used )
    {
        return 0;
    }

    end->used = false;
    end->finishes++;

    return payload_bytes > 0 ? payload_bytes + 64 : 0;
}

static void pc_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    pc_endpoint_t * end = (pc_endpoint_t *) transport;
    (void) context;
    if ( end->used )
    {
        end->used = false;
        end->aborts++;
    }
}

static void pc_send_packets( void * transport )
{
    (void) transport;
}

static uint64_t pc_connection( void * transport, int client_index )
{
    (void) transport;
    (void) client_index;
    return 1;
}

static int pc_state( void * transport )
{
    (void) transport;
    return ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
}

static void test_write_pass_honours_transport_payload_capacity()
{
    pc_endpoint_t endpoint = {};
    endpoint.next_id = 1;

    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = &endpoint;
    link.max_payload_bytes = pc_max_payload_bytes;
    link.start_packet = pc_start_packet;
    link.finish_packet = pc_finish_packet;
    link.abort_packet = pc_abort_packet;
    link.send_packets = pc_send_packets;
    link.connection = pc_connection;
    link.state = pc_state;

    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 4;
    config.packets_per_update = 8;

    struct rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1, &config, NULL, 0, 1, 1000.0 );
    rocketnet_unit_check( object != NULL );
    if ( !object )
    {
        return;
    }

    uint8_t * pointer = rn_message_object_start( object, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, PC_MESSAGE_BYTES );
    rocketnet_unit_check( pointer != NULL );
    if ( pointer )
    {
        memset( pointer, 0x5a, PC_MESSAGE_BYTES );
    }
    rocketnet_unit_check( rn_message_object_finish( object, 0, PC_MESSAGE_BYTES ) != 0 );

    const int starts_before = endpoint.starts;
    rn_message_object_send_packets( object );
    rocketnet_unit_check( endpoint.starts > starts_before );

    // the frame reports 64 bytes, less than the requested 1,415: the write
    // pass must not write at or past the reported capacity

    for ( int i = PC_CAPACITY; i < PC_FRAME_BYTES - PC_PAYLOAD_INDEX; i++ )
    {
        rocketnet_unit_check( endpoint.frame[PC_PAYLOAD_INDEX + i] == PC_GUARD );
    }

    rn_message_object_destroy( object );
}

// The held-back client payloads: the server's receive_batch is swapped for
// one that reads the loopback queue into this store and hands the server
// nothing, until release asks it to hand over that many at once.

#define HELD_EXTRA 3

struct held_payloads_t
{
    const struct rocketnet_server_transport_io_operations_t * loopback;
    uint8_t * data;
    int * bytes;
    struct rocketnet_address_t * from;
    int capacity;
    int count;
    int next;
    int release;
    int budget_seen;
    struct rocketnet_address_t scratch_from[256];
    int scratch_bytes[256];
    uint8_t scratch_data[256 * ROCKETNET_RECEIVE_CEILING];
};

static struct held_payloads_t * held;

static int held_receive_batch( void * instance, struct rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity )
{
    held->budget_seen = capacity;

    if ( held->release == 0 )
    {
        const int received = held->loopback->receive_batch( instance, held->scratch_from, held->scratch_data, ROCKETNET_RECEIVE_CEILING, held->scratch_bytes, 256 );
        for ( int i = 0; i < received; i++ )
        {
            const uint8_t * packet = held->scratch_data + (size_t) i * ROCKETNET_RECEIVE_CEILING;
            if ( packet[0] != ROCKETNET_CONNECTION_PAYLOAD_PACKET || held->count == held->capacity )
            {
                continue;
            }
            memcpy( held->data + (size_t) held->count * ROCKETNET_RECEIVE_CEILING, packet, (size_t) held->scratch_bytes[i] );
            held->bytes[held->count] = held->scratch_bytes[i];
            held->from[held->count] = held->scratch_from[i];
            held->count++;
        }
        return 0;
    }

    int handed = 0;
    while ( handed < held->release && handed < capacity && held->next < held->count )
    {
        memcpy( data + (size_t) handed * (size_t) stride, held->data + (size_t) held->next * ROCKETNET_RECEIVE_CEILING, (size_t) held->bytes[held->next] );
        bytes[handed] = held->bytes[held->next];
        from[handed] = held->from[held->next];
        handed++;
        held->next++;
    }
    held->release = 0;
    return handed;
}

static double payload_capacity_clock = 1000.0;
static double payload_capacity_time( void * ) { return payload_capacity_clock; }

static void payload_capacity_step( struct rocketnet_server_transport_t * server, struct rocketnet_client_transport_t * client )
{
    payload_capacity_clock += 0.001;
    rocketnet_server_transport_update( server, payload_capacity_clock );
    rocketnet_client_transport_update( client, payload_capacity_clock );
    rocketnet_server_transport_send_packets( server );
    rocketnet_client_transport_send_packets( client );
}

static void test_server_payload_batch_holds_the_receive_budget()
{
    rocketnet_quiet( 1 );
    payload_capacity_clock = 1000.0;

    const uint64_t protocol_id = 0x7061796c6f616463ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    rocketnet_unit_check( server != NULL );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                                    0x5eed5eed5eed5eedULL, 0, 1ULL << 40, 60, NULL,
                                                                    token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = payload_capacity_time;

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), payload_capacity_clock );
    rocketnet_unit_check( client != NULL );

    bool connected = false;
    for ( int i = 0; i < 400 && !connected; i++ )
    {
        payload_capacity_step( server, client );
        connected = rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                    rocketnet_server_transport_is_client_connected( server, 0 );
    }
    rocketnet_unit_check( connected );

    // hold the client's payloads back from the server

    const int budget = rocketnet_server_receive_packets_per_update( server_config.max_clients );

    held = (struct held_payloads_t *) calloc( 1, sizeof( *held ) );
    rocketnet_unit_check( held != NULL );
    held->capacity = budget + HELD_EXTRA;
    held->data = (uint8_t *) malloc( (size_t) held->capacity * ROCKETNET_RECEIVE_CEILING );
    held->bytes = (int *) calloc( (size_t) held->capacity, sizeof( int ) );
    held->from = (struct rocketnet_address_t *) calloc( (size_t) held->capacity, sizeof( struct rocketnet_address_t ) );
    rocketnet_unit_check( held->data && held->bytes && held->from );

    struct rocketnet_server_transport_io_operations_t operations = {};
    const struct rocketnet_server_transport_io_operations_t * swapped_operations = &operations;
    void * swapped_instance = NULL;
    rocketnet_server_transport_swap_io_for_test( server, &swapped_operations, &swapped_instance );
    held->loopback = swapped_operations;
    void * const loopback_instance = swapped_instance;
    operations = *swapped_operations;
    operations.receive_batch = held_receive_batch;
    swapped_operations = &operations;
    rocketnet_server_transport_swap_io_for_test( server, &swapped_operations, &swapped_instance );
    rocketnet_unit_check( swapped_instance == NULL );
    swapped_instance = loopback_instance;

    int sent = 0;
    for ( int round = 0; round < 2000 && held->count < held->capacity; round++ )
    {
        for ( int i = 0; i < 64; i++ )
        {
            struct rocketnet_packet_context_t context;
            uint8_t * frame = rocketnet_client_transport_start_packet( client, 4, &context );
            if ( !frame )
            {
                break;
            }
            uint8_t * payload = frame + context.payload_index;
            payload[0] = (uint8_t) ( sent & 0xFF );
            payload[1] = (uint8_t) ( ( sent >> 8 ) & 0xFF );
            payload[2] = (uint8_t) ( ( sent >> 16 ) & 0xFF );
            payload[3] = 0xA5;
            rocketnet_unit_check( rocketnet_client_transport_finish_packet( client, &context, 4 ) > 0 );
            sent++;
        }
        payload_capacity_step( server, client );
    }
    rocketnet_unit_check( held->count == held->capacity );
    rocketnet_unit_check( rocketnet_server_transport_num_payloads( server ) == 0 );

    // one update handed exactly the receive budget delivers every payload

    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    const uint64_t delivered_before = counters->values[ROCKETNET_COUNTER_PAYLOAD_DELIVERED];

    held->release = budget;
    payload_capacity_clock += 0.001;
    rocketnet_server_transport_update( server, payload_capacity_clock );

    rocketnet_unit_check( held->budget_seen == budget );
    rocketnet_unit_check( held->next == budget );
    rocketnet_unit_check( rocketnet_server_transport_num_payloads( server ) == budget );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] - delivered_before == (uint64_t) budget );
    int previous = -1;
    for ( int i = 0; i < budget; i++ )
    {
        int client_index = -1;
        int payload_bytes = 0;
        const uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &payload_bytes );
        rocketnet_unit_check( payload != NULL && client_index == 0 && payload_bytes == 4 && payload[3] == 0xA5 );
        const int index = payload[0] | ( payload[1] << 8 ) | ( payload[2] << 16 );
        rocketnet_unit_check( index > previous && index < sent );
        previous = index;
    }

    // the next update delivers the rest: nothing was spent and dropped

    held->release = HELD_EXTRA;
    payload_capacity_clock += 0.001;
    rocketnet_server_transport_update( server, payload_capacity_clock );
    rocketnet_unit_check( rocketnet_server_transport_num_payloads( server ) == HELD_EXTRA );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] - delivered_before == (uint64_t) ( budget + HELD_EXTRA ) );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED] == 0 );

    swapped_operations = held->loopback;
    rocketnet_server_transport_swap_io_for_test( server, &swapped_operations, &swapped_instance );
    rocketnet_unit_check( swapped_operations == &operations );

    rn_test_client_transport_stop_and_destroy( client );
    rn_test_server_transport_stop_and_destroy( server );

    free( held->from );
    free( held->bytes );
    free( held->data );
    free( held );
    held = NULL;
}

REGISTER_UNIT_TEST( test_server_payload_capacity_uses_live_session_path );
REGISTER_UNIT_TEST( test_write_pass_honours_transport_payload_capacity );
REGISTER_UNIT_TEST( test_server_payload_batch_holds_the_receive_budget );
