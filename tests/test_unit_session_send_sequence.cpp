/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The session send sequence is the ChaCha20-Poly1305 nonce under one
// directional session key (rocketnet_sequence_nonce), so no two packets on a
// connection may ever carry the same one, whatever order a caller drives the
// control and fill paths in. Every session send takes its sequence with one
// atomic fetch-add that returns the value it seals. The ack block the other end
// returns is read against the highest sequence actually sealed and recorded
// sent, so a sequence a fill has taken but not yet sealed never moves it.
//
// Each case runs a client and server protocol pair in memory on a mocked
// clock, on one thread: no sockets, no real time. The interleaving is forced
// through the DEV sequence-taken seam (sdk/rocketnet_test_seams.h): a
// fill-phase start runs inside a control send, after that send has taken its
// sequence and before it seals.

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
#include "rocketnet_fragment.h"
#include "rocketnet_message.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// the in-memory pair: a server protocol on the test vector keys and a client
// connected to it through the three-round-trip handshake on mocked time
// ---------------------------------------------------------------------------

struct send_sequence_pair_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    double time;
};

static void send_sequence_mint_token( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = 10;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 0x5E9ULL;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void send_sequence_pair_setup_rounds( struct send_sequence_pair_t * pair, int rounds )
{
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
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &pair->server_counters;
    check( server->encryption_map && server->used_token_history &&
           server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );
    check( rocketnet_address_parse( &pair->client_address, "198.51.100.7:56789" ) );

    rocketnet_client_protocol_init( &pair->client, &pair->client_counters );
    pair->client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, ROCKETNET_NUM_CLIENT_PACKETS / 2 );
    check( pair->client.reassembly );
    rocketnet_reassembly_set_begin_update( pair->client.reassembly );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    send_sequence_mint_token( token_data );
    check( rocketnet_client_protocol_connect( &pair->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );

    static uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    static uint8_t response[ROCKETNET_RECEIVE_CEILING];
    static uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0, response_bytes = 0, payload_bytes = 0, payload_client_index = -1;

    const double times[3] = { 1000.0, 1000.2, 1000.4 };

    for ( int i = 0; i < rounds; i++ )
    {
        rocketnet_client_protocol_update( &pair->client, times[i], packet, &packet_bytes );
        check( packet_bytes > 0 );
        check( rocketnet_server_protocol_process_packet( server, &pair->client_address, packet, packet_bytes,
                                                         ROCKETNET_TEST_CREATE_TS, times[i],
                                                         response, &response_bytes,
                                                         payload, &payload_bytes, &payload_client_index ) != ROCKETNET_SERVER_PROCESS_IGNORED );
        check( rocketnet_client_protocol_process_packet( &pair->client, response, response_bytes, times[i] + 0.05,
                                                         payload, (int) sizeof( payload ), &payload_bytes ) );
    }

    pair->time = times[rounds - 1] + 0.1;
}

static void send_sequence_pair_setup( struct send_sequence_pair_t * pair )
{
    send_sequence_pair_setup_rounds( pair, 3 );
    check( pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( pair->client.client_index == 0 );
    pair->time = 1001.0;
}

static void send_sequence_pair_teardown( struct send_sequence_pair_t * pair )
{
    rocketnet_dev_set_sequence_taken_hook( NULL, NULL );
    rocketnet_counters_assert_conserved( &pair->server_counters );
    rocketnet_counters_assert_conserved( &pair->client_counters );
    rocketnet_reassembly_set_destroy( pair->client.reassembly );
    rocketnet_encryption_map_destroy( pair->server.encryption_map );
    rocketnet_used_token_history_destroy( pair->server.used_token_history );
    rocketnet_client_slots_destroy( pair->server.client_slots );
    rocketnet_server_acks_destroy( pair->server.acks );
    rocketnet_reassembly_set_destroy( pair->server.reassembly );
}

static struct rocketnet_encryption_mapping_t * send_sequence_server_mapping( struct send_sequence_pair_t * pair )
{
    const int index = rocketnet_encryption_map_find( pair->server.encryption_map, &pair->client_address );
    check( index >= 0 );
    return rocketnet_encryption_map_entry( pair->server.encryption_map, index );
}

// writes a small SINGLE-form payload at the frame's payload offset

alignas( 16 ) static uint8_t g_send_sequence_frame[ROCKETNET_FRAME_STRIDE];

static int send_sequence_write_payload( uint8_t salt )
{
    memset( g_send_sequence_frame, 0, sizeof( g_send_sequence_frame ) );
    uint8_t * root = g_send_sequence_frame + ROCKETNET_FRAME_PAYLOAD_OFFSET;
    for ( int i = 0; i < 8; i++ )
        root[i] = (uint8_t) ( salt + i );
    root[8] = ROCKETNET_MESSAGE_FORM_SINGLE;
    return 9;
}

// ---------------------------------------------------------------------------
// the interleaving: the seam runs one fill-phase start inside the control
// send, between the control send taking its sequence and sealing it
// ---------------------------------------------------------------------------

struct send_sequence_interleave_t
{
    struct send_sequence_pair_t * pair;
    bool server_side;
    int calls;
    uint64_t control_sequence;
    uint64_t fill_sequence;
};

static void send_sequence_fill_inside_control( void * context, uint64_t sequence )
{
    struct send_sequence_interleave_t * interleave = (struct send_sequence_interleave_t *) context;
    interleave->calls++;
    if ( interleave->calls != 1 )
        return;

    interleave->control_sequence = sequence;

    if ( interleave->server_side )
    {
        struct rocketnet_address_t to;
        check( rocketnet_server_protocol_start_payload( &interleave->pair->server, 0, &to, &interleave->fill_sequence ) );
    }
    else
    {
        check( rocketnet_client_protocol_start_payload( &interleave->pair->client, &interleave->fill_sequence ) );
    }
}

static uint64_t send_sequence_max( uint64_t a, uint64_t b )
{
    return a > b ? a : b;
}

// ---------------------------------------------------------------------------
// Server: a keep-alive sent while a fill holds an unsealed sequence. The two
// sequences differ, the ack baseline names only what is sealed, both packets
// open at the client, and the client's ack of both comes back exact.
// ---------------------------------------------------------------------------

static void test_server_control_send_inside_fill_takes_a_distinct_sequence()
{
    static struct send_sequence_pair_t pair;
    send_sequence_pair_setup( &pair );

    struct rocketnet_encryption_mapping_t * mapping = send_sequence_server_mapping( &pair );
    const uint64_t baseline_before = rocketnet_server_protocol_highest_sent_session( &pair.server, 0, mapping );

    struct send_sequence_interleave_t interleave;
    memset( &interleave, 0, sizeof( interleave ) );
    interleave.pair = &pair;
    interleave.server_side = true;
    rocketnet_dev_set_sequence_taken_hook( send_sequence_fill_inside_control, &interleave );

    static uint8_t keep_alive[ROCKETNET_SEND_BUDGET];
    pair.time += 0.01;
    const int keep_alive_bytes = rocketnet_server_write_keep_alive( &pair.server, 0, pair.time, keep_alive );
    rocketnet_dev_set_sequence_taken_hook( NULL, NULL );

    check( keep_alive_bytes > 0 );
    check( interleave.calls == 1 );
    check( interleave.control_sequence != 0 && interleave.fill_sequence != 0 );
    check( interleave.control_sequence != interleave.fill_sequence );

    // the fill's sequence is taken but unsealed: the baseline is the
    // keep-alive alone

    check( rocketnet_server_protocol_highest_sent_session( &pair.server, 0, mapping ) == interleave.control_sequence );
    check( interleave.control_sequence > baseline_before );

    // the fill finishes

    const int payload_bytes = send_sequence_write_payload( 0x40 );
    uint8_t * wire_start = g_send_sequence_frame + ROCKETNET_FRAME_WIRE_OFFSET;
    const int payload_wire_bytes = rocketnet_server_protocol_seal_payload_frame( &pair.server, 0, interleave.fill_sequence,
                                                                                 interleave.fill_sequence, wire_start,
                                                                                 payload_bytes, pair.time );
    check( payload_wire_bytes > 0 );

    const uint64_t highest = send_sequence_max( interleave.control_sequence, interleave.fill_sequence );
    check( rocketnet_server_protocol_highest_sent_session( &pair.server, 0, mapping ) == highest );

    // both open at the client: a reused sequence would be refused by the
    // replay window, or open the second packet under a reused nonce

    static uint8_t received[ROCKETNET_RECEIVE_CEILING];
    int received_bytes = 0;
    pair.time += 0.01;
    check( rocketnet_client_protocol_process_packet( &pair.client, keep_alive, keep_alive_bytes, pair.time,
                                                     received, (int) sizeof( received ), &received_bytes ) == 1 );
    check( rocketnet_client_protocol_process_packet( &pair.client, wire_start, payload_wire_bytes, pair.time,
                                                     received, (int) sizeof( received ), &received_bytes ) == 1 );
    check( received_bytes == payload_bytes );

    // the client's next block names both, and the server reads it against
    // its baseline: every named sequence is one it sent

    const uint64_t unknown_before = pair.server_counters.values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE];

    uint64_t client_sequence = 0;
    check( rocketnet_client_protocol_start_payload( &pair.client, &client_sequence ) );
    const int reply_bytes = send_sequence_write_payload( 0x50 );
    const int reply_wire_bytes = rocketnet_client_protocol_seal_payload_frame( &pair.client, client_sequence, client_sequence,
                                                                               wire_start, reply_bytes, pair.time );
    check( reply_wire_bytes > 0 );

    static uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0, client_index = -1;
    pair.time += 0.01;
    check( rocketnet_server_protocol_process_packet( &pair.server, &pair.client_address, wire_start, reply_wire_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, pair.time, response, &response_bytes,
                                                     received, &received_bytes, &client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    check( pair.server_counters.values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE] == unknown_before );
    check( pair.server.acks->endpoints[0].largest_acked_sequence == highest );

    send_sequence_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// Client: the mirror. A keep-alive from update while a fill holds an
// unsealed sequence.
// ---------------------------------------------------------------------------

static void test_client_control_send_inside_fill_takes_a_distinct_sequence()
{
    static struct send_sequence_pair_t pair;
    send_sequence_pair_setup( &pair );

    struct send_sequence_interleave_t interleave;
    memset( &interleave, 0, sizeof( interleave ) );
    interleave.pair = &pair;
    interleave.server_side = false;
    rocketnet_dev_set_sequence_taken_hook( send_sequence_fill_inside_control, &interleave );

    // a second past the last send: the keep-alive is due

    static uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
    int keep_alive_bytes = 0;
    pair.time += 1.0;
    rocketnet_client_protocol_update( &pair.client, pair.time, keep_alive, &keep_alive_bytes );
    rocketnet_dev_set_sequence_taken_hook( NULL, NULL );

    check( keep_alive_bytes > 0 );
    check( interleave.calls == 1 );
    check( interleave.control_sequence != 0 && interleave.fill_sequence != 0 );
    check( interleave.control_sequence != interleave.fill_sequence );
    check( rocketnet_client_protocol_highest_sent_session( &pair.client ) == interleave.control_sequence );

    const int payload_bytes = send_sequence_write_payload( 0x60 );
    uint8_t * wire_start = g_send_sequence_frame + ROCKETNET_FRAME_WIRE_OFFSET;
    const int payload_wire_bytes = rocketnet_client_protocol_seal_payload_frame( &pair.client, interleave.fill_sequence,
                                                                                 interleave.fill_sequence, wire_start,
                                                                                 payload_bytes, pair.time );
    check( payload_wire_bytes > 0 );

    const uint64_t highest = send_sequence_max( interleave.control_sequence, interleave.fill_sequence );
    check( rocketnet_client_protocol_highest_sent_session( &pair.client ) == highest );

    static uint8_t received[ROCKETNET_RECEIVE_CEILING];
    static uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int received_bytes = 0, response_bytes = 0, client_index = -1;
    pair.time += 0.01;
    check( rocketnet_server_protocol_process_packet( &pair.server, &pair.client_address, keep_alive, keep_alive_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, pair.time, response, &response_bytes,
                                                     received, &received_bytes, &client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    check( rocketnet_server_protocol_process_packet( &pair.server, &pair.client_address, wire_start, payload_wire_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, pair.time, response, &response_bytes,
                                                     received, &received_bytes, &client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    check( received_bytes == payload_bytes );

    // the server's next block names both

    const uint64_t unknown_before = pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_UNKNOWN_SEQUENCE];

    uint64_t server_sequence = 0;
    struct rocketnet_address_t to;
    check( rocketnet_server_protocol_start_payload( &pair.server, 0, &to, &server_sequence ) );
    const int reply_bytes = send_sequence_write_payload( 0x70 );
    const int reply_wire_bytes = rocketnet_server_protocol_seal_payload_frame( &pair.server, 0, server_sequence, server_sequence,
                                                                               wire_start, reply_bytes, pair.time );
    check( reply_wire_bytes > 0 );

    pair.time += 0.01;
    check( rocketnet_client_protocol_process_packet( &pair.client, wire_start, reply_wire_bytes, pair.time,
                                                     received, (int) sizeof( received ), &received_bytes ) == 1 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_UNKNOWN_SEQUENCE] == unknown_before );
    check( pair.client.ack.largest_acked_sequence == highest );

    send_sequence_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// The update sweep's handshake-deadline branch releases the mapping's request
// claim, as the idle and integrity branches do. The claim and the mapping are
// born with the same deadline, so the release is normally a no-op; here the
// mapping's deadline is moved earlier so the claim is still live when the
// sweep removes the mapping, and it must leave with it.
// ---------------------------------------------------------------------------

static void test_server_handshake_deadline_releases_the_request_claim()
{
    static struct send_sequence_pair_t pair;
    send_sequence_pair_setup_rounds( &pair, 2 );     // cookie, then request: a half-open mapping holding a claim
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    struct rocketnet_encryption_mapping_t * mapping = send_sequence_server_mapping( &pair );
    check( !mapping->control->established );

    uint8_t token_identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    memcpy( token_identifier, mapping->token_identifier, sizeof( token_identifier ) );

    struct rocketnet_address_t other;
    check( rocketnet_address_parse( &other, "198.51.100.8:56789" ) );

    const int free_with_claim = rocketnet_used_token_history_claim_free_count( pair.server.used_token_history );
    check( rocketnet_used_token_history_claim_check( pair.server.used_token_history, token_identifier, &other, pair.time ) == 0 );

    mapping->control->handshake_deadline = pair.time + 0.1;
    const double sweep_time = pair.time + 0.2;          // past the mapping's deadline, inside the claim's and the idle timeout
    check( sweep_time < pair.time - 0.1 + pair.server.handshake_deadline_seconds );

    const uint64_t expired_before = pair.server_counters.values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED];
    rocketnet_server_protocol_update( &pair.server, sweep_time );

    check( pair.server_counters.values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == expired_before + 1 );
    check( rocketnet_encryption_map_find( pair.server.encryption_map, &pair.client_address ) < 0 );
    check( rocketnet_used_token_history_claim_check( pair.server.used_token_history, token_identifier, &other, sweep_time ) == 1 );
    check( rocketnet_used_token_history_claim_free_count( pair.server.used_token_history ) == free_with_claim + 1 );

    send_sequence_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_server_control_send_inside_fill_takes_a_distinct_sequence );
REGISTER_UNIT_TEST( test_server_handshake_deadline_releases_the_request_claim );
REGISTER_UNIT_TEST( test_client_control_send_inside_fill_takes_a_distinct_sequence );

#endif // #if ROCKETNET_DEV
