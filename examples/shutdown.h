/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_EXAMPLES_SHUTDOWN_H
#define ROCKETNET_EXAMPLES_SHUTDOWN_H

#include "rocketnet.h"
#include <stdio.h>
#include <stdlib.h>

// Command-line cleanup only. A game polls once per frame and keeps its event
// loop running. These programs explicitly wait at exit, with a failure bound;
// they never free an object while its worker still owns it.

// One bounded wait shared by every cleanup helper here and in
// tests/native_shutdown.h. The caller supplies the completion query, an
// optional step that progresses a step-driven object, and the clock and
// sleep to wait on; this helper owns only the 10 second bound. Keeping the
// clock and sleep as caller-supplied functions means including this header
// brings no clock, sleep, socket or thread into a caller that does not pass
// them: a unit test that includes tests/native_shutdown.h gets this helper's
// declaration, not a real-time wait of its own.
typedef int ( *rn_cli_stopped_fn )( void * object );
typedef void ( *rn_cli_step_fn )( void * object );
typedef double ( *rn_cli_now_fn )( void );
typedef void ( *rn_cli_sleep_fn )( double seconds );

static inline void rn_cli_bounded_wait( void * object, rn_cli_stopped_fn stopped, rn_cli_step_fn step, rn_cli_now_fn now, rn_cli_sleep_fn sleep_fn, const char * what )
{
    const double deadline = now() + 10.0;

    while ( !stopped( object ) )
    {
        if ( now() >= deadline )
        {
            fprintf( stderr, "%s shutdown did not complete within 10 seconds\n", what );
            abort();
        }

        if ( step )
        {
            step( object );
        }

        sleep_fn( 0.001 );
    }
}

// A client wait carries the caller's game clock: update takes the same clock
// on every call, and a time that goes backwards is API misuse, after
// disconnect too. The wait passes the caller's last time on every step; the
// same time again is not backwards.
struct rn_cli_client_wait_t
{
    struct rocketnet_client_t * client;
    double time;
};

static inline int rn_cli_client_is_disconnected( void * object )
{
    return rocketnet_client_disconnected( ( (struct rn_cli_client_wait_t *) object )->client );
}

static inline void rn_cli_client_step( void * object )
{
    // Needed only by an explicitly step-driven client. During shutdown,
    // update progresses cleanup at the caller's last time.

    const struct rn_cli_client_wait_t * wait = (const struct rn_cli_client_wait_t *) object;
    rocketnet_client_update( wait->client, wait->time );
}

static inline int rn_cli_server_is_stopped( void * object )
{
    return rocketnet_server_stopped( (struct rocketnet_server_t *) object );
}

// time: the last time the caller passed to rocketnet_client_update, or any
// later time on the same clock.

static inline void rn_cli_client_wait_disconnected( struct rocketnet_client_t * client, double time )
{
    struct rn_cli_client_wait_t wait = { client, time };
    rn_cli_bounded_wait( &wait, rn_cli_client_is_disconnected, rn_cli_client_step, rocketnet_time, rocketnet_sleep, "client" );
}

static inline void rn_cli_server_wait_stopped( struct rocketnet_server_t * server )
{
    rn_cli_bounded_wait( server, rn_cli_server_is_stopped, NULL, rocketnet_time, rocketnet_sleep, "server" );
}

static inline void rn_cli_client_stop_and_destroy( struct rocketnet_client_t * client, double time )
{
    if ( !client )
        return;
    rocketnet_client_disconnect( client );
    rn_cli_client_wait_disconnected( client, time );
    rocketnet_client_destroy( client );
}

static inline void rn_cli_server_stop_and_destroy( struct rocketnet_server_t * server )
{
    if ( !server )
        return;
    rocketnet_server_stop( server );
    rn_cli_server_wait_stopped( server );
    rocketnet_server_destroy( server );
}

static inline void rn_cli_loopback_hub_destroy( struct rocketnet_loopback_hub_t * hub )
{
    // after the server and every client on the hub are destroyed; a hub
    // with an endpoint still registered is refused and kept

    const enum rocketnet_status_t status = rocketnet_loopback_hub_destroy( hub );
    if ( status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "FAIL: loopback hub destroy: %s\n", rocketnet_status_name( status ) );
    }
}
#endif // ROCKETNET_EXAMPLES_SHUTDOWN_H
