/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Unit tests for the loopback transport backend, called directly through the
// transport open helpers with the backend's operations; no transport, no thread,
// no sleep, no clock.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "suite_runner.h"

#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_platform.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet.h"
#include "rocketnet_internal.h"

#include <stdio.h>
#include <string.h>

// The staged finish-path wire-total fold: single-threaded, no sockets, no
// real time (the clock is an argument, starting at 1000.0). Compiling the
// transport here gives the test the internal layout it swaps the fake staged
// transport into, exactly as test_transport_control.cpp does; the loopback
// backend's in-memory create is used so no real socket is ever opened.
#include "../sdk/rocketnet_server_transport.cpp"

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

static void set_ipv4_loopback( rocketnet_address_t & address )
{
    memset( &address.data, 0, sizeof( address.data ) );
    address.type = ROCKETNET_ADDRESS_IPV4;
    address.data.ipv4[0] = 127;
    address.data.ipv4[3] = 1;
}

static void test_loopback_transport_io_binds_and_discovers_a_port()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_transport_io_config_t setup = { "loopback", &server_config, NULL, NULL };

    rocketnet_address_t a;
    rocketnet_unit_check( rocketnet_address_parse( &a, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t sa = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &setup, &a, &sa ) );
    rocketnet_unit_check( a.port != 0 );

    rocketnet_address_t b = a;
    rocketnet_server_transport_io_t sb = {};
    rocketnet_unit_check( !rocketnet_server_transport_io_open( backend->server, NULL, &setup, &b, &sb ) );

    rocketnet_address_t c;
    rocketnet_unit_check( rocketnet_address_parse( &c, "[::]:0" ) );
    rocketnet_server_transport_io_t sc = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &setup, &c, &sc ) );
    rocketnet_unit_check( c.port != 0 );
    rocketnet_unit_check( c.port != a.port );

    rn_test_server_io_stop_and_close( &sa );
    rn_test_server_io_stop_and_close( &sc );
    printf( "test_loopback_transport_io_binds_and_discovers_a_port\n" );
}

static void test_loopback_transport_io_exchanges_both_ways()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };

    rocketnet_address_t server_address, client_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "0.0.0.0:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_client_transport_io_t client = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    const uint8_t data[] = { 10, 20, 30, 40 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    client.operations->send( client.instance, &to, data, sizeof( data ) );

    rocketnet_address_t from;
    uint8_t buf[64];
    int bytes = 0;
    const int received = server.operations->receive_batch( server.instance, &from, buf, 64, &bytes, 1 );
    rocketnet_unit_check( received == 1 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
    rocketnet_unit_check( from.port == client_address.port );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.data.ipv4[0] == 127 && from.data.ipv4[3] == 1 );

    rocketnet_platform_send_t packet = { &from, data, (int) sizeof( data ) };
    server.operations->send_batch( server.instance, &packet, 1 );

    bytes = client.operations->receive( client.instance, &from, buf, 64 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
    rocketnet_unit_check( from.port == server_address.port );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.data.ipv4[0] == 127 && from.data.ipv4[3] == 1 );

    rocketnet_unit_check( client.operations->receive( client.instance, &from, buf, 64 ) == 0 );

    rn_test_server_io_stop_and_close( &server );
    rocketnet_client_transport_io_close( &client );
    printf( "test_loopback_transport_io_exchanges_both_ways\n" );
}

// a server bound to the IPv6 wildcard is dual stack: it serves an IPv4
// client and an IPv6 client on the one port, and each reply comes from the
// loopback of the family the client sent to, so each client sees its
// server at the address it addressed.

static void test_loopback_transport_io_dual_stack_serves_ipv4_and_ipv6()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };

    rocketnet_address_t server_address, client4_address, client6_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "[::]:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client4_address, "0.0.0.0:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client6_address, "[::]:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_client_transport_io_t client4 = {};
    rocketnet_client_transport_io_t client6 = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client4_address, &client4 ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client6_address, &client6 ) );

    const uint8_t data4[] = { 4, 4, 4, 4 };
    const uint8_t data6[] = { 6, 6, 6, 6, 6, 6 };

    rocketnet_address_t to4 = server_address;
    set_ipv4_loopback( to4 );
    rocketnet_address_t to6;
    rocketnet_unit_check( rocketnet_address_parse( &to6, "[::1]:0" ) );
    to6.port = server_address.port;

    client4.operations->send( client4.instance, &to4, data4, sizeof( data4 ) );
    client6.operations->send( client6.instance, &to6, data6, sizeof( data6 ) );

    rocketnet_address_t from[2];
    uint8_t buf[2 * 64];
    int bytes[2] = { 0, 0 };
    const int received = server.operations->receive_batch( server.instance, from, buf, 64, bytes, 2 );
    rocketnet_unit_check( received == 2 );
    rocketnet_unit_check( bytes[0] == (int) sizeof( data4 ) && memcmp( buf, data4, sizeof( data4 ) ) == 0 );
    rocketnet_unit_check( from[0].type == ROCKETNET_ADDRESS_IPV4 && from[0].port == client4_address.port );
    rocketnet_unit_check( bytes[1] == (int) sizeof( data6 ) && memcmp( buf + 64, data6, sizeof( data6 ) ) == 0 );
    rocketnet_unit_check( from[1].type == ROCKETNET_ADDRESS_IPV6 && from[1].port == client6_address.port );

    rocketnet_platform_send_t replies[2] = { { &from[0], data4, (int) sizeof( data4 ) },
                                             { &from[1], data6, (int) sizeof( data6 ) } };
    server.operations->send_batch( server.instance, replies, 2 );

    rocketnet_address_t reply_from;
    uint8_t reply[64];
    int reply_bytes = client4.operations->receive( client4.instance, &reply_from, reply, 64 );
    rocketnet_unit_check( reply_bytes == (int) sizeof( data4 ) && memcmp( reply, data4, sizeof( data4 ) ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &reply_from, &to4 ) );

    reply_bytes = client6.operations->receive( client6.instance, &reply_from, reply, 64 );
    rocketnet_unit_check( reply_bytes == (int) sizeof( data6 ) && memcmp( reply, data6, sizeof( data6 ) ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &reply_from, &to6 ) );

    rn_test_server_io_stop_and_close( &server );
    rocketnet_client_transport_io_close( &client4 );
    rocketnet_client_transport_io_close( &client6 );
    printf( "test_loopback_transport_io_dual_stack_serves_ipv4_and_ipv6\n" );
}

static void test_loopback_transport_io_full_queue_drops_and_counts()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };
    rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_client_transport_io_t client = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    const int total = ROCKETNET_LOOPBACK_QUEUE_PACKETS + 5;
    uint8_t data[4];
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    for ( int i = 0; i < total; ++i )
    {
        data[0] = (uint8_t) ( i & 0xff );
        data[1] = (uint8_t) ( ( i >> 8 ) & 0xff );
        data[2] = (uint8_t) ( ( i >> 16 ) & 0xff );
        data[3] = 0;
        client.operations->send( client.instance, &to, data, 4 );
    }
    rocketnet_unit_check( rocketnet_loopback_endpoint_dropped( server.instance ) == 5 );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_QUEUE_PACKETS; ++i )
    {
        uint8_t buf[4];
        int bytes = 0;
        rocketnet_address_t from;
        const int n = server.operations->receive_batch( server.instance, &from, buf, 4, &bytes, 1 );
        rocketnet_unit_check( n == 1 );
        rocketnet_unit_check( bytes == 4 );
        const uint32_t v = (uint32_t) buf[0] | ( (uint32_t) buf[1] << 8 ) | ( (uint32_t) buf[2] << 16 );
        rocketnet_unit_check( v == (uint32_t) i );
        rocketnet_unit_check( from.port == client_address.port );
    }

    rocketnet_address_t from;
    uint8_t buf[4];
    int bytes = 0;
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, buf, 4, &bytes, 1 ) == 0 );

    rn_test_server_io_stop_and_close( &server );
    rocketnet_client_transport_io_close( &client );
    printf( "test_loopback_transport_io_full_queue_drops_and_counts\n" );
}

// The allocator hook for test_loopback_transport_io_runtime_does_not_allocate: it
// counts the bytes rocketnet_malloc asks for and passes every call to
// rocketnet_default_malloc_function and rocketnet_default_free_function, so
// the platform allocator (and its Debug fill) is unchanged. A small header in
// front of each block keeps the requested size for the free.

enum { LOOPBACK_COUNT_HEADER = 16 };
static size_t g_loopback_live_bytes;
static size_t g_loopback_allocation_calls;

static void * loopback_count_malloc( void * context, size_t bytes )
{
    ++g_loopback_allocation_calls;
    uint8_t * block = (uint8_t*) rocketnet_default_malloc_function( context, bytes + LOOPBACK_COUNT_HEADER );
    if ( !block )
        return NULL;
    memcpy( block, &bytes, sizeof( bytes ) );
    g_loopback_live_bytes += bytes;
    return block + LOOPBACK_COUNT_HEADER;
}

static void loopback_count_free( void * context, void * memory )
{
    if ( !memory )
        return;
    uint8_t * block = (uint8_t*) memory - LOOPBACK_COUNT_HEADER;
    size_t bytes;
    memcpy( &bytes, block, sizeof( bytes ) );
    g_loopback_live_bytes -= bytes;
    rocketnet_default_free_function( context, block );
}

// Full-size datagrams exercise both send/receive paths through two complete
// fill/drain rounds, including ring wrap and drops beyond the fixed ceiling.
// Count calls, not live bytes: a temporary allocation followed by a free must
// still fail the no-runtime-allocation contract.

static void test_loopback_transport_io_runtime_does_not_allocate()
{
    rocketnet_unit_check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_init( NULL, loopback_count_malloc, loopback_count_free ) == ROCKETNET_STATUS_OK );

    const size_t overhead = (size_t) rocketnet_allocation_overhead();
    const size_t live_before = g_loopback_live_bytes;

    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    const size_t server_create_bytes = g_loopback_live_bytes - live_before;
    // the server endpoint's queue is sized from the plan (rocketnet_loopback_server_queue_packets)
    rocketnet_unit_check( server_create_bytes == rocketnet_loopback_queue_bytes( rocketnet_loopback_server_queue_packets( server_config.max_clients ) ) + overhead );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };
    rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_client_transport_io_t client = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_address, &client ) );
    const size_t created_bytes = server_create_bytes + rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS ) + overhead;
    rocketnet_unit_check( g_loopback_live_bytes - live_before == created_bytes );

    const size_t allocation_calls_after_create = g_loopback_allocation_calls;
    uint8_t data[ROCKETNET_RECEIVE_CEILING];
    memset( data, 0xa5, sizeof( data ) );
    rocketnet_address_t to_server = server_address;
    rocketnet_address_t to_client = client_address;
    set_ipv4_loopback( to_server );
    set_ipv4_loopback( to_client );
    for ( int round = 0; round < 2; ++round )
    {
        for ( int i = 0; i < ROCKETNET_LOOPBACK_QUEUE_PACKETS + 5; ++i )
        {
            const uint32_t sequence = (uint32_t) ( round * ROCKETNET_LOOPBACK_QUEUE_PACKETS + i );
            memcpy( data, &sequence, sizeof( sequence ) );
            client.operations->send( client.instance, &to_server, data, sizeof( data ) );
            rocketnet_platform_send_t packet = { &to_client, data, sizeof( data ) };
            server.operations->send_batch( server.instance, &packet, 1 );
        }
        rocketnet_unit_check( g_loopback_allocation_calls == allocation_calls_after_create );
        rocketnet_unit_check( rocketnet_loopback_endpoint_dropped( server.instance ) == (uint64_t) ( round + 1 ) * 5 );
        rocketnet_unit_check( rocketnet_loopback_endpoint_dropped( client.instance ) == (uint64_t) ( round + 1 ) * 5 );
        for ( int i = 0; i < ROCKETNET_LOOPBACK_QUEUE_PACKETS; ++i )
        {
            const uint32_t sequence = (uint32_t) ( round * ROCKETNET_LOOPBACK_QUEUE_PACKETS + i );
            memcpy( data, &sequence, sizeof( sequence ) );
            uint8_t received[ROCKETNET_RECEIVE_CEILING];
            int bytes = 0;
            rocketnet_address_t from;
            rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, received, sizeof( received ), &bytes, 1 ) == 1 );
            rocketnet_unit_check( bytes == (int) sizeof( data ) );
            rocketnet_unit_check( memcmp( data, received, sizeof( data ) ) == 0 );
            rocketnet_unit_check( from.port == client_address.port );
            rocketnet_unit_check( client.operations->receive( client.instance, &from, received, sizeof( received ) ) == (int) sizeof( data ) );
            rocketnet_unit_check( memcmp( data, received, sizeof( data ) ) == 0 );
            rocketnet_unit_check( from.port == server_address.port );
        }
        rocketnet_unit_check( g_loopback_allocation_calls == allocation_calls_after_create );
    }
    uint8_t received[ROCKETNET_RECEIVE_CEILING];
    int bytes = 0;
    rocketnet_address_t from;
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, received, sizeof( received ), &bytes, 1 ) == 0 );
    rocketnet_unit_check( client.operations->receive( client.instance, &from, received, sizeof( received ) ) == 0 );
    rocketnet_unit_check( g_loopback_allocation_calls == allocation_calls_after_create );
    rocketnet_unit_check( g_loopback_live_bytes - live_before == created_bytes );

    rn_test_server_io_stop_and_close( &server );
    rocketnet_client_transport_io_close( &client );
    rocketnet_unit_check( g_loopback_live_bytes == live_before );

    rocketnet_unit_check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    printf( "test_loopback_transport_io_runtime_does_not_allocate\n" );
}

static void test_loopback_transport_io_destroy_frees_the_port()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    const int live_before = rocketnet_loopback_endpoints_live();

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    const uint16_t port = server_address.port;

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };
    rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_client_transport_io_t client = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    rn_test_server_io_stop_and_close( &server );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + 1 );

    uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    client.operations->send( client.instance, &to, data, 4 );
    rocketnet_unit_check( client.operations->receive( client.instance, &to, data, 4 ) == 0 );

    rocketnet_address_t again;
    rocketnet_unit_check( rocketnet_address_parse( &again, "127.0.0.1:0" ) );
    again.port = port;
    rocketnet_server_transport_io_t server2 = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &again, &server2 ) );
    rocketnet_unit_check( again.port == port );

    rn_test_server_io_stop_and_close( &server2 );
    rocketnet_client_transport_io_close( &client );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before );
    printf( "test_loopback_transport_io_destroy_frees_the_port\n" );
}


// The transport compiled into this TU includes <sodium.h>. CMakeLists.txt puts
// the vendored sodium/ on this one source's include path (as the transport's
// own targets have it), so its calls reach the renamed implementations the
// production archive carries; a ROCKETNET_USE_SYSTEM_SODIUM build links the
// system library instead. No original-name shim is needed either way, and the
// test no longer depends on a system sodium header being installed.

struct wire_totals_staged_fixture_t
{
    rocketnet_address_t reserved_to;
    // the padding to frame and after capacity is written out, so the
    // compiler adds none (MSVC C4324)
    uint8_t head_pad[( sizeof( rocketnet_address_t ) + 15 ) / 16 * 16 - sizeof( rocketnet_address_t )];
    // start_packet hands back a FRAME_STRIDE root, and the transport asserts
    // it is 16-byte aligned (rocketnet_server_transport_start_packet). Placed
    // after the address with no alignas, the frame is not.
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE];
    int used;
    int capacity;
    uint8_t tail_pad[16 - 2 * sizeof( int )];
};

static void wt_staged_destroy(void *) {}
static void wt_staged_stop( void * ) {}
static bool wt_staged_stopped( void * ) { return true; }
static void wt_staged_send_batch(void *, const rocketnet_platform_send_t *, int) {}
static int wt_staged_receive_batch(void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity)
{
    (void)p; (void)from; (void)data; (void)stride; (void)bytes; (void)capacity;
    return 0;
}
static rocketnet_server_socket_process_packets_t * wt_staged_receive(void *) { return NULL; }
static uint8_t * wt_staged_start_packet(void * p, const rocketnet_address_t * to, uint64_t * id)
{
    wire_totals_staged_fixture_t * f = (wire_totals_staged_fixture_t*) p;
    if ( f->used == f->capacity ) return NULL;
    f->reserved_to = *to;
    *id = (uint64_t) f->used;
    return f->frame;
}
static void wt_staged_finish_packet(void * p, uint64_t, uint8_t *, int)
{
    ++((wire_totals_staged_fixture_t*)p)->used;
}
static void wt_staged_abort_packet(void * p, uint64_t, uint8_t *) { (void)p; }
static void wt_staged_flush(void * p) { ((wire_totals_staged_fixture_t*)p)->used = 0; }

static void test_wire_totals_staged_finish_counts_exactly()
{
    // the staged producer path seals into the fake transport's staging, bumps
    // server_stats and the per-client totals with the relaxed atomics, and
    // the public stats readers fold them back; a non-atomic add either loses
    // an increment here (without a thread) or trips the tsan gate upstream.

    wire_totals_staged_fixture_t fx;
    memset( &fx, 0, sizeof(fx) );
    fx.capacity = 1024;

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = 4;
    config.protocol_id = 1;
    snprintf( config.transport, sizeof(config.transport), "%s", "loopback" );
    // production create refuses dev keys and zero keys; any non-zero, non-dev
    // bytes satisfy the key check for this fake-transport fixture, which never
    // runs a real handshake
    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );

    rocketnet_server_transport_io_operations_t operations;
    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    if ( !server ) printf( "create_ex status=%d\n", create_status );
    rocketnet_unit_check( server );
    rn_test_server_io_stop_and_close( &server->io );
    memset( &operations, 0, sizeof(operations) );
    operations.destroy = wt_staged_destroy;
    operations.stop = wt_staged_stop; operations.stopped = wt_staged_stopped;
    operations.send_batch = wt_staged_send_batch;
    operations.receive_batch = wt_staged_receive_batch;
    operations.flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
    operations.start_packet = wt_staged_start_packet;
    operations.finish_packet = wt_staged_finish_packet;
    operations.abort_packet = wt_staged_abort_packet;
    operations.flush = wt_staged_flush;
    operations.receive = wt_staged_receive;
    server->io.instance = &fx;
    server->io.operations = &operations;

    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_address_t address;
        rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
        address.port = (uint16_t) ( 1000 + i );
        const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 100.0 );
        rocketnet_unit_check( index >= 0 );
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, index );
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
        rocketnet_unit_check( rocketnet_client_slots_assign( server->protocol.client_slots, (uint64_t) ( i + 1 ), index, user_data ) == i );
        mapping->control->client_index = i;
        mapping->client_id = (uint64_t) ( i + 1 );
        mapping->control->established = 1;
        mapping->control->timeout_seconds = 1000;
        memset( &mapping->keys, i + 1, sizeof(mapping->keys) );
        server->protocol.acks->endpoints[i].rtt_history[0] = 17.0f;
    }

    const int N = 200;
    uint64_t total_wire = 0;
    for ( int i = 0; i < N; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
        rocketnet_unit_check( frame );
        memset( frame + context.payload_index, i & 0xff, 16 );
        const int wire_bytes = rocketnet_server_transport_finish_packet( server, &context, 16 );
        rocketnet_unit_check( wire_bytes > 0 );
        total_wire += (uint64_t) wire_bytes;
    }

    rocketnet_server_transport_send_packets( server );

    const uint64_t overhead = (uint64_t)( ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

    struct rocketnet_server_transport_stats_t sstats;
    memset( &sstats, 0, sizeof(sstats) );
    rocketnet_server_transport_stats( server, &sstats );
    rocketnet_unit_check( sstats.packets_sent == (uint64_t) N );
    rocketnet_unit_check( sstats.bytes_sent == total_wire + (uint64_t) N * overhead );

    struct rocketnet_server_transport_client_stats_t cstats;
    memset( &cstats, 0, sizeof(cstats) );
    rocketnet_server_transport_client_stats( server, 0, &cstats );
    rocketnet_unit_check( cstats.packets_sent == (uint64_t) N );
    rocketnet_unit_check( cstats.bytes_sent == total_wire + (uint64_t) N * overhead );

    rn_test_server_transport_stop_and_destroy( server );
    printf( "test_wire_totals_staged_finish_counts_exactly\n" );
}

// A zero-length datagram a batch receive hands the server transport (the
// Windows portable receive does) is counted on the prefilter as
// PREFILTER_TOO_SHORT, with the runts and oversize datagrams beside it: every
// dropped datagram is counted. A scripted transport, no socket, a test clock.
struct empty_datagram_fixture_t
{
    int reported[8];
    int count;
    int head;
};

static void empty_datagram_destroy(void *) {}
static void empty_datagram_stop( void * ) {}
static bool empty_datagram_stopped( void * ) { return true; }
static void empty_datagram_send_batch(void *, const rocketnet_platform_send_t *, int) {}
static int empty_datagram_receive_batch(void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity)
{
    empty_datagram_fixture_t * f = (empty_datagram_fixture_t*) p;
    int got = 0;
    while ( got < capacity && f->head < f->count )
    {
        rocketnet_unit_check( rocketnet_address_parse( &from[got], "127.0.0.1:5000" ) );
        const int reported = f->reported[f->head++];
        const int written = reported < stride ? reported : stride;
        if ( written > 0 )
            memset( data + (size_t) got * (size_t) stride, 0x5a, (size_t) written );
        bytes[got] = reported;
        got++;
    }
    return got;
}
static rocketnet_server_socket_process_packets_t * empty_datagram_receive(void *) { return NULL; }

static void test_server_counts_zero_length_datagram_as_too_short()
{
    empty_datagram_fixture_t fx;
    memset( &fx, 0, sizeof(fx) );
    fx.reported[fx.count++] = 0;
    fx.reported[fx.count++] = 0;
    fx.reported[fx.count++] = 1;
    fx.reported[fx.count++] = ROCKETNET_RECEIVE_CEILING + 1;
    fx.reported[fx.count++] = 0;

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = 4;
    config.protocol_id = 1;
    snprintf( config.transport, sizeof(config.transport), "%s", "loopback" );
    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );

    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    rocketnet_unit_check( server );
    rn_test_server_io_stop_and_close( &server->io );
    rocketnet_server_transport_io_operations_t operations;
    memset( &operations, 0, sizeof(operations) );
    operations.destroy = empty_datagram_destroy;
    operations.stop = empty_datagram_stop; operations.stopped = empty_datagram_stopped;
    operations.send_batch = empty_datagram_send_batch;
    operations.receive_batch = empty_datagram_receive_batch;
    operations.receive = empty_datagram_receive;
    operations.flags = ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
    server->io.instance = &fx;
    server->io.operations = &operations;

    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    const uint64_t short_before = rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_TOO_SHORT );
    const uint64_t large_before = rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_TOO_LARGE );

    rocketnet_server_transport_update( server, 1000.0 );

    rocketnet_unit_check( fx.head == fx.count );
    counters = rocketnet_server_transport_counters( server );
    rocketnet_unit_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_TOO_SHORT ) - short_before == 4 );
    rocketnet_unit_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_PREFILTER_TOO_LARGE ) - large_before == 1 );

    rn_test_server_transport_stop_and_destroy( server );
    printf( "test_server_counts_zero_length_datagram_as_too_short\n" );
}

// Both endpoint-count cases intentionally fill the process-wide table. Use
// the same full-queue byte calculation as loopback_create, including each
// SDK allocation header, plus 32 MiB for the executable, CRT bookkeeping,
// static endpoint table and the cases' small transport/address/batch arrays.
// The workload and endpoint counts stay unchanged. Native Debug fills every
// allocation; an additional unplanned 64 MiB must exceed this bound.
static long loopback_full_table_rss_bound_mb()
{
    const uint64_t endpoint_bytes = rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS ) + rocketnet_allocation_overhead();
    return suite_rss_bound_mb( (uint64_t) ROCKETNET_LOOPBACK_MAX_ENDPOINTS * endpoint_bytes, 32 );
}

SUITE_CASE_RSS_BOUND( test_loopback_transport_io_holds_max_clients_plus_server, loopback_full_table_rss_bound_mb );
SUITE_CASE_RSS_BOUND( test_loopback_transport_io_refuses_past_max_endpoints, loopback_full_table_rss_bound_mb );

static void test_loopback_transport_io_holds_max_clients_plus_server()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    const int live_before = rocketnet_loopback_endpoints_live();

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( server_address.port != 0 );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };
    rocketnet_client_transport_io_t * clients = (rocketnet_client_transport_io_t*) calloc( ROCKETNET_MAX_CLIENTS, sizeof( rocketnet_client_transport_io_t ) );
    rocketnet_address_t * client_addresses = (rocketnet_address_t*) calloc( ROCKETNET_MAX_CLIENTS, sizeof( rocketnet_address_t ) );
    rocketnet_unit_check( clients && client_addresses );

    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        rocketnet_unit_check( rocketnet_address_parse( &client_addresses[i], "127.0.0.1:0" ) );
        rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_addresses[i], &clients[i] ) );
        rocketnet_unit_check( client_addresses[i].port != 0 );
    }
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS );
    if ( live_before == 0 )
        rocketnet_unit_check( rocketnet_loopback_endpoints_live() == ROCKETNET_LOOPBACK_MAX_ENDPOINTS );

    const uint8_t data[] = { 1, 2, 3, 4 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
        clients[i].operations->send( clients[i].instance, &to, data, sizeof( data ) );

    const int capacity = 64;
    const int stride = ROCKETNET_RECEIVE_CEILING;
    uint8_t * batch = (uint8_t*) malloc( (size_t) stride * capacity );
    rocketnet_address_t * froms = (rocketnet_address_t*) malloc( sizeof( rocketnet_address_t ) * capacity );
    int * batch_bytes = (int*) malloc( sizeof( int ) * capacity );
    bool * seen = (bool*) calloc( ROCKETNET_MAX_CLIENTS, sizeof( bool ) );
    rocketnet_unit_check( batch && froms && batch_bytes && seen );

    int total = 0;
    while ( total < ROCKETNET_MAX_CLIENTS )
    {
        const int received = server.operations->receive_batch( server.instance, froms, batch, stride, batch_bytes, capacity );
        rocketnet_unit_check( received > 0 );
        for ( int i = 0; i < received; ++i )
        {
            rocketnet_unit_check( batch_bytes[i] == (int) sizeof( data ) );
            rocketnet_unit_check( memcmp( batch + (size_t) i * stride, data, sizeof( data ) ) == 0 );
            int index = -1;
            for ( int c = 0; c < ROCKETNET_MAX_CLIENTS; ++c )
                if ( client_addresses[c].port == froms[i].port )
                    { index = c; break; }
            rocketnet_unit_check( index >= 0 );
            rocketnet_unit_check( !seen[index] );
            seen[index] = true;
            ++total;
        }
    }
    rocketnet_unit_check( total == ROCKETNET_MAX_CLIENTS );

    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        rocketnet_platform_send_t packet = { &client_addresses[i], data, (int) sizeof( data ) };
        server.operations->send_batch( server.instance, &packet, 1 );
    }
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        rocketnet_address_t from;
        uint8_t buf[64];
        const int bytes = clients[i].operations->receive( clients[i].instance, &from, buf, 64 );
        rocketnet_unit_check( bytes == (int) sizeof( data ) );
        rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
        rocketnet_unit_check( from.port == server_address.port );
    }

    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
        rocketnet_client_transport_io_close( &clients[i] );
    rn_test_server_io_stop_and_close( &server );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before );

    free( batch );
    free( froms );
    free( batch_bytes );
    free( seen );
    free( clients );
    free( client_addresses );
    printf( "test_loopback_transport_io_holds_max_clients_plus_server\n" );
}

static char g_loopback_refusal_log[1024];
static int g_loopback_refusal_log_count;

static void loopback_refusal_log_capture( void *, enum rocketnet_log_level_t, const char * message, size_t length )
{
    g_loopback_refusal_log_count++;
    const size_t copy = length < sizeof( g_loopback_refusal_log ) - 1 ? length : sizeof( g_loopback_refusal_log ) - 1;
    memcpy( g_loopback_refusal_log, message, copy );
    g_loopback_refusal_log[copy] = '\0';
}

static void test_loopback_transport_io_refuses_past_max_endpoints()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    const int live_before = rocketnet_loopback_endpoints_live();
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    rocketnet_log_function( loopback_refusal_log_capture, NULL );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };
    rocketnet_client_transport_io_t * clients = (rocketnet_client_transport_io_t*) calloc( ROCKETNET_LOOPBACK_MAX_ENDPOINTS, sizeof( rocketnet_client_transport_io_t ) );
    rocketnet_unit_check( clients );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
    {
        rocketnet_address_t address;
        rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
        rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &address, &clients[i] ) );
    }
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS );

    g_loopback_refusal_log_count = 0;
    g_loopback_refusal_log[0] = '\0';
    rocketnet_address_t extra;
    rocketnet_unit_check( rocketnet_address_parse( &extra, "127.0.0.1:0" ) );
    rocketnet_client_transport_io_t refused = {};
    rocketnet_unit_check( !rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &extra, &refused ) );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS );

    char value[32];
    snprintf( value, sizeof( value ), "%d", (int) ROCKETNET_LOOPBACK_MAX_ENDPOINTS );
    rocketnet_unit_check( g_loopback_refusal_log_count == 1 );
    rocketnet_unit_check( strstr( g_loopback_refusal_log, "ROCKETNET_LOOPBACK_MAX_ENDPOINTS" ) != NULL );
    rocketnet_unit_check( strstr( g_loopback_refusal_log, value ) != NULL );

    rocketnet_client_transport_io_close( &clients[0] );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS - 1 );

    rocketnet_address_t again;
    rocketnet_unit_check( rocketnet_address_parse( &again, "127.0.0.1:0" ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &again, &clients[0] ) );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
        rocketnet_client_transport_io_close( &clients[i] );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );
    free( clients );
    printf( "test_loopback_transport_io_refuses_past_max_endpoints\n" );
}

// Junk from many sources takes no per-source state: the portable path reads
// 300 one-byte datagrams, each from its own address, then a real client still
// connects, and its slot's wire totals start at the slot's assignment. The
// table that once held each unknown source's totals is gone (Glenn,
// 2026-10-21: request bytes are not tracked), so nothing here can fill.

struct junk_sources_fixture_t
{
    int next;
};

static struct junk_sources_fixture_t junk_sources_fixture;
static double junk_sources_test_time;

static double junk_sources_test_clock( void * )
{
    return junk_sources_test_time;
}

static int junk_sources_receive_batch( void *, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    const int remaining = 300 - junk_sources_fixture.next;
    const int received = remaining < count ? remaining : count;
    for ( int i = 0; i < received; ++i )
    {
        const int source = junk_sources_fixture.next++;
        memset( &from[i], 0, sizeof(from[i]) );
        from[i].type = ROCKETNET_ADDRESS_IPV4;
        from[i].data.ipv4[0] = 10;
        from[i].data.ipv4[2] = (uint8_t) ( source >> 8 );
        from[i].data.ipv4[3] = (uint8_t) source;
        from[i].port = (uint16_t) ( 1000 + source );
        bytes[i] = 1;
        data[(size_t) i * (size_t) stride] = 0;
    }
    return received;
}

static void test_server_connects_after_junk_from_many_sources()
{
    const uint64_t protocol_id = UINT64_C(0x70656e64696e6777);
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = 4;
    config.protocol_id = protocol_id;
    config.handshake_deadline_seconds = 1;
    snprintf( config.transport, sizeof(config.transport), "%s", "loopback" );
    memcpy( config.backend_public_key, backend_public_key, sizeof(config.backend_public_key) );
    memcpy( config.server_public_key, server_public_key, sizeof(config.server_public_key) );
    memcpy( config.server_secret_key, server_secret_key, sizeof(config.server_secret_key) );

    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    if ( !server ) printf( "create_ex status=%d\n", create_status );
    rocketnet_unit_check( server );

    rocketnet_server_transport_io_operations_t scan_operations = *server->io.operations;
    scan_operations.receive_batch = junk_sources_receive_batch;
    server->io.operations = &scan_operations;
    memset( &junk_sources_fixture, 0, sizeof(junk_sources_fixture) );
    rocketnet_server_transport_update( server, 1000.01 );
    rocketnet_unit_check( junk_sources_fixture.next == 300 );
    rocketnet_unit_check( server->server_stats.packets_received == 300 );
    rocketnet_server_transport_update( server, 1001.01 );
    server->io.operations = rocketnet_loopback_transport.server;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );
    const char * addresses[] = { server_address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id,
        addresses, 1, UINT64_C(0x5057495245), 0, UINT64_C(1) << 40, 60, NULL, token, sizeof(token) ) == ROCKETNET_STATUS_OK );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "%s", "loopback" );
    client_config.step_driven = 1;
    client_config.time_callback = junk_sources_test_clock;
    junk_sources_test_time = 1001.02;
    rocketnet_client_transport_t * client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof(client_config), junk_sources_test_time );
    rocketnet_unit_check( client );

    for ( int i = 0; i < 100; ++i )
    {
        junk_sources_test_time = 1001.02 + i * 0.01;
        rocketnet_server_transport_update( server, junk_sources_test_time );
        rocketnet_client_transport_update( client, junk_sources_test_time );
        rocketnet_server_transport_send_packets( server );
        rocketnet_client_transport_send_packets( client );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_is_client_connected( server, 0 ) )
            break;
    }
    rocketnet_unit_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_unit_check( rocketnet_server_transport_is_client_connected( server, 0 ) );

    // a few more frames, so the connected client's keep-alives arrive
    const double connected_time = junk_sources_test_time;
    for ( int i = 1; i <= 50; ++i )
    {
        junk_sources_test_time = connected_time + i * 0.01;
        rocketnet_server_transport_update( server, junk_sources_test_time );
        rocketnet_client_transport_update( client, junk_sources_test_time );
        rocketnet_server_transport_send_packets( server );
        rocketnet_client_transport_send_packets( client );
    }
    rocketnet_unit_check( rocketnet_server_transport_is_client_connected( server, 0 ) );

    // the slot's totals start at assignment: the handshake's packets count in
    // the whole-server totals only, so the slot holds strictly fewer
    rocketnet_unit_check( server->client_wire_totals[0].packets_received > 0 );
    rocketnet_unit_check( server->client_wire_totals[0].packets_received < server->server_stats.packets_received - 300 );

    rn_test_client_transport_stop_and_destroy( client );
    rn_test_server_transport_stop_and_destroy( server );
    printf( "test_server_connects_after_junk_from_many_sources\n" );
}

// The shape check runs first (rocketnet_server_packet_shape_valid): a
// datagram that fails the prefilter or the server-inbound type and size rule
// gets no mapping lookup, no per-slot totals and no send frame, on both
// receive loops, and every refusal counter moves exactly as before. One
// datagram of each failing shape, each from its own unmapped source, and (on
// the staged loop) one more from a connected client's own address, which
// must not reach that slot's totals either.

#define SHAPE_JUNK_COUNT 9

struct shape_junk_t
{
    rocketnet_address_t from[SHAPE_JUNK_COUNT];
    int bytes[SHAPE_JUNK_COUNT];
    uint8_t data[SHAPE_JUNK_COUNT][128];
};

static void shape_junk_build( struct shape_junk_t * junk, const uint8_t * filter, const rocketnet_address_t * connected )
{
    memset( junk, 0, sizeof(*junk) );
    const uint8_t types[SHAPE_JUNK_COUNT] = {
        0,                                          // one byte: too short
        ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,     // wrong epoch
        ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,     // wrong filter value
        10,                                         // reserved type
        ROCKETNET_CONNECTION_COOKIE_PACKET,         // server-to-client only
        ROCKETNET_CONNECTION_REQUEST_PACKET,        // request, wrong size
        ROCKETNET_CONNECTION_RESPONSE_PACKET,       // response, wrong size
        ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,     // keep-alive below its floor
        ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,     // the same, from the connected client
    };
    const int bytes[SHAPE_JUNK_COUNT] = { 1, 40, 40, 40, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES, 100, 100, 10, 10 };
    for ( int i = 0; i < SHAPE_JUNK_COUNT; i++ )
    {
        junk->from[i].type = ROCKETNET_ADDRESS_IPV4;
        junk->from[i].data.ipv4[0] = 10;
        junk->from[i].data.ipv4[3] = (uint8_t) ( i + 1 );
        junk->from[i].port = (uint16_t) ( 2000 + i );
        junk->bytes[i] = bytes[i];
        uint8_t * d = junk->data[i];
        d[0] = types[i];
        d[1] = i == 1 ? (uint8_t) ( ROCKETNET_FILTER_EPOCH_UNKEYED + 1 ) : (uint8_t) ROCKETNET_FILTER_EPOCH_UNKEYED;
        memcpy( d + 2, filter, ROCKETNET_FILTER_VALUE_BYTES );
        if ( i == 2 )
            d[2] ^= 0xFF;
    }
    if ( connected )
        junk->from[SHAPE_JUNK_COUNT - 1] = *connected;
}

static void shape_junk_expect_counters( const struct rocketnet_counters_t * before, const struct rocketnet_counters_t * after )
{
    rocketnet_counters_expect( before, after,
                               { { ROCKETNET_COUNTER_PACKETS_RECEIVED, SHAPE_JUNK_COUNT },
                                 { ROCKETNET_COUNTER_PREFILTER_TOO_SHORT, 1 },
                                 { ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH, 1 },
                                 { ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE, 1 },
                                 { ROCKETNET_COUNTER_RESERVED_TYPE, 1 },
                                 { ROCKETNET_COUNTER_WRONG_DIRECTION, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE, 1 },
                                 { ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, 2 },
                                 { ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE, 2 } } );
}

// a connected client in slot 0 at 10.0.0.200:3000, as the staged wire-totals
// case above builds its clients
static void shape_junk_connect_slot0( rocketnet_server_transport_t * server, rocketnet_address_t * address )
{
    memset( address, 0, sizeof(*address) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = 10;
    address->data.ipv4[3] = 200;
    address->port = 3000;
    const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, address, 2000.0 );
    rocketnet_unit_check( index >= 0 );
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, index );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    rocketnet_unit_check( rocketnet_client_slots_assign( server->protocol.client_slots, 1, index, user_data ) == 0 );
    mapping->control->client_index = 0;
    mapping->client_id = 1;
    mapping->control->established = 1;
    mapping->control->timeout_seconds = 1000;
    mapping->control->last_receive_time = 1000.0;
    mapping->last_send_time = 1000.0;
    mapping->last_keep_alive_send_time = 1000.0;
    memset( &mapping->keys, 1, sizeof(mapping->keys) );
}

static rocketnet_server_transport_t * shape_junk_server_create()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = 4;
    config.protocol_id = 1;
    snprintf( config.transport, sizeof(config.transport), "%s", "loopback" );
    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );
    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    if ( !server ) printf( "create_ex status=%d\n", create_status );
    rocketnet_unit_check( server );
    rocketnet_server_transport_io_close( &server->io );
    return server;
}

struct shape_junk_staged_t
{
    struct shape_junk_t junk;
    rocketnet_server_socket_process_packets_t batch;
    rocketnet_address_t connected;
    int start_packet_calls;
    uint8_t * frame;                // shape_junk_staged_frame
};

// start_packet hands back a 16-byte aligned FRAME_STRIDE root. Kept out of
// the fixture struct: an alignas member there pads the struct (MSVC C4324).
alignas( 16 ) static uint8_t shape_junk_staged_frame[ROCKETNET_FRAME_STRIDE];

static rocketnet_server_socket_process_packets_t * shape_junk_staged_receive( void * p )
{
    struct shape_junk_staged_t * f = (struct shape_junk_staged_t*) p;
    return &f->batch;
}
static uint8_t * shape_junk_staged_start_packet( void * p, const rocketnet_address_t * to, uint64_t * id )
{
    struct shape_junk_staged_t * f = (struct shape_junk_staged_t*) p;
    // the connected slot's own maintenance sends are not junk reservations
    if ( !rocketnet_address_equal( to, &f->connected ) )
        f->start_packet_calls++;
    *id = 0;
    return f->frame;
}
static void shape_junk_staged_finish_packet( void *, uint64_t, uint8_t *, int ) {}
static void shape_junk_staged_abort_packet( void *, uint64_t, uint8_t * ) {}
static void shape_junk_staged_flush( void * ) {}

static void test_server_shape_check_first_on_the_staged_loop()
{
    rocketnet_server_transport_t * server = shape_junk_server_create();

    static struct shape_junk_staged_t fx;
    memset( &fx, 0, sizeof(fx) );
    fx.frame = shape_junk_staged_frame;
    rocketnet_server_transport_io_operations_t operations;
    memset( &operations, 0, sizeof(operations) );
    operations.destroy = wt_staged_destroy;
    operations.stop = wt_staged_stop; operations.stopped = wt_staged_stopped;
    operations.send_batch = wt_staged_send_batch;
    operations.receive_batch = wt_staged_receive_batch;
    operations.flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
    operations.start_packet = shape_junk_staged_start_packet;
    operations.finish_packet = shape_junk_staged_finish_packet;
    operations.abort_packet = shape_junk_staged_abort_packet;
    operations.flush = shape_junk_staged_flush;
    operations.receive = shape_junk_staged_receive;
    server->io.instance = &fx;
    server->io.operations = &operations;

    shape_junk_connect_slot0( server, &fx.connected );
    shape_junk_build( &fx.junk, server->protocol.packet_filter, &fx.connected );

    fx.batch.num_packets = SHAPE_JUNK_COUNT;
    for ( int i = 0; i < SHAPE_JUNK_COUNT; i++ )
    {
        fx.batch.from[i] = fx.junk.from[i];
        fx.batch.packet_bytes[i] = fx.junk.bytes[i];
        fx.batch.packet_data[i] = fx.junk.data[i];
    }

    const uint64_t received_before = server->server_stats.packets_received;
    const uint64_t slot_received_before = server->client_wire_totals[0].packets_received;
    const struct rocketnet_counters_t before = server->counters;
    rocketnet_server_transport_update( server, 1000.0 );

    rocketnet_unit_check( fx.start_packet_calls == 0 );
    rocketnet_unit_check( server->server_stats.packets_received == received_before + SHAPE_JUNK_COUNT );
    rocketnet_unit_check( server->client_wire_totals[0].packets_received == slot_received_before );
    rocketnet_unit_check( server->payloads.num_payloads == 0 );
    shape_junk_expect_counters( &before, &server->counters );

    rn_test_server_transport_stop_and_destroy( server );
    printf( "test_server_shape_check_first_on_the_staged_loop\n" );
}

static struct shape_junk_t shape_junk_portable;
static int shape_junk_portable_served;

static int shape_junk_portable_receive_batch( void *, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    if ( shape_junk_portable_served )
        return 0;
    rocketnet_unit_check( count >= SHAPE_JUNK_COUNT );
    for ( int i = 0; i < SHAPE_JUNK_COUNT; i++ )
    {
        from[i] = shape_junk_portable.from[i];
        bytes[i] = shape_junk_portable.bytes[i];
        memcpy( data + (size_t) i * (size_t) stride, shape_junk_portable.data[i], (size_t) shape_junk_portable.bytes[i] );
    }
    shape_junk_portable_served = 1;
    return SHAPE_JUNK_COUNT;
}

static void test_server_shape_check_first_on_the_portable_loop()
{
    rocketnet_server_transport_t * server = shape_junk_server_create();

    rocketnet_server_transport_io_operations_t operations;
    memset( &operations, 0, sizeof(operations) );
    operations.destroy = empty_datagram_destroy;
    operations.stop = empty_datagram_stop; operations.stopped = empty_datagram_stopped;
    operations.send_batch = empty_datagram_send_batch;
    operations.receive_batch = shape_junk_portable_receive_batch;
    operations.receive = empty_datagram_receive;
    operations.flags = ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
    server->io.instance = NULL;
    server->io.operations = &operations;

    // no connected slot here, so no maintenance send touches the send buffer:
    // all nine datagrams come from unmapped sources
    shape_junk_build( &shape_junk_portable, server->protocol.packet_filter, NULL );
    shape_junk_portable_served = 0;

    // a reserved portable frame writes its destination into the send buffer;
    // a marker there that survives the update proves no frame was reserved
    memset( &server->send_buffer.to[0], 0xA5, sizeof(server->send_buffer.to[0]) );
    rocketnet_address_t marker;
    memset( &marker, 0xA5, sizeof(marker) );

    const uint64_t received_before = server->server_stats.packets_received;
    const struct rocketnet_counters_t before = server->counters;
    rocketnet_server_transport_update( server, 1000.0 );

    rocketnet_unit_check( shape_junk_portable_served == 1 );
    rocketnet_unit_check( server->send_buffer.num_packets == 0 );
    rocketnet_unit_check( memcmp( &server->send_buffer.to[0], &marker, sizeof(marker) ) == 0 );
    rocketnet_unit_check( server->server_stats.packets_received == received_before + SHAPE_JUNK_COUNT );
    rocketnet_unit_check( server->payloads.num_payloads == 0 );
    shape_junk_expect_counters( &before, &server->counters );

    rn_test_server_transport_stop_and_destroy( server );
    printf( "test_server_shape_check_first_on_the_portable_loop\n" );
}

static void test_loopback_transport_server_batch_drops_packet_larger_than_stride()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };
    rocketnet_address_t server_address, client_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_client_transport_io_t client = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_address, &client ) );
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    const uint8_t oversized[] = { 1, 2, 3, 4 };
    client.operations->send( client.instance, &to, oversized, sizeof(oversized) );
    rocketnet_address_t from;
    uint8_t data[2];
    int bytes = 0;
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, data, sizeof(data), &bytes, 1 ) == 0 );

    const uint8_t valid[] = { 5, 6 };
    client.operations->send( client.instance, &to, valid, sizeof(valid) );
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, data, sizeof(data), &bytes, 1 ) == 1 );
    rocketnet_unit_check( bytes == (int) sizeof(valid) );
    rocketnet_unit_check( memcmp( data, valid, sizeof(valid) ) == 0 );
    rn_test_server_io_stop_and_close( &server );
    rocketnet_client_transport_io_close( &client );
    printf( "test_loopback_transport_server_batch_drops_packet_larger_than_stride\n" );
}

// This fake starts with a known completed input queue. It tests the server's
// bounded drain independently of any asynchronous client's send completion.
struct server_queued_budget_t { int remaining; };
static int server_queued_budget_receive( void * context, rocketnet_address_t * from,
    uint8_t * data, int stride, int * bytes, int capacity )
{
    auto * queue = (server_queued_budget_t *) context;
    const int count = queue->remaining < capacity ? queue->remaining : capacity;
    for ( int i = 0; i < count; ++i )
    {
        memset( &from[i], 0, sizeof(from[i]) );
        from[i].type = ROCKETNET_ADDRESS_IPV4;
        from[i].data.ipv4[0] = 127;
        from[i].data.ipv4[3] = 1;
        from[i].port = 40000;
        data[size_t(i) * size_t(stride)] = 0;
        bytes[i] = 1;
    }
    queue->remaining -= count;
    return count;
}

static void test_server_drains_completed_queue_to_budget()
{
    auto * server = shape_junk_server_create();
    const int budget = ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE;
    server_queued_budget_t queue = { budget + 17 };
    rocketnet_server_transport_io_operations_t operations = {};
    operations.destroy = wt_staged_destroy;
    operations.stop = wt_staged_stop;
    operations.stopped = wt_staged_stopped;
    operations.send_batch = wt_staged_send_batch;
    operations.receive_batch = server_queued_budget_receive;
    server->io.instance = &queue;
    server->io.operations = &operations;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( queue.remaining == 17 );
    rocketnet_unit_check( server->server_stats.packets_received == uint64_t(budget) );
    rocketnet_server_transport_update( server, 1000.1 );
    rocketnet_unit_check( queue.remaining == 0 );
    rocketnet_unit_check( server->server_stats.packets_received == uint64_t(budget + 17) );
    rocketnet_server_transport_update( server, 1000.2 );
    rocketnet_unit_check( server->server_stats.packets_received == uint64_t(budget + 17) );
    rn_test_server_transport_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_server_drains_completed_queue_to_budget );

REGISTER_UNIT_TEST( test_loopback_transport_io_binds_and_discovers_a_port );
REGISTER_UNIT_TEST( test_loopback_transport_io_exchanges_both_ways );
REGISTER_UNIT_TEST( test_loopback_transport_io_dual_stack_serves_ipv4_and_ipv6 );
REGISTER_UNIT_TEST( test_loopback_transport_io_full_queue_drops_and_counts );
REGISTER_UNIT_TEST( test_loopback_transport_io_runtime_does_not_allocate );
REGISTER_UNIT_TEST( test_loopback_transport_io_destroy_frees_the_port );
REGISTER_UNIT_TEST( test_server_counts_zero_length_datagram_as_too_short );
REGISTER_UNIT_TEST( test_loopback_transport_io_holds_max_clients_plus_server );
REGISTER_UNIT_TEST( test_loopback_transport_io_refuses_past_max_endpoints );
REGISTER_UNIT_TEST( test_wire_totals_staged_finish_counts_exactly );
REGISTER_UNIT_TEST( test_server_connects_after_junk_from_many_sources );
REGISTER_UNIT_TEST( test_server_shape_check_first_on_the_staged_loop );
REGISTER_UNIT_TEST( test_server_shape_check_first_on_the_portable_loop );
REGISTER_UNIT_TEST( test_loopback_transport_server_batch_drops_packet_larger_than_stride );

#include "test_unit_driver_results_checked.cpp"
