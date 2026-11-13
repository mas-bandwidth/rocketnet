/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport's public calls at their refusals
// (sdk/rocketnet_client_transport.cpp): a NULL argument is a counted no-op
// that returns before it touches anything (rocketnet_api_null_guard counts
// it and asserts; the assert hook here counts the assert and returns, as a
// build without asserts does), and a call the connection's state refuses
// changes nothing. Each client is step-driven over the fake client
// transport (tests/fake_client_driver.h): no socket, no thread; time is an
// argument starting at 1000.0. Each failing check prints the fault first.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_address.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

extern void rocketnet_client_transport_seed_refusal_state_for_test( rocketnet_client_transport_t *, uint64_t, const uint8_t *, const uint8_t *, const uint8_t * );

#define R2_API_PAYLOAD_BYTES 40

// ---------------------------------------------------------------------------
// a step-driven client over the fake driver, on the I/O core

static double r2_api_now = 1000.0;
static double r2_api_time( void * ) { return r2_api_now; }

static rocketnet_client_transport_t * r2_api_client( rocketnet_fake_client_transport_io_t * fake, bool connected )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );
    rocketnet_write_signed_connect_data( &signed_fields, token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = r2_api_time;

    r2_api_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &rocketnet_fake_client_operations_io, fake );
    check( transport != NULL );
    if ( transport && connected )
    {
        rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value,
                                                                rocketnet_test_salt, rocketnet_test_session_s2c_key );
        check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    }
    return transport;
}

// the assert hook: a NULL argument's assert is counted and returns, so the
// call goes on to its counted no-op return as in a build without asserts

static int r2_api_asserts = 0;

static void r2_api_assert( const char *, const char *, const char *, int )
{
    r2_api_asserts++;
}

static rocketnet_assert_function_t r2_api_saved_assert = NULL;

static void r2_api_misuse_begin()
{
    rocketnet_quiet( true );
    r2_api_saved_assert = rn_assert_function;
    r2_api_asserts = 0;
    rocketnet_assert_function( r2_api_assert );
}

static void r2_api_misuse_end()
{
    rocketnet_assert_function( r2_api_saved_assert );
    rocketnet_quiet( false );
}

// one NULL-argument call: it returned the refusal and counted exactly one misuse
static void r2_api_misuse_counted( const char * id, const char * call, bool refused, uint64_t misuses_before )
{
    const uint64_t misuses = rocketnet_api_misuse_count() - misuses_before;
    if ( !refused || misuses != 1 )
    {
        printf( "mutation gap %s: %s did not return its counted no-op (refusal returned: %d, misuses counted: %llu, not 1)\n", id, call, (int) refused,
                (unsigned long long) misuses );
    }
    check( refused );
    check( misuses == 1 );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7315, 2/7601, 2/7617, 2/7712, 2/7751, 2/7752, 2/7776, 2/7789, 2/7790, 2/7792, 2/7793,
// 2/7877, 2/7878, 2/7881, 2/7885 (each public call's NULL argument is a counted no-op)

static void test_gaps_r2_ct_api_null_arguments_are_counted_no_ops()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_api_client( fake, true );

    const int sent_before = fake->num_sent;

    r2_api_misuse_begin();

    uint64_t before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7315", "rocketnet_client_transport_stopped( NULL )", !rocketnet_client_transport_stopped( NULL ), before );

    // send_packet with NULL data: nothing sealed, nothing sent
    before = rocketnet_api_misuse_count();
    rocketnet_client_transport_send_packet( transport, NULL, R2_API_PAYLOAD_BYTES );
    r2_api_misuse_counted( "hunt 2/7601", "rocketnet_client_transport_send_packet( client, NULL, 40 )", fake->num_sent == sent_before, before );

    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7617", "rocketnet_client_transport_start_packet( client, 40, NULL )",
                           rocketnet_client_transport_start_packet( transport, R2_API_PAYLOAD_BYTES, NULL ) == NULL, before );

    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7712", "rocketnet_client_transport_finish_packet( client, NULL, 40 )",
                           rocketnet_client_transport_finish_packet( transport, NULL, R2_API_PAYLOAD_BYTES ) == 0, before );

    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7751,7752", "rocketnet_client_transport_receive_packet( client, NULL, 100 )",
                           rocketnet_client_transport_receive_packet( transport, NULL, 100 ) == 0, before );

    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7776", "rocketnet_client_transport_payload( client, 0, NULL )",
                           rocketnet_client_transport_payload( transport, 0, NULL ) == NULL, before );

    uint8_t * const * payload_data = NULL;
    const int * payload_bytes = NULL;
    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7789,7790", "rocketnet_client_transport_payloads( client, NULL, &bytes )",
                           rocketnet_client_transport_payloads( transport, NULL, &payload_bytes ) == 0 && payload_bytes == NULL, before );

    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7792,7793", "rocketnet_client_transport_payloads( client, &data, NULL )",
                           rocketnet_client_transport_payloads( transport, &payload_data, NULL ) == 0 && payload_data == NULL, before );

    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7877,7878", "rocketnet_client_transport_acked_packets( client, NULL )",
                           rocketnet_client_transport_acked_packets( transport, NULL ) == 0, before );

    before = rocketnet_api_misuse_count();
    r2_api_misuse_counted( "hunt 2/7881", "rocketnet_client_transport_rtt( NULL )", rocketnet_client_transport_rtt( NULL ) == 0.0, before );

    before = rocketnet_api_misuse_count();
    rocketnet_client_transport_stats( transport, NULL );
    r2_api_misuse_counted( "hunt 2/7885", "rocketnet_client_transport_stats( client, NULL )", true, before );

    r2_api_misuse_end();

    // and nothing above reached the wire
    rocketnet_client_transport_send_packets( transport );
    if ( fake->num_sent != sent_before )
    {
        printf( "mutation gap hunt 2/7601,7617,7712: refused calls put %d datagram(s) on the wire\n", fake->num_sent - sent_before );
    }
    check( fake->num_sent == sent_before );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_api_null_arguments_are_counted_no_ops );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7782, 2/7783 (an out-of-range payload index reports 0 bytes)

static void test_gaps_r2_ct_api_payload_out_of_range_reports_zero_bytes()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_api_client( fake, true );

    check( rocketnet_client_transport_num_payloads( transport ) == 0 );

    const int indexes[3] = { -1, 0, 5 };
    for ( int i = 0; i < 3; i++ )
    {
        int bytes = -1;
        const uint8_t * payload = rocketnet_client_transport_payload( transport, indexes[i], &bytes );
        if ( payload != NULL || bytes != 0 )
        {
            printf( "mutation gap hunt 2/7782,7783: payload index %d of an empty batch returned %s and %d byte(s), not NULL and 0\n", indexes[i],
                    payload ? "a pointer" : "NULL", bytes );
        }
        check( payload == NULL );
        check( bytes == 0 );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_api_payload_out_of_range_reports_zero_bytes );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7630 (a payload start before the connection is up takes no slot)

static void test_gaps_r2_ct_api_start_before_connected_takes_no_slot()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_api_client( fake, false );
    check( rocketnet_client_transport_state( transport ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( transport, R2_API_PAYLOAD_BYTES, &context );
    if ( frame != NULL )
    {
        printf( "mutation gap hunt 2/7630: a client in state %d (not connected) was given a frame for a payload\n", rocketnet_client_transport_state( transport ) );
        rocketnet_client_transport_abort_packet( transport, &context );
    }
    check( frame == NULL );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_api_start_before_connected_takes_no_slot );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7610, 2/7621, 2/7725 (after stop the caller's sends change nothing)
//
// Once stop has handed the rings to the worker, a start is refused, the
// convenience send sends nothing, and a send_packets hands nothing off: the
// datagrams on the wire after the stop completes are the same whatever the
// caller calls in between.

static void test_gaps_r2_ct_api_sends_after_stop_change_nothing()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_api_client( fake, true );

    rocketnet_client_transport_stop( transport );
    rn_test_client_transport_wait_stopped( transport );
    const int sent_after_stop = fake->num_sent;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( transport, R2_API_PAYLOAD_BYTES, &context );
    if ( frame != NULL )
    {
        printf( "mutation gap hunt 2/7621: a stopped client was given a frame in a ring its worker owns\n" );
    }
    check( frame == NULL );

    const uint8_t payload[R2_API_PAYLOAD_BYTES] = { 0x44 };
    rocketnet_client_transport_send_packet( transport, payload, (int) sizeof( payload ) );
    rocketnet_client_transport_send_packets( transport );

    if ( fake->num_sent != sent_after_stop )
    {
        printf( "mutation gap hunt 2/7610,7621,7725: a stopped client put %d more datagram(s) on the wire\n", fake->num_sent - sent_after_stop );
    }
    check( fake->num_sent == sent_after_stop );

    rocketnet_client_transport_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_api_sends_after_stop_change_nothing );


// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7763, 2/7764 (a payload larger than the caller's buffer is refused, counted and logged)
//
// Two payloads arrive from the server, 20 bytes then 2. Read into an 8-byte
// buffer, the first is refused whole: RECEIVE_CURSOR_TOO_SMALL counts it,
// a SPAM line says so, and the cursor returns the second.

static int r2_api_cursor_lines = 0;

static void r2_api_cursor_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( level == ROCKETNET_LOG_LEVEL_SPAM && strstr( message, "client receive cursor refused payload" ) != NULL )
    {
        r2_api_cursor_lines++;
    }
}

static int r2_api_server_payload( uint8_t * packet, uint64_t sequence, uint8_t fill, int payload_bytes )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 32];
    memset( plaintext, 0, sizeof( plaintext ) );
    memset( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, fill, (size_t) payload_bytes );
    return rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, rocketnet_test_filter_value, sequence, NULL, plaintext,
                                          ROCKETNET_ACK_BLOCK_MIN_BYTES + payload_bytes, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                          rocketnet_test_session_s2c_key );
}

static void test_gaps_r2_ct_api_receive_refuses_a_payload_larger_than_the_buffer()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_api_client( fake, true );

    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int bytes = r2_api_server_payload( packet, 100, 0xA1, 20 );
    check( bytes > 0 );
    rocketnet_fake_client_push( fake, &server, packet, bytes );
    bytes = r2_api_server_payload( packet, 101, 0xB2, 2 );
    check( bytes > 0 );
    rocketnet_fake_client_push( fake, &server, packet, bytes );

    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    check( io != NULL );
    if ( io )
    {
        rocketnet_client_io_step( io, false );
    }
    r2_api_now = 1000.01;
    rocketnet_client_transport_update( transport, r2_api_now );
    check( rocketnet_client_transport_num_payloads( transport ) == 2 );

    const uint64_t too_small = rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL];

    r2_api_cursor_lines = 0;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( r2_api_cursor_log, NULL );

    uint8_t copy[8];
    memset( copy, 0, sizeof( copy ) );
    const int read = rocketnet_client_transport_receive_packet( transport, copy, (int) sizeof( copy ) );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    check( read == 2 && copy[0] == 0xB2 && copy[1] == 0xB2 );

    const uint64_t counted = rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] - too_small;
    if ( counted != 1 || r2_api_cursor_lines != 1 )
    {
        printf( "mutation gap hunt 2/7763,7764: a 20-byte payload refused by an 8-byte buffer was counted %llu time(s) in RECEIVE_CURSOR_TOO_SMALL and logged %d time(s), not once each\n",
                (unsigned long long) counted, r2_api_cursor_lines );
    }
    check( counted == 1 );
    check( r2_api_cursor_lines == 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_api_receive_refuses_a_payload_larger_than_the_buffer );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7725 (after stop the caller's flush leaves the worker's ring alone)
//
// On the BASE path stop gives ring 0 to the receive worker, which sends what
// it holds as it finishes the stop. A payload sealed before the stop is in
// that ring: a send_packets between the stop and its completion must not
// send it from the caller's thread (the worker owns the ring), and the
// payload leaves once in all.

static void test_gaps_r2_ct_api_base_flush_after_stop_leaves_the_ring_to_the_worker()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );
    rocketnet_write_signed_connect_data( &signed_fields, token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = r2_api_time;
    r2_api_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &rocketnet_fake_client_operations_base, fake );
    check( transport != NULL );
    rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value, rocketnet_test_salt,
                                                            rocketnet_test_session_s2c_key );
    check( rocketnet_client_transport_client_io( transport ) == NULL );

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( transport, R2_API_PAYLOAD_BYTES, &context );
    check( frame != NULL );
    if ( frame )
    {
        memset( frame + context.payload_index, 0x55, R2_API_PAYLOAD_BYTES );
        check( rocketnet_client_transport_finish_packet( transport, &context, R2_API_PAYLOAD_BYTES ) > 0 );
    }

    const int sent_before = fake->num_sent;
    rocketnet_client_transport_stop( transport );
    const int sent_at_stop = fake->num_sent;

    rocketnet_client_transport_send_packets( transport );
    if ( fake->num_sent != sent_at_stop )
    {
        printf( "mutation gap hunt 2/7725: a send_packets after stop sent %d datagram(s) from the caller's thread out of the ring the worker now owns\n",
                fake->num_sent - sent_at_stop );
    }
    check( fake->num_sent == sent_at_stop );

    rn_test_client_transport_wait_stopped( transport );

    int payloads = 0;
    for ( int i = sent_before; i < fake->num_sent; i++ )
    {
        if ( fake->sent[i].bytes > R2_API_PAYLOAD_BYTES )
        {
            payloads++;
        }
    }
    if ( payloads > 1 )
    {
        printf( "mutation gap hunt 2/7725: the payload sealed before stop left %d times\n", payloads );
    }
    check( payloads <= 1 );

    rocketnet_client_transport_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_api_base_flush_after_stop_leaves_the_ring_to_the_worker );
#endif // #if ROCKETNET_DEV
