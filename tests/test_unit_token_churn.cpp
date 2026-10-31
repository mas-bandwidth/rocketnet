/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Token churn: one token, one live connection, and nothing more. A slot is
// freed when its connection closes, and a fresh token at a server with a
// free slot always connects, however many players have come and gone
// before it (proofs/tla/RNSecurityTables.tla, OneTokenOneLiveConnection and
// ChurnNeverDenied; docs/SESSION.md, Token reuse).
//
// Every case runs client and server protocols in memory on mocked time that
// starts at 1000.0: no sockets, no real time.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_crypto_tally.h"
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

#if ROCKETNET_DEV
extern "C" void rocketnet_dev_token_table_reset_operation_counts( struct rocketnet_used_token_history_t * history );
extern "C" uint64_t rocketnet_dev_token_claim_probe_steps( const struct rocketnet_used_token_history_t * history );
extern "C" uint64_t rocketnet_dev_token_claim_slot_steps( const struct rocketnet_used_token_history_t * history );
extern "C" int rocketnet_dev_token_claim_index_start( const struct rocketnet_used_token_history_t * history,
                                                      const uint8_t * token_identifier );
#endif // #if ROCKETNET_DEV

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// the token clock: every token here lives 30 seconds from its create
// timestamp, the default lifetime, and the server reads the token clock at
// the create timestamp unless a case moves it
#define TC_TOKEN_LIFETIME           30
#define TC_NOW                      ROCKETNET_TEST_CREATE_TS

struct tc_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    double time;
};

static void tc_mint_token( uint8_t * token_data, uint64_t client_id )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_CREATE_TS + TC_TOKEN_LIFETIME;
    signed_fields.timeout_seconds = ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = client_id;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = signed_fields.expire_timestamp;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// a server protocol for max_clients slots, with a used-token history of
// history_capacity entries and a mapping table of twice the slots plus two

static void tc_server_setup( struct tc_server_t * t, int max_clients, int history_capacity )
{
    memset( t, 0, sizeof( *t ) );
    struct rocketnet_server_protocol_t * server = &t->server;
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
    server->encryption_map = rocketnet_encryption_map_create( NULL, max_clients * 2 + 2 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, history_capacity );
    server->client_slots = rocketnet_client_slots_create( NULL, max_clients );
    server->acks = rocketnet_server_acks_create( NULL, max_clients );
    server->reassembly = rocketnet_reassembly_set_create( NULL, max_clients, 4, 16384, max_clients * 4 );
    server->counters = &t->counters;
    check( server->encryption_map && server->used_token_history &&
           server->client_slots && server->acks && server->reassembly );

    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, TC_NOW );
    t->time = 1000.0;
}

static void tc_server_teardown( struct tc_server_t * t )
{
    rocketnet_counters_assert_conserved( &t->counters );
    rocketnet_encryption_map_destroy( t->server.encryption_map );
    rocketnet_used_token_history_destroy( t->server.used_token_history );
    rocketnet_client_slots_destroy( t->server.client_slots );
    rocketnet_server_acks_destroy( t->server.acks );
    rocketnet_reassembly_set_destroy( t->server.reassembly );
}

struct tc_client_t
{
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t address;
};

static void tc_client_init( struct tc_client_t * c, const char * address )
{
    memset( c, 0, sizeof( *c ) );
    rocketnet_client_protocol_init( &c->client, &c->counters );
    c->client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, ROCKETNET_NUM_CLIENT_PACKETS / 2 );
    check( c->client.reassembly );

    rocketnet_reassembly_set_begin_update( c->client.reassembly );
    check( rocketnet_address_parse( &c->address, address ) );
}

static void tc_client_destroy( struct tc_client_t * c )
{
    rocketnet_counters_assert_conserved( &c->counters );
    rocketnet_reassembly_set_destroy( c->client.reassembly );
}

// drives the handshake on mocked time until the client connects, is denied,
// or ten seconds pass; returns the client's state

static int tc_drive( struct tc_server_t * t, struct tc_client_t * c, uint64_t now )
{
    uint8_t out_packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    const double start = t->time;
    for ( ; t->time < start + ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS; t->time += 0.05 )
    {
        if ( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ||
             c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED )
            break;

        int out_bytes = 0;
        rocketnet_client_protocol_update( &c->client, t->time, out_packet, &out_bytes );
        if ( out_bytes == 0 )
            continue;

        int response_bytes = 0;
        rocketnet_server_protocol_process_packet( &t->server, &c->address, out_packet, out_bytes,
                                                  now, t->time, response, &response_bytes,
                                                  payload, &payload_bytes, &payload_client_index );
        if ( response_bytes > 0 )
            rocketnet_client_protocol_process_packet( &c->client, response, response_bytes, t->time + 0.01,
                                                      payload, (int) sizeof( payload ), &payload_bytes );
    }

    return c->client.state;
}

// the client leaves cleanly: its disconnect burst reaches the server, which
// frees the slot and the mapping at once

static void tc_disconnect( struct tc_server_t * t, struct tc_client_t * c, uint64_t now )
{
    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_client_protocol_disconnect( &c->client, burst, &burst_count, &burst_bytes );
    check( burst_count > 0 );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    for ( int i = 0; i < burst_count; i++ )
    {
        int response_bytes = 0;
        rocketnet_server_protocol_process_packet( &t->server, &c->address, burst + i * ROCKETNET_SEND_BUDGET, burst_bytes,
                                                  now, t->time, response, &response_bytes,
                                                  payload, &payload_bytes, &payload_client_index );
        t->time += 0.001;
    }

    t->time += 0.2;
}

// The defect: with max_clients 1, two clean connect/disconnect cycles were
// admitted and the third fresh token was denied with the slot free, because
// the admitted-token set held 2 x max_clients entries until each token
// expired. Five cycles, each with a fresh token from a fresh address, are
// all admitted now, and nothing is ever denied.

static void test_server_admits_sequential_fresh_tokens_past_twice_max_clients()
{
    static struct tc_server_t t;
    tc_server_setup( &t, 1, 8 );

    char address[64];
    for ( int cycle = 0; cycle < 5; cycle++ )
    {
        struct tc_client_t c;
        snprintf( address, sizeof( address ), "198.51.100.%d:56789", 10 + cycle );
        tc_client_init( &c, address );

        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        tc_mint_token( token_data, UINT64_C( 0x3000 ) + (uint64_t) cycle );
        check( rocketnet_client_protocol_connect( &c.client, token_data, t.time, TC_NOW ) );

        const int state = tc_drive( &t, &c, TC_NOW );
        if ( state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            printf( "cycle %d of 5 not admitted: client state %d\n", cycle + 1, state );
        check( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        check( rocketnet_client_slots_num_connected( t.server.client_slots ) == 1 );

        tc_disconnect( &t, &c, TC_NOW );
        check( rocketnet_client_slots_num_connected( t.server.client_slots ) == 0 );
        check( rocketnet_encryption_map_num_mappings( t.server.encryption_map ) == 0 );

        tc_client_destroy( &c );
    }

    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED] == 5 );
    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS] == 0 );
    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] == 0 );

    tc_server_teardown( &t );
}

REGISTER_UNIT_TEST( test_server_admits_sequential_fresh_tokens_past_twice_max_clients );

// One token, one live connection. A token a connected client holds is
// refused from any other address by the used-token history, before the
// signature verify or the open: a hash and a lookup. With the history flushed
// -- it holds one entry here, so a second client's binding evicts the first's
// -- the replay passes the history and the open, and the client-id check
// refuses it: never a second seat.

// The four clients are static: an unoptimised MSVC build stacks them all at
// once, past the default 1 MB stack. tc_client_init resets each in full.

#if ROCKETNET_DEV
static void test_server_refuses_a_token_connected_now()
{
    static struct tc_server_t t;
    tc_server_setup( &t, 2, 1 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    tc_mint_token( token_data, 0x4000ULL );

    static struct tc_client_t a;
    tc_client_init( &a, "198.51.100.20:56789" );
    check( rocketnet_client_protocol_connect( &a.client, token_data, t.time, TC_NOW ) );
    check( tc_drive( &t, &a, TC_NOW ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // the same token from a second address while the history binds it: no
    // verify, no open, no seat
    {
        static struct tc_client_t b;
        tc_client_init( &b, "198.51.100.22:56789" );
        check( rocketnet_client_protocol_connect( &b.client, token_data, t.time, TC_NOW ) );

        const struct rocketnet_crypto_tally_t tally_before = rocketnet_crypto_tally;
        check( tc_drive( &t, &b, TC_NOW ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] > 0 );
        check( rocketnet_crypto_tally.signature_verify == tally_before.signature_verify );
        check( rocketnet_crypto_tally.seal_open == tally_before.seal_open );
        check( rocketnet_client_slots_num_connected( t.server.client_slots ) == 1 );

        tc_client_destroy( &b );
    }

    // a second client's binding flushes the history's one entry

    static struct tc_client_t c;
    tc_client_init( &c, "198.51.100.21:56789" );

    uint8_t other_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    tc_mint_token( other_token, 0x4001ULL );
    check( rocketnet_client_protocol_connect( &c.client, other_token, t.time, TC_NOW ) );
    check( tc_drive( &t, &c, TC_NOW ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // the same token from a third address now passes the history and the
    // open, and the client id it carries is connected: refused, never seated
    {
        static struct tc_client_t d;
        tc_client_init( &d, "198.51.100.23:56789" );
        check( rocketnet_client_protocol_connect( &d.client, token_data, t.time, TC_NOW ) );

        const uint64_t used_before = t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED];
        check( tc_drive( &t, &d, TC_NOW ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] == used_before );
        check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED] > 0 );
        check( rocketnet_client_slots_num_connected( t.server.client_slots ) == 2 );

        tc_client_destroy( &d );
    }

    tc_client_destroy( &c );
    tc_client_destroy( &a );
    tc_server_teardown( &t );
}

REGISTER_UNIT_TEST( test_server_refuses_a_token_connected_now );

// The short expiry is the fast reject: the expire timestamp is in the signed
// connect data, read at a fixed offset before the cookie gate, so an expired
// token is refused before any MAC, signature verify or open runs.

static void test_server_rejects_an_expired_token_before_any_crypto()
{
    static struct tc_server_t t;
    tc_server_setup( &t, 1, 8 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    tc_mint_token( token_data, 0x5000ULL );

    struct tc_client_t c;
    tc_client_init( &c, "198.51.100.30:56789" );
    check( rocketnet_client_protocol_connect( &c.client, token_data, t.time, TC_NOW ) );

    // the server's token clock has reached the token's expire timestamp

    const uint64_t expired_now = ROCKETNET_TEST_CREATE_TS + TC_TOKEN_LIFETIME;
    const struct rocketnet_crypto_tally_t tally_before = rocketnet_crypto_tally;

    uint8_t out_packet[ROCKETNET_RECEIVE_CEILING];
    int out_bytes = 0;
    rocketnet_client_protocol_update( &c.client, t.time, out_packet, &out_bytes );
    check( out_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    int payload_bytes = 0;
    int payload_client_index = -1;
    check( rocketnet_server_protocol_process_packet( &t.server, &c.address, out_packet, out_bytes,
                                                     expired_now, t.time, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( response_bytes == 0 );
    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] == 1 );
    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] == 0 );
    check( rocketnet_crypto_tally.signature_verify == tally_before.signature_verify );
    check( rocketnet_crypto_tally.seal_open == tally_before.seal_open );
    check( rocketnet_crypto_tally.aead_open == tally_before.aead_open );
    check( rocketnet_encryption_map_num_mappings( t.server.encryption_map ) == 0 );

    // one second earlier the same request is answered with a cookie

    check( rocketnet_server_protocol_process_packet( &t.server, &c.address, out_packet, out_bytes,
                                                     expired_now - 1, t.time, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    tc_client_destroy( &c );
    tc_server_teardown( &t );
}

REGISTER_UNIT_TEST( test_server_rejects_an_expired_token_before_any_crypto );
#endif // #if ROCKETNET_DEV

// A token whose connection has closed may connect again inside its short
// expiry: the residual risk accepted by the design (docs/SESSION.md, Token
// reuse). This pins it, so a change to the rule is a change to this test.

static void test_server_admits_a_token_again_after_its_connection_closes()
{
    static struct tc_server_t t;
    tc_server_setup( &t, 1, 8 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    tc_mint_token( token_data, 0x6000ULL );

    for ( int pass = 0; pass < 2; pass++ )
    {
        struct tc_client_t c;
        tc_client_init( &c, "198.51.100.40:56789" );
        check( rocketnet_client_protocol_connect( &c.client, token_data, t.time, TC_NOW ) );
        check( tc_drive( &t, &c, TC_NOW ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

        tc_disconnect( &t, &c, TC_NOW );
        check( rocketnet_client_slots_num_connected( t.server.client_slots ) == 0 );

        tc_client_destroy( &c );
    }

    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED] == 2 );

    tc_server_teardown( &t );
}

REGISTER_UNIT_TEST( test_server_admits_a_token_again_after_its_connection_closes );

// A retransmitted response proves the same handshake, not fresh session
// activity. It cannot postpone the connected slot's idle timeout.
static void test_response_replay_does_not_refresh_connected_slot()
{
    struct tc_server_t t;
    tc_server_setup( &t, 1, 8 );

    struct tc_client_t c;
    tc_client_init( &c, "198.51.100.7:56789" );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    tc_mint_token( token, 9001 );
    check( rocketnet_client_protocol_connect( &c.client, token, t.time, TC_NOW ) );
    check( tc_drive( &t, &c, TC_NOW ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int index = rocketnet_encryption_map_find( t.server.encryption_map, &c.address );
    check( index >= 0 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( t.server.encryption_map, index );
    const double received = mapping->control->last_receive_time;
    uint8_t response[ROCKETNET_RECEIVE_CEILING], payload[ROCKETNET_RECEIVE_CEILING];

    int response_bytes = 0, payload_bytes = 0, client_index = -1;
    check( rocketnet_server_protocol_process_packet( &t.server, &c.address,
                                                     c.client.stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, TC_NOW,
                                                     received + 1.0, response, &response_bytes, payload, &payload_bytes, &client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( mapping->control->last_receive_time == received );

    rocketnet_server_protocol_update( &t.server, received + mapping->control->timeout_seconds );
    check( rocketnet_client_slots_num_connected( t.server.client_slots ) == 0 );

    tc_client_destroy( &c );
    tc_server_teardown( &t );
}

REGISTER_UNIT_TEST( test_response_replay_does_not_refresh_connected_slot );

// ---------------------------------------------------------------------------
// The request-claim table's common capacity check is O(1) after its bounded
// full-token index probe (sdk/rocketnet_token_tables.cpp;
// proofs/tla/RNSecurityTables.tla, ClaimRecord, ClaimRelease,
// FreeAndOrderPartitionSlots, LiveClaimNeverReplaced and
// CanRecordMatchesScan). A released claim goes back on the free list, live
// claims sit in a chronological slot array, and the capacity test reads the
// free stack and the order array's head: no walk over the slot table. Each case
// drives the table directly, with addresses built in memory and time an
// argument from 1000.0.
// ---------------------------------------------------------------------------

#define TC_CLAIM_CAPACITY           4
#define TC_CLAIM_DEADLINE_SECONDS   5.0

#if ROCKETNET_DEV
static void tc_claim_id( uint8_t * identifier, uint8_t value )
{
    memset( identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    identifier[0] = value;
    identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES - 1] = (uint8_t) ( 0xA5 ^ value );
}

static void tc_claim_addresses( struct rocketnet_address_t * holder, struct rocketnet_address_t * other )
{
    check( rocketnet_address_parse( holder, "198.51.100.7:50000" ) );
    check( rocketnet_address_parse( other, "198.51.100.8:50000" ) );
}

// four live claims fill the table; releasing two returns both slots, a
// second release and a release from another address return nothing, and
// two new tokens take the two free slots while the two unreleased claims
// still refuse a second address

static void test_claim_release_returns_slot_to_free_list()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, TC_CLAIM_CAPACITY );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    const double now = 1000.0;

    check( rocketnet_used_token_history_claim_free_count( history ) == TC_CLAIM_CAPACITY );

    uint8_t ids[6][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 6; i++ )
    {
        tc_claim_id( ids[i], (uint8_t) ( i + 1 ) );
    }

    for ( int i = 0; i < 4; i++ )
    {
        check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, now + TC_CLAIM_DEADLINE_SECONDS, now ) == 1 );
    }

    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    rocketnet_used_token_history_claim_release( history, ids[0], &holder );
    rocketnet_used_token_history_claim_release( history, ids[1], &holder );
    check( rocketnet_used_token_history_claim_free_count( history ) == 2 );

    // a second release of a released claim, and a release from an address
    // that does not hold the claim, change nothing

    rocketnet_used_token_history_claim_release( history, ids[0], &holder );
    rocketnet_used_token_history_claim_release( history, ids[2], &other );
    check( rocketnet_used_token_history_claim_free_count( history ) == 2 );

    check( rocketnet_used_token_history_claim_can_record( history, ids[4], now ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[4], &holder, now + TC_CLAIM_DEADLINE_SECONDS, now ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[5], &holder, now + TC_CLAIM_DEADLINE_SECONDS, now ) == 1 );
    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    // the two unreleased claims still hold their tokens for their address

    for ( int i = 2; i < 4; i++ )
    {
        check( rocketnet_used_token_history_claim_check( history, ids[i], &other, now ) == 0 );
        check( rocketnet_used_token_history_claim_check( history, ids[i], &holder, now ) == 1 );
    }

    // the released tokens hold nothing

    check( rocketnet_used_token_history_claim_check( history, ids[0], &other, now ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[1], &other, now ) == 1 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_claim_release_returns_slot_to_free_list );

// a full table, nothing released: once time passes the first claim's
// deadline, and only that one, a new token takes the order list's head and
// every other claim stays live for its own address

static void test_claim_expired_slot_reused_from_order_head()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, TC_CLAIM_CAPACITY );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t ids[6][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 6; i++ )
    {
        tc_claim_id( ids[i], (uint8_t) ( i + 1 ) );
    }

    for ( int i = 0; i < 4; i++ )
    {
        const double at = 1000.0 + (double) i;
        check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, at + TC_CLAIM_DEADLINE_SECONDS, at ) == 1 );
    }

    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    // past the first claim's deadline (1005.0) only

    const double later = 1005.0;
    check( rocketnet_used_token_history_claim_can_record( history, ids[4], later ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[4], &other, later + TC_CLAIM_DEADLINE_SECONDS, later ) == 1 );
    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    // the first claim's slot went to the new token

    check( rocketnet_used_token_history_claim_check( history, ids[0], &other, later ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[4], &holder, later ) == 0 );
    check( rocketnet_used_token_history_claim_check( history, ids[4], &other, later ) == 1 );

    // every other claim is live for its own address and refuses another

    for ( int i = 1; i < 4; i++ )
    {
        check( rocketnet_used_token_history_claim_check( history, ids[i], &holder, later ) == 1 );
        check( rocketnet_used_token_history_claim_check( history, ids[i], &other, later ) == 0 );
    }

    // and the table is full of live claims again

    check( rocketnet_used_token_history_claim_can_record( history, ids[5], later ) == 0 );
    check( rocketnet_used_token_history_claim_record( history, ids[5], &other, later + TC_CLAIM_DEADLINE_SECONDS, later ) == 0 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_claim_expired_slot_reused_from_order_head );

// a table full of live claims refuses a new token at both the capacity test
// and the record, answers yes for a token already in the table, and evicts
// nothing

static void test_claim_full_of_live_claims_refuses()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, TC_CLAIM_CAPACITY );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    const double now = 1000.0;

    uint8_t ids[5][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 5; i++ )
    {
        tc_claim_id( ids[i], (uint8_t) ( i + 1 ) );
    }

    for ( int i = 0; i < 4; i++ )
    {
        check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, now + TC_CLAIM_DEADLINE_SECONDS, now ) == 1 );
    }

    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    check( rocketnet_used_token_history_claim_can_record( history, ids[4], now + 1.0 ) == 0 );
    check( rocketnet_used_token_history_claim_record( history, ids[4], &other, now + 1.0 + TC_CLAIM_DEADLINE_SECONDS, now + 1.0 ) == 0 );
    check( rocketnet_used_token_history_claim_can_record( history, ids[0], now + 1.0 ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[0], &holder, now + 1.0 + TC_CLAIM_DEADLINE_SECONDS, now + 1.0 ) == 1 );

    // nothing evicted: every claim still refuses another address, and the
    // refused token holds nothing

    for ( int i = 0; i < 4; i++ )
    {
        check( rocketnet_used_token_history_claim_check( history, ids[i], &other, now + 1.0 ) == 0 );
    }

    check( rocketnet_used_token_history_claim_check( history, ids[4], &holder, now + 1.0 ) == 1 );
    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_claim_full_of_live_claims_refuses );

// The server's time does not go backwards (the transport clamps a step
// backwards to the last good time) and every deadline is that time plus one
// fixed handshake deadline, so the order list is deadline order. Should a
// deadline still arrive out of order at this interface, a later claim that
// has expired waits behind the live head: its reuse is delayed until the
// head expires, and the live head is never replaced.

static void test_claim_out_of_order_deadline_delays_reuse_only()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 2 );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t ids[4][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 4; i++ )
    {
        tc_claim_id( ids[i], (uint8_t) ( i + 1 ) );
    }

    check( rocketnet_used_token_history_claim_record( history, ids[0], &holder, 1010.0, 1000.0 ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[1], &holder, 1003.0, 1000.0 ) == 1 );

    // the second claim has expired, the head has not: reuse waits

    check( rocketnet_used_token_history_claim_can_record( history, ids[2], 1004.0 ) == 0 );
    check( rocketnet_used_token_history_claim_record( history, ids[2], &other, 1009.0, 1004.0 ) == 0 );
    check( rocketnet_used_token_history_claim_check( history, ids[0], &other, 1004.0 ) == 0 );

    // once the head expires it is reused first, then the expired claim behind it

    check( rocketnet_used_token_history_claim_can_record( history, ids[2], 1010.0 ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[2], &other, 1015.0, 1010.0 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[0], &other, 1010.0 ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[3], &other, 1015.0, 1010.0 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[2], &holder, 1010.0 ) == 0 );
    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_claim_out_of_order_deadline_delays_reuse_only );

// Re-recording an expired identifier moves it behind the remaining claims;
// the next expired head can then be reused without replacing the renewed one.
static void test_claim_expired_identifier_moves_to_order_tail()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, TC_CLAIM_CAPACITY );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t ids[5][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 5; i++ )
    {
        tc_claim_id( ids[i], (uint8_t) ( i + 1 ) );
    }

    for ( int i = 0; i < TC_CLAIM_CAPACITY; i++ )
    {
        const double at = 1000.0 + (double) i;
        check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, at + TC_CLAIM_DEADLINE_SECONDS, at ) == 1 );
    }

    check( rocketnet_used_token_history_claim_record( history, ids[0], &holder, 1010.0, 1005.0 ) == 1 );
    check( rocketnet_used_token_history_claim_can_record( history, ids[4], 1006.0 ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[4], &other, 1011.0, 1006.0 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[0], &other, 1006.0 ) == 0 );
    check( rocketnet_used_token_history_claim_check( history, ids[1], &other, 1006.0 ) == 1 );
    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_claim_expired_identifier_moves_to_order_tail );

// Find full identifiers that share one keyed-index start, then prove that
// each remains distinct and that the full-table capacity check does only a
// bounded index probe plus the O(1) chronological-head query.
static void test_claim_flat_index_full_key_collisions_are_bounded()
{
    const int capacity = 4;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t colliding[5][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    int found = 0;
    int wanted_start = -1;
    for ( int candidate = 0; candidate < 10000 && found < 5; ++candidate )
    {
        uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        memset( identifier, 0, sizeof( identifier ) );
        identifier[0] = (uint8_t) candidate;
        identifier[1] = (uint8_t) ( candidate >> 8 );
        identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES - 1] = (uint8_t) ( candidate ^ 0xA5 );
        const int start = rocketnet_dev_token_claim_index_start( history, identifier );
        if ( wanted_start == -1 )
        {
            wanted_start = start;
        }

        if ( start == wanted_start )
        {
            memcpy( colliding[found++], identifier, sizeof( identifier ) );
        }
    }

    check( found == 5 );

    for ( int i = 0; i < capacity; ++i )
    {
        check( rocketnet_used_token_history_claim_record( history, colliding[i], &holder, 1010.0, 1000.0 ) == 1 );
    }

    for ( int i = 0; i < capacity; ++i )
    {
        check( rocketnet_used_token_history_claim_check( history, colliding[i], &holder, 1001.0 ) == 1 );
        check( rocketnet_used_token_history_claim_check( history, colliding[i], &other, 1001.0 ) == 0 );
    }

    rocketnet_dev_token_table_reset_operation_counts( history );
    check( rocketnet_used_token_history_claim_can_record( history, colliding[4], 1001.0 ) == 0 );
    check( rocketnet_dev_token_claim_probe_steps( history ) <= (uint64_t) ( capacity * 2 ) );

    rocketnet_used_token_history_destroy( history );
}

// Releasing an interior claim is one index removal and touches no other
// claim; the released slot is reused at once, and the claim it held is gone.
static void test_claim_interior_churn_is_bounded()
{
    const int capacity = 4;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t ids[6][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 6; ++i )
    {
        tc_claim_id( ids[i], (uint8_t) ( 0x50 + i ) );
    }

    for ( int i = 0; i < capacity; ++i )
    {
        check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, 1010.0, 1000.0 ) == 1 );
    }

    rocketnet_dev_token_table_reset_operation_counts( history );
    rocketnet_used_token_history_claim_release( history, ids[2], &holder );
    check( rocketnet_dev_token_claim_probe_steps( history ) <= (uint64_t) ( capacity * 2 ) );
    check( rocketnet_dev_token_claim_slot_steps( history ) == 0 );
    check( rocketnet_used_token_history_claim_free_count( history ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[2], &other, 1001.0 ) == 1 );

    check( rocketnet_used_token_history_claim_record( history, ids[5], &holder, 1010.0, 1000.0 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[5], &other, 1001.0 ) == 0 );
    check( rocketnet_used_token_history_claim_record( history, ids[4], &holder, 1010.0, 1000.0 ) == 0 );

    rocketnet_used_token_history_destroy( history );
}

// A compaction of a full, wrapped order ring keeps every live claim in order.
// Capacity 4 gives an 8-entry ring. A, B, C, D fill the table (entries 0-3);
// A is released and E takes its slot (entry 4); a full-table check drops A's
// tombstone, so the head is entry 1. Four release-and-reclaim cycles of E fill
// entries 5, 6, 7 and 0: the ring is full and wrapped, with E's live entry at
// physical 0. D is released and F claims its slot, which compacts the ring.
// Once every claim has expired, four fresh tokens must each take one; a live
// claim the compaction lost (E) would never be found as the oldest again.
static void test_claim_wrapped_compaction_keeps_live_claims()
{
    const int capacity = 4;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t ids[10][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 10; ++i )
    {
        tc_claim_id( ids[i], (uint8_t) ( 0x70 + i ) );
    }

    for ( int i = 0; i < 4; ++i )
    {
        check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, 1010.0, 1000.0 ) == 1 );
    }

    rocketnet_used_token_history_claim_release( history, ids[0], &holder );
    check( rocketnet_used_token_history_claim_record( history, ids[4], &holder, 1010.0, 1000.0 ) == 1 );
    check( rocketnet_used_token_history_claim_can_record( history, ids[6], 1000.0 ) == 0 );

    for ( int cycle = 0; cycle < 4; ++cycle )
    {
        rocketnet_used_token_history_claim_release( history, ids[4], &holder );
        check( rocketnet_used_token_history_claim_record( history, ids[4], &holder, 1010.0, 1000.0 ) == 1 );
    }

    rocketnet_used_token_history_claim_release( history, ids[3], &holder );
    check( rocketnet_used_token_history_claim_record( history, ids[5], &holder, 1010.0, 1000.0 ) == 1 );

    // B, C, E and F are live, and each still refuses another address

    const int live[4] = { 1, 2, 4, 5 };
    for ( int i = 0; i < 4; ++i )
    {
        check( rocketnet_used_token_history_claim_check( history, ids[live[i]], &other, 1001.0 ) == 0 );
    }

    // all four expire: four fresh tokens take all four slots

    for ( int i = 6; i < 10; ++i )
    {
        check( rocketnet_used_token_history_claim_record( history, ids[i], &holder, 1021.0, 1011.0 ) == 1 );
    }

    rocketnet_used_token_history_destroy( history );
}

// A full table of live claims is refused in constant time however much it
// has churned: the head of the insertion order is read, never the table.
static void test_claim_full_refusal_is_constant_time()
{
    const int capacity = 64;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int round = 0; round < 8; ++round )
    {
        for ( int i = 0; i < capacity; ++i )
        {
            tc_claim_id( id, (uint8_t) i );
            id[1] = (uint8_t) round;
            check( rocketnet_used_token_history_claim_record( history, id, &holder, 1000.0 + 10.0 * ( round + 1 ), 1000.0 + 10.0 * round ) == 1 );
        }
    }

    tc_claim_id( id, 0xEE );
    rocketnet_dev_token_table_reset_operation_counts( history );
    for ( int i = 0; i < 1000; ++i )
        check( rocketnet_used_token_history_claim_can_record( history, id, 1075.0 ) == 0 );
    check( rocketnet_dev_token_claim_slot_steps( history ) <= (uint64_t) ( 2 * capacity ) );

    rocketnet_used_token_history_destroy( history );
}

// Equal deadlines retain insertion chronology: expiry reuses the oldest
// physical owner first, without making the other full-token claim disappear.
static void test_claim_equal_deadlines_reuse_in_insertion_order()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 2 );
    check( history );

    struct rocketnet_address_t holder, other;
    tc_claim_addresses( &holder, &other );

    uint8_t ids[3][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 3; ++i )
    {
        tc_claim_id( ids[i], (uint8_t) ( 0x60 + i ) );
    }

    check( rocketnet_used_token_history_claim_record( history, ids[0], &holder, 1005.0, 1000.0 ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[1], &holder, 1005.0, 1000.0 ) == 1 );
    check( rocketnet_used_token_history_claim_record( history, ids[2], &other, 1010.0, 1005.0 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[0], &other, 1005.0 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, ids[1], &other, 1004.0 ) == 0 );
    check( rocketnet_used_token_history_claim_check( history, ids[2], &holder, 1005.0 ) == 0 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_claim_flat_index_full_key_collisions_are_bounded );
REGISTER_UNIT_TEST( test_claim_interior_churn_is_bounded );
REGISTER_UNIT_TEST( test_claim_full_refusal_is_constant_time );
REGISTER_UNIT_TEST( test_claim_wrapped_compaction_keeps_live_claims );
REGISTER_UNIT_TEST( test_claim_equal_deadlines_reuse_in_insertion_order );
#endif // #if ROCKETNET_DEV
