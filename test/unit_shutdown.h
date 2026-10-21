/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#ifndef ROCKETNET_TEST_UNIT_SHUTDOWN_H
#define ROCKETNET_TEST_UNIT_SHUTDOWN_H

#include "../test_unit_registry.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_client_io.h"

// Test-only cleanup for deterministic, step-driven fixtures. These helpers
// deliberately drive the fake worker; production completion queries never do.
// No clock, socket, sleep, or unbounded wait enters a unit cleanup path.
static inline void rn_test_client_transport_wait_stopped( rocketnet_client_transport_t * client )
{
    for ( int step = 0; !rocketnet_client_transport_stopped( client ) && step < 32; ++step )
    {
        // Stopping update only progresses the worker seam; it does not read
        // or change the connection clock. The fixed time is not game time.
        rocketnet_client_transport_update( client, 1000.0 );
    }
    rocketnet_unit_check( rocketnet_client_transport_stopped( client ) );
}

static inline void rn_test_client_wait_disconnected( rocketnet_client_t * client )
{
    if ( !rocketnet_client_disconnected( client ) )
    {
        rocketnet_message_client_t * message = rocketnet_client_message_client( client );
        rocketnet_unit_check( message != NULL );
        rn_test_client_transport_wait_stopped( rocketnet_message_client_transport( message ) );
    }
    rocketnet_unit_check( rocketnet_client_disconnected( client ) );
}

// Retry a terminal-state reconnect only after its first connect request has
// retired the old generation. This is test scheduling, not a blocking API.
static inline enum rocketnet_status_t rn_test_client_connect_after_completion( rocketnet_client_t * client, const char * token )
{
    enum rocketnet_status_t status = rocketnet_client_connect( client, token );
    if ( status == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED &&
         rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED )
    {
        rn_test_client_wait_disconnected( client );
        status = rocketnet_client_connect( client, token );
    }
    return status;
}

static inline void rn_test_server_wait_stopped( rocketnet_server_t * server )
{
    rocketnet_unit_check( rocketnet_server_stopped( server ) );
}

static inline void rn_test_client_stop_and_destroy( rocketnet_client_t * client )
{
    if ( client )
    {
        rocketnet_client_disconnect( client );
        rn_test_client_wait_disconnected( client );
    }
    rocketnet_client_destroy( client );
}

static inline void rn_test_server_stop_and_destroy( rocketnet_server_t * server )
{
    if ( server )
    {
        rocketnet_server_stop( server );
        rn_test_server_wait_stopped( server );
    }
    rocketnet_server_destroy( server );
}

static inline void rn_test_client_transport_stop_and_destroy( rocketnet_client_transport_t * client )
{
    if ( client )
    {
        rocketnet_client_transport_stop( client );
        rn_test_client_transport_wait_stopped( client );
    }
    rocketnet_client_transport_destroy( client );
}

static inline void rn_test_server_transport_stop_and_destroy( rocketnet_server_transport_t * server )
{
    if ( server )
    {
        rocketnet_server_transport_stop( server );
        rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    }
    rocketnet_server_transport_destroy( server );
}

static inline void rn_test_server_io_wait_stopped( rocketnet_server_transport_io_t * server )
{
    rocketnet_unit_check( rocketnet_server_transport_io_stopped( server ) );
}

static inline void rn_test_server_io_stop_and_close( rocketnet_server_transport_io_t * server )
{
    rocketnet_server_transport_io_stop( server );
    rn_test_server_io_wait_stopped( server );
    rocketnet_server_transport_io_close( server );
}

static inline void rn_test_message_client_stop_and_destroy( struct rocketnet_message_client_t * message )
{
    if ( message )
    {
        struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( message );
        rocketnet_client_transport_stop( transport );
        rn_test_client_transport_wait_stopped( transport );
    }
    rocketnet_message_client_destroy( message );
}

static inline void rn_test_message_server_stop_and_destroy( struct rocketnet_message_server_t * message )
{
    if ( message )
    {
        struct rocketnet_server_transport_t * transport = rocketnet_message_server_transport( message );
        rocketnet_server_transport_stop( transport );
        rocketnet_unit_check( rocketnet_server_transport_stopped( transport ) );
    }
    rocketnet_message_server_destroy( message );
}

#endif
