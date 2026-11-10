/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The client's BASE receive path (a transport without wait and wake) and the
// public counters: a byte count past the UDP size limit reads
// PREFILTER_TOO_LARGE once at update, as on the I/O thread path; a negative
// byte count is a transport fault no public counter names. The transport is
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
        return 0;
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

    // a negative byte count: a fault, not a datagram too large

    client_legacy_fault_bytes = -1;
    client_legacy_fault_calls = 0;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_unit_check( rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large );

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

#endif // #if ROCKETNET_DEV
