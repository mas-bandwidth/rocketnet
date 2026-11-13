/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The transport registry's ranked lists and the server transport open's
// refusal of an incomplete operations table, in the unit suite (./bin/test).
// tests/test_driver.cpp checks the lists too, but builds into the separate
// test_transport binary. No sockets, no real time, no threads.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_driver.h"
#include "rocketnet_address.h"

#include <stdio.h>
#include <string.h>

// mutation gap: me rocketnet_driver.cpp:139 list[index - 2]
//
// The registry inserts each row available here into the server and client
// lists by insertion sort on its preference, lower first. Each list holds every
// available row with a nonzero preference exactly once, in nondecreasing
// preference. A sort that compares two places back reads list[-1] at index 1
// (outside the array) and on Linux orders the servers xdp, portable, gso.

static void gaps_oob_wide_check_ranked( const int * list, int count, bool server )
{
    const char * side = server ? "server" : "client";

    int expected = 0;
    for ( int i = 0; i < ROCKETNET_NUM_TRANSPORTS; ++i )
    {
        const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( i );
        rocketnet_unit_check( definition != NULL );
        const int rank = server ? definition->server_preference : definition->client_preference;
        if ( !definition->backend || !rank )
        {
            continue;
        }

        ++expected;

        int times = 0;
        for ( int j = 0; j < count; ++j )
        {
            times += list[j] == i ? 1 : 0;
        }
        if ( times != 1 )
        {
            printf( "fault: the %s list holds transport %s %d times, expected once\n", side, definition->name, times );
        }
        rocketnet_unit_check( times == 1 );
    }
    rocketnet_unit_check( count == expected );

    for ( int j = 1; j < count; ++j )
    {
        const rocketnet_transport_definition_t * previous = rocketnet_transport_definition( list[j - 1] );
        const rocketnet_transport_definition_t * current = rocketnet_transport_definition( list[j] );
        rocketnet_unit_check( previous && current );
        const int previous_rank = server ? previous->server_preference : previous->client_preference;
        const int current_rank = server ? current->server_preference : current->client_preference;
        if ( previous_rank > current_rank )
        {
            printf( "fault: the %s list is misordered: %s (rank %d) before %s (rank %d) at position %d: the insertion sort compared the wrong neighbour\n",
                    side, previous->name, previous_rank, current->name, current_rank, j );
        }
        rocketnet_unit_check( previous_rank <= current_rank );
    }
}

static void test_gaps_oob_wide_transport_lists_ranked()
{
    int servers = 0;
    const int * server_list = rocketnet_server_transport_io_availability( &servers );
    rocketnet_unit_check( server_list != NULL && servers > 0 );
    gaps_oob_wide_check_ranked( server_list, servers, true );

    int clients = 0;
    const int * client_list = rocketnet_client_transport_io_availability( &clients );
    rocketnet_unit_check( client_list != NULL && clients > 0 );
    gaps_oob_wide_check_ranked( client_list, clients, false );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    // xdp 10, gso 20, io_uring 25 when built, portable 30 (DEV) or 50, loopback last
    rocketnet_unit_check( server_list[0] == ROCKETNET_TRANSPORT_XDP && server_list[1] == ROCKETNET_TRANSPORT_GSO );
    rocketnet_unit_check( server_list[servers - 1] == ROCKETNET_TRANSPORT_LOOPBACK );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
}

REGISTER_UNIT_TEST( test_gaps_oob_wide_transport_lists_ranked );

// mutation gap: me rocketnet_driver.cpp:187 send_batch || receive_batch
//
// A buffered (not staged) server transport is driven through send_batch and
// receive_batch, so open refuses a table missing either, before its create
// runs; one that accepted it would later call through the NULL pointer.

struct gaps_oob_wide_ops_t
{
    int creates;
};

static void * gaps_oob_wide_create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * )
{
    ( (gaps_oob_wide_ops_t *) context )->creates++;
    return context;
}

static void gaps_oob_wide_destroy( void * ) {}
static void gaps_oob_wide_stop( void * ) {}
static bool gaps_oob_wide_stopped( void * ) { return true; }
static void gaps_oob_wide_send_batch( void *, const rocketnet_platform_send_t *, int ) {}
static int gaps_oob_wide_receive_batch( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) { return 0; }

static void gaps_oob_wide_expect_refused( const rocketnet_server_transport_io_operations_t * operations, const char * missing )
{
    gaps_oob_wide_ops_t state = {};
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    const rocketnet_server_transport_io_config_t setup = { "fixture", &config, "127.0.0.1:0", NULL };

    rocketnet_address_t bind;
    rocketnet_unit_check( rocketnet_address_parse( &bind, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t transport = {};

    const bool opened = rocketnet_server_transport_io_open( operations, &state, &setup, &bind, &transport );
    if ( opened || state.creates != 0 )
    {
        printf( "fault: a buffered server transport with no %s was accepted (create ran %d times): it would be called through NULL\n", missing, state.creates );
    }
    rocketnet_unit_check( !opened && state.creates == 0 );
    rocketnet_unit_check( !transport.instance && !transport.operations );
}

static void test_gaps_oob_wide_buffered_ops_need_both_batches()
{
    rocketnet_server_transport_io_operations_t operations = {};
    operations.flags = 0;
    operations.create = gaps_oob_wide_create;
    operations.destroy = gaps_oob_wide_destroy;
    operations.stop = gaps_oob_wide_stop;
    operations.stopped = gaps_oob_wide_stopped;

    operations.send_batch = gaps_oob_wide_send_batch;
    operations.receive_batch = NULL;
    gaps_oob_wide_expect_refused( &operations, "receive_batch" );

    operations.send_batch = NULL;
    operations.receive_batch = gaps_oob_wide_receive_batch;
    gaps_oob_wide_expect_refused( &operations, "send_batch" );

    // both present: accepted, and create runs once

    operations.send_batch = gaps_oob_wide_send_batch;
    gaps_oob_wide_ops_t state = {};
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    const rocketnet_server_transport_io_config_t setup = { "fixture", &config, "127.0.0.1:0", NULL };
    rocketnet_address_t bind;
    rocketnet_unit_check( rocketnet_address_parse( &bind, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t transport = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( &operations, &state, &setup, &bind, &transport ) );
    rocketnet_unit_check( state.creates == 1 && transport.instance == &state );
    rocketnet_server_transport_io_stop( &transport );
    rocketnet_server_transport_io_close( &transport );
}

REGISTER_UNIT_TEST( test_gaps_oob_wide_buffered_ops_need_both_batches );
