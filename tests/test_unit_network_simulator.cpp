/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The network simulator's memory and refusals (sdk/rocketnet_network_simulator.cpp):
// create allocates exactly rocketnet_network_simulator_memory_bytes, refuses
// what that plan refuses before any allocation, and frees everything when an
// allocation fails; a reverse window the allocator refuses leaves the patterns
// off; deliver_packets refuses a size outside 1 .. the receive ceiling and a
// NULL packet, and both deliver_packets and send drop at a full wire. send
// only asserts its size and pointers (a caller error, never a refusal), so no
// case here sends a bad size through it. No socket, thread or clock: time is
// an argument, and the lock-contention case holds the mutex on this thread.

#include "test_unit_registry.h"
#include "unit_shutdown.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_wire.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_config.h"
#include "fake_client_driver.h"

#include <limits.h>
#include <string.h>

#include "rocketnet_platform.h"

#define check rocketnet_unit_check

#if ROCKETNET_DEV

// a counting allocator: live bytes and calls, and one call it refuses
// (refuse_call, -1 for none). A header in front of each block keeps its size.

enum
{
    SIMULATOR_COUNT_HEADER = 16
};

struct simulator_allocator_t
{
    uint64_t live_bytes;
    int calls;
    int refuse_call;
};

static simulator_allocator_t simulator_allocator;

static void * simulator_count_malloc( void * context, size_t bytes )
{
    const int call = simulator_allocator.calls++;
    if ( call == simulator_allocator.refuse_call )
    {
        return NULL;
    }

    uint8_t * block = (uint8_t *) rocketnet_default_malloc_function( context, bytes + SIMULATOR_COUNT_HEADER );
    if ( !block )
    {
        return NULL;
    }

    memcpy( block, &bytes, sizeof( bytes ) );
    simulator_allocator.live_bytes += bytes;

    return block + SIMULATOR_COUNT_HEADER;
}

static void simulator_count_free( void * context, void * memory )
{
    if ( !memory )
    {
        return;
    }

    uint8_t * block = (uint8_t *) memory - SIMULATOR_COUNT_HEADER;
    size_t bytes;
    memcpy( &bytes, block, sizeof( bytes ) );
    simulator_allocator.live_bytes -= bytes;
    rocketnet_default_free_function( context, block );
}

static void simulator_allocator_install()
{
    memset( &simulator_allocator, 0, sizeof( simulator_allocator ) );
    simulator_allocator.refuse_call = -1;

    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, simulator_count_malloc, simulator_count_free ) == ROCKETNET_STATUS_OK );
}

static void simulator_allocator_remove()
{
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static void simulator_quiet_assert( const char *, const char *, const char *, int ) {}

// create takes exactly its plan, and a reverse window adds exactly the plan's
// window term; destroy returns every byte

static void test_network_simulator_create_allocates_its_plan()
{
    simulator_allocator_install();

    const int capacities[] = { 1, 63, 64, 65, 130, 4096, 4097 };
    for ( int i = 0; i < (int) ( sizeof( capacities ) / sizeof( capacities[0] ) ); i++ )
    {
        const int capacity = capacities[i];

        struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, capacity, 1 );
        check( simulator != NULL );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 0 ) );

        check( rocketnet_network_simulator_patterns( simulator, 0, 64 ) );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 64 ) );

        check( rocketnet_network_simulator_patterns( simulator, 0, 2 ) );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 2 ) );

        check( rocketnet_network_simulator_patterns( simulator, 0, 0 ) );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 0 ) );

        rocketnet_network_simulator_destroy( simulator );
        check( simulator_allocator.live_bytes == 0 );
    }

    // a window under 2 is no pattern and holds no memory

    check( rocketnet_network_simulator_memory_bytes( 64, 1 ) == rocketnet_network_simulator_memory_bytes( 64, 0 ) );

    simulator_allocator_remove();
}

// a capacity the plan refuses is refused by create before any allocation

static void test_network_simulator_create_refuses_what_its_plan_refuses()
{
    simulator_allocator_install();
    rocketnet_assert_function( simulator_quiet_assert );

    const int refused[] = { 0, -1, INT_MIN, ( 1 << 30 ) + 1, INT_MAX };
    for ( int i = 0; i < (int) ( sizeof( refused ) / sizeof( refused[0] ) ); i++ )
    {
        check( rocketnet_network_simulator_memory_bytes( refused[i], 0 ) == 0 );
        check( rocketnet_network_simulator_create( NULL, refused[i], 1 ) == NULL );
        check( simulator_allocator.calls == 0 );
    }

    rocketnet_assert_function( NULL );

    check( rocketnet_network_simulator_memory_bytes( 1 << 30, 0 ) > 0 );

    simulator_allocator_remove();
}

// each of create's allocations refused in turn: create returns NULL and frees
// what it took

static void test_network_simulator_create_frees_on_a_refused_allocation()
{
    simulator_allocator_install();

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 130, 1 );
    check( simulator != NULL );
    const int allocations = simulator_allocator.calls;
    rocketnet_network_simulator_destroy( simulator );
    check( allocations > 1 );

    for ( int refuse = 0; refuse < allocations; refuse++ )
    {
        simulator_allocator.calls = 0;
        simulator_allocator.refuse_call = refuse;
        check( rocketnet_network_simulator_create( NULL, 130, 1 ) == NULL );
        check( simulator_allocator.live_bytes == 0 );
    }

    simulator_allocator_remove();
}

static void simulator_addresses( struct rocketnet_address_t * from, struct rocketnet_address_t * to )
{
    check( rocketnet_address_parse( from, "198.51.100.10:10000" ) );
    check( rocketnet_address_parse( to, "198.51.100.20:20000" ) );
}

// a reverse window the allocator refuses leaves both patterns off and the
// wire passes packets through in send order

static void test_network_simulator_refused_window_leaves_patterns_off()
{
    simulator_allocator_install();

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 16, 1 );
    check( simulator != NULL );

    simulator_allocator.refuse_call = simulator_allocator.calls;
    check( !rocketnet_network_simulator_patterns( simulator, 7, 4 ) );
    check( !rocketnet_network_simulator_patterns_active( simulator ) );
    check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( 16, 0 ) );

    struct rocketnet_address_t from, to;
    simulator_addresses( &from, &to );

    // the seventh send is past the refused every-7th pattern

    for ( int i = 0; i < 8; i++ )
    {
        const uint8_t byte = (uint8_t) i;
        rocketnet_network_simulator_send( simulator, &from, &to, &byte, 1, 10.0 );
    }

    for ( int i = 0; i < 8; i++ )
    {
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        struct rocketnet_address_t received_from;
        check( rocketnet_network_simulator_receive( simulator, &to, 10.0, &received_from, packet ) == 1 );
        check( packet[0] == (uint8_t) i );
    }

    rocketnet_network_simulator_destroy( simulator );
    simulator_allocator_remove();
}

// deliver_packets skips a NULL packet and a size outside 1 .. the receive
// ceiling, takes the rest in list order, and drops what a full wire cannot
// hold; send at a full wire drops too (send's size and pointers are asserted,
// not refused)

static void test_network_simulator_refuses_bad_sizes_and_a_full_wire()
{
    const int capacity = 4;
    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, capacity, 1 );
    check( simulator != NULL );

    struct rocketnet_address_t from, to;
    simulator_addresses( &from, &to );

    static uint8_t largest[ROCKETNET_RECEIVE_CEILING + 1];
    memset( largest, 0x5A, sizeof( largest ) );
    largest[0] = 1;

    const uint8_t two = 2;
    const uint8_t * packets[] = { NULL, &two, largest, largest, &two };
    const int bytes[] = { 1, 0, ROCKETNET_RECEIVE_CEILING + 1, ROCKETNET_RECEIVE_CEILING, 1 };
    check( rocketnet_network_simulator_deliver_packets( simulator, &from, &to, packets, bytes, 5, 10.0 ) == 2 );

    uint8_t received[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_address_t received_from;
    check( rocketnet_network_simulator_receive( simulator, &to, 10.0, &received_from, received ) == ROCKETNET_RECEIVE_CEILING );
    check( received[0] == 1 && received[ROCKETNET_RECEIVE_CEILING - 1] == 0x5A );
    check( rocketnet_network_simulator_receive( simulator, &to, 10.0, &received_from, received ) == 1 );
    check( received[0] == 2 );
    check( !rocketnet_network_simulator_holds_any( simulator ) );

    // a full wire takes the first capacity packets of a list and drops the rest

    const uint8_t values[] = { 10, 11, 12, 13, 14, 15 };
    const uint8_t * list[6];
    int list_bytes[6];
    for ( int i = 0; i < 6; i++ )
    {
        list[i] = &values[i];
        list_bytes[i] = 1;
    }

    check( rocketnet_network_simulator_deliver_packets( simulator, &from, &to, list, list_bytes, 6, 10.0 ) == capacity );

    const uint8_t late = 99;
    rocketnet_network_simulator_send( simulator, &from, &to, &late, 1, 0.0 );

    for ( int i = 0; i < capacity; i++ )
    {
        check( rocketnet_network_simulator_receive( simulator, &to, 10.0, &received_from, received ) == 1 );
        check( received[0] == values[i] );
    }

    check( rocketnet_network_simulator_receive( simulator, &to, 10.0, &received_from, received ) == 0 );
    check( !rocketnet_network_simulator_holds_any( simulator ) );

    rocketnet_network_simulator_destroy( simulator );
}

// Each client's link simulator gets its own seed: two clients created with
// the same 50% loss setting must not drop at the same sends.

static void network_simulator_client_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = UINT64_C( 1 ) << 40;
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

static rocketnet_client_transport_t * network_simulator_client(
    rocketnet_fake_client_transport_io_t * transport, const rocketnet_client_transport_config_t * config )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    network_simulator_client_token( token );
    return rocketnet_client_transport_create_step_driven_current( transport, token, config, 1000.0,
                                                                  &rocketnet_fake_client_operations_base, transport );
}

static void test_network_simulator_clients_have_distinct_seeds()
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_loss_percent = 50;

    rocketnet_fake_client_transport_io_t * first_io = new rocketnet_fake_client_transport_io_t();
    rocketnet_fake_client_transport_io_t * second_io = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * first = network_simulator_client( first_io, &config );
    rocketnet_client_transport_t * second = network_simulator_client( second_io, &config );
    check( first != NULL );
    check( second != NULL );

    bool first_dropped[40];
    bool second_dropped[40];
    for ( int i = 0; i < 40; i++ )
    {
        const double time = 1000.0 + 0.2 * ( i + 1 );
        const int first_before = first_io->num_sent;
        const int second_before = second_io->num_sent;
        rocketnet_client_transport_update( first, time );
        rocketnet_client_transport_update( second, time );
        first_dropped[i] = first_io->num_sent == first_before;
        second_dropped[i] = second_io->num_sent == second_before;
    }

    bool identical = true;
    for ( int i = 0; i < 40; i++ )
    {
        if ( first_dropped[i] != second_dropped[i] )
        {
            identical = false;
        }
    }
    check( !identical );

    rn_test_client_transport_stop_and_destroy( first );
    rn_test_client_transport_stop_and_destroy( second );
    delete first_io;
    delete second_io;
}

REGISTER_UNIT_TEST( test_network_simulator_clients_have_distinct_seeds );

// receive_any honours a global delivery-time bound, and a drain at one time
// resumes after the slot it last delivered: many not-yet-due packets ahead of
// due ones must not be rescanned from slot zero for every due packet, whether
// the due packets were queued together or arrive between drains. The drains
// run at 1000.0: the resume hint is kept per drain time, and a time of zero
// is the one time a hint that was never stamped would still match.
static void test_network_simulator_receive_any_bounds_its_scan()
{
    const int capacity = 512;
    const int late_count = 256;
    const int due_count = 16;

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, capacity, 1 );
    check( simulator != NULL );

    struct rocketnet_address_t from, to;
    simulator_addresses( &from, &to );

    uint8_t values[capacity];
    const uint8_t * packets[capacity];
    int bytes[capacity];
    for ( int i = 0; i < capacity; i++ )
    {
        values[i] = (uint8_t) i;
        packets[i] = &values[i];
        bytes[i] = 1;
    }

    check( rocketnet_network_simulator_deliver_packets( simulator, &from, &to, packets, bytes, late_count, 2000.0 ) == late_count );

    uint8_t received[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_address_t received_from;
    struct rocketnet_address_t received_to;
    struct rocketnet_network_simulator_index_stats_t stats;

    rocketnet_network_simulator_test_reset_index_stats( simulator );
    check( rocketnet_network_simulator_receive_any( simulator, 1000.0, &received_from, &received_to, received ) == 0 );
    rocketnet_network_simulator_test_index_stats( simulator, &stats );
    check( stats.receive_any_slots == 0 );

    check( rocketnet_network_simulator_deliver_packets( simulator, &from, &to,
                                                        packets + late_count, bytes + late_count,
                                                        due_count, 1000.0 ) == due_count );

    rocketnet_network_simulator_test_reset_index_stats( simulator );
    for ( int i = 0; i < due_count; i++ )
    {
        check( rocketnet_network_simulator_receive_any( simulator, 1000.0, &received_from, &received_to, received ) == 1 );
        check( received[0] == values[late_count + i] );
    }
    check( rocketnet_network_simulator_receive_any( simulator, 1000.0, &received_from, &received_to, received ) == 0 );

    rocketnet_network_simulator_test_index_stats( simulator, &stats );
    check( stats.receive_any_slots <= (uint64_t) capacity );

    // arrival and drain interleave: a due packet reuses the lowest free slot
    // below the resume hint, which must lower the hint to that slot instead of
    // restarting the next scan from zero.

    rocketnet_network_simulator_test_reset_index_stats( simulator );
    for ( int i = 0; i < due_count; i++ )
    {
        check( rocketnet_network_simulator_deliver_packets( simulator, &from, &to,
                                                            packets + late_count + i, bytes + late_count + i,
                                                            1, 1000.0 ) == 1 );
        check( rocketnet_network_simulator_receive_any( simulator, 1000.0, &received_from, &received_to, received ) == 1 );
        check( received[0] == values[late_count + i] );
    }
    check( rocketnet_network_simulator_receive_any( simulator, 1000.0, &received_from, &received_to, received ) == 0 );

    rocketnet_network_simulator_test_index_stats( simulator, &stats );
    check( stats.receive_any_slots <= (uint64_t) capacity );

    rocketnet_network_simulator_destroy( simulator );
}

// A producer that still owns the simulator mutex must not stall the send
// path. This thread holds the mutex across the call (a normal mutex refuses a
// second acquisition). take_setting returns without applying the handed-over
// latency, and the next pass that finds the mutex free applies it. Delivery
// time is an argument.

static void test_network_simulator_take_setting_does_not_wait()
{
    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 8, 1 );
    check( simulator != NULL );

    struct rocketnet_address_t from, to;
    simulator_addresses( &from, &to );

    struct rocketnet_platform_mutex_t mutex;
    check( rocketnet_platform_mutex_create( &mutex ) );

    rocketnet_network_simulator_reconfigure( simulator, &mutex, 1.0, 0.0, 0.0f, 0.0f );

    rocketnet_platform_mutex_acquire( &mutex );

    const bool active = rocketnet_network_simulator_take_setting( simulator, &mutex );
    check( !active );

    const uint8_t byte = 7;
    rocketnet_network_simulator_send( simulator, &from, &to, &byte, 1, 0.0 );
    uint8_t received[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_address_t received_from;
    check( rocketnet_network_simulator_receive( simulator, &to, 0.0, &received_from, received ) == 1 );
    check( received[0] == byte );

    rocketnet_platform_mutex_release( &mutex );

    check( rocketnet_network_simulator_take_setting( simulator, &mutex ) );

    const uint8_t late = 8;
    rocketnet_network_simulator_send( simulator, &from, &to, &late, 1, 0.0 );
    check( rocketnet_network_simulator_receive( simulator, &to, 0.0, &received_from, received ) == 0 );
    check( rocketnet_network_simulator_receive( simulator, &to, 1.0, &received_from, received ) == 1 );
    check( received[0] == late );

    rocketnet_platform_mutex_destroy( &mutex );
    rocketnet_network_simulator_destroy( simulator );
}

REGISTER_UNIT_TEST( test_network_simulator_create_allocates_its_plan );
REGISTER_UNIT_TEST( test_network_simulator_create_refuses_what_its_plan_refuses );
REGISTER_UNIT_TEST( test_network_simulator_create_frees_on_a_refused_allocation );
REGISTER_UNIT_TEST( test_network_simulator_refused_window_leaves_patterns_off );
REGISTER_UNIT_TEST( test_network_simulator_refuses_bad_sizes_and_a_full_wire );
REGISTER_UNIT_TEST( test_network_simulator_take_setting_does_not_wait );
REGISTER_UNIT_TEST( test_network_simulator_receive_any_bounds_its_scan );

#endif // #if ROCKETNET_DEV
