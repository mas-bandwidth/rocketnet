/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The client's BASE receive path (a transport without wait and wake) and the
// public counters: a byte count past the UDP size limit reads
// PREFILTER_TOO_LARGE once at update, as on the I/O thread path; a negative
// byte count is a transport fault no public counter names, checked against
// every counter of a run that received nothing. The transport is
// created step-driven against the fake client transport
// (tests/fake_client_driver.h): no thread, no socket, no real time; the clock
// argument starts at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_config.h"

#include <string.h>

#if ROCKETNET_DEV

static double client_legacy_fault_now = 1000.0;
static double client_legacy_fault_time( void * ) { return client_legacy_fault_now; }

// receive answers this byte count once, then nothing
static int client_legacy_fault_bytes;
static int client_legacy_fault_calls;

static int client_legacy_fault_receive( void *, rocketnet_address_t * from, uint8_t *, int )
{
    if ( client_legacy_fault_calls++ > 0 )
    {
        return 0;
    }
    memset( from, 0, sizeof( *from ) );
    from->type = ROCKETNET_ADDRESS_IPV4;
    from->port = 1;
    return client_legacy_fault_bytes;
}

static void client_legacy_fault_token( uint8_t * token )
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

static void test_client_legacy_receive_counts_too_large_at_update()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = client_legacy_fault_receive;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_legacy_fault_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_legacy_fault_time;

    client_legacy_fault_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &operations, &fake );
    rocketnet_unit_check( transport != NULL );

    const uint64_t too_large = rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];

    // a byte count past the UDP size limit: once, however many updates follow

    client_legacy_fault_bytes = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES + 1;
    client_legacy_fault_calls = 0;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.1 );
    rocketnet_client_transport_update( transport, 1000.1 );
    rocketnet_unit_check( rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large + 1 );
    rocketnet_client_transport_update( transport, 1000.2 );
    rocketnet_unit_check( rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large + 1 );

    rn_test_client_transport_stop_and_destroy( transport );
}
REGISTER_UNIT_TEST( test_client_legacy_receive_counts_too_large_at_update );

// A negative byte count is a fault, not a datagram: every public counter
// after one receive step and update that met it equals the same run whose
// receive met no datagram at all. Both runs see the same clock, so whatever
// the client sends or counts on its own is the same in each.

static void client_legacy_fault_counters_after( int bytes, struct rocketnet_counters_t * counters )
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = client_legacy_fault_receive;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_legacy_fault_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_legacy_fault_time;

    client_legacy_fault_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &operations, &fake );
    rocketnet_unit_check( transport != NULL );

    client_legacy_fault_bytes = bytes;
    client_legacy_fault_calls = 0;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    rocketnet_unit_check( client_legacy_fault_calls == 1 );
    rocketnet_client_transport_update( transport, 1000.0 );

    memcpy( counters->values, rocketnet_client_transport_counters( transport )->values, sizeof( counters->values ) );

    rn_test_client_transport_stop_and_destroy( transport );
}

static void test_client_legacy_negative_byte_count_moves_no_counter()
{
    struct rocketnet_counters_t fault;
    struct rocketnet_counters_t control;
    memset( &fault, 0, sizeof( fault ) );
    memset( &control, 0, sizeof( control ) );

    client_legacy_fault_counters_after( -1, &fault );
    client_legacy_fault_counters_after( 0, &control );

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        rocketnet_unit_check( fault.values[i] == control.values[i] );
    }
}
REGISTER_UNIT_TEST( test_client_legacy_negative_byte_count_moves_no_counter );

// A byte count of exactly the UDP size limit is a datagram, not a fault: the
// receive keeps it, the update's prefilter refuses it as too large, and it
// counts PREFILTER_TOO_LARGE once, never also as a count past the limit. It
// comes from the token's server (127.0.0.1:40000), so the source check
// passes it to the prefilter.

static int client_legacy_server_receive( void *, rocketnet_address_t * from, uint8_t *, int )
{
    if ( client_legacy_fault_calls++ > 0 )
    {
        return 0;
    }
    memset( from, 0, sizeof( *from ) );
    from->type = ROCKETNET_ADDRESS_IPV4;
    from->data.ipv4[0] = 127;
    from->data.ipv4[3] = 1;
    from->port = 40000;
    return client_legacy_fault_bytes;
}

static void test_client_legacy_receive_keeps_a_datagram_at_the_size_limit()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = client_legacy_server_receive;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_legacy_fault_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_legacy_fault_time;
    client_legacy_fault_now = 1000.0;

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &operations, &fake );
    rocketnet_unit_check( transport != NULL );

    const uint64_t * values = rocketnet_client_transport_counters( transport )->values;
    const uint64_t too_large = values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];

    client_legacy_fault_bytes = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES;
    client_legacy_fault_calls = 0;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    rocketnet_unit_check( client_legacy_fault_calls == 2 );
    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_unit_check( values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large + 1 );

    rocketnet_client_transport_update( transport, 1000.1 );
    rocketnet_unit_check( values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large + 1 );

    rn_test_client_transport_stop_and_destroy( transport );
}

REGISTER_UNIT_TEST( test_client_legacy_receive_keeps_a_datagram_at_the_size_limit );

// receive answers client_legacy_budget_left one-byte datagrams, then nothing
static int client_legacy_budget_left;

static int client_legacy_budget_receive( void *, rocketnet_address_t * from, uint8_t * data, int )
{
    if ( client_legacy_budget_left <= 0 )
    {
        return 0;
    }
    client_legacy_budget_left--;
    memset( from, 0, sizeof( *from ) );
    from->type = ROCKETNET_ADDRESS_IPV4;
    from->port = 1;
    data[0] = 0;
    return 1;
}

// The update's packet budget is a bound, not a cut short of it: two
// published buffers holding exactly the budget between them (3 and
// ROCKETNET_NUM_CLIENT_PACKETS - 3) are both taken by one update, and the
// next update has nothing left. Every datagram is a runt from a foreign
// source, so each one taken counts once, as a foreign source or a prefilter
// refusal. Mutation that must bite: refuse a buffer that brings the update
// to exactly its budget.

static uint64_t client_legacy_budget_taken( rocketnet_client_transport_t * transport )
{
    const uint64_t * values = rocketnet_client_transport_counters( transport )->values;
    return values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] + values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT];
}

static void test_client_legacy_update_takes_exactly_its_budget()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = client_legacy_budget_receive;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_legacy_fault_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_legacy_fault_time;

    client_legacy_fault_now = 1000.0;
    client_legacy_budget_left = 0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &operations, &fake );
    rocketnet_unit_check( transport != NULL );

    const uint64_t before = client_legacy_budget_taken( transport );

    client_legacy_budget_left = 3;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    client_legacy_budget_left = ROCKETNET_NUM_CLIENT_PACKETS - 3;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    rocketnet_unit_check( client_legacy_budget_left == 0 );

    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_unit_check( client_legacy_budget_taken( transport ) == before + ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_client_transport_update( transport, 1000.01 );
    rocketnet_unit_check( client_legacy_budget_taken( transport ) == before + ROCKETNET_NUM_CLIENT_PACKETS );

    rn_test_client_transport_stop_and_destroy( transport );
}
REGISTER_UNIT_TEST( test_client_legacy_update_takes_exactly_its_budget );

#endif // #if ROCKETNET_DEV
