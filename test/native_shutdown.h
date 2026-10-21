/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#ifndef ROCKETNET_TEST_NATIVE_SHUTDOWN_H
#define ROCKETNET_TEST_NATIVE_SHUTDOWN_H

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_driver.h"
#include "../examples/shutdown.h"

// Native functional/benchmark cleanup. Explicitly bounded waiting belongs to
// the harness, never to stop/stopped. Preserve the suite's outer watchdog.
static inline void rn_native_client_transport_wait_stopped( struct rocketnet_client_transport_t * client )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_client_transport_stopped( client ) )
    {
        if ( rocketnet_time() >= deadline )
        {
            fprintf( stderr, "client transport shutdown did not complete within 10 seconds\n" );
            abort();
        }
        rocketnet_client_transport_update( client, 1000.0 );
        rocketnet_sleep( 0.001 );
    }
}

static inline void rn_native_server_transport_wait_stopped( struct rocketnet_server_transport_t * server )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_server_transport_stopped( server ) )
    {
        if ( rocketnet_time() >= deadline )
        {
            fprintf( stderr, "server transport shutdown did not complete within 10 seconds\n" );
            abort();
        }
        rocketnet_sleep( 0.001 );
    }
}

static inline void rn_native_client_transport_stop_and_destroy( struct rocketnet_client_transport_t * client )
{
    if ( client )
    {
        rocketnet_client_transport_stop( client );
        rn_native_client_transport_wait_stopped( client );
    }
    rocketnet_client_transport_destroy( client );
}

static inline void rn_native_server_transport_stop_and_destroy( struct rocketnet_server_transport_t * server )
{
    if ( server )
    {
        rocketnet_server_transport_stop( server );
        rn_native_server_transport_wait_stopped( server );
    }
    rocketnet_server_transport_destroy( server );
}
static inline void rn_native_server_io_wait_stopped( rocketnet_server_transport_io_t * server )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_server_transport_io_stopped( server ) )
    {
        if ( rocketnet_time() >= deadline )
        {
            fprintf( stderr, "server driver shutdown did not complete within 10 seconds\n" );
            abort();
        }
        rocketnet_sleep( 0.001 );
    }
}

static inline void rn_native_server_io_stop_and_close( rocketnet_server_transport_io_t * server )
{
    rocketnet_server_transport_io_stop( server );
    rn_native_server_io_wait_stopped( server );
    rocketnet_server_transport_io_close( server );
}

#ifdef __cplusplus
template <typename transport_t>
static inline void rn_native_backend_stop_and_destroy( transport_t * transport,
    void (*stop)( transport_t * ), bool (*stopped)( transport_t * ), void (*destroy)( transport_t * ) )
{
    if ( transport )
    {
        stop( transport );
        const double deadline = rocketnet_time() + 10.0;
        while ( !stopped( transport ) )
        {
            if ( rocketnet_time() >= deadline )
            {
                fprintf( stderr, "backend shutdown did not complete within 10 seconds\n" );
                abort();
            }
            rocketnet_sleep( 0.001 );
        }
    }
    destroy( transport );
}
#endif

static inline void rn_native_message_client_stop_and_destroy( struct rocketnet_message_client_t * message )
{
    if ( message )
    {
        struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( message );
        rocketnet_client_transport_stop( transport );
        rn_native_client_transport_wait_stopped( transport );
    }
    rocketnet_message_client_destroy( message );
}

static inline void rn_native_message_server_stop_and_destroy( struct rocketnet_message_server_t * message )
{
    if ( message )
    {
        struct rocketnet_server_transport_t * transport = rocketnet_message_server_transport( message );
        rocketnet_server_transport_stop( transport );
        rn_native_server_transport_wait_stopped( transport );
    }
    rocketnet_message_server_destroy( message );
}

#endif
