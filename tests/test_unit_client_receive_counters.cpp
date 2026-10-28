/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Table-driven receive tests, one row per counter that no test named: the
// client's whole receive ladder and the session-type refusals the server's
// ladder was only ever summed over. Every row crafts one packet with the
// wire's own sealing under a live session's keys, hands it to the one end,
// and asserts the exact delta: the packet entry's PACKETS_RECEIVED, the
// type's RECEIVED, the one outcome, and nothing else moved. Conservation of
// both counter blocks is asserted when each fixture is torn down.
//
// Every case runs a client and a server protocol in memory on mocked time:
// no sockets, no real time.

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
#include "rocketnet_replay.h"
#include "rocketnet_fragment.h"
#include "rocketnet_keys.h"
#include "rocketnet_read_write.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// ---------------------------------------------------------------------------
// the fixture: a server protocol on the test vector keys and a client driven
// through the handshake to a chosen stop
// ---------------------------------------------------------------------------

struct rcr_pair_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    double time;
    uint64_t next_sequence;             // crafted packets name sequences from here, well clear of the handshake's
    uint8_t held_challenge[ROCKETNET_RECEIVE_CEILING];
    int held_challenge_bytes;
};

enum
{
    RCR_STAGE_DISCONNECTED,             // a client that never connected, state disconnected
    RCR_STAGE_REQUESTING_COOKIE,        // connect called, nothing sent yet
    RCR_STAGE_CHALLENGE_HELD,           // cookie stored, the server's challenge produced and held back
    RCR_STAGE_HALF_OPEN,                // the client holds session keys, the server holds a mapping without a slot
    RCR_STAGE_CONNECTED,                // both ends connected
};

static void rcr_mint_token( uint8_t * token_data, uint64_t client_id )
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
    private_fields.client_id = client_id;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void rcr_server_setup( struct rocketnet_server_protocol_t * server, struct rocketnet_counters_t * counters )
{
    memset( server, 0, sizeof( *server ) );
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = counters;
    check( server->encryption_map && server->used_token_history &&
           server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );
}

static void rcr_server_teardown( struct rocketnet_server_protocol_t * server )
{
    rocketnet_encryption_map_destroy( server->encryption_map );
    rocketnet_used_token_history_destroy( server->used_token_history );
    rocketnet_client_slots_destroy( server->client_slots );
    rocketnet_server_acks_destroy( server->acks );
    rocketnet_reassembly_set_destroy( server->reassembly );
}

static void rcr_client_init( struct rocketnet_client_protocol_t * client, struct rocketnet_counters_t * counters )
{
    memset( counters, 0, sizeof( *counters ) );
    rocketnet_client_protocol_init( client, counters );
    client->reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, ROCKETNET_NUM_CLIENT_PACKETS / 2 );
    check( client->reassembly );
    rocketnet_reassembly_set_begin_update( client->reassembly );
}

// one packet from the client to the server, the server's response (if any)
// handed straight back to the client

static void rcr_pump_client_packet( struct rcr_pair_t * pair, const uint8_t * packet, int packet_bytes, double time,
                                    uint8_t * response, int * response_bytes )
{
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    *response_bytes = 0;
    rocketnet_server_protocol_process_packet( &pair->server, &pair->client_address, packet, packet_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, response, response_bytes,
                                              payload, &payload_bytes, &payload_client_index );
}

static void rcr_pair_setup( struct rcr_pair_t * pair, int stage )
{
    memset( pair, 0, sizeof( *pair ) );
    rcr_server_setup( &pair->server, &pair->server_counters );
    check( rocketnet_address_parse( &pair->client_address, "198.51.100.7:56789" ) );
    rcr_client_init( &pair->client, &pair->client_counters );
    pair->next_sequence = 1000;
    pair->time = 100.0;

    if ( stage == RCR_STAGE_DISCONNECTED )
        return;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rcr_mint_token( token_data, 0x1400ULL );
    check( rocketnet_client_protocol_connect( &pair->client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    if ( stage == RCR_STAGE_REQUESTING_COOKIE )
        return;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    for ( double time = 100.0; time < 110.0; time += 0.05 )
    {
        int packet_bytes = 0;
        rocketnet_client_protocol_update( &pair->client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
        {
            int response_bytes = 0;
            rcr_pump_client_packet( pair, packet, packet_bytes, time, response, &response_bytes );
            if ( response_bytes > 0 )
            {
                if ( stage == RCR_STAGE_CHALLENGE_HELD && response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES )
                {
                    memcpy( pair->held_challenge, response, (size_t) response_bytes );
                    pair->held_challenge_bytes = response_bytes;
                    pair->time = time + 0.01;
                    return;
                }
                rocketnet_client_protocol_process_packet( &pair->client, response, response_bytes, time + 0.01,
                                                          payload, (int) sizeof( payload ), &payload_bytes );
            }
        }
        if ( stage == RCR_STAGE_HALF_OPEN && pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
        {
            pair->time = time + 0.02;
            return;
        }
        if ( stage == RCR_STAGE_CONNECTED && pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            check( pair->client.client_index == 0 );
            pair->time = time + 0.02;
            return;
        }
    }
    check( 0 && "the handshake did not reach the stage" );
}

static void rcr_pair_teardown( struct rcr_pair_t * pair )
{
    rocketnet_counters_assert_conserved( &pair->server_counters );
    rocketnet_counters_assert_conserved( &pair->client_counters );
    rocketnet_reassembly_set_destroy( pair->client.reassembly );
    rcr_server_teardown( &pair->server );
}

// delivery. the server sees the client's packets from the one address; both
// ends advance mocked time a millisecond per packet

static int rcr_to_server( struct rcr_pair_t * pair, const uint8_t * packet, int packet_bytes )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    int payload_bytes = 0;
    int payload_client_index = -1;
    pair->time += 0.001;
    return rocketnet_server_protocol_process_packet( &pair->server, &pair->client_address, packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, pair->time, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client_index );
}

static int rcr_to_server_from( struct rcr_pair_t * pair, const struct rocketnet_address_t * from, const uint8_t * packet, int packet_bytes )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    int payload_bytes = 0;
    int payload_client_index = -1;
    pair->time += 0.001;
    return rocketnet_server_protocol_process_packet( &pair->server, from, packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, pair->time, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client_index );
}

static int rcr_to_client( struct rcr_pair_t * pair, const uint8_t * packet, int packet_bytes )
{
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    pair->time += 0.001;
    return rocketnet_client_protocol_process_packet( &pair->client, packet, packet_bytes, pair->time,
                                                     payload, (int) sizeof( payload ), &payload_bytes );
}

// ---------------------------------------------------------------------------
// crafting: the wire's own sealing, under the client's live session keys
// ---------------------------------------------------------------------------

static int rcr_block_fresh( uint8_t * out )
{
    memset( out, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    return ROCKETNET_ACK_BLOCK_MIN_BYTES;
}

// a valid block with one loss range: largest 40, the top run 1, then a gap
// of 1 and a run of 1 -- eleven bytes that describe three sequences

static int rcr_block_ranged( uint8_t * out )
{
    uint8_t * p = out;
    rocketnet_write_uint32( &p, 40 );
    rocketnet_write_uint8( &p, 1 );
    rocketnet_write_uint16( &p, 1 );
    rocketnet_write_uint16( &p, 1 );
    rocketnet_write_uint16( &p, 1 );
    return (int) ( p - out );
}

// a block that names a largest and an empty first run: malformed

static int rcr_block_bad( uint8_t * out )
{
    uint8_t * p = out;
    rocketnet_write_uint32( &p, 5 );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint16( &p, 0 );
    return (int) ( p - out );
}

// the wire floor of a session type

static int rcr_floor( uint8_t type )
{
    switch ( type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET: return ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:    return ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET: return ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_ACK_PACKET:        return ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:   return ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES;
        default:                                     check( 0 ); return 0;
    }
}

// the sealed plaintext of a valid packet of the type: a fresh block and the
// type's own data. a keep-alive names client index 0 of 4 slots.

static int rcr_plaintext_valid( uint8_t type, uint8_t * out )
{
    uint8_t * p = out + rcr_block_fresh( out );
    switch ( type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET:
            rocketnet_write_uint32( &p, 0 );
            rocketnet_write_uint32( &p, 4 );
            break;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:
            rocketnet_write_uint8( &p, 0xAB );
            break;
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:
            rocketnet_write_uint32( &p, 1 );                // payload id
            rocketnet_write_uint8( &p, 0 );                 // fragment index
            rocketnet_write_uint8( &p, 1 );                 // count minus one
            rocketnet_write_uint8( &p, 0xEE );              // one fragment byte
            break;
        default:
            break;
    }
    return (int) ( p - out );
}

// a malformed block, padded with zeros to the type's floor

static int rcr_plaintext_bad_block( uint8_t type, uint8_t * out )
{
    const int bytes = rcr_floor( type ) - ROCKETNET_PACKET_OVERHEAD_BYTES;
    memset( out, 0, (size_t) bytes );
    rcr_block_bad( out );
    return bytes;
}

// a valid block over type data of the wrong size, at or above the type's floor

static int rcr_plaintext_wrong_opened_size( uint8_t type, uint8_t * out )
{
    memset( out, 0, 64 );
    switch ( type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET:
            return rcr_block_fresh( out ) + 9;              // nine bytes where eight belong
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:
            return rcr_block_ranged( out );                 // a payload of nothing: the wide block alone clears the wire floor
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET:
        case ROCKETNET_CONNECTION_ACK_PACKET:
            return rcr_block_fresh( out ) + 1;              // one byte where none belong
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:
            return rcr_block_ranged( out ) + 3;             // three bytes behind the block: under the header and a byte
        default:
            check( 0 );
            return 0;
    }
}

// seals a session packet. to_server uses the client's send key, otherwise the
// server's. a zero sequence is written by sealing sequence 1 and clearing the
// four sequence bytes: the receiver refuses a zero sequence before it opens.

static int rcr_seal( struct rcr_pair_t * pair, int to_server, uint8_t type, uint64_t sequence,
                     const uint8_t * plaintext, int plaintext_bytes, const uint8_t * key_override, uint8_t * out )
{
    const uint8_t * key = key_override ? key_override : ( to_server ? pair->client.keys.session_client_to_server : pair->client.keys.session_server_to_client );
    const uint64_t sealed_sequence = sequence ? sequence : 1;
    const int bytes = rocketnet_write_sealed_packet( out, type, pair->client.packet_filter, sealed_sequence, NULL,
                                                     plaintext, plaintext_bytes,
                                                     pair->client.signed_fields.protocol_id, pair->client.connection_salt,
                                                     NULL, key );
    check( bytes > 0 );
    if ( sequence == 0 )
        memset( out + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_SEQUENCE_BYTES );
    return bytes;
}

// ---------------------------------------------------------------------------
// the session matrix: one row per (end, type, outcome)
// ---------------------------------------------------------------------------

enum
{
    RCR_END_SERVER,
    RCR_END_CLIENT,
};

enum
{
    RCR_WRONG_SIZE,                 // one byte under the type's wire floor
    RCR_NO_MAPPING,                 // server: a source with no mapping. client: not in a session state
    RCR_NOT_CONNECTED,              // server: a mapping that holds no slot yet
    RCR_ZERO_SEQUENCE,
    RCR_ALREADY_RECEIVED,           // the same sequence delivered twice
    RCR_DID_NOT_OPEN,               // sealed under the wrong key
    RCR_BAD_ACK_BLOCK,
    RCR_WRONG_OPENED_SIZE,
    RCR_ACCEPTED,                   // the control: a valid packet takes the type's success outcome
};

struct rcr_row_t
{
    int end;
    uint8_t type;
    int defect;
    int received;                   // the type's RECEIVED counter at that end
    int outcome;                    // the one outcome the packet takes
    int extra;                      // a second counter the success outcome moves, or ROCKETNET_COUNTER_NONE
};

#define RCR_NONE ROCKETNET_COUNTER_NONE

static const rcr_row_t rcr_rows[] = {
    // server keep-alive
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_NOT_CONNECTED, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED, RCR_NONE },
    // server payload
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_NOT_CONNECTED, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_DELIVERED, RCR_NONE },
    // server disconnect
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_NOT_CONNECTED, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED, ROCKETNET_COUNTER_CLIENT_DISCONNECTED },
    // server ack
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_WRONG_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_NO_MAPPING, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_NOT_CONNECTED, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_NOT_CONNECTED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_ACK_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_ACCEPTED, RCR_NONE },
    // server fragment
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_NOT_CONNECTED, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_SERVER, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE, RCR_NONE },
    // client keep-alive
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_STATE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED, RCR_NONE },
    // client payload
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_PAYLOAD_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED, RCR_NONE },
    // client disconnect
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_STATE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_DISCONNECT_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED, RCR_NONE },
    // client ack
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_WRONG_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_WRONG_STATE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_WRONG_OPENED_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_ACK_PACKET, RCR_ACCEPTED, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED, RCR_NONE },
    // client fragment
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_SIZE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_NO_MAPPING, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_SEQUENCE, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_ALREADY_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_ALREADY_RECEIVED, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_DID_NOT_OPEN, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_BAD_ACK_BLOCK, RCR_NONE },
    { RCR_END_CLIENT, ROCKETNET_CONNECTION_FRAGMENT_PACKET, RCR_WRONG_OPENED_SIZE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_OPENED_SIZE, RCR_NONE },
};

static const char * rcr_defect_name( int defect )
{
    static const char * const names[] = { "wrong size", "no mapping / wrong state", "not connected", "zero sequence",
                                          "already received", "did not open", "bad ack block", "wrong opened size", "accepted" };
    return names[defect];
}

static void rcr_run_row( const rcr_row_t * row )
{
    const bool to_server = row->end == RCR_END_SERVER;
    const uint8_t type = row->type;

    int stage = RCR_STAGE_CONNECTED;
    if ( row->defect == RCR_NOT_CONNECTED )
        stage = RCR_STAGE_HALF_OPEN;
    else if ( row->defect == RCR_NO_MAPPING && !to_server )
        stage = RCR_STAGE_REQUESTING_COOKIE;

    static struct rcr_pair_t pair;
    rcr_pair_setup( &pair, stage );

    uint8_t plaintext[256];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int bytes = 0;
    struct rocketnet_address_t stranger;
    check( rocketnet_address_parse( &stranger, "198.51.100.99:4444" ) );

    // a prior valid keep-alive, for the replay row: delivered, accepted, and
    // its sequence reused by the packet under test
    const uint64_t sequence = pair.next_sequence++;
    if ( row->defect == RCR_ALREADY_RECEIVED )
    {
        const int prior_bytes = rcr_plaintext_valid( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, plaintext );
        bytes = rcr_seal( &pair, to_server, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence, plaintext, prior_bytes, NULL, packet );
        if ( to_server )
            check( rcr_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        else
            check( rcr_to_client( &pair, packet, bytes ) );
    }

    int plaintext_bytes = 0;
    const uint8_t * key_override = NULL;
    static const uint8_t wrong_key[32] = { 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
                                           0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42 };
    uint64_t seal_sequence = sequence;
    bool sealed = true;

    switch ( row->defect )
    {
        case RCR_WRONG_SIZE:
        case RCR_NO_MAPPING:
        {
            // a header and zeros: the wire floor is the only thing it satisfies
            const int length = rcr_floor( type ) - ( row->defect == RCR_WRONG_SIZE ? 1 : 0 );
            memset( packet, 0, sizeof( packet ) );
            uint8_t * p = packet;
            rocketnet_write_prefilter_header( &p, type, pair.client.packet_filter );
            bytes = length;
            sealed = false;
            break;
        }
        case RCR_ZERO_SEQUENCE:
            seal_sequence = 0;
            plaintext_bytes = rcr_plaintext_valid( type, plaintext );
            break;
        case RCR_DID_NOT_OPEN:
            key_override = wrong_key;
            plaintext_bytes = rcr_plaintext_valid( type, plaintext );
            break;
        case RCR_BAD_ACK_BLOCK:
            plaintext_bytes = rcr_plaintext_bad_block( type, plaintext );
            break;
        case RCR_WRONG_OPENED_SIZE:
            plaintext_bytes = rcr_plaintext_wrong_opened_size( type, plaintext );
            break;
        case RCR_NOT_CONNECTED:
        case RCR_ALREADY_RECEIVED:
        case RCR_ACCEPTED:
            plaintext_bytes = rcr_plaintext_valid( type, plaintext );
            break;
        default:
            check( 0 );
    }

    if ( sealed )
    {
        bytes = rcr_seal( &pair, to_server, type, seal_sequence, plaintext, plaintext_bytes, key_override, packet );
        check( bytes >= rcr_floor( type ) );
    }

    struct rocketnet_counters_t before = to_server ? pair.server_counters : pair.client_counters;
    const struct rocketnet_counters_t * after = to_server ? &pair.server_counters : &pair.client_counters;

    if ( to_server )
        rcr_to_server_from( &pair, row->defect == RCR_NO_MAPPING ? &stranger : &pair.client_address, packet, bytes );
    else
        rcr_to_client( &pair, packet, bytes );

    char report[16384];
    bool moved_as_named;
    if ( row->extra == RCR_NONE )
        moved_as_named = rocketnet_counters_expect_core( &before, after, { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { row->received, 1 }, { row->outcome, 1 } },
                                                         report, (int) sizeof( report ) );
    else
        moved_as_named = rocketnet_counters_expect_core( &before, after, { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { row->received, 1 }, { row->outcome, 1 }, { row->extra, 1 } },
                                                         report, (int) sizeof( report ) );
    if ( !moved_as_named )
        printf( "row: %s %s %s -> %s\n%s", to_server ? "server" : "client", rocketnet_counter_name( row->received ),
                rcr_defect_name( row->defect ), rocketnet_counter_name( row->outcome ), report );
    check( moved_as_named );

    rcr_pair_teardown( &pair );
}

static void test_client_and_server_session_receive_counters_move_exactly()
{
    for ( size_t i = 0; i < sizeof( rcr_rows ) / sizeof( rcr_rows[0] ); i++ )
    {
        rcr_run_row( &rcr_rows[i] );
    }
}

REGISTER_UNIT_TEST( test_client_and_server_session_receive_counters_move_exactly );

// ---------------------------------------------------------------------------
// the client's handshake ladder: cookie, challenge, denied
// ---------------------------------------------------------------------------

// the exact-delta check for one delivery, naming the case on failure

#define RCR_EXPECT( label, before, after, ... )                                                                   \
do                                                                                                                \
{                                                                                                                 \
    char rcr_report[16384];                                                                                       \
    const bool rcr_ok = rocketnet_counters_expect_core( (before), (after), __VA_ARGS__, rcr_report, (int) sizeof(rcr_report) ); \
    if ( !rcr_ok )                                                                                                \
        printf( "case: %s\n%s", (label), rcr_report );                                                            \
    check( rcr_ok );                                                                                              \
} while(0)

// a header and zeros of the packet's exact wire size: enough to pass the
// prefilter, nothing more

static int rcr_header_only( struct rcr_pair_t * pair, uint8_t type, int bytes, uint8_t * out )
{
    memset( out, 0, (size_t) bytes );
    uint8_t * p = out;
    rocketnet_write_prefilter_header( &p, type, pair->client.packet_filter );
    return bytes;
}

static void test_client_handshake_receive_counters_move_exactly()
{
    static struct rcr_pair_t pair;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_counters_t before;
    int bytes;

    // the cookie: stored in the requesting-cookie state, refused anywhere else

    rcr_pair_setup( &pair, RCR_STAGE_REQUESTING_COOKIE );
    {
        const uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES] = { 0 };
        rocketnet_write_connection_cookie_packet( packet, pair.client.packet_filter, pair.client.connection_salt, 0, mac );
        before = pair.client_counters;
        check( rcr_to_client( &pair, packet, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES ) );
        RCR_EXPECT( "cookie stored", &before, &pair.client_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_COOKIE_STORED, 1 } } );
        check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    }
    rcr_pair_teardown( &pair );

    rcr_pair_setup( &pair, RCR_STAGE_DISCONNECTED );
    {
        const uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES] = { 0 };
        rocketnet_write_connection_cookie_packet( packet, pair.client.packet_filter, pair.client.connection_salt, 0, mac );
        before = pair.client_counters;
        check( !rcr_to_client( &pair, packet, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES ) );
        RCR_EXPECT( "cookie to a client that is not connecting", &before, &pair.client_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE, 1 } } );

        // denied is also a handshake packet: to a client with no attempt it is wrong state
        bytes = rcr_header_only( &pair, ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES, packet );
        before = pair.client_counters;
        check( !rcr_to_client( &pair, packet, bytes ) );
        RCR_EXPECT( "denied to a client that is not connecting", &before, &pair.client_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_STATE, 1 } } );

        // wrong-size challenge and denied are refused before the state gate
        bytes = rcr_header_only( &pair, ROCKETNET_CONNECTION_CHALLENGE_PACKET, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES - 1, packet );
        before = pair.client_counters;
        check( !rcr_to_client( &pair, packet, bytes ) );
        RCR_EXPECT( "wrong-size challenge to a client that is not connecting", &before, &pair.client_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_SIZE, 1 } } );

        bytes = rcr_header_only( &pair, ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES - 1, packet );
        before = pair.client_counters;
        check( !rcr_to_client( &pair, packet, bytes ) );
        RCR_EXPECT( "wrong-size denied to a client that is not connecting", &before, &pair.client_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_SIZE, 1 } } );
    }
    rcr_pair_teardown( &pair );

    // the challenge and the denied while the request is out: the held
    // challenge is the server's own answer to the client's request
    rcr_pair_setup( &pair, RCR_STAGE_CHALLENGE_HELD );
    check( pair.held_challenge_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

    bytes = rcr_header_only( &pair, ROCKETNET_CONNECTION_CHALLENGE_PACKET, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES, packet );
    before = pair.client_counters;
    check( !rcr_to_client( &pair, packet, bytes ) );
    RCR_EXPECT( "challenge with a zero sequence", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_ZERO_SEQUENCE, 1 } } );

    bytes = rcr_header_only( &pair, ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES, packet );
    before = pair.client_counters;
    check( !rcr_to_client( &pair, packet, bytes ) );
    RCR_EXPECT( "denied with a zero sequence", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_DENIED_ZERO_SEQUENCE, 1 } } );

    // a challenge sealed correctly under the handshake keys, carrying a
    // small-order server ephemeral: the derivation refuses it
    {
        uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
        memset( session_nonce, 0x11, sizeof( session_nonce ) );
        struct rocketnet_connection_keys_t handshake;
        rocketnet_derive_handshake_keys( &handshake, pair.client.token.client_to_server_key, pair.client.token.server_to_client_key,
                                         pair.client.connection_salt, session_nonce );
        const uint8_t small_order_point[ROCKETNET_KEY_BYTES] = { 0 };
        bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET, pair.client.packet_filter, 1,
                                               session_nonce, small_order_point, ROCKETNET_KEY_BYTES,
                                               pair.client.signed_fields.protocol_id, pair.client.connection_salt,
                                               pair.client.client_ephemeral_public_key, handshake.handshake_server_to_client );
        check( bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
        before = pair.client_counters;
        check( !rcr_to_client( &pair, packet, bytes ) );
        RCR_EXPECT( "challenge carrying a small-order key", &before, &pair.client_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_SMALL_ORDER_KEY, 1 } } );
        check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    }

    // the real challenge is accepted, and its retransmission has no question
    // left to answer
    before = pair.client_counters;
    check( rcr_to_client( &pair, pair.held_challenge, pair.held_challenge_bytes ) );
    RCR_EXPECT( "challenge accepted", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_ACCEPTED, 1 } } );
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    before = pair.client_counters;
    check( !rcr_to_client( &pair, pair.held_challenge, pair.held_challenge_bytes ) );
    RCR_EXPECT( "challenge retransmitted after the answer", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_STATE, 1 } } );
    rcr_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_client_handshake_receive_counters_move_exactly );

// ---------------------------------------------------------------------------
// the client's fragment ladder: every reassembly refusal, from a server's
// crafted type 9 packets
// ---------------------------------------------------------------------------

// seals a type 9 packet from the server's side: the fresh block, the fragment
// header and the given number of body bytes

static int rcr_craft_fragment( struct rcr_pair_t * pair, uint32_t payload_id, int index, int count_minus_one,
                               int body_bytes, uint8_t * out )
{
    static uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_FRAGMENT_BYTES];
    check( body_bytes >= 1 && body_bytes <= ROCKETNET_MAX_FRAGMENT_BYTES );
    uint8_t * p = plaintext + rcr_block_fresh( plaintext );
    rocketnet_write_uint32( &p, payload_id );
    rocketnet_write_uint8( &p, (uint8_t) index );
    rocketnet_write_uint8( &p, (uint8_t) count_minus_one );
    memset( p, 0x77, (size_t) body_bytes );
    p += body_bytes;
    return rcr_seal( pair, 0, ROCKETNET_CONNECTION_FRAGMENT_PACKET, pair->next_sequence++, plaintext, (int) ( p - plaintext ), NULL, out );
}

static void test_client_fragment_receive_counters_move_exactly()
{
    static struct rcr_pair_t pair;
    static uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    static uint8_t packets[ROCKETNET_MAX_FRAGMENTS * ROCKETNET_SEND_BUDGET];
    int packet_bytes[ROCKETNET_MAX_FRAGMENTS];
    static uint8_t payload_data[3000];
    memset( payload_data, 0x5A, sizeof( payload_data ) );
    struct rocketnet_counters_t before;
    int bytes;

    rcr_pair_setup( &pair, RCR_STAGE_CONNECTED );

    // a fragment naming a refusable shape: the arrival and the one reason

    struct
    {
        const char * label;
        uint32_t payload_id;
        int index;
        int count_minus_one;
        int reason;
    } refusals[] = {
        { "zero payload id", 0, 0, 1, ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_PAYLOAD_ID },
        { "a count of one", 1, 0, 0, ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_INVALID },
        { "index at count", 1, 3, 2, ROCKETNET_COUNTER_CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE },
        { "index past count", 1, 200, 100, ROCKETNET_COUNTER_CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE },
    };
    for ( size_t i = 0; i < sizeof( refusals ) / sizeof( refusals[0] ); i++ )
    {
        bytes = rcr_craft_fragment( &pair, refusals[i].payload_id, refusals[i].index, refusals[i].count_minus_one, 1, packet );
        before = pair.client_counters;
        check( !rcr_to_client( &pair, packet, bytes ) );
        RCR_EXPECT( refusals[i].label, &before, &pair.client_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { refusals[i].reason, 1 } } );
    }

    // payload id 1 is the server's first fragmented send: its first fragment
    // lands and pins the count at 3 and the stride
    check( rocketnet_server_protocol_write_fragments( &pair.server, 0, payload_data, 3000, pair.time,
                                                      packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );
    before = pair.client_counters;
    check( rcr_to_client( &pair, packets, packet_bytes[0] ) );
    RCR_EXPECT( "first fragment of a real payload", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED, 1 } } );

    // the count-widening attack: count 4 against the pinned 3
    bytes = rcr_craft_fragment( &pair, 1, 1, 3, 1, packet );
    before = pair.client_counters;
    check( !rcr_to_client( &pair, packet, bytes ) );
    RCR_EXPECT( "count widened against the pin", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_MISMATCH, 1 } } );

    // a non-final fragment off the pinned stride
    bytes = rcr_craft_fragment( &pair, 1, 1, 2, 1, packet );
    before = pair.client_counters;
    check( !rcr_to_client( &pair, packet, bytes ) );
    RCR_EXPECT( "non-final fragment off the stride", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_SIZE_MISMATCH, 1 } } );

    // index 0 again, stride-sized (3000 bytes in three fragments is a stride
    // of 1000): the replay window never saw this packet, so the refusal is
    // reassembly's own
    bytes = rcr_craft_fragment( &pair, 1, 0, 2, 1000, packet );
    before = pair.client_counters;
    check( !rcr_to_client( &pair, packet, bytes ) );
    RCR_EXPECT( "duplicate fragment index", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_DUPLICATE_INDEX, 1 } } );

    // a stale payload id: ids 2 through 4 are sent and dropped, 5 lands in the
    // slot id 1 held (abandoning it), 6 through 8 are dropped, 9 takes the
    // slot from 5, and a crafted 5 is then older than the occupant
    for ( int spin = 2; spin <= 4; spin++ )
        check( rocketnet_server_protocol_write_fragments( &pair.server, 0, payload_data, 3000, pair.time,
                                                          packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );
    check( rocketnet_server_protocol_write_fragments( &pair.server, 0, payload_data, 3000, pair.time,
                                                      packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );     // id 5
    before = pair.client_counters;
    check( rcr_to_client( &pair, packets, packet_bytes[0] ) );
    RCR_EXPECT( "payload 5 evicts the incomplete payload 1", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED, 1 } } );

    for ( int spin = 6; spin <= 8; spin++ )
        check( rocketnet_server_protocol_write_fragments( &pair.server, 0, payload_data, 3000, pair.time,
                                                          packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );
    check( rocketnet_server_protocol_write_fragments( &pair.server, 0, payload_data, 3000, pair.time,
                                                      packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );     // id 9
    before = pair.client_counters;
    check( rcr_to_client( &pair, packets, packet_bytes[0] ) );
    RCR_EXPECT( "payload 9 evicts the incomplete payload 5", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED, 1 } } );

    bytes = rcr_craft_fragment( &pair, 5, 1, 2, 1, packet );
    before = pair.client_counters;
    check( !rcr_to_client( &pair, packet, bytes ) );
    RCR_EXPECT( "payload 5 behind the occupant 9", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE, 1 } } );

    // a full-stride first fragment claiming 256 fragments overruns the
    // configured payload maximum
    bytes = rcr_craft_fragment( &pair, 100, 0, 255, ROCKETNET_MAX_FRAGMENT_BYTES, packet );
    before = pair.client_counters;
    check( !rcr_to_client( &pair, packet, bytes ) );
    RCR_EXPECT( "payload larger than the configured maximum", &before, &pair.client_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, 1 }, { ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE, 1 } } );

    rcr_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_client_fragment_receive_counters_move_exactly );

// ---------------------------------------------------------------------------
// a fragment packet becomes ack-eligible only once reassembly accepts it
// (standard/TRANSPORT.md t15.3; tla/FragmentsAndAcks.tla RecvData,
// NamedOnlyIfAccepted): a fragment reassembly refuses is never named in a
// later ack block and adds no ack debt, at the client and at the server
// ---------------------------------------------------------------------------

// seals a type 9 packet toward either end: the fresh block, the fragment
// header and the given number of body bytes, on the next crafted sequence

static int rcr_craft_fragment_toward( struct rcr_pair_t * pair, int to_server, uint32_t payload_id, int index,
                                      int count_minus_one, int body_bytes, uint64_t * out_sequence, uint8_t * out )
{
    static uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_FRAGMENT_BYTES];
    check( body_bytes >= 1 && body_bytes <= ROCKETNET_MAX_FRAGMENT_BYTES );
    uint8_t * p = plaintext + rcr_block_fresh( plaintext );
    rocketnet_write_uint32( &p, payload_id );
    rocketnet_write_uint8( &p, (uint8_t) index );
    rocketnet_write_uint8( &p, (uint8_t) count_minus_one );
    memset( p, 0x77, (size_t) body_bytes );
    p += body_bytes;
    *out_sequence = pair->next_sequence++;
    return rcr_seal( pair, to_server, ROCKETNET_CONNECTION_FRAGMENT_PACKET, *out_sequence, plaintext, (int) ( p - plaintext ), NULL, out );
}

static void test_fragment_refused_by_reassembly_is_not_acknowledged()
{
    static struct rcr_pair_t pair;
    static uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint64_t sequence = 0;
    int bytes;

    rcr_pair_setup( &pair, RCR_STAGE_CONNECTED );

    // a payload id half the id space from the newest completed (none yet,
    // so 0): further than ROCKETNET_REASSEMBLY_REACH in both directions,
    // refused as stale with the slot empty

    const uint32_t stale_payload_id = 0x80000000u;

    // the client

    {
        uint64_t debt_before = pair.client.ack.ack_debt;
        uint64_t stale_before = pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE];
        bytes = rcr_craft_fragment_toward( &pair, 0, stale_payload_id, 0, 2, 1000, &sequence, packet );
        check( !rcr_to_client( &pair, packet, bytes ) );
        check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE] == stale_before + 1 );
        check( rocketnet_replay_already_received( &pair.client.accepted_session, sequence ) == 0 );
        check( pair.client.ack.ack_debt == debt_before );

        // the control: a fragment reassembly accepts is eligible and owed
        debt_before = pair.client.ack.ack_debt;
        bytes = rcr_craft_fragment_toward( &pair, 0, 1, 0, 2, 1000, &sequence, packet );
        check( rcr_to_client( &pair, packet, bytes ) );
        check( rocketnet_replay_already_received( &pair.client.accepted_session, sequence ) == 1 );
        check( pair.client.ack.ack_debt == debt_before + 1 );
    }

    // the server

    {
        const int mapping_index = rocketnet_client_slots_mapping_index( pair.server.client_slots, 0 );
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( pair.server.encryption_map, mapping_index );
        struct rocketnet_ack_endpoint_t * endpoint = &pair.server.acks->endpoints[0];

        uint64_t debt_before = endpoint->ack_debt;
        uint64_t stale_before = pair.server_counters.values[ROCKETNET_COUNTER_FRAGMENT_STALE];
        bytes = rcr_craft_fragment_toward( &pair, 1, stale_payload_id, 0, 2, 1000, &sequence, packet );
        check( rcr_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( pair.server_counters.values[ROCKETNET_COUNTER_FRAGMENT_STALE] == stale_before + 1 );
        check( rocketnet_replay_already_received( &mapping->accepted_session, sequence ) == 0 );
        check( endpoint->ack_debt == debt_before );

        debt_before = endpoint->ack_debt;
        bytes = rcr_craft_fragment_toward( &pair, 1, 1, 0, 2, 1000, &sequence, packet );
        check( rcr_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        check( rocketnet_replay_already_received( &mapping->accepted_session, sequence ) == 1 );
        check( endpoint->ack_debt == debt_before + 1 );
    }

    rcr_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_fragment_refused_by_reassembly_is_not_acknowledged );

// ---------------------------------------------------------------------------
// the server's response ladder, and the counters only a hooked server reaches
// ---------------------------------------------------------------------------

static void test_server_response_and_slot_counters_move_exactly()
{
    static struct rcr_pair_t pair;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_counters_t before;
    int bytes;

    // a response with a zero sequence, to a mapping whose handshake is in flight
    rcr_pair_setup( &pair, RCR_STAGE_HALF_OPEN );
    bytes = rcr_header_only( &pair, ROCKETNET_CONNECTION_RESPONSE_PACKET, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, packet );
    before = pair.server_counters;
    check( rcr_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    RCR_EXPECT( "response with a zero sequence", &before, &pair.server_counters,
                { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CONNECTION_RESPONSE_ZERO_SEQUENCE, 1 } } );

    // the same client id held by a connected client: this handshake is still
    // in flight (the client holds session keys and its response is built at
    // its next update) when another client with that id connects
    {
        struct rocketnet_client_protocol_t other;
        struct rocketnet_counters_t other_counters;
        rcr_client_init( &other, &other_counters );
        struct rocketnet_address_t other_address;
        check( rocketnet_address_parse( &other_address, "198.51.100.8:56789" ) );
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rcr_mint_token( token_data, 0x1400ULL );                    // the same client id, a different token
        check( rocketnet_client_protocol_connect( &other, token_data, pair.time, ROCKETNET_TEST_CREATE_TS ) );

        uint8_t out_packet[ROCKETNET_RECEIVE_CEILING];
        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;
        double time = pair.time;
        for ( ; time < pair.time + 10.0 && other.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; time += 0.05 )
        {
            int out_bytes = 0;
            rocketnet_client_protocol_update( &other, time, out_packet, &out_bytes );
            if ( out_bytes == 0 )
                continue;
            int response_bytes = 0;
            rocketnet_server_protocol_process_packet( &pair.server, &other_address, out_packet, out_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( &other, response, response_bytes, time + 0.01,
                                                          payload, (int) sizeof( payload ), &payload_bytes );
        }
        check( other.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        pair.time = time + 0.2;

        // now the first client's response arrives
        int out_bytes = 0;
        rocketnet_client_protocol_update( &pair.client, pair.time, out_packet, &out_bytes );
        check( out_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
        before = pair.server_counters;
        check( rcr_to_server( &pair, out_packet, out_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        RCR_EXPECT( "response from a client id already connected", &before, &pair.server_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED, 1 }, { ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED, 1 } } );

        rocketnet_counters_assert_conserved( &other_counters );
        rocketnet_reassembly_set_destroy( other.reassembly );
    }
    rcr_pair_teardown( &pair );

    // a slot that holds no mapping: the three send-side entries that look
    // the mapping up count the state and refuse
    rcr_pair_setup( &pair, RCR_STAGE_CONNECTED );
    {
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
        memset( user_data, 0, sizeof( user_data ) );
        const int slot = rocketnet_client_slots_assign( pair.server.client_slots, 0xBEEFULL, -1,
                                                        0xBEEFULL, user_data );
        check( slot != -1 );

        struct rocketnet_address_t to;
        uint64_t sequence = 0;
        uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
        before = pair.server_counters;
        check( rocketnet_server_protocol_client_address( &pair.server, slot, &to ) == 0 );
        check( rocketnet_server_protocol_start_payload( &pair.server, slot, &to, &sequence ) == 0 );
        check( rocketnet_server_write_keep_alive( &pair.server, slot, pair.time, keep_alive ) == 0 );
        RCR_EXPECT( "slot without a mapping", &before, &pair.server_counters,
                    { { ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING, 3 } } );
        rocketnet_client_slots_release( pair.server.client_slots, slot );
    }
    rcr_pair_teardown( &pair );

    // the delivery batch fills: three packets sent with packet ids, one ack
    // block acknowledging all three against a batch that holds two. the third
    // is deferred, never dropped
    rcr_pair_setup( &pair, RCR_STAGE_CONNECTED );
    {
        pair.server.acks->acked_capacity = 2;
        uint64_t first_sequence = 0;
        for ( int i = 0; i < 3; i++ )
        {
            struct rocketnet_address_t to;
            uint64_t sequence = 0;
            check( rocketnet_server_protocol_start_payload( &pair.server, 0, &to, &sequence ) );
            if ( i == 0 )
                first_sequence = sequence;
            alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE];
            memset( frame, 0xCD, sizeof( frame ) );
            check( rocketnet_server_protocol_seal_payload_frame( &pair.server, 0, sequence, (uint64_t) ( 100 + i ), frame, 4, pair.time ) > 0 );
        }
        uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES];
        uint8_t * p = plaintext;
        rocketnet_write_uint32( &p, (uint32_t) ( first_sequence + 2 ) );        // the largest sequence sent
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 3 );                                        // and the three below it
        bytes = rcr_seal( &pair, 1, ROCKETNET_CONNECTION_ACK_PACKET, pair.next_sequence++, plaintext, (int) sizeof( plaintext ), NULL, packet );
        before = pair.server_counters;
        check( rcr_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        RCR_EXPECT( "ack block wider than the batch", &before, &pair.server_counters,
                    { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 }, { ROCKETNET_COUNTER_ACK_RECEIVED, 1 }, { ROCKETNET_COUNTER_ACK_ACCEPTED, 1 }, { ROCKETNET_COUNTER_ACKED_BATCH_FULL, 1 } } );
        check( pair.server.acks->num_acked == 2 );
    }
    rcr_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_server_response_and_slot_counters_move_exactly );

// ---------------------------------------------------------------------------
// the evidence for the classes: a session that ends with packets in flight
// ---------------------------------------------------------------------------

// A connection that ends leaves the other end's packets in flight, and those
// packets arrive at an end that is gone from the session. That needs no
// network fault -- a perfect wire, one end disconnecting, the other still
// sending -- so the counters it moves are EXPECTED, never REFUSAL: the
// no-mapping and wrong-state outcomes of the session types, with the
// redundant disconnect burst's own copies. This test runs both orders on a
// perfect wire and asserts that no REFUSAL counter moved at either end while
// each of those EXPECTED counters did.

static void rcr_assert_no_refusals( const struct rocketnet_counters_t * counters )
{
    char report[4096];
    const int refusals = rocketnet_counters_refusals( counters, NULL, report, (int) sizeof( report ) );
    if ( refusals )
        printf( "%s", report );
    check( refusals == 0 );
}

static void test_session_end_races_move_only_expected_counters()
{
    static struct rcr_pair_t pair;
    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    static const uint8_t session_types[4] = { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                              ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_FRAGMENT_PACKET };
    static uint8_t in_flight[4][ROCKETNET_RECEIVE_CEILING];
    int in_flight_bytes[4];
    uint8_t plaintext[256];

    // the client ends the session with the server's packets in flight

    rcr_pair_setup( &pair, RCR_STAGE_CONNECTED );
    for ( int i = 0; i < 4; i++ )
    {
        const int plaintext_bytes = rcr_plaintext_valid( session_types[i], plaintext );
        in_flight_bytes[i] = rcr_seal( &pair, 0, session_types[i], pair.next_sequence++, plaintext, plaintext_bytes, NULL, in_flight[i] );
    }
    rocketnet_client_protocol_disconnect( &pair.client, burst, &burst_count, &burst_bytes );
    check( burst_count == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    for ( int i = 0; i < burst_count; i++ )
        check( rcr_to_server( &pair, burst + i * ROCKETNET_SEND_BUDGET, burst_bytes ) == ( i == 0 ? ROCKETNET_SERVER_PROCESS_ACCEPTED : ROCKETNET_SERVER_PROCESS_IGNORED ) );
    for ( int i = 0; i < 4; i++ )
        check( !rcr_to_client( &pair, in_flight[i], in_flight_bytes[i] ) );

    check( pair.server_counters.values[ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING] == (uint64_t) ( burst_count - 1 ) );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_STATE] == 1 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE] == 1 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_WRONG_STATE] == 1 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE] == 1 );
    rcr_assert_no_refusals( &pair.server_counters );
    rcr_assert_no_refusals( &pair.client_counters );
    rcr_pair_teardown( &pair );

    // the server ends the session with the client's packets in flight

    rcr_pair_setup( &pair, RCR_STAGE_CONNECTED );
    for ( int i = 0; i < 4; i++ )
    {
        const int plaintext_bytes = rcr_plaintext_valid( session_types[i], plaintext );
        in_flight_bytes[i] = rcr_seal( &pair, 1, session_types[i], pair.next_sequence++, plaintext, plaintext_bytes, NULL, in_flight[i] );
    }
    static uint8_t server_burst[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    rocketnet_server_protocol_disconnect_client( &pair.server, 0, server_burst, &burst_count, &burst_bytes );
    check( burst_count == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    for ( int i = 0; i < 4; i++ )
        check( rcr_to_server( &pair, in_flight[i], in_flight_bytes[i] ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    for ( int i = 0; i < burst_count; i++ )
        check( rcr_to_client( &pair, server_burst + i * ROCKETNET_SEND_BUDGET, burst_bytes ) == ( i == 0 ) );

    check( pair.server_counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING] == 1 );
    check( pair.server_counters.values[ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING] == 1 );
    check( pair.server_counters.values[ROCKETNET_COUNTER_ACK_NO_MAPPING] == 1 );
    check( pair.server_counters.values[ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING] == 1 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_STATE] == (uint64_t) ( burst_count - 1 ) );
    rcr_assert_no_refusals( &pair.server_counters );
    rcr_assert_no_refusals( &pair.client_counters );
    rcr_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_session_end_races_move_only_expected_counters );


// netcode.io's client: a payload is processed only once connected. The
// server's first payloads to a newly connected client can beat the
// keep-alive that connects it (the xdp server sent CONFIG and ARM ahead of
// it in 4 of 5 runs); delivered in sending connection response, the
// application replied before connected, and the reply was refused as
// SEND_NOT_CONNECTED and lost. A payload or fragment there is ignored,
// counted WRONG_STATE, and the server's reliable resend carries it after
// the keep-alive. The standalone ack keeps its carve-out.
static void test_client_ignores_payloads_before_connected()
{
    static struct rcr_pair_t pair;
    uint8_t plaintext[256];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    rcr_pair_setup( &pair, RCR_STAGE_HALF_OPEN );
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    int bytes = rcr_seal( &pair, 0, ROCKETNET_CONNECTION_PAYLOAD_PACKET, pair.next_sequence++, plaintext,
                          rcr_plaintext_valid( ROCKETNET_CONNECTION_PAYLOAD_PACKET, plaintext ), NULL, packet );
    check( !rcr_to_client( &pair, packet, bytes ) );
    bytes = rcr_seal( &pair, 0, ROCKETNET_CONNECTION_FRAGMENT_PACKET, pair.next_sequence++, plaintext,
                      rcr_plaintext_valid( ROCKETNET_CONNECTION_FRAGMENT_PACKET, plaintext ), NULL, packet );
    check( !rcr_to_client( &pair, packet, bytes ) );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE] == 1 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] == 0 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE] == 1 );

    bytes = rcr_seal( &pair, 0, ROCKETNET_CONNECTION_ACK_PACKET, pair.next_sequence++, plaintext,
                      rcr_plaintext_valid( ROCKETNET_CONNECTION_ACK_PACKET, plaintext ), NULL, packet );
    check( rcr_to_client( &pair, packet, bytes ) );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED] == 1 );
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    bytes = rcr_seal( &pair, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, pair.next_sequence++, plaintext,
                      rcr_plaintext_valid( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, plaintext ), NULL, packet );
    check( rcr_to_client( &pair, packet, bytes ) );
    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    bytes = rcr_seal( &pair, 0, ROCKETNET_CONNECTION_PAYLOAD_PACKET, pair.next_sequence++, plaintext,
                      rcr_plaintext_valid( ROCKETNET_CONNECTION_PAYLOAD_PACKET, plaintext ), NULL, packet );
    check( rcr_to_client( &pair, packet, bytes ) );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] == 1 );
    check( pair.client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE] == 1 );

    rcr_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_client_ignores_payloads_before_connected );
