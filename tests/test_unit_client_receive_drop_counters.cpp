/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every datagram the client drops is counted. A step-driven client transport
// over a scripted driver (no socket, no thread, a test clock from 1000.0)
// receives datagrams whose reported sizes are outside [prefilter, receive
// ceiling], as a socket reports a runt or an oversize datagram, and
// datagrams from a source other than the dialed server. The I/O step drops
// nothing (sdk/rocketnet_client_io.h:390-396): on update, a datagram from
// another source counts CLIENT_RECEIVE_FOREIGN_SOURCE before any protocol
// check (sdk/rocketnet_client_transport.cpp:1296-1301), whatever its size,
// and one from the server reaches the prefilter
// (sdk/rocketnet_client_protocol.cpp:1248-1253), which counts
// PREFILTER_TOO_SHORT or PREFILTER_TOO_LARGE; per-packet and batch. With a
// link pattern configured the client runs no I/O core and update receives
// from the driver itself (sdk/rocketnet_client_transport.cpp:1570-1571); that
// receive keeps every datagram too, so the same counters count the same drops.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK check
#include "counters_expect.h"

#define DROP_COUNTERS_MAX_SCRIPT 32

// the fake first, so the fake's own callbacks read the same pointer
struct drop_counters_driver_t
{
    rocketnet_fake_client_transport_io_t fake;
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

static rocketnet_client_transport_t * drop_counters_create( drop_counters_driver_t * driver, const rocketnet_client_transport_io_operations_t * operations, drop_counters_clock_t * clock )
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
    rocketnet_client_transport_io_operations_t operations = batch ? rocketnet_fake_client_operations_batch : rocketnet_fake_client_operations_io;
    operations.receive = drop_counters_receive;
    if ( batch )
        operations.receive_batch = drop_counters_receive_batch;

    drop_counters_driver_t * driver = new drop_counters_driver_t();
    drop_counters_clock_t clock = { 1000.0 };
    rocketnet_client_transport_t * transport = drop_counters_create( driver, &operations, &clock );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );

    // Seed a live attempt before any refusal: connect already holds genuine
    // nonzero protocol state -- the client sits in requesting cookie, the
    // dialed server address is held, and the connection salt and ephemeral
    // keypair are armed. An empty step and update run the handshake send
    // scheduler with nothing inbound, so no receive state moves while the
    // prestate is established.
    rocketnet_client_io_step( io, false );
    rocketnet_client_transport_update( transport, clock.now );
    check( rocketnet_client_transport_state( transport ) ==
           ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    const struct rocketnet_counters_t before = *rocketnet_client_transport_counters( transport );

    // from the dialed server, each outside the size bounds: passed by the
    // I/O step, refused by the prefilter on update, each counted once
    const char * server = "127.0.0.1:40000";
    int too_short = 0;
    int too_large = 0;
    if ( batch )
    {
        drop_counters_push( driver, server, 0 );
        too_short++;
    }
    const int short_sizes[] = { 1, ROCKETNET_PREFILTER_BYTES - 1 };
    for ( size_t i = 0; i < sizeof(short_sizes) / sizeof(short_sizes[0]); i++ )
    {
        drop_counters_push( driver, server, short_sizes[i] );
        too_short++;
    }
    const int large_sizes[] = { ROCKETNET_RECEIVE_CEILING + 1, 1500, 9000, 65507 };
    for ( size_t i = 0; i < sizeof(large_sizes) / sizeof(large_sizes[0]); i++ )
    {
        drop_counters_push( driver, server, large_sizes[i] );
        too_large++;
    }

    // from other sources: dropped on update before any protocol check, each
    // counted once, a runt among them: the source check comes first
    drop_counters_push( driver, "127.0.0.1:40001", ROCKETNET_PREFILTER_BYTES );
    drop_counters_push( driver, "127.0.0.2:40000", 100 );
    drop_counters_push( driver, "10.0.0.1:40000", ROCKETNET_RECEIVE_CEILING );
    drop_counters_push( driver, "127.0.0.1:40001", 2 );
    const int foreign = 4;

    rocketnet_client_io_step( io, false );
    check( driver->head == driver->count );

    clock.now = 1000.05;
    rocketnet_client_transport_update( transport, clock.now );

    // Every rejected datagram is a dead end. The only counters that may move
    // are the diagnostics that name the stage: the source check before any
    // protocol work for the foreign group (sdk/rocketnet_client_transport.cpp:
    // 2208-2212), and the prefilter for the server's short and large group
    // (sdk/rocketnet_client_protocol.cpp:1366-1372). Everything else must sit
    // still: protocol state, the ack endpoint, the replay/window marks, any
    // payload/message queue, delivery, and connection or ownership. The
    // exact-delta oracle over the whole counter block proves it; nothing a
    // refused packet could inappropriately feed has a counter that moved.
    const struct rocketnet_counters_t after = *rocketnet_client_transport_counters( transport );
    check( rocketnet_client_transport_state( transport ) ==
           ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    rocketnet_counters_expect( &before, &after,
    {
        { ROCKETNET_COUNTER_PACKETS_RECEIVED, too_short + too_large },
        { ROCKETNET_COUNTER_PREFILTER_TOO_SHORT, too_short },
        { ROCKETNET_COUNTER_PREFILTER_TOO_LARGE, too_large },
        { ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE, foreign }
    } );

    // a refusal
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE ) == ROCKETNET_COUNTER_CLASS_REFUSAL );

    rn_test_client_transport_stop_and_destroy( transport );
    delete driver;
}

// a step-driven client with a link pattern configured: no I/O core, so update
// receives from the driver on the caller's thread
// (sdk/rocketnet_client_transport.cpp:1570-1571). The create reads the whole
// current config (rocketnet_client_transport_create_step_driven_current), as
// rocketnet_client_transport_create_step_driven reads only the fields before
// the link simulator's and would leave simulated_drop_every_nth unread and the
// I/O core on.
static void test_client_receive_counts_prefilter_drops_without_io_core()
{
    rocketnet_public_link_simulator_test_development( 1 );

    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.receive = drop_counters_receive;

    drop_counters_driver_t * driver = new drop_counters_driver_t();
    drop_counters_clock_t clock = { 1000.0 };

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    drop_counters_token( token );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.time_callback = drop_counters_time;
    config.step_driven = true;
    config.simulated_drop_every_nth = 1000;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_current( &clock, token, &config, clock.now, &operations, driver );
    check( transport != NULL );

    // seed the live attempt as in drop_counters_run: the create already
    // holds the dialed server address and requesting-cookie protocol state;
    // an empty step and update run the handshake send scheduler with nothing
    // inbound
    rocketnet_client_io_step( rocketnet_client_transport_client_io( transport ), false );
    rocketnet_client_transport_update( transport, clock.now );
    check( rocketnet_client_transport_state( transport ) ==
           ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    const struct rocketnet_counters_t before = *rocketnet_client_transport_counters( transport );

    const char * server = "127.0.0.1:40000";
    const int short_sizes[] = { 1, ROCKETNET_PREFILTER_BYTES - 1 };
    for ( size_t i = 0; i < sizeof(short_sizes) / sizeof(short_sizes[0]); i++ )
        drop_counters_push( driver, server, short_sizes[i] );
    const int large_sizes[] = { ROCKETNET_RECEIVE_CEILING + 1, 1500, 9000, 65507 };
    for ( size_t i = 0; i < sizeof(large_sizes) / sizeof(large_sizes[0]); i++ )
        drop_counters_push( driver, server, large_sizes[i] );
    drop_counters_push( driver, "127.0.0.1:40001", 2 );

    clock.now = 1000.05;
    rocketnet_client_transport_update( transport, clock.now );
    check( driver->head == driver->count );

    const struct rocketnet_counters_t after = *rocketnet_client_transport_counters( transport );
    check( rocketnet_client_transport_state( transport ) ==
           ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    rocketnet_counters_expect( &before, &after,
    {
        { ROCKETNET_COUNTER_PACKETS_RECEIVED, 6 },
        { ROCKETNET_COUNTER_PREFILTER_TOO_SHORT, 2 },
        { ROCKETNET_COUNTER_PREFILTER_TOO_LARGE, 4 },
        { ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE, 1 }
    } );

    rn_test_client_transport_stop_and_destroy( transport );
    delete driver;
    rocketnet_public_link_simulator_test_development( 0 );
}

REGISTER_UNIT_TEST( test_client_receive_counts_prefilter_drops_without_io_core );

// after the client disconnects, the reset clears the dialed address
// (sdk/rocketnet_connection_reset.cpp:42), and the former server's redundant
// disconnect packets still arrive: dropped as before, never counted as a
// foreign source (sdk/rocketnet_client_transport.cpp:1296-1301)
static void test_client_receive_foreign_source_not_counted_after_disconnect()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.receive = drop_counters_receive;

    drop_counters_driver_t * driver = new drop_counters_driver_t();
    drop_counters_clock_t clock = { 1000.0 };
    rocketnet_client_transport_t * transport = drop_counters_create( driver, &operations, &clock );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );

    rocketnet_client_transport_disconnect( transport );
    check( rocketnet_client_transport_state( transport ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    drop_counters_push( driver, "127.0.0.1:40000", 33 );
    drop_counters_push( driver, "127.0.0.1:40000", 33 );
    rocketnet_client_io_step( io, false );
    check( driver->head == driver->count );

    clock.now = 1000.1;
    rocketnet_client_transport_update( transport, clock.now );
    check( drop_counters_value( transport, ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE ) == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete driver;
}

REGISTER_UNIT_TEST( test_client_receive_foreign_source_not_counted_after_disconnect );

static void test_client_receive_counts_prefilter_and_foreign_source_drops_per_packet()
{
    drop_counters_run( false );
}

REGISTER_UNIT_TEST( test_client_receive_counts_prefilter_and_foreign_source_drops_per_packet );

static void test_client_receive_counts_prefilter_and_foreign_source_drops_batch()
{
    drop_counters_run( true );
}

REGISTER_UNIT_TEST( test_client_receive_counts_prefilter_and_foreign_source_drops_batch );
