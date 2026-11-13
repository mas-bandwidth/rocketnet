/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport's refusals no test reached (sdk/rocketnet_client_transport.cpp):
// every allocation of a create failing in turn, the step-driven creates
// given no driver, an out-of-range step_driven, a NULL buffer read while a
// payload waits, and the development seams' own refusals. Each client is
// step-driven over the fake client transport (tests/fake_client_driver.h)
// or stands in its socket and thread by the memory plan seam: no socket, no
// thread; time is an argument starting at 1000.0. Each failing check prints
// the fault first.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "loopback_hub.h"

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_address.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_counters.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

extern void rocketnet_client_transport_seed_refusal_state_for_test( rocketnet_client_transport_t *, uint64_t, const uint8_t *, const uint8_t *, const uint8_t * );
extern void rocketnet_client_transport_set_memory_plan_seam_for_test( bool enable );
extern uint16_t rocketnet_client_transport_test_bound_port( struct rocketnet_client_transport_t * client_transport );

#define R2_REST_PAYLOAD_BYTES 40

// ---------------------------------------------------------------------------
// a token for 127.0.0.1:40000, and a step-driven client over the fake driver

static double r2_rest_now = 1000.0;
static double r2_rest_time( void * ) { return r2_rest_now; }

static void r2_rest_token( uint8_t * token )
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
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static rocketnet_client_transport_t * r2_rest_client( rocketnet_fake_client_transport_io_t * fake, const rocketnet_client_transport_io_operations_t * operations )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_rest_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = r2_rest_time;

    r2_rest_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, operations, fake );
    check( transport != NULL );
    if ( transport )
    {
        rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value, rocketnet_test_salt,
                                                                rocketnet_test_session_s2c_key );
        check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    }
    return transport;
}

// one payload sealed into the fill ring and left there
static void r2_rest_fill_one( rocketnet_client_transport_t * transport )
{
    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( transport, R2_REST_PAYLOAD_BYTES, &context );
    check( frame != NULL );
    if ( frame )
    {
        memset( frame + context.payload_index, 0x5A, R2_REST_PAYLOAD_BYTES );
        check( rocketnet_client_transport_finish_packet( transport, &context, R2_REST_PAYLOAD_BYTES ) > 0 );
    }
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6849, 2/6855, 2/6857, 2/6860 (a step-driven create with no driver is refused)
//
// Each step-driven create needs the caller's driver: given none (or, at a
// timestamp, the timestamp 0) it returns NULL before anything is created.
// Were it to go on, the create would open the real backend's socket.

static void r2_rest_refused( const char * id, const char * call, rocketnet_client_transport_t * transport, int status, int status_before )
{
    if ( transport != NULL || status != status_before )
    {
        printf( "mutation gap %s: %s returned %s (status %d), not NULL with the status untouched: it went on without the caller's driver\n", id, call,
                transport ? "a transport" : "NULL", status );
    }
    check( transport == NULL );
    check( status == status_before );
    if ( transport )
    {
        rn_test_client_transport_stop_and_destroy( transport );
    }
}

static void test_gaps_r2_ct_rest_step_driven_creates_without_a_driver_are_refused()
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_rest_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = r2_rest_time;
    r2_rest_now = 1000.0;

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    rocketnet_quiet( true );

    r2_rest_refused( "hunt 2/6849", "rocketnet_client_transport_create_step_driven( operations NULL )",
                     rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, NULL, fake ), 0, 0 );

    int status = 77;
    r2_rest_refused( "hunt 2/6855", "rocketnet_client_transport_create_step_driven_at_timestamp_internal( operations NULL )",
                     rocketnet_client_transport_create_step_driven_at_timestamp_internal( NULL, token, &config, 1000.0, 1000, NULL, fake, &status ), status, 77 );

    status = 77;
    r2_rest_refused( "hunt 2/6855", "rocketnet_client_transport_create_step_driven_at_timestamp_internal( timestamp 0 )",
                     rocketnet_client_transport_create_step_driven_at_timestamp_internal( NULL, token, &config, 1000.0, 0, &rocketnet_fake_client_operations_io, fake, &status ),
                     status, 77 );

    r2_rest_refused( "hunt 2/6857", "rocketnet_client_transport_create_step_driven_current( operations NULL )",
                     rocketnet_client_transport_create_step_driven_current( NULL, token, &config, 1000.0, NULL, fake ), 0, 0 );

    status = 77;
    r2_rest_refused( "hunt 2/6860", "rocketnet_client_transport_create_step_driven_internal( operations NULL )",
                     rocketnet_client_transport_create_step_driven_internal( NULL, token, &config, 1000.0, NULL, fake, &status ), status, 77 );

    rocketnet_quiet( false );

    check( fake->num_sent == 0 );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_rest_step_driven_creates_without_a_driver_are_refused );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6964 (an out-of-range step_driven says so in the status)
//
// step_driven is 0 or 1. Any other value is refused before a transport is
// chosen, with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE through the status.

static void test_gaps_r2_ct_rest_step_driven_out_of_range_reports_its_status()
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_rest_token( token );

    const uint64_t values[2] = { 2, UINT64_MAX };
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_transport_config_t config;
        rocketnet_client_transport_default_config( &config );
        config.step_driven = values[i];

        int status = ROCKETNET_STATUS_OK;
        rocketnet_quiet( true );
        rocketnet_client_transport_t * transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, &status );
        rocketnet_quiet( false );

        if ( transport != NULL || status != ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE )
        {
            printf( "mutation gap hunt 2/6964: a create with step_driven %llu returned %s with status %d, not NULL with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE (%d)\n",
                    (unsigned long long) values[i], transport ? "a transport" : "NULL", status, (int) ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
        }
        check( transport == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
        if ( transport )
        {
            rn_test_client_transport_stop_and_destroy( transport );
        }
    }
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_rest_step_driven_out_of_range_reports_its_status );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6740, 2/7095, 2/7096, 2/7108, 2/7109, 2/7223, 2/7224 (every failed allocation of a create is OUT_OF_MEMORY)
// mutation gap: hunt 2/7119 (the seam client counts no send failure)
//
// A legacy-path client (a link simulator setting, with drop and reverse
// patterns) under the memory plan seam: the seam's I/O client, socket and
// wake and its thread record stand in for the real ones. A create that
// succeeds makes N allocations; failing each of them in turn, the create
// returns NULL, says ROCKETNET_STATUS_OUT_OF_MEMORY through a status
// pointer (and writes nothing without one) and leaves nothing allocated.

struct r2_rest_oom_t
{
    int live;
    int calls;
    int fail_at; // 1-based; 0 fails none
};

static r2_rest_oom_t r2_rest_oom;

static void * r2_rest_oom_alloc( void * context, size_t bytes )
{
    (void) context;
    r2_rest_oom.calls++;
    if ( r2_rest_oom.fail_at != 0 && r2_rest_oom.calls == r2_rest_oom.fail_at )
    {
        return NULL;
    }
    void * pointer = malloc( bytes );
    if ( pointer )
    {
        r2_rest_oom.live++;
    }
    return pointer;
}

static void r2_rest_oom_free( void * context, void * pointer )
{
    (void) context;
    if ( pointer )
    {
        r2_rest_oom.live--;
        free( pointer );
    }
}

static void r2_rest_oom_config( struct rocketnet_client_transport_config_t * config )
{
    rocketnet_client_transport_default_config( config );
    snprintf( config->transport, sizeof( config->transport ), "%s", "portable" );
    config->step_driven = 0;
    config->simulated_latency_ms = 50;
    config->simulated_drop_every_nth = 3;
    config->simulated_reverse_window = 2;
}

static void test_gaps_r2_ct_rest_every_failed_create_allocation_is_out_of_memory()
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_rest_token( token );

    struct rocketnet_client_transport_config_t config;
    r2_rest_oom_config( &config );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    memset( &r2_rest_oom, 0, sizeof( r2_rest_oom ) );
    int status = -1;
    check( rocketnet_init_internal( NULL, r2_rest_oom_alloc, r2_rest_oom_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    rocketnet_client_transport_set_memory_plan_seam_for_test( true );
    rocketnet_quiet( true );

    // the create that succeeds: its allocations are counted

    const int live_before = r2_rest_oom.live;
    r2_rest_oom.calls = 0;
    r2_rest_oom.fail_at = 0;
    status = -1;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, &status );
    const int allocations = r2_rest_oom.calls;
    check( transport != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( allocations > 0 );
    if ( transport )
    {
        check( rocketnet_client_transport_client_io( transport ) == NULL ); // the legacy path: the seam's thread record

        const uint64_t send_failed = rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_SEND_FAILED];
        if ( send_failed != 0 )
        {
            printf( "mutation gap hunt 2/7119: a seam client that sent nothing reads SEND_FAILED %llu, not 0\n", (unsigned long long) send_failed );
        }
        check( send_failed == 0 );

        rn_test_client_transport_stop_and_destroy( transport );
    }
    check( r2_rest_oom.live == live_before );

    // each allocation failing in turn, with a status pointer and without one

    int refused = 0;
    for ( int fail_at = 1; fail_at <= allocations; fail_at++ )
    {
        for ( int with_status = 1; with_status >= 0; with_status-- )
        {
            r2_rest_oom.calls = 0;
            r2_rest_oom.fail_at = fail_at;
            status = ROCKETNET_STATUS_OK;
            transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, with_status ? &status : NULL );
            r2_rest_oom.fail_at = 0;

            const bool out_of_memory = transport == NULL && ( !with_status || status == ROCKETNET_STATUS_OUT_OF_MEMORY );
            if ( !out_of_memory )
            {
                printf( "mutation gap hunt 2/6740,7095,7096,7108,7109,7223,7224: with allocation %d of %d failing, a create %s returned %s with status %d, not NULL with ROCKETNET_STATUS_OUT_OF_MEMORY (%d)\n",
                        fail_at, allocations, with_status ? "given a status pointer" : "given no status pointer", transport ? "a transport" : "NULL", status,
                        (int) ROCKETNET_STATUS_OUT_OF_MEMORY );
            }
            check( out_of_memory );
            if ( transport )
            {
                rn_test_client_transport_stop_and_destroy( transport );
            }

            if ( r2_rest_oom.live != live_before )
            {
                printf( "mutation gap hunt 2/6740,7095,7108,7223: with allocation %d of %d failing, the refused create left %d allocation(s) live\n", fail_at, allocations,
                        r2_rest_oom.live - live_before );
            }
            check( r2_rest_oom.live == live_before );
            refused++;
        }
    }
    check( refused == 2 * allocations );

    rocketnet_quiet( false );
    rocketnet_client_transport_set_memory_plan_seam_for_test( false );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( r2_rest_oom.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_rest_every_failed_create_allocation_is_out_of_memory );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7752 (a NULL buffer while a payload waits copies nothing)
//
// A payload from the server waits in the batch. receive_packet with a NULL
// buffer is a counted no-op: it returns 0 at its guard, never reaches the
// copy, and the payload is still there for the next read.

static int r2_rest_asserts = 0;

// the guard's own assert is the first; a second means the call went on past it
static void r2_rest_assert( const char * condition, const char *, const char *, int )
{
    r2_rest_asserts++;
    if ( r2_rest_asserts > 1 )
    {
        printf( "mutation gap hunt 2/7752: receive_packet( client, NULL, 100 ) went past its NULL guard toward the copy into NULL (a second assert fired: %s)\n",
                condition ? condition : "?" );
        fflush( stdout );
    }
}

static int r2_rest_server_payload( uint8_t * packet, uint64_t sequence, uint8_t fill, int payload_bytes )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 32];
    memset( plaintext, 0, sizeof( plaintext ) );
    memset( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, fill, (size_t) payload_bytes );
    return rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, rocketnet_test_filter_value, sequence, NULL, plaintext,
                                          ROCKETNET_ACK_BLOCK_MIN_BYTES + payload_bytes, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                          rocketnet_test_session_s2c_key );
}

static void test_gaps_r2_ct_rest_receive_into_null_with_a_payload_waiting_copies_nothing()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rest_client( fake, &rocketnet_fake_client_operations_io );

    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int bytes = r2_rest_server_payload( packet, 100, 0xC3, 12 );
    check( bytes > 0 );
    rocketnet_fake_client_push( fake, &server, packet, bytes );

    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    check( io != NULL );
    if ( io )
    {
        rocketnet_client_io_step( io, false );
    }
    r2_rest_now = 1000.01;
    rocketnet_client_transport_update( transport, r2_rest_now );
    check( rocketnet_client_transport_num_payloads( transport ) == 1 );

    rocketnet_quiet( true );
    const rocketnet_assert_function_t saved = rn_assert_function;
    r2_rest_asserts = 0;
    rocketnet_assert_function( r2_rest_assert );
    const uint64_t misuses_before = rocketnet_api_misuse_count();

    const int read_null = rocketnet_client_transport_receive_packet( transport, NULL, 100 );

    const uint64_t misuses = rocketnet_api_misuse_count() - misuses_before;
    rocketnet_assert_function( saved );
    rocketnet_quiet( false );

    if ( read_null != 0 || misuses != 1 || r2_rest_asserts != 1 )
    {
        printf( "mutation gap hunt 2/7752: receive_packet( client, NULL, 100 ) with a payload waiting returned %d, counted %llu misuse(s) and fired %d assert(s), not 0, 1 and 1\n",
                read_null, (unsigned long long) misuses, r2_rest_asserts );
    }
    check( read_null == 0 );
    check( misuses == 1 );
    check( r2_rest_asserts == 1 );

    uint8_t copy[64];
    memset( copy, 0, sizeof( copy ) );
    const int read = rocketnet_client_transport_receive_packet( transport, copy, (int) sizeof( copy ) );
    if ( read != 12 || copy[0] != 0xC3 )
    {
        printf( "mutation gap hunt 2/7752: after the refused NULL read the waiting payload read %d byte(s), not the 12 the server sent\n", read );
    }
    check( read == 12 && copy[0] == 0xC3 && copy[11] == 0xC3 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_rest_receive_into_null_with_a_payload_waiting_copies_nothing );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6373 (the reset witness refuses NULL and a client with an I/O thread)
// mutation gap: hunt 2/6406, 2/6410, 2/6415 (the clean check names what was not reset)
//
// The reset witness poisons a client's rings and runs the connection reset,
// so it refuses a client whose handed-off ring another thread owns. The
// clean check names the first member a connection end left behind, and on a
// client with an I/O thread it looks only at the fill ring: a handed-off
// ring belongs to that thread and is not the reset's to clear.

static void test_gaps_r2_ct_rest_reset_witness_refuses_null_and_an_io_client()
{
    const bool refused_null = !rocketnet_client_transport_reset_witness_for_test( NULL );
    if ( !refused_null )
    {
        printf( "mutation gap hunt 2/6373: the reset witness accepted a NULL client\n" );
    }
    check( refused_null );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rest_client( fake, &rocketnet_fake_client_operations_io );
    check( rocketnet_client_transport_client_io( transport ) != NULL );

    const bool refused_io = !rocketnet_client_transport_reset_witness_for_test( transport );
    if ( !refused_io )
    {
        printf( "mutation gap hunt 2/6373: the reset witness ran on a client with an I/O thread, poisoning rings that thread owns\n" );
    }
    check( refused_io );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_rest_reset_witness_refuses_null_and_an_io_client );

static void r2_rest_clean( const char * id, rocketnet_client_transport_t * transport, bool want_clean, const char * want_member )
{
    char failure[256];
    const bool clean = rocketnet_client_transport_connection_state_clean_for_test( transport, failure, sizeof( failure ) );
    const bool named = want_member == NULL || strstr( failure, want_member ) != NULL;
    if ( clean != want_clean || !named )
    {
        printf( "mutation gap %s: the clean check said %s (\"%s\"), not %s%s\n", id, clean ? "clean" : "not clean", failure, want_clean ? "clean" : "not clean, naming ",
                want_member ? want_member : "" );
    }
    check( clean == want_clean );
    check( named );
}

static void test_gaps_r2_ct_rest_clean_check_names_what_was_not_reset()
{
    // a sealed payload left in the fill ring (BASE path: one ring, the fill ring)

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rest_client( fake, &rocketnet_fake_client_operations_base );
    check( rocketnet_client_transport_client_io( transport ) == NULL );
    r2_rest_clean( "hunt 2/6415", transport, true, NULL );
    r2_rest_fill_one( transport );
    r2_rest_clean( "hunt 2/6410", transport, false, "payload_ring" );
    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    // a live per-connection counter

    fake = new rocketnet_fake_client_transport_io_t();
    transport = r2_rest_client( fake, &rocketnet_fake_client_operations_base );
    rocketnet_client_transport_mark_connection_state_for_test( transport );
    r2_rest_clean( "hunt 2/6415", transport, false, "stats_counted" );
    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    // an I/O-thread client: the handed-off ring is the I/O thread's, the fill ring is empty

    fake = new rocketnet_fake_client_transport_io_t();
    transport = r2_rest_client( fake, &rocketnet_fake_client_operations_io );
    check( rocketnet_client_transport_client_io( transport ) != NULL );
    r2_rest_fill_one( transport );
    r2_rest_clean( "hunt 2/6410", transport, false, "payload_ring" );
    rocketnet_client_transport_send_packets( transport ); // handed off; the I/O step has not run
    r2_rest_clean( "hunt 2/6406", transport, true, NULL );
    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_rest_clean_check_names_what_was_not_reset );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7839 (the bound-port seam says 0 for no client)
// mutation gap: hunt 2/7852 (the delivery capture refuses NULL arguments)

static void test_gaps_r2_ct_rest_seams_refuse_null_arguments()
{
    const uint16_t port = rocketnet_client_transport_test_bound_port( NULL );
    if ( port != 0 )
    {
        printf( "mutation gap hunt 2/7839: the bound-port seam said port %d for a NULL client, not 0\n", (int) port );
    }
    check( port == 0 );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rest_client( fake, &rocketnet_fake_client_operations_io );

    rocketnet_quiet( true );
    const rocketnet_assert_function_t saved = rn_assert_function;
    rocketnet_assert_function( []( const char *, const char *, const char *, int ) {} );

    struct rocketnet_delivery_capture_t capture;
    memset( &capture, 0, sizeof( capture ) );
    uint64_t before = rocketnet_api_misuse_count();
    rocketnet_client_transport_delivery_capture( NULL, &capture );
    const uint64_t null_client = rocketnet_api_misuse_count() - before;

    before = rocketnet_api_misuse_count();
    rocketnet_client_transport_delivery_capture( transport, NULL );
    const uint64_t null_capture = rocketnet_api_misuse_count() - before;

    rocketnet_assert_function( saved );
    rocketnet_quiet( false );

    if ( null_client != 1 || null_capture != 1 || capture.connected != 0 )
    {
        printf( "mutation gap hunt 2/7852: the delivery capture counted %llu misuse(s) for a NULL client and %llu for a NULL capture, not 1 each (capture connected %d)\n",
                (unsigned long long) null_client, (unsigned long long) null_capture, capture.connected );
    }
    check( null_client == 1 );
    check( null_capture == 1 );
    check( capture.connected == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_rest_seams_refuse_null_arguments );

#endif // #if ROCKETNET_DEV
