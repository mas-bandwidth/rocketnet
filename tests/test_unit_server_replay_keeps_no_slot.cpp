/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A connected slot's idle clock moves only for fresh session traffic that
// opens under the connection's keys (standard/TRANSPORT.md t12, t13). The
// handshake packets are exempt from replay marking, so a captured connection
// response still opens whenever it is replayed: only a half-open mapping may
// refresh its clock from one (sdk/rocketnet_server_protocol.cpp, the
// response branch of the sealed path). A captured connection request, with or
// without its cookie, and a keep-alive whose sequence is already received
// refresh nothing either. Replayed on path every half timeout, none of them
// keeps a gone client's slot alive: it times out on schedule, the slot is
// freed, and no replay is answered with more bytes than it carries.
//
// And the other side of the same line: a half-open mapping's clock does move
// on its response, so a slow completion counts the connected timeout from the
// response, not from the request.
//
// A client and a server protocol pair run in memory on a mocked clock that
// starts at 1000.0: no sockets, no network, no real time. Everything big is
// heap held.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#define REPLAY_SLOTS 4
#define REPLAY_TIMEOUT 10           // the token's timeout seconds

enum replay_captured_t
{
    REPLAY_REQUEST = 0,             // the first connection request, no cookie
    REPLAY_REQUEST_WITH_COOKIE,
    REPLAY_RESPONSE,
    REPLAY_KEEP_ALIVE,              // a keep-alive the server has already received
    REPLAY_CAPTURED
};

struct replay_pair_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    int end_reasons[REPLAY_SLOTS];
    struct rocketnet_address_t client_address;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    uint8_t captured[REPLAY_CAPTURED][ROCKETNET_RECEIVE_CEILING];
    int captured_bytes[REPLAY_CAPTURED];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    double last_heard;              // the last time the server heard fresh traffic from the client
};

static void replay_mint( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = REPLAY_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 0x4E91A7ULL;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// the server receives a copy of the bytes (the open may work in place);
// returns what it sends back, in bytes

static int replay_deliver( struct replay_pair_t * pair, const uint8_t * data, int bytes, double time )
{
    memcpy( pair->packet, data, (size_t) bytes );
    int response_bytes = 0, payload_bytes = 0, payload_client_index = -1;
    (void) rocketnet_server_protocol_process_packet( &pair->server, &pair->client_address, pair->packet, bytes,
                                                     ROCKETNET_TEST_CREATE_TS, time,
                                                     pair->response, &response_bytes,
                                                     pair->payload, &payload_bytes, &payload_client_index );
    return response_bytes;
}

static void replay_capture( struct replay_pair_t * pair, int which, int bytes )
{
    check( bytes > 0 );
    memcpy( pair->captured[which], pair->packet, (size_t) bytes );
    pair->captured_bytes[which] = bytes;
}

// the client sends at 1000.0, 1000.2 and 1000.4; its response reaches the
// server response_delay later. The server's answers reach the client at
// once. Then, when keep_alive, one keep-alive at 1000.5 + response_delay,
// heard by the server.

static struct replay_pair_t * replay_pair_create( double response_delay, bool keep_alive )
{
    struct replay_pair_t * pair = (struct replay_pair_t *) malloc( sizeof( struct replay_pair_t ) );
    check( pair != NULL );
    memset( pair, 0, sizeof( *pair ) );

    struct rocketnet_server_protocol_t * server = &pair->server;
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
    server->client_slots = rocketnet_client_slots_create( NULL, REPLAY_SLOTS );
    server->acks = rocketnet_server_acks_create( NULL, REPLAY_SLOTS );
    server->end_reasons = pair->end_reasons;
    server->counters = &pair->server_counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, 1000.0 );
    check( rocketnet_address_parse( &pair->client_address, "198.51.100.7:56789" ) );

    rocketnet_client_protocol_init( &pair->client, &pair->client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    replay_mint( token_data );
    check( rocketnet_client_protocol_connect( &pair->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );

    const double sent[3] = { 1000.0, 1000.2, 1000.4 };
    const int captured[3] = { REPLAY_REQUEST, REPLAY_REQUEST_WITH_COOKIE, REPLAY_RESPONSE };
    int payload_bytes = 0;

    for ( int i = 0; i < 3; i++ )
    {
        int bytes = 0;
        rocketnet_client_protocol_update( &pair->client, sent[i], pair->packet, &bytes );
        replay_capture( pair, captured[i], bytes );
        const double arrival = sent[i] + ( i == 2 ? response_delay : 0.0 );
        const int answer_bytes = replay_deliver( pair, pair->captured[captured[i]], bytes, arrival );
        check( answer_bytes > 0 );
        check( rocketnet_client_protocol_process_packet( &pair->client, pair->response, answer_bytes, arrival,
                                                         pair->payload, (int) sizeof( pair->payload ), &payload_bytes ) );
    }

    check( pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_client_slots_num_connected( server->client_slots ) == 1 );

    pair->last_heard = 1000.4 + response_delay;
    if ( !keep_alive )
    {
        return pair;
    }

    const double keep_alive_time = 1000.5 + response_delay;
    int bytes = 0;
    rocketnet_client_protocol_update( &pair->client, keep_alive_time, pair->packet, &bytes );
    check( pair->packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );
    replay_capture( pair, REPLAY_KEEP_ALIVE, bytes );
    (void) replay_deliver( pair, pair->captured[REPLAY_KEEP_ALIVE], bytes, keep_alive_time );
    pair->last_heard = keep_alive_time;

    return pair;
}

static void replay_pair_destroy( struct replay_pair_t * pair )
{
    rocketnet_counters_assert_conserved( &pair->server_counters );
    rocketnet_counters_assert_conserved( &pair->client_counters );
    rocketnet_encryption_map_destroy( pair->server.encryption_map );
    rocketnet_used_token_history_destroy( pair->server.used_token_history );
    rocketnet_client_slots_destroy( pair->server.client_slots );
    rocketnet_server_acks_destroy( pair->server.acks );
    free( pair );
}

static int replay_slot_held( struct replay_pair_t * pair )
{
    return rocketnet_encryption_map_find( pair->server.encryption_map, &pair->client_address ) != -1 &&
           rocketnet_client_slots_connected( pair->server.client_slots, 0 );
}

// replay one captured packet (or all of them, which = -1) every half timeout
// from the last fresh traffic on: the slot lives until exactly the timeout
// and is then freed with TIMED_OUT; replays after that keep it freed

static void replay_on_path_and_check( int which )
{
    struct replay_pair_t * pair = replay_pair_create( 0.0, true );
    const double heard = pair->last_heard;
    const double half = REPLAY_TIMEOUT / 2.0;

    for ( int k = 1; k <= 8; k++ )
    {
        const double t = heard + half * k;

        for ( int c = 0; c < REPLAY_CAPTURED; c++ )
        {
            if ( which != -1 && c != which )
            {
                continue;
            }
            const int answer_bytes = replay_deliver( pair, pair->captured[c], pair->captured_bytes[c], t );
            check( answer_bytes <= pair->captured_bytes[c] );
        }

        if ( k == 1 )
        {
            rocketnet_server_protocol_update( &pair->server, t );
            check( replay_slot_held( pair ) );
            rocketnet_server_protocol_update( &pair->server, heard + REPLAY_TIMEOUT - 0.01 );
            check( replay_slot_held( pair ) );
            check( pair->server_counters.values[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == 0 );
            continue;
        }

        rocketnet_server_protocol_update( &pair->server, t );
        check( !replay_slot_held( pair ) );
        check( pair->server_counters.values[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == 1 );
        check( rocketnet_server_protocol_end_reason( &pair->server, 0 ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );
        check( rocketnet_client_slots_num_connected( pair->server.client_slots ) == 0 );
        check( rocketnet_client_slots_num_free( pair->server.client_slots ) == REPLAY_SLOTS );
    }

    replay_pair_destroy( pair );
}

static void test_server_replayed_response_keeps_no_slot_alive()
{
    replay_on_path_and_check( REPLAY_RESPONSE );
}

REGISTER_UNIT_TEST( test_server_replayed_response_keeps_no_slot_alive );

static void test_server_replayed_handshake_and_old_keep_alive_keep_no_slot_alive()
{
    replay_on_path_and_check( REPLAY_REQUEST );
    replay_on_path_and_check( REPLAY_REQUEST_WITH_COOKIE );
    replay_on_path_and_check( REPLAY_KEEP_ALIVE );
    replay_on_path_and_check( -1 );
}

REGISTER_UNIT_TEST( test_server_replayed_handshake_and_old_keep_alive_keep_no_slot_alive );

// the response reaches the half-open mapping 3 seconds after the request
// that made it (inside the handshake deadline of 5): the mapping's clock
// moves to the response, so the connected timeout counts from there

static void test_server_slow_response_refreshes_the_half_open_mapping()
{
    const double delay = 3.0;
    struct replay_pair_t * pair = replay_pair_create( delay, false );
    const double response_arrival = pair->last_heard;
    check( response_arrival == 1000.4 + delay );

    // no traffic after the response: the slot outlives the request's time
    // plus the timeout, and times out at the response's time plus the timeout

    rocketnet_server_protocol_update( &pair->server, 1000.2 + REPLAY_TIMEOUT + 0.5 );
    check( replay_slot_held( pair ) );
    rocketnet_server_protocol_update( &pair->server, response_arrival + REPLAY_TIMEOUT - 0.01 );
    check( replay_slot_held( pair ) );
    rocketnet_server_protocol_update( &pair->server, response_arrival + REPLAY_TIMEOUT );
    check( !replay_slot_held( pair ) );
    check( rocketnet_server_protocol_end_reason( &pair->server, 0 ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );

    replay_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_server_slow_response_refreshes_the_half_open_mapping );
