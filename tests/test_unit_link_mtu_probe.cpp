/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_link_mtu_probe.cpp -- the path probe, one end at a time
// (standard/TRANSPORT.md t8.13, t11.4, t12 rule 15; proofs/tla/RNPathProbe.tla).
//
// The server: a probe is answered at its own size only from an address that
// holds a mapping and no connected slot, one opened per mapping per update,
// and only at a size its own list gives; a probe that does not open is an
// integrity failure; the response's packet size field must be on the list and
// fixes the slot's packet size. The client: a one-entry list never probes and
// builds today's response; a longer list probes each candidate for a window,
// accepts a reply at or below the candidate it is probing (a late reply to an
// earlier, larger probe included) and refuses one above it.
//
// No socket, no thread, no clock: both ends are driven through their protocol
// calls with the time passed in, from the published test values.

#include "test_unit_standard.h"

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
#include "rocketnet_link_mtu.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

// the sealed overhead of a probe: prefilter, sequence and tag
#define PROBE_SEALED_OVERHEAD_BYTES ( ROCKETNET_PREFILTER_BYTES + 4 + 16 )

static const int probe_three_step[3] = { 1492, 1200, 576 };

// a probe, or a probe reply, of wire_bytes: zero padding (one byte set when
// nonzero_padding) sealed under key on sequence; an end seals it under its
// session key of its direction

static int probe_write( uint8_t * out, int wire_bytes, uint64_t sequence, const uint8_t * salt, const uint8_t * key, bool nonzero_padding )
{
    uint8_t padding[ROCKETNET_CONNECTION_PROBE_MAX_WIRE_BYTES] = { 0 };
    if ( nonzero_padding )
    {
        padding[0] = 1;
    }
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    return rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_PROBE_PACKET, filter, sequence, NULL,
                                          padding, wire_bytes - PROBE_SEALED_OVERHEAD_BYTES,
                                          ROCKETNET_TEST_PROTOCOL_ID, salt, NULL, key );
}

// ---------------------------------------------------------------------------
// the server

struct probe_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_encryption_mapping_t * mapping;
    double time;
};

// the handshake deadline the next server is set up with
static int probe_server_deadline_seconds = 30;

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
    server->handshake_deadline_seconds = probe_server_deadline_seconds;
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

// the next update: one probe more may be opened per mapping

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

// the client's probe of wire_bytes, sealed as a client would seal it

static int probe_server_client_probe( struct probe_server_t * test, uint8_t * out, int wire_bytes, uint64_t sequence, bool nonzero_padding )
{
    return probe_write( out, wire_bytes, sequence, test->mapping->connection_salt,
                        test->mapping->keys.session_client_to_server, nonzero_padding );
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

// A probe at a size the list gives is answered at the same size, a zero
// padding sealed under the handshake server-to-client key; a second probe in
// the same update is withheld unopened, and the next update answers again
// (planted: the per-update allowance removed, the second probe is answered).

static void test_server_probe_answered_at_its_size_once_per_update()
{
    struct probe_server_t test;
    probe_server_setup( &test, probe_three_step, 3 );

    uint8_t probe[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;

    check( probe_server_client_probe( &test, probe, 1172, 2, false ) == 1172 );
    check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
    check( reply_bytes == 1172 );
    check( reply[0] == ROCKETNET_CONNECTION_PROBE_PACKET );

    const uint8_t * p = reply + ROCKETNET_PREFILTER_BYTES;
    const uint64_t reply_sequence = rocketnet_read_uint32( &p );
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), reply, reply_bytes,
                                                           ROCKETNET_CONNECTION_PROBE_PACKET, reply_sequence,
                                                           ROCKETNET_TEST_PROTOCOL_ID, test.mapping->connection_salt, NULL,
                                                           test.mapping->keys.session_server_to_client );
    check( opened_bytes == 1172 - PROBE_SEALED_OVERHEAD_BYTES );
    for ( int i = 0; i < opened_bytes; i++ )
    {
        check( opened[i] == 0 );
    }
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 1 );

    // the same update: withheld, unopened, even a probe that would not open

    check( probe_server_client_probe( &test, probe, 548, 3, false ) == 548 );
    check( probe_server_receive( &test, &test.client_address, probe, 548, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( reply_bytes == 0 );
    probe[ROCKETNET_PREFILTER_BYTES + 6] ^= 1;
    check( probe_server_receive( &test, &test.client_address, probe, 548, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_WITHHELD] == 2 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_DID_NOT_OPEN] == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 1 );

    // the next update answers again, at the smallest size

    probe_server_next_update( &test );
    check( probe_server_client_probe( &test, probe, 548, 4, false ) == 548 );
    check( probe_server_receive( &test, &test.client_address, probe, 548, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
    check( reply_bytes == 548 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 2 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_RECEIVED] == 4 );

    probe_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_probe_answered_at_its_size_once_per_update );

// Each refusal of a probe, with its own counter and nothing answered: a size
// the list does not give (on the default list, every size below the ceiling),
// an address with no mapping, a probe that does not open (an integrity
// failure on the mapping), nonzero padding (not one), and a probe from a
// connected client (planted: the list check removed, the off-list size is
// answered).

static void test_server_probe_refusals()
{
    uint8_t probe[ROCKETNET_RECEIVE_CEILING];
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    int reply_bytes = 0;

    {
        struct probe_server_t test;
        probe_server_setup( &test, probe_three_step, 3 );

        // 1000 bytes: no entry of the list gives it

        check( probe_server_client_probe( &test, probe, 1000, 2, false ) == 1000 );
        check( probe_server_receive( &test, &test.client_address, probe, 1000, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_SIZE_NOT_ALLOWED] == 1 );

        // an address the server holds no mapping for

        struct rocketnet_address_t stranger;
        check( rocketnet_address_parse( &stranger, "198.51.100.8:56789" ) );
        check( probe_server_client_probe( &test, probe, 1172, 2, false ) == 1172 );
        check( probe_server_receive( &test, &stranger, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_NO_MAPPING] == 1 );

        // sealed under the handshake key, not the session key: an integrity
        // failure

        check( probe_write( probe, 1172, 2, test.mapping->connection_salt, test.mapping->keys.handshake_client_to_server, false ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_DID_NOT_OPEN] == 1 );
        check( test.mapping->integrity_failures == 1 );

        // opened, padding not zero: refused, not a forgery

        probe_server_next_update( &test );
        check( probe_server_client_probe( &test, probe, 1172, 3, true ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_PADDING_NOT_ZERO] == 1 );
        check( test.mapping->integrity_failures == 1 );

        // connected: the response at 1172 takes a slot, and a probe after it
        // is ignored

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        check( probe_server_client_response( &test, response, 4, 1172 ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
        check( probe_server_receive( &test, &test.client_address, response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
        probe_server_next_update( &test );
        check( probe_server_client_probe( &test, probe, 1172, 5, false ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ALREADY_CONNECTED] == 1 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 0 );

        probe_server_teardown( &test );
    }

    {
        // the default list gives the ceiling alone

        struct probe_server_t test;
        probe_server_setup( &test, NULL, 0 );
        check( probe_server_client_probe( &test, probe, 1172, 2, false ) == 1172 );
        check( probe_server_receive( &test, &test.client_address, probe, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( reply_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_SIZE_NOT_ALLOWED] == 1 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_ANSWERED] == 0 );
        probe_server_teardown( &test );
    }
}

REGISTER_UNIT_TEST( test_server_probe_refusals );

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
            check( probe_write( forged, 1172, (uint64_t) ( 2 + update ), test.mapping->connection_salt, wrong_key, false ) == 1172 );
            check( probe_server_receive( &test, &test.client_address, forged, 1172, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
            check( reply_bytes == 0 );
        }

        check( probe_server_client_probe( &test, probe, 1172, (uint64_t) ( 2 + update ), false ) == 1172 );
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

// The handshake deadline of a half-open mapping: the configured deadline on
// the default list; on a list that probes, the probe's whole bound more, so a
// walk of the longest list ends before the mapping does (standard/
// TRANSPORT.md t12 rule 14). Planted: no bound added, the mapping of a
// probing server is removed at 5 s, before the walk can end.

static void test_server_probe_bound_extends_the_handshake_deadline()
{
    probe_server_deadline_seconds = 5;

    {
        struct probe_server_t test;
        probe_server_setup( &test, NULL, 0 );
        check( test.mapping->control->handshake_deadline == test.time + 5.0 );
        probe_server_teardown( &test );
    }

    {
        const int longest[8] = { 1500, 1400, 1300, 1200, 1000, 800, 700, 576 };
        struct probe_server_t test;
        probe_server_setup( &test, longest, 8 );
        const double created = test.time;
        check( test.mapping->control->handshake_deadline == created + 5.0 + ROCKETNET_LINK_MTU_PROBE_BUDGET_SECONDS );

        // the client's eighth window ends 8 s after the challenge; its last
        // probe, answered, refreshes the half-open clock, and the mapping is
        // still there for the response

        uint8_t probe[ROCKETNET_RECEIVE_CEILING];
        uint8_t reply[ROCKETNET_RECEIVE_CEILING];
        int reply_bytes = 0;
        for ( int step = 0; step < 790; step++ )
        {
            probe_server_next_update( &test );
        }
        check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) != -1 );
        check( probe_server_client_probe( &test, probe, 548, 2, false ) == 548 );
        check( probe_server_receive( &test, &test.client_address, probe, 548, reply, &reply_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_PROBE );
        check( test.counters.values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 0 );

        probe_server_teardown( &test );
    }

    probe_server_deadline_seconds = 30;
}

REGISTER_UNIT_TEST( test_server_probe_bound_extends_the_handshake_deadline );

// ---------------------------------------------------------------------------
// the client

struct probe_client_t
{
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    double time;
    uint64_t reply_sequence;
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
    test->reply_sequence = 2;
}

// the client's next send at time (zero bytes when none is due)

static int probe_client_send( struct probe_client_t * test, double time, uint8_t * packet )
{
    int packet_bytes = 0;
    test->time = time;
    rocketnet_client_protocol_update( &test->client, time, packet, &packet_bytes );
    return packet_bytes;
}

// sends until one goes out, at most a second of 10 ms steps

static int probe_client_next_send( struct probe_client_t * test, uint8_t * packet )
{
    for ( int i = 0; i < 100; i++ )
    {
        const int bytes = probe_client_send( test, test->time + 0.01, packet );
        if ( bytes > 0 )
        {
            return bytes;
        }
    }
    return 0;
}

// a server's probe reply of wire_bytes reaching the client

static int probe_client_reply( struct probe_client_t * test, int wire_bytes, const uint8_t * key, bool nonzero_padding )
{
    uint8_t reply[ROCKETNET_RECEIVE_CEILING];
    check( probe_write( reply, wire_bytes, test->reply_sequence++, rocketnet_test_salt,
                        key ? key : test->client.keys.session_server_to_client, nonzero_padding ) == wire_bytes );
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    return rocketnet_client_protocol_process_packet( &test->client, reply, wire_bytes, test->time, payload, sizeof( payload ), &payload_bytes );
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

// A one-entry list never probes: the first send after the challenge is the
// response. On the default list it is byte for byte the response of a client
// that never set the list, its size field zero; a one-entry list below the
// ceiling writes its size (planted: a one-entry list probes, and its first
// send is a probe).

static void test_client_one_entry_list_sends_no_probe()
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
    check( one.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 0 );
    check( rocketnet_client_protocol_packet_bytes( &one.client ) == ROCKETNET_MAX_PACKET_BYTES );

    const int small_list[1] = { 1200 };
    struct probe_client_t small;
    probe_client_setup( &small, small_list, 1 );
    check( probe_client_next_send( &small, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( probe_client_response_field( &small ) == 1172 );
    check( small.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 0 );
    check( rocketnet_client_protocol_packet_bytes( &small.client ) == 1172 );
}

REGISTER_UNIT_TEST( test_client_one_entry_list_sends_no_probe );

// The three-step list: four probes of 1452, then of 1172; a 548 reply while
// probing 1172 is refused (above the candidate probed), a forged reply is an
// integrity failure, a reply with nonzero padding is refused, and a late reply
// to the 1452 probe is accepted: the connection is 1452 and the next send is
// the response carrying zero; a reply after that is refused (planted: a
// reply at any candidate accepted, 548 is taken).

static void test_client_probe_windows_and_replies()
{
    struct probe_client_t test;
    probe_client_setup( &test, probe_three_step, 3 );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    // a 1172 reply before the 1172 probe is sent: above the candidate

    check( probe_client_reply( &test, 1172, NULL, false ) == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SIZE_NOT_CANDIDATE] == 1 );

    for ( int i = 0; i < ROCKETNET_CLIENT_PROBE_WINDOW_SENDS; i++ )
    {
        check( probe_client_next_send( &test, packet ) == 1452 );
        check( packet[0] == ROCKETNET_CONNECTION_PROBE_PACKET );
    }
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED] == 0 );

    check( probe_client_next_send( &test, packet ) == 1172 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == ROCKETNET_CLIENT_PROBE_WINDOW_SENDS + 1 );

    // 548 is below the candidate probed: refused

    check( probe_client_reply( &test, 548, NULL, false ) == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SIZE_NOT_CANDIDATE] == 2 );

    // a reply sealed under the handshake key, which does not open, and one
    // whose padding is not zero

    check( probe_client_reply( &test, 1172, test.client.keys.handshake_server_to_client, false ) == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_DID_NOT_OPEN] == 1 );
    check( probe_client_reply( &test, 1172, NULL, true ) == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_PADDING_NOT_ZERO] == 1 );
    check( test.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    // the late reply to a 1452 probe: accepted, the connection is 1452

    check( probe_client_reply( &test, 1452, NULL, false ) == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_ACCEPTED] == 1 );
    check( rocketnet_client_protocol_packet_bytes( &test.client ) == 1452 );
    check( probe_client_response_field( &test ) == 0 );

    // the early send is the response, and so is every send after it

    check( probe_client_send( &test, test.time, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( packet[0] == ROCKETNET_CONNECTION_RESPONSE_PACKET );
    check( probe_client_next_send( &test, packet ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( memcmp( packet, test.client.stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES ) == 0 );

    check( probe_client_reply( &test, 1172, NULL, false ) == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WRONG_STATE] == 1 );
    check( rocketnet_client_protocol_packet_bytes( &test.client ) == 1452 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == ROCKETNET_CLIENT_PROBE_WINDOW_SENDS + 1 );
}

REGISTER_UNIT_TEST( test_client_probe_windows_and_replies );

// A window is never longer than its bound, however long the round trip: at
// a 2 s round trip, ten sends, not sixty (planted: the cap removed, the
// window is sixty).

static void test_client_probe_window_capped()
{
    struct probe_client_t test;
    probe_client_setup( &test, probe_three_step, 3 );
    test.client.rtt_estimate = 2.0;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < ROCKETNET_CLIENT_PROBE_WINDOW_MAX_SENDS; i++ )
    {
        check( probe_client_next_send( &test, packet ) == 1452 );
    }
    check( probe_client_next_send( &test, packet ) == 1172 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED] == 1 );
}

REGISTER_UNIT_TEST( test_client_probe_window_capped );

// Every window expires: four probes of each candidate, then the attempt ends
// in MTU_PROBE_FAILED with nothing more sent, though the walk outlasts the
// silence timeout (planted: past the last candidate the client starts over,
// and the attempt never ends; or the silence timeout runs while probing, and
// the attempt ends in -4).

static void test_client_probe_fails_past_the_last_candidate()
{
    struct probe_client_t test;
    probe_client_setup( &test, probe_three_step, 3 );

    // a silence timeout shorter than the walk: the walk still ends in its own
    // error, never connection response timed out (-4)
    test.client.timeout_seconds = 1;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int sizes[3] = { 1452, 1172, 548 };
    for ( int c = 0; c < 3; c++ )
    {
        for ( int i = 0; i < ROCKETNET_CLIENT_PROBE_WINDOW_SENDS; i++ )
        {
            check( probe_client_next_send( &test, packet ) == sizes[c] );
        }
    }
    check( test.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    check( probe_client_next_send( &test, packet ) == 0 );
    check( test.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_MTU_PROBE_FAILED );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED] == 3 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_FAILED] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_SENT] == 3 * ROCKETNET_CLIENT_PROBE_WINDOW_SENDS );

    // a reply after the end changes nothing

    check( probe_client_reply( &test, 548, NULL, false ) == 0 );
    check( test.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_MTU_PROBE_FAILED );
}

REGISTER_UNIT_TEST( test_client_probe_fails_past_the_last_candidate );

#endif // #if ROCKETNET_DEV
