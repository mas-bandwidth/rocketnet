/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#ifndef ROCKETNET_EXAMPLE_SHUTDOWN_H
#define ROCKETNET_EXAMPLE_SHUTDOWN_H

#include "rocketnet.h"
#include <stdio.h>
#include <stdlib.h>

// Command-line cleanup only. A game polls once per frame and keeps its event
// loop running. These programs explicitly wait at exit, with a failure bound;
// they never free an object while its worker still owns it.
static inline void rn_cli_client_wait_disconnected( struct rocketnet_client_t * client )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_client_disconnected( client ) )
    {
        if ( rocketnet_time() >= deadline )
        {
            fprintf( stderr, "client shutdown did not complete within 10 seconds\n" );
            abort();
        }
        // Needed only by an explicitly step-driven client. During shutdown,
        // update progresses cleanup without reading or changing game time.
        rocketnet_client_update( client, 1000.0 );
        rocketnet_sleep( 0.001 );
    }
}

static inline void rn_cli_server_wait_stopped( struct rocketnet_server_t * server )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_server_stopped( server ) )
    {
        if ( rocketnet_time() >= deadline )
        {
            fprintf( stderr, "server shutdown did not complete within 10 seconds\n" );
            abort();
        }
        rocketnet_sleep( 0.001 );
    }
}

static inline void rn_cli_client_stop_and_destroy( struct rocketnet_client_t * client )
{
    if ( !client )
        return;
    if ( client )
    {
        rocketnet_client_disconnect( client );
        rn_cli_client_wait_disconnected( client );
    }
    rocketnet_client_destroy( client );
}

static inline void rn_cli_server_stop_and_destroy( struct rocketnet_server_t * server )
{
    if ( !server )
        return;
    if ( server )
    {
        rocketnet_server_stop( server );
        rn_cli_server_wait_stopped( server );
    }
    rocketnet_server_destroy( server );
}
#endif
