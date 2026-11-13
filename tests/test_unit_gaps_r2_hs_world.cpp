/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A client protocol and a server protocol in memory, as
// tests/test_unit_server_session_edges.cpp runs them: the handshake's
// response sequence high water, the acked-id tagging loop's bound, the
// client's keep-alive type bytes, the client's handshake rtt sample at its
// edges, the verdicts a client I/O thread hands the update with a record, and
// the client's payload writers outside the connected state. No sockets, no
// network, no threads; time is an argument. Two rtt cases run the client's
// clock through 0.0, because the edge under test is a send stamped at exactly
// 0.0; every other clock starts at 1000.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#define R2HS_SLOTS 4
#define R2HS_TIMEOUT 10

static void r2hs_world_fault_if( bool fault, const char * text )
{
    if ( fault )
    {
        printf( "fault: %s\n", text );
    }
    check( !fault );
}

struct r2hs_world_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    int end_reasons[R2HS_SLOTS];
    uint64_t connection_ids[R2HS_SLOTS];
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_address_t address;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes;
    int payload_bytes;
};

static void r2hs_world_mint( uint8_t * token_data, uint64_t client_id )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = R2HS_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = client_id;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static struct r2hs_world_t * r2hs_world_create()
{
    struct r2hs_world_t * world = (struct r2hs_world_t *) calloc( 1, sizeof( struct r2hs_world_t ) );
    check( world != NULL );

    struct rocketnet_server_protocol_t * server = &world->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, R2HS_SLOTS );
    server->acks = rocketnet_server_acks_create( NULL, R2HS_SLOTS );
    server->end_reasons = world->end_reasons;
    server->connection_ids = world->connection_ids;
    server->counters = &world->server_counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, 1000.0 );

    check( rocketnet_address_parse( &world->address, "198.51.100.7:56789" ) );
    rocketnet_client_protocol_init( &world->client, &world->client_counters );
    return world;
}

static void r2hs_world_destroy( struct r2hs_world_t * world )
{
    rocketnet_counters_assert_conserved( &world->server_counters );
    rocketnet_encryption_map_destroy( world->server.encryption_map );
    rocketnet_used_token_history_destroy( world->server.used_token_history );
    rocketnet_client_slots_destroy( world->server.client_slots );
    rocketnet_server_acks_destroy( world->server.acks );
    free( world );
}

// the server takes the packet at its own clock, never below 1000.0 (the two
// rtt cases run the client's clock below it): the server's cookie and
// mapping clocks are not under test here

static int r2hs_world_to_server( struct r2hs_world_t * world, const uint8_t * packet, int bytes, double server_time )
{
    world->response_bytes = 0;
    world->payload_bytes = 0;
    int payload_client_index = -1;
    return rocketnet_server_protocol_process_packet( &world->server, &world->address, packet, bytes, ROCKETNET_TEST_CREATE_TS,
                                                     server_time, world->response, &world->response_bytes, world->payload,
                                                     &world->payload_bytes, &payload_client_index );
}

static int r2hs_world_to_client( struct r2hs_world_t * world, const uint8_t * packet, int bytes, double client_time )
{
    int payload_bytes = 0;
    return rocketnet_client_protocol_process_packet( &world->client, packet, bytes, client_time, world->payload,
                                                     (int) sizeof( world->payload ), &payload_bytes );
}

// one client send at client_time, taken by the server and its answer
// delivered to the client at answer_time

static void r2hs_world_exchange( struct r2hs_world_t * world, double client_time, double answer_time )
{
    int bytes = 0;
    rocketnet_client_protocol_update( &world->client, client_time, world->packet, &bytes );
    check( bytes > 0 );
    (void) r2hs_world_to_server( world, world->packet, bytes, client_time >= 1000.0 ? client_time : 1000.0 );
    check( world->response_bytes > 0 );
    check( r2hs_world_to_client( world, world->response, world->response_bytes, answer_time ) );
}

static int r2hs_world_connect( struct r2hs_world_t * world, double t )
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2hs_world_mint( token_data, 0x5151 );
    check( rocketnet_client_protocol_connect( &world->client, token_data, t, ROCKETNET_TEST_CREATE_TS ) );
    for ( int i = 0; i < 3; i++ )
    {
        r2hs_world_exchange( world, t + 0.2 * i, t + 0.2 * i );
    }
    check( world->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    const int mapping_index = rocketnet_encryption_map_find( world->server.encryption_map, &world->address );
    check( mapping_index != -1 );
    const int client_index = rocketnet_encryption_map_entry( world->server.encryption_map, mapping_index )->control->client_index;
    check( client_index >= 0 && client_index < R2HS_SLOTS );
    check( world->client.client_index == client_index );
    return client_index;
}

static struct rocketnet_encryption_mapping_t * r2hs_world_mapping( struct r2hs_world_t * world )
{
    const int mapping_index = rocketnet_encryption_map_find( world->server.encryption_map, &world->address );
    check( mapping_index != -1 );
    return rocketnet_encryption_map_entry( world->server.encryption_map, mapping_index );
}

// mutation gap: hunt 2/1605
//
// An opened connection response raises the mapping's handshake high water, and
// a later response is reconstructed against it: one at full sequence 2^31 + 5,
// then one at 2^32 + 3 (wire 3), which reconstructs to 2^32 + 3 only from that
// high water and opens. From a high water left at 0 it would reconstruct to
// 3, a nonce it was not sealed under, and not open.

static int r2hs_world_send_response( struct r2hs_world_t * world, uint64_t sequence )
{
    struct rocketnet_encryption_mapping_t * mapping = r2hs_world_mapping( world );
    uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    const int bytes = rocketnet_write_sealed_packet( world->packet, ROCKETNET_CONNECTION_RESPONSE_PACKET, rocketnet_test_filter_value,
                                                     sequence, NULL, padding, (int) sizeof( padding ), ROCKETNET_TEST_PROTOCOL_ID,
                                                     mapping->connection_salt, mapping->client_ephemeral_public_key,
                                                     mapping->keys.handshake_client_to_server );
    check( bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    return r2hs_world_to_server( world, world->packet, bytes, 1000.5 );
}

static void test_gaps_r2_hs_response_raises_handshake_high_water()
{
    struct r2hs_world_t * world = r2hs_world_create();

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2hs_world_mint( token_data, 0x5151 );
    check( rocketnet_client_protocol_connect( &world->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
    r2hs_world_exchange( world, 1000.0, 1000.0 );
    r2hs_world_exchange( world, 1000.2, 1000.2 );
    check( world->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    struct rocketnet_encryption_mapping_t * mapping = r2hs_world_mapping( world );
    check( mapping->handshake_highest_opened == 0 );

    const uint64_t first = ( 1ULL << 31 ) + 5;
    check( r2hs_world_send_response( world, first ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    check( world->server_counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED] == 1 );
    if ( mapping->handshake_highest_opened != first )
    {
        printf( "fault: an opened connection response at sequence 2^31+5 left the handshake high water at %llu\n",
                (unsigned long long) mapping->handshake_highest_opened );
    }
    check( mapping->handshake_highest_opened == first );

    const uint64_t did_not_open = world->server_counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN];
    (void) r2hs_world_send_response( world, ( 1ULL << 32 ) + 3 );
    r2hs_world_fault_if( world->server_counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN] != did_not_open,
                         "a response at sequence 2^32+3 (wire 3) after one at 2^31+5 did not open: it was reconstructed against a stale high water" );
    check( mapping->handshake_highest_opened == ( 1ULL << 32 ) + 3 );
    check( mapping->integrity_failures == 0 );

    r2hs_world_destroy( world );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_response_raises_handshake_high_water );

// mutation gap: hunt 2/1628
//
// The ids a block acks are tagged with their client index and connection id
// as they are appended, and only those: every entry from the new count on,
// inside the same allocations and invisible to ASan, keeps the canary
// planted there before the packet.

static void test_gaps_r2_hs_acked_tagging_stops_at_the_count()
{
    struct r2hs_world_t * world = r2hs_world_create();
    struct rocketnet_server_acks_t * acks = world->server.acks;
    check( acks->acked_capacity >= 2 );
    uint64_t * acked_connection_ids = (uint64_t *) calloc( (size_t) acks->acked_capacity, sizeof( uint64_t ) );
    check( acked_connection_ids != NULL );
    world->server.acked_connection_ids = acked_connection_ids;

    const int client_index = r2hs_world_connect( world, 1000.0 );
    const uint64_t connection_id = world->connection_ids[client_index];
    check( connection_id != 0 );

    // the server sends one payload under packet id 0x77 and the client takes it

    struct rocketnet_address_t to;
    uint64_t sequence = 0;
    check( rocketnet_server_protocol_start_payload( &world->server, client_index, &to, &sequence ) == 1 );
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE] = {};
    frame[ROCKETNET_FRAME_PAYLOAD_OFFSET] = 0x5C;
    const int wire_bytes = rocketnet_server_protocol_seal_payload_frame( &world->server, client_index, sequence, 0x77,
                                                                         frame + ROCKETNET_FRAME_WIRE_OFFSET, 1, 1000.5 );
    check( wire_bytes > 0 );
    check( r2hs_world_to_client( world, frame + ROCKETNET_FRAME_WIRE_OFFSET, wire_bytes, 1000.5 ) );

    const int before = acks->num_acked;
    const int canary_index = 0x5A5A5A5A;
    const uint64_t canary_id = 0xC0FFEE00C0FFEE00ULL;
    for ( int i = before; i < acks->acked_capacity; i++ )
    {
        acks->acked_client_index[i] = canary_index;
        acked_connection_ids[i] = canary_id;
    }

    // the client's next keep-alive carries the block that acks it

    int bytes = 0;
    rocketnet_client_protocol_update( &world->client, 1001.0, world->packet, &bytes );
    check( bytes > 0 );
    check( r2hs_world_to_server( world, world->packet, bytes, 1001.0 ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );

    const int after = acks->num_acked;
    check( after > before && after < acks->acked_capacity );
    bool found = false;
    for ( int i = before; i < after; i++ )
    {
        check( acks->acked_client_index[i] == client_index );
        check( acked_connection_ids[i] == connection_id );
        found = found || acks->acked_packet_ids[i] == 0x77;
    }
    check( found );

    for ( int i = after; i < acks->acked_capacity; i++ )
    {
        if ( acks->acked_client_index[i] != canary_index || acked_connection_ids[i] != canary_id )
        {
            printf( "fault: the acked-id tagging wrote entry %d, past the %d ids appended\n", i, after );
        }
        check( acks->acked_client_index[i] == canary_index );
        check( acked_connection_ids[i] == canary_id );
    }

    world->server.acked_connection_ids = NULL;
    free( acked_connection_ids );
    r2hs_world_destroy( world );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_acked_tagging_stops_at_the_count );

// mutation gap: hunt 2/2129
// mutation gap: hunt 2/2130
//
// The client's keep-alive names its slot: opened with the server's key, the
// eight type bytes behind the block are the client index, then the slot count.

static void test_gaps_r2_hs_client_keep_alive_names_its_slot()
{
    struct r2hs_world_t * world = r2hs_world_create();
    const int client_index = r2hs_world_connect( world, 1000.0 );

    int bytes = 0;
    rocketnet_client_protocol_update( &world->client, 1001.0, world->packet, &bytes );
    check( bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    check( world->packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );

    const uint8_t * p = world->packet + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_read_uint32( &p );

    struct rocketnet_encryption_mapping_t * mapping = r2hs_world_mapping( world );
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), world->packet, bytes,
                                                           ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence, ROCKETNET_TEST_PROTOCOL_ID,
                                                           mapping->connection_salt, NULL, mapping->keys.session_client_to_server );
    check( opened_bytes >= ROCKETNET_ACK_BLOCK_MIN_BYTES + 8 );

    const uint8_t * type_data = opened + opened_bytes - 8;
    const uint32_t index_field = rocketnet_read_uint32( &type_data );
    const uint32_t slots_field = rocketnet_read_uint32( &type_data );
    if ( index_field != (uint32_t) client_index )
    {
        printf( "fault: the client's keep-alive names client index %u, not its slot %d\n", index_field, client_index );
    }
    check( index_field == (uint32_t) client_index );
    if ( slots_field != (uint32_t) R2HS_SLOTS )
    {
        printf( "fault: the client's keep-alive names %u slots, not the server's %d\n", slots_field, R2HS_SLOTS );
    }
    check( slots_field == (uint32_t) R2HS_SLOTS );

    r2hs_world_destroy( world );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_client_keep_alive_names_its_slot );

// mutation gap: hunt 2/1975
//
// The challenge round trip is sampled from the first request of sending
// connection request even when that request goes out at exactly 0.0, a
// clock a caller may start at: a stamp of 0.0 is armed. The cookie exchange
// runs before 0.0 so that the stamped request is the one at 0.0.

static void test_gaps_r2_hs_client_rtt_sampled_from_a_send_at_zero()
{
    struct r2hs_world_t * world = r2hs_world_create();

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2hs_world_mint( token_data, 0x5151 );
    check( rocketnet_client_protocol_connect( &world->client, token_data, -0.2, ROCKETNET_TEST_CREATE_TS ) );
    r2hs_world_exchange( world, -0.2, -0.15 );
    check( world->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    check( world->client.rtt_estimate == 0.0 );

    r2hs_world_exchange( world, 0.0, 0.03 );
    check( world->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    if ( fabs( world->client.rtt_estimate - 0.03 ) >= 1e-9 )
    {
        printf( "fault: the challenge answering a request sent at 0.0 gave rtt %.6f, not 0.03: a send stamped at 0.0 was treated as disarmed\n",
                world->client.rtt_estimate );
    }
    check( fabs( world->client.rtt_estimate - 0.03 ) < 1e-9 );

    r2hs_world_destroy( world );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_client_rtt_sampled_from_a_send_at_zero );

// mutation gap: hunt 2/1981
//
// A challenge processed at a time before the request it answers was sent
// (the caller's clock stepped back) is no sample: the estimate stays 0, never
// negative.

static void test_gaps_r2_hs_client_rtt_ignores_a_negative_sample()
{
    struct r2hs_world_t * world = r2hs_world_create();

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2hs_world_mint( token_data, 0x5151 );
    check( rocketnet_client_protocol_connect( &world->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
    r2hs_world_exchange( world, 1000.0, 1000.05 );
    r2hs_world_exchange( world, 1000.2, 1000.1 );
    check( world->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    if ( world->client.rtt_estimate != 0.0 )
    {
        printf( "fault: a challenge processed 0.1s before its request was sent set the rtt estimate to %.6f\n", world->client.rtt_estimate );
    }
    check( world->client.rtt_estimate == 0.0 );

    r2hs_world_destroy( world );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_client_rtt_ignores_a_negative_sample );

// the log lines of one stretch of a case, kept to find a refusal's line

static char r2hs_log[16384];
static size_t r2hs_log_bytes = 0;

static void r2hs_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    if ( r2hs_log_bytes + length + 2 < sizeof( r2hs_log ) )
    {
        memcpy( r2hs_log + r2hs_log_bytes, message, length );
        r2hs_log_bytes += length;
        r2hs_log[r2hs_log_bytes++] = '\n';
        r2hs_log[r2hs_log_bytes] = '\0';
    }
}

static void r2hs_log_clear()
{
    r2hs_log_bytes = 0;
    r2hs_log[0] = '\0';
}

static bool r2hs_log_has( const char * text )
{
    return strstr( r2hs_log, text ) != NULL;
}

struct r2hs_record_counts_t
{
    uint64_t accepted;
    uint64_t zero_sequence;
    uint64_t already_received;
    uint64_t did_not_open;
    uint64_t integrity_failures;
};

static void r2hs_record_counts( struct r2hs_world_t * world, struct r2hs_record_counts_t * counts )
{
    const uint64_t * values = world->client_counters.values;
    counts->accepted = values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED];
    counts->zero_sequence = values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE];
    counts->already_received = values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED];
    counts->did_not_open = values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN];
    counts->integrity_failures = world->client.integrity_failures;
}

// one record refused: process_record returns 0, the keep-alive is not
// accepted, its own counter moves by one and its own line is logged

static void r2hs_record_refused( struct r2hs_world_t * world, const uint8_t * packet, int bytes, uint8_t verdict, uint64_t sequence,
                                 const char * line, int counter, const char * what )
{
    struct r2hs_record_counts_t before;
    r2hs_record_counts( world, &before );
    const uint64_t counter_before = world->client_counters.values[counter];

    r2hs_log_clear();
    int payload_bytes = 0;
    const int result = rocketnet_client_protocol_process_record( &world->client, packet, bytes, verdict, sequence, 1002.0,
                                                                 world->payload, (int) sizeof( world->payload ), &payload_bytes );

    struct r2hs_record_counts_t after;
    r2hs_record_counts( world, &after );

    if ( result != 0 || after.accepted != before.accepted )
    {
        printf( "fault: %s: the keep-alive was accepted (process_record returned %d)\n", what, result );
    }
    check( result == 0 );
    check( after.accepted == before.accepted );
    if ( world->client_counters.values[counter] != counter_before + 1 )
    {
        printf( "fault: %s: its refusal counter moved by %llu, not 1\n", what,
                (unsigned long long) ( world->client_counters.values[counter] - counter_before ) );
    }
    check( world->client_counters.values[counter] == counter_before + 1 );
    if ( !r2hs_log_has( line ) )
    {
        printf( "fault: %s: the line \"%s\" was not logged\n", what, line );
    }
    check( r2hs_log_has( line ) );
    if ( after.integrity_failures != before.integrity_failures )
    {
        printf( "fault: %s: counted an integrity failure for authentic bytes\n", what );
    }
    check( after.integrity_failures == before.integrity_failures );
}

// mutation gap: hunt 2/2293
// mutation gap: hunt 2/2294
// mutation gap: hunt 2/2296
// mutation gap: hunt 2/2297
// mutation gap: hunt 2/2298
// mutation gap: hunt 2/2300
// mutation gap: hunt 2/2308
// mutation gap: hunt 2/2309
// mutation gap: hunt 2/2311
// mutation gap: hunt 2/2313
// mutation gap: hunt 2/2314
// mutation gap: hunt 2/2316
//
// The verdicts a client I/O thread hands the update with a record of a genuine
// keep-alive: ZERO_SEQUENCE and ALREADY_RECEIVED are refused as such; a
// record the I/O thread opened in place is refused, without an integrity
// failure, when its sequence disagrees with the update's own reconstruction
// and when the update's window already holds it. Each refusal is counted
// once, logged at SPAM, and accepts nothing.

static void test_gaps_r2_hs_client_record_verdicts_refused()
{
    struct r2hs_world_t * world = r2hs_world_create();
    const int client_index = r2hs_world_connect( world, 1000.0 );

    uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
    const int bytes = rocketnet_server_write_keep_alive( &world->server, client_index, 1001.0, keep_alive );
    check( bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    const uint8_t * p = keep_alive + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_read_uint32( &p );
    check( sequence != 0 );

    // the record an I/O thread makes when it opens the keep-alive: the
    // plaintext in place behind the wire header

    uint8_t opened_record[ROCKETNET_RECEIVE_CEILING];
    memcpy( opened_record, keep_alive, (size_t) bytes );
    uint8_t plaintext[ROCKETNET_RECEIVE_CEILING];
    const int plaintext_bytes = rocketnet_open_sealed_packet( plaintext, NULL, (int) sizeof( plaintext ), keep_alive, bytes,
                                                              ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                                              world->client.signed_fields.protocol_id, world->client.connection_salt, NULL,
                                                              world->client.keys.session_server_to_client );
    check( plaintext_bytes == bytes - ROCKETNET_PACKET_OVERHEAD_BYTES );
    memcpy( opened_record + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, plaintext, (size_t) plaintext_bytes );

    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( r2hs_log_capture, NULL );

    r2hs_record_refused( world, keep_alive, bytes, ROCKETNET_CLIENT_IO_VERDICT_ZERO_SEQUENCE, 0,
                         "client ignored session packet. zero sequence", ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE,
                         "a record with verdict ZERO_SEQUENCE" );

    r2hs_record_refused( world, keep_alive, bytes, ROCKETNET_CLIENT_IO_VERDICT_ALREADY_RECEIVED, 0,
                         "client ignored session packet. already received", ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED,
                         "a record with verdict ALREADY_RECEIVED" );

    r2hs_record_refused( world, opened_record, bytes, ROCKETNET_CLIENT_IO_VERDICT_OPENED, sequence + 1,
                         "client ignored session packet. sequence disagrees with the I/O thread",
                         ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN, "an opened record whose sequence disagrees with the update's" );

    // the raw keep-alive is accepted; its opened record afterwards is a replay

    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_record( &world->client, keep_alive, bytes, ROCKETNET_CLIENT_IO_VERDICT_RAW, 0, 1002.0,
                                                     world->payload, (int) sizeof( world->payload ), &payload_bytes ) == 1 );

    r2hs_record_refused( world, opened_record, bytes, ROCKETNET_CLIENT_IO_VERDICT_OPENED, sequence,
                         "client ignored session packet. already received", ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED,
                         "an opened record the update's window already holds" );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    r2hs_world_destroy( world );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_client_record_verdicts_refused );

// mutation gap: hunt 2/2460
// mutation gap: hunt 2/2461
// mutation gap: hunt 2/2469
// mutation gap: hunt 2/2491
//
// A client that is not connected starts no payload (no sequence taken, none
// handed out) and seals no frame; the DEV writer writes nothing.

static void test_gaps_r2_hs_client_payload_needs_connected()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    struct rocketnet_client_protocol_t * client = (struct rocketnet_client_protocol_t *) calloc( 1, sizeof( struct rocketnet_client_protocol_t ) );
    check( client != NULL );
    rocketnet_client_protocol_init( client, &counters );
    check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    const uint64_t next_before = client->send_sequence_session;

    uint64_t sequence = 0xABCDu;
    const int started = rocketnet_client_protocol_start_payload( client, &sequence );
    r2hs_world_fault_if( started != 0, "start_payload started a payload on a client that is not connected" );
    r2hs_world_fault_if( sequence != 0xABCDu, "start_payload handed out a sequence on a client that is not connected" );
    r2hs_world_fault_if( client->send_sequence_session != next_before, "start_payload took a session sequence on a client that is not connected" );

    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE] = {};
    const int sealed = rocketnet_client_protocol_seal_payload_frame( client, 1, 1, frame + ROCKETNET_FRAME_WIRE_OFFSET, 4, 1000.0 );
    r2hs_world_fault_if( sealed != 0, "seal_payload_frame sealed a frame on a client that is not connected" );

#if ROCKETNET_DEV
    const uint8_t data[4] = { 1, 2, 3, 4 };
    uint8_t packet[ROCKETNET_SEND_BUDGET];
    const int written = rocketnet_client_protocol_write_payload( client, data, (int) sizeof( data ), 1000.0, packet );
    r2hs_world_fault_if( written != 0, "the DEV payload writer wrote a payload on a client that is not connected" );
    r2hs_world_fault_if( client->send_sequence_session != next_before, "the DEV payload writer took a session sequence on a client that is not connected" );
#endif // #if ROCKETNET_DEV

    free( client );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_client_payload_needs_connected );

// mutation gap: hunt 2/2472
// mutation gap: hunt 2/2497
//
// A connected client's frame seal refuses a payload one byte past
// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES and INT_MAX bytes, and its DEV writer
// refuses 0 bytes: 0 bytes written, no sequence taken, the ack debt still
// owed, the send clock not stamped. A 4-byte payload seals.

static void test_gaps_r2_hs_client_payload_size_refusals()
{
    struct r2hs_world_t * world = r2hs_world_create();
    (void) r2hs_world_connect( world, 1000.0 );
    struct rocketnet_client_protocol_t * client = &world->client;

    uint64_t sequence = 0;
    check( rocketnet_client_protocol_start_payload( client, &sequence ) == 1 );
    client->ack.ack_debt = 2;
    const double last_send = client->last_send_time;
    const uint64_t next_before = client->send_sequence_session;

    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;

    int bytes = rocketnet_client_protocol_seal_payload_frame( client, sequence, 9, wire, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1, 1001.0 );
    r2hs_world_fault_if( bytes != 0, "the client sealed a payload one byte past ROCKETNET_MAX_PACKET_PAYLOAD_BYTES" );
    bytes = rocketnet_client_protocol_seal_payload_frame( client, sequence, 9, wire, INT_MAX, 1001.0 );
    r2hs_world_fault_if( bytes != 0, "the client sealed a payload of INT_MAX bytes" );

#if ROCKETNET_DEV
    const uint8_t data[4] = { 1, 2, 3, 4 };
    uint8_t packet[ROCKETNET_SEND_BUDGET];
    bytes = rocketnet_client_protocol_write_payload( client, data, 0, 1001.0, packet );
    r2hs_world_fault_if( bytes != 0, "the client's DEV writer wrote a 0-byte payload" );
#endif // #if ROCKETNET_DEV

    r2hs_world_fault_if( client->send_sequence_session != next_before, "a refused client payload took a session sequence" );
    r2hs_world_fault_if( client->ack.ack_debt != 2, "a refused client payload cleared the ack debt" );
    r2hs_world_fault_if( client->last_send_time != last_send, "a refused client payload stamped the send clock" );

    bytes = rocketnet_client_protocol_seal_payload_frame( client, sequence, 9, wire, 4, 1001.0 );
    check( bytes > 0 );
    check( client->ack.ack_debt == 0 );

    r2hs_world_destroy( world );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_client_payload_size_refusals );
