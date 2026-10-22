/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The packet size limits. One number -- the link MTU -- fixes every other
// limit by derivation (STANDARD.md _Packet sizes_): the receive ceiling is
// the IPv6 UDP payload of a 1500-byte link, equal to the send budget, the
// payload capacity the budget less the fixed overhead and the minimum ack
// block rounded down to eight, and the SINGLE message the capacity less its
// form byte. A game packet of 1400 bytes travels as one unfragmented type 5
// packet, zero copy, on either address family.
//
// Every case runs a client and server protocol pair in memory on a mocked
// clock: no sockets, no real time.

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
#include "rocketnet_message.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"
#include "wycheproof.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

// the conservation assert, checked by the registry's check
#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// the largest application packet represented here: baseline, delta, explosion
// and message writers fill to this many bytes

#define MAX_PAYLOAD_TEST_GAME_PACKET_BYTES      1400

// ---------------------------------------------------------------------------
// the in-memory pair: a server protocol on the test vector keys and a client
// connected to it through the three-round-trip handshake on mocked time
// ---------------------------------------------------------------------------

struct max_payload_pair_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    double time;
};

static void max_payload_mint_token( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
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
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = 0x1400ULL;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void max_payload_pair_setup( struct max_payload_pair_t * pair )
{
    memset( pair, 0, sizeof(*pair) );

    struct rocketnet_server_protocol_t * server = &pair->server;
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
    max_payload_mint_token( token_data );
    check( rocketnet_client_protocol_connect( &pair->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );

    static uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    static uint8_t response[ROCKETNET_RECEIVE_CEILING];
    static uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0, response_bytes = 0, payload_bytes = 0, payload_client_index = -1;

    const double times[3] = { 1000.0, 1000.2, 1000.4 };
    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_client_protocol_update( &pair->client, times[i], packet, &packet_bytes );
        check( packet_bytes > 0 );
        check( rocketnet_server_protocol_process_packet( server, &pair->client_address, packet, packet_bytes,
                                                         ROCKETNET_TEST_CREATE_TS, times[i],
                                                         response, &response_bytes,
                                                         payload, &payload_bytes, &payload_client_index ) != ROCKETNET_SERVER_PROCESS_IGNORED );
        check( rocketnet_client_protocol_process_packet( &pair->client, response, response_bytes, times[i] + 0.05,
                                                         payload, (int) sizeof(payload), &payload_bytes ) );
    }
    check( pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( pair->client.client_index == 0 );
    pair->time = 1001.0;
}

static void max_payload_pair_teardown( struct max_payload_pair_t * pair )
{
    rocketnet_counters_assert_conserved( &pair->server_counters );
    rocketnet_counters_assert_conserved( &pair->client_counters );
    rocketnet_reassembly_set_destroy( pair->client.reassembly );
    rocketnet_encryption_map_destroy( pair->server.encryption_map );
    rocketnet_used_token_history_destroy( pair->server.used_token_history );
    rocketnet_client_slots_destroy( pair->server.client_slots );
    rocketnet_server_acks_destroy( pair->server.acks );
    rocketnet_reassembly_set_destroy( pair->server.reassembly );
}

// hands one client packet to the server; the opened payload lands in out
static int max_payload_to_server( struct max_payload_pair_t * pair, const uint8_t * packet, int packet_bytes,
                                  uint8_t * out, int * out_bytes )
{
    static uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0, client_index = -1;
    *out_bytes = 0;
    pair->time += 0.001;
    return rocketnet_server_protocol_process_packet( &pair->server, &pair->client_address, packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, pair->time,
                                                     response, &response_bytes, out, out_bytes, &client_index );
}

static uint64_t max_payload_fragment_counters( const struct rocketnet_counters_t * counters )
{
    uint64_t sum = 0;
    for ( int c = ROCKETNET_COUNTER_FRAGMENT_RECEIVED; c <= ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL; c++ )
        sum += counters->values[c];
    sum += counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED];
    return sum;
}

static void max_payload_fill( uint8_t * data, int bytes, int salt )
{
    for ( int i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( i * 29 + salt );
}

// a send frame, the shape every transport hands out: 16-aligned root, wire
// packet at the wire offset, payload at the payload offset

alignas( 16 ) static uint8_t g_frame[ROCKETNET_FRAME_STRIDE];
static uint8_t g_received[ROCKETNET_RECEIVE_PAYLOAD_BYTES];

// seals the payload already written at the frame's payload offset as one
// type 5 packet, delivers it, and checks it opened whole on the far side
static void max_payload_seal_and_deliver( struct max_payload_pair_t * pair, int payload_bytes )
{
    uint8_t * wire_start = g_frame + ROCKETNET_FRAME_WIRE_OFFSET;
    uint64_t sequence = 0;
    check( rocketnet_client_protocol_start_payload( &pair->client, &sequence ) );
    const int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &pair->client, sequence, sequence,
                                                                         wire_start, payload_bytes, pair->time );
    check( wire_bytes > 0 );
    check( wire_bytes <= ROCKETNET_SEND_BUDGET );
    check( wire_start[0] == ROCKETNET_CONNECTION_PAYLOAD_PACKET );

    // the sealed copy is decrypted in place by the receive path; keep the
    // plaintext to compare against
    static uint8_t sent[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    int received_bytes = 0;
    static uint8_t wire_copy[ROCKETNET_RECEIVE_CEILING];
    memcpy( wire_copy, wire_start, (size_t) wire_bytes );
    check( max_payload_to_server( pair, wire_copy, wire_bytes, g_received, &received_bytes ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    check( received_bytes == payload_bytes );
    (void) sent;
}

// ---------------------------------------------------------------------------
// A 1400-byte message is one unfragmented type 5 packet on each channel kind
// the game uses: unreliable (the SINGLE form, written in place at the payload
// root -- zero copy), acked-unreliable (reliability 5, which travels as wire
// code 0) and reliable ordered (code 3). Nothing fragments.
// ---------------------------------------------------------------------------

static void test_message_of_1400_bytes_is_one_unfragmented_packet()
{
    static struct max_payload_pair_t pair;
    max_payload_pair_setup( &pair );

    static uint8_t message[MAX_PAYLOAD_TEST_GAME_PACKET_BYTES];
    uint8_t * root = g_frame + ROCKETNET_FRAME_PAYLOAD_OFFSET;
    check( ( (uintptr_t) root & 15 ) == 0 );

    // 1. unreliable, SINGLE form: the pointer handed back is the wire frame's
    // payload root, so the game writes straight into the packet

    {
        memset( g_frame, 0, sizeof(g_frame) );
        max_payload_fill( message, (int) sizeof(message), 1 );
        memcpy( root, message, sizeof(message) );                                   // the message at the payload root
        root[sizeof(message)] = ROCKETNET_MESSAGE_FORM_SINGLE;                      // the form byte last
        const int length = (int) sizeof(message) + 1;
        max_payload_seal_and_deliver( &pair, length );
        check( memcmp( g_received, message, sizeof(message) ) == 0 );
        check( g_received[length - 1] == ROCKETNET_MESSAGE_FORM_SINGLE );
    }

    // 2. acked-unreliable (reliability 5): a code 0 record in a RECORDS payload

    {
        memset( g_frame, 0, sizeof(g_frame) );
        max_payload_fill( message, (int) sizeof(message), 5 );
        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, root, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        struct rocketnet_message_record_t record;
        memset( &record, 0, sizeof(record) );
        record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        record.data = message;
        record.bytes = (int) sizeof(message);
        check( rocketnet_message_writer_add( &writer, &record ) );
        const int length = rocketnet_message_writer_finish( &writer );
        check( length > 0 && length <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        max_payload_seal_and_deliver( &pair, length );

        struct rocketnet_message_limits_t limits;
        limits.num_channels = 32;
        limits.enabled_codes = rocketnet_message_enabled_codes( 1u << 5 );
        limits.reliable_messages_in_flight = 1024;
        limits.max_message_bytes = 32768;
        limits.receive_message_buffer_bytes = 65536;
        struct rocketnet_message_reader_t reader;
        check( rocketnet_message_reader_begin( &reader, g_received, length, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        struct rocketnet_message_record_t read;
        check( rocketnet_message_reader_next( &reader, &read ) );
        check( read.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE && read.bytes == (int) sizeof(message) );
        check( memcmp( read.data, message, sizeof(message) ) == 0 );
        check( !rocketnet_message_reader_next( &reader, &read ) );
    }

    // 3. reliable ordered (code 3): reliable records may only ride a payload
    // at or under the wire capacity (STANDARD.md _Messages_ check 9), so the
    // whole 1400-byte message must fit in one unfragmented packet

    {
        memset( g_frame, 0, sizeof(g_frame) );
        max_payload_fill( message, (int) sizeof(message), 3 );
        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, root, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        struct rocketnet_message_record_t record;
        memset( &record, 0, sizeof(record) );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.number = 1;
        record.ring = 0;
        record.ordering = 1;
        record.channel = 2;
        record.data = message;
        record.bytes = (int) sizeof(message);
        check( rocketnet_message_writer_add( &writer, &record ) );
        const int length = rocketnet_message_writer_finish( &writer );
        check( length > 0 && length <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        max_payload_seal_and_deliver( &pair, length );

        struct rocketnet_message_limits_t limits;
        limits.num_channels = 32;
        limits.enabled_codes = rocketnet_message_enabled_codes( 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
        limits.reliable_messages_in_flight = 1024;
        limits.max_message_bytes = 32768;
        limits.receive_message_buffer_bytes = 65536;
        struct rocketnet_message_reader_t reader;
        check( rocketnet_message_reader_begin( &reader, g_received, length, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        struct rocketnet_message_record_t read;
        check( rocketnet_message_reader_next( &reader, &read ) );
        check( read.code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED && read.bytes == (int) sizeof(message) );
        check( memcmp( read.data, message, sizeof(message) ) == 0 );
    }

    // three messages, three type 5 packets, no fragment anywhere

    check( max_payload_fragment_counters( &pair.client_counters ) == 0 );
    check( max_payload_fragment_counters( &pair.server_counters ) == 0 );
    check( pair.server_counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == 0 );

    max_payload_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// The limits are derivations of the link MTU, checked here at run time as
// the headers check them at compile time.
// ---------------------------------------------------------------------------

static void test_limits_are_derived_from_the_packet_size()
{
    const int mtu = 1500, ipv6 = 40, udp = 8;

    check( ROCKETNET_RECEIVE_PACKET_BYTES == mtu - ipv6 - udp );                 // 1452
    check( ROCKETNET_MAX_PACKET_BYTES == mtu - ipv6 - udp );            // 1452
    check( ROCKETNET_RECEIVE_CEILING == ROCKETNET_RECEIVE_PACKET_BYTES );
    check( ROCKETNET_SEND_BUDGET == ROCKETNET_MAX_PACKET_BYTES );
    check( ROCKETNET_SEND_BUDGET == ROCKETNET_RECEIVE_CEILING );

    const int room = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES;
    check( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES == room - room % 8 );                 // 1416
    check( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES % 8 == 0 );
    check( ROCKETNET_RECEIVE_PAYLOAD_BYTES ==
           ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );   // 1416
    check( ROCKETNET_MESSAGE_SINGLE_BYTES == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 );                                  // 1415
    check( ROCKETNET_MESSAGE_SINGLE_BYTES >= MAX_PAYLOAD_TEST_GAME_PACKET_BYTES );
    check( ROCKETNET_MAX_FRAGMENT_BYTES == room - ROCKETNET_FRAGMENT_HEADER_BYTES );                             // 1413
    check( ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES == ROCKETNET_MAX_FRAGMENT_BYTES ); // 1413

    // the config table's floor for max_message_bytes is the SINGLE size
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );
    config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES - 1;
    check( rocketnet_message_validate_config( &config ) == 0 );

    // the frame holds the slack and a full receive-ceiling packet, 16-aligned;
    // the payload offset is the wire offset, the sealed header and the minimum
    // block, 16-aligned
    check( ROCKETNET_FRAME_STRIDE % 16 == 0 );
    check( ROCKETNET_FRAME_STRIDE >= ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING );
    check( ROCKETNET_FRAME_STRIDE - 16 < ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING );
    check( ROCKETNET_FRAME_PAYLOAD_OFFSET == ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_PREFILTER_BYTES +
                                             ROCKETNET_SEQUENCE_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES );
    check( ROCKETNET_FRAME_SIZE >= ROCKETNET_FRAME_PAYLOAD_OFFSET + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + ROCKETNET_PACKET_TAG_BYTES );
    check( ROCKETNET_FRAME_SIZE >= ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING );
}

// ---------------------------------------------------------------------------
// A maximum payload seals at or under the send budget however long the loss
// history is: the seal paths give the application's bytes first and cut the
// ack block to the room left, never below the 7-byte minimum. Both directions
// open on the far side.
// ---------------------------------------------------------------------------

static void test_sealed_packet_at_max_size_opens()
{
    static struct max_payload_pair_t pair;
    max_payload_pair_setup( &pair );

    // a long, gappy receive history on both ends, so a full block would be
    // hundreds of bytes
    for ( uint64_t s = 1000; s <= 3000; s++ )
    {
        if ( s % 3 != 0 )
        {
            rocketnet_replay_advance( &pair.client.replay_session, s );
        }
    }
    struct rocketnet_encryption_mapping_t * mapping = NULL;
    {
        int iterator = -1;
        while ( rocketnet_encryption_map_next( pair.server.encryption_map, &iterator ) )
        {
            mapping = rocketnet_encryption_map_entry( pair.server.encryption_map, iterator );
            if ( mapping->control->client_index == 0 )
                break;
        }
    }
    check( mapping && mapping->control->client_index == 0 );

    // a block with these ranges is far larger than the room a max payload leaves
    {
        static uint8_t block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
        check( rocketnet_ack_block_build( block, ROCKETNET_ACK_BLOCK_MAX_BYTES, &pair.client.replay_session ) > 100 );
    }

    // client to server, through the zero-copy frame seal
    {
        memset( g_frame, 0, sizeof(g_frame) );
        uint8_t * root = g_frame + ROCKETNET_FRAME_PAYLOAD_OFFSET;
        max_payload_fill( root, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, 9 );
        static uint8_t expect[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        memcpy( expect, root, sizeof(expect) );

        uint8_t * wire_start = g_frame + ROCKETNET_FRAME_WIRE_OFFSET;
        uint64_t sequence = 0;
        check( rocketnet_client_protocol_start_payload( &pair.client, &sequence ) );
        const int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &pair.client, sequence, sequence, wire_start,
                                                                             ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, pair.time );
        check( wire_bytes > 0 && wire_bytes <= ROCKETNET_SEND_BUDGET );
        // the block took what the room allows in whole 4-byte entries, never
        // less than the minimum
        check( wire_bytes >= ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        check( ROCKETNET_SEND_BUDGET - wire_bytes < 4 );
        int received = 0;
        check( max_payload_to_server( &pair, wire_start, wire_bytes, g_received, &received ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        check( received == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES && memcmp( g_received, expect, sizeof(expect) ) == 0 );
    }

    // client to server, the copying path
    {
        static uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        max_payload_fill( payload, (int) sizeof(payload), 11 );
        static uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        const int wire_bytes = rocketnet_client_protocol_write_payload( &pair.client, payload, (int) sizeof(payload), pair.time, packet );
        check( wire_bytes > 0 && wire_bytes <= ROCKETNET_SEND_BUDGET );
        int received = 0;
        check( max_payload_to_server( &pair, packet, wire_bytes, g_received, &received ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        check( received == (int) sizeof(payload) && memcmp( g_received, payload, sizeof(payload) ) == 0 );
    }

    // server to client, under the server's own long history
    for ( uint64_t s = 1000; s <= 3000; s++ )
    {
        if ( s % 5 != 0 )
        {
            rocketnet_replay_advance( &mapping->replay_session, s );
        }
    }
    {
        static uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        max_payload_fill( payload, (int) sizeof(payload), 13 );
        static uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        const int wire_bytes = rocketnet_server_protocol_write_payload( &pair.server, 0, payload, (int) sizeof(payload), pair.time, packet );
        check( wire_bytes > 0 && wire_bytes <= ROCKETNET_SEND_BUDGET );
        int received = 0;
        check( rocketnet_client_protocol_process_packet( &pair.client, packet, wire_bytes, pair.time,
                                                         g_received, (int) sizeof(g_received), &received ) );
        check( received == (int) sizeof(payload) && memcmp( g_received, payload, sizeof(payload) ) == 0 );
    }

    // one byte over the capacity is refused at the seal, not truncated
    {
        static uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1];
        static uint8_t packet[ROCKETNET_RECEIVE_CEILING + 64];
        check( rocketnet_client_protocol_write_payload( &pair.client, payload, (int) sizeof(payload), pair.time, packet ) == 0 );
    }

    check( max_payload_fragment_counters( &pair.client_counters ) == 0 );
    check( max_payload_fragment_counters( &pair.server_counters ) == 0 );

    max_payload_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// Both endpoints accept the sender's bounds and reject the first byte over.
// A full datagram uses the unrounded fragment stride; type 5 is capped at
// the separately rounded payload capacity, including when the datagram fits.
// ---------------------------------------------------------------------------

static void test_receive_ceiling_refuses_one_byte_over()
{
    static struct max_payload_pair_t pair;
    max_payload_pair_setup( &pair );
    static uint8_t plaintext[ROCKETNET_SEND_BUDGET + 1];
    static uint8_t packet[ROCKETNET_SEND_BUDGET + 1];
    static uint8_t output[ROCKETNET_SEND_BUDGET];

    for ( int direction = 0; direction < 2; direction++ )
    {
        // A legal maximum fragment fills the entire datagram, even though
        // its type data (header + body) is larger than the type-5 capacity.
        memset( plaintext, 0, sizeof( plaintext ) );
        rocketnet_write_fragment_header( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, 1, 0, 2 );
        max_payload_fill( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES,
                          ROCKETNET_MAX_FRAGMENT_BYTES, 17 );
        const uint8_t * key = direction == 0 ? pair.client.keys.session_client_to_server : pair.client.keys.session_server_to_client;
        const int wire_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
            pair.client.packet_filter, 100, NULL, plaintext, ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES,
            ROCKETNET_TEST_PROTOCOL_ID, pair.client.connection_salt, NULL, key );
        check( wire_bytes == ROCKETNET_SEND_BUDGET );
        int received = 0;
        if ( direction == 0 )
            check( max_payload_to_server( &pair, packet, wire_bytes, output, &received ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        else
            check( rocketnet_client_protocol_process_packet( &pair.client, packet, wire_bytes, pair.time,
                                                             output, (int) sizeof( output ), &received ) );
        check( received == 0 ); // incomplete reassembly, not a payload delivery

        packet[wire_bytes] = 0;
        struct rocketnet_counters_t * counters = direction == 0 ? &pair.server_counters : &pair.client_counters;
        const uint64_t too_large = counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];
        if ( direction == 0 )
            check( max_payload_to_server( &pair, packet, wire_bytes + 1, output, &received ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        else
            check( !rocketnet_client_protocol_process_packet( &pair.client, packet, wire_bytes + 1, pair.time,
                                                              output, (int) sizeof( output ), &received ) );
        check( received == 0 );
        check( counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large + 1 );
    }
    max_payload_pair_teardown( &pair );
}

static void test_receive_payload_refuses_above_send_capacity()
{
    static struct max_payload_pair_t pair;
    max_payload_pair_setup( &pair );
    static uint8_t plaintext[ROCKETNET_SEND_BUDGET];
    static uint8_t packet[ROCKETNET_SEND_BUDGET];
    static uint8_t output[ROCKETNET_SEND_BUDGET];
    const int payload_sizes[] = { ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1,
        ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES };
    for ( int direction = 0; direction < 2; direction++ )
    {
        for ( int i = 0; i < 3; i++ )
        {
            memset( plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
            max_payload_fill( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, payload_sizes[i], 23 );
            const uint8_t * key = direction == 0 ? pair.client.keys.session_client_to_server : pair.client.keys.session_server_to_client;
            const int wire_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                pair.client.packet_filter, (uint64_t) ( 100 + i ), NULL, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + payload_sizes[i],
                ROCKETNET_TEST_PROTOCOL_ID, pair.client.connection_salt, NULL, key );
            check( wire_bytes <= ROCKETNET_SEND_BUDGET );
            struct rocketnet_counters_t * counters = direction == 0 ? &pair.server_counters : &pair.client_counters;
            const int reason = direction == 0 ? ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE : ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE;
            const uint64_t wrong_size = counters->values[reason];
            int received = 0;
            const int accepted = direction == 0 ?
                max_payload_to_server( &pair, packet, wire_bytes, output, &received ) == ROCKETNET_SERVER_PROCESS_ACCEPTED :
                rocketnet_client_protocol_process_packet( &pair.client, packet, wire_bytes, pair.time,
                                                          output, (int) sizeof( output ), &received );
            check( accepted == ( i == 0 ) );
            check( received == ( i == 0 ? payload_sizes[i] : 0 ) );
            check( counters->values[reason] == wrong_size + ( i == 0 ? 0u : 1u ) );
            if ( i == 0 )
                check( memcmp( output, plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, (size_t) received ) == 0 );
        }
    }
    max_payload_pair_teardown( &pair );
}

// ---------------------------------------------------------------------------
// The client drops reserved packet types and server-bound types, counted.
// The client's two refusals live in rocketnet_client_protocol_process_packet
// (sdk/rocketnet_client_protocol.cpp): RESERVED_TYPE for a type above
// ROCKETNET_CONNECTION_FRAGMENT_PACKET, WRONG_DIRECTION for a request or a
// response. Each refusal also counts the packet's arrival.
// ---------------------------------------------------------------------------

static uint64_t client_all_counters_sum( const struct rocketnet_counters_t * counters )
{
    uint64_t sum = 0;
    for ( int c = 0; c < ROCKETNET_NUM_COUNTERS; c++ )
        sum += counters->values[c];
    return sum;
}

static void test_client_drops_reserved_and_wrong_direction_types()
{
    static struct max_payload_pair_t pair;
    max_payload_pair_setup( &pair );

    static uint8_t out[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    static uint8_t packet[64];
    int out_bytes = 0;

    const uint64_t packets_before = pair.client_counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];

    // types 10 through 255 are reserved and ignored: PACKETS_RECEIVED and
    // RESERVED_TYPE each rise by exactly one and nothing else moves, so the
    // counter sum rises by exactly two
    static const uint8_t reserved_type[3] = { 10, 200, 255 };
    for ( int i = 0; i < 3; i++ )
    {
        const uint64_t before = client_all_counters_sum( &pair.client_counters );
        memset( packet, 0, sizeof(packet) );
        uint8_t * p = packet;
        rocketnet_write_prefilter_header( &p, reserved_type[i], pair.client.packet_filter );
        check( !rocketnet_client_protocol_process_packet( &pair.client, packet, 64, pair.time,
                                                           out, (int) sizeof(out), &out_bytes ) );
        check( client_all_counters_sum( &pair.client_counters ) == before + 2 );
        check( pair.client_counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == packets_before + 1 + (uint64_t) i );
        check( pair.client_counters.values[ROCKETNET_COUNTER_RESERVED_TYPE] == (uint64_t) ( i + 1 ) );
    }

    // a request or a response is the wrong direction for a client: the same
    // arrival count and WRONG_DIRECTION each rise by exactly one
    static const uint8_t server_bound_type[2] = { ROCKETNET_CONNECTION_REQUEST_PACKET,
                                                  ROCKETNET_CONNECTION_RESPONSE_PACKET };
    for ( int i = 0; i < 2; i++ )
    {
        const uint64_t before = client_all_counters_sum( &pair.client_counters );
        memset( packet, 0, sizeof(packet) );
        uint8_t * p = packet;
        rocketnet_write_prefilter_header( &p, server_bound_type[i], pair.client.packet_filter );
        check( !rocketnet_client_protocol_process_packet( &pair.client, packet, 64, pair.time,
                                                           out, (int) sizeof(out), &out_bytes ) );
        check( client_all_counters_sum( &pair.client_counters ) == before + 2 );
        check( pair.client_counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == packets_before + 4 + (uint64_t) i );
        check( pair.client_counters.values[ROCKETNET_COUNTER_WRONG_DIRECTION] == (uint64_t) ( i + 1 ) );
    }

    check( pair.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    max_payload_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_message_of_1400_bytes_is_one_unfragmented_packet );
REGISTER_UNIT_TEST( test_limits_are_derived_from_the_packet_size );
REGISTER_UNIT_TEST( test_sealed_packet_at_max_size_opens );
REGISTER_UNIT_TEST( test_receive_ceiling_refuses_one_byte_over );
REGISTER_UNIT_TEST( test_receive_payload_refuses_above_send_capacity );
REGISTER_UNIT_TEST( test_client_drops_reserved_and_wrong_direction_types );

// ---------------------------------------------------------------------------
// rocketnet-wycheproof-chacha20-poly1305: every ChaCha20-Poly1305 vector in
// tests/wycheproof/chacha20_poly1305_test.json through the vendored sodium the
// SDK links, and every valid vector through rocketnet_write_sealed_packet and
// rocketnet_open_sealed_packet (sdk/rocketnet_protocol.cpp), the two functions
// that seal and open every session packet with
// crypto_aead_chacha20poly1305_ietf_encrypt/decrypt.
//
// The witness plants one bug -- a decrypt that returns success without
// comparing the tag -- and requires the 60 modified-tag vectors to go red.
// With a system libsodium these tests are compiled out: they are a statement
// about the code rocketnet ships.
// ---------------------------------------------------------------------------

#if !defined(ROCKETNET_USE_SYSTEM_SODIUM)

#include "../sodium/sodium.h"

struct wp_aead_tallies_t
{
    int opened;
    int refused_open;
    int nonce_size;
};

static bool wp_aead_field( const wycheproof_test_t & test, const char * name, std::vector<uint8_t> & out )
{
    std::map<std::string, std::string>::const_iterator it = test.fields.find( name );
    if ( it == test.fields.end() )
        return false;
    return wycheproof_hex( it->second, out );
}

static void wp_aead_report( const wycheproof_test_t & test, const char * outcome )
{
    printf( "wycheproof chacha20_poly1305_test.json tcId %d %s %s flags",
            test.tc_id, test.result.c_str(), outcome );
    for ( size_t i = 0; i < test.flags.size(); i++ )
        printf( " %s", test.flags[i].c_str() );
    printf( "\n" );
}

// Returns the number of vectors whose outcome disagrees with Wycheproof and
// fills the tally per outcome name. On a disagreement it prints the file,
// tcId, result, outcome and flags -- never a key byte. witness_ok, when not
// null, is cleared by any disagreeing vector that is not an invalid vector
// with a 12-byte iv, the exact set a skipped tag comparison opens.
static int wp_aead_run( bool skip_tag_check, struct wp_aead_tallies_t * tallies, bool * witness_ok )
{
    std::vector<wycheproof_test_t> tests;
    int number_of_tests = 0, number_of_groups = 0;
    check( wycheproof_load( "chacha20_poly1305_test.json", tests, &number_of_tests, &number_of_groups ) );
    check( number_of_tests == 325 );

    tallies->opened = 0;
    tallies->refused_open = 0;
    tallies->nonce_size = 0;
    if ( witness_ok )
        *witness_ok = true;

    int disagreements = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        const wycheproof_test_t & test = tests[i];
        std::vector<uint8_t> key, iv, aad, msg, ct, tag;
        check( wp_aead_field( test, "key", key ) );
        check( wp_aead_field( test, "iv", iv ) );
        check( wp_aead_field( test, "aad", aad ) );
        check( wp_aead_field( test, "msg", msg ) );
        check( wp_aead_field( test, "ct", ct ) );
        check( wp_aead_field( test, "tag", tag ) );
        check( key.size() == 32 );

        const bool valid = test.result == "valid";
        bool accepted = false;
        const char * outcome = NULL;

        if ( iv.size() != 12 )
        {
            // rocketnet_sequence_nonce builds 12 bytes, so the sdk never
            // offers another nonce size; the runner refuses it itself.
            check( !valid );
            tallies->nonce_size++;
            outcome = "nonce_size";
        }
        else
        {
            check( tag.size() == 16 );
            std::vector<uint8_t> sealed_input( ct );
            sealed_input.insert( sealed_input.end(), tag.begin(), tag.end() );
            std::vector<uint8_t> opened( msg.size() + 16 );
            unsigned long long opened_bytes = 0;
            const int rc = crypto_aead_chacha20poly1305_ietf_decrypt( opened.data(), &opened_bytes, NULL,
                                                                      sealed_input.data(), (unsigned long long) sealed_input.size(),
                                                                      aad.data(), (unsigned long long) aad.size(),
                                                                      iv.data(), key.data() );
            if ( rc == 0 )
            {
                tallies->opened++;
                outcome = "opened";
                accepted = true;
            }
            else
            {
                tallies->refused_open++;
                outcome = "refused_open";
            }

            if ( skip_tag_check )
            {
                // the planted bug: a decrypt that never compares the tag
                // opens whatever it was handed
                if ( !accepted )
                {
                    tallies->refused_open--;
                    tallies->opened++;
                }
                outcome = "opened";
                accepted = true;
            }

            if ( valid )
            {
                std::vector<uint8_t> expected( ct );
                expected.insert( expected.end(), tag.begin(), tag.end() );
                std::vector<uint8_t> encrypted( msg.size() + 16 );
                unsigned long long encrypted_bytes = 0;
                check( crypto_aead_chacha20poly1305_ietf_encrypt( encrypted.data(), &encrypted_bytes,
                                                                  msg.data(), (unsigned long long) msg.size(),
                                                                  aad.data(), (unsigned long long) aad.size(),
                                                                  NULL, iv.data(), key.data() ) == 0 );
                const bool exact_output = rc == 0 && opened_bytes == msg.size() &&
                                          ( msg.empty() || memcmp( opened.data(), msg.data(), msg.size() ) == 0 );
                const bool exact_seal = encrypted_bytes == expected.size() &&
                                        ( expected.empty() || memcmp( encrypted.data(), expected.data(), expected.size() ) == 0 );
                if ( !exact_output || !exact_seal )
                {
                    disagreements++;
                    wp_aead_report( test, outcome );
                    if ( witness_ok )
                        *witness_ok = false;
                    continue;
                }
            }
        }

        if ( valid != accepted )
        {
            disagreements++;
            wp_aead_report( test, outcome );
            if ( witness_ok && !( !valid && iv.size() == 12 ) )
                *witness_ok = false;
        }
    }

    return disagreements;
}

static void test_wycheproof_chacha20_poly1305_vectors()
{
    // Every vector through the vendored sodium the SDK links. Valid vectors
    // open with the exact plaintext and re-seal to ct||tag; invalid vectors
    // are refused, the 9 other nonce sizes by the runner's own check.
    struct wp_aead_tallies_t tallies;
    check( wp_aead_run( false, &tallies, NULL ) == 0 );
    check( tallies.opened == 256 );
    check( tallies.refused_open == 60 );
    check( tallies.nonce_size == 9 );

    // Every valid vector of 1 to 1024 message bytes through the sdk wrapper:
    // seal and open with the vector key as the session key, then refuse a
    // flipped bit in each of the 16 tag bytes and in the first sealed byte.
    std::vector<wycheproof_test_t> tests;
    int number_of_tests = 0, number_of_groups = 0;
    check( wycheproof_load( "chacha20_poly1305_test.json", tests, &number_of_tests, &number_of_groups ) );

    static uint8_t packet[2048];
    static uint8_t out[2048];
    int sdk_vectors = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        const wycheproof_test_t & test = tests[i];
        if ( test.result != "valid" )
            continue;
        std::vector<uint8_t> key, msg;
        check( wp_aead_field( test, "key", key ) );
        check( wp_aead_field( test, "msg", msg ) );
        if ( msg.size() < 1 || msg.size() > 1024 )
            continue;

        const uint64_t sequence = 1000 + (uint64_t) sdk_vectors;
        sdk_vectors++;

        const int packet_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                                rocketnet_test_filter_value, sequence, NULL,
                                                                msg.data(), (int) msg.size(),
                                                                ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                                key.data() );
        check( packet_bytes > 0 );
        check( packet_bytes <= (int) sizeof(packet) );

        const int opened_bytes = rocketnet_open_sealed_packet( out, NULL, (int) sizeof(out),
                                                               packet, packet_bytes,
                                                               ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                                               ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                               key.data() );
        check( opened_bytes == (int) msg.size() );
        check( memcmp( out, msg.data(), msg.size() ) == 0 );

        const int tag_offset = packet_bytes - ROCKETNET_PACKET_TAG_BYTES;
        for ( int t = 0; t < ROCKETNET_PACKET_TAG_BYTES; t++ )
        {
            packet[tag_offset + t] ^= 1;
            check( rocketnet_open_sealed_packet( out, NULL, (int) sizeof(out),
                                                 packet, packet_bytes,
                                                 ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                                 ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                 key.data() ) == -1 );
            packet[tag_offset + t] ^= 1;
        }

        const int first_sealed = ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES;
        packet[first_sealed] ^= 1;
        check( rocketnet_open_sealed_packet( out, NULL, (int) sizeof(out),
                                             packet, packet_bytes,
                                             ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                             ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                             key.data() ) == -1 );
        packet[first_sealed] ^= 1;

        // restored, the same packet opens again
        check( rocketnet_open_sealed_packet( out, NULL, (int) sizeof(out),
                                             packet, packet_bytes,
                                             ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                             ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                             key.data() ) == (int) msg.size() );
    }
    check( sdk_vectors == 254 );
}

static void test_wycheproof_chacha20_poly1305_witness_skipped_tag_goes_red()
{
    // The planted bug removes exactly the tag comparison. The 60 invalid
    // vectors with a 12-byte iv are the only ones it opens, so the runner
    // returns exactly 60 and every disagreement is one of them.
    struct wp_aead_tallies_t tallies;
    bool witness_ok = false;
    check( wp_aead_run( true, &tallies, &witness_ok ) == 60 );
    check( witness_ok );
}

REGISTER_UNIT_TEST( test_wycheproof_chacha20_poly1305_vectors );
REGISTER_UNIT_TEST( test_wycheproof_chacha20_poly1305_witness_skipped_tag_goes_red );

#endif // !defined(ROCKETNET_USE_SYSTEM_SODIUM)
