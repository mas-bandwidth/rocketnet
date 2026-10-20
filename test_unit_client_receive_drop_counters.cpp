/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every datagram the client drops is counted. A step-driven client transport
// over a scripted driver (no socket, no thread, a test clock from 1000.0)
// receives datagrams whose reported sizes are outside [prefilter, receive
// ceiling], as a socket reports a runt or an oversize datagram, and
// datagrams from a source other than the dialed server:
// CLIENT_RECEIVE_DROPPED_SIZE counts the first, on the I/O step, per-packet
// and batch; CLIENT_RECEIVE_FOREIGN_SOURCE counts the second, on update.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

#define DROP_COUNTERS_MAX_SCRIPT 32

// the fake first, so the fake's own callbacks read the same pointer
struct drop_counters_driver_t
{
    rocketnet_fake_client_driver_t fake;
    rocketnet_address_t from[DROP_COUNTERS_MAX_SCRIPT];
    int reported[DROP_COUNTERS_MAX_SCRIPT];
    int head, count;
};

static void drop_counters_push( drop_counters_driver_t * driver, const char * from, int reported )
{
    check( driver->count < DROP_COUNTERS_MAX_SCRIPT );
    check( rocketnet_address_parse( &driver->from[driver->count], from ) );
    driver->reported[driver->count] = reported;
    driver->count++;
}

// a socket reports a datagram's true size, past the buffer for an oversize
// one; only what fits is written
static int drop_counters_pop( drop_counters_driver_t * driver, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    const int reported = driver->reported[driver->head];
    *from = driver->from[driver->head];
    driver->head++;
    const int written = reported < capacity ? reported : capacity;
    if ( written > 0 )
        memset( data, 0x5a, (size_t) written );
    return reported;
}

static int drop_counters_receive( void * instance, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    drop_counters_driver_t * driver = (drop_counters_driver_t *) instance;
    // the per-packet call reads 0 as drained, so a zero-length datagram is
    // never scripted on it
    if ( driver->head == driver->count )
        return 0;
    return drop_counters_pop( driver, from, data, capacity );
}

static int drop_counters_receive_batch( void * instance, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int max_packets )
{
    drop_counters_driver_t * driver = (drop_counters_driver_t *) instance;
    int got = 0;
    while ( got < max_packets && driver->head < driver->count )
    {
        bytes[got] = drop_counters_pop( driver, &from[got], data + (size_t) got * (size_t) stride, stride );
        got++;
    }
    return got;
}

struct drop_counters_clock_t
{
    double now;
};

static double drop_counters_time( void * context )
{
    return ( (drop_counters_clock_t *) context )->now;
}

// a connect token naming 127.0.0.1:40000
static void drop_counters_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static rocketnet_client_transport_t * drop_counters_create( drop_counters_driver_t * driver, const rocketnet_client_driver_operations_t * operations, drop_counters_clock_t * clock )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    drop_counters_token( token );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = drop_counters_time;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( clock, token, &config, clock->now, operations, driver );
    check( transport != NULL );
    check( rocketnet_client_transport_client_io( transport ) != NULL );
    return transport;
}

static uint64_t drop_counters_value( rocketnet_client_transport_t * transport, int counter )
{
    return rocketnet_counter_value( rocketnet_client_transport_counters( transport ), counter );
}

static void drop_counters_run( bool batch )
{
    rocketnet_client_driver_operations_t operations = batch ? rocketnet_fake_client_operations_batch : rocketnet_fake_client_operations_io;
    operations.receive = drop_counters_receive;
    if ( batch )
        operations.receive_batch = drop_counters_receive_batch;

    drop_counters_driver_t * driver = new drop_counters_driver_t();
    drop_counters_clock_t clock = { 1000.0 };
    rocketnet_client_transport_t * transport = drop_counters_create( driver, &operations, &clock );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );

    check( drop_counters_value( transport, ROCKETNET_COUNTER_CLIENT_RECEIVE_DROPPED_SIZE ) == 0 );
    check( drop_counters_value( transport, ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE ) == 0 );

    // from the dialed server, each outside the size bounds: dropped on the
    // I/O step, each counted once
    const char * server = "127.0.0.1:40000";
    int size_drops = 0;
    if ( batch )
    {
        drop_counters_push( driver, server, 0 );
        size_drops++;
    }
    const int sizes[] = { 1, ROCKETNET_PREFILTER_BYTES - 1, ROCKETNET_RECEIVE_CEILING + 1, 1500, 9000, 65507 };
    for ( size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++ )
    {
        drop_counters_push( driver, server, sizes[i] );
        size_drops++;
    }

    // inside the bounds, from other sources: dropped on update before any
    // protocol check, each counted once
    drop_counters_push( driver, "127.0.0.1:40001", ROCKETNET_PREFILTER_BYTES );
    drop_counters_push( driver, "127.0.0.2:40000", 100 );
    drop_counters_push( driver, "10.0.0.1:40000", ROCKETNET_RECEIVE_CEILING );
    const int foreign = 3;

    // a runt from another source is a size drop: the size check comes first
    drop_counters_push( driver, "127.0.0.1:40001", 2 );
    size_drops++;

    rocketnet_client_io_step( io, false );
    check( driver->head == driver->count );
    check( drop_counters_value( transport, ROCKETNET_COUNTER_CLIENT_RECEIVE_DROPPED_SIZE ) == (uint64_t) size_drops );

    clock.now = 1000.1;
    rocketnet_client_transport_update( transport, clock.now );
    check( drop_counters_value( transport, ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE ) == (uint64_t) foreign );
    check( drop_counters_value( transport, ROCKETNET_COUNTER_CLIENT_RECEIVE_DROPPED_SIZE ) == (uint64_t) size_drops );

    // both are refusals
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CLIENT_RECEIVE_DROPPED_SIZE ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE ) == ROCKETNET_COUNTER_CLASS_REFUSAL );

    rocketnet_client_transport_destroy( transport );
    delete driver;
}

static void client_receive_counts_size_drops_and_foreign_sources_per_packet()
{
    drop_counters_run( false );
}

REGISTER_UNIT_TEST( client_receive_counts_size_drops_and_foreign_sources_per_packet );

static void client_receive_counts_size_drops_and_foreign_sources_batch()
{
    drop_counters_run( true );
}

REGISTER_UNIT_TEST( client_receive_counts_size_drops_and_foreign_sources_batch );
