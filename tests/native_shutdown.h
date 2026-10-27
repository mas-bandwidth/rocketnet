/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_TESTS_NATIVE_SHUTDOWN_H
#define ROCKETNET_TESTS_NATIVE_SHUTDOWN_H

#include "../sdk/rocketnet_internal.h"
#include "../examples/shutdown.h"

#ifdef __cplusplus
#include "../sdk/rocketnet_driver.h"
#endif

// Native functional/benchmark cleanup. Explicitly bounded waiting belongs to
// the harness, never to stop/stopped. Preserve the suite's outer watchdog.
// Every wait below is the one shared bounded wait in examples/shutdown.h
// (rn_cli_bounded_wait, 10 second bound); this header supplies each native
// type's completion query and step, and passes the harness clock and sleep
// in at those call sites.
static inline int rn_native_client_transport_is_stopped( void * object )
{
    return rocketnet_client_transport_stopped( (struct rocketnet_client_transport_t *) object );
}

static inline void rn_native_client_transport_step( void * object )
{
    rocketnet_client_transport_update( (struct rocketnet_client_transport_t *) object, 1000.0 );
}

static inline int rn_native_server_transport_is_stopped( void * object )
{
    return rocketnet_server_transport_stopped( (struct rocketnet_server_transport_t *) object );
}

static inline void rn_native_client_transport_wait_stopped( struct rocketnet_client_transport_t * client )
{
    rn_cli_bounded_wait( client, rn_native_client_transport_is_stopped, rn_native_client_transport_step, rocketnet_time, rocketnet_sleep, "client transport" );
}

static inline void rn_native_server_transport_wait_stopped( struct rocketnet_server_transport_t * server )
{
    rn_cli_bounded_wait( server, rn_native_server_transport_is_stopped, NULL, rocketnet_time, rocketnet_sleep, "server transport" );
}

static inline void rn_native_client_transport_stop_and_destroy( struct rocketnet_client_transport_t * client )
{
    if ( !client )
        return;
    rocketnet_client_transport_stop( client );
    rn_native_client_transport_wait_stopped( client );
    rocketnet_client_transport_destroy( client );
}

static inline void rn_native_server_transport_stop_and_destroy( struct rocketnet_server_transport_t * server )
{
    if ( !server )
        return;
    rocketnet_server_transport_stop( server );
    rn_native_server_transport_wait_stopped( server );
    rocketnet_server_transport_destroy( server );
}
#ifdef __cplusplus
static inline int rn_native_server_io_is_stopped( void * object )
{
    return rocketnet_server_transport_io_stopped( (rocketnet_server_transport_io_t *) object );
}

static inline void rn_native_server_io_wait_stopped( rocketnet_server_transport_io_t * server )
{
    rn_cli_bounded_wait( server, rn_native_server_io_is_stopped, NULL, rocketnet_time, rocketnet_sleep, "server driver" );
}

static inline void rn_native_server_io_stop_and_close( rocketnet_server_transport_io_t * server )
{
    rocketnet_server_transport_io_stop( server );
    rn_native_server_io_wait_stopped( server );
    rocketnet_server_transport_io_close( server );
}

template <typename transport_t>
struct rn_native_backend_wait_t
{
    transport_t * transport;
    bool ( *stopped )( transport_t * );
};

template <typename transport_t>
static int rn_native_backend_is_stopped( void * object )
{
    rn_native_backend_wait_t<transport_t> * wait = (rn_native_backend_wait_t<transport_t> *) object;
    return wait->stopped( wait->transport );
}

template <typename transport_t>
static inline void rn_native_backend_stop_and_destroy( transport_t * transport,
                                                       void ( *stop )( transport_t * ), bool ( *stopped )( transport_t * ), void ( *destroy )( transport_t * ) )
{
    if ( transport )
    {
        stop( transport );
        rn_native_backend_wait_t<transport_t> wait = { transport, stopped };
        rn_cli_bounded_wait( &wait, rn_native_backend_is_stopped<transport_t>, NULL, rocketnet_time, rocketnet_sleep, "backend" );
    }
    destroy( transport );
}
#endif

static inline void rn_native_message_client_stop_and_destroy( struct rocketnet_message_client_t * message )
{
    if ( !message )
        return;
    struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( message );
    rocketnet_client_transport_stop( transport );
    rn_native_client_transport_wait_stopped( transport );
    rocketnet_message_client_destroy( message );
}

static inline void rn_native_message_server_stop_and_destroy( struct rocketnet_message_server_t * message )
{
    if ( !message )
        return;
    struct rocketnet_server_transport_t * transport = rocketnet_message_server_transport( message );
    rocketnet_server_transport_stop( transport );
    rn_native_server_transport_wait_stopped( transport );
    rocketnet_message_server_destroy( message );
}

#endif // ROCKETNET_TESTS_NATIVE_SHUTDOWN_H
