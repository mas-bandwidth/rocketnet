/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Receive-bound audit witnesses. All traffic stays in memory. The ordinary
// protocol entry points read the existing frozen token and writer output;
// the only variation is the supplied byte count. Time is an argument.

#include "test_unit_registry.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_counters.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_ack.h"
#include "rocketnet_coalesce.h"
#include "rocketnet_message.h"
#include "rocketnet_uring_codec.h"
#include "rocketnet_client_io.h"
#include "rocketnet_client_gso.h"
#include "rocketnet_xdp_frame.h"

#if defined( ROCKETNET_XDP_FILTER_HOST ) && ROCKETNET_XDP_FILTER_HOST
#include "xdp_host/server_xdp_host.h"
#endif

#include <string.h>

#define check rocketnet_unit_check

struct audit_pair_t
{
    rocketnet_server_protocol_t server;
    rocketnet_client_protocol_t client;
    rocketnet_counters_t server_counters;
    rocketnet_counters_t client_counters;
    rocketnet_address_t from;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int response_bytes;
    int payload_bytes;
    int client_index;
};

static void audit_pair_init( audit_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    rocketnet_server_protocol_t * server = &pair->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    check( rocketnet_address_parse( &pair->from, "198.51.100.7:56789" ) );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->counters = &pair->server_counters;
    check( server->encryption_map && server->used_token_history && server->client_slots );
    rocketnet_server_protocol_update( server, 1000.0 );
    rocketnet_client_protocol_init( &pair->client, &pair->client_counters );
    check( rocketnet_client_protocol_connect( &pair->client, rocketnet_test_frozen_token,
                                             1000.0, ROCKETNET_TEST_CREATE_TS ) );
}

static void audit_pair_destroy( audit_pair_t * pair )
{
    rocketnet_encryption_map_destroy( pair->server.encryption_map );
    rocketnet_used_token_history_destroy( pair->server.used_token_history );
    rocketnet_client_slots_destroy( pair->server.client_slots );
}

static int audit_server_read( audit_pair_t * pair, const uint8_t * data, int bytes )
{
    pair->response_bytes = -1;
    pair->payload_bytes = -1;
    pair->client_index = -2;
    return rocketnet_server_protocol_process_packet( &pair->server, &pair->from, data, bytes,
                                                     ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                     pair->response, &pair->response_bytes,
                                                     pair->payload, &pair->payload_bytes, &pair->client_index );
}

static int audit_client_read( audit_pair_t * pair, const uint8_t * data, int bytes )
{
    pair->payload_bytes = -1;
    return rocketnet_client_protocol_process_packet( &pair->client, data, bytes, 1000.0,
                                                     pair->payload, (int) sizeof( pair->payload ), &pair->payload_bytes );
}

static void audit_request( audit_pair_t * pair, uint8_t * data )
{
    rocketnet_write_connection_request_packet( data, pair->client.packet_filter,
                                               pair->client.has_cookie ? pair->client.return_cookie : NULL,
                                               pair->client.connection_salt, pair->client.client_ephemeral_public_key,
                                               pair->client.token.signed_connect_data, pair->client.token.signature );
}

static void audit_pair_challenge( audit_pair_t * pair )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    audit_request( pair, request );
    check( audit_server_read( pair, request, (int) sizeof( request ) ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( pair->response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    check( audit_client_read( pair, pair->response, pair->response_bytes ) == 1 );
    audit_request( pair, request );
    check( audit_server_read( pair, request, (int) sizeof( request ) ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( pair->response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
}

static void audit_pair_connect( audit_pair_t * pair )
{
    audit_pair_challenge( pair );
    check( audit_client_read( pair, pair->response, pair->response_bytes ) == 1 );
    check( audit_server_read( pair, pair->client.stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    check( audit_client_read( pair, pair->response, pair->response_bytes ) == 1 );
    check( pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
}

static void test_audit_prefilter_bounds()
{
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    uint8_t * p = data;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, rocketnet_test_filter_value );
    const int short_lengths[] = { -1, 0, ROCKETNET_PREFILTER_BYTES - 1 };
    for ( int length : short_lengths )
        check( rocketnet_prefilter_check( data, length, rocketnet_test_filter_value ) == ROCKETNET_PREFILTER_TOO_SHORT );
    check( rocketnet_prefilter_check( data, ROCKETNET_PREFILTER_BYTES, rocketnet_test_filter_value ) == ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    check( rocketnet_prefilter_check( data, ROCKETNET_RECEIVE_CEILING, rocketnet_test_filter_value ) == ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    check( rocketnet_prefilter_check( data, ROCKETNET_RECEIVE_CEILING + 1, rocketnet_test_filter_value ) == ROCKETNET_PREFILTER_TOO_LARGE );
}

static void test_audit_server_request_bounds()
{
    audit_pair_t pair;
    audit_pair_init( &pair );
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    audit_request( &pair, data );
    const int lengths[] = { -1, 0, ROCKETNET_PREFILTER_BYTES - 1, ROCKETNET_PREFILTER_BYTES,
                           ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1,
                           ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + 1,
                           ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int length : lengths )
    {
        check( audit_server_read( &pair, data, length ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( pair.response_bytes == 0 && pair.payload_bytes == 0 && pair.client_index == -1 );
    }
    check( pair.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] == 4 );
    check( audit_server_read( &pair, data, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( pair.response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    check( pair.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] == 1 );
    check( pair.server_counters.handshakes_processed_this_update == 0 );
    audit_pair_destroy( &pair );
}

static void test_audit_client_cookie_bounds()
{
    audit_pair_t pair;
    audit_pair_init( &pair );
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    rocketnet_write_connection_cookie_packet( data, pair.client.packet_filter, pair.client.connection_salt,
                                              ROCKETNET_TEST_EXPIRE_TS, rocketnet_test_cookie_mac );
    const int lengths[] = { 0, ROCKETNET_PREFILTER_BYTES - 1, ROCKETNET_PREFILTER_BYTES,
                           ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES - 1, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES + 1,
                           ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int length : lengths )
    {
        check( audit_client_read( &pair, data, length ) == 0 );
        check( pair.payload_bytes == 0 && pair.client.has_cookie == 0 );
        check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
    }
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_SIZE] == 4 );
    check( audit_client_read( &pair, data, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES ) == 1 );
    check( pair.client.has_cookie == 1 );
    audit_pair_destroy( &pair );
}

static void test_audit_client_challenge_bounds()
{
    audit_pair_t pair;
    audit_pair_init( &pair );
    audit_pair_challenge( &pair );
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    memcpy( data, pair.response, (size_t) pair.response_bytes );
    const int lengths[] = { 0, ROCKETNET_PREFILTER_BYTES, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES - 1,
                           ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES + 1, ROCKETNET_RECEIVE_CEILING,
                           ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int length : lengths )
    {
        check( audit_client_read( &pair, data, length ) == 0 );
        check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
        check( pair.client.handshake_highest_opened == 0 );
    }
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_SIZE] == 4 );
    check( audit_client_read( &pair, data, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES ) == 1 );
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    audit_pair_destroy( &pair );
}

static void test_audit_client_denied_bounds()
{
    audit_pair_t pair;
    audit_pair_init( &pair );
    audit_pair_challenge( &pair );
    check( audit_client_read( &pair, pair.response, pair.response_bytes ) == 1 );
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    check( rocketnet_write_sealed_packet( data, ROCKETNET_CONNECTION_DENIED_PACKET, pair.client.packet_filter,
                                         2, pair.client.session_nonce, NULL, 0, ROCKETNET_TEST_PROTOCOL_ID,
                                         pair.client.connection_salt, NULL, pair.client.keys.handshake_server_to_client ) == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );
    const int lengths[] = { 0, ROCKETNET_PREFILTER_BYTES, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES - 1,
                           ROCKETNET_CONNECTION_DENIED_WIRE_BYTES + 1, ROCKETNET_RECEIVE_CEILING,
                           ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int length : lengths )
    {
        check( audit_client_read( &pair, data, length ) == 0 );
        check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    }
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_SIZE] == 4 );
    check( audit_client_read( &pair, data, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES ) == 1 );
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );
    audit_pair_destroy( &pair );
}

static void test_audit_server_response_bounds()
{
    audit_pair_t pair;
    audit_pair_init( &pair );
    audit_pair_challenge( &pair );
    check( audit_client_read( &pair, pair.response, pair.response_bytes ) == 1 );
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    memcpy( data, pair.client.stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    const int lengths[] = { 0, ROCKETNET_PREFILTER_BYTES, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES - 1,
                           ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES + 1, ROCKETNET_RECEIVE_CEILING,
                           ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int length : lengths )
    {
        check( audit_server_read( &pair, data, length ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( pair.response_bytes == 0 && pair.payload_bytes == 0 && pair.client_index == -1 );
    }
    check( pair.server_counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE] == 4 );
    check( audit_server_read( &pair, data, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    audit_pair_destroy( &pair );
}

static void audit_session_bounds( uint8_t type, bool to_client, int wrong_size_counter, int unopened_counter )
{
    audit_pair_t pair;
    audit_pair_init( &pair );
    audit_pair_connect( &pair );
    rocketnet_counters_t * counters = to_client ? &pair.client_counters : &pair.server_counters;
    const uint64_t wrong_size_before = counters->values[wrong_size_counter];
    const uint64_t unopened_before = counters->values[unopened_counter];
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    uint8_t * p = data;
    rocketnet_write_prefilter_header( &p, type, rocketnet_test_filter_value );
    rocketnet_write_uint32( &p, 100 );
    const int floor = rocketnet_session_min_wire_bytes( type );
    const int lengths[] = { 0, ROCKETNET_PREFILTER_BYTES - 1, ROCKETNET_PREFILTER_BYTES, floor - 1,
                           floor, ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int length : lengths )
    {
        const int result = to_client ? audit_client_read( &pair, data, length ) : audit_server_read( &pair, data, length );
        check( result == 0 );
        check( pair.payload_bytes == 0 );
        check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    }
    // The floor and ceiling reach the authentication check. The short and
    // one-past inputs are rejected before it, and nothing becomes payload.
    check( counters->values[wrong_size_counter] - wrong_size_before == 2 );
    check( counters->values[unopened_counter] - unopened_before == 2 );
    check( counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == 1 );
    audit_pair_destroy( &pair );
}

#define AUDIT_SESSION_CASE( name, type, client, wrong, unopened ) \
    static void name() { audit_session_bounds( type, client, wrong, unopened ); }

AUDIT_SESSION_CASE( test_audit_server_keep_alive_bounds, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, false,
                   ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE, ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN );
AUDIT_SESSION_CASE( test_audit_server_payload_bounds, ROCKETNET_CONNECTION_PAYLOAD_PACKET, false,
                   ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE, ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN );
AUDIT_SESSION_CASE( test_audit_server_disconnect_bounds, ROCKETNET_CONNECTION_DISCONNECT_PACKET, false,
                   ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE, ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN );
AUDIT_SESSION_CASE( test_audit_server_ack_bounds, ROCKETNET_CONNECTION_ACK_PACKET, false,
                   ROCKETNET_COUNTER_ACK_WRONG_SIZE, ROCKETNET_COUNTER_ACK_DID_NOT_OPEN );
AUDIT_SESSION_CASE( test_audit_client_keep_alive_bounds, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, true,
                   ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN );
AUDIT_SESSION_CASE( test_audit_client_payload_bounds, ROCKETNET_CONNECTION_PAYLOAD_PACKET, true,
                   ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN );
AUDIT_SESSION_CASE( test_audit_client_disconnect_bounds, ROCKETNET_CONNECTION_DISCONNECT_PACKET, true,
                   ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_DISCONNECT_DID_NOT_OPEN );
AUDIT_SESSION_CASE( test_audit_client_ack_bounds, ROCKETNET_CONNECTION_ACK_PACKET, true,
                   ROCKETNET_COUNTER_CLIENT_ACK_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_ACK_DID_NOT_OPEN );

static void test_audit_rejected_packet_types()
{
    audit_pair_t pair;
    audit_pair_init( &pair );
    uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
    const int lengths[] = { ROCKETNET_PREFILTER_BYTES, ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int type = ROCKETNET_FIRST_RESERVED_PACKET_TYPE; type <= 255; ++type )
    {
        uint8_t * p = data;
        rocketnet_write_prefilter_header( &p, (uint8_t) type, rocketnet_test_filter_value );
        for ( int length : lengths )
        {
            check( audit_client_read( &pair, data, length ) == 0 );
            check( audit_server_read( &pair, data, length ) == ROCKETNET_SERVER_PROCESS_IGNORED );
            check( pair.response_bytes == 0 && pair.payload_bytes == 0 );
        }
    }
    const uint64_t reserved = 2 * ( 256 - ROCKETNET_FIRST_RESERVED_PACKET_TYPE );
    check( pair.client_counters.values[ROCKETNET_COUNTER_RESERVED_TYPE] == reserved );
    check( pair.server_counters.values[ROCKETNET_COUNTER_RESERVED_TYPE] == reserved );
    const uint8_t server_only[] = { ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_CHALLENGE_PACKET, ROCKETNET_CONNECTION_COOKIE_PACKET };
    for ( uint8_t type : server_only )
    {
        data[0] = type;
        check( audit_server_read( &pair, data, ROCKETNET_RECEIVE_CEILING ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    }
    const uint8_t client_only[] = { ROCKETNET_CONNECTION_REQUEST_PACKET, ROCKETNET_CONNECTION_RESPONSE_PACKET };
    for ( uint8_t type : client_only )
    {
        data[0] = type;
        check( audit_client_read( &pair, data, ROCKETNET_RECEIVE_CEILING ) == 0 );
    }
    check( pair.server_counters.values[ROCKETNET_COUNTER_WRONG_DIRECTION] == 3 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_WRONG_DIRECTION] == 2 );
    audit_pair_destroy( &pair );
}

static void test_audit_client_io_bounds()
{
    rocketnet_client_io_t io = {};
    io.open.loaded_epoch = ROCKETNET_CLIENT_IO_EPOCH_LIVE;
    memcpy( io.open.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    check( rocketnet_address_parse( &io.open.server_address, "203.0.113.10:40000" ) );
    const uint8_t types[] = { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                             ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_ACK_PACKET };
    for ( uint8_t type : types )
    {
        uint8_t data[ROCKETNET_RECEIVE_CEILING + 1] = {};
        uint8_t * p = data;
        rocketnet_write_prefilter_header( &p, type, rocketnet_test_filter_value );
        rocketnet_write_uint32( &p, 1 );
        const int floor = rocketnet_session_min_wire_bytes( type );
        const int lengths[] = { 0, ROCKETNET_PREFILTER_BYTES - 1, floor - 1, floor,
                               ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1 };
        for ( int length : lengths )
        {
            uint8_t verdict = 255;
            uint64_t sequence = UINT64_MAX;
            rocketnet_client_io_decrypt_datagram( &io, &io.open.server_address, data, length, &verdict, &sequence );
            const uint8_t expected = length < ROCKETNET_PREFILTER_BYTES || length > ROCKETNET_RECEIVE_CEILING
                                         ? ROCKETNET_CLIENT_IO_VERDICT_RAW
                                         : length < floor ? ROCKETNET_CLIENT_IO_VERDICT_WRONG_SIZE
                                                          : ROCKETNET_CLIENT_IO_VERDICT_DID_NOT_OPEN;
            check( verdict == expected && sequence == 0 );
            check( io.open.replay.highest_opened == 0 );
        }
    }
}

static void test_audit_ack_block_bounds()
{
    uint8_t data[ROCKETNET_ACK_BLOCK_MAX_BYTES] = {};
    rocketnet_ack_block_t block;
    check( rocketnet_ack_block_read( &block, data, 0, 0 ) == 0 );
    check( rocketnet_ack_block_read( &block, data, ROCKETNET_ACK_BLOCK_MIN_BYTES - 1, 0 ) == 0 );
    check( rocketnet_ack_block_read( &block, data, ROCKETNET_ACK_BLOCK_MIN_BYTES, 0 ) == ROCKETNET_ACK_BLOCK_MIN_BYTES );
    uint8_t * p = data;
    rocketnet_write_uint32( &p, 1 + 2 * ROCKETNET_ACK_BLOCK_MAX_RANGES );
    rocketnet_write_uint8( &p, ROCKETNET_ACK_BLOCK_MAX_RANGES );
    rocketnet_write_uint16( &p, 1 );
    for ( int i = 0; i < ROCKETNET_ACK_BLOCK_MAX_RANGES; ++i )
    {
        rocketnet_write_uint16( &p, 1 );
        rocketnet_write_uint16( &p, 1 );
    }
    const uint64_t highest = 1 + 2 * ROCKETNET_ACK_BLOCK_MAX_RANGES;
    // The declared last byte is one past the shortened available buffer.
    check( rocketnet_ack_block_read( &block, data, ROCKETNET_ACK_BLOCK_MAX_BYTES - 1, highest ) == 0 );
    check( rocketnet_ack_block_read( &block, data, ROCKETNET_ACK_BLOCK_MAX_BYTES, highest ) == ROCKETNET_ACK_BLOCK_MAX_BYTES );
    check( block.num_ranges == ROCKETNET_ACK_BLOCK_MAX_RANGES );
}

static void test_audit_config_body_bounds()
{
    uint8_t data[ROCKETNET_MESSAGE_CONFIG_BYTES + 1] = {};
    rocketnet_message_config_body_t body = {};
    body.version = ROCKETNET_MESSAGE_CONFIG_VERSION;
    rocketnet_message_write_config_body( data, &body );
    const int bad_lengths[] = { 0, 1, ROCKETNET_MESSAGE_CONFIG_BYTES - 1, ROCKETNET_MESSAGE_CONFIG_BYTES + 1 };
    for ( int length : bad_lengths )
        check( rocketnet_message_read_config_body( data, length, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL );
    check( rocketnet_message_read_config_body( data, ROCKETNET_MESSAGE_CONFIG_BYTES, &body ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
}

static void test_audit_message_record_bounds()
{
    uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES] = {};
    rocketnet_message_limits_t limits = { 4, 0x1F, 1024, ROCKETNET_MAX_MESSAGE_BYTES, 65536 };
    rocketnet_message_reader_t reader;
    rocketnet_message_record_t record;
    check( rocketnet_message_reader_begin( &reader, data, 0, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_FORM );
    data[1] = ROCKETNET_MESSAGE_FORM_RECORDS;
    check( rocketnet_message_reader_begin( &reader, data, 2, &limits ) == ROCKETNET_MESSAGE_VIOLATION_TRUNCATED );
    check( rocketnet_message_reader_next( &reader, &record ) == 0 );
    const int lengths[] = { 1, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - ROCKETNET_MESSAGE_RECORD_FIXED_BYTES - 1 };
    for ( int length : lengths )
    {
        memset( data, 0, sizeof( data ) );
        uint8_t * p = data + 1;
        rocketnet_write_uint16( &p, (uint16_t) length );
        const int bytes = ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + length + 1;
        data[bytes - 1] = ROCKETNET_MESSAGE_FORM_RECORDS;
        check( rocketnet_message_reader_begin( &reader, data, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( rocketnet_message_reader_next( &reader, &record ) == 1 && record.bytes == length );
        check( rocketnet_message_reader_next( &reader, &record ) == 0 );
        p = data + 1;
        rocketnet_write_uint16( &p, (uint16_t) ( length + 1 ) );
        check( rocketnet_message_reader_begin( &reader, data, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH );
        check( rocketnet_message_reader_next( &reader, &record ) == 0 );
    }
}

// F2's local token invariant. This changes only an unused reserved byte in
// the existing frozen token and never writes or sends a connection packet.
static void test_audit_token_zero_padding_refuses()
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memcpy( token, rocketnet_test_frozen_token, sizeof( token ) );
    token[704] = 1;
    rocketnet_counters_t counters = {};
    rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );
    const int accepted = rocketnet_client_protocol_connect( &client, token, 1000.0, ROCKETNET_TEST_CREATE_TS );
    rocketnet_client_protocol_zero_state( &client );
    check( accepted == 0 );
}
REGISTER_UNIT_TEST_EXPECTED_FAIL( test_audit_token_zero_padding_refuses, "rna-l1-surface-map F2" );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
// F1's benign completion witness: no network bytes, only an OS truncation
// flag on a full receive buffer. The invariant intentionally fails until the
// receive path refuses the incomplete message before staging any record.
static int audit_gso_truncated_receive( void * context, int, struct mmsghdr * messages, unsigned int )
{
    int * calls = (int *) context;
    if ( ( *calls )++ != 0 )
        return 0;
    memset( messages[0].msg_hdr.msg_iov[0].iov_base, 0, messages[0].msg_hdr.msg_iov[0].iov_len );
    messages[0].msg_len = (unsigned int) messages[0].msg_hdr.msg_iov[0].iov_len;
    messages[0].msg_hdr.msg_flags = MSG_TRUNC;
    messages[0].msg_hdr.msg_controllen = 0;
    return 1;
}

static bool audit_gso_source( void *, const struct sockaddr_storage *, rocketnet_address_t * from )
{
    memset( from, 0, sizeof( *from ) );
    from->type = ROCKETNET_ADDRESS_IPV4;
    from->data.ipv4[0] = 192;
    from->data.ipv4[1] = 0;
    from->data.ipv4[2] = 2;
    from->data.ipv4[3] = 1;
    from->port = 40000;
    return true;
}

static void test_audit_client_gso_truncation_refuses()
{
    rocketnet_client_gso_t gso = {};
    int calls = 0;
    const rocketnet_client_gso_syscalls_t syscalls = { NULL, NULL, audit_gso_truncated_receive, NULL, audit_gso_source };
    gso.syscalls = &syscalls;
    gso.syscall_context = &calls;
    rocketnet_address_t from;
    uint8_t data[ROCKETNET_RECEIVE_CEILING];
    int bytes = -1;
    const int received = rocketnet_client_gso_receive_batch( &gso, &from, data, (int) sizeof( data ), &bytes, 1 );
    check( received == 0 );
    check( gso.carry_segments == 0 );
}
REGISTER_UNIT_TEST_EXPECTED_FAIL( test_audit_client_gso_truncation_refuses, "rna-l1-surface-map F1" );
#endif

static void test_audit_coalesced_receive_bounds()
{
    int offsets[2] = { -1, -1 };
    int lengths[2] = { -1, -1 };
    check( rocketnet_coalesce_split( 0, ROCKETNET_RECEIVE_CEILING, offsets, lengths, 2 ) == 0 );
    check( rocketnet_coalesce_split( 1, ROCKETNET_RECEIVE_CEILING, offsets, lengths, 2 ) == 1 );
    check( lengths[0] == 1 );
    check( rocketnet_coalesce_split( 2 * ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING, offsets, lengths, 2 ) == 2 );
    check( offsets[1] == ROCKETNET_RECEIVE_CEILING && lengths[1] == ROCKETNET_RECEIVE_CEILING );
    offsets[0] = offsets[1] = lengths[0] = lengths[1] = -1;
    check( rocketnet_coalesce_split( 2 * ROCKETNET_RECEIVE_CEILING + 1, ROCKETNET_RECEIVE_CEILING, offsets, lengths, 2 ) == -1 );
    check( offsets[0] == -1 && offsets[1] == -1 && lengths[0] == -1 && lengths[1] == -1 );
}

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT )
static void test_audit_uring_receive_bounds()
{
    const int prefix = (int) sizeof( io_uring_recvmsg_out ) + (int) sizeof( sockaddr_storage );
    uint8_t data[sizeof( io_uring_recvmsg_out ) + sizeof( sockaddr_storage ) + ROCKETNET_RECEIVE_CEILING] = {};
    io_uring_recvmsg_out header = {};
    header.namelen = sizeof( sockaddr_in );
    const uint8_t * name = NULL;
    const uint8_t * payload = NULL;
    int name_bytes = -1;
    int payload_bytes = -1;
    const int payload_lengths[] = { 0, 1, ROCKETNET_RECEIVE_CEILING };
    for ( int length : payload_lengths )
    {
        header.payloadlen = (uint32_t) length;
        memcpy( data, &header, sizeof( header ) );
        check( rocketnet_uring_recvmsg_parse( data, prefix + length, (int) sizeof( sockaddr_storage ), 0,
                                             &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_OK );
        check( payload_bytes == length && payload == data + prefix );
        check( rocketnet_uring_recvmsg_parse( data, prefix + length - 1, (int) sizeof( sockaddr_storage ), 0,
                                             &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) != ROCKETNET_URING_RECV_OK );
    }
    header.payloadlen = ROCKETNET_RECEIVE_CEILING + 1;
    memcpy( data, &header, sizeof( header ) );
    check( rocketnet_uring_recvmsg_parse( data, (int) sizeof( data ), (int) sizeof( sockaddr_storage ), 0,
                                         &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED );
    header.payloadlen = 0;
    header.namelen = sizeof( sockaddr_storage ) + 1;
    memcpy( data, &header, sizeof( header ) );
    check( rocketnet_uring_recvmsg_parse( data, (int) sizeof( data ), (int) sizeof( sockaddr_storage ), 0,
                                         &name, &name_bytes, &payload, &payload_bytes, NULL, NULL ) == ROCKETNET_URING_RECV_NAME_TRUNCATED );
}
REGISTER_UNIT_TEST( test_audit_uring_receive_bounds );
#endif

#if defined( ROCKETNET_XDP_FILTER_HOST ) && ROCKETNET_XDP_FILTER_HOST
static void audit_xdp_bounds( bool ipv6 )
{
    rocketnet_address_t source;
    rocketnet_address_t destination;
    check( rocketnet_address_parse( &source, ipv6 ? "[2001:db8::7]:50000" : "198.51.100.7:50000" ) );
    check( rocketnet_address_parse( &destination, ipv6 ? "[2001:db8::10]:40000" : "203.0.113.10:40000" ) );
    rocketnet_server_xdp_config_t config = {};
    const uint8_t port[2] = { 0x9c, 0x40 };
    memcpy( &config.public_port, port, sizeof( port ) );
    memcpy( config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    rocketnet_xdp_host_reset();
    rocketnet_xdp_host_set_config( &config );
    rocketnet_xdp_host_set_socket_slot( 0, 1 );
    const uint8_t mac[6] = { 2, 0, 0, 0, 0, 1 };
    const int header_bytes = rocketnet_xdp_frame_header_bytes( source.type );
    const int floor = ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;
    const int lengths[] = { 0, floor - 1, floor, ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int length : lengths )
    {
        uint8_t frame[ROCKETNET_XDP_FRAME_SIZE] = {};
        uint8_t * p = frame + header_bytes;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, rocketnet_test_filter_value );
        const int written_length = length > ROCKETNET_RECEIVE_CEILING ? ROCKETNET_RECEIVE_CEILING : length;
        check( rocketnet_xdp_frame_build( frame, sizeof( frame ), mac, mac, &source, &destination,
                                          written_length ) == header_bytes + written_length );
        // The one-past boundary changes only the envelope lengths in this
        // in-memory frame; it is never submitted to a socket or a NIC.
        rocketnet_xdp_frame_write_be16( frame + header_bytes - 4, (uint16_t) ( length + ROCKETNET_UDP_HEADER_BYTES ) );
        rocketnet_xdp_frame_write_be16( frame + ( ipv6 ? 18 : 16 ),
                                        (uint16_t) ( length + ROCKETNET_UDP_HEADER_BYTES + ( ipv6 ? 0 : ROCKETNET_IPV4_HEADER_BYTES ) ) );
        const bool valid = length >= floor && length <= ROCKETNET_RECEIVE_CEILING;
        check( rocketnet_xdp_host_run( frame, header_bytes + length, 0 ) == ( valid ? 4 : 1 ) );
        if ( valid )
        {
            rocketnet_address_t from;
            const uint8_t * payload = NULL;
            int payload_bytes = 0;
            rocketnet_xdp_receive_drops_t drops = {};
            check( rocketnet_xdp_frame_receive( frame, (size_t) ( header_bytes + length ), source.type,
                                                &from, &payload, &payload_bytes, &drops ) );
            check( payload == frame + header_bytes && payload_bytes == length );
            check( rocketnet_address_equal( &from, &source ) );
        }
    }
    check( rocketnet_xdp_host_redirect_calls() == 2 );
    check( rocketnet_xdp_host_counter( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) == 1 );
}

static void test_audit_xdp_ipv4_bounds() { audit_xdp_bounds( false ); }
static void test_audit_xdp_ipv6_bounds() { audit_xdp_bounds( true ); }
REGISTER_UNIT_TEST( test_audit_xdp_ipv4_bounds );
REGISTER_UNIT_TEST( test_audit_xdp_ipv6_bounds );
#endif

static void test_audit_xdp_descriptor_bounds()
{
    uint8_t frame[ROCKETNET_XDP_FRAME_SIZE] = {};
    rocketnet_xdp_receive_drops_t drops = {};
    check( rocketnet_xdp_receive_descriptor( frame, 0, sizeof( frame ), 0, 0, &drops ) == frame );
    check( rocketnet_xdp_receive_descriptor( frame, 0, sizeof( frame ), 0, sizeof( frame ), &drops ) == frame );
    check( rocketnet_xdp_receive_descriptor( frame, 0, sizeof( frame ), 0, sizeof( frame ) + 1, &drops ) == NULL );
    check( rocketnet_xdp_receive_descriptor( frame, 0, sizeof( frame ), sizeof( frame ) - 1, 1, &drops ) == frame + sizeof( frame ) - 1 );
    check( rocketnet_xdp_receive_descriptor( frame, 0, sizeof( frame ), sizeof( frame ) - 1, 2, &drops ) == NULL );
    check( drops.descriptor == 2 );
}

REGISTER_UNIT_TEST( test_audit_prefilter_bounds );
REGISTER_UNIT_TEST( test_audit_server_request_bounds );
REGISTER_UNIT_TEST( test_audit_client_cookie_bounds );
REGISTER_UNIT_TEST( test_audit_client_challenge_bounds );
REGISTER_UNIT_TEST( test_audit_client_denied_bounds );
REGISTER_UNIT_TEST( test_audit_server_response_bounds );
REGISTER_UNIT_TEST( test_audit_server_keep_alive_bounds );
REGISTER_UNIT_TEST( test_audit_server_payload_bounds );
REGISTER_UNIT_TEST( test_audit_server_disconnect_bounds );
REGISTER_UNIT_TEST( test_audit_server_ack_bounds );
REGISTER_UNIT_TEST( test_audit_client_keep_alive_bounds );
REGISTER_UNIT_TEST( test_audit_client_payload_bounds );
REGISTER_UNIT_TEST( test_audit_client_disconnect_bounds );
REGISTER_UNIT_TEST( test_audit_client_ack_bounds );
REGISTER_UNIT_TEST( test_audit_rejected_packet_types );
REGISTER_UNIT_TEST( test_audit_client_io_bounds );
REGISTER_UNIT_TEST( test_audit_ack_block_bounds );
REGISTER_UNIT_TEST( test_audit_config_body_bounds );
REGISTER_UNIT_TEST( test_audit_message_record_bounds );
REGISTER_UNIT_TEST( test_audit_coalesced_receive_bounds );
REGISTER_UNIT_TEST( test_audit_xdp_descriptor_bounds );
