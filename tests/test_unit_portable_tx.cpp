/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "suite_runner.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_layers.h"
#include "unit_shutdown.h"
#include "loopback_hub.h"

#include <stdio.h>

// Needs ROCKETNET_DEV. No socket or wall-clock schedule:
// the adapter drives borrow, contested publish, full-bank drop and both stop
// interleavings at exact index transitions; one worker uses a paused seam.
bool rocketnet_portable_tx_test_handoff();

static void test_portable_tx_handoff()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_handoff() );
}

REGISTER_UNIT_TEST( test_portable_tx_handoff );

// Stop keeps a producer it cannot publish and publishes it on a later poll,
// before quit (proofs/tla/RNPortableTx.tla MStopRetry); nothing is dropped.
bool rocketnet_portable_tx_test_stop_publishes_across_polls();

static void test_portable_tx_stop_publishes_across_polls()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_stop_publishes_across_polls() );
}

REGISTER_UNIT_TEST( test_portable_tx_stop_publishes_across_polls );


#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
bool rocketnet_portable_tx_test_carry();
static void test_portable_tx_retains_suffix_and_terminal_drain_unblocks_stop()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_carry() );
}
REGISTER_UNIT_TEST( test_portable_tx_retains_suffix_and_terminal_drain_unblocks_stop );

// After EAGAIN retains a bank, the step is the carry wait (writable/1 ms, not
// the 100 ms idle wait) and a NO_FREE_BANK publish wakes the worker.
bool rocketnet_portable_tx_test_carry_wake();
static void test_portable_tx_carry_waits_for_writable_and_no_free_bank_wakes()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_carry_wake() );
}
REGISTER_UNIT_TEST( test_portable_tx_carry_waits_for_writable_and_no_free_bank_wakes );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// The maintenance sweep spends the portable producer bank's room per control
// frame it buffers: with room for 2 frames and 10 clients due a keep-alive,
// an update buffers 2 and the rest wait for the next updates, none dropped
// (proofs/tla/ServerStopSpread.tla Room). The real portable send path runs
// with no socket and no worker; the test plays the worker between updates,
// and time is an argument from 1.0.
#if defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )

void * rocketnet_portable_tx_test_sweep_owner( int capacity, int room, const rocketnet_server_transport_io_operations_t ** operations );
uint64_t rocketnet_portable_tx_test_sweep_drain( void * transport );
uint64_t rocketnet_portable_tx_test_sweep_dropped( void * transport );
int rocketnet_portable_tx_test_sweep_room( void * transport );
void rocketnet_portable_tx_test_sweep_destroy( void * transport );
bool rocketnet_server_transport_seat_clients_for_test( struct rocketnet_server_transport_t * server, int clients );

static void test_portable_tx_sweep_spends_room_per_control_frame()
{
    const int clients = 10;
    const int capacity = 4;
    const int room = 2;

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = clients;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &config, sizeof( config ) );
    rocketnet_unit_check( server != NULL );
    if ( !server )
    {
        return;
    }
    rocketnet_unit_check( rocketnet_server_transport_seat_clients_for_test( server, clients ) );

    const struct rocketnet_server_transport_io_operations_t * operations = NULL;
    void * portable = rocketnet_portable_tx_test_sweep_owner( capacity, room, &operations );
    rocketnet_unit_check( portable != NULL );
    if ( !portable )
    {
        rn_test_server_transport_stop_and_destroy( server );
        return;
    }
    rocketnet_unit_check( rocketnet_portable_tx_test_sweep_room( portable ) == room );
    void * instance = portable;
    rocketnet_server_transport_swap_io_for_test( server, &operations, &instance );

    // the first update meets a bank with room for 2: it buffers 2 and stops

    rocketnet_server_transport_update( server, 1.0 );
    rocketnet_unit_check( rocketnet_portable_tx_test_sweep_dropped( portable ) == 0 );
    rocketnet_unit_check( rocketnet_portable_tx_test_sweep_room( portable ) == 0 );

    // each later update, after the worker drains, serves the clients still due

    uint64_t sent = rocketnet_portable_tx_test_sweep_drain( portable );
    rocketnet_unit_check( sent == (uint64_t) capacity );
    for ( int step = 1; step <= 2; ++step )
    {
        rocketnet_server_transport_update( server, 1.0 + 0.01 * step );
        rocketnet_unit_check( rocketnet_portable_tx_test_sweep_dropped( portable ) == 0 );
        sent += rocketnet_portable_tx_test_sweep_drain( portable );
    }
    rocketnet_unit_check( sent == (uint64_t) ( capacity - room + clients ) );
    rocketnet_unit_check( rocketnet_portable_tx_test_sweep_dropped( portable ) == 0 );

    rocketnet_server_transport_swap_io_for_test( server, &operations, &instance );
    rocketnet_portable_tx_test_sweep_destroy( portable );
    rn_test_server_transport_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_portable_tx_sweep_spends_room_per_control_frame );
#endif // #if defined( ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE )
