/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_link_mtu_probe.cpp -- the path probe after connect
// (standard/TRANSPORT.md t8.13, t11.4, t12 rules 15 and 16;
// proofs/tla/RNPathProbe.tla; the pacing in sdk/rocketnet_link_mtu.h).
//
// One end at a time: the server answers a probe or a confirm only from a
// connected client, at its own size and header, one of each kind opened per
// mapping per update, and raises the slot's packet size only on a confirm,
// never lowering it; the response's packet size field must be on its list.
// The client: a one-entry list never probes and builds today's response; a
// longer list connects at its smallest size P0 and probes upward after
// connect.
//
// Then both ends together, a real client protocol and a real server
// protocol across a path of the test's own: a capacity each way, a one-way
// delay, and loss by a seeded generator. The times to the largest size at
// 0%, 10%, 50% for the first 10 s and 100% for the first 10 s, the strike
// spacing that rides out a loss burst of large packets, the -14 of a P0 the
// path does not carry, and the end of probing for good.
//
// No socket, no thread, no clock: both ends are driven through their protocol
// calls with the time passed in, from the published test values. Set
// ROCKETNET_LINK_MTU_PRINT_TIMES to print the measured times.

#include "test_unit_standard.h"
#include "portable_crt.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_keys.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_link_mtu.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

static const int probe_three_step[3] = { 1492, 1200, 576 };

// a probe, a confirm, or a reply of either, of wire_bytes: the 8-byte probe
// header (kind, the size field, the pair) and zero padding (one padding byte
// set when nonzero_padding), sealed under key on sequence

static int probe_write( uint8_t * out, int wire_bytes, int kind, int size_field, uint32_t pair, uint64_t sequence,
                        const uint8_t * salt, const uint8_t * key, bool nonzero_padding )
{
    uint8_t plaintext[ROCKETNET_CONNECTION_PROBE_MAX_WIRE_BYTES] = { 0 };
    uint8_t * p = plaintext;
    rocketnet_write_uint8( &p, (uint8_t) kind );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint16( &p, (uint16_t) size_field );
    rocketnet_write_uint32( &p, pair );
    if ( nonzero_padding )
    {
        plaintext[ROCKETNET_PROBE_HEADER_BYTES] = 1;
    }
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_PROBE_PACKET, filter, sequence, NULL,
                                          plaintext, wire_bytes - ROCKETNET_PACKET_OVERHEAD_BYTES,
                                          ROCKETNET_TEST_PROTOCOL_ID, salt, NULL, key );
}

// ---------------------------------------------------------------------------
// the server, one end

struct probe_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_encryption_mapping_t * mapping;
    double time;
};

// a server on link_mtus (none: the default list), with one client that has
// sent its request and holds the challenge: a mapping, no slot

static void probe_server_setup( struct probe_server_t * test, const int * link_mtus, int num_link_mtus )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots );
    if ( num_link_mtus > 0 )
    {
        memcpy( server->link_mtus, link_mtus, sizeof( int ) * (size_t) num_link_mtus );
        server->num_link_mtus = num_link_mtus;
    }

    test->time = 1000.0;
    rocketnet_server_protocol_update( server, test->time );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );

    // the request, from the published token and cookie

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t advertised[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, advertised, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields, rocketnet_test_backend_secret_key,
                                                        rocketnet_test_token_vector_ephemeral_secret[0], token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_write_connection_request_packet( request, rocketnet_test_filter_value, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public, token.signed_connect_data, token.signature );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    check( rocketnet_generate_cookie_mac( cookie_mac, &test->client_address, rocketnet_test_salt, cookie_expire, rocketnet_test_cookie_key ) );
    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int challenge_bytes = 0;
    check( rocketnet_server_protocol_process_connection_request( server, &test->client_address, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 ROCKETNET_TEST_CREATE_TS, test->time,
                                                                 challenge, &challenge_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    const int mapping_index = rocketnet_encryption_map_find( server->encryption_map, &test->client_address );
    check( mapping_index != -1 );
    test->mapping = rocketnet_encryption_map_entry( server->encryption_map, mapping_index );
}

static void probe_server_teardown( struct probe_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
}

// the next update: one more probe of each kind may be opened per mapping

static void probe_server_next_update( struct probe_server_t * test )
{
    test->time += 0.01;
    rocketnet_server_protocol_update( &test->server, test->time );
}

static int probe_server_receive( struct probe_server_t * test, const struct rocketnet_address_t * from,
                                 const uint8_t * packet, int packet_bytes, uint8_t * reply, int * reply_bytes )
{
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    *reply_bytes = 0;
    return rocketnet_server_protocol_process_packet( &test->server, from, packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, test->time,
                                                     reply, reply_bytes, payload, &payload_bytes, &payload_client_index );
}

// the client's probe (kind 0, its own size in the size field) of wire_bytes,
// sealed as a client would seal it

static int probe_server_client_probe( struct probe_server_t * test, uint8_t * out, int wire_bytes, uint32_t pair, uint64_t sequence, bool nonzero_padding )
{
    return probe_write( out, wire_bytes, ROCKETNET_PROBE_KIND_PROBE, wire_bytes, pair, sequence, test->mapping->connection_salt,
                        test->mapping->keys.session_client_to_server, nonzero_padding );
}

// the client's confirm of packet_bytes

static int probe_server_client_confirm( struct probe_server_t * test, uint8_t * out, int packet_bytes, uint64_t sequence )
{
    return probe_write( out, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, ROCKETNET_PROBE_KIND_CONFIRM, packet_bytes, 0, sequence,
                        test->mapping->connection_salt, test->mapping->keys.session_client_to_server, false );
}

// the client's response, with field in its packet size field

static int probe_server_client_response( struct probe_server_t * test, uint8_t * out, uint64_t sequence, int field )
{
    uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    padding[0] = (uint8_t) ( field & 0xFF );
    padding[1] = (uint8_t) ( field >> 8 );
    return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_RESPONSE_PACKET, rocketnet_test_filter_value, sequence, NULL,
                                          padding, (int) sizeof( padding ), ROCKETNET_TEST_PROTOCOL_ID,
                                          test->mapping->connection_salt, test->mapping->client_ephemeral_public_key,
                                          test->mapping->keys.handshake_client_to_server );
}

// the client connects at field: its response takes a slot

static void probe_server_connect( struct probe_server_t * test, int field )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;
    check( probe_server_client_response( test, response, 1, field ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( probe_server_receive( test, &test->client_address, response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    check( test->mapping->control->client_index != -1 );
}

// the reply opened under the session server-to-client key: its kind, size
// field and pair, false when it does not open or its header is malformed

static bool probe_server_open_reply( struct probe_server_t * test, const uint8_t * reply, int reply_bytes, int * kind, int * size, uint32_t * pair )
{
    const uint8_t * p = reply + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_read_uint32( &p );
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), reply, reply_bytes,
                                                           ROCKETNET_CONNECTION_PROBE_PACKET, sequence,
                                                           ROCKETNET_TEST_PROTOCOL_ID, test->mapping->connection_salt, NULL,
                                                           test->mapping->keys.session_server_to_client );
    return opened_bytes >= 0 && rocketnet_probe_header_read( opened, opened_bytes, reply_bytes, kind, size, pair );
}

// A connected client's probes are answered at their own size with their own
// header: a small probe and a large one in the same update both, a second of
// either kind withheld unopened, and the next update answers again (planted:
// one allowance shared by both kinds, the large probe of the pair is
// withheld).

static void test_server_probe_answered_at_its_size_once_per_kind_per_update()
{
    struct probe_server_t test;
    probe_server_setup( &test, probe_three_step, 3 );
    probe_server_connect( &test, 548 );

    uint8_t probe[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;
    int kind = -1;
    int size = 0;
    uint32_t pair = 0;

    check( probe_server_client_probe( &test, probe, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, 7, 2, false ) == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
    check( probe_server_receive( &test, &test.client_address, probe, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
    check( reply_bytes == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
    check( probe_server_open_reply( &test, reply, reply_bytes, &kind, &size, &pair ) );
    check( kind == ROCKETNET_PROBE_KIND_PROBE && size == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES && pair == 7 );

    check( probe_server_client_probe( &test, probe, 1452, 7, 3, false ) == 1452 );
    check( probe_server_receive( &test, &test.client_address, probe, 1452, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
    check( reply_bytes == 1452 );
    check( reply[0] == ROCKETNET_CONNECTION_PROBE_PACKET );
    check( probe_server_open_reply( &test, reply, reply_bytes, &kind, &size, &pair ) );
    check( kind == ROCKETNET_PROBE_KIND_PROBE && size == 1452 && pair == 7 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 2 );

    // a probe answers nothing about the slot's size: only a confirm raises it

    check( rocketnet_server_protocol_client_packet_bytes( &test.server, test.mapping->control->client_index ) == 548 );

    // the same update: a second of each kind is withheld, unopened, even one
    // that would not open

    check( probe_server_client_probe( &test, probe, 1172, 8, 4, false ) == 1172 );
    check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( reply_bytes == 0 );
    probe[ROCKETNET_PREFILTER_BYTES + 6] ^= 1;
    check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( probe_server_client_confirm( &test, probe, 1452, 5 ) == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
    check( probe_server_receive( &test, &test.client_address, probe, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_WITHHELD] == 3 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_DID_NOT_OPEN] == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 2 );

    // the next update answers again

    probe_server_next_update( &test );
    check( probe_server_client_probe( &test, probe, 1172, 8, 6, false ) == 1172 );
    check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
    check( reply_bytes == 1172 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 3 );

    probe_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_probe_answered_at_its_size_once_per_kind_per_update );

// Each refusal of a probe, with its own counter and nothing answered: a size
// neither small nor on the list (on the default list, every large size below
// the ceiling), an address with no mapping, a client not yet connected, a
// probe that does not open (an integrity failure on the mapping), a malformed
// header, nonzero padding (planted: the connected check removed, the
// half-open client's probe is answered).

static void test_server_probe_refusals()
{
    uint8_t probe[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;

    {
        struct probe_server_t test;
        probe_server_setup( &test, probe_three_step, 3 );

        // half-open: a client probes only once connected

        check( probe_server_client_probe( &test, probe, 1172, 1, 2, false ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_NOT_CONNECTED] == 1 );

        probe_server_connect( &test, 548 );

        // 1000 bytes: no entry of the list gives it

        check( probe_server_client_probe( &test, probe, 1000, 1, 3, false ) == 1000 );
        check( probe_server_receive( &test, &test.client_address, probe, 1000, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_SIZE_NOT_ALLOWED] == 1 );

        // an address the server holds no mapping for

        struct rocketnet_address_t stranger;
        check( rocketnet_address_parse( &stranger, "198.51.100.8:56789" ) );
        check( probe_server_client_probe( &test, probe, 1172, 1, 3, false ) == 1172 );
        check( probe_server_receive( &test, &stranger, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_NO_MAPPING] == 1 );

        // sealed under the handshake key, not the session key: an integrity
        // failure

        check( probe_write( probe, 1172, ROCKETNET_PROBE_KIND_PROBE, 1172, 1, 4, test.mapping->connection_salt,
                            test.mapping->keys.handshake_client_to_server, false ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_DID_NOT_OPEN] == 1 );
        check( test.mapping->integrity_failures == 1 );

        // opened: a size field that is not its own size, an unknown kind,
        // nonzero padding, a confirm that is not small -- each refused, none a
        // forgery

        const struct
        {
            int wire;
            int kind;
            int size;
            uint32_t pair;
            bool nonzero;
        } malformed[] = {
            { 1172, ROCKETNET_PROBE_KIND_PROBE, 1452, 1, false },
            { 1172, 2, 1172, 1, false },
            { 1172, ROCKETNET_PROBE_KIND_PROBE, 1172, 1, true },
            { 1172, ROCKETNET_PROBE_KIND_CONFIRM, 1172, 0, false },
            { ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, ROCKETNET_PROBE_KIND_CONFIRM, 1452, 9, false },
        };
        for ( size_t i = 0; i < sizeof( malformed ) / sizeof( malformed[0] ); i++ )
        {
            probe_server_next_update( &test );
            check( probe_write( probe, malformed[i].wire, malformed[i].kind, malformed[i].size, malformed[i].pair, 5 + i,
                                test.mapping->connection_salt, test.mapping->keys.session_client_to_server, malformed[i].nonzero ) == malformed[i].wire );
            check( probe_server_receive( &test, &test.client_address, probe, malformed[i].wire, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
            check( reply_bytes == 0 );
        }
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_PADDING_NOT_ZERO] == 5 );
        check( test.mapping->integrity_failures == 1 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 0 );
        check( rocketnet_server_protocol_client_packet_bytes( &test.server, test.mapping->control->client_index ) == 548 );

        probe_server_teardown( &test );
    }

    {
        // the default list gives the ceiling alone: a large probe is off it

        struct probe_server_t test;
        probe_server_setup( &test, NULL, 0 );
        probe_server_connect( &test, 0 );
        check( probe_server_client_probe( &test, probe, 1172, 1, 2, false ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_SIZE_NOT_ALLOWED] == 1 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 0 );
        probe_server_teardown( &test );
    }
}

REGISTER_UNIT_TEST( test_server_probe_refusals );

// The server raises a slot's packet size only on a confirm of a size on its
// list above the slot's, and never lowers it: a confirm below, at, or off the
// list changes nothing; every confirm on the list is echoed so the client
// stops resending (planted: a confirm sets the size whatever it is, and the
// confirm of 548 lowers the slot to 548).

static void test_server_raises_only_on_a_confirm_and_never_lowers()
{
    struct probe_server_t test;
    probe_server_setup( &test, probe_three_step, 3 );
    probe_server_connect( &test, 548 );
    const int client_index = test.mapping->control->client_index;

    uint8_t confirm[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;
    int kind = -1;
    int size = 0;
    uint32_t pair = 0;

    // off the list: refused, not echoed

    check( probe_server_client_confirm( &test, confirm, 1000, 2 ) == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
    check( probe_server_receive( &test, &test.client_address, confirm, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_SIZE_NOT_ALLOWED] == 1 );
    check( rocketnet_server_protocol_client_packet_bytes( &test.server, client_index ) == 548 );

    // 1452: raised, echoed

    probe_server_next_update( &test );
    check( probe_server_client_confirm( &test, confirm, 1452, 3 ) == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
    check( probe_server_receive( &test, &test.client_address, confirm, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
    check( probe_server_open_reply( &test, reply, reply_bytes, &kind, &size, &pair ) );
    check( kind == ROCKETNET_PROBE_KIND_CONFIRM && size == 1452 && pair == 0 );
    check( rocketnet_server_protocol_client_packet_bytes( &test.server, client_index ) == 1452 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_CONFIRMED] == 1 );

    // 548 and 1172 after it: echoed, the size stays

    const int lower[2] = { 548, 1172 };
    for ( int i = 0; i < 2; i++ )
    {
        probe_server_next_update( &test );
        check( probe_server_client_confirm( &test, confirm, lower[i], (uint64_t) ( 4 + i ) ) == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
        check( probe_server_receive( &test, &test.client_address, confirm, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
        check( rocketnet_server_protocol_client_packet_bytes( &test.server, client_index ) == 1452 );
    }
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_CONFIRMED] == 1 );

    probe_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_raises_only_on_a_confirm_and_never_lowers );

// The response's packet size field: a size off the server's list is refused
// with nothing connected; one on the list connects and is the slot's packet
// size; zero is the ceiling (planted: the list check of the field removed,
// 1000 connects).

static void test_server_response_packet_size_field()
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;

    {
        struct probe_server_t test;
        probe_server_setup( &test, probe_three_step, 3 );

        check( probe_server_client_response( &test, response, 2, 1000 ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
        check( probe_server_receive( &test, &test.client_address, response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_SIZE_NOT_ALLOWED] == 1 );
        check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 0 );
        check( test.mapping->integrity_failures == 0 );

        check( probe_server_client_response( &test, response, 3, 548 ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
        check( probe_server_receive( &test, &test.client_address, response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
        check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
        check( rocketnet_server_protocol_client_packet_bytes( &test.server, test.mapping->control->client_index ) == 548 );

        probe_server_teardown( &test );
    }

    {
        struct probe_server_t test;
        probe_server_setup( &test, NULL, 0 );

        // the default list: 1172 is off it, zero is the ceiling

        check( probe_server_client_response( &test, response, 2, 1172 ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
        check( probe_server_receive( &test, &test.client_address, response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_SIZE_NOT_ALLOWED] == 1 );

        check( probe_server_client_response( &test, response, 3, 0 ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
        check( probe_server_receive( &test, &test.client_address, response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
        check( rocketnet_server_protocol_client_packet_bytes( &test.server, test.mapping->control->client_index ) == ROCKETNET_MAX_PACKET_BYTES );

        probe_server_teardown( &test );
    }
}

REGISTER_UNIT_TEST( test_server_response_packet_size_field );

// A probe forged at the client's address, which does not open under the
// session key, spends nothing: the genuine probe of the same update is still
// answered, every update, however many forgeries arrive first (standard/
// TRANSPORT.md t12 rule 15; proofs/tla/RNPathProbe.tla ForgedNeverStarves).
// Planted: the allowance spent before the open, the genuine probe is withheld.

static void test_server_forged_probe_does_not_starve_the_client()
{
    struct probe_server_t test;
    probe_server_setup( &test, probe_three_step, 3 );
    probe_server_connect( &test, 548 );

    uint8_t forged[ROCKETNET_RECEIVE_CEILING];
    uint8_t probe[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;

    for ( int update = 0; update < 3; update++ )
    {
        // a forger knows the address, the size and the sequence, not the key
        for ( int i = 0; i < 4; i++ )
        {
            uint8_t wrong_key[32];
            memset( wrong_key, 0x5A + i, sizeof( wrong_key ) );
            check( probe_write( forged, 1172, ROCKETNET_PROBE_KIND_PROBE, 1172, 1, (uint64_t) ( 2 + update ), test.mapping->connection_salt, wrong_key, false ) == 1172 );
            check( probe_server_receive( &test, &test.client_address, forged, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
            check( reply_bytes == 0 );
        }

        check( probe_server_client_probe( &test, probe, 1172, 1, (uint64_t) ( 2 + update ), false ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
        check( reply_bytes == 1172 );

        probe_server_next_update( &test );
    }

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_DID_NOT_OPEN] == 12 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_WITHHELD] == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 3 );

    probe_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_forged_probe_does_not_starve_the_client );

// The half-open mapping's handshake deadline is the configured one on every
// list: the probe runs after connect, so nothing extends the handshake
// (planted: a probing list adds time to the deadline).

static void test_server_handshake_deadline_is_the_configured_one()
{
    const int longest[8] = { 1500, 1400, 1300, 1200, 1000, 800, 700, 576 };
    struct probe_server_t test;
    probe_server_setup( &test, longest, 8 );
    check( test.mapping->control->handshake_deadline == test.time + 5.0 );
    probe_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_handshake_deadline_is_the_configured_one );

// ---------------------------------------------------------------------------
// the client, one end, to the challenge

struct probe_client_t
{
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    double time;
};

// a client on link_mtus (none: the default list) that has just opened the
// challenge, the published handshake of t11.client-transcript

static void probe_client_setup( struct probe_client_t * test, const int * link_mtus, int num_link_mtus )
{
    memset( test, 0, sizeof( *test ) );
    rocketnet_client_protocol_init( &test->client, &test->counters );
    if ( num_link_mtus > 0 )
    {
        memcpy( test->client.link_mtus, link_mtus, sizeof( int ) * (size_t) num_link_mtus );
        test->client.num_link_mtus = num_link_mtus;
    }

    check( rocketnet_client_protocol_connect( &test->client, rocketnet_test_frozen_token, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
    memcpy( test->client.connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( test->client.client_ephemeral_secret_key, rocketnet_test_client_eph_secret, ROCKETNET_KEY_BYTES );
    memcpy( test->client.client_ephemeral_public_key, rocketnet_test_client_eph_public, ROCKETNET_KEY_BYTES );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int packet_bytes = 0;

    rocketnet_client_protocol_update( &test->client, 1000.0, packet, &packet_bytes );
    check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( cookie, tag, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_mac );
    check( rocketnet_client_protocol_process_packet( &test->client, cookie, sizeof( cookie ), 1000.05, payload, sizeof( payload ), &payload_bytes ) == 1 );

    rocketnet_client_protocol_update( &test->client, 1000.1, packet, &packet_bytes );
    check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    struct rocketnet_connection_keys_t handshake_keys;
    rocketnet_derive_handshake_keys( &handshake_keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, rocketnet_test_session_nonce );
    uint8_t challenge[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    check( rocketnet_write_sealed_packet( challenge, ROCKETNET_CONNECTION_CHALLENGE_PACKET, tag, 1, rocketnet_test_session_nonce,
                                          rocketnet_test_server_eph_public, ROCKETNET_KEY_BYTES, ROCKETNET_TEST_PROTOCOL_ID,
                                          rocketnet_test_salt, rocketnet_test_client_eph_public,
                                          handshake_keys.handshake_server_to_client ) == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    check( rocketnet_client_protocol_process_packet( &test->client, challenge, sizeof( challenge ), 1000.15, payload, sizeof( payload ), &payload_bytes ) == 1 );
    check( test->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    test->time = 1000.15;
}

// the client's next send, at most a second of 10 ms steps (zero bytes when
// none went out)

static int probe_client_next_send( struct probe_client_t * test, uint8_t * packet )
{
    for ( int i = 0; i < 100; i++ )
    {
        int packet_bytes = 0;
        test->time += 0.01;
        rocketnet_client_protocol_update( &test->client, test->time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
        {
            return packet_bytes;
        }
    }
    return 0;
}

// the packet size field of the client's stored response

static int probe_client_response_field( struct probe_client_t * test )
{
    const uint8_t * p = test->client.stored_response + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_read_uint32( &p );
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), test->client.stored_response,
                                                           ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, ROCKETNET_CONNECTION_RESPONSE_PACKET,
                                                           sequence, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                           rocketnet_test_client_eph_public, test->client.keys.handshake_client_to_server );
    check( opened_bytes == ROCKETNET_RESPONSE_PADDING_BYTES );
    for ( int i = ROCKETNET_RESPONSE_PACKET_BYTES_BYTES; i < opened_bytes; i++ )
    {
        check( opened[i] == 0 );
    }
    return opened[0] | ( opened[1] << 8 );
}

// Every list joins as the default does: the first send after the challenge
// is the response, never a probe. On the default list it is byte for byte the
// response of a client that never set the list, its size field zero; a
// one-entry list below the ceiling writes its size; the three-step list
// writes its smallest size P0, 548, and sends no probe before connect
// (planted: the three-step list writes its largest size, 1452, and its
// response carries zero).

static void test_client_joins_at_its_smallest_size_with_no_probe()
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    struct probe_client_t unset;
    probe_client_setup( &unset, NULL, 0 );
    check( probe_client_next_send( &unset, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

    const int default_list[1] = { 1500 };
    struct probe_client_t one;
    probe_client_setup( &one, default_list, 1 );
    check( probe_client_next_send( &one, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( packet[0] == ROCKETNET_CONNECTION_RESPONSE_PACKET );
    check( memcmp( one.client.stored_response, unset.client.stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES ) == 0 );
    check( probe_client_response_field( &one ) == 0 );
    check( rocketnet_client_protocol_packet_bytes( &one.client ) == ROCKETNET_MAX_PACKET_BYTES );

    const int small_list[1] = { 1200 };
    struct probe_client_t small;
    probe_client_setup( &small, small_list, 1 );
    check( probe_client_next_send( &small, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( probe_client_response_field( &small ) == 1172 );
    check( rocketnet_client_protocol_packet_bytes( &small.client ) == 1172 );

    struct probe_client_t three;
    probe_client_setup( &three, probe_three_step, 3 );
    check( probe_client_next_send( &three, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( packet[0] == ROCKETNET_CONNECTION_RESPONSE_PACKET );
    check( probe_client_response_field( &three ) == 548 );
    check( rocketnet_client_protocol_packet_bytes( &three.client ) == 548 );
    check( !three.client.probing );

    check( unset.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 0 );
    check( one.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 0 );
    check( small.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 0 );
    check( three.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 0 );
}

REGISTER_UNIT_TEST( test_client_joins_at_its_smallest_size_with_no_probe );

// An entry whose packet size, after the family's headers and the 1452
// ceiling, equals the entry before it is not probed again for that family
// (standard/TRANSPORT.md t11.4): over IPv4 { 1500, 1492 } is the one size
// 1452, so the client never probes and its response is byte for byte the
// default list's; over IPv6 the two entries are 1452 and 1444, two sizes.
// Planted: equal sizes kept, the IPv4 client has two candidates.

static void test_link_mtu_equal_sizes_probed_once()
{
    const int pppoe[2] = { 1500, 1492 };
    int candidates[ROCKETNET_LINK_MTU_MAX_ENTRIES];

    check( rocketnet_link_mtu_candidates( pppoe, 2, ROCKETNET_ADDRESS_IPV4, candidates ) == 1 );
    check( candidates[0] == 1452 );
    check( rocketnet_link_mtu_candidates( pppoe, 2, ROCKETNET_ADDRESS_IPV6, candidates ) == 2 );
    check( candidates[0] == 1452 && candidates[1] == 1444 );

    const int four[4] = { 1500, 1492, 1200, 576 };
    check( rocketnet_link_mtu_candidates( four, 4, ROCKETNET_ADDRESS_IPV4, candidates ) == 3 );
    check( candidates[0] == 1452 && candidates[1] == 1172 && candidates[2] == 548 );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    struct probe_client_t unset;
    probe_client_setup( &unset, NULL, 0 );
    check( probe_client_next_send( &unset, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

    struct probe_client_t client;
    probe_client_setup( &client, pppoe, 2 );
    check( client.client.num_probe_candidates == 1 );
    check( probe_client_next_send( &client, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( memcmp( client.client.stored_response, unset.client.stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES ) == 0 );
    check( rocketnet_client_protocol_packet_bytes( &client.client ) == ROCKETNET_MAX_PACKET_BYTES );
}

REGISTER_UNIT_TEST( test_link_mtu_equal_sizes_probed_once );

// ---------------------------------------------------------------------------
// both ends across a path

#define PATH_STEP_SECONDS 0.01
#define PATH_START_TIME 100.0

struct path_packet_t
{
    double arrive;
    std::vector<uint8_t> bytes;
};

struct path_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_address_t client_address;
    std::vector<path_packet_t> up;
    std::vector<path_packet_t> down;
    double now;
    double connect_time;            // when the client connected
    int client_index;
    uint64_t random;

    // the path: each direction's capacity, the one-way delay; loss of every
    // packet with probability loss_percent while now - connect_time is below
    // loss_seconds and after_percent after; packets above large_bytes lost
    // while now - connect_time is below large_loss_seconds; the client's
    // confirms dropped while drop_confirms
    int cap_up;
    int cap_down;
    double delay;
    int loss_percent;
    double loss_seconds;
    int after_percent;
    int large_bytes;
    double large_loss_seconds;
    bool drop_confirms;

    // the times, from connect, at which each end first reached the target
    double client_reached;
    double server_reached;
};

static uint32_t path_next_random( path_t * path )
{
    uint64_t x = path->random;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    path->random = x;
    return (uint32_t) ( x >> 32 );
}

static bool path_lost( path_t * path, int bytes )
{
    const double since = path->connect_time < 0.0 ? 0.0 : path->now - path->connect_time;
    if ( path->connect_time >= 0.0 && bytes > path->large_bytes && since < path->large_loss_seconds )
    {
        return true;
    }
    const int percent = path->connect_time >= 0.0 && since < path->loss_seconds ? path->loss_percent : ( path->connect_time >= 0.0 ? path->after_percent : 0 );
    return percent > 0 && (int) ( path_next_random( path ) % 100 ) < percent;
}

static void path_send( path_t * path, std::vector<path_packet_t> & flight, int capacity, const uint8_t * bytes, int count )
{
    if ( count <= 0 || count > capacity || path_lost( path, count ) )
    {
        return;
    }
    path_packet_t packet;
    packet.arrive = path->now + path->delay;
    packet.bytes.assign( bytes, bytes + count );
    flight.push_back( packet );
}

static void path_client_send( path_t * path, const uint8_t * bytes, int count )
{
    // after a raise every small probe the client sends is a confirm: its
    // walk ended at the raise
    if ( path->drop_confirms && count == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES && bytes[0] == ROCKETNET_CONNECTION_PROBE_PACKET &&
         path->client.confirm_bytes > 0 )
    {
        return;
    }
    path_send( path, path->up, path->cap_up, bytes, count );
}

static struct rocketnet_encryption_mapping_t * path_mapping( path_t * path )
{
    const int index = rocketnet_encryption_map_find( path->server.encryption_map, &path->client_address );
    return index < 0 ? NULL : rocketnet_encryption_map_entry( path->server.encryption_map, index );
}

static int path_server_packet_bytes( path_t * path )
{
    return path->client_index >= 0 ? rocketnet_server_protocol_client_packet_bytes( &path->server, path->client_index ) : 0;
}

// a server on server_mtus and a client on client_mtus, the handshake run to
// connected at one instant on a clean path; the path's settings after
// connect are the caller's, set once this returns

static void path_setup( path_t * path, const int * client_mtus, int num_client_mtus, const int * server_mtus, int num_server_mtus, uint64_t seed )
{
    path->up.clear();
    path->down.clear();
    path->random = seed * 0x9E3779B97F4A7C15ULL + 1;
    path->cap_up = ROCKETNET_MAX_PACKET_BYTES;
    path->cap_down = ROCKETNET_MAX_PACKET_BYTES;
    path->delay = 0.025;
    path->loss_percent = 0;
    path->loss_seconds = 0.0;
    path->after_percent = 0;
    path->large_bytes = ROCKETNET_RECEIVE_CEILING;
    path->large_loss_seconds = 0.0;
    path->drop_confirms = false;
    path->client_reached = -1.0;
    path->server_reached = -1.0;
    path->connect_time = -1.0;
    path->client_index = -1;
    memset( &path->server_counters, 0, sizeof( path->server_counters ) );
    memset( &path->client_counters, 0, sizeof( path->client_counters ) );

    struct rocketnet_server_protocol_t * server = &path->server;
    memset( server, 0, sizeof( *server ) );
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5;
    server->send_rate = ROCKETNET_DEFAULT_SEND_RATE;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = &path->server_counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    memcpy( server->link_mtus, server_mtus, sizeof( int ) * (size_t) num_server_mtus );
    server->num_link_mtus = num_server_mtus;
    check( rocketnet_address_parse( &path->client_address, "198.51.100.7:56789" ) );

    path->now = PATH_START_TIME;
    rocketnet_server_protocol_update( server, path->now );

    // a token whose silence timeout outlasts every loss window here

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = 60;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t advertised[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, advertised, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields, rocketnet_test_backend_secret_key,
                                                        rocketnet_test_token_vector_ephemeral_secret[0], token_data ) );

    struct rocketnet_client_protocol_t * client = &path->client;
    rocketnet_client_protocol_init( client, &path->client_counters );
    memcpy( client->link_mtus, client_mtus, sizeof( int ) * (size_t) num_client_mtus );
    client->num_link_mtus = num_client_mtus;
    check( rocketnet_client_protocol_connect( client, token_data, path->now, ROCKETNET_TEST_CREATE_TS ) );

    // cookie request, cookie, request, challenge, response, keep-alive: each
    // packet answered at once, so the handshake takes no time

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < 8 && client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; i++ )
    {
        int packet_bytes = 0;
        rocketnet_client_protocol_update( client, path->now, packet, &packet_bytes );
        check( packet_bytes > 0 );
        int response_bytes = 0;
        int payload_bytes = 0;
        int payload_client_index = -1;
        rocketnet_server_protocol_process_packet( server, &path->client_address, packet, packet_bytes, ROCKETNET_TEST_CREATE_TS, path->now,
                                                  response, &response_bytes, payload, &payload_bytes, &payload_client_index );
        check( response_bytes > 0 );
        rocketnet_client_protocol_process_packet( client, response, response_bytes, path->now, payload, (int) sizeof( payload ), &payload_bytes );
    }
    check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    struct rocketnet_encryption_mapping_t * mapping = path_mapping( path );
    check( mapping != NULL && mapping->control->client_index != -1 );
    path->client_index = mapping->control->client_index;
    path->connect_time = path->now;
}

static void path_teardown( path_t * path )
{
    rocketnet_encryption_map_destroy( path->server.encryption_map );
    rocketnet_used_token_history_destroy( path->server.used_token_history );
    rocketnet_client_slots_destroy( path->server.client_slots );
    rocketnet_server_acks_destroy( path->server.acks );
}

// one step: the client's update and probe sends, the server's update and
// keep-alive, then the clock forward and every packet due delivered

static void path_step( path_t * path, int target )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];

    if ( path->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        int packet_bytes = 0;
        rocketnet_client_protocol_update( &path->client, path->now, packet, &packet_bytes );
        path_client_send( path, packet, packet_bytes );

        uint8_t small[ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES];
        uint8_t large[ROCKETNET_RECEIVE_CEILING];
        int small_bytes = 0;
        int large_bytes = 0;
        rocketnet_client_protocol_probe_update( &path->client, path->now, small, &small_bytes, large, &large_bytes );
        path_client_send( path, small, small_bytes );
        path_client_send( path, large, large_bytes );
    }

    rocketnet_server_protocol_update( &path->server, path->now );
    if ( path->client_index >= 0 )
    {
        const int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &path->server, path->client_index, path->now, packet );
        path_send( path, path->down, path->cap_down, packet, keep_alive_bytes );
    }

    path->now += PATH_STEP_SECONDS;

    for ( size_t i = 0; i < path->up.size(); )
    {
        if ( path->up[i].arrive > path->now )
        {
            i++;
            continue;
        }
        path_packet_t arrived = path->up[i];
        path->up.erase( path->up.begin() + (long) i );
        uint8_t reply[ROCKETNET_RECEIVE_CEILING];
        int reply_bytes = 0;
        int payload_bytes = 0;
        int payload_client_index = -1;
        rocketnet_server_protocol_process_packet( &path->server, &path->client_address, arrived.bytes.data(), (int) arrived.bytes.size(),
                                                  ROCKETNET_TEST_CREATE_TS, path->now, reply, &reply_bytes, payload, &payload_bytes, &payload_client_index );
        path_send( path, path->down, path->cap_down, reply, reply_bytes );
    }

    for ( size_t i = 0; i < path->down.size(); )
    {
        if ( path->down[i].arrive > path->now )
        {
            i++;
            continue;
        }
        path_packet_t arrived = path->down[i];
        path->down.erase( path->down.begin() + (long) i );
        int payload_bytes = 0;
        rocketnet_client_protocol_process_packet( &path->client, arrived.bytes.data(), (int) arrived.bytes.size(), path->now,
                                                  payload, (int) sizeof( payload ), &payload_bytes );
    }

    const double since = path->now - path->connect_time;
    if ( path->client_reached < 0.0 && rocketnet_client_protocol_packet_bytes( &path->client ) == target )
    {
        path->client_reached = since;
    }
    if ( path->server_reached < 0.0 && path_server_packet_bytes( path ) == target )
    {
        path->server_reached = since;
    }
}

static void path_run( path_t * path, double seconds, int target )
{
    const int steps = (int) ( seconds / PATH_STEP_SECONDS + 0.5 );
    for ( int i = 0; i < steps; i++ )
    {
        path_step( path, target );
    }
}

static bool path_print_times()
{
    char value[8];
    return rocketnet_test_getenv( "ROCKETNET_LINK_MTU_PRINT_TIMES", value, sizeof( value ) );
}

// the slowest time over seeds of the later of the two ends reaching 1452,
// and the mean; false when any seed did not reach it at both ends

struct path_times_t
{
    double worst;
    double mean;
    bool all_reached;
};

static path_times_t path_times( int loss_percent, double loss_seconds, int after_percent, int seeds, double run_seconds )
{
    path_times_t times = { 0.0, 0.0, true };
    path_t * path = new path_t();
    for ( int seed = 1; seed <= seeds; seed++ )
    {
        path_setup( path, probe_three_step, 3, probe_three_step, 3, (uint64_t) seed );
        path->loss_percent = loss_percent;
        path->loss_seconds = loss_seconds;
        path->after_percent = after_percent;
        path_run( path, run_seconds, 1452 );
        if ( path->client_reached < 0.0 || path->server_reached < 0.0 )
        {
            times.all_reached = false;
        }
        const double both = path->client_reached > path->server_reached ? path->client_reached : path->server_reached;
        times.worst = both > times.worst ? both : times.worst;
        times.mean += both / seeds;
        path_teardown( path );
    }
    delete path;
    return times;
}

// A lossless 1500 path at a 50 ms round trip: the client connects at 548,
// sends its first pair at connect, and both ends are at 1452 within one
// round trip and the confirm -- the client at one round trip, the server at
// one and a half -- well under a second (planted: the first pair waits one
// interval after connect, and the client reaches 1452 at 0.3 s, not 0.05 s).

static void test_link_mtu_lossless_reaches_max_in_one_round_trip()
{
    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 548 );
    check( path_server_packet_bytes( path ) == 548 );

    path_run( path, 2.0, 1452 );

    const double round_trip = 2.0 * path->delay;
    check( path->client_reached >= 0.0 && path->client_reached <= round_trip + 2 * PATH_STEP_SECONDS );
    check( path->server_reached >= 0.0 && path->server_reached <= 1.5 * round_trip + 3 * PATH_STEP_SECONDS );
    check( path->server_reached < 1.0 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_STRIKE] == 0 );
    check( path->server_counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_CONFIRMED] == 1 );

    if ( path_print_times() )
    {
        printf( "link mtu probe, lossless at a %.0f ms round trip: client at 1452 after %.3f s, server after %.3f s\n",
                round_trip * 1000.0, path->client_reached, path->server_reached );
    }

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_lossless_reaches_max_in_one_round_trip );

// 10% loss each way, the whole time, over 50 seeds: every one at 1452 at
// both ends within 5 s, the pairs paced at a quarter second and doubling to
// one a second (planted: a pair every 5 s, the first design's pace, and the
// worst seed passes 5 s).

static void test_link_mtu_ten_percent_loss_reaches_max_in_seconds()
{
    const path_times_t times = path_times( 10, 1e9, 10, 50, 10.0 );
    check( times.all_reached );
    check( times.worst <= 5.0 );
    if ( path_print_times() )
    {
        printf( "link mtu probe, 10%% loss each way: both ends at 1452 after %.3f s on average, %.3f s at worst of 50 seeds\n", times.mean, times.worst );
    }
}

REGISTER_UNIT_TEST( test_link_mtu_ten_percent_loss_reaches_max_in_seconds );

// 50% loss each way for the first 10 s on a 1500 path, then none, over 50
// seeds: every one ends at 1452 at both ends, never ruled out by the loss,
// since four counted strikes span 15 s; once the loss ends, within about a
// second (planted: every strike counts, unspaced, and a seed rules 1452 out
// and ends at 1172).

static void test_link_mtu_heavy_loss_first_ten_seconds_still_ends_at_max()
{
    path_t * path = new path_t();
    double worst = 0.0;
    double mean = 0.0;
    for ( int seed = 1; seed <= 50; seed++ )
    {
        path_setup( path, probe_three_step, 3, probe_three_step, 3, (uint64_t) seed );
        path->loss_percent = 50;
        path->loss_seconds = 10.0;
        path_run( path, 30.0, 1452 );
        check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );
        check( path_server_packet_bytes( path ) == 1452 );
        check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT] == 0 );
        const double both = path->client_reached > path->server_reached ? path->client_reached : path->server_reached;
        worst = both > worst ? both : worst;
        mean += both / 50.0;
        path_teardown( path );
    }
    check( worst <= 12.0 );
    if ( path_print_times() )
    {
        printf( "link mtu probe, 50%% loss for the first 10 s: both ends at 1452 after %.3f s on average, %.3f s at worst of 50 seeds\n", mean, worst );
    }
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_heavy_loss_first_ten_seconds_still_ends_at_max );

// 100% loss for the first 10 s, then a clean path: nothing counts while the
// small probes are lost, the probe keeps going at one pair a second, and both
// ends are at 1452 within 2 s of the loss ending (planted: the backoff
// doubles past one second, to 8 s, and the first pair after the loss goes
// out too late).

static void test_link_mtu_total_loss_then_clean_reaches_max_within_two_seconds()
{
    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );
    path->loss_percent = 100;
    path->loss_seconds = 10.0;
    path_run( path, 20.0, 1452 );

    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );
    check( path_server_packet_bytes( path ) == 1452 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_STRIKE] == 0 );
    check( path->client_reached >= 10.0 && path->client_reached <= 12.0 );
    check( path->server_reached >= 10.0 && path->server_reached <= 12.0 );
    if ( path_print_times() )
    {
        printf( "link mtu probe, 100%% loss for the first 10 s: client at 1452 after %.3f s, server after %.3f s (%.3f s after the loss)\n",
                path->client_reached, path->server_reached, path->server_reached - 10.0 );
    }

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_total_loss_then_clean_reaches_max_within_two_seconds );

// A burst of 12 s in which every packet above 200 bytes is lost both ways,
// the small probes and keep-alives arriving: the large probes collect
// strikes, but a strike counts only 5 s after the last counted one, so at
// most three count before the burst ends and 1452 is never ruled out
// (planted: every strike counts, and 1452 is ruled out at about one second).

static void test_link_mtu_large_loss_burst_never_rules_out_a_carried_size()
{
    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );
    path->large_bytes = 200;
    path->large_loss_seconds = 12.0;
    path_run( path, 25.0, 1452 );

    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_STRIKE] >= 2 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_STRIKE] <= 3 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT] == 0 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );
    check( path_server_packet_bytes( path ) == 1452 );

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_large_loss_burst_never_rules_out_a_carried_size );

// A 1200 link (1172-byte datagrams both ways): 1452 collects four counted
// strikes 5 s apart and is ruled out about 16 s after connect, 1172 is
// answered at once, and both ends end at 1172; then probing has ended for
// good: in a minute more nothing but the confirm's echo is sent (planted:
// the walk goes on after the raise, and probes keep going out).

static void test_link_mtu_rules_out_what_the_path_does_not_carry_then_stops()
{
    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );
    path->cap_up = 1172;
    path->cap_down = 1172;
    path_run( path, 25.0, 1172 );

    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT] == 1 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_STRIKE] == ROCKETNET_LINK_MTU_PROBE_STRIKES );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1172 );
    check( path_server_packet_bytes( path ) == 1172 );
    check( path->client_reached >= 15.0 && path->client_reached <= 18.0 );
    check( !path->client.probing );
    check( path->client.confirm_bytes == 0 );

    const uint64_t sent = path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT];
    const uint64_t received = path->server_counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_RECEIVED];
    path_run( path, 60.0, 1172 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == sent );
    check( path->server_counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_RECEIVED] == received );

    if ( path_print_times() )
    {
        printf( "link mtu probe, a 1200 link: 1452 ruled out and both ends at 1172 after %.3f s\n", path->server_reached );
    }

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_rules_out_what_the_path_does_not_carry_then_stops );

// Once the size is the largest candidate, probing ends for good: on a clean
// path, after the raise and the confirm's echo, a minute more sends no probe
// and no confirm (planted: the walk is not ended by the raise, and a pair
// goes out every second).

static void test_link_mtu_no_probe_after_the_largest_size()
{
    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );
    path_run( path, 1.0, 1452 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );
    check( !path->client.probing );
    check( path->client.confirm_bytes == 0 );

    // the first pair and the confirm, nothing more
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 3 );
    path_run( path, 60.0, 1452 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 3 );

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_no_probe_after_the_largest_size );

// The server raises only on the confirm: with every confirm lost, the client
// sends at 1452 and the server stays at 548, the client resending its confirm
// each second; once confirms get through, the server raises within about a
// second (planted: the server raises on the large probe it answers, and it is
// at 1452 while every confirm is lost).

static void test_link_mtu_server_raises_only_on_the_confirm()
{
    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );
    path->drop_confirms = true;
    path_run( path, 5.0, 1452 );

    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );
    check( path_server_packet_bytes( path ) == 548 );
    check( path->server_counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_CONFIRMED] == 0 );
    check( path->client.confirm_bytes == 1452 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] >= 2 + 4 );

    path->drop_confirms = false;
    path_run( path, 1.5, 1452 );
    check( path_server_packet_bytes( path ) == 1452 );
    check( path->client.confirm_bytes == 0 );

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_server_raises_only_on_the_confirm );

// The client raises only on an authenticated reply: a large reply forged at
// 1452 under a key that is not the session key does not open, is an
// integrity failure, and the size stays 548; an authentic reply at a size
// below the client's, after it raised, changes nothing (planted: the reply's
// open skipped, the forged reply raises the client to 1452).

static void test_link_mtu_client_raises_only_on_an_authenticated_reply()
{
    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );

    // the client's first pair is out; a forged reply arrives before the real one
    path->delay = 1.0;
    path_step( path, 1452 );
    check( path->client.probing && path->client.probe_pair == 1 );

    uint8_t forged[ROCKETNET_RECEIVE_CEILING];
    uint8_t wrong_key[ROCKETNET_KEY_BYTES];
    memset( wrong_key, 0x77, sizeof( wrong_key ) );
    check( probe_write( forged, 1452, ROCKETNET_PROBE_KIND_PROBE, 1452, 1, 2, path->client.connection_salt, wrong_key, false ) == 1452 );
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_packet( &path->client, forged, 1452, path->now, payload, (int) sizeof( payload ), &payload_bytes ) == 0 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_DID_NOT_OPEN] == 1 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 548 );

    // the real replies: 1452
    path_run( path, 3.0, 1452 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );

    // an authentic large reply at 1172 after the raise: not probing, ignored
    check( probe_write( forged, 1172, ROCKETNET_PROBE_KIND_PROBE, 1172, 1, 1000, path->client.connection_salt,
                        path->client.keys.session_server_to_client, false ) == 1172 );
    check( rocketnet_client_protocol_process_packet( &path->client, forged, 1172, path->now, payload, (int) sizeof( payload ), &payload_bytes ) == 0 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_client_raises_only_on_an_authenticated_reply );

// A path that carries the small probes but not P0 (1000-byte datagrams, the
// list { 1500, 1200 } connecting at 1172): 1452 is ruled out, then 1172, the
// connection's own size, and the connection ends in mtu probe failed (-14)
// within the walk's bound (planted: past the last candidate the walk starts
// over, and the client is still connected).

static void test_link_mtu_fails_when_its_own_size_is_not_carried()
{
    const int two[2] = { 1500, 1200 };
    path_t * path = new path_t();
    path_setup( path, two, 2, two, 2, 1 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1172 );
    path->cap_up = 1000;
    path->cap_down = 1000;

    double failed = -1.0;
    for ( int i = 0; i < 6000 && failed < 0.0; i++ )
    {
        path_step( path, 1452 );
        if ( path->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_MTU_PROBE_FAILED )
        {
            failed = path->now - path->connect_time;
        }
    }
    check( failed > 0.0 );
    check( failed <= 2 * ( 4 * ROCKETNET_LINK_MTU_PROBE_STRIKE_SPACING_SECONDS ) );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_RULED_OUT] == 2 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_FAILED] == 1 );

    if ( path_print_times() )
    {
        printf( "link mtu probe, P0 not carried: mtu probe failed after %.3f s\n", failed );
    }

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_fails_when_its_own_size_is_not_carried );

// The server's refusals of a probe before it opens it, each counted as
// itself: shorter than the small probe (33 bytes), and a zero sequence on a
// connected client's mapping, which is not opened and is not an integrity
// failure (planted: the zero sequence check removed, the probe goes to the
// open and fails it).

static void test_server_probe_refused_before_the_open()
{
    uint8_t probe[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;

    struct probe_server_t test;
    probe_server_setup( &test, probe_three_step, 3 );
    probe_server_connect( &test, 548 );

    check( probe_server_client_probe( &test, probe, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES, 1, 2, false ) == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
    check( probe_server_receive( &test, &test.client_address, probe, ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES - 1, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( reply_bytes == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_WRONG_SIZE] == 1 );

    check( probe_server_client_probe( &test, probe, 1172, 1, 3, false ) == 1172 );
    memset( probe + ROCKETNET_PREFILTER_BYTES, 0, 4 );     // the sequence field: the writer seals no zero sequence
    check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( reply_bytes == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ZERO_SEQUENCE] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_DID_NOT_OPEN] == 0 );
    check( test.mapping->integrity_failures == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 0 );

    probe_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_probe_refused_before_the_open );

// The client's refusals of a probe reply, each counted as itself and none
// changing its size: before it connects; at a size neither small nor a
// candidate (1000 bytes); a zero sequence, not opened and not an integrity
// failure; nonzero padding under the session key; and an authentic reply
// once the walk has ended. Every reply counts as received (planted: the zero
// sequence check removed, the reply goes to the open and fails it).

static void test_link_mtu_client_probe_reply_refusals()
{
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    path_t * path = new path_t();
    path_setup( path, probe_three_step, 3, probe_three_step, 3, 1 );
    path->delay = 1.0;
    path_step( path, 1452 );
    check( path->client.probing );
    const uint8_t * key = path->client.keys.session_server_to_client;
    const uint64_t received = path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_RECEIVED];
    const uint64_t integrity_failures = path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_DID_NOT_OPEN];

    // a size no candidate has

    check( probe_write( reply, 1000, ROCKETNET_PROBE_KIND_PROBE, 1000, 1, 1000, path->client.connection_salt, key, false ) == 1000 );
    check( rocketnet_client_protocol_process_packet( &path->client, reply, 1000, path->now, payload, (int) sizeof( payload ), &payload_bytes ) == 0 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SIZE_NOT_CANDIDATE] == 1 );

    // a zero sequence

    check( probe_write( reply, 1452, ROCKETNET_PROBE_KIND_PROBE, 1452, 1, 999, path->client.connection_salt, key, false ) == 1452 );
    memset( reply + ROCKETNET_PREFILTER_BYTES, 0, 4 );     // the sequence field: the writer seals no zero sequence
    check( rocketnet_client_protocol_process_packet( &path->client, reply, 1452, path->now, payload, (int) sizeof( payload ), &payload_bytes ) == 0 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_ZERO_SEQUENCE] == 1 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_DID_NOT_OPEN] == integrity_failures );

    // nonzero padding, opened under the session key

    check( probe_write( reply, 1452, ROCKETNET_PROBE_KIND_PROBE, 1452, 1, 1001, path->client.connection_salt, key, true ) == 1452 );
    check( rocketnet_client_protocol_process_packet( &path->client, reply, 1452, path->now, payload, (int) sizeof( payload ), &payload_bytes ) == 0 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_PADDING_NOT_ZERO] == 1 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 548 );

    // the real replies raise to 1452 and end the walk; an authentic reply
    // after that is in the wrong state

    path_run( path, 3.0, 1452 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );
    check( !path->client.probing );
    const uint64_t wrong_state = path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WRONG_STATE];
    check( probe_write( reply, 1172, ROCKETNET_PROBE_KIND_PROBE, 1172, 1, 1002, path->client.connection_salt, key, false ) == 1172 );
    check( rocketnet_client_protocol_process_packet( &path->client, reply, 1172, path->now, payload, (int) sizeof( payload ), &payload_bytes ) == 0 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WRONG_STATE] == wrong_state + 1 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_RECEIVED] > received + 3 );
    check( rocketnet_client_protocol_packet_bytes( &path->client ) == 1452 );

    // a client that is not connected

    path->client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
    check( rocketnet_client_protocol_process_packet( &path->client, reply, 1172, path->now, payload, (int) sizeof( payload ), &payload_bytes ) == 0 );
    check( path->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WRONG_STATE] == wrong_state + 2 );
    path->client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;

    path_teardown( path );
    delete path;
}

REGISTER_UNIT_TEST( test_link_mtu_client_probe_reply_refusals );

#endif // #if ROCKETNET_DEV
